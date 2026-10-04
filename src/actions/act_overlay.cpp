// ===========================================================================
// act_overlay.cpp —— 预备窗口（蓝圈）
//
// 点击这类"有破坏性"的动作执行前，先在光标处亮一个蓝圈 1 秒。
// 这 1 秒是给人的【否决窗口】：只要在这期间把鼠标移开超过 50px，动作就取消。
//
// ---------------------------------------------------------------------------
// 这个窗口的四个必要条件（少一个都会出问题）
// ---------------------------------------------------------------------------
//   点击穿透   WS_EX_TRANSPARENT + WM_NCHITTEST 返回 HTTRANSPARENT
//              —— 否则蓝圈会挡住它自己要触发的那个点击
//   不抢焦点   WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE
//              —— 否则亮圈这个动作本身就把焦点移走了，后续 type 会打错地方
//   始终可见   WS_EX_TOPMOST
//   不进任务栏 WS_EX_TOOLWINDOW
//
// ---------------------------------------------------------------------------
// 画法
// ---------------------------------------------------------------------------
// 用 per-pixel alpha 的分层窗口（UpdateLayeredWindow + ULW_ALPHA），
// 这样蓝圈边缘可以抗锯齿，而不是硬邦邦的像素方块。
//
// ⚠️ ULW_ALPHA 要求位图是【预乘 alpha】的 BGRA：
//    每个颜色分量要乘上 alpha/255。纯蓝 (0,0,255) 在 alpha=a 时是
//    B=a, G=0, R=0, A=a —— 忘了预乘会出现"边缘发白/发黑"的怪现象。
//
// 圈的形状很简单（一个圆环），所以直接逐像素算覆盖率，不需要 GDI+，
// 也就没有"GDI 画不进 alpha 通道"那个经典麻烦。
// ===========================================================================

#include "actions.h"
#include "jsonhelp.h"
#include "../common/winmon.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 可调参数
// ---------------------------------------------------------------------------
#define ARM_MS            1000   // 预备窗口时长。设计稿定了 1 秒，不给调用方调 ——
                                 // 它是安全属性，不是偏好；可配置就等于允许把它关掉。

// 取消阈值。判断用的是【欧氏距离】，所以它的边界就是一个半径 50 的圆。
#define ARM_CANCEL_PX       50

// ★ 环的外边缘【正好等于】取消阈值 ——
//   光标一越过看得见的圈，按定义就已经超过了 50px，动作就该取消。
//   所以这里直接引用 ARM_CANCEL_PX，免得以后改了一个忘了另一个。
#define RING_OUTER    ARM_CANCEL_PX
#define RING_THICKNESS       12                    // 环的粗细
#define RING_INNER   (RING_OUTER - RING_THICKNESS) // 内边缘

// 位图边长：要能装下直径 RING_OUTER*2，再留几像素余量
#define RING_SIZE          108

// 必须是宽字符 —— 下面用的是 RegisterClassExW / CreateWindowExW
static const wchar_t* const kRingClass = L"SimpleCuaArmRing";

// ---------------------------------------------------------------------------
// 窗口与位图（只建一次，之后复用）
// ---------------------------------------------------------------------------
static HWND      g_wnd    = NULL;
static HDC       g_dc     = NULL;
static HBITMAP   g_bmp    = NULL;
static void*     g_bits   = NULL;
static bool      g_ready  = false;
static bool      g_failed = false;   // 建失败过就别反复重试

static LRESULT CALLBACK RingProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // ★ 点击穿透：让鼠标事件穿过去，落到它下面的真正的窗口上
    if (m == WM_NCHITTEST)   return HTTRANSPARENT;
    if (m == WM_ERASEBKGND)  return 1;   // 分层窗口自己管背景，不用擦
    return DefWindowProcW(h, m, w, l);
}

static bool RingInit() {
    if (g_ready)  return true;
    if (g_failed) return false;

    HINSTANCE inst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = RingProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kRingClass;
    RegisterClassExW(&wc);   // 已经注册过会失败，无所谓

    g_wnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kRingClass, L"", WS_POPUP,
        0, 0, RING_SIZE, RING_SIZE,
        NULL, NULL, inst, NULL);
    if (!g_wnd) { g_failed = true; return false; }

    // 32bpp top-down DIB（高度为负 = 从第一行开始，和屏幕坐标一致）
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = RING_SIZE;
    bi.bmiHeader.biHeight      = -RING_SIZE;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    g_dc  = CreateCompatibleDC(screen);
    g_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    ReleaseDC(NULL, screen);

    if (!g_dc || !g_bmp || !g_bits) { g_failed = true; return false; }
    SelectObject(g_dc, g_bmp);

    g_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// 逐像素画一个抗锯齿的蓝环（预乘 alpha）
