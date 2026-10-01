// ===========================================================================
// winmon.h —— 本目录所有工具共用的基础设施（header-only）
//
// 提供三件事：
//   1) 打开 PerMonitorV2 DPI 感知（【必须最先执行】，早于任何 GDI / 窗口调用）
//   2) 枚举显示器，一律以【物理像素 + 屏幕坐标】返回
//   3) 一个 UTF-8 输出助手，用来打印可能含中文的路径
//
// ---------------------------------------------------------------------------
// 为什么这一切都要围绕 DPI 感知转
// ---------------------------------------------------------------------------
// Windows 的坐标 API 历史基准是 96 DPI。屏幕设成 125% 时，缩放系数 = 120/96
// = 1.25。系统会根据调用进程声明的"DPI 感知级别"决定给你哪一套数字：
//
//   Unaware（默认）  → 所有坐标被 ÷1.25，给你一套"虚拟化"的假坐标
//   SystemAware      → 主屏给真值，其它屏仍被虚拟化
//   PerMonitorV2     → 全给物理真值  ← 我们要的就是这个
//
// 实测对照（同一台机器、同一时刻、同一批 API）：
//   Unaware 进程看到  : 虚拟桌面 origin(0,-166)  size 2400x1536
//   PerMonitorV2 进程 : 虚拟桌面 origin(0,-208)  size 3000x1920
//   3000/1.25 = 2400，1920/1.25 = 1536，-208/1.25 = -166.4  ← 精确吻合
//
// 一旦两个进程各自拿到不同的一套数，坐标换算必错。所以本项目的铁律是：
//   【每个 exe 的第一行都调 WmEnablePerMonitorV2()，然后只谈物理像素。】
//
// ---------------------------------------------------------------------------
// 两个编码上的注意事项
// ---------------------------------------------------------------------------
// * 故意【不】定义 WIN32_LEAN_AND_MEAN —— gdiplus.h 需要完整的 windows.h。
// * 控制台输出一律 ASCII，只有"路径 / 窗口标题"走 WmPrintW 转 UTF-8。
//   这是为了避开 GBK/UTF-8 乱码：cmd 和 PowerShell 5.1 对非 ASCII 的处理
//   非常容易踩雷。
// ===========================================================================

#ifndef WINMON_H
#define WINMON_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---------------------------------------------------------------------------
// 动态取函数，避免依赖 MinGW 头文件的版本
//
// SetProcessDpiAwarenessContext 是 Win10 1607+ 才有的；GetDpiForMonitor 在
// shcore.dll 里。用 GetProcAddress 取，编译期就不需要它们的导入库，
// 换 MinGW 版本也不会因为头文件有没有声明而编不过。
// ---------------------------------------------------------------------------
typedef BOOL    (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);

// 这几个宏在较老的 SDK 头文件里可能没有，自己补一份
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE    ((HANDLE)-3)
#endif
#ifndef DPI_AWARENESS_CONTEXT_SYSTEM_AWARE
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE         ((HANDLE)-2)
#endif

// GetSystemMetrics 里"虚拟屏幕"用的下标。
// 虚拟屏幕 = 所有显示器矩形的【包围盒】，主屏左上角是 (0,0)，
// 主屏左边/上边的显示器就在负坐标区。
#define WM_SM_XVIRTUALSCREEN   76
#define WM_SM_YVIRTUALSCREEN   77
#define WM_SM_CXVIRTUALSCREEN  78
#define WM_SM_CYVIRTUALSCREEN  79

#define WM_MAX_MONITORS 16

// 一台显示器的信息
struct WmMon {
    HMONITOR hmon;
    RECT     rc;        // 物理像素；屏幕坐标。主屏左上角 = (0,0)，左/上方的屏为负
    RECT     work;      // 工作区 = rc 扣掉任务栏
    int      primary;   // 是否主显示器
    UINT     dpiX, dpiY; // 通常 96 / 120 / 144 ...
    WCHAR    device[64]; // 例如 \\.\DISPLAY1
};

// header-only 的全局状态：每个 .cpp 编译单元各有一份自己的副本。
// 对这些"一次运行、做完就退出"的小工具来说完全够用。
static WmMon                  g_wmMon[WM_MAX_MONITORS];
static int                    g_wmMonCount = 0;
static PFN_GetDpiForMonitor   g_wmGetDpiForMonitor = NULL;

// ---------------------------------------------------------------------------
// 控制台
// ---------------------------------------------------------------------------
static void WmInitConsole(void) {
    // 让控制台按 UTF-8 解释我们 printf 出去的字节（路径可能是中文）
    SetConsoleOutputCP(CP_UTF8);
}

// 把宽字符串转成 UTF-8 后打印。只在"路径 / 窗口标题"这类地方用。
static void WmPrintW(const wchar_t* s) {
    char buf[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, (int)sizeof(buf), NULL, NULL);
    if (n > 1) { buf[n - 1] = '\0'; printf("%s", buf); }
    else       { printf("<?>"); }
}

