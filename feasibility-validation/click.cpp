// click.cpp — 在指定的物理像素坐标点击。
//
// 三关，任一关不过就拒绝点击：报出目标点下面的窗口 → 移动并读回验证 →
// 复检光标下的窗口没变。
// 退出码：0 已点击 / 1 拒绝点击 / 2 参数错误 / 3 定位失败 / 4 SendInput 失败

#include "winmon.h"

// Vista+ 的常量，老头文件里可能没有
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

// 一个点下面的窗口快照
struct WmWin {
    HWND  hwnd;              // WindowFromPoint 直接返回的那个（可能是子窗口，比如按钮）
    HWND  root;              // 它所属的顶层窗口
    WCHAR cls[128];          // 窗口类名
    WCHAR title[256];        // 窗口标题
    DWORD pid;               // 所属进程
    WCHAR image[MAX_PATH];   // 进程 exe 全路径；查不到就留空
};

// QueryFullProcessImageNameW 也是 Vista+ 的，跟 winmon.h 的做法一致：
// 动态取地址，编译期不依赖头文件版本。
typedef BOOL (WINAPI *PFN_QueryFullProcessImageNameW)(HANDLE, DWORD, LPWSTR, PDWORD);

static void WmQueryExePath(DWORD pid, WCHAR* out, int cch) {
    out[0] = 0;
    if (!pid) return;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_QueryFullProcessImageNameW fn = k32
        ? (PFN_QueryFullProcessImageNameW)(void*)GetProcAddress(k32, "QueryFullProcessImageNameW")
        : NULL;
    if (!fn) return;

    // 先要权限最小的那个；老系统上没有它再退回 QUERY_INFORMATION。
    // 都失败也正常：系统进程、更高完整性级别的进程不让看。
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return;

    DWORD n = (DWORD)cch;
    if (!fn(h, 0, out, &n)) out[0] = 0;
    CloseHandle(h);
}

static void WmQueryWindowAt(POINT pt, WmWin* w) {
    ZeroMemory(w, sizeof(*w));

    // WindowFromPoint 的命中测试跟鼠标点击同一套规则，所以它就是
    // "点下去会落到谁身上"的答案；它只看到可见且未被禁用的窗口。
    w->hwnd = WindowFromPoint(pt);
    if (!w->hwnd) return;

    w->root = GetAncestor(w->hwnd, GA_ROOT);
    if (!w->root) w->root = w->hwnd;

    GetClassNameW(w->hwnd, w->cls, 127);
    w->cls[127] = 0;

    // 用 GetWindowTextW 而不是 SendMessage(WM_GETTEXT)：取别的进程的窗口时
    // 它读窗口缓存的标题、不发消息过去，所以对方卡死也不会拖住我们。
    // 代价是拿不到别的进程里子控件的文字，这时退一步用顶层窗口的标题。
    if (GetWindowTextW(w->hwnd, w->title, 255) == 0) {
        GetWindowTextW(w->root, w->title, 255);
    }
    w->title[255] = 0;

    GetWindowThreadProcessId(w->hwnd, &w->pid);
    WmQueryExePath(w->pid, w->image, MAX_PATH);
}

static void WmPrintWin(const char* label, const WmWin* w) {
    printf("%s\n", label);
    if (!w->hwnd) {
        printf("  (WindowFromPoint returned NULL -- no window at that point)\n");
        return;
    }
    printf("  hwnd   : 0x%p  (root 0x%p)\n", (void*)w->hwnd, (void*)w->root);
    printf("  class  : "); WmPrintW(w->cls);   printf("\n");
    printf("  title  : "); WmPrintW(w->title); printf("\n");
    printf("  pid    : %lu\n", (unsigned long)w->pid);
    printf("  exe    : ");
    if (w->image[0]) WmPrintW(w->image);
    else             printf("(unavailable - access denied or process gone)");
    printf("\n");
}

// 第 1 关和第 3 关的报告是不是同一个目标
static int WmSameTarget(const WmWin* a, const WmWin* b) {
    if (!a->hwnd || !b->hwnd) return a->hwnd == b->hwnd;
    return (a->hwnd == b->hwnd) && (a->pid == b->pid);
}

// 移动 + 读回验证，跟 move.exe 同一套做法。
//   1 = 读回精确命中
//   0 = 没命中（钳制 / 被别的东西挪走）→ 调用方必须拒绝点击
//  -1 = API 本身失败
static int WmMoveVerified(int x, int y, POINT* got) {
    if (!SetCursorPos(x, y)) {
        printf("ERROR: SetCursorPos(%d,%d) failed, GetLastError=%lu\n", x, y, GetLastError());
        return -1;
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!GetCursorPos(got)) { printf("ERROR: GetCursorPos failed\n"); return -1; }
        if ((int)got->x == x && (int)got->y == y) return 1;
        Sleep(2);
    }
    return 0;
}

