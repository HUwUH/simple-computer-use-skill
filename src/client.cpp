// ===========================================================================
// client.cpp —— 命令行客户端
//
// 它只做三件事：
//   1. 把 argv 编码成 NUL 分隔的字节流发给 server
//   2. 读回"状态码行 + JSON 行"
//   3. 把 JSON 原样转发给 stdout（加 RESULT 前缀），状态码作为自己的退出码
//
// 所以 client 【不需要 JSON 解析器】，也不需要 DPI 感知 ——
// 它不碰坐标，一切系统操作都在 server 那边发生。
//
// 用法： simple_cua.exe <动作> [参数...]
// ===========================================================================

// 注意：client 【不】包含 winmon.h —— 它不碰坐标，也不需要 DPI 感知，
// 所有系统操作都在 server 那边发生。少包含一个头就少一堆"未使用函数"警告。
#include "common/winutil.h"
#include "common/wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 统一输出一行结果。msg 里的反斜杠等由这里负责转义，调用方写自然文本即可。
static int EmitFail(const char* code, const char* msg, int status) {
    char esc[2048] = "";
    wire::JsonEscapeAppend(esc, sizeof(esc), msg);
    printf("RESULT {\"ok\":false,\"code\":\"%s\",\"msg\":%s}\n", code, esc);
    fflush(stdout);
    return status;
}

int wmain(int argc, wchar_t** argv) {
    WinUseUtf8Console();

    if (argc < 2) {
        printf("usage: simple_cua.exe <action> [args...]\n");
        printf("  e.g. simple_cua.exe ping\n");
        WinRestoreConsole();
        return ST_USAGE;
    }

    // --- 连接 ---
    HANDLE h = wire::ConnectToServer(500);
    if (h == INVALID_HANDLE_VALUE) {
        // 这条信息是给 agent 看的，所以要说清"该怎么向用户求助"
        int rc = EmitFail("server-not-running",
                          "the cua server is not running. "
                          "ask the user to start bin\\cua_server.exe "
                          "(normal privileges, NOT as administrator).",
                          ST_NO_SERVER);
        WinRestoreConsole();
        return rc;
    }

    // --- 发送请求：argv[1..] -> NUL 分隔，末尾补一个换行 ---
    char req[8192];
    int n = wire::EncodeArgv(argc - 1, argv + 1, req, sizeof(req) - 2);
    if (n < 0) {
        CloseHandle(h);
        int rc = EmitFail("request-too-long", "the argument list is too long", ST_USAGE);
        WinRestoreConsole();
        return rc;
    }
    req[n++] = '\n';

    if (!wire::SendAll(h, req, n)) {
        CloseHandle(h);
        int rc = EmitFail("send-failed", "failed to write the request to the server", ST_SERVER);
        WinRestoreConsole();
        return rc;
    }

    // --- 收响应：第一行状态码，第二行 JSON ---
    char statusLine[64]   = "";
    char jsonLine[16384]  = "";

    if (wire::RecvLine(h, statusLine, sizeof(statusLine)) < 0) {
        CloseHandle(h);
        int rc = EmitFail("no-response", "the server closed the pipe without answering", ST_SERVER);
        WinRestoreConsole();
        return rc;
    }
    if (wire::RecvLine(h, jsonLine, sizeof(jsonLine)) < 0) {
        CloseHandle(h);
        int rc = EmitFail("no-body", "the server sent a status but no result body", ST_SERVER);
        WinRestoreConsole();
        return rc;
    }
    CloseHandle(h);

    // --- 原样转发 ---
    printf("RESULT %s\n", jsonLine);
    fflush(stdout);

    WinRestoreConsole();
    return atoi(statusLine);
}
