// ===========================================================================
// act_kbd.cpp —— type / key
//
// ---------------------------------------------------------------------------
// 两条铁律（都踩过，写在这里免得忘）
// ---------------------------------------------------------------------------
// 1) 打字发给【焦点窗口】，和光标位置【无关】。
//    所以调用方必须自己先把焦点弄对（一般是先 click 一下目标窗口）。
//    这里能做的是把"当前前台窗口是谁"如实回报，好让调用方核对。
//
// 2) 换行走的是【按键通道】—— type 里的换行会被翻译成一个真正的 VK_RETURN。
//
//    为什么换行不能当普通字符发（这是实测出来的，两个都试过）：
//      LF(0x0A)  绝大多数程序直接忽略
//      CR(0x0D)  经典 Win32 EDIT 控件认它，但 Chromium/Electron 系（VS Code、
//                Chrome）会把控制字符整个过滤掉 —— 结果就是 abc 挤在一行
//
//    所以换行只能发 VK_RETURN。⚠️ 代价是【它真的就是按回车】：
//    在单行输入框 / 地址栏里会被当成提交。
//
//    要在多行框里插入一个"纯文本换行"而不触发任何提交，唯一可靠的办法是
//    走剪贴板：clipboard set（内容里带真换行）+ key ctrl+v。
//    粘贴是"插入文本"，不是按键，所以不会被当成提交。
// ===========================================================================

#include "actions.h"
#include "jsonhelp.h"
#include "../common/winutil.h"
#include "../common/wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// type 一次最多输入多少【UTF-8 字节】。
// 打字是逐个字符模拟的，长文本既慢又没意义 —— 更长的内容应该走剪贴板：
// 粘贴是一次原子操作，比逐字符快几个数量级，而且不占请求长度。
#define TYPE_MAX_BYTES 1024

// 一次组合键最多几个键。
// 限制它的理由不只是"用不到那么多"：组合越长，能拼出来的花样越多，
// 黑名单就越难穷尽 —— 收窄输入空间本身就是在帮黑名单可靠工作。
#define MAX_CHORD_KEYS 4

// ===========================================================================
// 键名表
// ===========================================================================
struct KeyName { const char* name; WORD vk; };

static const KeyName kKeys[] = {
    // 编辑与导航
    { "enter",       VK_RETURN   },
    { "esc",         VK_ESCAPE   },
    { "escape",      VK_ESCAPE   },
    { "tab",         VK_TAB      },
    { "space",       VK_SPACE    },
    { "backspace",   VK_BACK     },
    { "delete",      VK_DELETE   },
    { "insert",      VK_INSERT   },
    { "home",        VK_HOME     },
    { "end",         VK_END      },
    { "pageup",      VK_PRIOR    },
    { "pagedown",    VK_NEXT     },
    { "up",          VK_UP       },
    { "down",        VK_DOWN     },
    { "left",        VK_LEFT     },
    { "right",       VK_RIGHT    },
    { "capslock",    VK_CAPITAL  },
    { "printscreen", VK_SNAPSHOT },
    { "pause",       VK_PAUSE    },
    { "numlock",     VK_NUMLOCK  },
    { "scrolllock",  VK_SCROLL   },
    { "apps",        VK_APPS     },

    // 修饰键 —— 故意用【左】变体。
    // SendInput 收到 VK_CONTROL / VK_SHIFT 这种"通用"码时生成的扫描码不够明确，
    // 某些程序会处理得很怪；VK_LCONTROL 这类才是"真的左 Ctrl 键"。
    { "ctrl",        VK_LCONTROL },
    { "control",     VK_LCONTROL },
    { "shift",       VK_LSHIFT   },
    { "alt",         VK_LMENU    },
    { "win",         VK_LWIN     },

    // 小键盘
    { "numpad0", VK_NUMPAD0 }, { "numpad1", VK_NUMPAD1 },
    { "numpad2", VK_NUMPAD2 }, { "numpad3", VK_NUMPAD3 },
    { "numpad4", VK_NUMPAD4 }, { "numpad5", VK_NUMPAD5 },
    { "numpad6", VK_NUMPAD6 }, { "numpad7", VK_NUMPAD7 },
    { "numpad8", VK_NUMPAD8 }, { "numpad9", VK_NUMPAD9 },
    { "multiply", VK_MULTIPLY }, { "add", VK_ADD },
    { "subtract", VK_SUBTRACT }, { "decimal", VK_DECIMAL },
    { "divide",   VK_DIVIDE   },

    // 符号（这些是"OEM 键"，位置随键盘布局变，但物理位置是对的）
    { "-",  VK_OEM_MINUS  }, { "=", VK_OEM_PLUS   }, { "[", VK_OEM_4      },
    { "]",  VK_OEM_6      }, { "\\", VK_OEM_5     }, { ";", VK_OEM_1      },
    { "'",  VK_OEM_7      }, { ",", VK_OEM_COMMA  }, { ".", VK_OEM_PERIOD },
    { "/",  VK_OEM_2      }, { "`", VK_OEM_3      },
};

