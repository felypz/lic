#include "lua_api.h"

#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

static struct LuaEngine *get_engine(lua_State *L) {
	lua_getfield(L, LUA_REGISTRYINDEX, "lic_engine");
	struct LuaEngine *eng = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return eng;
}

static int l_send_raw(lua_State *L) {
	struct LuaEngine *eng = get_engine(L);
	const char *raw = luaL_checkstring(L, 1);
	if (eng && eng->conn && eng->conn->connected) {
		irc_send_raw(eng->conn, "%s", raw);
	}
	return 0;
}

static int l_privmsg(lua_State *L) {
	struct LuaEngine *eng = get_engine(L);
	const char *target = luaL_checkstring(L, 1);
	const char *text = luaL_checkstring(L, 2);
	if (eng && eng->conn && eng->conn->connected) {
		irc_send_privmsg(eng->conn, target, text);
		Buffer *b = state_get_or_create_buffer(eng->state, target, target[0] == '#' ? BUF_CHANNEL : BUF_QUERY);
		buffer_push_line(b, LINE_MSG, eng->state->nick, text, false);
	}
	return 0;
}

static int l_join(lua_State *L) {
	struct LuaEngine *eng = get_engine(L);
	const char *channel = luaL_checkstring(L, 1);
	if (eng && eng->conn && eng->conn->connected) {
		irc_send_join(eng->conn, channel);
		state_get_or_create_buffer(eng->state, channel, BUF_CHANNEL);
	}
	return 0;
}

static int l_print(lua_State *L) {
	struct LuaEngine *eng = get_engine(L);
	const char *text = luaL_checkstring(L, 1);
	const char *buf_name = luaL_optstring(L, 2, NULL);
	Buffer *b = buf_name ? state_get_or_create_buffer(eng->state, buf_name, BUF_SERVER)
	                     : state_active_buffer(eng->state);
	buffer_push_line(b, LINE_EVENT, "", text, false);
	return 0;
}

static int l_active_buffer(lua_State *L) {
	struct LuaEngine *eng = get_engine(L);
	Buffer *b = state_active_buffer(eng->state);
	lua_pushstring(L, b ? b->name : "");
	return 1;
}

static int l_nick(lua_State *L) {
	struct LuaEngine *eng = get_engine(L);
	lua_pushstring(L, eng->state->nick);
	return 1;
}

static int l_on(lua_State *L) {
	const char *event = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	lua_getfield(L, LUA_REGISTRYINDEX, "lic_hooks");
	lua_getfield(L, -1, event);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, event);
	}

	int len = (int)lua_rawlen(L, -1);
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, len + 1);
	lua_pop(L, 2);
	return 0;
}

static int l_command(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	lua_getfield(L, LUA_REGISTRYINDEX, "lic_commands");
	lua_pushvalue(L, 2);
	lua_setfield(L, -2, name);
	lua_pop(L, 1);
	return 0;
}

static void parse_config_table(struct LuaEngine *eng) {
	lua_State *L = eng->L;
	lua_getglobal(L, "lic");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return;
	}

	lua_getfield(L, -1, "config");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return;
	}

	lua_getfield(L, -1, "host");
	if (lua_isstring(L, -1)) {
		snprintf(eng->config.host, sizeof(eng->config.host), "%s", lua_tostring(L, -1));
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "port");
	if (lua_isstring(L, -1)) {
		snprintf(eng->config.port, sizeof(eng->config.port), "%s", lua_tostring(L, -1));
	} else if (lua_isnumber(L, -1)) {
		snprintf(eng->config.port, sizeof(eng->config.port), "%d", (int)lua_tointeger(L, -1));
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "tls");
	if (lua_isboolean(L, -1)) {
		eng->config.tls = lua_toboolean(L, -1);
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "nick");
	if (lua_isstring(L, -1)) {
		snprintf(eng->config.nick, sizeof(eng->config.nick), "%s", lua_tostring(L, -1));
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "user");
	if (lua_isstring(L, -1)) {
		snprintf(eng->config.user, sizeof(eng->config.user), "%s", lua_tostring(L, -1));
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "realname");
	if (lua_isstring(L, -1)) {
		snprintf(eng->config.realname, sizeof(eng->config.realname), "%s", lua_tostring(L, -1));
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "sasl_user");
	if (lua_isstring(L, -1)) {
		snprintf(eng->config.sasl_user, sizeof(eng->config.sasl_user), "%s", lua_tostring(L, -1));
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "sasl_pass");
	if (lua_isstring(L, -1)) {
		snprintf(eng->config.sasl_pass, sizeof(eng->config.sasl_pass), "%s", lua_tostring(L, -1));
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "history_size");
	if (lua_isnumber(L, -1)) {
		lua_Integer val = lua_tointeger(L, -1);
		if (val > 0) {
			eng->config.history_size = (size_t)val;
		}
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "channels");
	if (lua_istable(L, -1)) {
		eng->config.auto_join_count = 0;
		int n = (int)lua_rawlen(L, -1);
		for (int i = 1; i <= n && eng->config.auto_join_count < 16; i++) {
			lua_rawgeti(L, -1, i);
			if (lua_isstring(L, -1)) {
				snprintf(eng->config.auto_join[eng->config.auto_join_count++],
				         STATE_NAME_LEN, "%s", lua_tostring(L, -1));
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 3);
}

static void register_api(struct LuaEngine *eng) {
	lua_State *L = eng->L;

	lua_pushlightuserdata(L, eng);
	lua_setfield(L, LUA_REGISTRYINDEX, "lic_engine");

	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "lic_hooks");

	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "lic_commands");

	static const luaL_Reg funcs[] = {
		{"send_raw",      l_send_raw},
		{"privmsg",       l_privmsg},
		{"join",          l_join},
		{"print",         l_print},
		{"active_buffer", l_active_buffer},
		{"nick",          l_nick},
		{"on",            l_on},
		{"command",       l_command},
		{NULL,            NULL}
	};

	luaL_newlib(L, funcs);
	lua_newtable(L);
	lua_setfield(L, -2, "config");
	lua_setglobal(L, "lic");
}