// ---------------------------------------------------------------------------
static void RingPaint() {
    if (!g_bits) return;

    BYTE* px = (BYTE*)g_bits;
    const double cx = RING_SIZE / 2.0;
    const double cy = RING_SIZE / 2.0;
    const double rIn  = (double)RING_INNER;
    const double rOut = (double)RING_OUTER;   // ★ 外边缘 = 取消阈值（50px）

    for (int y = 0; y < RING_SIZE; ++y) {
        for (int x = 0; x < RING_SIZE; ++x) {
            double dx = x + 0.5 - cx;
            double dy = y + 0.5 - cy;
            double d  = sqrt(dx * dx + dy * dy);

            // 覆盖率：实心带是 [rIn, rOut]，内外边缘各做 1 像素的过渡。
            //
            // ⚠️ 两端的过渡都【朝内】：这样圈绝不会画到 rOut 之外 ——
            //    视觉边界和"超过 50px 就取消"这条规则严格对齐，
            //    不会出现"看着还在圈里、其实已经被判越界"的情况。
            double a;
            if      (d <= rIn)        a = 0.0;
            else if (d < rIn + 1.0)   a = d - rIn;
            else if (d <= rOut - 1.0) a = 1.0;
            else if (d < rOut)        a = rOut - d;
            else                      a = 0.0;
            if (a < 0.0) a = 0.0;
            if (a > 1.0) a = 1.0;

            BYTE alpha = (BYTE)(a * 255.0 + 0.5);

            // 蓝色 R=0 G=0 B=255，预乘：分量都要乘 alpha/255
            BYTE* p = px + (y * RING_SIZE + x) * 4;
            p[0] = alpha;     // B
            p[1] = 0;         // G
            p[2] = 0;         // R
            p[3] = alpha;     // A
        }
    }
}

static void RingShow(POINT center) {
    if (!RingInit()) return;

    RingPaint();

    int left = center.x - RING_SIZE / 2;
    int top  = center.y - RING_SIZE / 2;

    HDC     screen = GetDC(NULL);
    POINT   src    = { 0, 0 };
    SIZE    size   = { RING_SIZE, RING_SIZE };
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;   // 用位图自己的 alpha 通道

    UpdateLayeredWindow(g_wnd, screen, NULL, &size, g_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);

    SetWindowPos(g_wnd, HWND_TOPMOST, left, top, RING_SIZE, RING_SIZE,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void RingHide() {
    if (g_wnd) ShowWindow(g_wnd, SW_HIDE);
}

// ===========================================================================
// 预备窗口
//
// 返回 true  = 通过（可以执行动作）
// 返回 false = 人在预备窗口里把光标移开了，动作取消
//
// movedOutPx 回传实际移动了多少像素（没移动就是 0）。
// ===========================================================================
bool OverlayArmWait(POINT startPt, int* movedOutPx) {
    if (movedOutPx) *movedOutPx = 0;

    if (!RingInit()) {
        // 蓝圈画不出来时的降级：仍然等满 1 秒。
        // 人看不到提示，但至少不会变成"不打招呼立刻点" —— 安全的下限要保住。
        Sleep(ARM_MS);

        // 这一秒里也得盯着光标，否则降级路径就完全没有否决权了
        POINT cur;
        GetCursorPos(&cur);
        int dx = (int)(cur.x - startPt.x);
        int dy = (int)(cur.y - startPt.y);
        int moved = (int)sqrt((double)(dx * dx + dy * dy));
        if (movedOutPx) *movedOutPx = moved;
        return moved <= ARM_CANCEL_PX;
    }

    RingShow(startPt);

    const DWORD t0 = GetTickCount();
    bool  cancelled = false;
    int   moved     = 0;

    while (GetTickCount() - t0 < ARM_MS) {
        // 抽消息：分层窗口虽然不需要重绘，但不抽消息会让窗口变成"未响应"，
        // 而且 ShowWindow / SetWindowPos 的延迟生效也会受影响。
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        POINT cur;
        GetCursorPos(&cur);
        int dx = (int)(cur.x - startPt.x);
        int dy = (int)(cur.y - startPt.y);
        moved = (int)sqrt((double)(dx * dx + dy * dy));

        if (moved > ARM_CANCEL_PX) { cancelled = true; break; }

        Sleep(10);
    }

    RingHide();
    if (movedOutPx) *movedOutPx = moved;
    return !cancelled;
}
