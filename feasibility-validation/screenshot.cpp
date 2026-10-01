// ===========================================================================
// screenshot.cpp —— 截取【单个显示器】的物理像素，存成 PNG
//
// ---------------------------------------------------------------------------
// 这个程序要验证什么
// ---------------------------------------------------------------------------
//   1. PerMonitorV2 DPI 感知确实生效
//   2. 输出图片的像素尺寸 == 该显示器的【真实物理分辨率】
//      125% 缩放下，如果没做 DPI 感知，这里会变成 1536x864 之类的假值，
//      而且截出来的图还会缺掉右边/下边一截
//   3. 同时打印显示器布局，供鼠标类工具做坐标换算
//
// ---------------------------------------------------------------------------
// 截图是怎么运作的（GDI 路线，也就是这里用的路线）
// ---------------------------------------------------------------------------
//       GetDC(NULL)          拿到"整个屏幕"的设备上下文（DC）
//            ↓
//       BitBlt(...)          把屏幕上一块矩形像素拷进一张内存位图
//            ↓
//       GDI+ Bitmap::Save    编码成 PNG
//
// 关键认知：【位图本身不携带任何坐标信息】。
// 一张 PNG 只是一堆像素，它不告诉你：
//     * 它的 (0,0) 对应物理屏幕的哪个点
//     * 它是哪块区域
//     * 它有没有被缩放
// 这些必须由工具自己约定并写进返回值 —— 这就是下面 "mapping" 那行的作用。
//
// GDI 这条路线是【操作系统原语】，不是包装库：零依赖、二十来行。
// 代价是抓不到硬件加速内容（见文件末尾的"已知限制"）。
//
// ---------------------------------------------------------------------------
// 为什么一次只截一个显示器
// ---------------------------------------------------------------------------
// 也可以截"整个虚拟桌面"（所有显示器的包围盒），但那样有三个坏处：
//   1. 包围盒里可能有【没有显示器的黑洞区】，截出来是一片黑，看着像 bug
//   2. 图太大（两块屏的 PNG 轻松到 4MB）
//   3. 坐标要多绕一层原点偏移
// 一次一屏，坐标就退化成"局部坐标 + 一个固定原点"，简单得多。
//
// ---------------------------------------------------------------------------
// 用法
// ---------------------------------------------------------------------------
//   screenshot.exe                        截光标所在显示器 → shot.png
//   screenshot.exe --list                 只列显示器，不截图
//   screenshot.exe --monitor 1            截 1 号显示器
//   screenshot.exe --out d2.png           指定输出文件
//   screenshot.exe --layered              额外抓分层窗口（CAPTUREBLT，可能闪烁）
//
// 构建：见编译说明
// ===========================================================================

#include "winmon.h"
#include <gdiplus.h>

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 找 PNG 编码器的 CLSID
//
// GDI+ 没有"直接存成 PNG"的入口，要先问它"你有哪些编码器"，
// 再按 MIME 类型在里面找 image/png。
// ---------------------------------------------------------------------------
static int GetEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return -1;

    ImageCodecInfo* info = (ImageCodecInfo*)malloc(size);
    if (!info) return -1;
    if (GetImageEncoders(num, size, info) != Ok) { free(info); return -1; }

    int found = -1;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; found = (int)i; break; }
    }
    free(info);
    return found;
}

static void PrintUsage(void) {
    printf("usage: screenshot.exe [--list] [--monitor N] [--out FILE] [--layered]\n");
    printf("  default: capture the monitor under the cursor, save to shot.png\n");
}

