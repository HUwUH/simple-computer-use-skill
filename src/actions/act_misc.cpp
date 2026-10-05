// ===========================================================================
// act_misc.cpp —— clipboard / release_all
//
// ---------------------------------------------------------------------------
// 剪贴板的两条纪律
// ---------------------------------------------------------------------------
// 1) 剪贴板是【共享资源】。别的程序随时可能正开着它，这时 OpenClipboard 会以
//    ERROR_ACCESS_DENIED 失败。所以必须【重试】—— 一次失败不代表剪贴板不可用。
//
// 2) SetClipboardData 成功之后，那块内存的【所有权就交给系统了】，
//    绝不能再 GlobalFree。只有 SetClipboardData 失败时才要自己释放。
//    （这条写反了会变成很难查的崩溃/内存损坏。）
//
// ---------------------------------------------------------------------------
// release_all 为什么用"系统层面的检查"
// ---------------------------------------------------------------------------
// 正常路径下 server 自己知道按了什么，不需要查系统。只有在【出错】的时候
// 账本才可能不可信 —— 那时候只能相信系统的检查。
// 代价是它区分不出"我注入的"和"用户真人按着的"，所以 release_all 会连带
// 松开用户正按着的键（正在拖拽的话，拖拽会被中途打断）。这已经在设计里接受了。
// ===========================================================================

#include "actions.h"
#include "jsonhelp.h"
#include "../common/pathutil.h"
#include "../common/winmon.h"
#include "../common/winutil.h"
#include "../common/wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// clipboard get 内联返回的上限（设计稿：1KB）
#define CLIP_INLINE_MAX 1024

// clipboard set 能接受的文本上限。请求本身有 8KB 的管道上限，这里留点余量。
#define CLIP_SET_MAX    7000

// ===========================================================================
// 剪贴板底层
// ===========================================================================

// 重试着打开剪贴板 —— 别的程序随时可能正占着它
static bool OpenClipRetry() {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(NULL)) return true;
        Sleep(30);
    }
    return false;
}

// 读剪贴板文本。成功时 *out 是 malloc 出来的宽字符串（调用方 free）。
// 失败 = 剪贴板里不是文本 / 打不开。
static bool ReadClipboardText(WCHAR** out, int* outLen) {
    *out = NULL;
    *outLen = 0;

    if (!OpenClipRetry()) return false;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return false; }   // 不是文本

    SIZE_T bytes = GlobalSize(h);
    const WCHAR* p = (const WCHAR*)GlobalLock(h);
    if (!p) { CloseClipboard(); return false; }

    // GlobalSize 给的是分配大小，通常比字符串大 —— 自己量真实长度
    int maxChars = (int)(bytes / sizeof(WCHAR));
    int len = 0;
    while (len < maxChars && p[len]) ++len;

    WCHAR* copy = (WCHAR*)malloc(((size_t)len + 1) * sizeof(WCHAR));
    if (copy) {
        memcpy(copy, p, (size_t)len * sizeof(WCHAR));
        copy[len] = 0;
    }

    GlobalUnlock(h);
    CloseClipboard();

    *out    = copy;
    *outLen = len;
    return copy != NULL;
}

static bool WriteClipboardText(const WCHAR* text, int len) {
    if (!OpenClipRetry()) return false;

    if (!EmptyClipboard()) { CloseClipboard(); return false; }

    SIZE_T bytes = ((SIZE_T)len + 1) * sizeof(WCHAR);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) { CloseClipboard(); return false; }

    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); CloseClipboard(); return false; }
    memcpy(p, text, bytes);
    GlobalUnlock(h);

    // ★ 成功之后所有权归系统，绝不能再 GlobalFree
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);          // 只有失败才自己释放
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

static bool ClearClipboardAll() {
    if (!OpenClipRetry()) return false;
    BOOL ok = EmptyClipboard();
    CloseClipboard();
    return ok != FALSE;
}

