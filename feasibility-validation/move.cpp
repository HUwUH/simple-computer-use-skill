// move.cpp — 把光标移到指定的物理像素坐标，然后读回验证。
// 命令行与退出码见 PrintUsage()。
//
// 为什么要读回：SetCursorPos 返回非 0 只代表调用被受理。坐标落在虚拟屏幕
// 包围盒内、却不在任何显示器矩形上时，Windows 会把光标钳到最近显示器的
// 边缘，而函数仍返回成功。所以移动之后必须 GetCursorPos 读回来比对。

#include "winmon.h"

// 离 (x,y) 最近的"落得上去的点"：在哪台显示器上、坐标是多少。
// 诊断用：读回值若正好等于它，说明光标是被钳到最近边缘了。
// 坐标夹进显示器矩形即得最近点（right / bottom 是开区间，所以要 -1）。
static int WmNearestPointOnMonitors(int x, int y, POINT* out) {
    int best = -1;
    long long bestD = 0;

    for (int i = 0; i < g_wmMonCount; ++i) {
        const RECT& rc = g_wmMon[i].rc;
        int cx = (x < rc.left) ? (int)rc.left : ((x > rc.right  - 1) ? (int)rc.right  - 1 : x);
        int cy = (y < rc.top ) ? (int)rc.top  : ((y > rc.bottom - 1) ? (int)rc.bottom - 1 : y);

        long long dx = (long long)x - cx;
        long long dy = (long long)y - cy;
        long long d  = dx * dx + dy * dy;

        if (best < 0 || d < bestD) {
            best = i;
            bestD = d;
            if (out) { out->x = cx; out->y = cy; }
        }
    }
    return best;
}

// 这个参数是数字（可能是负数）吗？
// 多屏时主屏左边 / 上边的显示器坐标是负的，"-100 50" 必须被认成坐标而不是
// 选项。选项一律写成 --xxx，不会撞上。
static int WmLooksNumeric(const wchar_t* s) {
    if (!s || !s[0]) return 0;
    if (s[0] == L'-' || s[0] == L'+') return s[1] >= L'0' && s[1] <= L'9';
    return s[0] >= L'0' && s[0] <= L'9';
}

static void PrintUsage(void) {
    printf("usage: move.exe X Y [--dry] [--tolerance N]\n");
    printf("       move.exe --dx N [--dy M] [--dry] [--tolerance N]\n");
    printf("  X Y           absolute target, physical screen pixels (never scaled;\n");
    printf("                negative values are normal on a monitor left of / above\n");
    printf("                the primary one)\n");
    printf("  --dx/--dy     relative move from the current cursor position\n");
    printf("  --dry         report the target and the prediction, do not move\n");
    printf("  --tolerance N accept |error| <= N px on readback (default 0)\n");
}

