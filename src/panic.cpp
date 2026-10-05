// ===========================================================================
// panic.cpp —— 救火程序（cua_panic.exe）
//
// 用途：server 卡死了、或者有键卡住了，人跑一下这个程序，一次性收拾干净。
//
// ---------------------------------------------------------------------------
// 三个动作，顺序是有讲究的
// ---------------------------------------------------------------------------
//   [1] 先松开当前按着的一切
//       —— 这件事【不能委托给 server】：server 可能就是卡住的那个
//   [2] 再把 cua_server.exe 杀掉
//       —— 杀完之后它就没法再注入任何东西了
//   [3] 再松一次
//       —— 万一它在被杀之前又按下去了什么
//
// ---------------------------------------------------------------------------
// 必须由【人】用普通权限运行
// ---------------------------------------------------------------------------
// 要杀一个 Medium 完整性的进程，动手的进程也得是 Medium。
// agent 在 DSH 沙箱里是 Low，跑这个会因为强制完整性检查失败（杀不动 + 注入不了）。
//
// 它不依赖 server、不依赖管道、不依赖任何配置文件 —— 越简单越可靠。
// ===========================================================================

#include "common/winutil.h"

#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PANIC_VERSION "0.1.0"

// ---------------------------------------------------------------------------
static void SendMouseUp(DWORD flag) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    SendInput(1, &in, sizeof(INPUT));
}

static void SendKeyUp(WORD vk) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = vk;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// 松开当前按着的一切。用的是【系统层面的检查】—— 出错的时候只有它可信。
// 返回松开了几项。
// ---------------------------------------------------------------------------
static int ReleaseEverything() {
    int count = 0;

    bool l = false, r = false, m = false;
    WinGetHeldMouse(&l, &r, &m);

    // 鼠标按钮先松：如果用户正按着左键拖拽，这一步会立刻终止拖拽
    if (l) { SendMouseUp(MOUSEEVENTF_LEFTUP);   printf("      mouse-left\n");   count++; }
    if (r) { SendMouseUp(MOUSEEVENTF_RIGHTUP);  printf("      mouse-right\n");  count++; }
    if (m) { SendMouseUp(MOUSEEVENTF_MIDDLEUP); printf("      mouse-middle\n"); count++; }

    WORD keys[128];
    int  n = WinGetHeldKeys(keys, 128);
    for (int i = 0; i < n; ++i) {
        SendKeyUp(keys[i]);
        char nm[32];
        WinVkName(keys[i], nm, sizeof(nm));
        printf("      %s\n", nm);
        count++;
    }

    return count;
}

// ---------------------------------------------------------------------------
// 按名字杀掉进程。按【名字】而不是完整路径 —— 因为 server 可能是从别处启动的，
// 而 cua_server.exe 这个名字足够特别。（万一真有同名的别的程序，这里也会一并
// 杀掉 —— 所以下面会把 pid 打出来，让人看得见到底动了谁。）
// ---------------------------------------------------------------------------
static int KillServers(const wchar_t* exeName) {
    int killed = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        printf("      CreateToolhelp32Snapshot failed, err=%lu\n",
               (unsigned long)GetLastError());
        return 0;
    }

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) != 0) continue;
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;

            HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (!p) {
                printf("      pid %lu: cannot open (err=%lu) -- skipped\n",
                       (unsigned long)pe.th32ProcessID, (unsigned long)GetLastError());
                continue;
            }

            if (TerminateProcess(p, 1)) {
                WaitForSingleObject(p, 3000);
                printf("      pid %lu: terminated\n", (unsigned long)pe.th32ProcessID);
                killed++;
            } else {
                printf("      pid %lu: TerminateProcess failed, err=%lu\n",
                       (unsigned long)pe.th32ProcessID, (unsigned long)GetLastError());
            }
            CloseHandle(p);
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return killed;
}

// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    (void)argc;
    (void)argv;

    WinUseUtf8Console();

    printf("cua_panic %s  --  emergency stop\n\n", PANIC_VERSION);

    // [1] 先松手
    printf("[1] releasing everything currently held down...\n");
    int released = ReleaseEverything();
    printf("    released %d item(s)\n\n", released);

    // [2] 再杀 server
    printf("[2] terminating cua_server.exe...\n");
    int killed = KillServers(L"cua_server.exe");
    if (killed == 0) {
        printf("      nothing matched. Either the server was not running, or it runs\n");
        printf("      as another user (then you have to stop it yourself).\n");
    }
    printf("\n");

    // [3] 再松一次
    printf("[3] releasing again, in case the server injected something while dying...\n");
    released = ReleaseEverything();
    printf("    released %d item(s)\n\n", released);

    printf("done.\n");
    WinRestoreConsole();
    return 0;
}
