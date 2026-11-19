#define _POSIX_C_SOURCE 200809L
#include "lua_api.h"

#include <ctype.h>
#include <dirent.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_LUA_TIMERS 32

typedef struct {
	bool active;
	bool repeat;
	int interval_ms;
	uint64_t next_fire_ms;
	int callback_ref;
} LuaTimer;

static struct LuaEngine *g_eng = NULL;
static LuaTimer g_timers[MAX_LUA_TIMERS];

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void clear_timers(lua_State *L) {
	for (size_t i = 0; i < MAX_LUA_TIMERS; i++) {
		if (g_timers[i].active && L) {
			luaL_unref(L, LUA_REGISTRYINDEX, g_timers[i].callback_ref);
		}
		g_timers[i].active = false;
	}
}

static LicColor make_ansi(uint8_t idx) {
	LicColor c = {0};
	c.kind = COLOR_ANSI;
	c.idx = idx;
	return c;
}

static LicColor make_rgb(uint8_t r, uint8_t g, uint8_t b) {
	LicColor c = {0};
	c.kind = COLOR_RGB;
	c.r = r;
	c.g = g;
	c.b = b;
	return c;
}

static LicColor parse_color_string(const char *s, LicColor fallback) {
	if (!s || s[0] == '\0' || strcasecmp(s, "default") == 0) {
		LicColor c = {0};
		c.kind = COLOR_DEFAULT;
		return c;
	}

	if (s[0] == '#' && strlen(s) == 7) {
		char *endp = NULL;
		unsigned long hex = strtoul(s + 1, &endp, 16);
		if (endp && *endp == '\0') {
			return make_rgb((uint8_t)((hex >> 16) & 0xFF),
			                (uint8_t)((hex >> 8) & 0xFF),
			                (uint8_t)(hex & 0xFF));
		}
	}

	char *endp = NULL;
	long idx = strtol(s, &endp, 10);
	if (endp && *endp == '\0' && idx >= 0 && idx <= 255) {
		return make_ansi((uint8_t)idx);
	}

	if (strcasecmp(s, "black") == 0) return make_ansi(0);
	if (strcasecmp(s, "red") == 0) return make_ansi(1);
	if (strcasecmp(s, "green") == 0) return make_ansi(2);
	if (strcasecmp(s, "yellow") == 0) return make_ansi(3);
	if (strcasecmp(s, "blue") == 0) return make_ansi(4);
	if (strcasecmp(s, "magenta") == 0) return make_ansi(5);
	if (strcasecmp(s, "cyan") == 0) return make_ansi(6);
	if (strcasecmp(s, "white") == 0) return make_ansi(7);
	if (strcasecmp(s, "bright_black") == 0 || strcasecmp(s, "gray") == 0) return make_ansi(8);
	if (strcasecmp(s, "bright_red") == 0) return make_ansi(9);
	if (strcasecmp(s, "bright_green") == 0) return make_ansi(10);
	if (strcasecmp(s, "bright_yellow") == 0) return make_ansi(11);
	if (strcasecmp(s, "bright_blue") == 0) return make_ansi(12);
	if (strcasecmp(s, "bright_magenta") == 0) return make_ansi(13);
	if (strcasecmp(s, "bright_cyan") == 0) return make_ansi(14);
	if (strcasecmp(s, "bright_white") == 0) return make_ansi(15);

	return fallback;
}

static void read_theme_field(lua_State *L, const char *field, LicColor *dst) {
	lua_getfield(L, -1, field);
	if (lua_isinteger(L, -1)) {
		lua_Integer v = lua_tointeger(L, -1);
		if (v >= 0 && v <= 255) {
			*dst = make_ansi((uint8_t)v);
		}
	} else if (lua_isstring(L, -1)) {
		*dst = parse_color_string(lua_tostring(L, -1), *dst);
	}
	lua_pop(L, 1);
}

