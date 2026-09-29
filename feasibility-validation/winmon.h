// winmon.h —— screenshot.cpp 与 mousepos.cpp 共用的基础设施
//
//   1) 打开 PerMonitorV2 DPI 感知（必须最先做，早于任何 GDI / 窗口调用）
//   2) 枚举显示器（一律物理像素 + 屏幕坐标）
//   3) 一个 UTF-8 输出助手（用于打印可能含中文的路径）
//
// 注意：这里故意【不】定义 WIN32_LEAN_AND_MEAN —— gdiplus.h 需要完整的 windows.h。
// 注意：控制台输出一律 ASCII，只有"路径"走 WmPrintW。这是为了避开 GBK/UTF-8 乱码。

#ifndef WINMON_H
#define WINMON_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---------------------------------------------------------------------------
// 动态取函数，避免依赖 MinGW 头文件的版本
// ---------------------------------------------------------------------------
typedef BOOL    (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE    ((HANDLE)-3)
#endif
#ifndef DPI_AWARENESS_CONTEXT_SYSTEM_AWARE
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE         ((HANDLE)-2)
#endif

// GetSystemMetrics 的虚拟屏幕索引
#define WM_SM_XVIRTUALSCREEN   76
#define WM_SM_YVIRTUALSCREEN   77
#define WM_SM_CXVIRTUALSCREEN  78
#define WM_SM_CYVIRTUALSCREEN  79

#define WM_MAX_MONITORS 16

struct WmMon {
    HMONITOR hmon;
    RECT     rc;        // 物理像素；屏幕坐标（主屏左上角 = 0,0，左/上方的屏为负）
    RECT     work;      // 工作区（扣掉任务栏）
    int      primary;
    UINT     dpiX, dpiY;
    WCHAR    device[64];
};

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

static void WmPrintW(const wchar_t* s) {
    char buf[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, (int)sizeof(buf), NULL, NULL);
    if (n > 1) { buf[n - 1] = '\0'; printf("%s", buf); }
    else       { printf("<?>"); }
}

// ---------------------------------------------------------------------------
// 【第一条必须执行的】打开 PerMonitorV2
//   不做这一步，Windows 会给你一套被 125% 除过的"假坐标"，
//   尺寸会变成 1536x864 之类，截图也会缺一块。
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
    SetProcessDPIAware();
    return "SystemAware(fallback)";
}

// ---------------------------------------------------------------------------
// 显示器枚举
// ---------------------------------------------------------------------------
static BOOL CALLBACK WmEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
    if (g_wmMonCount >= WM_MAX_MONITORS) return FALSE;
    WmMon& m = g_wmMon[g_wmMonCount];
    ZeroMemory(&m, sizeof(m));
    m.hmon = hMon;

    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, (LPMONITORINFO)&mi)) {
        m.rc      = mi.rcMonitor;
        m.work    = mi.rcWork;
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
        wcsncpy(m.device, mi.szDevice, 63);
        m.device[63] = 0;
    }
    if (g_wmGetDpiForMonitor) {
        UINT x = 0, y = 0;
        if (SUCCEEDED(g_wmGetDpiForMonitor(hMon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) {
            m.dpiX = x; m.dpiY = y;
        }
    }
    g_wmMonCount++;
    return TRUE;
}

// 每次运行都要重新调用它 —— 显示器拓扑会随时变（拔屏 / Win+P / 游戏改模式）
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

static void WmPrintMonitors(void) {
    printf("monitor count  : %d\n", g_wmMonCount);
    for (int i = 0; i < g_wmMonCount; ++i) {
        const WmMon& m = g_wmMon[i];
        int w = m.rc.right - m.rc.left;
        int h = m.rc.bottom - m.rc.top;
        printf("  [%d] ", i);
        WmPrintW(m.device);
        printf("  rect=(%d,%d)-(%d,%d)  %dx%d  work=(%d,%d)-(%d,%d)  primary=%d  dpi=%u (%u%%)\n",
               m.rc.left, m.rc.top, m.rc.right, m.rc.bottom, w, h,
               m.work.left, m.work.top, m.work.right, m.work.bottom,
               m.primary, m.dpiX, m.dpiX ? (m.dpiX * 100 / 96) : 0);
    }
}

// 返回包含该点的显示器下标；-1 表示不在任何显示器上（虚拟桌面的"黑洞区"）
static int WmMonitorFromPoint(POINT pt) {
    for (int i = 0; i < g_wmMonCount; ++i) {
        if (PtInRect(&g_wmMon[i].rc, pt)) return i;
    }
    return -1;
}

#endif // WINMON_H
