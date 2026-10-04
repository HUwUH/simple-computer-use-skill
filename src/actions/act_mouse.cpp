// ===========================================================================
// act_mouse.cpp —— move / click
//
// ---------------------------------------------------------------------------
// 这个文件里最重要的两条纪律
// ---------------------------------------------------------------------------
// 1) 【不信返回值】。SetCursorPos 失败时返回 FALSE 且 GetLastError() 是 0；
//    目标越界时它还会把光标【静默钳】到最近的屏幕边缘、然后返回 TRUE。
//    所以只有"移完之后读回来的位置等于目标"才算成功。
//
// 2) 【越界必须拒绝】。绝不能让它被静默钳掉 —— 那种"看起来成功了、其实点到
//    了别处"的行为是最危险的。
//
// ---------------------------------------------------------------------------
// 坐标
// ---------------------------------------------------------------------------
// 参数一律是【显示器局部坐标】（左上角 0,0），需要时用 WmLocalToScreen 换算成
// 屏幕坐标；返回给调用方的也一律是局部坐标。
// ===========================================================================

#include "actions.h"
#include "jsonhelp.h"
#include "../common/winmon.h"
#include "../common/winutil.h"
#include "../common/wire.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 局部坐标 -> 屏幕坐标，带越界检查
// ---------------------------------------------------------------------------
static bool LocalToScreenChecked(int mon, int lx, int ly, POINT* out, char* err, int errCap) {
    if (mon < 0 || mon >= g_wmMonCount) {
        snprintf(err, errCap, "monitor %d does not exist (only %d monitor(s))",
                 mon, g_wmMonCount);
        return false;
    }
    if (WmMonitorFromLocal(mon, lx, ly) < 0) {
        const WmMon& m = g_wmMon[mon];
        int w = (int)(m.rc.right - m.rc.left);
        int h = (int)(m.rc.bottom - m.rc.top);
        snprintf(err, errCap,
                 "(%d,%d) is outside monitor %d (size %dx%d); coordinates are "
                 "monitor-local, see get_state", lx, ly, mon, w, h);
        return false;
    }
    *out = WmLocalToScreen(mon, lx, ly);
    return true;
}

// ---------------------------------------------------------------------------
// 移动光标并读回验证
// ---------------------------------------------------------------------------
static bool SetAndVerifyCursor(POINT target, POINT* actual, int* errPx) {
    SetCursorPos(target.x, target.y);

    POINT got;
    GetCursorPos(&got);
    if (actual) *actual = got;

    int dx = (int)(got.x - target.x);
    int dy = (int)(got.y - target.y);
    int e  = (int)sqrt((double)(dx * dx + dy * dy));
    if (errPx) *errPx = e;

    return e == 0;
}

// ---------------------------------------------------------------------------
// 发送按键
//
// ⚠️ 不回报 SendInput 的返回值 —— 它是"成功塞进队列的事件数"，
//    而且今天实测过：在 Low 完整性下它返回 1 却什么都没发生。放进响应里
//    只会引导调用方去信它。要确认生效，调用方应该再截一张图。
// ---------------------------------------------------------------------------
static void SendMousePair(WORD down, WORD up) {
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    in[0].type       = INPUT_MOUSE;
    in[0].mi.dwFlags = down;
    in[1].type       = INPUT_MOUSE;
    in[1].mi.dwFlags = up;
    SendInput(2, in, sizeof(INPUT));
}

// "double" 不是按键，是"左键双击" —— 按设计稿的口径放在 --button 里
static bool ButtonFlags(const char* name, WORD* down, WORD* up, bool* isDouble) {
    *isDouble = false;
    if      (!strcmp(name, "left"))   { *down = MOUSEEVENTF_LEFTDOWN;   *up = MOUSEEVENTF_LEFTUP;   }
    else if (!strcmp(name, "right"))  { *down = MOUSEEVENTF_RIGHTDOWN;  *up = MOUSEEVENTF_RIGHTUP;  }
    else if (!strcmp(name, "middle")) { *down = MOUSEEVENTF_MIDDLEDOWN; *up = MOUSEEVENTF_MIDDLEUP; }
    else if (!strcmp(name, "double")) { *down = MOUSEEVENTF_LEFTDOWN;   *up = MOUSEEVENTF_LEFTUP;
                                        *isDouble = true; }
    else return false;
    return true;
}