// 把名字统一成小写（原地改传入的副本）
static void ToLowerInPlace(char* s) {
    for (; *s; ++s) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

// 查键名。name 必须已经是小写。
static bool LookupKey(const char* name, WORD* vk) {
    // 单个字母 / 数字
    if (name[0] && !name[1]) {
        if (name[0] >= 'a' && name[0] <= 'z') { *vk = (WORD)('A' + (name[0] - 'a')); return true; }
        if (name[0] >= '0' && name[0] <= '9') { *vk = (WORD)(name[0]);                return true; }
    }
    // f1..f24（后面必须全是数字，免得 "f" 之类被误认）
    if (name[0] == 'f' && name[1]) {
        bool digitsOnly = true;
        for (const char* p = name + 1; *p; ++p) {
            if (*p < '0' || *p > '9') { digitsOnly = false; break; }
        }
        if (digitsOnly) {
            int n = atoi(name + 1);
            if (n >= 1 && n <= 24) { *vk = (WORD)(VK_F1 + n - 1); return true; }
        }
    }
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
        if (!strcmp(kKeys[i].name, name)) { *vk = kKeys[i].vk; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 把 "ctrl+shift+esc" 拆成若干段，并小写化。
// 返回段数；-1 = 格式错误（空段，或段数超了）。
// ---------------------------------------------------------------------------
static int ParseChord(const char* chord, char (*out)[32], int maxParts) {
    int n = 0;
    const char* p = chord;

    while (*p) {
        if (n >= maxParts) return -1;
        int i = 0;
        while (*p && *p != '+') {
            if (i >= 31) return -1;
            out[n][i++] = *p++;
        }
        out[n][i] = '\0';
        if (i == 0) return -1;          // 空段，比如 "ctrl++v" 或结尾的 "ctrl+"
        ToLowerInPlace(out[n]);
        n++;
        if (*p == '+') p++;
    }
    return n;
}

// ===========================================================================
// 黑名单
//
// 标准是"发出去之后，人不容易把机器恢复原状"，或者干脆不可逆。
//
// ⚠️ 判定用的是【包含】而不是相等：
//    只要按下的键里【包含】某个被禁组合的全部按键，就拦下来。
//    否则 alt+f4+a、win+alt+l 这种"多带一个键"的写法就成了绕过口子 ——
//    而 Windows 判定 Alt+F4 看的是"修饰键当前状态 + 是否按下 F4"，
//    多按一个 a 往往照样会关窗口。
// ===========================================================================
static const char* const kBlockedChords[] = {
    "alt+f4",              // 关掉前台窗口（可能丢未保存的东西）
    "win+l",               // ⚠️ 锁屏 —— 锁上之后 agent 就瞎了，人要重新登录
    "win+ctrl+shift+b",    // 重置显卡驱动，屏幕黑一下，可能打断正在进行的一切
    "shift+delete",        // 永久删除，不进回收站
};

static const char* const kBlockedSingles[] = {
    "scrolllock",          // 开关型；键盘上没有这个键就只能用屏幕键盘关掉
    "pause",
    "apps",                // 等于点右键
};

// 单独的 F1..F24
//
// 笔记本上这些键通常是【双功能】的（媒体 / 亮度 / 飞行模式 / 触控板开关），
// 发出去到底是"F5"还是"飞行模式"，取决于 Fn 锁的状态 —— 效果无法预测。
// 它们的正经用途（刷新、全屏）都有菜单或按钮的等价物。
static bool IsFunctionKeyName(const char* lower) {
    if (lower[0] != 'f' || !lower[1]) return false;
    for (const char* p = lower + 1; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    int n = atoi(lower + 1);
    return n >= 1 && n <= 24;
}

// parts 的键集合是否【包含】blocked 的全部键（顺序无关）
//
// 用"包含"而不是"相等"的理由见上面那段注释。代价是会有少量误伤
// （比如 Chrome 的 ctrl+shift+delete「清除浏览数据」会被连带拦住），
// 但那正是安全的方向。
static bool ContainsAllKeys(char (*parts)[32], int n, char (*blocked)[32], int m) {
    for (int i = 0; i < m; ++i) {
        bool found = false;
        for (int j = 0; j < n; ++j) {
            if (!strcmp(blocked[i], parts[j])) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

// 返回 NULL = 允许发送；否则返回给人看的理由
static const char* BlockedReason(char (*parts)[32], int n) {
    for (size_t i = 0; i < sizeof(kBlockedChords) / sizeof(kBlockedChords[0]); ++i) {
        char blockedParts[MAX_CHORD_KEYS][32];
        int  m = ParseChord(kBlockedChords[i], blockedParts, MAX_CHORD_KEYS);
        if (m <= 0) continue;
        if (ContainsAllKeys(parts, n, blockedParts, m)) {
            return "this key combination is on the blocked list: it can leave the "
                   "machine in a state a human cannot easily undo";
        }
    }

    for (int i = 0; i < n; ++i) {
        for (size_t j = 0; j < sizeof(kBlockedSingles) / sizeof(kBlockedSingles[0]); ++j) {
            if (!strcmp(parts[i], kBlockedSingles[j])) {
                return "this key is on the blocked list";
            }
        }
        if (IsFunctionKeyName(parts[i])) {
            return "F1-F24 are blocked: on laptops these keys are usually dual-purpose "
                   "(media / brightness / airplane mode / touchpad), so the effect "
                   "cannot be predicted without knowing the Fn-lock state";
        }
    }
    return NULL;
}

// ===========================================================================
// 按键的发送
// ===========================================================================
static void SendKeyEvent(WORD vk, bool up) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(INPUT));
}

// 全部按下 -> 保持 hold -> 逆序抬起
//
// ⚠️ 按下和抬起之间【不能有 return】，否则会留下一个"卡住的键"。
//    调用方必须先把所有键名都校验通过再调这里（ActKey 就是这么做的）。
static void SendChord(const WORD* vks, int n, int holdMs) {
    for (int i = 0; i < n; ++i) SendKeyEvent(vks[i], false);
    Sleep((DWORD)holdMs);
    for (int i = n - 1; i >= 0; --i) SendKeyEvent(vks[i], true);
}

// ---------------------------------------------------------------------------
// base64 解码。返回字节数；-1 = 非法输入。
// ---------------------------------------------------------------------------
static int B64Val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int Base64Decode(const char* in, char* out, int cap) {
    int n = 0, acc = 0, bits = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p; ++p) {
        unsigned char c = *p;
        if (c == '=') break;                                  // 补位，后面没内容了
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = B64Val(c);
        if (v < 0) return -1;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

// ===========================================================================
// type —— 逐字符输入一段文本
//
//   --text "..."     可选。适合简单文字（要穿过 shell 的引用规则，容易崩）
//   --b64 <base64>   可选。文本 UTF-8 字节的 base64，纯 ASCII，没有引号风险
//                    与 --text 只能有一个
//   --interval MS    可选。每字符之间的间隔，默认 0
//
// 返回：字符数、来源（text/b64）、前台窗口
//
// ⚠️ 长文本请走剪贴板：请求本身有长度上限（管道那一行 8KB），而 base64 还会
//    再膨胀 4/3。要输入一大段文字，正确做法是 clipboard set + key ctrl+v。
// ===========================================================================
void ActType(int argc, char** argv, ActionResult* r) {
    const char* text       = NULL;
    const char* b64        = NULL;
    int         intervalMs = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--text")     && i + 1 < argc) text       = argv[++i];
        else if (!strcmp(argv[i], "--b64")      && i + 1 < argc) b64        = argv[++i];
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) intervalMs = atoi(argv[++i]);
        else { FailJ(r, ST_USAGE, "type", "bad-arg", argv[i]); return; }
    }

    if (text && b64) {
        FailJ(r, ST_USAGE, "type", "conflicting-args", "give either --text or --b64");
        return;
    }
    if (!text && !b64) {
        FailJ(r, ST_USAGE, "type", "need-text", "give --text or --b64");
        return;
    }
    if (intervalMs < 0 || intervalMs > 1000) {
        FailJ(r, ST_USAGE, "type", "bad-interval", "--interval must be 0..1000 ms");
        return;
    }

    // --- 先拿到 UTF-8 字节 ---
    static char utf8[8000];
    int utf8Len = 0;
    const char* source = text ? "text" : "b64";

    if (text) {
        utf8Len = (int)strlen(text);
        if (utf8Len > (int)sizeof(utf8) - 1) {
            FailJ(r, ST_USAGE, "type", "too-long",
                  "the text is too long; use the clipboard (clipboard set + key ctrl+v)");
            return;
        }
        memcpy(utf8, text, utf8Len + 1);
    } else {
        utf8Len = Base64Decode(b64, utf8, sizeof(utf8) - 1);
        if (utf8Len < 0) {
            FailJ(r, ST_USAGE, "type", "bad-b64", "--b64 is not valid base64");
            return;
        }
        utf8[utf8Len] = '\0';
    }

    // --- 长度上限 ---
    // 打字是逐个字符模拟的，长文本会占用很久；而且更长的内容本来就该走剪贴板。
    if (utf8Len > TYPE_MAX_BYTES) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "the text is %d bytes of UTF-8; the limit is %d bytes. "
                 "For longer text use the clipboard: clipboard set + key ctrl+v",
                 utf8Len, TYPE_MAX_BYTES);
        FailJ(r, ST_USAGE, "type", "too-long", msg);
        return;
    }

    // --- UTF-8 -> UTF-16 ---
    // KEYEVENTF_UNICODE 的 wScan 收的是【UTF-16 码元】，不是 UTF-8。
    // 超出 BMP 的字符（emoji 之类）是两个码元，逐个发也能正确还原。
    static WCHAR wbuf[8192];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, utf8Len, wbuf, 8191);
    if (wlen <= 0) {
        FailJ(r, ST_USAGE, "type", "bad-utf8", "the text is not valid UTF-8");
        return;
    }

    // 换行不在这里处理 —— 它要走按键通道，见下面的发送循环。

    // --- 逐字符发送 ---
    //
    // 普通字符走【字符通道】(KEYEVENTF_UNICODE)，换行走【按键通道】(VK_RETURN)。
    for (int i = 0; i < wlen; ++i) {
        WCHAR c = wbuf[i];

        if (c == L'\r' || c == L'\n') {
            if (c == L'\r' && i + 1 < wlen && wbuf[i + 1] == L'\n') ++i;  // CRLF 只算一个
            SendKeyEvent(VK_RETURN, false);
            SendKeyEvent(VK_RETURN, true);
        } else {
            INPUT in[2];
            ZeroMemory(in, sizeof(in));
            in[0].type       = INPUT_KEYBOARD;
            in[0].ki.wVk     = 0;                 // 用 Unicode 通道时必须为 0
            in[0].ki.wScan   = (WORD)c;
            in[0].ki.dwFlags = KEYEVENTF_UNICODE;
            in[1]            = in[0];
            in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
        }

        if (intervalMs > 0 && i + 1 < wlen) Sleep((DWORD)intervalMs);
    }

    Sleep(40);   // 给焦点窗口一点时间响应，再读前台窗口

    char fc[256] = "", ft[512] = "";
    WinGetForegroundInfo(fc, sizeof(fc), ft, sizeof(ft));

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"type\"");
    CatKV(r->json, sizeof(r->json), "source", source);
    Cat(r->json, sizeof(r->json), ",\"chars\":%d", wlen);
    Cat(r->json, sizeof(r->json), ",\"interval_ms\":%d", intervalMs);
    Cat(r->json, sizeof(r->json), ",\"foreground\":{\"class\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), fc);
    Cat(r->json, sizeof(r->json), ",\"title\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), ft);
    Cat(r->json, sizeof(r->json), "}}");
    r->status = ST_OK;
}

// ===========================================================================
// key --show
// ===========================================================================
static void ActKeyShow(ActionResult* r) {
    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"key\",\"show\":true");
    Cat(r->json, sizeof(r->json), ",\"format\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json),
        "names are lowercase; join with + to form a chord, where the last part is the "
        "main key and the earlier ones are held down as modifiers. "
        "Example: ctrl+shift+esc. At most 4 keys per chord.");
    Cat(r->json, sizeof(r->json), ",\"examples\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json),
        "enter | ctrl+c | ctrl+v | alt+f4 | ctrl+shift+esc | f5 | win+r | pagedown");
    Cat(r->json, sizeof(r->json), ",\"modifiers\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), "ctrl alt shift win");
    Cat(r->json, sizeof(r->json), ",\"letters\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), "a-z");
    Cat(r->json, sizeof(r->json), ",\"digits\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), "0-9");
    Cat(r->json, sizeof(r->json), ",\"function_keys\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), "f1-f24 -- BLOCKED, do not use");

    // 具名键：把表里的名字列出来（去掉别名，也去掉被拉黑的 —— 免得 agent 白试）
    Cat(r->json, sizeof(r->json), ",\"named_keys\":\"");
    {
        int  used  = (int)strlen(r->json);
        bool first = true;
        for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
            // 别名不重复列（escape / control 是别名）
            if (!strcmp(kKeys[i].name, "escape") || !strcmp(kKeys[i].name, "control")) continue;

            bool blocked = false;
            for (size_t j = 0; j < sizeof(kBlockedSingles) / sizeof(kBlockedSingles[0]); ++j) {
                if (!strcmp(kKeys[i].name, kBlockedSingles[j])) { blocked = true; break; }
            }
            if (blocked) continue;

            if (used + (int)strlen(kKeys[i].name) + 2 >= (int)sizeof(r->json)) break;
            if (!first) r->json[used++] = ' ';
            strcpy(r->json + used, kKeys[i].name);
            used += (int)strlen(kKeys[i].name);
            r->json[used] = '\0';
            first = false;
        }
    }
    Cat(r->json, sizeof(r->json), "\"");

    Cat(r->json, sizeof(r->json), ",\"blocked\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json),
        "f1-f24, alt+f4, win+l, win+ctrl+shift+b, shift+delete, "
        "scrolllock, pause, apps");
    Cat(r->json, sizeof(r->json), ",\"not_sendable\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json),
        "ctrl+alt+delete (SAS) cannot be injected by SendInput at all, and the Fn key "
        "itself does not exist in Windows (firmware handles it)");

    Cat(r->json, sizeof(r->json), "}");
    r->status = ST_OK;
}

// ===========================================================================
// key —— 单个按键，或组合键
//
//   CHORD        位置参数，如 enter、ctrl+v。与 --show 互斥
//   --hold MS    可选。按住时长，默认 30，最大 3000
//   --show       可选。不需要 CHORD，列出所有合法键名、组合格式和示例
//
// 返回：解析出的键序列、按住时长、前台窗口
// ===========================================================================
void ActKey(int argc, char** argv, ActionResult* r) {
    const char* chord  = NULL;
    bool        show   = false;
    int         holdMs = 30;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--show"))                     show   = true;
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc)     holdMs = atoi(argv[++i]);
        else if (argv[i][0] != '-' && !chord)                    chord  = argv[i];
        else { FailJ(r, ST_USAGE, "key", "bad-arg", argv[i]); return; }
    }

    if (holdMs < 0 || holdMs > 3000) {
        FailJ(r, ST_USAGE, "key", "bad-hold", "--hold must be 0..3000 ms");
        return;
    }

    if (show) {
        if (chord) {
            FailJ(r, ST_USAGE, "key", "conflicting-args", "--show does not take a CHORD");
            return;
        }
        ActKeyShow(r);
        return;
    }

    if (!chord) {
        FailJ(r, ST_USAGE, "key", "need-chord",
              "give a key or chord (e.g. enter, ctrl+v); "
              "use --show to list every valid name");
        return;
    }

    // --- 解析，并【先全部校验通过】再发 ---
    // 顺序很重要：绝不能发出去一半才发现某个键名不认识，那会留下卡住的修饰键。
    char parts[MAX_CHORD_KEYS][32];
    int  n = ParseChord(chord, parts, MAX_CHORD_KEYS);
    if (n <= 0) {
        FailJ(r, ST_USAGE, "key", "bad-chord",
              "a chord looks like ctrl+shift+esc: at most 4 keys, and no empty parts");
        return;
    }

    WORD vks[MAX_CHORD_KEYS];
    for (int i = 0; i < n; ++i) {
        if (!LookupKey(parts[i], &vks[i])) {
            FailJ(r, ST_USAGE, "key", "unknown-key", parts[i]);
            return;
        }
    }

    // --- 黑名单 ---
    // 和键名校验一样，必须放在【发送之前】—— 绝不能发出去一半才拦下来。
    {
        const char* why = BlockedReason(parts, n);
        if (why) {
            FailJ(r, ST_REFUSED, "key", "blocked-key", why);
            return;
        }
    }

    SendChord(vks, n, holdMs);
    Sleep(40);

    char fc[256] = "", ft[512] = "";
    WinGetForegroundInfo(fc, sizeof(fc), ft, sizeof(ft));

    Cat(r->json, sizeof(r->json), "{\"ok\":true,\"op\":\"key\"");
    CatKV(r->json, sizeof(r->json), "chord", chord);
    Cat(r->json, sizeof(r->json), ",\"keys\":[");
    for (int i = 0; i < n; ++i) {
        if (i) Cat(r->json, sizeof(r->json), ",");
        wire::JsonEscapeAppend(r->json, sizeof(r->json), parts[i]);
    }
    Cat(r->json, sizeof(r->json), "]");
    Cat(r->json, sizeof(r->json), ",\"hold_ms\":%d", holdMs);
    Cat(r->json, sizeof(r->json), ",\"foreground\":{\"class\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), fc);
    Cat(r->json, sizeof(r->json), ",\"title\":");
    wire::JsonEscapeAppend(r->json, sizeof(r->json), ft);
    Cat(r->json, sizeof(r->json), "}}");
    r->status = ST_OK;
}
