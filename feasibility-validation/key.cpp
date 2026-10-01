// key.cpp — 验证 SendInput + 虚拟键：按下组合键，可指定按住时长。
// 用法: key.exe [--hold MS] ctrl+s
//       key.exe enter
//       key.exe ctrl shift esc
//
// 修饰键（ctrl/shift/alt/win）总是先按下、最后抬起，其余键按命令行顺序。
// 无论怎么退出（包括运行中途 Ctrl+C），都会把还按着的键补上 KEYUP。

#include "winmon.h"

// 键盘不涉及屏幕坐标，所以不调 WmEnablePerMonitorV2()

struct KeyName { const wchar_t* name; BYTE vk; DWORD flags; };

// 带 KEYEVENTF_EXTENDEDKEY 的是扩展键：方向键、右 Ctrl/Alt、小键盘回车等
static const KeyName g_keyNames[] = {
    { L"ctrl",   VK_CONTROL, 0 }, { L"control", VK_CONTROL, 0 },
    { L"lctrl",  VK_LCONTROL, 0 }, { L"rctrl",  VK_RCONTROL, KEYEVENTF_EXTENDEDKEY },
    { L"shift",  VK_SHIFT,   0 }, { L"lshift",  VK_LSHIFT,  0 },
    { L"rshift", VK_RSHIFT,  0 },
    { L"alt",    VK_MENU,    0 }, { L"menu",    VK_MENU,    0 },
    { L"lalt",   VK_LMENU,   0 }, { L"ralt",    VK_RMENU,   KEYEVENTF_EXTENDEDKEY },
    { L"win",    VK_LWIN,    0 }, { L"rwin",    VK_RWIN,    0 },

    { L"enter",  VK_RETURN,  0 }, { L"return",  VK_RETURN,  0 },
    { L"numpadenter", VK_RETURN, KEYEVENTF_EXTENDEDKEY },
    { L"esc",    VK_ESCAPE,  0 }, { L"escape",  VK_ESCAPE,  0 },
    { L"tab",    VK_TAB,     0 }, { L"space",   VK_SPACE,   0 },
    { L"backspace", VK_BACK, 0 }, { L"bs",      VK_BACK,    0 },

    { L"delete", VK_DELETE, KEYEVENTF_EXTENDEDKEY }, { L"del", VK_DELETE, KEYEVENTF_EXTENDEDKEY },
    { L"insert", VK_INSERT, KEYEVENTF_EXTENDEDKEY }, { L"ins", VK_INSERT, KEYEVENTF_EXTENDEDKEY },
    { L"home",   VK_HOME,   KEYEVENTF_EXTENDEDKEY }, { L"end", VK_END,    KEYEVENTF_EXTENDEDKEY },
    { L"pageup",   VK_PRIOR, KEYEVENTF_EXTENDEDKEY }, { L"pgup", VK_PRIOR, KEYEVENTF_EXTENDEDKEY },
    { L"pagedown", VK_NEXT,  KEYEVENTF_EXTENDEDKEY }, { L"pgdn", VK_NEXT,  KEYEVENTF_EXTENDEDKEY },
    { L"up",     VK_UP,     KEYEVENTF_EXTENDEDKEY }, { L"down",  VK_DOWN,  KEYEVENTF_EXTENDEDKEY },
    { L"left",   VK_LEFT,   KEYEVENTF_EXTENDEDKEY }, { L"right", VK_RIGHT, KEYEVENTF_EXTENDEDKEY },

    { L"capslock",    VK_CAPITAL,  0 },
    { L"printscreen", VK_SNAPSHOT, KEYEVENTF_EXTENDEDKEY },
};

// 名字 -> VK。字母、数字、f1..f24 直接算，不用列进表里。
static int LookupKey(const wchar_t* name, BYTE* vk, DWORD* flags) {
    for (int i = 0; i < (int)(sizeof(g_keyNames) / sizeof(g_keyNames[0])); ++i) {
        if (!_wcsicmp(name, g_keyNames[i].name)) {
            *vk = g_keyNames[i].vk; *flags = g_keyNames[i].flags; return 1;
        }
    }
    if (name[0] && !name[1]) {                              // 单个字符：VK 就是大写的 ASCII
        wchar_t c = name[0];
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) { *vk = (BYTE)c; *flags = 0; return 1; }
        return 0;
    }
    if ((name[0] == L'f' || name[0] == L'F') && name[1] >= L'0' && name[1] <= L'9') {
        int n = _wtoi(name + 1);
        if (n >= 1 && n <= 24) { *vk = (BYTE)(VK_F1 + n - 1); *flags = 0; return 1; }
    }
    return 0;
}

