#include "wire.h"

// SetSecurityInfo / SE_OBJECT_TYPE / SE_KERNEL_OBJECT 在 aclapi.h + accctrl.h 里，
// windows.h 【不会】自动把它们带进来（ACL 的构造函数在 securitybaseapi.h，那个倒是带了）。
#include <aclapi.h>
#include <accctrl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace wire {

const wchar_t* PipeName() { return L"\\\\.\\pipe\\simple_cua"; }

// ===========================================================================
// 安全描述符
// ===========================================================================

static SID_IDENTIFIER_AUTHORITY g_mlAuthority = SECURITY_MANDATORY_LABEL_AUTHORITY;

static PACL  g_lowLabelSacl  = NULL;   // 建一次，复用
static bool  g_allowAllUsers = false;  // server 的 --allow-all-users
static bool  g_usedEveryone  = false;  // 实际有没有用 Everyone
static DWORD g_labelError    = 0;      // 0 = 设标签成功
static char  g_summary[256]  = "";     // 给 server 启动日志用

void SetAllowAllUsers(bool on) { g_allowAllUsers = on; }

// ---------------------------------------------------------------------------
// 取当前进程用户的 SID。返回的内存由调用方 free。
// ---------------------------------------------------------------------------
static PSID GetCurrentUserSid() {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return NULL;

    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, NULL, 0, &len);
    if (len == 0) { CloseHandle(tok); return NULL; }

    TOKEN_USER* tu = (TOKEN_USER*)malloc(len);
    if (!tu) { CloseHandle(tok); return NULL; }
    if (!GetTokenInformation(tok, TokenUser, tu, len, &len)) {
        free(tu); CloseHandle(tok); return NULL;
    }

    DWORD sidLen = GetLengthSid(tu->User.Sid);
    PSID sid = malloc(sidLen);
    if (sid) CopySid(sidLen, sid, tu->User.Sid);

    free(tu);
    CloseHandle(tok);
    return sid;
}

// ---------------------------------------------------------------------------
// 取本进程的【登录会话 SID】(S-1-5-5-X-Y)
//
// 同一登录会话里的所有进程共享它 —— 包括你在终端里启动的 server，和 DSH 里
// agent 启动的 client。这就是让受限令牌客户端能通过写检查的关键：
// 登录 SID 在"限制 SID 检查"里是被接受的（而 S-1-4-… capability 不是）。
//
// 返回的内存由调用方 free。
// ---------------------------------------------------------------------------
static PSID GetLogonSid() {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return NULL;

    DWORD len = 0;
    GetTokenInformation(tok, TokenGroups, NULL, 0, &len);
    if (len == 0) { CloseHandle(tok); return NULL; }

    TOKEN_GROUPS* groups = (TOKEN_GROUPS*)malloc(len);
    if (!groups) { CloseHandle(tok); return NULL; }
    if (!GetTokenInformation(tok, TokenGroups, groups, len, &len)) {
        free(groups); CloseHandle(tok); return NULL;
    }

    PSID logonSid = NULL;
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            DWORD n = GetLengthSid(groups->Groups[i].Sid);
            logonSid = malloc(n);
            if (logonSid) CopySid(n, logonSid, groups->Groups[i].Sid);
            break;
        }
    }

    free(groups);
    CloseHandle(tok);
    return logonSid;
}

// Everyone (S-1-1-0)
static PSID GetEveryoneSid() {
    SID_IDENTIFIER_AUTHORITY worldAuth = SECURITY_WORLD_SID_AUTHORITY;
    PSID sid = NULL;
    if (!AllocateAndInitializeSid(&worldAuth, 1, SECURITY_WORLD_RID,
                                  0, 0, 0, 0, 0, 0, 0, &sid)) {
        return NULL;
    }
    return sid;
}

