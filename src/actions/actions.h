// ===========================================================================
// actions.h —— server 侧的动作接口
//
// 每个动作把结果写进 ActionResult：
//   status —— client 直接拿它当自己的退出码（0 = 成功）
//   json   —— 一行 JSON，client 原样转发给 agent
//
// 退出码定义在 common/wire.h（ST_OK / ST_USAGE / ST_NO_SERVER / ST_SERVER / ST_REFUSED）
// ===========================================================================

#ifndef ACTIONS_H
#define ACTIONS_H

#include <windows.h>

#define CUA_VERSION "0.1.0"

struct ActionResult {
    int  status;
    char json[16384];
};

// server 启动时调用一次
void ActionsInit(void);

// argv[0] = 动作名，argv[1..] = 参数（UTF-8）
void DispatchAction(int argc, char** argv, ActionResult* res);

// server 已经运行了多少秒
unsigned long long ActionsUptimeSeconds(void);

// ---------------------------------------------------------------------------
// 各动作的处理函数
//
// 按文件分组列出，而不是搞注册表 —— 加一个动作就是加一行，看得见。
// ---------------------------------------------------------------------------

// act_state.cpp
void ActPing(ActionResult* r);
void ActGetState(int argc, char** argv, ActionResult* r);

// act_screen.cpp
void ScreenInit(void);      // 由 ActionsInit 调用
void ActScreenshot(int argc, char** argv, ActionResult* r);
void ActZoom(int argc, char** argv, ActionResult* r);

#endif // ACTIONS_H
