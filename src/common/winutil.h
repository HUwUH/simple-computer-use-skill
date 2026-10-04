// ===========================================================================
// winutil.h —— Win32 环境类封装：控制台编码、窗口查询、光标状态、按键状态
// ===========================================================================

#ifndef WINUTIL_H
#define WINUTIL_H

#include <windows.h>

// ---------------------------------------------------------------------------
// 控制台编码
//
// 输出一律 UTF-8。程序启动时调用 WinUseUtf8Console()，退出前 WinRestoreConsole()
// 把控制台代码页恢复回去 —— 因为 SetConsoleOutputCP 改的是那个终端窗口的状态，
// 会一直持续到程序退出之后，不恢复的话用户终端里后续命令会受影响。
// ---------------------------------------------------------------------------
void WinUseUtf8Console(void);
void WinRestoreConsole(void);

// ---------------------------------------------------------------------------
// 字符串编码
// ---------------------------------------------------------------------------

// 宽字符 -> UTF-8。写不下时截断并保证以 NUL 结尾。
void WinToUtf8(const wchar_t* src, char* out, int cap);

// UTF-8 -> 宽字符。返回 false = 失败（含路径过长）。
bool WinToWide(const char* src, wchar_t* out, int outCount);

// ---------------------------------------------------------------------------
// 窗口查询
// ---------------------------------------------------------------------------

// 取窗口的类名和标题（UTF-8）。任一参数可为 NULL。
// 标题可能为空 —— GetWindowTextW 会向目标进程发 WM_GETTEXT，浏览器/游戏常拒绝回答。
void WinGetWindowInfo(HWND hwnd, char* clsOut, int clsCap, char* titleOut, int titleCap);

// 前台窗口（UTF-8）
void WinGetForegroundInfo(char* clsOut, int clsCap, char* titleOut, int titleCap);

// 光标底下的窗口（UTF-8）。pt 是屏幕坐标。
void WinGetWindowUnderCursor(POINT pt, char* clsOut, int clsCap, char* titleOut, int titleCap);

// ---------------------------------------------------------------------------
// 光标状态
// ---------------------------------------------------------------------------

// 把 CURSORINFO.flags 转成可读字符串，例如 "SHOWING" / "SUPPRESSED" / "none"。
// 写进 out（UTF-8），返回 out。
const char* WinCursorFlagsText(char* out, int cap);

// 光标裁剪矩形。返回 false 表示查询失败。
// 正常情况下它等于整个屏幕；比屏幕小说明有程序在把光标限制在自己窗口内
// （全屏游戏、远程桌面、某些模拟器）—— 那时 SetCursorPos 到矩形外会失败
// 或被静默钳制。
bool WinGetClipRect(RECT* out);

// ---------------------------------------------------------------------------
// 按键状态
// ---------------------------------------------------------------------------

// 系统层面的"当前按下了什么"。写进 out（UTF-8 逗号分隔），返回是否至少有一个。
//
// ⚠️ 它区分不出"我注入的"和"用户真人按着的"。所以 release_all 一旦用它，
//    会连带松开用户正按着的键 —— 包括正在拖拽时按住的鼠标左键（拖拽会被
//    中途打断）。这条只应在异常恢复路径上使用。
bool WinGetHeldInput(char* out, int cap);

#endif // WINUTIL_H