static void DoClick(WORD down, WORD up, bool doubleClick) {
    SendMousePair(down, up);
    if (doubleClick) {
        // 两击必须落在系统认定的双击时间内（GetDoubleClickTime，默认 500ms），
        // 中间也不能插入别的鼠标事件 —— 所以分两批发，中间小睡一下。
        Sleep(60);
        SendMousePair(down, up);
    }
}

// 把一个屏幕点换算成"它所在显示器 + 该显示器内的局部坐标"
static void ScreenToLocalReport(POINT pt, int* monOut, int* lxOut, int* lyOut) {
    int m = WmMonitorFromPoint(pt);
    int x = (int)pt.x, y = (int)pt.y;
    if (m >= 0) {
        x -= (int)g_wmMon[m].rc.left;
        y -= (int)g_wmMon[m].rc.top;
    }
    *monOut = m;
    *lxOut  = x;
    *lyOut  = y;
}

// ===========================================================================
// move
//
//   X Y            该显示器内的绝对坐标
//   --dx/--dy      相对当前光标位置的偏移（与 X Y 互斥）
//   --monitor N    默认光标所在显示器
// ===========================================================================
void ActMove(int argc, char** argv, ActionResult* r) {
    int  mon     = -1;
    int  pos[2]  = { 0, 0 };
    int  npos    = 0;
    bool hasDelta = false;
    int  dx = 0, dy = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--monitor") && i + 1 < argc) mon = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dx")      && i + 1 < argc) { dx = atoi(argv[++i]); hasDelta = true; }
        else if (!strcmp(argv[i], "--dy")      && i + 1 < argc) { dy = atoi(argv[++i]); hasDelta = true; }
        else if (argv[i][0] != '-' && npos < 2)                 pos[npos++] = atoi(argv[i]);
        else { FailJ(r, ST_USAGE, "move", "bad-arg", argv[i]); return; }
    }

    const bool hasXY = (npos == 2);
    if (hasXY && hasDelta) {
        FailJ(r, ST_USAGE, "move", "conflicting-args",
              "give either X Y or --dx/--dy, not both");
        return;
    }
    if (npos == 1) {
        FailJ(r, ST_USAGE, "move", "need-target", "X and Y must both be given");
        return;
    }
    if (!hasXY && !hasDelta) {
        FailJ(r, ST_USAGE, "move", "need-target",
              "give X Y (monitor-local) or --dx/--dy (relative offset)");
        return;
    }

    // 默认显示器 = 光标所在的那台
    if (mon < 0) {
        POINT c;
        GetCursorPos(&c);
        mon = WmMonitorFromPoint(c);
        if (mon < 0) mon = 0;
    }

    POINT target;
    char  err[256] = "";

    if (hasDelta) {
        POINT cur;
        GetCursorPos(&cur);
        target.x = cur.x + dx;
        target.y = cur.y + dy;
        if (WmMonitorFromPoint(target) < 0) {
            FailJ(r, ST_REFUSED, "move", "not-on-monitor",
                  "the offset would land outside every monitor");
            return;
        }
    } else {
        if (!LocalToScreenChecked(mon, pos[0], pos[1], &target, err, sizeof(err))) {
            FailJ(r, ST_REFUSED, "move", "not-on-monitor", err);
            return;
        }
    }

    POINT actual;
    int   errPx = 0;
    if (!SetAndVerifyCursor(target, &actual, &errPx)) {
        // 走到了这里说明光标没落在要求的位置上（多半是被钳到屏幕边缘了）
        FailJ(r, ST_REFUSED, "move", "landed-elsewhere",
              "the cursor did not land on the requested point; it may have been "
              "clamped to a monitor edge");
        return;
    }

    int am = 0, lx = 0, ly = 0;
    ScreenToLocalReport(actual, &am, &lx, &ly);

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"move\"");
    Cat(r->json, sizeof(r->json), ",\"monitor\":%d", am);
    Cat(r->json, sizeof(r->json), ",\"at\":[%d,%d]", lx, ly);
    Cat(r->json, sizeof(r->json), ",\"err_px\":%d", errPx);
    Cat(r->json, sizeof(r->json), "}");
    r->status = ST_OK;
}