static void config_set_defaults(LicConfig *cfg) {
	memset(cfg, 0, sizeof(*cfg));
	snprintf(cfg->host, sizeof(cfg->host), "irc.libera.chat");
	snprintf(cfg->port, sizeof(cfg->port), "6697");
	cfg->tls = true;
	snprintf(cfg->user, sizeof(cfg->user), "lic");
	snprintf(cfg->realname, sizeof(cfg->realname), "Lunar IRC Client");
	cfg->history_size = 1024;

	cfg->theme.bar_bg = make_ansi(4);
	cfg->theme.bar_fg = make_ansi(15);
	cfg->theme.active_bg = make_ansi(15);
	cfg->theme.active_fg = make_ansi(0);
	cfg->theme.unread = make_ansi(14);
	cfg->theme.mention = make_ansi(11);
	cfg->theme.timestamp = make_ansi(8);
	cfg->theme.event = make_ansi(6);
	cfg->theme.error = make_ansi(9);
	cfg->theme.action = make_ansi(13);
	cfg->theme.prompt = make_ansi(10);
}

static int l_send(lua_State *L) {
	const char *target = luaL_checkstring(L, 1);
	const char *text = luaL_checkstring(L, 2);
	if (g_eng && g_eng->conn && g_eng->conn->connected) {
		irc_send_privmsg(g_eng->conn, target, text);
		BufferType btype = (target[0] == '#' || target[0] == '&') ? BUF_CHANNEL : BUF_QUERY;
		Buffer *b = state_get_or_create_buffer(g_eng->state, target, btype);
		buffer_push_line(b, LINE_MSG, g_eng->state->nick, text, false);
	}
	return 0;
}

static int l_raw(lua_State *L) {
	const char *line = luaL_checkstring(L, 1);
	if (g_eng && g_eng->conn && g_eng->conn->connected) {
		irc_send_raw(g_eng->conn, "%s", line);
	}
	return 0;
}

static int l_echo(lua_State *L) {
	const char *text = luaL_checkstring(L, 1);
	const char *buf_name = luaL_optstring(L, 2, NULL);
	if (!g_eng || !g_eng->state) {
		return 0;
	}
	Buffer *b = buf_name ? state_get_or_create_buffer(g_eng->state, buf_name, BUF_CHANNEL)
	                     : state_active_buffer(g_eng->state);
	if (b) {
		buffer_push_line(b, LINE_EVENT, "", text, false);
	}
	return 0;
}

