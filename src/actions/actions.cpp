// ===========================================================================
// actions.cpp —— 动作表与初始化
// ===========================================================================

#include "actions.h"
#include "jsonhelp.h"
#include "../common/winmon.h"
#include "../common/wire.h"

#include <string.h>

static ULONGLONG g_startTick = 0;

void ActionsInit(void) {
    WmLoadMonitors();   // 每次启动都重新枚举（显示器拓扑随时会变）
    ScreenInit();       // GDI+
    g_startTick = GetTickCount64();
}

unsigned long long ActionsUptimeSeconds(void) {
    return (unsigned long long)((GetTickCount64() - g_startTick) / 1000);
}

// ---------------------------------------------------------------------------
// 动作表
//
// 刻意就是一条 if-else 链，不用注册表也不用 map：加动作 = 加两行，
// 而且一眼能看全有多少个动作。
// ---------------------------------------------------------------------------
void DispatchAction(int argc, char** argv, ActionResult* res) {
    res->status  = ST_OK;
    res->json[0] = '\0';

    if (argc <= 0) {
        FailJ(res, ST_USAGE, "", "no-action", "no action given");
        return;
    }

    const char* op = argv[0];

    if      (!strcmp(op, "ping"))       ActPing(res);
    else if (!strcmp(op, "get_state"))  ActGetState(argc, argv, res);
    else if (!strcmp(op, "screenshot")) ActScreenshot(argc, argv, res);
    else if (!strcmp(op, "zoom"))       ActZoom(argc, argv, res);
    else if (!strcmp(op, "move"))       ActMove(argc, argv, res);
    else if (!strcmp(op, "click"))      ActClick(argc, argv, res);
    else if (!strcmp(op, "drag"))       ActDrag(argc, argv, res);
    else if (!strcmp(op, "scroll"))     ActScroll(argc, argv, res);
    else FailJ(res, ST_USAGE, op, "unknown-action", "unknown action");
}
