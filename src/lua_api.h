#ifndef LUA_API_H
#define LUA_API_H

#include "irc.h"
#include "net.h"
#include "state.h"
#include <lua.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	COLOR_DEFAULT = 0,
	COLOR_ANSI,
	COLOR_RGB
} LicColorKind;

typedef struct {
	LicColorKind kind;
	uint8_t idx;
	uint8_t r;
	uint8_t g;
	uint8_t b;
} LicColor;

typedef struct {
	LicColor bar_bg;
	LicColor bar_fg;
	LicColor active_bg;
	LicColor active_fg;
	LicColor unread;
	LicColor mention;
	LicColor timestamp;
	LicColor event;
	LicColor error;
	LicColor action;
	LicColor prompt;
} LicTheme;

typedef struct {
	char host[128];
	char port[16];
	bool tls;
	char nick[STATE_NICK_LEN];
	char user[64];
	char realname[128];
	char sasl_user[64];
	char sasl_pass[128];
	size_t history_size;
	char auto_join[16][STATE_NAME_LEN];
	size_t auto_join_count;
	LicTheme theme;
} LicConfig;

struct LuaEngine {
	lua_State *L;
	AppState *state;
	NetConn *conn;
	char script_path[256];
	char plugin_dir[256];
	LicConfig config;
};

bool lua_engine_init(struct LuaEngine *eng, AppState *st, NetConn *conn, const char *script_path, const char *plugin_dir);
void lua_engine_close(struct LuaEngine *eng);
bool lua_reload(struct LuaEngine *eng);
void lua_exec_string(struct LuaEngine *eng, const char *code);
bool lua_emit_message(struct LuaEngine *eng, const IrcMessage *msg);
bool lua_emit_input(struct LuaEngine *eng, const char *input);
bool lua_emit_command(struct LuaEngine *eng, const char *cmd, const char *arg);
size_t lua_list_commands(struct LuaEngine *eng, const char **out, size_t max);
void lua_tick_timers(struct LuaEngine *eng, bool *dirty);

#endif
