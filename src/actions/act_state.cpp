// ===========================================================================
// act_state.cpp —— ping / get_state
//
// 这两个动作都不改变系统状态，可以随时调用。
// ===========================================================================

#include "actions.h"
#include "jsonhelp.h"
#include "../common/winmon.h"
#include "../common/winutil.h"

#include <stdio.h>
#include <string.h>

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
void ActPing(ActionResult* r) {
    char integ[64];
    bool elev = false;
    GetIntegrity(integ, sizeof(integ), &elev);

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"ping\"");
    Cat(r->json, sizeof(r->json), ",\"version\":\"%s\"", CUA_VERSION);
    Cat(r->json, sizeof(r->json), ",\"pid\":%lu", (unsigned long)GetCurrentProcessId());
    CatKV(r->json, sizeof(r->json), "integrity", integ);
    CatKR(r->json, sizeof(r->json), "elevated", elev ? "true" : "false");
    Cat(r->json, sizeof(r->json), ",\"uptime_s\":%llu", ActionsUptimeSeconds());
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
void ActGetState(int argc, char** argv, ActionResult* r) {
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
                WinToUtf8(m.device, dev, sizeof(dev));
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

        // 唯一的例外：clip 天生是【屏幕空间】的矩形，可能横跨多台显示器
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
