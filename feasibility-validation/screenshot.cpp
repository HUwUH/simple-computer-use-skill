// screenshot.cpp —— 截取【单个显示器】的物理像素
//
// 可行性验证目标：
//   1. PerMonitorV2 DPI 感知确实生效
//   2. 输出图片的像素尺寸 == 该显示器的真实物理分辨率
//      （125% 缩放下如果没做 DPI 感知，这里会变成 1536x864 之类的假值）
//   3. 同时打印显示器布局，供点击/移动程序做坐标换算
//
// 用法：
//   screenshot.exe                        截"光标所在显示器"到 shot.png
//   screenshot.exe --list                 只列显示器，不截图
//   screenshot.exe --monitor 1            截 1 号显示器
//   screenshot.exe --out d2.png           指定输出文件
//   screenshot.exe --layered              额外抓分层窗口（CAPTUREBLT，可能有闪烁）
//
// 构建：见编译说明

#include "winmon.h"
#include <gdiplus.h>

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 找 PNG 编码器的 CLSID
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

    // ★ 第一条：DPI 感知，必须早于任何 GDI / 窗口调用
    const char* dpiMode = WmEnablePerMonitorV2();
    WmLoadMonitors();

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

    printf("=== display layout (queried fresh on every run) ===\n");
    printf("dpi awareness  : %s\n", dpiMode);
    WmPrintVirtualScreen();
    WmPrintMonitors();

    POINT cur;
    GetCursorPos(&cur);
    int curMon = WmMonitorFromPoint(cur);
    printf("cursor now     : (%ld,%ld)  on monitor %d\n", cur.x, cur.y, curMon);

    if (wantList) return 0;

    int idx = (wantMon >= 0) ? wantMon : curMon;
    if (idx < 0) idx = 0;
    if (idx >= g_wmMonCount) { printf("ERROR: monitor index %d out of range (0..%d)\n", idx, g_wmMonCount - 1); return 2; }

    const WmMon& m = g_wmMon[idx];
    const int w = m.rc.right - m.rc.left;
    const int h = m.rc.bottom - m.rc.top;

    // --- 抓屏：屏幕 DC + BitBlt（操作系统原语，零依赖） ---
    HDC hScreen = GetDC(NULL);
    if (!hScreen) { printf("ERROR: GetDC(NULL) failed\n"); return 3; }

    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, h);
    if (!hMem || !hBmp) { printf("ERROR: CreateCompatibleDC/Bitmap failed\n"); return 3; }

    HGDIOBJ oldObj = SelectObject(hMem, hBmp);

    DWORD rop = SRCCOPY;
    if (layered) rop |= CAPTUREBLT;
    if (!BitBlt(hMem, 0, 0, w, h, hScreen, m.rc.left, m.rc.top, rop)) {
        printf("WARN: BitBlt failed, GetLastError=%lu (image may be black)\n", GetLastError());
    }

    // --- 存 PNG（GDI+） ---
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
        Bitmap bmp(hBmp, NULL);
        Status st = bmp.Save(outPath, &pngClsid, NULL);
        if (st != Ok) printf("ERROR: Save failed, status=%d\n", (int)st);
        else          saved = 1;
    }
    GdiplusShutdown(token);

    SelectObject(hMem, oldObj);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(NULL, hScreen);

    // --- 报告 ---
    printf("\n=== capture result ===\n");
    printf("monitor index  : %d\n", idx);
    printf("bitmap size    : %dx%d   <== must equal this monitor's REAL physical resolution\n", w, h);
    printf("screen rect    : (%d,%d)-(%d,%d)\n", m.rc.left, m.rc.top, m.rc.right, m.rc.bottom);
    printf("mapping        : image(x,y) == screen(%d+x, %d+y)\n", m.rc.left, m.rc.top);
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
