// ===========================================================================
// mousepos.cpp —— 实时打印光标的屏幕物理坐标
//
// ---------------------------------------------------------------------------
// 这个程序要验证什么
// ---------------------------------------------------------------------------
//   1. PerMonitorV2 DPI 感知确实生效。
//      如果没生效，GetCursorPos 返回的是被 ÷1.25 的"虚拟化"坐标，
//      鼠标推到屏幕最右下角也只能读到 (1535, 863)，永远够不到 1919。
//   2. GetCursorPos 返回的确实是【物理像素】，和屏幕上的真实位置一一对应。
//   3. 显示器拓扑被正确识别（含多屏、负坐标、"不在任何显示器上"的情况）。
//
// 怎么用：
//   把鼠标推到屏幕【最右下角】→ 读数应当接近 (宽-1, 高-1)
//   推到【左上角】            → 应当接近 (0, 0)
//   插上/拔掉一块屏再跑一次   → 布局应当立刻跟着变（本程序每次启动都重新查）
//
// 这个程序同时也是 move.exe 的验证器：一边跑它，一边在另一个窗口跑 move，
// 就能肉眼确认光标是不是真的到了指定位置。
//
// ---------------------------------------------------------------------------
// 关于屏幕坐标
// ---------------------------------------------------------------------------
// 主显示器的左上角定义为 (0, 0)。主屏【左边】或【上边】的显示器就在负坐标区。
// 输出里"local"那一列是光标相对于所在显示器左上角的偏移 ——
// 只截一块屏时，用局部坐标描述位置会更好读。
//
// ---------------------------------------------------------------------------
// 用法
// ---------------------------------------------------------------------------
//   mousepos.exe                  每 500ms 打印一次，Ctrl+C 退出
//   mousepos.exe --count 10       打印 10 次后自动退出
//   mousepos.exe --interval 100   改成 100ms 一次（最小 50）
//
// 构建：见编译说明
// ===========================================================================

#include "winmon.h"

static void PrintUsage(void) {
    printf("usage: mousepos.exe [--interval MS] [--count N]\n");
    printf("  default interval 500 ms, infinite until Ctrl+C\n");
}

int wmain(int argc, wchar_t** argv) {
    WmInitConsole();

    // ★ 第一条：DPI 感知。少了它，下面的读数全都是被除过的假坐标。
    const char* dpiMode = WmEnablePerMonitorV2();
    WmLoadMonitors();   // 每次运行都重新查 —— 拓扑会变

    // ---- 解析参数 -----------------------------------------------------------
    int interval = 500;
    long long count = 0;      // 0 = 无限循环

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--interval") && i + 1 < argc) interval = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--count")    && i + 1 < argc) count = _wtoi64(argv[++i]);
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { PrintUsage(); return 0; }
        else { printf("unknown arg: "); WmPrintW(argv[i]); printf("\n"); PrintUsage(); return 2; }
    }
    if (interval < 50) interval = 50;   // 别刷爆控制台

    // ---- 先自报家门 ---------------------------------------------------------
    printf("=== display layout (queried fresh on every run) ===\n");
    printf("dpi awareness  : %s\n", dpiMode);
    WmPrintVirtualScreen();
    WmPrintMonitors();

    printf("\ncursor position, every %d ms  (Ctrl+C to quit)\n", interval);
    printf("   time       |   screen x,  y  | mon |  local x, y  | note\n");
    printf("--------------+-----------------+-----+--------------+-------------------\n");

    // ---- 主循环 -------------------------------------------------------------
    long long n = 0;
    for (;;) {
        POINT p;
        if (!GetCursorPos(&p)) { printf("ERROR: GetCursorPos failed\n"); return 3; }

        // 这个点属于哪台显示器？-1 表示落在虚拟桌面的"黑洞区"里
        // （包围盒里有、但没有任何显示器覆盖的地方）。
        int idx = WmMonitorFromPoint(p);

        SYSTEMTIME st;
        GetLocalTime(&st);

        char local[64];
        const char* note = "";
        if (idx >= 0) {
            // 换算成"相对该显示器左上角"的局部坐标
            sprintf(local, "%6d,%6d",
                    (int)(p.x - g_wmMon[idx].rc.left),
                    (int)(p.y - g_wmMon[idx].rc.top));
        } else {
            strcpy(local, "     -,     -");
            note = "NOT ON ANY MONITOR (virtual-desktop hole)";
        }

        // p.x / p.y 是 LONG，所以用 %ld
        printf("%02d:%02d:%02d.%03d | %7ld,%7ld | %3d | %12s | %s\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               p.x, p.y, idx, local, note);
        fflush(stdout);   // 不加这句，重定向到文件时看不到实时输出

        ++n;
        if (count > 0 && n >= count) break;
        Sleep((DWORD)interval);
    }
    return 0;
}
