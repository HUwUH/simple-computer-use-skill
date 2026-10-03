// ===========================================================================
// actions.h —— server 侧的动作分发
//
// 每个动作把结果写进 ActionResult：
//   status —— client 直接拿它当自己的退出码（0 = 成功）
//   json   —— 一行 JSON，client 原样转发给 agent
// ===========================================================================

#ifndef ACTIONS_H
#define ACTIONS_H

#define CUA_VERSION "0.1.0"

// 退出码约定见 common/wire.h（ST_OK / ST_USAGE / ST_NO_SERVER / ST_SERVER / ST_REFUSED）

struct ActionResult {
    int  status;
    char json[16384];
};

// server 启动时调用一次
void ActionsInit(void);

// argv[0] = 动作名，argv[1..] = 参数（UTF-8）
void DispatchAction(int argc, char** argv, ActionResult* res);

#endif // ACTIONS_H
