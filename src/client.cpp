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
// 哪些动作会写文件，以及对应的规则
//
//   defaultName —— 不给 --out 时注入的默认文件名。
//                  NULL = 不主动注入（clipboard 只有 get 用得上 --out，
//                  set / clear 不需要）
//   ext         —— 允许的扩展名。这是【安全白名单】：可写性探测只回答
//                  "能不能写"，不回答"该不该写"。没有它，--out 指到设计稿上
//                  就会把稿子毁掉。
//
// 路径策略是 client 的职责：
//   * 只有 client 知道"自己被调用时的 cwd"是什么（server 是常驻进程，它不知道）
//   * 只有 client 的令牌能做那个保守的可写性探测
// ---------------------------------------------------------------------------
struct FileAction {
    const char* action;
    const char* defaultName;
    const char* ext;
};

static const FileAction kFileActions[] = {
    { "screenshot", "scua-screenshot.png", ".png" },
    { "zoom",       "scua-zoom.png",       ".png" },
    { "clipboard",  NULL,                  ".txt" },
};

static const FileAction* FileActionFor(const char* action) {
    for (size_t i = 0; i < sizeof(kFileActions) / sizeof(kFileActions[0]); ++i) {
        if (!strcmp(kFileActions[i].action, action)) return &kFileActions[i];
    }
    return NULL;
}

// 统一输出一行失败结果。msg 里写自然文本即可，这里负责 JSON 转义。
//
// op 也要带上 —— 和服务端产生的失败结果保持同样的形状，
// 免得调用方按 op 解析时拿到 nil。
static int EmitFail(const char* op, const char* code, const char* msg, int status) {
    char opEsc[128]   = "";
    char msgEsc[2048] = "";
    wire::JsonEscapeAppend(opEsc,  sizeof(opEsc),  op  ? op  : "");
    wire::JsonEscapeAppend(msgEsc, sizeof(msgEsc), msg ? msg : "");
    printf("RESULT {\"ok\":false,\"op\":%s,\"code\":\"%s\",\"msg\":%s}\n",
           opEsc, code, msgEsc);
    fflush(stdout);
    return status;
}

// ---------------------------------------------------------------------------
// 处理 --out：解析成绝对路径 + 探测可写
//
// 返回 0 = 通过；非 0 = 已经输出过错误，直接拿它当退出码。
// ---------------------------------------------------------------------------
static int ResolveOutput(std::vector<std::string>& args, const char* action) {
    const FileAction* fa = FileActionFor(action);
    if (!fa) return 0;                  // 这个动作不写文件

    // args[0] 是动作名，所以从 1 开始找
    int idx = -1;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out") { idx = (int)i + 1; break; }
    }

    if (idx < 0) {
        // 没给 --out。有默认名的就注入一个（落在 client 的 cwd）；
        // 没有默认名的（clipboard）就什么都不做。
        if (!fa->defaultName) return 0;
        args.push_back("--out");
        args.push_back(fa->defaultName);
        idx = (int)args.size() - 1;
    } else if (idx >= (int)args.size()) {
        return EmitFail(action, "bad-arg", "--out needs a value", ST_USAGE);
    }

    char abs[32768] = "";
    if (!PathResolveAbsolute(args[idx].c_str(), abs, sizeof(abs))) {
        return EmitFail(action, "bad-out",
                        "the --out path could not be resolved to an absolute path",
                        ST_USAGE);
    }

    // 安全白名单：扩展名必须是这个动作该产出的那种。
    // 探测只回答"能不能写"，不回答"该不该写" —— 没有这条，
    // `--out 设计计划.md` 会把 PNG 写进文档里，把文档毁掉。
    if (!PathHasExtension(abs, fa->ext)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "--out must end with %s (safety rule: this action must not overwrite "
                 "a file of another kind)", fa->ext);
        return EmitFail(action, "out-bad-extension", msg, ST_REFUSED);
    }

    char why[512] = "";
    if (!PathProbeWritable(abs, why, sizeof(why))) {
        // 这里用 ST_REFUSED：这是"安全闸门拒绝了"，不是"参数写错了"，
        // 调用方据此决定要不要换个路径重试。
        return EmitFail(action, "out-not-writable", why, ST_REFUSED);
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
        rc = EmitFail(args[0].c_str(), "server-not-running",
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
    int n = wire::EncodeArgv((int)ptrs.size(), ptrs.data(), req, sizeof(req));
    if (n < 0) {
        CloseHandle(h);
        rc = EmitFail(args[0].c_str(), "request-too-long",
                      "the argument list is too long", ST_USAGE);
        WinRestoreConsole();
        return rc;
    }

    // 长度前缀定界 —— 内容里含换行也没问题（type --text 就可能是这样）
    if (!wire::SendFrame(h, req, n)) {
        CloseHandle(h);
        rc = EmitFail(args[0].c_str(), "send-failed",
                      "failed to write the request to the server", ST_SERVER);
        WinRestoreConsole();
        return rc;
    }

    // --- 收响应：第一行状态码，第二行 JSON ---
    char statusLine[64]  = "";
    char jsonLine[16384] = "";

    if (wire::RecvLine(h, statusLine, sizeof(statusLine)) < 0) {
        CloseHandle(h);
        rc = EmitFail(args[0].c_str(), "no-response",
                      "the server closed the pipe without answering", ST_SERVER);
        WinRestoreConsole();
        return rc;
    }
    if (wire::RecvLine(h, jsonLine, sizeof(jsonLine)) < 0) {
        CloseHandle(h);
        rc = EmitFail(args[0].c_str(), "no-body",
                      "the server sent a status but no result body", ST_SERVER);
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
