// type.cpp — 验证 SendInput + KEYEVENTF_UNICODE：逐字符输入 Unicode 文本。
// 用法: type.exe "要输入的文字" [--interval MS]
//
// KEYEVENTF_UNICODE 要求 wVk = 0、字符放在 wScan，系统把它当成一个 VK_PACKET
// 击键注入：不经过键盘布局和输入法翻译，直接产出 WM_CHAR。

#include "winmon.h"

// 键盘不涉及屏幕坐标，所以不调 WmEnablePerMonitorV2()

// 一个字符 = 按下 + 抬起。非 BMP 字符（如 emoji）在 argv 里本来就是两个
// WCHAR 的代理对，逐码元发就行。
static void TypeChar(WCHAR ch) {
    INPUT in[2];
    ZeroMemory(in, sizeof(in));

    in[0].type       = INPUT_KEYBOARD;
    in[0].ki.wVk     = 0;                 // 用 UNICODE 时必须是 0
    in[0].ki.wScan   = ch;                // 要插入的字符
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;

    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    UINT sent = SendInput(2, in, sizeof(INPUT));
    if (sent != 2) printf("SendInput inserted %u of 2 events, GetLastError=%lu\n", sent, GetLastError());
}

int wmain(int argc, wchar_t** argv) {
    WmInitConsole();

    const wchar_t* text = NULL;
    int interval = 30;

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--interval") && i + 1 < argc) interval = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            printf("usage: type.exe \"text\" [--interval MS]\n");
            return 0;
        }
        else if (argv[i][0] == L'-' && argv[i][1] == L'-') {
            printf("unknown arg: "); WmPrintW(argv[i]); printf("\n");
            return 2;
        }
        else text = argv[i];
    }

    if (!text) { printf("usage: type.exe \"text\" [--interval MS]\n"); return 2; }

    printf("typing : "); WmPrintW(text); printf("\n");
    printf("chars  : %d   interval: %d ms\n", (int)wcslen(text), interval);

    for (const wchar_t* p = text; *p; ++p) {
        TypeChar(*p);
        if (interval > 0) Sleep(interval);
    }

    printf("done\n");
    return 0;
}