// ---------------------------------------------------------------------------
// 构造 DACL：当前用户 + （登录会话 SID 或 Everyone）
//
// 只写 DACL，不写 SACL —— 标签必须走 SetSecurityInfo 单独设（见头文件说明）。
// ---------------------------------------------------------------------------
static PACL BuildPipeDacl(PSID userSid, PSID secondSid) {
    const DWORD aceHeader = sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD);
    DWORD userLen   = userSid   ? GetLengthSid(userSid)   : 0;
    DWORD secondLen = secondSid ? GetLengthSid(secondSid) : 0;

    DWORD size = sizeof(ACL);
    if (userLen)   size += aceHeader + userLen;
    if (secondLen) size += aceHeader + secondLen;

    PACL dacl = (PACL)malloc(size);
    if (!dacl) return NULL;
    if (!InitializeAcl(dacl, size, ACL_REVISION)) return NULL;

    if (userLen &&
        !AddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_READ | GENERIC_WRITE, userSid)) {
        return NULL;
    }
    if (secondLen &&
        !AddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_READ | GENERIC_WRITE, secondSid)) {
        return NULL;
    }
    return dacl;
}

// ---------------------------------------------------------------------------
// 构造强制标签 SACL：Low + NO_WRITE_UP
//
// 失败时把 Win32 错误码记进 g_saclError —— 诊断串要把它打出来。
// 这个函数可能失败在好几步（分配 SID / 建 ACL / 拷 SID / 加 ACE），
// 只报一个笼统的 FAILED 会让人多绕好几轮。
// ---------------------------------------------------------------------------
static DWORD g_saclError = 0;

static PACL BuildLowLabelSacl() {
    g_saclError = 0;

    PSID lowSid = NULL;
    if (!AllocateAndInitializeSid(&g_mlAuthority, 1, SECURITY_MANDATORY_LOW_RID,
                                  0, 0, 0, 0, 0, 0, 0, &lowSid)) {
        g_saclError = GetLastError();
        return NULL;
    }
    DWORD lowLen = GetLengthSid(lowSid);

    DWORD size = sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) + lowLen;
    PACL sacl = (PACL)malloc(size);
    if (!sacl) {
        g_saclError = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (!InitializeAcl(sacl, size, ACL_REVISION)) {
        g_saclError = GetLastError();
        return NULL;
    }

    // 手工构造这条 ACE，而不是用 AddMandatoryAce —— 后者在部分 MinGW 头文件里
    // 未必声明，手工构造可移植。
    BYTE aceBuf[sizeof(SYSTEM_MANDATORY_LABEL_ACE) + SECURITY_MAX_SID_SIZE];
    ZeroMemory(aceBuf, sizeof(aceBuf));
    SYSTEM_MANDATORY_LABEL_ACE* ace = (SYSTEM_MANDATORY_LABEL_ACE*)aceBuf;
    ace->Header.AceType  = SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    ace->Header.AceSize  = (WORD)(sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) + lowLen);
    ace->Header.AceFlags = 0;
    ace->Mask            = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;
    if (!CopySid(lowLen, (PSID)&ace->SidStart, lowSid)) {
        g_saclError = GetLastError();
        return NULL;
    }
    if (!AddAce(sacl, ACL_REVISION, MAXDWORD, ace, ace->Header.AceSize)) {
        g_saclError = GetLastError();
        return NULL;
    }

    return sacl;
}

