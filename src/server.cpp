// ===========================================================================
// server.cpp —— 常驻服务端
//
// 它是【唯一】真正操作鼠标键盘的进程，所以它必须跑在普通用户权限
// （Medium 完整性）下。由人类启动一次，之后一直待命。
//
// ⚠️ server.exe 必须放在 DSH 工作区【外面】。工作区目录带 Low 完整性标签，
//    标签可继承 —— 放在那里的 exe 会以低完整性运行，一切输入注入静默失效。
//
// 用法： cua_server.exe
// 退出： 直接关窗口，或用 cua_panic.exe 停掉它
// ===========================================================================

#include "actions/actions.h"
#include "common/winmon.h"
#include "common/winutil.h"
#include "common/wire.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---------------------------------------------------------------------------
// 日志
//
// 两条通道，用途不同：
//   控制台  —— 给人【立刻】看的，所以短。默认只报一行摘要；
//              -v 时把回给 client 的内容也打出来，但截断。
//   日志文件 —— 留档用的，所以【永远写全文】。
// ---------------------------------------------------------------------------
static char g_logPath[MAX_PATH] = "";
static bool g_verbose = false;

static void LogInit(void) {
    WCHAR exePath[MAX_PATH] = L"";
    DWORD n = GetModuleFileNameW(NULL, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    // 去掉文件名，留下目录
    for (int i = (int)n - 1; i >= 0; --i) {
        if (exePath[i] == L'\\' || exePath[i] == L'/') { exePath[i] = 0; break; }
    }

    WCHAR full[MAX_PATH + 32];
    _snwprintf(full, MAX_PATH + 31, L"%s\\cua_server.log", exePath);

    int m = WideCharToMultiByte(CP_UTF8, 0, full, -1, g_logPath, MAX_PATH, NULL, NULL);
    if (m <= 0) g_logPath[0] = '\0';
}

// 超过 1MB 就把当前日志改名为 <名字>.1（覆盖旧的），重新开始
static void LogRotateIfNeeded(void) {
    if (!g_logPath[0]) return;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(g_logPath, GetFileExInfoStandard, &fad)) return;

    ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (sz < 1024ull * 1024ull) return;

    char rot[MAX_PATH + 8];
    snprintf(rot, sizeof(rot), "%s.1", g_logPath);
    MoveFileExA(g_logPath, rot, MOVEFILE_REPLACE_EXISTING);
}

// 只写控制台。时分秒就够 —— 人看的是"刚刚发生了什么"
static void WriteConsole(const char* msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
    fflush(stdout);
}