// ===========================================================================
// clipboard get
// ===========================================================================
static void ClipGet(ActionResult* r, const char* outPath) {
    if (outPath && outPath[0]) {
        // 纵深防御：client 已经查过一次扩展名
        if (!PathHasExtension(outPath, ".txt")) {
            FailJ(r, ST_REFUSED, "clipboard", "out-not-txt",
                  "--out must end with .txt for clipboard get");
            return;
        }
        bool absolute = (outPath[1] == ':') || (outPath[0] == '\\' && outPath[1] == '\\');
        if (!absolute) {
            FailJ(r, ST_USAGE, "clipboard", "out-not-absolute",
                  "--out must be an absolute path (client-side resolution failed)");
            return;
        }
    }

    WCHAR* text = NULL;
    int    wlen = 0;
    if (!ReadClipboardText(&text, &wlen)) {
        FailJ(r, ST_SERVER, "clipboard", "no-text",
              "the clipboard does not contain text right now (or another program is "
              "holding it open). Only CF_UNICODETEXT is supported.");
        return;
    }

    // UTF-16 -> UTF-8
    int cap = WideCharToMultiByte(CP_UTF8, 0, text, wlen, NULL, 0, NULL, NULL);
    if (cap < 0) cap = 0;
    char* utf8 = (char*)malloc((size_t)cap + 1);
    if (!utf8) {
        free(text);
        FailJ(r, ST_SERVER, "clipboard", "oom", "out of memory");
        return;
    }
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, wlen, utf8, cap, NULL, NULL);
    if (utf8Len < 0) utf8Len = 0;
    utf8[utf8Len] = '\0';
    free(text);

    // --- 落文件（给超过 1KB 的内容用）---
    bool wroteFile = false;
    if (outPath && outPath[0]) {
        WCHAR wpath[32768];
        if (WinToWide(outPath, wpath, 32768)) {
            HANDLE f = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                WriteFile(f, utf8, (DWORD)utf8Len, &wrote, NULL);
                CloseHandle(f);
                wroteFile = true;
            }
        }
        if (!wroteFile) {
            free(utf8);
            FailJ(r, ST_SERVER, "clipboard", "write-failed",
                  "could not write the clipboard text to the requested file");
            return;
        }
    }

    // --- 内联返回，按【字符边界】截断 ---
    // 直接切在第 1024 字节会把一个多字节字符劈成两半，产生非法 UTF-8。
    int  inlineLen = utf8Len;
    bool truncated = false;
    if (inlineLen > CLIP_INLINE_MAX) {
        inlineLen = CLIP_INLINE_MAX;
        // 回退到上一个字符的开头（UTF-8 续字节是 10xxxxxx）
        while (inlineLen > 0 && ((unsigned char)utf8[inlineLen] & 0xC0) == 0x80) --inlineLen;
        truncated = true;
    }

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"clipboard\",\"action\":\"get\"");
    Cat(r->json, sizeof(r->json), ",\"chars\":%d", wlen);
    Cat(r->json, sizeof(r->json), ",\"bytes\":%d", utf8Len);
    CatKR(r->json, sizeof(r->json), "truncated", truncated ? "true" : "false");
    if (wroteFile) {
        Cat(r->json, sizeof(r->json), ",\"path\":");
        wire::JsonEscapeAppend(r->json, sizeof(r->json), outPath);
    }
    Cat(r->json, sizeof(r->json), ",\"text\":");
    {
        char save = utf8[inlineLen];
        utf8[inlineLen] = '\0';
        wire::JsonEscapeAppend(r->json, sizeof(r->json), utf8);
        utf8[inlineLen] = save;
    }
    Cat(r->json, sizeof(r->json), "}");

    free(utf8);
    r->status = ST_OK;
}

// ===========================================================================
// clipboard set
// ===========================================================================
static void ClipSet(ActionResult* r, const char* text, const char* b64) {
    if (text && b64) {
        FailJ(r, ST_USAGE, "clipboard", "conflicting-args", "give either --text or --b64");
        return;
    }
    if (!text && !b64) {
        FailJ(r, ST_USAGE, "clipboard", "need-text", "give --text or --b64");
        return;
    }

    static char utf8[CLIP_SET_MAX + 1];
    int utf8Len = 0;

    if (text) {
        utf8Len = (int)strlen(text);
        if (utf8Len > CLIP_SET_MAX) {
            FailJ(r, ST_USAGE, "clipboard", "too-long", "the text is too long for a request");
            return;
        }
        memcpy(utf8, text, utf8Len + 1);
    } else {
        utf8Len = wire::Base64Decode(b64, utf8, CLIP_SET_MAX);
        if (utf8Len < 0) {
            FailJ(r, ST_USAGE, "clipboard", "bad-b64", "--b64 is not valid base64");
            return;
        }
        utf8[utf8Len] = '\0';
    }

    static WCHAR wbuf[8192];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, utf8Len, wbuf, 8191);
    if (wlen < 0) wlen = 0;

    if (!WriteClipboardText(wbuf, wlen)) {
        FailJ(r, ST_SERVER, "clipboard", "set-failed",
              "could not write to the clipboard (another program may be holding it open)");
        return;
    }

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"clipboard\",\"action\":\"set\"");
    Cat(r->json, sizeof(r->json), ",\"chars\":%d", wlen);
    Cat(r->json, sizeof(r->json), "}");
    r->status = ST_OK;
}