// ===========================================================================
// server 侧
// ===========================================================================
HANDLE CreateServerPipe() {
    PSID userSid = GetCurrentUserSid();

    // --- 选第二个被授权者 ---
    //   默认：登录会话 SID（范围最小，够用）
    //   --allow-all-users：Everyone（逃生开关）
    //   读不到登录 SID 时也退化成 Everyone，并在日志里标出来
    PSID secondSid = NULL;
    const char* secondName = "(none)";

    if (g_allowAllUsers) {
        secondSid  = GetEveryoneSid();
        secondName = "EVERYONE(--allow-all-users)";
        g_usedEveryone = true;
    } else {
        secondSid = GetLogonSid();
        if (secondSid) {
            secondName = "logon-session";
        } else {
            secondSid  = GetEveryoneSid();
            secondName = "EVERYONE(fallback: no logon sid)";
            g_usedEveryone = true;
        }
    }

    if (!userSid && !secondSid) return NULL;

    PACL dacl = BuildPipeDacl(userSid, secondSid);
    if (!dacl) return NULL;

    if (!g_lowLabelSacl) g_lowLabelSacl = BuildLowLabelSacl();

    SECURITY_DESCRIPTOR* sd = (SECURITY_DESCRIPTOR*)malloc(sizeof(SECURITY_DESCRIPTOR));
    if (!sd) return NULL;
    if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(sd, TRUE, dacl, FALSE)) {
        return NULL;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle       = FALSE;

    // maxInstances = 1 -> 同一时刻只允许一个 client，天然实现"带锁"
    // WRITE_OWNER是必要的，尽管我们并不打算改属主，但是，设置【强制完整性标签】(SetSecurityInfo + LABEL_SECURITY_INFORMATION)
    // 要求句柄带 WRITE_OWNER。CreateNamedPipe 默认不返回这个权限，必须显式加上。
    HANDLE h = CreateNamedPipeW(
        PipeName(),
        PIPE_ACCESS_DUPLEX  | WRITE_OWNER, // 给予server修改属主的权限
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,          // 只允许一个实例
        64 * 1024,  // 输出缓冲
        64 * 1024,  // 输入缓冲
        0,
        &sa);

    if (h == INVALID_HANDLE_VALUE) return NULL;

    // -----------------------------------------------------------------------
    // ★ 强制标签必须单独设
    //
    // 传给 CreateNamedPipe 的 SACL 会被内核【静默忽略】—— 创建对象时提供 SACL
    // 需要 SeSecurityPrivilege，而普通权限进程没有。设标签的正当途径是
    // SetSecurityInfo + LABEL_SECURITY_INFORMATION，它只要求 WRITE_OWNER
    //（所以上面 dwOpenMode 里必须带 WRITE_OWNER）。
    //
    // 顺便对比一下 DACL 和强制标签在这件事上的差别 —— 这是最容易记混的一点：
    //   DACL     —— 可以走创建时的 SECURITY_ATTRIBUTES，【不需要任何句柄权限】
    //   强制标签 —— 创建时给不了（会被静默丢掉），只能事后设，而事后设要 WRITE_OWNER
    // -----------------------------------------------------------------------
    g_labelError = 0;
    if (g_lowLabelSacl) {
        g_labelError = SetSecurityInfo(h, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION,
                                       NULL, NULL, NULL, g_lowLabelSacl);
    }
    // 若 g_lowLabelSacl 为 NULL，说明 BuildLowLabelSacl 失败，原因在 g_saclError

    // 诊断串：把"构造失败"和"设置失败"分开，并且【一定带上错误码】
    char labelInfo[80];
    if (!g_lowLabelSacl) {
        snprintf(labelInfo, sizeof(labelInfo), "BUILD-FAILED(err=%lu)",
                 (unsigned long)g_saclError);
    } else if (g_labelError != 0) {
        snprintf(labelInfo, sizeof(labelInfo), "SET-FAILED(err=%lu)",
                 (unsigned long)g_labelError);
    } else {
        snprintf(labelInfo, sizeof(labelInfo), "set(Low)");
    }

    snprintf(g_summary, sizeof(g_summary), "dacl=%s+%s  low-label=%s",
             userSid ? "current-user" : "(no-user)",
             secondName,
             labelInfo);

    return h;
}

bool AcceptClient(HANDLE serverPipe) {
    if (!serverPipe) return false;
    // 阻塞等待。返回 false 且 GetLastError()==ERROR_PIPE_CONNECTED 也算连上了。
    BOOL ok = ConnectNamedPipe(serverPipe, NULL);
    if (ok) return true;
    return GetLastError() == ERROR_PIPE_CONNECTED;
}

bool PipeUsesEveryone() { return g_usedEveryone; }

const char* PipeSecuritySummary() {
    if (g_summary[0]) return g_summary;
    return "(pipe not created yet)";
}

// ===========================================================================
// client 侧
// ===========================================================================
HANDLE ConnectToServer(int timeoutMs) {
    if (!WaitNamedPipeW(PipeName(), (DWORD)timeoutMs)) return INVALID_HANDLE_VALUE;

    HANDLE h = CreateFileW(PipeName(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    return h;
}

// ===========================================================================
// 收发
// ===========================================================================
bool SendAll(HANDLE h, const void* data, int len) {
    const char* p    = (const char*)data;
    int         left = len;
    while (left > 0) {
        DWORD wrote = 0;
        if (!WriteFile(h, p, (DWORD)left, &wrote, NULL) || wrote == 0) return false;
        p    += wrote;
        left -= (int)wrote;
    }
    return true;
}

// 收满 len 个字节
static bool RecvAll(HANDLE h, void* buf, int len) {
    char* p    = (char*)buf;
    int   left = len;
    while (left > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, (DWORD)left, &got, NULL) || got == 0) return false;
        p    += got;
        left -= (int)got;
    }
    return true;
}

