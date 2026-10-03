// ===========================================================================
// act_state.cpp —— ping / get_state
//
// 这两个动作都不改变系统状态，可以随时调用。
// ===========================================================================

#include "actions.h"
#include "../common/winmon.h"
#include "../common/winutil.h"
#include "../common/wire.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static ULONGLONG g_startTick = 0;

void ActionsInit(void) {
    WmLoadMonitors();
    g_startTick = GetTickCount64();
}

// ---------------------------------------------------------------------------
// JSON 拼接小工具
//
// 就是往一个固定缓冲里追加。没有解析器，所以很简单 ——
// 真正需要小心的只有"字符串要转义"和"别溢出"。
// ---------------------------------------------------------------------------
static void Cat(char* buf, int cap, const char* fmt, ...) {
    int n = (int)strlen(buf);
    if (n >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
}

// 追加 ,"key":"value"（utf8 字符串，自动 JSON 转义）
static void CatKV(char* buf, int cap, const char* key, const char* value) {
    Cat(buf, cap, ",\"%s\":", key);
    wire::JsonEscapeAppend(buf, cap, value ? value : "");
}

// 追加 ,"key":raw（数字 / true / false / 数组 —— 调用方保证是合法 JSON）
static void CatKR(char* buf, int cap, const char* key, const char* raw) {
    Cat(buf, cap, ",\"%s\":%s", key, raw);
}

// 统一的失败输出
static void FailJ(ActionResult* r, int status, const char* op,
                  const char* code, const char* msg) {
    r->status = status;
    char esc[1024] = "";
    wire::JsonEscapeAppend(esc, sizeof(esc), msg ? msg : "");
    snprintf(r->json, sizeof(r->json),
             "{\"ok\":false,\"op\":\"%s\",\"code\":\"%s\",\"msg\":%s}",
             op, code, esc);
}

// ---------------------------------------------------------------------------
// 取本进程的完整性级别和是否提权
//
// 完整性级别必须回传：server 若跑在 Low（比如被放进了 DSH 工作区），
// 它的一切输入注入都会【静默失效】—— 这是最快的自查手段。
// ---------------------------------------------------------------------------
static void GetIntegrity(char* out, int cap, bool* elevated) {
    out[0] = '\0';
    if (elevated) *elevated = false;

    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;

    BYTE  buf[512];
    DWORD ret = 0;

    if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &ret)) {
        TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)buf;
        DWORD sub = *GetSidSubAuthority(
                        tml->Label.Sid,
                        (UCHAR)(*GetSidSubAuthorityCount(tml->Label.Sid) - 1));
        const char* name =
            (sub >= 0x4000) ? "System"   :
            (sub >= 0x3000) ? "High"     :
            (sub >= 0x2000) ? "Medium"   :
            (sub >= 0x1000) ? "Low"      : "Untrusted";
        snprintf(out, cap, "S-1-16-%lu (%s)", (unsigned long)sub, name);
    }

    if (elevated) {
        TOKEN_ELEVATION te;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &ret))
            *elevated = te.TokenIsElevated != 0;
    }
    CloseHandle(tok);
}

// ===========================================================================
// ping
// ===========================================================================
static void ActPing(ActionResult* r) {
    char integ[64];
    bool elev = false;
    GetIntegrity(integ, sizeof(integ), &elev);

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"ping\"");
    Cat(r->json, sizeof(r->json), ",\"version\":\"%s\"", CUA_VERSION);
    Cat(r->json, sizeof(r->json), ",\"pid\":%lu", (unsigned long)GetCurrentProcessId());
    CatKV(r->json, sizeof(r->json), "integrity", integ);
    CatKR(r->json, sizeof(r->json), "elevated", elev ? "true" : "false");
    Cat(r->json, sizeof(r->json), ",\"uptime_s\":%llu",
        (unsigned long long)((GetTickCount64() - g_startTick) / 1000));
    Cat(r->json, sizeof(r->json), "}");
    r->status = ST_OK;
}