// ===========================================================================
// clipboard 分发
// ===========================================================================
void ActClipboard(int argc, char** argv, ActionResult* r) {
    if (argc < 2) {
        FailJ(r, ST_USAGE, "clipboard", "need-subcommand",
              "usage: clipboard get [--out FILE] | clipboard set --text/--b64 ... | "
              "clipboard clear");
        return;
    }

    const char* sub = argv[1];

    if (!strcmp(sub, "get")) {
        const char* out = NULL;
        for (int i = 2; i < argc; ++i) {
            if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
            else { FailJ(r, ST_USAGE, "clipboard", "bad-arg", argv[i]); return; }
        }
        ClipGet(r, out);
        return;
    }

    if (!strcmp(sub, "set")) {
        const char* text = NULL;
        const char* b64  = NULL;
        for (int i = 2; i < argc; ++i) {
            if      (!strcmp(argv[i], "--text") && i + 1 < argc) text = argv[++i];
            else if (!strcmp(argv[i], "--b64")  && i + 1 < argc) b64  = argv[++i];
            else { FailJ(r, ST_USAGE, "clipboard", "bad-arg", argv[i]); return; }
        }
        ClipSet(r, text, b64);
        return;
    }

    if (!strcmp(sub, "clear")) {
        if (argc > 2) {
            FailJ(r, ST_USAGE, "clipboard", "bad-arg", argv[2]);
            return;
        }
        if (!ClearClipboardAll()) {
            FailJ(r, ST_SERVER, "clipboard", "clear-failed",
                  "could not open the clipboard (another program may be holding it open)");
            return;
        }
        Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"clipboard\",\"action\":\"clear\"}");
        r->status = ST_OK;
        return;
    }

    FailJ(r, ST_USAGE, "clipboard", "bad-subcommand",
          "subcommand must be get, set or clear");
}

// ===========================================================================
// release_all
//
// ⚠️ 会打断用户当前的操作（包括正在进行的拖拽）。只在异常恢复时用。
// ===========================================================================
void ActReleaseAll(int argc, char** argv, ActionResult* r) {
    (void)argc;
    (void)argv;

    // 查一遍当前按着什么
    WORD keys[128];
    int  nKey = WinGetHeldKeys(keys, 128);
    bool mouseL = false, mouseR = false, mouseM = false;
    WinGetHeldMouse(&mouseL, &mouseR, &mouseM);

    // --- 先松鼠标按钮 ---
    // 如果用户正按着左键拖拽，这一步会立刻终止拖拽（这正是"救火"想要的）
    auto sendMouseUp = [](DWORD flag) {
        INPUT in;
        ZeroMemory(&in, sizeof(in));
        in.type       = INPUT_MOUSE;
        in.mi.dwFlags = flag;
        SendInput(1, &in, sizeof(INPUT));
    };

    char btnStr[64] = "";
    if (mouseL) { sendMouseUp(MOUSEEVENTF_LEFTUP);                              strncat(btnStr, "left",   sizeof(btnStr) - strlen(btnStr) - 1); }
    if (mouseR) { sendMouseUp(MOUSEEVENTF_RIGHTUP);  if (btnStr[0]) strncat(btnStr, ",", sizeof(btnStr) - strlen(btnStr) - 1); strncat(btnStr, "right",  sizeof(btnStr) - strlen(btnStr) - 1); }
    if (mouseM) { sendMouseUp(MOUSEEVENTF_MIDDLEUP); if (btnStr[0]) strncat(btnStr, ",", sizeof(btnStr) - strlen(btnStr) - 1); strncat(btnStr, "middle", sizeof(btnStr) - strlen(btnStr) - 1); }

    // --- 再松键盘 ---
    char keyStr[512] = "";
    for (int i = 0; i < nKey; ++i) {
        INPUT in;
        ZeroMemory(&in, sizeof(in));
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = keys[i];
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));

        char nm[32];
        WinVkName(keys[i], nm, sizeof(nm));
        if (keyStr[0]) strncat(keyStr, ",", sizeof(keyStr) - strlen(keyStr) - 1);
        strncat(keyStr, nm, sizeof(keyStr) - strlen(keyStr) - 1);
    }

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"release_all\"");
    CatKV(r->json, sizeof(r->json), "released_buttons", btnStr);
    CatKV(r->json, sizeof(r->json), "released_keys", keyStr);
    Cat(r->json, sizeof(r->json), "}");
    r->status = ST_OK;
}