// ===========================================================================
// click
//
//   X Y            可选。给了就先移过去（并验证），没给就在当前光标位置点
//   --monitor N    默认光标所在显示器
//   --button       left(默认) / right / middle / double
//
// ★ 任何点击之前都会先亮 1 秒黄圈；这 1 秒里光标移动超过 50px 就取消。
// ===========================================================================
void ActClick(int argc, char** argv, ActionResult* r) {
    int         mon    = -1;
    const char* button = "left";
    int         pos[2] = { 0, 0 };
    int         npos   = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--monitor") && i + 1 < argc) mon    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--button")  && i + 1 < argc) button = argv[++i];
        else if (argv[i][0] != '-' && npos < 2)                 pos[npos++] = atoi(argv[i]);
        else { FailJ(r, ST_USAGE, "click", "bad-arg", argv[i]); return; }
    }

    WORD down = 0, up = 0;
    bool isDouble = false;
    if (!ButtonFlags(button, &down, &up, &isDouble)) {
        FailJ(r, ST_USAGE, "click", "bad-button",
              "--button must be left, right, middle or double");
        return;
    }

    // --- 1) 确定目标点 ---
    POINT target;
    char  err[256] = "";

    if (npos == 2) {
        if (mon < 0) {
            POINT c;
            GetCursorPos(&c);
            mon = WmMonitorFromPoint(c);
            if (mon < 0) mon = 0;
        }
        if (!LocalToScreenChecked(mon, pos[0], pos[1], &target, err, sizeof(err))) {
            FailJ(r, ST_REFUSED, "click", "not-on-monitor", err);
            return;
        }
        POINT actual;
        int   errPx = 0;
        if (!SetAndVerifyCursor(target, &actual, &errPx)) {
            FailJ(r, ST_REFUSED, "click", "move-failed",
                  "could not move the cursor to the requested point; click not sent");
            return;
        }
    } else if (npos == 1) {
        FailJ(r, ST_USAGE, "click", "need-target", "X and Y must both be given");
        return;
    } else {
        GetCursorPos(&target);   // 就在当前位置点
    }

    // --- 2) ★ 预备窗口：1 秒黄圈，人移动超过 50px 就否决 ---
    int movedPx = 0;
    if (!OverlayArmWait(target, &movedPx)) {
        FailJ(r, ST_REFUSED, "click", "cancelled-by-user-motion",
              "the cursor moved more than 50 px during the 1 s arm window, so the "
              "click was cancelled. This is a normal outcome (the human vetoed it), "
              "not a failure to retry blindly.");
        return;
    }

    // --- 3) 就点光标【现在】所在的位置 ---
    //
    // ⚠️ 不要在这里把光标"召回"原目标！
    //
    // 那 50px 的容差有第二层用途：agent 瞄偏了一点时，人可以在这一秒里把光标
    // 轻轻推到正确的位置上。召回会把这下修正直接抹掉，等于白让人帮忙。
    //
    // 所以以【最终光标位置】为准，并且照实回报给调用方。
    GetCursorPos(&target);

    // --- 4) 点击 ---
    DoClick(down, up, isDouble);

    // 点击是异步生效的，稍微等一下再读前台窗口，否则读到的是点击之前的那个
    Sleep(60);

    // --- 5) 返回 ---
    int am = 0, lx = 0, ly = 0;
    ScreenToLocalReport(target, &am, &lx, &ly);

    char fc[256] = "", ft[512] = "";
    WinGetForegroundInfo(fc, sizeof(fc), ft, sizeof(ft));

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"click\"");
    Cat(r->json, sizeof(r->json), ",\"monitor\":%d", am);
    Cat(r->json, sizeof(r->json), ",\"at\":[%d,%d]", lx, ly);
    CatKV(r->json, sizeof(r->json), "button", button);
    Cat(r->json, sizeof(r->json), ",\"moved_px\":%d", movedPx);
    Cat(r->json, sizeof(r->json), ",\"foreground\":{\"class\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), fc);
    Cat(r->json, sizeof(r->json), ",\"title\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), ft);
    Cat(r->json, sizeof(r->json), "}}");
    r->status = ST_OK;
}
