// mousepos.cpp —— 实时打印光标的屏幕物理坐标
//
// 可行性验证目标：
//   1. PerMonitorV2 DPI 感知确实生效（否则坐标会被 125% 除过，永远到不了 1920 右边界）
//   2. GetCursorPos 返回的是【物理像素】，与屏幕真实位置一一对应
//   3. 显示器拓扑被正确识别（含多屏、负坐标、"不在任何显示器上"的情况）
//
// 验证方法：把鼠标推到屏幕最右下角，读数应当接近 (宽-1, 高-1)；
//           推到左上角应当接近 (0,0)。若看到的是 (1535,863) 这种被除过的数，
//           说明 DPI 感知没生效。
//
// 用法：
//   mousepos.exe                  每 500ms 打印一次，Ctrl+C 退出
//   mousepos.exe --count 10       打印 10 次后自动退出
//   mousepos.exe --interval 100   改成 100ms 一次
//
// 构建：见编译说明

#include "winmon.h"

static void PrintUsage(void) {
    printf("usage: mousepos.exe [--interval MS] [--count N]\n");
    printf("  default interval 500 ms, infinite until Ctrl+C\n");
}

int wmain(int argc, wchar_t** argv) {
    WmInitConsole();

    // ★ 第一条：DPI 感知
    const char* dpiMode = WmEnablePerMonitorV2();
    WmLoadMonitors();

    int interval = 500;
    long long count = 0;   // 0 = 无限

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--interval") && i + 1 < argc) interval = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--count")    && i + 1 < argc) count = _wtoi64(argv[++i]);
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { PrintUsage(); return 0; }
        else { printf("unknown arg: "); WmPrintW(argv[i]); printf("\n"); PrintUsage(); return 2; }
    }
    if (interval < 50) interval = 50;

    printf("=== display layout (queried fresh on every run) ===\n");
    printf("dpi awareness  : %s\n", dpiMode);
    WmPrintVirtualScreen();
    WmPrintMonitors();
    printf("\ncursor position, every %d ms  (Ctrl+C to quit)\n", interval);
    printf("   time       |   screen x,  y  | mon |  local x, y  | note\n");
    printf("--------------+-----------------+-----+--------------+-------------------\n");

    long long n = 0;
    for (;;) {
        POINT p;
        if (!GetCursorPos(&p)) { printf("ERROR: GetCursorPos failed\n"); return 3; }

        int idx = WmMonitorFromPoint(p);

        SYSTEMTIME st;
        GetLocalTime(&st);

        char local[64] = "-";
        const char* note = "";
        if (idx >= 0) {
            sprintf(local, "%6ld,%6ld", p.x - g_wmMon[idx].rc.left, p.y - g_wmMon[idx].rc.top);
        } else {
            strcpy(local, "     -,     -");
            note = "NOT ON ANY MONITOR (virtual-desktop hole)";
        }

        printf("%02d:%02d:%02d.%03d | %7ld,%7ld | %3d | %12s | %s\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               p.x, p.y, idx, local, note);
        fflush(stdout);

        ++n;
        if (count > 0 && n >= count) break;
        Sleep((DWORD)interval);
    }
    return 0;
}
