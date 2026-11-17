#ifndef LUA_API_H
#define LUA_API_H

#include "irc.h"
#include "net.h"
#include "state.h"
#include <lua.h>
#include <stdbool.h>

typedef struct {
	char host[128];
	char port[16];
	bool tls;
	char nick[STATE_NICK_LEN];
	char user[STATE_NICK_LEN];
	char realname[128];
	char sasl_user[STATE_NICK_LEN];
	char sasl_pass[128];
	size_t history_size;
	char auto_join[16][STATE_NAME_LEN];
	size_t auto_join_count;
} LicConfig;

struct LuaEngine {
	lua_State *L;
	AppState *state;
	NetConn *conn;
	LicConfig config;
	char script_path[256];
};

bool lua_engine_init(struct LuaEngine *eng, AppState *st, NetConn *conn, const char *script_path);
void lua_engine_close(struct LuaEngine *eng);
bool lua_reload(struct LuaEngine *eng);
void lua_exec_string(struct LuaEngine *eng, const char *code);

bool lua_emit_message(struct LuaEngine *eng, const IrcMessage *msg);
bool lua_emit_input(struct LuaEngine *eng, const char *input);
bool lua_emit_command(struct LuaEngine *eng, const char *cmd, const char *arg);
size_t lua_list_commands(struct LuaEngine *eng, const char **out, size_t max);

#endif
