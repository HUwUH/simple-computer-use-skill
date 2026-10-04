// ===========================================================================
// client.cpp —— 命令行客户端
//
// 它做四件事：
//   1. 把 argv 从宽字符转成 UTF-8
//   2. 处理输出路径：把 --out 解析成【绝对路径】，并用【自己的令牌】探测可写性
//   3. 编码成 NUL 分隔的字节流发给 server
//   4. 读回"状态码行 + JSON 行"，JSON 原样转发，状态码作为自己的退出码
//
// 所以 client 【不需要 JSON 解析器】，也【不需要 DPI 感知】—— 它不碰坐标，
// 一切系统操作都在 server 那边发生。
//
// 用法： simple_cua.exe <动作> [参数...]
// ===========================================================================

#include "common/pathutil.h"
#include "common/winutil.h"
#include "common/wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 哪些动作会写文件，以及不给 --out 时的默认文件名
//
// 路径策略是 client 的职责：
//   * 只有 client 知道"自己被调用时的 cwd"是什么（server 是常驻进程，它不知道）
//   * 只有 client 的令牌能做那个保守的可写性探测
// ---------------------------------------------------------------------------
static const struct { const char* action; const char* defaultName; } kFileActions[] = {
    { "screenshot", "scua-screenshot.png" },
    { "zoom",       "scua-zoom.png"       },
};

static const char* DefaultNameFor(const char* action) {
    for (size_t i = 0; i < sizeof(kFileActions) / sizeof(kFileActions[0]); ++i) {
        if (!strcmp(kFileActions[i].action, action)) return kFileActions[i].defaultName;
    }
    return NULL;
}

// 统一输出一行失败结果。msg 里写自然文本即可，这里负责 JSON 转义。
static int EmitFail(const char* code, const char* msg, int status) {
    char esc[2048] = "";
    wire::JsonEscapeAppend(esc, sizeof(esc), msg);
    printf("RESULT {\"ok\":false,\"code\":\"%s\",\"msg\":%s}\n", code, esc);
    fflush(stdout);
    return status;
}

// ---------------------------------------------------------------------------
// 处理 --out：解析成绝对路径 + 探测可写
//
// 返回 0 = 通过；非 0 = 已经输出过错误，直接拿它当退出码。
// ---------------------------------------------------------------------------
static int ResolveOutput(std::vector<std::string>& args, const char* action) {
    const char* def = DefaultNameFor(action);
    if (!def) return 0;                 // 这个动作不写文件

    // args[0] 是动作名，所以从 1 开始找
    int idx = -1;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out") { idx = (int)i + 1; break; }
    }

    if (idx < 0) {
        // 没给 --out -> 用默认名（落在 client 的 cwd）
        args.push_back("--out");
        args.push_back(def);
        idx = (int)args.size() - 1;
    } else if (idx >= (int)args.size()) {
        return EmitFail("bad-arg", "--out needs a value", ST_USAGE);
    }

    char abs[32768] = "";
    if (!PathResolveAbsolute(args[idx].c_str(), abs, sizeof(abs))) {
        return EmitFail("bad-out",
                        "the --out path could not be resolved to an absolute path",
                        ST_USAGE);
    }

    // 安全白名单：必须以 .png 结尾。
    // 探测只回答"能不能写"，不回答"该不该写" —— 没有这条，
    // `--out 设计计划.md` 会把 PNG 写进文档里，把文档毁掉。
    if (!PathHasPngExtension(abs)) {
        return EmitFail("out-not-png",
                        "--out must end with .png (safety rule: a screenshot must not "
                        "overwrite a non-image file)",
                        ST_REFUSED);
    }

    char why[512] = "";
    if (!PathProbeWritable(abs, why, sizeof(why))) {
        // 这里用 ST_REFUSED：这是"安全闸门拒绝了"，不是"参数写错了"，
        // 调用方据此决定要不要换个路径重试。
        return EmitFail("out-not-writable", why, ST_REFUSED);
    }

    args[idx] = abs;
    return 0;
}

// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    WinUseUtf8Console();

    if (argc < 2) {
        printf("usage: simple_cua.exe <action> [args...]\n");
        printf("  e.g. simple_cua.exe ping\n");
        printf("       simple_cua.exe screenshot\n");
        printf("       simple_cua.exe zoom 100 100 400 300 --scale 3\n");
        WinRestoreConsole();
        return ST_USAGE;
    }

    // --- 宽 argv -> UTF-8 字符串列表 ---
    std::vector<std::string> args;
    args.reserve((size_t)argc);
    for (int i = 1; i < argc; ++i) {
        char utf8[8192] = "";
        WinToUtf8(argv[i], utf8, sizeof(utf8));
        args.push_back(std::string(utf8));
    }

    // --- 路径策略（必须在连接之前：连不上就没必要先探测，但先探测更省事） ---
    int rc = ResolveOutput(args, args[0].c_str());
    if (rc != 0) { WinRestoreConsole(); return rc; }

    // --- 连接 ---
    HANDLE h = wire::ConnectToServer(500);
    if (h == INVALID_HANDLE_VALUE) {
        // 这条信息是给 agent 看的，所以要说清"该怎么向用户求助"
        rc = EmitFail("server-not-running",
                      "the cua server is not running. "
                      "ask the user to start bin\\cua_server.exe "
                      "(normal privileges, NOT as administrator).",
                      ST_NO_SERVER);
        WinRestoreConsole();
        return rc;
    }

    // --- 编码并发送 ---
    std::vector<const char*> ptrs;
    ptrs.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) ptrs.push_back(args[i].c_str());

    char req[8192];
    int n = wire::EncodeArgv((int)ptrs.size(), ptrs.data(), req, sizeof(req) - 2);
    if (n < 0) {
        CloseHandle(h);
        rc = EmitFail("request-too-long", "the argument list is too long", ST_USAGE);
        WinRestoreConsole();
        return rc;
    }
    req[n++] = '\n';

    if (!wire::SendAll(h, req, n)) {
        CloseHandle(h);
        rc = EmitFail("send-failed", "failed to write the request to the server", ST_SERVER);
        WinRestoreConsole();
        return rc;
    }

    // --- 收响应：第一行状态码，第二行 JSON ---
    char statusLine[64]  = "";
    char jsonLine[16384] = "";

    if (wire::RecvLine(h, statusLine, sizeof(statusLine)) < 0) {
        CloseHandle(h);
        rc = EmitFail("no-response", "the server closed the pipe without answering", ST_SERVER);
        WinRestoreConsole();
        return rc;
    }
    if (wire::RecvLine(h, jsonLine, sizeof(jsonLine)) < 0) {
        CloseHandle(h);
        rc = EmitFail("no-body", "the server sent a status but no result body", ST_SERVER);
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