// ---------------------------------------------------------------------------
// ★ 每个程序的第一步都是它
//
// 必须早于任何 GDI / 窗口 / 屏幕相关的调用。做过之后 Windows 才会把
// 物理像素原样给你，而不是那套被 ÷1.25 的假坐标。
//
// 失败时（比如进程已经通过 manifest 声明过感知级别）逐级降级，
// 并返回实际生效的模式字符串，调用方打印出来供人核对。
// ---------------------------------------------------------------------------
static const char* WmEnablePerMonitorV2(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    PFN_SetProcessDpiAwarenessContext fn =
        (PFN_SetProcessDpiAwarenessContext)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (fn) {
        if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return "PerMonitorV2";
        if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))    return "PerMonitorV1";
        if (fn(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))         return "SystemAware";
    }
    SetProcessDPIAware();   // 很老的回退路径
    return "SystemAware(fallback)";
}

// ---------------------------------------------------------------------------
// 显示器枚举
// ---------------------------------------------------------------------------

// EnumDisplayMonitors 的回调：把每台显示器记进 g_wmMon[]
static BOOL CALLBACK WmEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
    if (g_wmMonCount >= WM_MAX_MONITORS) return FALSE;

    WmMon& m = g_wmMon[g_wmMonCount];
    ZeroMemory(&m, sizeof(m));
    m.hmon = hMon;

    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);   // 这个字段必须填，否则 GetMonitorInfoW 会失败
    if (GetMonitorInfoW(hMon, (LPMONITORINFO)&mi)) {
        m.rc      = mi.rcMonitor;
        m.work    = mi.rcWork;
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
        wcsncpy(m.device, mi.szDevice, 63);
        m.device[63] = 0;
    }

    // 每台显示器自己的 DPI（多屏可能是不同的缩放档）
    if (g_wmGetDpiForMonitor) {
        UINT x = 0, y = 0;
        if (SUCCEEDED(g_wmGetDpiForMonitor(hMon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) {
            m.dpiX = x; m.dpiY = y;
        }
    }

    g_wmMonCount++;
    return TRUE;
}

// 每次运行都要重新调用 —— 显示器拓扑会随时变
// （拔屏 / Win+P 切换 / 显示器休眠掉线 / 游戏改显示模式）
static void WmLoadMonitors(void) {
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        g_wmGetDpiForMonitor = (PFN_GetDpiForMonitor)(void*)GetProcAddress(shcore, "GetDpiForMonitor");
    }
    g_wmMonCount = 0;
    EnumDisplayMonitors(NULL, NULL, WmEnumProc, 0);
}

static void WmPrintVirtualScreen(void) {
    int vx = GetSystemMetrics(WM_SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(WM_SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(WM_SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(WM_SM_CYVIRTUALSCREEN);
    printf("virtual screen : origin(%d,%d)  size %dx%d  (physical)\n", vx, vy, vw, vh);
}

// 注意所有 RECT 成员都显式转成 int。
// RECT 的成员是 LONG，而 printf 的可变参数不会做类型提升 —— 直接传 LONG
// 给 %d 会被 gcc 警告（format '%d' expects 'int', but argument has type 'long int'）。
static void WmPrintMonitors(void) {
    printf("monitor count  : %d\n", g_wmMonCount);
    for (int i = 0; i < g_wmMonCount; ++i) {
        const WmMon& m = g_wmMon[i];
        int w = (int)(m.rc.right - m.rc.left);
        int h = (int)(m.rc.bottom - m.rc.top);

        printf("  [%d] ", i);
        WmPrintW(m.device);
        printf("  rect=(%d,%d)-(%d,%d)  %dx%d  work=(%d,%d)-(%d,%d)  primary=%d  dpi=%u (%u%%)\n",
               (int)m.rc.left, (int)m.rc.top, (int)m.rc.right, (int)m.rc.bottom, w, h,
               (int)m.work.left, (int)m.work.top, (int)m.work.right, (int)m.work.bottom,
               m.primary, m.dpiX, m.dpiX ? (m.dpiX * 100 / 96) : 0);
    }
}

// 这个点属于哪台显示器？返回下标；-1 = 不在任何显示器上。
//
// "-1" 是个真实存在的情况：虚拟桌面是【包围盒】，当两块屏高度不一致时，
// 包围盒里会有一块谁都没覆盖的"黑洞"。落在那里的坐标会被 SetCursorPos
// 悄悄钳到最近的显示器边缘（详见 move.cpp 的说明）。
static int WmMonitorFromPoint(POINT pt) {
    for (int i = 0; i < g_wmMonCount; ++i) {
        if (PtInRect(&g_wmMon[i].rc, pt)) return i;
    }
    return -1;
}

static int WmMonitorFromXY(int x, int y) {
    POINT p; p.x = x; p.y = y;
    return WmMonitorFromPoint(p);
}

#endif // WINMON_H
