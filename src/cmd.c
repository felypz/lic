#include "cmd.h"
#include "irc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void split_first_arg(const char *src, char *arg1, size_t max1, const char **rest) {
	while (*src == ' ') {
		src++;
	}
	size_t i = 0;
	while (*src && *src != ' ' && i + 1 < max1) {
		arg1[i++] = *src++;
	}
	arg1[i] = '\0';
	while (*src == ' ') {
		src++;
	}
	*rest = src;
}

bool cmd_execute(AppState *st, NetConn *conn, struct LuaEngine *lua, const char *input) {
	if (!input || input[0] == '\0') {
		return true;
	}

	if (lua_emit_input(lua, input)) {
		return true;
	}

	Buffer *active = state_active_buffer(st);

	if (input[0] != '/') {
		if (!active || active->type == BUF_SERVER) {
			buffer_push_line(active, LINE_ERROR, "", "Cannot send plain text to server buffer", false);
			return true;
		}
		if (!conn || !conn->connected) {
			buffer_push_line(active, LINE_ERROR, "", "Not connected", false);
			return true;
		}
		irc_send_privmsg(conn, active->name, input);
		buffer_push_line(active, LINE_MSG, st->nick, input, false);
		return true;
	}

	char cmd[32];
	const char *args = NULL;
	split_first_arg(input + 1, cmd, sizeof(cmd), &args);

	if (strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "q") == 0) {
		if (conn && conn->connected) {
			irc_send_raw(conn, "QUIT :%s", args[0] ? args : "lic");
		}
		return false;
	}

	if (strcasecmp(cmd, "join") == 0 || strcasecmp(cmd, "j") == 0) {
		if (args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /join <#channel>", false);
			return true;
		}
		char chan[STATE_NAME_LEN];
		const char *key = NULL;
		split_first_arg(args, chan, sizeof(chan), &key);
		if (key[0] != '\0') {
			irc_send_raw(conn, "JOIN %s %s", chan, key);
		} else {
			irc_send_join(conn, chan);
		}
		state_get_or_create_buffer(st, chan, BUF_CHANNEL);
		return true;
	}

	if (strcasecmp(cmd, "part") == 0 || strcasecmp(cmd, "leave") == 0) {
		const char *target = args[0] ? args : (active ? active->name : "");
		if (target[0] == '#' || target[0] == '&') {
			irc_send_raw(conn, "PART %s", target);
			state_remove_buffer(st, target);
		}
		return true;
	}

	if (strcasecmp(cmd, "close") == 0) {
		if (active && active->type != BUF_SERVER) {
			if (active->type == BUF_CHANNEL && conn && conn->connected) {
				irc_send_raw(conn, "PART %s", active->name);
			}
			state_remove_buffer(st, active->name);
		}
		return true;
	}

	if (strcasecmp(cmd, "msg") == 0 || strcasecmp(cmd, "query") == 0) {
		char target[STATE_NAME_LEN];
		const char *text = NULL;
		split_first_arg(args, target, sizeof(target), &text);
		if (target[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /msg <nick> [message]", false);
			return true;
		}
		BufferType btype = (target[0] == '#' || target[0] == '&') ? BUF_CHANNEL : BUF_QUERY;
		Buffer *b = state_get_or_create_buffer(st, target, btype);
		if (text[0] != '\0') {
			irc_send_privmsg(conn, target, text);
			buffer_push_line(b, LINE_MSG, st->nick, text, false);
		}
		return true;
	}

	if (strcasecmp(cmd, "me") == 0) {
		if (!active || active->type == BUF_SERVER || args[0] == '\0') {
			return true;
		}
		char ctcp[IRC_MAX_LINE];
		snprintf(ctcp, sizeof(ctcp), "\001ACTION %s\001", args);
		irc_send_privmsg(conn, active->name, ctcp);
		buffer_push_line(active, LINE_ACTION, st->nick, args, false);
		return true;
	}

	if (strcasecmp(cmd, "nick") == 0) {
		if (args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /nick <newnick>", false);
			return true;
		}
		irc_send_raw(conn, "NICK %s", args);
		return true;
	}

	if (strcasecmp(cmd, "whois") == 0) {
		if (args[0] != '\0') {
			irc_send_raw(conn, "WHOIS %s", args);
		}
		return true;
	}

	if (strcasecmp(cmd, "topic") == 0) {
		if (!active || active->type != BUF_CHANNEL) {
			return true;
		}
		if (args[0] != '\0') {
			irc_send_raw(conn, "TOPIC %s :%s", active->name, args);
		} else {
			irc_send_raw(conn, "TOPIC %s", active->name);
		}
		return true;
	}

	if (strcasecmp(cmd, "kick") == 0) {
		if (!active || active->type != BUF_CHANNEL || args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /kick <nick> [reason]", false);
			return true;
		}
		char target[STATE_NICK_LEN];
		const char *reason = NULL;
		split_first_arg(args, target, sizeof(target), &reason);
		if (reason[0] != '\0') {
			irc_send_raw(conn, "KICK %s %s :%s", active->name, target, reason);
		} else {
			irc_send_raw(conn, "KICK %s %s", active->name, target);
		}
		return true;
	}

	if (strcasecmp(cmd, "mode") == 0) {
		if (args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /mode [target] <modes>", false);
			return true;
		}
		if (args[0] == '+' || args[0] == '-') {
			if (active && active->type == BUF_CHANNEL) {
				irc_send_raw(conn, "MODE %s %s", active->name, args);
			}
		} else {
			irc_send_raw(conn, "MODE %s", args);
		}
		return true;
	}

	if (strcasecmp(cmd, "notice") == 0) {
		char target[STATE_NAME_LEN];
		const char *text = NULL;
		split_first_arg(args, target, sizeof(target), &text);
		if (target[0] == '\0' || text[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /notice <target> <message>", false);
			return true;
		}
		irc_send_raw(conn, "NOTICE %s :%s", target, text);
		char info[STATE_TEXT_LEN];
		snprintf(info, sizeof(info), "-> -%s- %s", target, text);
		buffer_push_line(active, LINE_EVENT, "", info, false);
		return true;
	}

	if (strcasecmp(cmd, "names") == 0) {
		if (!active || active->type != BUF_CHANNEL) {
			return true;
		}
		char list[STATE_TEXT_LEN];
		size_t pos = 0;
		pos += (size_t)snprintf(list + pos, sizeof(list) - pos, "Users (%zu): ", active->user_count);
		for (size_t i = 0; i < active->user_count && pos < sizeof(list) - 32; i++) {
			if (active->users[i].prefix) {
				pos += (size_t)snprintf(list + pos, sizeof(list) - pos, "%c%s ",
				                        active->users[i].prefix, active->users[i].nick);
			} else {
				pos += (size_t)snprintf(list + pos, sizeof(list) - pos, "%s ", active->users[i].nick);
			}
		}
		buffer_push_line(active, LINE_EVENT, "", list, false);
		return true;
	}

	if (strcasecmp(cmd, "clear") == 0) {
		if (active) {
			active->line_count = 0;
			active->line_head = 0;
		}
		return true;
	}

	if (strcasecmp(cmd, "raw") == 0 || strcasecmp(cmd, "quote") == 0) {
		if (args[0] != '\0') {
			irc_send_raw(conn, "%s", args);
		}
		return true;
	}

	if (strcasecmp(cmd, "reload") == 0) {
		if (lua_reload(lua)) {
			buffer_push_line(active, LINE_EVENT, "", "Lua configuration reloaded", false);
		}
		return true;
	}

	if (strcasecmp(cmd, "lua") == 0) {
		if (args[0] != '\0') {
			lua_exec_string(lua, args);
		}
		return true;
	}

	if (lua_emit_command(lua, cmd, args)) {
		return true;
	}

	char err[128];
	snprintf(err, sizeof(err), "Unknown command: /%s", cmd);
	buffer_push_line(active, LINE_ERROR, "", err, false);
	return true;
}

static void format_lua_string_assign(char *dst, size_t dst_size, const char *lhs, const char *val) {
	size_t pos = 0;
	int w = snprintf(dst, dst_size, "%s = \"", lhs);
	if (w > 0) {
		pos = (size_t)w < dst_size ? (size_t)w : dst_size - 1;
	}

	for (const char *p = val; *p && pos + 3 < dst_size; p++) {
		if (*p == '"' || *p == '\\') {
			dst[pos++] = '\\';
		}
		dst[pos++] = *p;
	}

	if (pos + 2 < dst_size) {
		dst[pos++] = '"';
	}
	dst[pos] = '\0';
}

static void apply_vim_set(AppState *st, UiState *ui, struct LuaEngine *lua, const char *expr) {
	Buffer *active = state_active_buffer(st);
	if (!expr || expr[0] == '\0') {
		buffer_push_line(active, LINE_ERROR, "", "Usage: :set <option>=<value>", false);
		return;
	}

	char key[64];
	const char *eq = strchr(expr, '=');
	if (!eq) {
		if (strcmp(expr, "split") == 0) {
			ui->split_enabled = true;
			return;
		}
		if (strcmp(expr, "nosplit") == 0) {
			ui->split_enabled = false;
			return;
		}
		buffer_push_line(active, LINE_ERROR, "", "Usage: :set <option>=<value>", false);
		return;
	}

	size_t klen = (size_t)(eq - expr);
	if (klen == 0 || klen >= sizeof(key)) {
		return;
	}
	memcpy(key, expr, klen);
	key[klen] = '\0';
	const char *val = eq + 1;

	if (strcmp(key, "split") == 0) {
		ui->split_enabled = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "on") == 0);
		return;
	}

	char lua_snippet[512];
	char lhs[128];

	if (strcmp(key, "bar_bg") == 0 || strcmp(key, "bar_fg") == 0 ||
	    strcmp(key, "active_bg") == 0 || strcmp(key, "active_fg") == 0 ||
	    strcmp(key, "unread") == 0 || strcmp(key, "mention") == 0 ||
	    strcmp(key, "timestamp") == 0 || strcmp(key, "event") == 0 ||
	    strcmp(key, "error") == 0 || strcmp(key, "action") == 0 ||
	    strcmp(key, "prompt") == 0) {
		snprintf(lhs, sizeof(lhs), "lic.theme.%s", key);
		char *endp = NULL;
		long num = strtol(val, &endp, 10);
		if (endp && *endp == '\0') {
			snprintf(lua_snippet, sizeof(lua_snippet), "%s = %ld", lhs, num);
		} else {
			format_lua_string_assign(lua_snippet, sizeof(lua_snippet), lhs, val);
		}
		lua_exec_string(lua, lua_snippet);
		return;
	}

	snprintf(lhs, sizeof(lhs), "lic.config.%s", key);
	if (strcmp(key, "tls") == 0) {
		bool b = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0 || strcasecmp(val, "on") == 0);
		snprintf(lua_snippet, sizeof(lua_snippet), "%s = %s", lhs, b ? "true" : "false");
	} else if (strcmp(key, "port") == 0 || strcmp(key, "history_size") == 0) {
		snprintf(lua_snippet, sizeof(lua_snippet), "%s = %d", lhs, atoi(val));
	} else {
		format_lua_string_assign(lua_snippet, sizeof(lua_snippet), lhs, val);
	}
	lua_exec_string(lua, lua_snippet);
}