// ===========================================================================
// 请求的收发：4 字节小端长度 + 内容
// ===========================================================================
bool SendFrame(HANDLE h, const void* data, int len) {
    unsigned char hdr[4];
    hdr[0] = (unsigned char)( len        & 0xFF);
    hdr[1] = (unsigned char)((len >>  8) & 0xFF);
    hdr[2] = (unsigned char)((len >> 16) & 0xFF);
    hdr[3] = (unsigned char)((len >> 24) & 0xFF);

    return SendAll(h, hdr, 4) && SendAll(h, data, len);
}

int RecvFrame(HANDLE h, char* out, int cap) {
    if (!out || cap <= 0) return -1;

    unsigned char hdr[4];
    if (!RecvAll(h, hdr, 4)) return -1;

    int len = (int)hdr[0] | ((int)hdr[1] << 8) | ((int)hdr[2] << 16) | ((int)hdr[3] << 24);
    if (len < 0 || len >= cap) return -1;   // 长度不合理 / 缓冲装不下

    if (!RecvAll(h, out, len)) return -1;
    out[len] = '\0';
    return len;
}

// 返回实际字节数；-1 = 失败。
// 注意：读到的字节里可以包含 '\0'（请求体就是 NUL 分隔的），
//       所以不能用 strlen 来量长度 —— 这也是这个函数必须返回字节数的原因。
int RecvLine(HANDLE h, char* out, int cap) {
    if (!out || cap <= 0) return -1;
    int n = 0;
    for (;;) {
        char c = 0;
        DWORD got = 0;
        if (!ReadFile(h, &c, 1, &got, NULL) || got == 0) return -1;
        if (c == '\n') { out[n] = '\0'; return n; }
        if (c == '\r') continue;
        if (n >= cap - 1) return -1;   // 行太长
        out[n++] = c;
    }
}

// ===========================================================================
// argv 编解码
// ===========================================================================
int EncodeArgv(int n, const char* const* args, char* out, int cap) {
    int used = 0;
    for (int i = 0; i < n; ++i) {
        const char* a = args[i] ? args[i] : "";
        int len = (int)strlen(a);
        if (used + len + 1 > cap) return -1;
        memcpy(out + used, a, len);
        used += len;
        out[used++] = '\0';       // 分隔符
    }
    return used;
}

int DecodeArgv(char* buffer, int len, char** argv, int maxArgc) {
    int argc = 0;
    int i    = 0;
    while (i < len && argc < maxArgc) {
        argv[argc++] = buffer + i;
        while (i < len && buffer[i] != '\0') ++i;
        ++i;                       // 跳过这个 NUL
    }
    return argc;
}

// ===========================================================================
// JSON 转义
// ===========================================================================
void JsonEscapeAppend(char* out, int cap, const char* s) {
    int n = (int)strlen(out);
    if (n < cap - 1) out[n++] = '"';

    for (const unsigned char* p = (const unsigned char*)s; p && *p; ++p) {
        if (n >= cap - 8) break;
        switch (*p) {
            case '"':  out[n++] = '\\'; out[n++] = '"';  break;
            case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
            case '\n': out[n++] = '\\'; out[n++] = 'n';  break;
            case '\r': out[n++] = '\\'; out[n++] = 'r';  break;
            case '\t': out[n++] = '\\'; out[n++] = 't';  break;
            default:
                if (*p < 0x20) {
                    // 其它控制字符
                    n += sprintf(out + n, "\\u%04X", (unsigned)*p);
                } else {
                    out[n++] = (char)*p;   // UTF-8 字节原样通过
                }
        }
    }
    if (n < cap - 1) out[n++] = '"';
    out[n] = '\0';
}

} // namespace wire
