// ===========================================================================
// jsonhelp.h —— 往固定缓冲里拼 JSON 的小工具（header-only，各 act_*.cpp 共用）
//
// 只有"生成"，没有"解析" —— 这是整套协议省掉 200 行最容易出 bug 的代码的关键。
// 真正要小心的只有两件事：字符串要转义、别溢出。
// ===========================================================================

#ifndef JSONHELP_H
#define JSONHELP_H

#include "actions.h"
#include "../common/wire.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// 追加一段格式化的 JSON 片段
static inline void Cat(char* buf, int cap, const char* fmt, ...) {
    int n = (int)strlen(buf);
    if (n >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
}

// 追加 ,"key":"value"（UTF-8 字符串，自动 JSON 转义）
static inline void CatKV(char* buf, int cap, const char* key, const char* value) {
    Cat(buf, cap, ",\"%s\":", key);
    wire::JsonEscapeAppend(buf, cap, value ? value : "");
}

// 追加 ,"key":raw —— raw 必须是合法 JSON（数字 / true / false / 数组…）
static inline void CatKR(char* buf, int cap, const char* key, const char* raw) {
    Cat(buf, cap, ",\"%s\":%s", key, raw);
}

// 统一的失败输出。msg 写自然文本即可，这里负责转义。
static inline void FailJ(ActionResult* r, int status, const char* op,
                         const char* code, const char* msg) {
    r->status = status;

    // op 也要转义 —— 它来自 argv，未知动作名里完全可能带引号
    char opEsc[128]   = "";
    char msgEsc[1024] = "";
    wire::JsonEscapeAppend(opEsc,  sizeof(opEsc),  op  ? op  : "");
    wire::JsonEscapeAppend(msgEsc, sizeof(msgEsc), msg ? msg : "");

    snprintf(r->json, sizeof(r->json),
             "{\"ok\":false,\"op\":%s,\"code\":\"%s\",\"msg\":%s}",
             opEsc, code, msgEsc);
}

#endif // JSONHELP_H