int wmain(int argc, wchar_t** argv) {
    WmInitConsole();

    // DPI 感知必须最先调用，早于任何 GDI / 窗口调用
    const char* dpiMode = WmEnablePerMonitorV2();
    WmLoadMonitors();

    // 解析参数
    int absCount = 0, absX = 0, absY = 0;   // 位置参数 X Y
    int relCount = 0, relX = 0, relY = 0;   // --dx / --dy
    int dry = 0, tolerance = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--dx") && i + 1 < argc) { relX = _wtoi(argv[++i]); relCount++; }
        else if (!wcscmp(argv[i], L"--dy") && i + 1 < argc) { relY = _wtoi(argv[++i]); relCount++; }
        else if (!wcscmp(argv[i], L"--dry")) dry = 1;
        else if (!wcscmp(argv[i], L"--tolerance") && i + 1 < argc) tolerance = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { PrintUsage(); return 0; }
        else if (argv[i][0] == L'-' && !WmLooksNumeric(argv[i])) {
            printf("unknown arg: "); WmPrintW(argv[i]); printf("\n");
            PrintUsage(); return 2;
        }
        else if (absCount == 0) { absX = _wtoi(argv[i]); absCount = 1; }
        else if (absCount == 1) { absY = _wtoi(argv[i]); absCount = 2; }
        else { printf("ERROR: too many positional args (need at most X Y)\n"); PrintUsage(); return 2; }
    }

    if (absCount && relCount) { printf("ERROR: give either X Y or --dx/--dy, not both\n"); return 2; }
    if (absCount == 1)        { printf("ERROR: X and Y must be given together\n");         return 2; }
    if (!absCount && !relCount) { printf("ERROR: nothing to do\n"); PrintUsage(); return 2; }
    if (tolerance < 0) tolerance = 0;

    printf("=== display layout (queried fresh on every run) ===\n");
    printf("dpi awareness  : %s\n", dpiMode);
    WmPrintVirtualScreen();
    WmPrintMonitors();

    // 目标点
    POINT before;
    if (!GetCursorPos(&before)) { printf("ERROR: GetCursorPos failed\n"); return 3; }

    int tx, ty;
    if (relCount) {
        // 物理坐标下的相对移动就是加法，不做缩放换算
        tx = (int)before.x + relX;
        ty = (int)before.y + relY;
    } else {
        tx = absX;
        ty = absY;
    }

    int tmon = WmMonitorFromXY(tx, ty);

    printf("\n=== move plan ===\n");
    printf("mode           : %s\n", relCount ? "relative (--dx/--dy)" : "absolute (X Y)");
    printf("cursor before  : (%d,%d)  on monitor %d\n", (int)before.x, (int)before.y, WmMonitorFromPoint(before));
    printf("target         : (%d,%d)", tx, ty);
    if (tmon >= 0) {
        printf("  on monitor %d  local(%d,%d)\n", tmon,
               tx - (int)g_wmMon[tmon].rc.left,
               ty - (int)g_wmMon[tmon].rc.top);
    } else {
        printf("  NOT on any monitor (virtual-desktop hole)\n");
        // 不能命名 near：windows.h 把 near / far 定义成空宏，
        // "POINT near;" 会被展开成 "POINT ;"
        POINT nearPt;
        nearPt.x = tx;   // 初始化只防 g_wmMonCount == 0 时读到未初始化值
        nearPt.y = ty;
        int nearMon = WmNearestPointOnMonitors(tx, ty, &nearPt);
        printf("WARN: the target is inside the virtual-screen bounding box but outside\n");
        printf("      every monitor rect. Expect SetCursorPos to silently clamp it to\n");
        printf("      the nearest edge (%d,%d) on monitor %d and still return success.\n",
               (int)nearPt.x, (int)nearPt.y, nearMon);
        printf("      The readback below is what catches it.\n");
    }

    if (dry) {
        printf("\n--dry: nothing moved. exit 0\n");
        return 0;
    }

    // 移动
    if (!SetCursorPos(tx, ty)) {
        DWORD e = GetLastError();
        printf("ERROR: SetCursorPos(%d,%d) failed, GetLastError=%lu\n", tx, ty, e);
        if (e == ERROR_ACCESS_DENIED) {
            printf("       the input desktop is not the current desktop.\n");
            printf("       Happens while a UAC prompt / the lock screen / the\n");
            printf("       Ctrl+Alt+Del screen is up.\n");
        }
        return 3;
    }

    // 读回：钳制是同步发生的，多读几次只是给别的程序也在动鼠标留余量
    POINT after;
    after.x = 0;
    after.y = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!GetCursorPos(&after)) { printf("ERROR: GetCursorPos failed\n"); return 3; }
        if ((int)after.x == tx && (int)after.y == ty) break;
        Sleep(2);
    }

    int ex = (int)after.x - tx;
    int ey = (int)after.y - ty;
    int ok = (ex <= tolerance && ex >= -tolerance && ey <= tolerance && ey >= -tolerance);

    printf("\n=== move result ===\n");
    printf("requested      : (%d,%d)\n", tx, ty);
    printf("cursor after   : (%d,%d)  on monitor %d\n",
           (int)after.x, (int)after.y, WmMonitorFromPoint(after));
    printf("error          : dx=%+d  dy=%+d   (tolerance %d)\n", ex, ey, tolerance);
    printf("verdict        : %s\n", ok ? "HIT - cursor is exactly where we asked" :
                                       "MISMATCH - the move did not take effect");

    if (!ok) {
        POINT nearPt;
        nearPt.x = tx;
        nearPt.y = ty;
        int nearMon = WmNearestPointOnMonitors(tx, ty, &nearPt);

        printf("\n--- why? ---\n");
        if (tmon < 0) {
            printf("the target (%d,%d) is not inside any monitor rect: it is in the\n", tx, ty);
            printf("virtual-desktop hole (inside the bounding box, outside every screen).\n");
        }
        if (nearMon >= 0 && (int)after.x == nearPt.x && (int)after.y == nearPt.y && (ex || ey)) {
            printf("the cursor landed exactly on (%d,%d) = the nearest point of monitor %d.\n",
                   (int)nearPt.x, (int)nearPt.y, nearMon);
            printf("=> consistent with SILENT CLAMPING: Windows moved it to the nearest\n");
            printf("   edge on its own, and SetCursorPos still reported success.\n");
        } else {
            printf("the cursor did not land on the nearest edge either, so something\n");
            printf("other than the clamp moved or refused it (another process, or the\n");
            printf("input desktop changed while we ran).\n");
        }
        printf("\npolicy: exit 1. A caller that clicks must stop here instead of\n");
        printf("        clicking at the wrong place.\n");
    }

    return ok ? 0 : 1;
}