int wmain(int argc, wchar_t** argv) {
    WmInitConsole();

    // ★ 第一条：DPI 感知。必须早于任何 GDI / 窗口调用 ——
    // 晚了的话 GetDC 拿到的坐标空间就已经被虚拟化了。
    const char* dpiMode = WmEnablePerMonitorV2();
    WmLoadMonitors();

    // ---- 解析参数 -----------------------------------------------------------
    const wchar_t* outPath = L"shot.png";
    int wantList = 0, layered = 0, wantMon = -1;

    for (int i = 1; i < argc; ++i) {
        if      (!wcscmp(argv[i], L"--list"))    wantList = 1;
        else if (!wcscmp(argv[i], L"--layered")) layered = 1;
        else if (!wcscmp(argv[i], L"--out")     && i + 1 < argc) outPath = argv[++i];
        else if (!wcscmp(argv[i], L"--monitor") && i + 1 < argc) wantMon = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) { PrintUsage(); return 0; }
        else { printf("unknown arg: "); WmPrintW(argv[i]); printf("\n"); PrintUsage(); return 2; }
    }

    // ---- 先把布局打出来（每次运行都重新查） ----------------------------------
    printf("=== display layout (queried fresh on every run) ===\n");
    printf("dpi awareness  : %s\n", dpiMode);
    WmPrintVirtualScreen();
    WmPrintMonitors();

    POINT cur;
    GetCursorPos(&cur);
    int curMon = WmMonitorFromPoint(cur);
    printf("cursor now     : (%ld,%ld)  on monitor %d\n", cur.x, cur.y, curMon);

    if (wantList) return 0;

    // ---- 挑一台显示器 -------------------------------------------------------
    // 默认截"光标所在的那台"，最符合直觉；光标不在任何屏上则退到 0 号。
    int idx = (wantMon >= 0) ? wantMon : curMon;
    if (idx < 0) idx = 0;
    if (idx >= g_wmMonCount) {
        printf("ERROR: monitor index %d out of range (0..%d)\n", idx, g_wmMonCount - 1);
        return 2;
    }

    const WmMon& m = g_wmMon[idx];
    const int w = (int)(m.rc.right - m.rc.left);
    const int h = (int)(m.rc.bottom - m.rc.top);

    // ---- 抓屏 ---------------------------------------------------------------
    HDC hScreen = GetDC(NULL);                 // 整个屏幕的 DC
    if (!hScreen) { printf("ERROR: GetDC(NULL) failed\n"); return 3; }

    HDC hMem = CreateCompatibleDC(hScreen);    // 一块内存 DC
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, h);
    if (!hMem || !hBmp) { printf("ERROR: CreateCompatibleDC/Bitmap failed\n"); return 3; }

    HGDIOBJ oldObj = SelectObject(hMem, hBmp);

    // SRCCOPY = 直接拷像素。
    // CAPTUREBLT 会额外把【分层窗口】也抓进来（比如置顶的覆盖层），
    // 代价是可能导致光标闪烁，所以做成可选开关。
    DWORD rop = SRCCOPY;
    if (layered) rop |= CAPTUREBLT;

    // 源坐标 (m.rc.left, m.rc.top) 是【物理屏幕坐标】——
    // 只有在 DPI 感知生效时这个理解才成立。
    if (!BitBlt(hMem, 0, 0, w, h, hScreen, (int)m.rc.left, (int)m.rc.top, rop)) {
        printf("WARN: BitBlt failed, GetLastError=%lu (image may be black)\n", GetLastError());
    }

    // ---- 存 PNG（GDI+） ------------------------------------------------------
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, NULL) != Ok) {
        printf("ERROR: GdiplusStartup failed\n");
        return 4;
    }

    CLSID pngClsid;
    int saved = 0;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
        printf("ERROR: PNG encoder not found\n");
    } else {
        // Bitmap 只是"包"了一下 hBmp，并不接管它的生命周期，
        // 所以下面仍然要自己 DeleteObject。
        Bitmap bmp(hBmp, NULL);
        Status st = bmp.Save(outPath, &pngClsid, NULL);
        if (st != Ok) printf("ERROR: Save failed, status=%d\n", (int)st);
        else          saved = 1;
    }
    GdiplusShutdown(token);

    // ---- 收尾 ---------------------------------------------------------------
    SelectObject(hMem, oldObj);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(NULL, hScreen);

    // ---- 报告 ---------------------------------------------------------------
    // 注意：所有 RECT 成员都显式转成 int。
    // RECT 成员是 LONG，printf 的可变参数不做类型提升，直接传给 %d 会触发
    // -Wformat 警告（"expects int, but argument has type long int"）。
    printf("\n=== capture result ===\n");
    printf("monitor index  : %d\n", idx);
    printf("bitmap size    : %dx%d   <== must equal this monitor's REAL physical resolution\n", w, h);
    printf("screen rect    : (%d,%d)-(%d,%d)\n",
           (int)m.rc.left, (int)m.rc.top, (int)m.rc.right, (int)m.rc.bottom);

    // 这一行是给后续工具用的坐标桥：
    // 图上量到 (x,y)，屏幕上就是 (left+x, top+y)。
    printf("mapping        : image(x,y) == screen(%d+x, %d+y)\n", (int)m.rc.left, (int)m.rc.top);

    printf("saved          : %s\n", saved ? "yes" : "NO");
    printf("output path    : "); WmPrintW(outPath); printf("\n");

    HANDLE f = CreateFileW(outPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz; sz.QuadPart = 0;
        GetFileSizeEx(f, &sz);
        printf("file size      : %lld bytes\n", (long long)sz.QuadPart);
        CloseHandle(f);
    }
    return saved ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 已知限制
// ---------------------------------------------------------------------------
// * BitBlt 抓不到硬件加速内容（独占全屏游戏、部分视频、某些 UWP）——
//   那些区域会是纯黑。需要时改用 DXGI Desktop Duplication 或
//   Windows.Graphics.Capture。
// * 截图【不包含鼠标指针】（BitBlt 从来不含光标）。这是正常的，
//   想确认光标位置请用 mousepos.exe 或看 move.exe 的读回值。
// * 独占全屏或游戏用 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
//   屏蔽捕获时，同样只会拿到黑图。这属于"被屏蔽"，不是程序出错。
// ===========================================================================
