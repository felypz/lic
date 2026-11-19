#ifndef CMD_H
#define CMD_H

#include "lua_api.h"
#include "net.h"
#include "state.h"
#include "ui.h"
#include <stdbool.h>

bool cmd_execute(AppState *st, NetConn *conn, struct LuaEngine *lua, const char *input);
bool cmd_execute_vim(AppState *st, NetConn *conn, UiState *ui, struct LuaEngine *lua, const char *ex_cmd);

#endif
