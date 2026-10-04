#include "wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace wire {

const wchar_t* PipeName() { return L"\\\\.\\pipe\\simple_cua"; }

// ===========================================================================
// 安全描述符：DACL 只给当前用户 + 强制标签 Low
// ===========================================================================

// 取当前进程用户的 SID。返回的内存由调用方 free。
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

static SID_IDENTIFIER_AUTHORITY g_mlAuthority = SECURITY_MANDATORY_LABEL_AUTHORITY;

// 构造一个 SECURITY_ATTRIBUTES（含 DACL + Low 强制标签）。
// 里面的内存是一次性的，进程存活期间不释放（只建一次管道，无所谓）。
static SECURITY_ATTRIBUTES* BuildPipeSecurity() {
    PSID userSid = GetCurrentUserSid();
    if (!userSid) return NULL;

    PSID lowSid = NULL;
    if (!AllocateAndInitializeSid(&g_mlAuthority, 1, SECURITY_MANDATORY_LOW_RID,
                                  0, 0, 0, 0, 0, 0, 0, &lowSid)) {
        free(userSid);
        return NULL;
    }

    DWORD userLen = GetLengthSid(userSid);
    DWORD lowLen  = GetLengthSid(lowSid);

    // --- DACL：只给当前用户读写 ---
    DWORD daclSize = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + userLen;
    PACL dacl = (PACL)malloc(daclSize);
    if (!dacl) return NULL;
    if (!InitializeAcl(dacl, daclSize, ACL_REVISION) ||
        !AddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_READ | GENERIC_WRITE, userSid)) {
        return NULL;
    }

    // --- SACL：一条强制标签 ACE，Low + NO_WRITE_UP ---
    //
    // 手工构造这条 ACE，而不是用 AddMandatoryAce —— 后者在部分 MinGW 头文件里
    // 未必声明，手工构造可移植。
    DWORD saclSize = sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) + lowLen;
    PACL sacl = (PACL)malloc(saclSize);
    if (!sacl) return NULL;
    if (!InitializeAcl(sacl, saclSize, ACL_REVISION)) return NULL;

    BYTE aceBuf[sizeof(SYSTEM_MANDATORY_LABEL_ACE) + SECURITY_MAX_SID_SIZE];
    ZeroMemory(aceBuf, sizeof(aceBuf));
    SYSTEM_MANDATORY_LABEL_ACE* ace = (SYSTEM_MANDATORY_LABEL_ACE*)aceBuf;
    ace->Header.AceType  = SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    ace->Header.AceSize  = (WORD)(sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) + lowLen);
    ace->Header.AceFlags = 0;
    ace->Mask            = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;
    if (!CopySid(lowLen, (PSID)&ace->SidStart, lowSid)) return NULL;
    if (!AddAce(sacl, ACL_REVISION, MAXDWORD, ace, ace->Header.AceSize)) return NULL;

    // --- 组装 SD ---
    SECURITY_DESCRIPTOR* sd = (SECURITY_DESCRIPTOR*)malloc(sizeof(SECURITY_DESCRIPTOR));
    if (!sd) return NULL;
    if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(sd, TRUE, dacl, FALSE) ||
        !SetSecurityDescriptorSacl(sd, TRUE, sacl, FALSE)) {
        return NULL;
    }

    SECURITY_ATTRIBUTES* sa = (SECURITY_ATTRIBUTES*)malloc(sizeof(SECURITY_ATTRIBUTES));
    if (!sa) return NULL;
    sa->nLength              = sizeof(SECURITY_ATTRIBUTES);
    sa->lpSecurityDescriptor = sd;
    sa->bInheritHandle       = FALSE;
    return sa;
}

// ===========================================================================
// server 侧
// ===========================================================================
HANDLE CreateServerPipe() {
    SECURITY_ATTRIBUTES* sa = BuildPipeSecurity();
    if (!sa) return NULL;

    // maxInstances = 1 -> 同一时刻只允许一个 client，天然实现"带锁"
    HANDLE h = CreateNamedPipeW(
        PipeName(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,          // 只允许一个实例
        64 * 1024,  // 输出缓冲
        64 * 1024,  // 输入缓冲
        0,
        sa);

    return (h == INVALID_HANDLE_VALUE) ? NULL : h;
}

bool AcceptClient(HANDLE serverPipe) {
    if (!serverPipe) return false;
    // 阻塞等待。返回 false 且 GetLastError()==ERROR_PIPE_CONNECTED 也算连上了。
    BOOL ok = ConnectNamedPipe(serverPipe, NULL);
    if (ok) return true;
    return GetLastError() == ERROR_PIPE_CONNECTED;
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
    const char* p   = (const char*)data;
    int         left = len;
    while (left > 0) {
        DWORD wrote = 0;
        if (!WriteFile(h, p, (DWORD)left, &wrote, NULL) || wrote == 0) return false;
        p    += wrote;
        left -= (int)wrote;
    }
    return true;
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
