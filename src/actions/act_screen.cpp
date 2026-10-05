// ===========================================================================
// act_screen.cpp —— screenshot / zoom
//
// 截图用 GDI 的 BitBlt 抓屏幕，再用 GDI+ 编码成 PNG。
//
// 【坐标】一律是"显示器内的局部坐标"。抓屏前用 WmLocalToScreen 换算成屏幕坐标，
// 因为 BitBlt 的源坐标是屏幕空间的。
//
// 【路径】--out 由 client 解析成绝对路径后注入；server 只接受绝对路径，
// 也不做任何可写性判断 —— 那两件事都必须用 client 的 cwd 和 client 的令牌。
// ===========================================================================

#include "actions.h"
#include "jsonhelp.h"
#include "../common/pathutil.h"
#include "../common/winmon.h"
#include "../common/winutil.h"
#include "../common/wire.h"

#include <gdiplus.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace Gdiplus;

static ULONG_PTR g_gdiplusToken = 0;
static bool      g_gdiplusReady = false;

void ScreenInit(void) {
    GdiplusStartupInput input;
    if (GdiplusStartup(&g_gdiplusToken, &input, NULL) == Ok) {
        g_gdiplusReady = true;
    }
    // 故意不 GdiplusShutdown —— 进程退出时系统会收尾，而 GDI+ 关掉之后
    // 不能重新启动，常驻 server 没有必要冒这个险。
}

// ---------------------------------------------------------------------------
// 抓屏：把屏幕上的 rcSrc 映射到 outW x outH 的位图上
//
// 用 StretchBlt 而不是 BitBlt，是因为 zoom 也要用它放大。
// StretchBltMode = COLORONCOLOR 表示【不做插值，直接删/复制像素】，
// 也就是最近邻 —— 放大后的像素是硬的，核对细节时不会被插值模糊掉。
// ---------------------------------------------------------------------------
static HBITMAP CaptureRect(const RECT& rcSrc, int outW, int outH) {
    int srcW = (int)(rcSrc.right - rcSrc.left);
    int srcH = (int)(rcSrc.bottom - rcSrc.top);
    if (srcW <= 0 || srcH <= 0 || outW <= 0 || outH <= 0) return NULL;

    HDC hScreen = GetDC(NULL);
    if (!hScreen) return NULL;

    HDC hMem = CreateCompatibleDC(hScreen);
    // 注意：必须用【屏幕 DC】创建位图。用内存 DC 会得到 1bpp 的单色图。
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, outW, outH);

    if (!hMem || !hBmp) {
        if (hBmp) DeleteObject(hBmp);
        if (hMem) DeleteDC(hMem);
        ReleaseDC(NULL, hScreen);
        return NULL;
    }

    HGDIOBJ old = SelectObject(hMem, hBmp);
    SetStretchBltMode(hMem, COLORONCOLOR);
    BOOL ok = StretchBlt(hMem, 0, 0, outW, outH,
                         hScreen, (int)rcSrc.left, (int)rcSrc.top, srcW, srcH,
                         SRCCOPY);
    SelectObject(hMem, old);
    DeleteDC(hMem);
    ReleaseDC(NULL, hScreen);

    if (!ok) { DeleteObject(hBmp); return NULL; }
    return hBmp;
}

// ---------------------------------------------------------------------------
// PNG 编码
// ---------------------------------------------------------------------------
static bool FindEncoderClsid(const WCHAR* mime, CLSID* out) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return false;

    ImageCodecInfo* info = (ImageCodecInfo*)malloc(size);
    if (!info) return false;
    GetImageEncoders(num, size, info);

    bool found = false;
    for (UINT i = 0; i < num; ++i) {
        if (!wcscmp(info[i].MimeType, mime)) { *out = info[i].Clsid; found = true; break; }
    }
    free(info);
    return found;
}

static bool SavePng(HBITMAP hBmp, const WCHAR* path) {
    if (!g_gdiplusReady) return false;

    CLSID clsid;
    if (!FindEncoderClsid(L"image/png", &clsid)) return false;

    Bitmap bmp(hBmp, NULL);
    return bmp.Save(path, &clsid, NULL) == Ok;
}