// 只写文件。带完整日期 —— 留档要能跟别的东西对上时间
static void WriteFile(const char* msg) {
    if (!g_logPath[0]) return;
    LogRotateIfNeeded();

    SYSTEMTIME st;
    GetLocalTime(&st);
    FILE* f = fopen(g_logPath, "a");
    if (f) {
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
}

// 两条通道都写
static void LogLine(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    WriteConsole(msg);
    WriteFile(msg);
}

// ---------------------------------------------------------------------------
// 记录"回给 client 的完整内容"
//
//   日志文件：永远全文 —— 出问题时要能事后看到到底回了什么
//   控制台  ：只有 -v 才打，而且截断
//
// 为什么截断到 384 字节就够了：
//   控制台是给人【立刻】看的，一行几百字节以上就没法看了。384 字节正好够看清
//   ok / op / 那几个关键字段；而各动作都把可能很长的字段（最典型的是 clipboard
//   的 text）放在【最后】—— 所以从尾部截断恰好不会切掉有用的部分。
// ---------------------------------------------------------------------------
#define CONSOLE_REPLY_MAX 384

static void LogReply(const char* op, int status, const char* json) {
    // --- 文件：全文 ---
    {
        static char full[8192 + 128];
        snprintf(full, sizeof(full), "%s -> status=%d\n    %s", op, status, json);
        WriteFile(full);
    }

    // --- 控制台 ---
    if (!g_verbose) {
        char brief[256];
        snprintf(brief, sizeof(brief), "%s -> status=%d", op, status);
        WriteConsole(brief);
        return;
    }

    int n   = (int)strlen(json);
    int cut = (n > CONSOLE_REPLY_MAX) ? CONSOLE_REPLY_MAX : n;
    // 按 UTF-8 字符边界截断 —— 否则控制台会显示半个汉字
    while (cut > 0 && ((unsigned char)json[cut] & 0xC0) == 0x80) --cut;

    char body[CONSOLE_REPLY_MAX + 1];
    memcpy(body, json, cut);
    body[cut] = '\0';

    char line[CONSOLE_REPLY_MAX + 128];
    snprintf(line, sizeof(line), "%s -> status=%d  %s%s",
             op, status, body, (n > cut) ? "  ...(truncated)" : "");
    WriteConsole(line);
}

// ---------------------------------------------------------------------------
// 是否已经有一个 server 在跑
//
// 管道名是全局的，且 maxInstances = 1。用"像 client 一样去连一下"来判断：
//   连上了            -> 已有 server
//   ERROR_PIPE_BUSY   -> 已有 server（它正忙）
//   其它（文件不存在） -> 没有
// ---------------------------------------------------------------------------
static bool AnotherServerIsRunning(void) {
    HANDLE h = CreateFileW(wire::PipeName(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return true; }
    return GetLastError() == ERROR_PIPE_BUSY;
}

// ---------------------------------------------------------------------------
// 用法： cua_server.exe [--allow-all-users] [-v|--verbose]
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    WinUseUtf8Console();

    bool allowAllUsers = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--allow-all-users")) {
            allowAllUsers = true;
        } else if (!wcscmp(argv[i], L"--verbose") || !wcscmp(argv[i], L"-v")) {
            g_verbose = true;
        } else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            printf("usage: cua_server.exe [--allow-all-users] [-v|--verbose]\n\n");
            printf("  (no option)         只允许【本登录会话】里的进程连接（默认，最安全）\n");
            printf("  --allow-all-users   把管道 DACL 放宽到 Everyone。\n");
            printf("                      只应在默认方式连不上时才用（即 server 与 agent 不在\n");
            printf("                      同一个登录会话）。打开后【任何本地账户】都能连上来\n");
            printf("                      驱动这台机器的鼠标和键盘。\n");
            printf("  -v, --verbose       控制台也打印回给 client 的内容（截断到 %d 字节）。\n",
                   CONSOLE_REPLY_MAX);
            printf("                      完整内容始终写进日志文件。\n");
            return ST_OK;
        } else {
            printf("unknown option: %ls\n", argv[i]);
            printf("usage: cua_server.exe [--allow-all-users] [-v|--verbose]\n");
            return ST_USAGE;
        }
    }
    wire::SetAllowAllUsers(allowAllUsers);

    // ★ 第一件事：DPI 感知。之后所有坐标才是物理像素。
    const char* dpiMode = WmEnablePerMonitorV2();

    LogInit();
    ActionsInit();

    if (AnotherServerIsRunning()) {
        LogLine("ERROR: another cua server is already running (pipe %ls)", wire::PipeName());
        WinRestoreConsole();
        return ST_USAGE;
    }

    LogLine("cua_server %s starting", CUA_VERSION);
    LogLine("  pid=%lu  dpi-awareness=%s", (unsigned long)GetCurrentProcessId(), dpiMode);
    LogLine("  pipe=%ls", wire::PipeName());
    LogLine("  monitors=%d", g_wmMonCount);
    LogLine("  log=%s", g_logPath[0] ? g_logPath : "(none)");
    LogLine("  run `simple_cua.exe ping` to see the integrity level this server runs at.");
    LogLine("  press Ctrl+C or close this window to stop.");
    if (g_verbose) {
        LogLine("  verbose: ON -- replies are echoed to the console (max %d bytes); "
                "the log file always gets the full text", CONSOLE_REPLY_MAX);
    }

    if (allowAllUsers) {
        LogLine("");
        LogLine("  ###########################################################");
        LogLine("  #  --allow-all-users is ON                                 #");
        LogLine("  #  ANY local account can connect and drive this machine's  #");
        LogLine("  #  mouse and keyboard while this server is running.        #");
        LogLine("  ###########################################################");
        LogLine("");
    }

    // --- 建管道 ---
    HANDLE srv = wire::CreateServerPipe();
    if (!srv) {
        LogLine("ERROR: CreateNamedPipe failed, err=%lu", GetLastError());
        LogLine("       (if this is a permissions problem, the Low integrity label "
                "could not be applied)");
        WinRestoreConsole();
        return ST_SERVER;
    }

    // 这一行是排查"低权限的 agent 连不上"的第一现场。
    // 正常应该是： dacl=current-user+logon-session  low-label=set
    LogLine("  pipe-security: %s", wire::PipeSecuritySummary());
    if (wire::PipeUsesEveryone() && !allowAllUsers) {
        LogLine("  WARNING: could not read this process's logon SID; "
                "fell back to Everyone.");
    }

    // --- 主循环：一次一个 client ---
    for (;;) {
        if (!wire::AcceptClient(srv)) {
            DWORD err = GetLastError();
            LogLine("accept failed, err=%lu -- recreating the pipe", err);
            CloseHandle(srv);
            srv = wire::CreateServerPipe();
            if (!srv) { LogLine("ERROR: cannot recreate the pipe, exiting"); break; }
            continue;
        }

        char req[8192] = "";
        // 用【长度前缀】定界，不是"读到换行" —— 请求里可以含换行
        int  reqLen = wire::RecvFrame(srv, req, sizeof(req));

        if (reqLen >= 0) {
            char* av[64];
            // ⚠️ 必须用 reqLen（真实字节数），不能用 strlen —— 请求体是 NUL 分隔的，
            //    strlen 只会量到第一个 token，后面的参数会被静默丢掉。
            int ac = wire::DecodeArgv(req, reqLen, av, 64);

            ActionResult res;
            DispatchAction(ac, av, &res);

            char statusLine[32];
            snprintf(statusLine, sizeof(statusLine), "%d\n", res.status);

            bool ok = wire::SendAll(srv, statusLine, (int)strlen(statusLine))
                   && wire::SendAll(srv, res.json, (int)strlen(res.json))
                   && wire::SendAll(srv, "\n", 1);
            FlushFileBuffers(srv);

            if (!ok) LogLine("write-back failed (client went away?)");
            LogReply(ac > 0 ? av[0] : "(empty)", res.status, res.json);
        } else {
            LogLine("client disconnected before sending a complete request");
        }

        // ⚠️ 这里【不能】CloseHandle(srv) —— 连接建立后用的就是 srv 这个句柄本身。
        //    （之前这么写，导致下一次 accept 必然 err=6 ERROR_INVALID_HANDLE）
        DisconnectNamedPipe(srv);
    }

    if (srv) CloseHandle(srv);
    LogLine("cua_server exiting");
    WinRestoreConsole();
    return ST_OK;
}