bool cmd_execute_vim(AppState *st, NetConn *conn, UiState *ui, struct LuaEngine *lua, const char *ex_cmd) {
	if (!ex_cmd) {
		return true;
	}
	while (*ex_cmd == ' ' || *ex_cmd == ':') {
		ex_cmd++;
	}
	if (ex_cmd[0] == '\0') {
		return true;
	}

	char verb[32];
	const char *args = NULL;
	split_first_arg(ex_cmd, verb, sizeof(verb), &args);
	Buffer *active = state_active_buffer(st);

	if (strcmp(verb, "q") == 0 || strcmp(verb, "q!") == 0 ||
	    strcmp(verb, "qa") == 0 || strcmp(verb, "qa!") == 0 ||
	    strcmp(verb, "quit") == 0 || strcmp(verb, "wq") == 0) {
		if (conn && conn->connected) {
			irc_send_raw(conn, "QUIT :lic");
		}
		return false;
	}

	if (strcmp(verb, "bd") == 0 || strcmp(verb, "bd!") == 0 || strcmp(verb, "bw") == 0) {
		if (!active || active->type == BUF_SERVER) {
			buffer_push_line(active, LINE_ERROR, "", "Cannot close server buffer", false);
			return true;
		}
		if (active->type == BUF_CHANNEL && conn && conn->connected) {
			irc_send_raw(conn, "PART %s", active->name);
		}
		state_remove_buffer(st, active->name);
		return true;
	}

	if (strcmp(verb, "bn") == 0 || strcmp(verb, "bnext") == 0) {
		state_next_buffer(st);
		return true;
	}

	if (strcmp(verb, "bp") == 0 || strcmp(verb, "bprev") == 0 || strcmp(verb, "bN") == 0) {
		state_prev_buffer(st);
		return true;
	}

	if (strcmp(verb, "b") == 0 || strcmp(verb, "buffer") == 0) {
		if (args[0] == '\0') {
			return true;
		}
		char *endp = NULL;
		long idx = strtol(args, &endp, 10);
		if (endp && *endp == '\0' && idx >= 1) {
			state_switch_buffer(st, (size_t)(idx - 1));
			return true;
		}
		for (size_t i = 0; i < st->buffer_count; i++) {
			if (strcasecmp(st->buffers[i].name, args) == 0) {
				state_switch_buffer(st, i);
				return true;
			}
		}
		buffer_push_line(active, LINE_ERROR, "", "Buffer not found", false);
		return true;
	}

	if (strcmp(verb, "vs") == 0 || strcmp(verb, "vsplit") == 0 ||
	    strcmp(verb, "sp") == 0 || strcmp(verb, "split") == 0) {
		ui->split_enabled = !ui->split_enabled;
		return true;
	}

	if (strcmp(verb, "on") == 0 || strcmp(verb, "only") == 0) {
		ui->split_enabled = false;
		return true;
	}

	if (strcmp(verb, "e") == 0 || strcmp(verb, "e!") == 0 || strcmp(verb, "reload") == 0) {
		if (lua_reload(lua)) {
			buffer_push_line(active, LINE_EVENT, "", "Lua configuration reloaded", false);
		}
		return true;
	}

	if (strcmp(verb, "so") == 0 || strcmp(verb, "source") == 0) {
		if (args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: :source <file.lua>", false);
			return true;
		}
		if (lua_exec_file(lua, args)) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Sourced %s", args);
			buffer_push_line(active, LINE_EVENT, "", msg, false);
		}
		return true;
	}

	if (strcmp(verb, "lua") == 0) {
		if (args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: :lua <code>", false);
			return true;
		}
		lua_exec_string(lua, args);
		return true;
	}

	if (strcmp(verb, "set") == 0) {
		apply_vim_set(st, ui, lua, args);
		return true;
	}

	char slash_fallback[UI_INPUT_MAX];
	snprintf(slash_fallback, sizeof(slash_fallback), "/%s", ex_cmd);
	return cmd_execute(st, conn, lua, slash_fallback);
}