static int l_current_buffer(lua_State *L) {
	if (g_eng && g_eng->state) {
		Buffer *b = state_active_buffer(g_eng->state);
		if (b) {
			lua_pushstring(L, b->name);
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

static int l_nick(lua_State *L) {
	if (g_eng && g_eng->state) {
		lua_pushstring(L, g_eng->state->nick);
		return 1;
	}
	lua_pushstring(L, "");
	return 1;
}

static int l_buffers(lua_State *L) {
	lua_newtable(L);
	if (!g_eng || !g_eng->state) {
		return 1;
	}
	for (size_t i = 0; i < g_eng->state->buffer_count; i++) {
		lua_pushstring(L, g_eng->state->buffers[i].name);
		lua_rawseti(L, -2, (lua_Integer)(i + 1));
	}
	return 1;
}

static int l_users(lua_State *L) {
	const char *buf_name = luaL_optstring(L, 1, NULL);
	lua_newtable(L);
	if (!g_eng || !g_eng->state) {
		return 1;
	}
	Buffer *b = NULL;
	if (buf_name) {
		for (size_t i = 0; i < g_eng->state->buffer_count; i++) {
			if (strcasecmp(g_eng->state->buffers[i].name, buf_name) == 0) {
				b = &g_eng->state->buffers[i];
				break;
			}
		}
	} else {
		b = state_active_buffer(g_eng->state);
	}
	if (!b) {
		return 1;
	}
	for (size_t i = 0; i < b->user_count; i++) {
		lua_newtable(L);
		lua_pushstring(L, b->users[i].nick);
		lua_setfield(L, -2, "nick");
		char pfx[2] = {b->users[i].prefix, '\0'};
		lua_pushstring(L, pfx);
		lua_setfield(L, -2, "prefix");
		lua_rawseti(L, -2, (lua_Integer)(i + 1));
	}
	return 1;
}

static int l_join(lua_State *L) {
	const char *chan = luaL_checkstring(L, 1);
	if (g_eng && g_eng->conn && g_eng->conn->connected) {
		irc_send_join(g_eng->conn, chan);
		state_get_or_create_buffer(g_eng->state, chan, BUF_CHANNEL);
	}
	return 0;
}

static int l_part(lua_State *L) {
	const char *chan = luaL_checkstring(L, 1);
	const char *reason = luaL_optstring(L, 2, NULL);
	if (g_eng && g_eng->conn && g_eng->conn->connected) {
		if (reason && reason[0] != '\0') {
			irc_send_raw(g_eng->conn, "PART %s :%s", chan, reason);
		} else {
			irc_send_raw(g_eng->conn, "PART %s", chan);
		}
	}
	return 0;
}

static int l_switch_buffer(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	if (!g_eng || !g_eng->state) {
		return 0;
	}
	for (size_t i = 0; i < g_eng->state->buffer_count; i++) {
		if (strcasecmp(g_eng->state->buffers[i].name, name) == 0) {
			state_switch_buffer(g_eng->state, i);
			break;
		}
	}
	return 0;
}

static int l_timer(lua_State *L) {
	int ms = (int)luaL_checkinteger(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	bool repeat = lua_toboolean(L, 3);

	if (ms < 10) {
		ms = 10;
	}

	for (size_t i = 0; i < MAX_LUA_TIMERS; i++) {
		if (!g_timers[i].active) {
			lua_pushvalue(L, 2);
			g_timers[i].callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
			g_timers[i].interval_ms = ms;
			g_timers[i].repeat = repeat;
			g_timers[i].next_fire_ms = now_ms() + (uint64_t)ms;
			g_timers[i].active = true;
			lua_pushinteger(L, (lua_Integer)i);
			return 1;
		}
	}

	lua_pushnil(L);
	return 1;
}

static int l_cancel_timer(lua_State *L) {
	lua_Integer id = luaL_checkinteger(L, 1);
	if (id >= 0 && (size_t)id < MAX_LUA_TIMERS && g_timers[id].active) {
		luaL_unref(L, LUA_REGISTRYINDEX, g_timers[id].callback_ref);
		g_timers[id].active = false;
	}
	return 0;
}

static void setup_lua_globals(lua_State *L) {
	lua_newtable(L);

	lua_pushcfunction(L, l_send);
	lua_setfield(L, -2, "send");

	lua_pushcfunction(L, l_raw);
	lua_setfield(L, -2, "raw");

	lua_pushcfunction(L, l_echo);
	lua_setfield(L, -2, "echo");

	lua_pushcfunction(L, l_current_buffer);
	lua_setfield(L, -2, "current_buffer");

	lua_pushcfunction(L, l_nick);
	lua_setfield(L, -2, "nick");

	lua_pushcfunction(L, l_buffers);
	lua_setfield(L, -2, "buffers");

	lua_pushcfunction(L, l_users);
	lua_setfield(L, -2, "users");

	lua_pushcfunction(L, l_join);
	lua_setfield(L, -2, "join");

	lua_pushcfunction(L, l_part);
	lua_setfield(L, -2, "part");

	lua_pushcfunction(L, l_switch_buffer);
	lua_setfield(L, -2, "switch_buffer");

	lua_pushcfunction(L, l_timer);
	lua_setfield(L, -2, "timer");

	lua_pushcfunction(L, l_cancel_timer);
	lua_setfield(L, -2, "cancel_timer");

	lua_newtable(L);
	lua_setfield(L, -2, "config");

	lua_newtable(L);
	lua_setfield(L, -2, "theme");

	lua_newtable(L);
	lua_setfield(L, -2, "commands");

	lua_setglobal(L, "lic");

	const char *helper_lua =
		"lic._msg_hooks = {}\n"
		"lic._input_hooks = {}\n"
		"function lic.on_message(fn) table.insert(lic._msg_hooks, fn) end\n"
		"function lic.on_input(fn) table.insert(lic._input_hooks, fn) end\n"
		"function lic.command(name, fn) lic.commands[name:lower()] = fn end\n"
		"function lic._dispatch_message(msg)\n"
		"  for _, fn in ipairs(lic._msg_hooks) do\n"
		"    if fn(msg) == true then return true end\n"
		"  end\n"
		"  return false\n"
		"end\n"
		"function lic._dispatch_input(line)\n"
		"  for _, fn in ipairs(lic._input_hooks) do\n"
		"    if fn(line) == true then return true end\n"
		"  end\n"
		"  return false\n"
		"end\n"
		"function lic._dispatch_command(name, arg)\n"
		"  local fn = lic.commands[name:lower()]\n"
		"  if fn then fn(arg) return true end\n"
		"  return false\n"
		"end\n";

	(void)luaL_dostring(L, helper_lua);
}

static void read_str_field(lua_State *L, const char *field, char *dst, size_t size) {
	lua_getfield(L, -1, field);
	if (lua_isstring(L, -1)) {
		snprintf(dst, size, "%s", lua_tostring(L, -1));
	}
	lua_pop(L, 1);
}

static void parse_config_table(struct LuaEngine *eng) {
	lua_State *L = eng->L;
	lua_getglobal(L, "lic");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return;
	}

	lua_getfield(L, -1, "config");
	if (lua_istable(L, -1)) {
		read_str_field(L, "host", eng->config.host, sizeof(eng->config.host));
		read_str_field(L, "port", eng->config.port, sizeof(eng->config.port));
		read_str_field(L, "nick", eng->config.nick, sizeof(eng->config.nick));
		read_str_field(L, "user", eng->config.user, sizeof(eng->config.user));
		read_str_field(L, "realname", eng->config.realname, sizeof(eng->config.realname));
		read_str_field(L, "sasl_user", eng->config.sasl_user, sizeof(eng->config.sasl_user));
		read_str_field(L, "sasl_pass", eng->config.sasl_pass, sizeof(eng->config.sasl_pass));

		lua_getfield(L, -1, "tls");
		if (lua_isboolean(L, -1)) {
			eng->config.tls = lua_toboolean(L, -1);
		}
		lua_pop(L, 1);

		lua_getfield(L, -1, "history_size");
		if (lua_isinteger(L, -1)) {
			lua_Integer hs = lua_tointeger(L, -1);
			if (hs >= 64 && hs <= 65536) {
				eng->config.history_size = (size_t)hs;
			}
		}
		lua_pop(L, 1);

		lua_getfield(L, -1, "auto_join");
		if (lua_istable(L, -1)) {
			eng->config.auto_join_count = 0;
			size_t len = lua_rawlen(L, -1);
			for (size_t i = 1; i <= len && eng->config.auto_join_count < 16; i++) {
				lua_rawgeti(L, -1, (lua_Integer)i);
				if (lua_isstring(L, -1)) {
					snprintf(eng->config.auto_join[eng->config.auto_join_count++],
					         STATE_NAME_LEN, "%s", lua_tostring(L, -1));
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	lua_getfield(L, -1, "theme");
	if (lua_istable(L, -1)) {
		read_theme_field(L, "bar_bg", &eng->config.theme.bar_bg);
		read_theme_field(L, "bar_fg", &eng->config.theme.bar_fg);
		read_theme_field(L, "active_bg", &eng->config.theme.active_bg);
		read_theme_field(L, "active_fg", &eng->config.theme.active_fg);
		read_theme_field(L, "unread", &eng->config.theme.unread);
		read_theme_field(L, "mention", &eng->config.theme.mention);
		read_theme_field(L, "timestamp", &eng->config.theme.timestamp);
		read_theme_field(L, "event", &eng->config.theme.event);
		read_theme_field(L, "error", &eng->config.theme.error);
		read_theme_field(L, "action", &eng->config.theme.action);
		read_theme_field(L, "prompt", &eng->config.theme.prompt);
	}
	lua_pop(L, 1);
	lua_pop(L, 1);
}

static int cmp_str_ptr(const void *a, const void *b) {
	const char *sa = *(const char *const *)a;
	const char *sb = *(const char *const *)b;
	return strcmp(sa, sb);
}

static void load_plugins_from_dir(struct LuaEngine *eng) {
	if (!eng->plugin_dir[0]) {
		return;
	}
	DIR *d = opendir(eng->plugin_dir);
	if (!d) {
		return;
	}

	char *files[128];
	size_t count = 0;
	struct dirent *ent;

	while ((ent = readdir(d)) != NULL && count < 128) {
		const char *name = ent->d_name;
		if (name[0] == '.') {
			continue;
		}
		size_t len = strlen(name);
		if (len > 4 && strcmp(name + len - 4, ".lua") == 0) {
			files[count] = strdup(name);
			if (files[count]) {
				count++;
			}
		}
	}
	closedir(d);

	qsort(files, count, sizeof(char *), cmp_str_ptr);

	for (size_t i = 0; i < count; i++) {
		char fullpath[512];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", eng->plugin_dir, files[i]);
		if (luaL_dofile(eng->L, fullpath) != LUA_OK) {
			Buffer *b = state_active_buffer(eng->state);
			if (b) {
				buffer_push_line(b, LINE_ERROR, "", lua_tostring(eng->L, -1), false);
			}
			lua_pop(eng->L, 1);
		}
		free(files[i]);
	}
}

void lua_sync_runtime(struct LuaEngine *eng) {
	if (!eng || !eng->L) {
		return;
	}
	char prev_nick[STATE_NICK_LEN];
	snprintf(prev_nick, sizeof(prev_nick), "%s", eng->config.nick);

	parse_config_table(eng);

	if (eng->state) {
		if (eng->config.nick[0] != '\0' && strcmp(prev_nick, eng->config.nick) != 0) {
			if (eng->conn && eng->conn->connected) {
				irc_send_raw(eng->conn, "NICK %s", eng->config.nick);
			} else {
				snprintf(eng->state->nick, sizeof(eng->state->nick), "%s", eng->config.nick);
			}
		}
		if (eng->config.history_size > 0) {
			eng->state->history_cap = eng->config.history_size;
		}
	}
}

bool lua_reload(struct LuaEngine *eng) {
	if (!eng) {
		return false;
	}
	if (eng->L) {
		clear_timers(eng->L);
		lua_close(eng->L);
	}

	eng->L = luaL_newstate();
	if (!eng->L) {
		return false;
	}

	luaL_openlibs(eng->L);
	setup_lua_globals(eng->L);

	if (eng->script_path[0] != '\0') {
		if (luaL_dofile(eng->L, eng->script_path) != LUA_OK) {
			Buffer *b = state_active_buffer(eng->state);
			if (b) {
				buffer_push_line(b, LINE_ERROR, "", lua_tostring(eng->L, -1), false);
			}
			lua_pop(eng->L, 1);
			return false;
		}
	}

	load_plugins_from_dir(eng);
	lua_sync_runtime(eng);
	return true;
}

bool lua_engine_init(struct LuaEngine *eng, AppState *st, NetConn *conn, const char *script_path, const char *plugin_dir) {
	memset(eng, 0, sizeof(*eng));
	eng->state = st;
	eng->conn = conn;
	g_eng = eng;

	if (script_path) {
		snprintf(eng->script_path, sizeof(eng->script_path), "%s", script_path);
	}
	if (plugin_dir) {
		snprintf(eng->plugin_dir, sizeof(eng->plugin_dir), "%s", plugin_dir);
	}

	config_set_defaults(&eng->config);
	return lua_reload(eng);
}

void lua_engine_close(struct LuaEngine *eng) {
	if (eng && eng->L) {
		clear_timers(eng->L);
		lua_close(eng->L);
		eng->L = NULL;
	}
	g_eng = NULL;
}

void lua_exec_string(struct LuaEngine *eng, const char *code) {
	if (!eng || !eng->L) {
		return;
	}
	if (luaL_dostring(eng->L, code) != LUA_OK) {
		Buffer *b = state_active_buffer(eng->state);
		if (b) {
			buffer_push_line(b, LINE_ERROR, "", lua_tostring(eng->L, -1), false);
		}
		lua_pop(eng->L, 1);
		return;
	}
	lua_sync_runtime(eng);
}

bool lua_exec_file(struct LuaEngine *eng, const char *path) {
	if (!eng || !eng->L || !path || path[0] == '\0') {
		return false;
	}
	if (luaL_dofile(eng->L, path) != LUA_OK) {
		Buffer *b = state_active_buffer(eng->state);
		if (b) {
			buffer_push_line(b, LINE_ERROR, "", lua_tostring(eng->L, -1), false);
		}
		lua_pop(eng->L, 1);
		return false;
	}
	lua_sync_runtime(eng);
	return true;
}

bool lua_emit_message(struct LuaEngine *eng, const IrcMessage *msg) {
	if (!eng || !eng->L) {
		return false;
	}
	lua_State *L = eng->L;

	lua_getglobal(L, "lic");
	lua_getfield(L, -1, "_dispatch_message");

	lua_newtable(L);
	lua_pushstring(L, msg->command);
	lua_setfield(L, -2, "command");

	if (msg->nick) {
		lua_pushstring(L, msg->nick);
		lua_setfield(L, -2, "nick");
	}
	if (msg->prefix) {
		lua_pushstring(L, msg->prefix);
		lua_setfield(L, -2, "prefix");
	}

	lua_newtable(L);
	for (int i = 0; i < msg->param_count; i++) {
		lua_pushstring(L, msg->params[i]);
		lua_rawseti(L, -2, (lua_Integer)(i + 1));
	}
	lua_setfield(L, -2, "params");

	lua_newtable(L);
	lua_setfield(L, -2, "tags");

	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		Buffer *b = state_active_buffer(eng->state);
		if (b) {
			buffer_push_line(b, LINE_ERROR, "", lua_tostring(L, -1), false);
		}
		lua_pop(L, 2);
		return false;
	}

	bool handled = lua_toboolean(L, -1);
	lua_pop(L, 2);
	return handled;
}

bool lua_emit_input(struct LuaEngine *eng, const char *input) {
	if (!eng || !eng->L) {
		return false;
	}
	lua_State *L = eng->L;

	lua_getglobal(L, "lic");
	lua_getfield(L, -1, "_dispatch_input");
	lua_pushstring(L, input);

	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		Buffer *b = state_active_buffer(eng->state);
		if (b) {
			buffer_push_line(b, LINE_ERROR, "", lua_tostring(L, -1), false);
		}
		lua_pop(L, 2);
		return false;
	}

	bool handled = lua_toboolean(L, -1);
	lua_pop(L, 2);
	return handled;
}

bool lua_emit_command(struct LuaEngine *eng, const char *cmd, const char *arg) {
	if (!eng || !eng->L) {
		return false;
	}
	lua_State *L = eng->L;

	lua_getglobal(L, "lic");
	lua_getfield(L, -1, "_dispatch_command");
	lua_pushstring(L, cmd);
	lua_pushstring(L, arg ? arg : "");

	if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
		Buffer *b = state_active_buffer(eng->state);
		if (b) {
			buffer_push_line(b, LINE_ERROR, "", lua_tostring(L, -1), false);
		}
		lua_pop(L, 2);
		return false;
	}

	bool handled = lua_toboolean(L, -1);
	lua_pop(L, 2);
	return handled;
}

size_t lua_list_commands(struct LuaEngine *eng, const char **out, size_t max) {
	if (!eng || !eng->L || max == 0) {
		return 0;
	}
	lua_State *L = eng->L;
	size_t count = 0;

	lua_getglobal(L, "lic");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}

	lua_getfield(L, -1, "commands");
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING && count < max) {
				out[count++] = lua_tostring(L, -2);
			}
			lua_pop(L, 1);
		}
	}

	lua_pop(L, 2);
	return count;
}

void lua_tick_timers(struct LuaEngine *eng, bool *dirty) {
	if (!eng || !eng->L) {
		return;
	}
	uint64_t t = now_ms();
	lua_State *L = eng->L;

	for (size_t i = 0; i < MAX_LUA_TIMERS; i++) {
		if (!g_timers[i].active || t < g_timers[i].next_fire_ms) {
			continue;
		}

		int ref = g_timers[i].callback_ref;
		if (g_timers[i].repeat) {
			g_timers[i].next_fire_ms = t + (uint64_t)g_timers[i].interval_ms;
		} else {
			g_timers[i].active = false;
			luaL_unref(L, LUA_REGISTRYINDEX, ref);
		}

		lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
		if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
			Buffer *b = state_active_buffer(eng->state);
			if (b) {
				buffer_push_line(b, LINE_ERROR, "", lua_tostring(L, -1), false);
			}
			lua_pop(L, 1);
		}
		if (dirty) {
			*dirty = true;
		}
	}
}
