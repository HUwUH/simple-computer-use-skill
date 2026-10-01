// clip.cpp — 验证剪贴板 API：写 / 读 CF_UNICODETEXT，列出当前有哪些格式。
// 用法: clip.exe --set "文字"
//       clip.exe --get
//       clip.exe --list
//
// 固定流程：OpenClipboard -> 操作 -> CloseClipboard。
// 写还要 EmptyClipboard，并把 GlobalAlloc 出来的内存交给系统（所有权转移）。

#include "winmon.h"

// 剪贴板不涉及屏幕坐标，所以不调 WmEnablePerMonitorV2()

// 写：OpenClipboard -> EmptyClipboard -> GlobalAlloc/GlobalLock 拷字符
//     -> SetClipboardData -> CloseClipboard
static int ClipSet(const wchar_t* text) {
    if (!OpenClipboard(NULL)) {
        printf("OpenClipboard failed, GetLastError=%lu\n", GetLastError());
        return 0;
    }

    EmptyClipboard();   // 清掉旧内容并把所有权拿到本进程；不调它 SetClipboardData 会失败

    SIZE_T bytes = (wcslen(text) + 1) * sizeof(WCHAR);   // 必须含结尾的 L'\0'
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) { printf("GlobalAlloc failed\n"); CloseClipboard(); return 0; }

    void* p = GlobalLock(h);
    if (!p) { printf("GlobalLock failed\n"); GlobalFree(h); CloseClipboard(); return 0; }
    memcpy(p, text, bytes);
    GlobalUnlock(h);

    // 成功之后这块内存归剪贴板所有，不能再 GlobalFree；失败才要自己释放
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        printf("SetClipboardData failed, GetLastError=%lu\n", GetLastError());
        GlobalFree(h);
        CloseClipboard();
        return 0;
    }

    CloseClipboard();
    printf("set %d chars\n", (int)wcslen(text));
    return 1;
}

// 读：OpenClipboard -> GetClipboardData -> GlobalLock -> 拷出来用
//     -> GlobalUnlock -> CloseClipboard
static int ClipGet(void) {
    if (!OpenClipboard(NULL)) {
        printf("OpenClipboard failed, GetLastError=%lu\n", GetLastError());
        return 0;
    }

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        printf("clipboard has no CF_UNICODETEXT\n");
        CloseClipboard();
        return 0;
    }

    HANDLE h = GetClipboardData(CF_UNICODETEXT);   // 这块内存归剪贴板所有，绝不能释放
    if (!h) {
        printf("GetClipboardData failed, GetLastError=%lu\n", GetLastError());
        CloseClipboard();
        return 0;
    }

    const wchar_t* p = (const wchar_t*)GlobalLock(h);
    printf("got %d chars: ", p ? (int)wcslen(p) : 0);
    if (p) WmPrintW(p);
    printf("\n");
    GlobalUnlock(h);   // 用完解锁；CloseClipboard 之后这个句柄就无效了

    CloseClipboard();
    return 1;
}

// 列出当前剪贴板上所有格式：EnumClipboardFormats 遍历，
// 预定义格式（CF_TEXT 之类）没有名字，GetClipboardFormatNameW 返回 0。
static void ClipList(void) {
    if (!OpenClipboard(NULL)) {
        printf("OpenClipboard failed, GetLastError=%lu\n", GetLastError());
        return;
    }

    printf("formats:\n");
    UINT fmt = 0;
    while ((fmt = EnumClipboardFormats(fmt)) != 0) {
        WCHAR name[128];
        if (GetClipboardFormatNameW(fmt, name, 128) > 0) {
            printf("  %5u  ", fmt); WmPrintW(name); printf("\n");
        } else {
            printf("  %5u  (predefined)\n", fmt);
        }
    }

    CloseClipboard();
}

int wmain(int argc, wchar_t** argv) {
    WmInitConsole();

    if (argc >= 3 && !wcscmp(argv[1], L"--set"))  return ClipSet(argv[2]) ? 0 : 1;
    if (argc >= 2 && !wcscmp(argv[1], L"--get"))  return ClipGet() ? 0 : 1;
    if (argc >= 2 && !wcscmp(argv[1], L"--list")) { ClipList(); return 0; }

    printf("usage: clip.exe --set \"text\" | --get | --list\n");
    return 2;
}