static int IsModifier(BYTE vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
           vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
           vk == VK_LWIN    || vk == VK_RWIN;
}

// 目前已按下、还没抬起的键。它是全局状态 —— 漏一个 KEYUP，整个系统都会
// 认为那个键一直按着，所以退出时一定要清空。
struct DownKey { BYTE vk; DWORD flags; };
static DownKey g_down[16];
static int     g_downCount = 0;

static void SendKey(BYTE vk, DWORD flags, int up) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = vk;
    in.ki.dwFlags = flags | (up ? KEYEVENTF_KEYUP : 0);
    if (SendInput(1, &in, sizeof(INPUT)) != 1)
        printf("SendInput failed, GetLastError=%lu\n", GetLastError());
}

static void KeyDown(BYTE vk, DWORD flags) {
    SendKey(vk, flags, 0);
    if (g_downCount < (int)(sizeof(g_down) / sizeof(g_down[0]))) {
        g_down[g_downCount].vk    = vk;
        g_down[g_downCount].flags = flags;
        g_downCount++;
    }
}

static void KeyUp(BYTE vk, DWORD flags) {
    SendKey(vk, flags, 1);
    for (int i = g_downCount - 1; i >= 0; --i) {
        if (g_down[i].vk == vk && g_down[i].flags == flags) {
            g_down[i] = g_down[--g_downCount];
            break;
        }
    }
}

static void ReleaseAll(void) {
    for (int i = g_downCount - 1; i >= 0; --i) SendKey(g_down[i].vk, g_down[i].flags, 1);
    g_downCount = 0;
}

// Ctrl+C / 关窗口之类的非正常退出也要抬起按键。
// 返回 FALSE = 不接管，让系统继续默认处理（结束进程）。
static BOOL WINAPI CtrlHandler(DWORD type) {
    ReleaseAll();
    return FALSE;
}

static void PrintUsage(void) {
    printf("usage: key.exe [--hold MS] KEY [KEY...]\n");
    printf("  KEY      a-z 0-9 f1-f24, or: ctrl shift alt win enter esc tab space\n");
    printf("           backspace delete insert home end pageup pagedown up down\n");
    printf("           left right capslock printscreen rctrl ralt numpadenter\n");
    printf("  --hold   how long the keys stay pressed, default 30 ms\n");
}

int wmain(int argc, wchar_t** argv) {
    WmInitConsole();
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    struct Plan { const wchar_t* name; BYTE vk; DWORD flags; };
    Plan mods[8]; int nMods = 0;
    Plan keys[8]; int nKeys = 0;
    int hold = 30;

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--hold") && i + 1 < argc) { hold = _wtoi(argv[++i]); continue; }

        BYTE vk = 0; DWORD flags = 0;
        if (!LookupKey(argv[i], &vk, &flags)) {
            printf("unknown key: "); WmPrintW(argv[i]); printf("\n");
            PrintUsage();
            return 2;
        }
        if (IsModifier(vk)) {
            if (nMods < 8) { mods[nMods].name = argv[i]; mods[nMods].vk = vk; mods[nMods].flags = flags; nMods++; }
        } else {
            if (nKeys < 8) { keys[nKeys].name = argv[i]; keys[nKeys].vk = vk; keys[nKeys].flags = flags; nKeys++; }
        }
    }

    if (!nMods && !nKeys) { PrintUsage(); return 2; }

    printf("down : ");
    for (int i = 0; i < nMods; ++i) { KeyDown(mods[i].vk, mods[i].flags); WmPrintW(mods[i].name); printf(" "); }
    for (int i = 0; i < nKeys; ++i) { KeyDown(keys[i].vk, keys[i].flags); WmPrintW(keys[i].name); printf(" "); }
    printf("\nhold : %d ms\n", hold);

    Sleep(hold);   // 按住时长；这段时间里键处于按下状态，被 CtrlHandler 兜住

    printf("up   : ");
    for (int i = nKeys - 1; i >= 0; --i) { KeyUp(keys[i].vk, keys[i].flags); WmPrintW(keys[i].name); printf(" "); }
    for (int i = nMods - 1; i >= 0; --i) { KeyUp(mods[i].vk, mods[i].flags); WmPrintW(mods[i].name); printf(" "); }
    printf("\n");

    ReleaseAll();   // 正常路径下此时已经没按着任何键，这行是保险
    return 0;
}
