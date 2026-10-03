// ===========================================================================
// winmon.cpp —— 显示器枚举与 DPI 感知
//
// ---------------------------------------------------------------------------
// 为什么要围绕 DPI 感知转
// ---------------------------------------------------------------------------
// Windows 的坐标 API 历史基准是 96 DPI。屏幕设成 125% 时缩放系数 = 120/96 = 1.25。
// 系统会根据进程声明的"DPI 感知级别"决定给你哪一套数字：
//
//   Unaware（默认）  → 所有坐标被 ÷1.25，给你一套"虚拟化"的假坐标
//   SystemAware      → 主屏给真值，其它屏仍被虚拟化
//   PerMonitorV2     → 全给物理真值  ← 我们要的就是这个
//
// 实测对照（同一台机器、同一时刻）：
//   Unaware 进程看到  : 虚拟桌面 origin(0,-166)  size 2400x1536
//   PerMonitorV2 进程 : 虚拟桌面 origin(0,-208)  size 3000x1920
//   3000/1.25 = 2400，1920/1.25 = 1536，-208/1.25 = -166.4  ← 精确吻合
//
// 一旦截图和点击各自拿到不同的一套数，坐标换算必错 —— 这正是 cua 那个 bug。
// 所以本项目只谈物理像素。
// ===========================================================================

#include "winmon.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 全局状态（唯一实例）
// ---------------------------------------------------------------------------
WmMon   g_wmMon[WM_MAX_MONITORS];
int     g_wmMonCount = 0;

// GetDpiForMonitor 在 shcore.dll 里，用 GetProcAddress 动态取，
// 编译期就不需要它的导入库，换 MinGW 版本也不会因为头文件有没有声明而编不过。
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
static PFN_GetDpiForMonitor g_getDpiForMonitor = NULL;

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

typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);

const char* WmEnablePerMonitorV2(void) {
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
// 枚举回调
// ---------------------------------------------------------------------------
static BOOL CALLBACK EnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM) {
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

    if (g_getDpiForMonitor) {
        UINT x = 0, y = 0;
        if (SUCCEEDED(g_getDpiForMonitor(hMon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) {
            m.dpiX = x; m.dpiY = y;
        }
    }

    g_wmMonCount++;
    return TRUE;
}

void WmLoadMonitors(void) {
    if (!g_getDpiForMonitor) {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            g_getDpiForMonitor = (PFN_GetDpiForMonitor)(void*)GetProcAddress(shcore, "GetDpiForMonitor");
        }
    }
    g_wmMonCount = 0;
    EnumDisplayMonitors(NULL, NULL, EnumProc, 0);
}

int WmMonitorFromPoint(POINT pt) {
    for (int i = 0; i < g_wmMonCount; ++i) {
        if (PtInRect(&g_wmMon[i].rc, pt)) return i;
    }
    return -1;
}

int WmMonitorFromLocal(int monIndex, int localX, int localY) {
    if (monIndex < 0 || monIndex >= g_wmMonCount) return -1;
    const WmMon& m = g_wmMon[monIndex];
    if (localX < 0 || localY < 0) return -1;
    if (localX >= (int)(m.rc.right - m.rc.left)) return -1;
    if (localY >= (int)(m.rc.bottom - m.rc.top)) return -1;
    return monIndex;
}

POINT WmLocalToScreen(int monIndex, int localX, int localY) {
    POINT p = { localX, localY };
    if (monIndex >= 0 && monIndex < g_wmMonCount) {
        p.x += (int)g_wmMon[monIndex].rc.left;
        p.y += (int)g_wmMon[monIndex].rc.top;
    }
    return p;
}
