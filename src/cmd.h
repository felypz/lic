#ifndef CMD_H
#define CMD_H

#include "net.h"
#include "state.h"
#include <stdbool.h>

typedef struct LuaEngine LuaEngine;

bool cmd_execute(AppState *st, NetConn *conn, LuaEngine *lua, const char *input);

#endif
