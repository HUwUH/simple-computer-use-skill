#include "pathutil.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define PATH_WMAX (32768)

// UTF-8 -> 宽字符
static bool ToWide(const char* s, WCHAR* out, int outCount) {
    if (!s || !s[0]) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, outCount);
    return n > 0;
}

bool PathResolveAbsolute(const char* path, char* out, int cap) {
    if (!path || !path[0] || !out || cap <= 0) return false;
    out[0] = '\0';

    WCHAR wpath[PATH_WMAX];
    if (!ToWide(path, wpath, PATH_WMAX)) return false;

    WCHAR wfull[PATH_WMAX];
    DWORD n = GetFullPathNameW(wpath, PATH_WMAX, wfull, NULL);
    if (n == 0 || n >= PATH_WMAX) return false;

    int m = WideCharToMultiByte(CP_UTF8, 0, wfull, -1, out, cap, NULL, NULL);
    if (m <= 0) { out[0] = '\0'; return false; }
    return true;
}

bool PathHasExtension(const char* path, const char* ext) {
    if (!path || !ext || ext[0] != '.') return false;

    size_t np = strlen(path);
    size_t ne = strlen(ext);
    if (np < ne) return false;

    const char* tail = path + np - ne;
    for (size_t i = 0; i < ne; ++i) {
        char a = tail[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool PathProbeWritable(const char* path, char* whyOut, int whyCap) {
    if (whyOut && whyCap > 0) whyOut[0] = '\0';

    WCHAR wpath[PATH_WMAX];
    if (!ToWide(path, wpath, PATH_WMAX)) {
        snprintf(whyOut, whyCap, "the path cannot be converted to a Windows path");
        return false;
    }

    DWORD attr = GetFileAttributesW(wpath);

    // --- 目标已存在：试开它本身 ---
    // OPEN_EXISTING 且不给 TRUNCATE_EXISTING，所以只是"打开"，不改内容。
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            snprintf(whyOut, whyCap, "the target is a directory, not a file");
            return false;
        }
        HANDLE h = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            snprintf(whyOut, whyCap,
                     "refused: this process cannot write to the existing file (win32 error %lu)",
                     GetLastError());
            return false;
        }
        CloseHandle(h);
        return true;
    }

    // --- 目标不存在：试开它所在的目录，要求 FILE_ADD_FILE ---
    WCHAR dir[PATH_WMAX];
    wcsncpy(dir, wpath, PATH_WMAX - 1);
    dir[PATH_WMAX - 1] = 0;

    WCHAR* slash = wcsrchr(dir, L'\\');
    if (!slash) {
        snprintf(whyOut, whyCap, "the path is not an absolute Windows path");
        return false;
    }
    if (slash == dir) slash[1] = 0;   // 形如 "C:\"
    else               *slash = 0;

    // 打开目录必须带 FILE_FLAG_BACKUP_SEMANTICS
    HANDLE h = CreateFileW(dir, FILE_ADD_FILE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            snprintf(whyOut, whyCap, "refused: the directory does not exist");
        } else {
            snprintf(whyOut, whyCap,
                     "refused: this process cannot create files in that directory "
                     "(win32 error %lu)", e);
        }
        return false;
    }
    CloseHandle(h);
    return true;
}