// 发输入。用 SendInput 而不是 mouse_event：它返回实际插入队列的事件数，
// 可以拿来当验证依据；失败是静默的（UIPI 拦截时返回 0），所以必须看返回值。
static int WmSendClick(int button, int dbl) {
    // 先补一个零位移的注入移动事件：SetCursorPos 会引发移动消息，但个别
    // 工具包（Qt / WPF / 自绘命中测试）只认真正的鼠标输入事件，不刷新
    // 悬停 / 热跟踪状态就点不中。它同时是探针：这一步就被 UIPI 挡下的话，
    // 后面的点击必然也发不出去，正好提前报出来。
    INPUT mv;
    ZeroMemory(&mv, sizeof(mv));
    mv.type = INPUT_MOUSE;
    mv.mi.dwFlags = MOUSEEVENTF_MOVE;   // dx = dy = 0，光标不动
    if (SendInput(1, &mv, sizeof(INPUT)) != 1) {
        printf("ERROR: injected move rejected, GetLastError=%lu\n", GetLastError());
        printf("       (the target window most likely runs at a higher integrity\n");
        printf("        level than us, so UIPI blocks injected input)\n");
        return 0;
    }

    DWORD down, up;
    if      (button == 1) { down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP;  }
    else if (button == 2) { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
    else                  { down = MOUSEEVENTF_LEFTDOWN;   up = MOUSEEVENTF_LEFTUP;   }

    // 双击 = 两对按下 / 抬起，必须一次发完：Windows 靠两次点击的时间间隔
    // 自己把第二次 WM_LBUTTONDOWN 合成为 WM_LBUTTONDBLCLK，中间插 Sleep
    // 就会被当成两次单击。
    INPUT ev[4];
    ZeroMemory(ev, sizeof(ev));
    UINT n = 0;
    for (int k = 0; k < (dbl ? 2 : 1); ++k) {
        ev[n].type = INPUT_MOUSE; ev[n].mi.dwFlags = down; ++n;
        ev[n].type = INPUT_MOUSE; ev[n].mi.dwFlags = up;   ++n;
    }

    UINT sent = SendInput(n, ev, sizeof(INPUT));
    if (sent != n) {
        printf("ERROR: SendInput inserted %u of %u events, GetLastError=%lu\n",
               sent, n, GetLastError());
        return 0;
    }
    return 1;
}

// 这个参数是数字（可能是负数）吗？主屏左边 / 上边的显示器坐标是负的，
// "-100 50" 必须被认成坐标而不是选项。选项一律是 --xxx，不会撞上。
// （跟 move.cpp 里同名函数一样，两个 exe 各自独立编译。）
static int WmLooksNumeric(const wchar_t* s) {
    if (!s || !s[0]) return 0;
    if (s[0] == L'-' || s[0] == L'+') return s[1] >= L'0' && s[1] <= L'9';
    return s[0] >= L'0' && s[0] <= L'9';
}

static void PrintUsage(void) {
    printf("usage: click.exe [X Y] [--left|--right|--middle] [--double] [--dry]\n");
    printf("  no X Y        click where the cursor already is\n");
    printf("  X Y           physical screen pixels; the cursor is moved there and\n");
    printf("                verified by readback BEFORE anything is clicked\n");
    printf("                (negative values are normal on a monitor left of / above\n");
    printf("                the primary one)\n");
    printf("  --dry         only report the window under the target, never click\n");
}

int wmain(int argc, wchar_t** argv) {
    WmInitConsole();

    // DPI 感知必须最先调用，早于任何 GDI / 窗口调用
    const char* dpiMode = WmEnablePerMonitorV2();
    WmLoadMonitors();

    // 解析参数
    int button = 0;   // 0 = 左键, 1 = 右键, 2 = 中键
    int dbl = 0, dry = 0;
    int xyCount = 0, argX = 0, argY = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--right"))  button = 1;
        else if (!wcscmp(argv[i], L"--middle")) button = 2;
        else if (!wcscmp(argv[i], L"--left"))   button = 0;
        else if (!wcscmp(argv[i], L"--double")) dbl = 1;
        else if (!wcscmp(argv[i], L"--dry"))    dry = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { PrintUsage(); return 0; }
        else if (argv[i][0] == L'-' && !WmLooksNumeric(argv[i])) {
            printf("unknown arg: "); WmPrintW(argv[i]); printf("\n");
            PrintUsage(); return 2;
        }
        else if (xyCount == 0) { argX = _wtoi(argv[i]); xyCount = 1; }
        else if (xyCount == 1) { argY = _wtoi(argv[i]); xyCount = 2; }
        else { printf("ERROR: too many positional args (need at most X Y)\n"); PrintUsage(); return 2; }
    }
    if (xyCount == 1) { printf("ERROR: X and Y must be given together\n"); return 2; }

    const int hasTarget = (xyCount == 2);

    printf("=== display layout (queried fresh on every run) ===\n");
    printf("dpi awareness  : %s\n", dpiMode);
    WmPrintVirtualScreen();
    WmPrintMonitors();

    POINT cur;
    if (!GetCursorPos(&cur)) { printf("ERROR: GetCursorPos failed\n"); return 3; }

    POINT target;
    target.x = hasTarget ? (LONG)argX : cur.x;
    target.y = hasTarget ? (LONG)argY : cur.y;

    const char* btnName = (button == 1) ? "right" : ((button == 2) ? "middle" : "left");

    printf("\n=== what this run is about to do ===\n");
    printf("action         : %s%s\n", btnName, dbl ? " double-click" : " click");
    printf("cursor now     : (%d,%d)  on monitor %d\n",
           (int)cur.x, (int)cur.y, WmMonitorFromPoint(cur));
    printf("target point   : (%d,%d)  %s\n", (int)target.x, (int)target.y,
           hasTarget ? "[given on the command line]" : "[= current cursor position]");

    int tmon = WmMonitorFromPoint(target);
    if (tmon >= 0) {
        printf("target monitor : %d  local(%d,%d)\n", tmon,
               (int)target.x - (int)g_wmMon[tmon].rc.left,
               (int)target.y - (int)g_wmMon[tmon].rc.top);
    } else {
        printf("target monitor : NONE -- this point is in a virtual-desktop hole\n");
        printf("WARN: SetCursorPos is expected to silently clamp such a point to the\n");
        printf("      nearest monitor edge. The readback in gate 2 will catch it and\n");
        printf("      this program will REFUSE to click.\n");
    }

    // 第 1 关：报告目标点下面的窗口，此时还没动任何东西
    WmWin before;
    WmQueryWindowAt(target, &before);

    printf("\n=== gate 1/3: window under the target point (before any action) ===\n");
    WmPrintWin("window at the target point:", &before);
    if (!before.hwnd) {
        printf("WARN: no window there. On an empty desktop that is still a legitimate\n");
        printf("      target (the click lands on the desktop window), so we continue.\n");
    }

    if (dry) {
        printf("\n--dry: nothing moved, nothing clicked. exit 0\n");
        return 0;
    }

    // 第 2 关：定位 + 读回验证
    POINT at = target;

    if (hasTarget) {
        int moved = WmMoveVerified(argX, argY, &at);
        if (moved < 0) {
            printf("REFUSING TO CLICK: could not position the cursor at all.\n");
            return 3;
        }
        if (moved == 0) {
            printf("REFUSING TO CLICK: the cursor did not reach the target.\n");
            printf("  wanted : (%d,%d)\n", argX, argY);
            printf("  got    : (%d,%d)   error dx=%+d dy=%+d\n",
                   (int)at.x, (int)at.y, (int)at.x - argX, (int)at.y - argY);
            if (tmon < 0) {
                printf("  the target is in a virtual-desktop hole, so silent clamping is\n");
                printf("  the expected explanation. Pick a point inside a monitor rect.\n");
            } else {
                printf("  something else moved the cursor, or the move was refused.\n");
            }
            printf("  (full explanation of silent clamping: see move.cpp)\n");
            return 1;
        }
        printf("\ngate 2/3: cursor verified at (%d,%d) -- exact hit\n", (int)at.x, (int)at.y);
    } else {
        printf("\ngate 2/3: no coordinates given, so we click where the cursor already is\n");
        printf("          (position readback: (%d,%d))\n", (int)at.x, (int)at.y);
    }

    // 第 3 关：复检光标下的窗口还是不是第 1 关那一个
    WmWin after;
    WmQueryWindowAt(at, &after);

    printf("\n=== gate 3/3: window under the cursor now (still before clicking) ===\n");
    WmPrintWin("window at the cursor:", &after);

    if (!WmSameTarget(&before, &after)) {
        printf("\nREFUSING TO CLICK: the window under the point is not the one we\n");
        printf("reported in gate 1 -- something popped up over it, or it closed.\n");
        WmPrintWin("  gate 1:", &before);
        WmPrintWin("  gate 3:", &after);
        return 1;
    }
    printf("gate 3/3: same window as gate 1 -- safe to click\n");

    // 三关都过，发输入
    printf("\n=== clicking ===\n");
    if (!WmSendClick(button, dbl)) {
        printf("click          : NOT DELIVERED\n");
        return 4;
    }
    printf("SendInput      : ok, %s%s delivered\n", btnName, dbl ? " double-click" : " click");

    // 事后读回
    POINT post = at;
    if (!GetCursorPos(&post)) printf("WARN: GetCursorPos failed after the click\n");

    WmWin changed;
    WmQueryWindowAt(post, &changed);

    printf("\n=== post-action readback ===\n");
    printf("cursor after   : (%d,%d)%s\n", (int)post.x, (int)post.y,
           ((int)post.x != (int)at.x || (int)post.y != (int)at.y)
               ? "   <== MOVED (unexpected; a window may have repositioned it)"
               : "   (unchanged)");
    WmPrintWin("window under the cursor after the click:", &changed);
    printf("state change   : %s\n", WmSameTarget(&after, &changed)
           ? "none observed under the point (the app may still have reacted internally)"
           : "the window under the point is different than before the click");

    printf("\nNOTE: this run only proves the input events were delivered and that the\n");
    printf("      cursor really was on the reported window. Whether the app did what\n");
    printf("      you wanted is only provable with a screenshot (screenshot.exe) or\n");
    printf("      the app's own feedback.\n");
    return 0;
}