// 取文件字节数（拿不到就返回 0 —— 不影响成功与否）
static unsigned long long FileBytes(const WCHAR* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    return ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

// ---------------------------------------------------------------------------
// 参数检查的公共部分
// ---------------------------------------------------------------------------

// --out 必须是绝对路径：client 一定注入过，这里只防"直接调 server"的调试场景
static bool CheckOutPath(ActionResult* r, const char* op, const char* out) {
    if (!out || !out[0]) {
        FailJ(r, ST_USAGE, op, "no-out",
              "--out is required; the client resolves it to an absolute path before sending");
        return false;
    }
    // "C:\..." 或 "\\server\share\..." 都算绝对
    bool absolute = (out[1] == ':') || (out[0] == '\\' && out[1] == '\\');
    if (!absolute) {
        FailJ(r, ST_USAGE, op, "out-not-absolute",
              "--out must be an absolute path (client-side resolution failed)");
        return false;
    }
    // 白名单：client 已经查过一次，这里再查一次（纵深防御 —— 万一有人绕过 client
    // 自己写了个管道客户端，也不该能毁掉非图片文件）
    if (!PathHasExtension(out, ".png")) {
        FailJ(r, ST_REFUSED, op, "out-not-png", "--out must end with .png");
        return false;
    }
    return true;
}

// 解析 --monitor，默认取光标所在显示器
static int ResolveMonitor(int mon, char* errOut, int errCap) {
    if (mon < 0) {
        POINT cur;
        GetCursorPos(&cur);
        mon = WmMonitorFromPoint(cur);
        if (mon < 0) mon = 0;
    }
    if (mon >= g_wmMonCount) {
        snprintf(errOut, errCap, "monitor %d does not exist (only %d monitor(s))",
                 mon, g_wmMonCount);
        return -1;
    }
    return mon;
}

// ===========================================================================
// screenshot
// ===========================================================================
void ActScreenshot(int argc, char** argv, ActionResult* r) {
    int         mon = -1;
    const char* out = NULL;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--monitor") && i + 1 < argc) mon = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out")     && i + 1 < argc) out = argv[++i];
        else { FailJ(r, ST_USAGE, "screenshot", "bad-arg", argv[i]); return; }
    }

    if (!CheckOutPath(r, "screenshot", out)) return;

    char err[256] = "";
    mon = ResolveMonitor(mon, err, sizeof(err));
    if (mon < 0) { FailJ(r, ST_USAGE, "screenshot", "bad-monitor", err); return; }

    RECT rc = g_wmMon[mon].rc;
    int w = (int)(rc.right - rc.left);
    int h = (int)(rc.bottom - rc.top);

    HBITMAP hBmp = CaptureRect(rc, w, h);
    if (!hBmp) {
        FailJ(r, ST_SERVER, "screenshot", "capture-failed",
              "BitBlt could not copy the screen (is the session locked or headless?)");
        return;
    }

    WCHAR wpath[32768];
    bool havePath = WinToWide(out, wpath, 32768);
    bool saved = havePath && SavePng(hBmp, wpath);
    DeleteObject(hBmp);

    if (!saved) {
        FailJ(r, ST_SERVER, "screenshot", "save-failed",
              "failed to encode or write the PNG file");
        return;
    }

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"screenshot\"");
    Cat(r->json, sizeof(r->json), ",\"monitor\":%d", mon);
    Cat(r->json, sizeof(r->json), ",\"size\":[%d,%d]", w, h);
    Cat(r->json, sizeof(r->json), ",\"bytes\":%llu", FileBytes(wpath));
    Cat(r->json, sizeof(r->json), ",\"path\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), out);
    Cat(r->json, sizeof(r->json), "}");
    r->status = ST_OK;
}

// ===========================================================================
// zoom
//
// 参数：X Y W H（必填，该显示器内的局部坐标）、--scale（默认 2）、
//       --monitor、--out
// ===========================================================================
void ActZoom(int argc, char** argv, ActionResult* r) {
    int         mon   = -1;
    int         scale = 2;
    const char* out   = NULL;
    int         pos[4] = { 0, 0, 0, 0 };
    int         npos   = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--monitor") && i + 1 < argc) mon   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale")   && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out")     && i + 1 < argc) out   = argv[++i];
        else if (argv[i][0] != '-' && npos < 4)                 pos[npos++] = atoi(argv[i]);
        else { FailJ(r, ST_USAGE, "zoom", "bad-arg", argv[i]); return; }
    }

    if (npos != 4) {
        FailJ(r, ST_USAGE, "zoom", "need-region", "X Y W H (four numbers) are required");
        return;
    }
    if (!CheckOutPath(r, "zoom", out)) return;

    char err[256] = "";
    mon = ResolveMonitor(mon, err, sizeof(err));
    if (mon < 0) { FailJ(r, ST_USAGE, "zoom", "bad-monitor", err); return; }

    int x = pos[0], y = pos[1], w = pos[2], h = pos[3];
    if (w <= 0 || h <= 0) {
        FailJ(r, ST_USAGE, "zoom", "bad-region", "W and H must be positive");
        return;
    }
    if (scale < 1 || scale > 16) {
        FailJ(r, ST_USAGE, "zoom", "bad-scale", "--scale must be between 1 and 16");
        return;
    }

    // 区域必须完整落在这一台显示器内 —— 越界的话 StretchBlt 会从别的屏（或
    // 虚拟桌面的黑洞里）抓像素，那种结果没法解释。
    if (WmMonitorFromLocal(mon, x, y) < 0 ||
        WmMonitorFromLocal(mon, x + w - 1, y + h - 1) < 0) {
        FailJ(r, ST_REFUSED, "zoom", "region-not-on-monitor",
              "the region is not fully inside that monitor; "
              "coordinates are monitor-local (see get_state)");
        return;
    }

    POINT tl = WmLocalToScreen(mon, x, y);
    RECT rc;
    rc.left   = tl.x;
    rc.top    = tl.y;
    rc.right  = tl.x + w;
    rc.bottom = tl.y + h;

    int outW = w * scale;
    int outH = h * scale;

    HBITMAP hBmp = CaptureRect(rc, outW, outH);
    if (!hBmp) {
        FailJ(r, ST_SERVER, "zoom", "capture-failed", "BitBlt could not copy that region");
        return;
    }

    WCHAR wpath[32768];
    bool havePath = WinToWide(out, wpath, 32768);
    bool saved = havePath && SavePng(hBmp, wpath);
    DeleteObject(hBmp);

    if (!saved) {
        FailJ(r, ST_SERVER, "zoom", "save-failed", "failed to encode or write the PNG file");
        return;
    }

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"zoom\"");
    Cat(r->json, sizeof(r->json), ",\"monitor\":%d", mon);
    Cat(r->json, sizeof(r->json), ",\"region\":[%d,%d,%d,%d]", x, y, w, h);
    Cat(r->json, sizeof(r->json), ",\"scale\":%d", scale);
    Cat(r->json, sizeof(r->json), ",\"size\":[%d,%d]", outW, outH);
    Cat(r->json, sizeof(r->json), ",\"bytes\":%llu", FileBytes(wpath));
    Cat(r->json, sizeof(r->json), ",\"path\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), out);
    Cat(r->json, sizeof(r->json), "}");
    r->status = ST_OK;
}