// ===========================================================================
// get_state
//
// 参数：
//   --cursor  只返回光标相关
//   --held    只返回当前按住的键和鼠标按钮
//   两个都给 -> 返回这两项；都不给 -> 返回全部
// ===========================================================================
static void ActGetState(int argc, char** argv, ActionResult* r) {
    bool onlyCursor = false, onlyHeld = false;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--cursor")) onlyCursor = true;
        else if (!strcmp(argv[i], "--held"))   onlyHeld   = true;
        else { FailJ(r, ST_USAGE, "get_state", "unknown-option", argv[i]); return; }
    }

    const bool wantCursor = !onlyHeld;
    const bool wantHeld   = !onlyCursor;
    const bool wantFull   = !onlyCursor && !onlyHeld;

    POINT cur;
    GetCursorPos(&cur);
    int cm = WmMonitorFromPoint(cur);

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"get_state\"");

    if (wantCursor) {
        int lx = (int)cur.x, ly = (int)cur.y;
        if (cm >= 0) {
            lx -= (int)g_wmMon[cm].rc.left;
            ly -= (int)g_wmMon[cm].rc.top;
        }
        Cat(r->json, sizeof(r->json),
            ",\"cursor\":{\"local\":[%d,%d],\"monitor\":%d}", lx, ly, cm);

        char flags[64];
        WinCursorFlagsText(flags, sizeof(flags));
        CatKV(r->json, sizeof(r->json), "cursor_flags", flags);
    }

    if (wantFull) {
        Cat(r->json, sizeof(r->json), ",\"monitor_count\":%d", g_wmMonCount);
        Cat(r->json, sizeof(r->json), ",\"monitors\":[");
        for (int i = 0; i < g_wmMonCount; ++i) {
            const WmMon& m = g_wmMon[i];
            int w  = (int)(m.rc.right - m.rc.left);
            int h  = (int)(m.rc.bottom - m.rc.top);
            int wx = (int)(m.work.left - m.rc.left);
            int wy = (int)(m.work.top  - m.rc.top);
            int ww = (int)(m.work.right - m.work.left);
            int wh = (int)(m.work.bottom - m.work.top);
            unsigned scale = m.dpiX ? (m.dpiX * 100 / 96) : 0;

            if (i) Cat(r->json, sizeof(r->json), ",");
            Cat(r->json, sizeof(r->json), "{\"i\":%d,\"dev\":", i);
            {
                char dev[128];
                // 设备名是宽字符，转 UTF-8 后按 JSON 字符串输出
                int n = WideCharToMultiByte(CP_UTF8, 0, m.device, -1, dev, sizeof(dev), NULL, NULL);
                if (n <= 0) dev[0] = '\0';
                wire::JsonEscapeAppend(r->json, sizeof(r->json), dev);
            }
            Cat(r->json, sizeof(r->json),
                ",\"size\":[%d,%d],\"work\":[%d,%d,%d,%d],\"primary\":%s,\"dpi\":%u,\"scale\":%u}",
                w, h, wx, wy, ww, wh, m.primary ? "true" : "false", m.dpiX, scale);
        }
        Cat(r->json, sizeof(r->json), "]");

        char fc[256] = "", ft[512] = "";
        WinGetForegroundInfo(fc, sizeof(fc), ft, sizeof(ft));
        Cat(r->json, sizeof(r->json), ",\"foreground\":{\"class\":");
        wire::JsonEscapeAppend(r->json, sizeof(r->json), fc);
        Cat(r->json, sizeof(r->json), ",\"title\":");
        wire::JsonEscapeAppend(r->json, sizeof(r->json), ft);
        Cat(r->json, sizeof(r->json), "}");

        char uc[256] = "", ut[512] = "";
        WinGetWindowUnderCursor(cur, uc, sizeof(uc), ut, sizeof(ut));
        Cat(r->json, sizeof(r->json), ",\"under_cursor\":{\"class\":");
        wire::JsonEscapeAppend(r->json, sizeof(r->json), uc);
        Cat(r->json, sizeof(r->json), ",\"title\":");
        wire::JsonEscapeAppend(r->json, sizeof(r->json), ut);
        Cat(r->json, sizeof(r->json), "}");

        // 注意坐标系：这是唯一的例外，clip 天生是【屏幕空间】的矩形，
        // 不是显示器局部坐标 —— 它可能横跨多台显示器。
        RECT clip;
        if (WinGetClipRect(&clip)) {
            Cat(r->json, sizeof(r->json), ",\"clip_screen\":[%d,%d,%d,%d]",
                (int)clip.left, (int)clip.top,
                (int)(clip.right - clip.left), (int)(clip.bottom - clip.top));
        }
    }

    if (wantHeld) {
        char held[1024] = "";
        WinGetHeldInput(held, sizeof(held));
        CatKV(r->json, sizeof(r->json), "held", held);
    }

    Cat(r->json, sizeof(r->json), "}");
    r->status = ST_OK;
}

// ===========================================================================
// 分发
// ===========================================================================
void DispatchAction(int argc, char** argv, ActionResult* res) {
    res->status = ST_OK;
    res->json[0] = '\0';

    if (argc <= 0) {
        FailJ(res, ST_USAGE, "", "no-action", "no action given");
        return;
    }

    const char* op = argv[0];

    if      (!strcmp(op, "ping"))      ActPing(res);
    else if (!strcmp(op, "get_state")) ActGetState(argc, argv, res);
    else FailJ(res, ST_USAGE, op, "unknown-action", "unknown action");
}