bool lua_engine_init(struct LuaEngine *eng, AppState *st, NetConn *conn, const char *script_path) {
	memset(eng, 0, sizeof(*eng));
	eng->state = st;
	eng->conn = conn;
	snprintf(eng->script_path, sizeof(eng->script_path), "%s", script_path);

	snprintf(eng->config.host, sizeof(eng->config.host), "irc.libera.chat");
	snprintf(eng->config.port, sizeof(eng->config.port), "6697");
	eng->config.tls = true;
	snprintf(eng->config.nick, sizeof(eng->config.nick), "lic_test");
	snprintf(eng->config.user, sizeof(eng->config.user), "lic");
	snprintf(eng->config.realname, sizeof(eng->config.realname), "Lunar IRC Client");
	eng->config.history_size = 1024;

	return lua_reload(eng);
}

void lua_engine_close(struct LuaEngine *eng) {
	if (eng->L) {
		lua_close(eng->L);
		eng->L = NULL;
	}
}

bool lua_reload(struct LuaEngine *eng) {
	if (eng->L) {
		lua_close(eng->L);
	}
	eng->L = luaL_newstate();
	if (!eng->L) {
		return false;
	}

	luaL_openlibs(eng->L);
	register_api(eng);

	if (luaL_dofile(eng->L, eng->script_path) != LUA_OK) {
		Buffer *b = state_active_buffer(eng->state);
		if (b) {
			buffer_push_line(b, LINE_ERROR, "", lua_tostring(eng->L, -1), false);
		}
		lua_pop(eng->L, 1);
		return false;
	}

	parse_config_table(eng);
	return true;
}

void lua_exec_string(struct LuaEngine *eng, const char *code) {
	if (!eng->L) {
		return;
	}
	if (luaL_dostring(eng->L, code) != LUA_OK) {
		Buffer *b = state_active_buffer(eng->state);
		buffer_push_line(b, LINE_ERROR, "", lua_tostring(eng->L, -1), false);
		lua_pop(eng->L, 1);
	}
}

bool lua_emit_message(struct LuaEngine *eng, const IrcMessage *msg) {
	if (!eng->L) {
		return false;
	}
	lua_State *L = eng->L;

	lua_getfield(L, LUA_REGISTRYINDEX, "lic_hooks");
	lua_getfield(L, -1, msg->command);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return false;
	}

	bool cancel = false;
	int len = (int)lua_rawlen(L, -1);
	for (int i = 1; i <= len; i++) {
		lua_rawgeti(L, -1, i);
		lua_newtable(L);

		lua_pushstring(L, msg->command);
		lua_setfield(L, -2, "command");

		if (msg->nick) {
			lua_pushstring(L, msg->nick);
			lua_setfield(L, -2, "nick");
		}
		if (msg->tags) {
			lua_pushstring(L, msg->tags);
			lua_setfield(L, -2, "tags");
		}

		lua_newtable(L);
		for (int p = 0; p < msg->param_count; p++) {
			lua_pushstring(L, msg->params[p]);
			lua_rawseti(L, -2, p + 1);
		}
		lua_setfield(L, -2, "params");

		if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
			if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
				cancel = true;
			}
			lua_pop(L, 1);
		} else {
			Buffer *b = state_active_buffer(eng->state);
			buffer_push_line(b, LINE_ERROR, "", lua_tostring(L, -1), false);
			lua_pop(L, 1);
		}
	}

	lua_pop(L, 2);
	return cancel;
}

bool lua_emit_input(struct LuaEngine *eng, const char *input) {
	if (!eng->L) {
		return false;
	}
	lua_State *L = eng->L;

	lua_getfield(L, LUA_REGISTRYINDEX, "lic_hooks");
	lua_getfield(L, -1, "INPUT");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return false;
	}

	bool cancel = false;
	int len = (int)lua_rawlen(L, -1);
	for (int i = 1; i <= len; i++) {
		lua_rawgeti(L, -1, i);
		lua_pushstring(L, input);
		if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
			if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
				cancel = true;
			}
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
		}
	}

	lua_pop(L, 2);
	return cancel;
}

bool lua_emit_command(struct LuaEngine *eng, const char *cmd, const char *arg) {
	if (!eng->L) {
		return false;
	}
	lua_State *L = eng->L;

	lua_getfield(L, LUA_REGISTRYINDEX, "lic_commands");
	lua_getfield(L, -1, cmd);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return false;
	}

	lua_pushstring(L, arg);
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		Buffer *b = state_active_buffer(eng->state);
		buffer_push_line(b, LINE_ERROR, "", lua_tostring(L, -1), false);
		lua_pop(L, 1);
	}

	lua_pop(L, 1);
	return true;
}

size_t lua_list_commands(struct LuaEngine *eng, const char **out, size_t max) {
	if (!eng || !eng->L || max == 0) {
		return 0;
	}
	lua_State *L = eng->L;
	lua_getfield(L, LUA_REGISTRYINDEX, "lic_commands");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}

	size_t count = 0;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		if (lua_type(L, -2) == LUA_TSTRING && count < max) {
			out[count++] = lua_tostring(L, -2);
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return count;
}
