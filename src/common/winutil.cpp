#include "winutil.h"

#include <stdio.h>
#include <string.h>

// ===========================================================================
// 控制台编码
// ===========================================================================
static UINT g_savedOutputCP = 0;
static bool g_consoleSaved  = false;

void WinUseUtf8Console(void) {
    if (!g_consoleSaved) {
        g_savedOutputCP = GetConsoleOutputCP();
        g_consoleSaved  = true;
    }
    SetConsoleOutputCP(CP_UTF8);
}

void WinRestoreConsole(void) {
    if (g_consoleSaved && g_savedOutputCP) {
        SetConsoleOutputCP(g_savedOutputCP);
    }
}

// ===========================================================================
// 宽字符 -> UTF-8
// ===========================================================================
void WinToUtf8(const wchar_t* src, char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    if (!src) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = '\0';
    else        out[n - 1] = '\0';
}

static void ToUtf8(const wchar_t* src, char* out, int cap) {
    WinToUtf8(src, out, cap);
}

// UTF-8 -> 宽字符
bool WinToWide(const char* src, wchar_t* out, int outCount) {
    if (!src || !out || outCount <= 0) return false;
    out[0] = 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, out, outCount);
    return n > 0;
}

// ===========================================================================
// 窗口查询
// ===========================================================================
void WinGetWindowInfo(HWND hwnd, char* clsOut, int clsCap, char* titleOut, int titleCap) {
    if (clsOut && clsCap > 0)   clsOut[0] = '\0';
    if (titleOut && titleCap > 0) titleOut[0] = '\0';
    if (!hwnd) return;

    if (clsOut && clsCap > 0) {
        WCHAR buf[256] = L"";
        GetClassNameW(hwnd, buf, 255);
        ToUtf8(buf, clsOut, clsCap);
    }
    if (titleOut && titleCap > 0) {
        WCHAR buf[512] = L"";
        // 注意：对别的进程可能返回空 —— 它会向目标窗口发 WM_GETTEXT
        GetWindowTextW(hwnd, buf, 511);
        ToUtf8(buf, titleOut, titleCap);
    }
}

void WinGetForegroundInfo(char* clsOut, int clsCap, char* titleOut, int titleCap) {
    HWND h = GetForegroundWindow();
    WinGetWindowInfo(h, clsOut, clsCap, titleOut, titleCap);
}

void WinGetWindowUnderCursor(POINT pt, char* clsOut, int clsCap, char* titleOut, int titleCap) {
    if (clsOut && clsCap > 0)   clsOut[0] = '\0';
    if (titleOut && titleCap > 0) titleOut[0] = '\0';

    HWND deep = WindowFromPoint(pt);
    if (!deep) return;

    // 深层窗口给类名；顶层窗口给类名 + 标题（标题只有顶层窗口才有意义）
    WinGetWindowInfo(deep, clsOut, clsCap, NULL, 0);

    HWND root = GetAncestor(deep, GA_ROOT);
    if (root && titleOut && titleCap > 0) {
        char tmpCls[256] = "";
        WinGetWindowInfo(root, tmpCls, sizeof(tmpCls), titleOut, titleCap);
    }
}

// ===========================================================================
// 光标状态
// ===========================================================================
const char* WinCursorFlagsText(char* out, int cap) {
    if (!out || cap <= 0) return "";
    out[0] = '\0';

    CURSORINFO ci;
    ZeroMemory(&ci, sizeof(ci));
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) {
        strncpy(out, "unknown", cap - 1);
        return out;
    }

    // 两个标志都没有也是异常信号（正常情况下至少有一个）
    if (ci.flags == 0) { strncpy(out, "none", cap - 1); return out; }
    if (ci.flags & CURSOR_SHOWING)    strncat(out, "SHOWING",    cap - strlen(out) - 1);
    if (ci.flags & CURSOR_SUPPRESSED) {
        if (out[0]) strncat(out, ",", cap - strlen(out) - 1);
        strncat(out, "SUPPRESSED", cap - strlen(out) - 1);
    }
    return out;
}

bool WinGetClipRect(RECT* out) {
    if (!out) return false;
    return GetClipCursor(out) != FALSE;
}

// ===========================================================================
// 按键状态
//
// 遍历虚拟键码，用 GetAsyncKeyState 的高位判断"当前是否按下"。
// 约 250 次调用，开销可忽略。
//
// 注意：用 KEYEVENTF_UNICODE 打出去的字符不产生虚拟键，所以不会出现在这里 ——
//       这没关系，我们只关心按住类按键和鼠标按钮。
// ===========================================================================
struct VkName { UINT vk; const char* name; };

static const VkName kMouseButtons[] = {
    { VK_LBUTTON, "mouse-left"   },
    { VK_RBUTTON, "mouse-right"  },
    { VK_MBUTTON, "mouse-middle" },
};

static const VkName kSpecialKeys[] = {
    { VK_BACK,    "backspace" }, { VK_TAB,     "tab"       },
    { VK_RETURN,  "enter"     }, { VK_ESCAPE,  "esc"       },
    { VK_SPACE,   "space"     }, { VK_PRIOR,   "pageup"    },
    { VK_NEXT,    "pagedown"  }, { VK_END,     "end"       },
    { VK_HOME,    "home"      }, { VK_LEFT,    "left"      },
    { VK_UP,      "up"        }, { VK_RIGHT,   "right"     },
    { VK_DOWN,    "down"      }, { VK_INSERT,  "insert"    },
    { VK_DELETE,  "delete"    }, { VK_LWIN,    "win"       },
    { VK_LSHIFT,  "shift"     }, { VK_LCONTROL,"ctrl"      },
    { VK_LMENU,   "alt"       },
};

static void Append(char* out, int cap, const char* s) {
    if (!out || cap <= 0) return;
    if (out[0]) strncat(out, ",", cap - (int)strlen(out) - 1);
    strncat(out, s, cap - (int)strlen(out) - 1);
}

static void AppendVk(char* out, int cap, UINT vk) {
    for (size_t i = 0; i < sizeof(kSpecialKeys) / sizeof(kSpecialKeys[0]); ++i) {
        if (kSpecialKeys[i].vk == vk) { Append(out, cap, kSpecialKeys[i].name); return; }
    }
    if (vk >= 'A' && vk <= 'Z') { char b[4] = { (char)(vk + 32), 0, 0, 0 }; Append(out, cap, b); return; }
    if (vk >= '0' && vk <= '9') { char b[4] = { (char)vk, 0, 0, 0 }; Append(out, cap, b); return; }
    if (vk >= VK_F1 && vk <= VK_F24) {
        char b[8];
        sprintf(b, "f%u", (unsigned)(vk - VK_F1 + 1));
        Append(out, cap, b);
        return;
    }
    char b[16];
    sprintf(b, "vk-0x%02X", (unsigned)vk);
    Append(out, cap, b);
}

bool WinGetHeldInput(char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = '\0';

    for (size_t i = 0; i < sizeof(kMouseButtons) / sizeof(kMouseButtons[0]); ++i) {
        if (GetAsyncKeyState((int)kMouseButtons[i].vk) & 0x8000) {
            Append(out, cap, kMouseButtons[i].name);
        }
    }
    for (UINT vk = 0x08; vk <= 0xFE; ++vk) {
        // VK_LBUTTON..VK_XBUTTON2 已经单独处理过，跳过
        if (vk >= 0x01 && vk <= 0x06) continue;
        if (GetAsyncKeyState((int)vk) & 0x8000) AppendVk(out, cap, vk);
    }
    return out[0] != '\0';
}
