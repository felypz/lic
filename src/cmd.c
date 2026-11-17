#include "cmd.h"
#include "irc.h"
#include "lua_api.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static void switch_to_buffer_ptr(AppState *st, Buffer *target) {
	if (!target) {
		return;
	}
	for (size_t i = 0; i < st->buffer_count; i++) {
		if (&st->buffers[i] == target) {
			state_switch_buffer(st, i);
			break;
		}
	}
}

bool cmd_execute(AppState *st, NetConn *conn, LuaEngine *lua, const char *input) {
	if (!input || input[0] == '\0') {
		return true;
	}

	if (lua && lua_emit_input(lua, input)) {
		return true;
	}

	Buffer *active = state_active_buffer(st);

	if (input[0] != '/') {
		if (!active || active->type == BUF_SERVER) {
			buffer_push_line(active, LINE_ERROR, "", "Cannot send messages to server buffer", false);
			return true;
		}
		irc_send_privmsg(conn, active->name, input);
		buffer_push_line(active, LINE_MSG, st->nick, input, false);
		return true;
	}

	char buf[STATE_TEXT_LEN];
	snprintf(buf, sizeof(buf), "%s", input + 1);

	char *arg = strchr(buf, ' ');
	if (arg) {
		*arg++ = '\0';
		while (*arg == ' ') {
			arg++;
		}
		if (*arg == '\0') {
			arg = NULL;
		}
	}

	if (strcasecmp(buf, "join") == 0 || strcasecmp(buf, "j") == 0) {
		if (!arg) {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /join <#channel>", false);
			return true;
		}
		irc_send_join(conn, arg);
		switch_to_buffer_ptr(st, state_get_or_create_buffer(st, arg, BUF_CHANNEL));
	} else if (strcasecmp(buf, "part") == 0 || strcasecmp(buf, "leave") == 0) {
		const char *target = arg ? arg : (active && active->type == BUF_CHANNEL ? active->name : NULL);
		if (!target) {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /part [#channel]", false);
			return true;
		}
		irc_send_raw(conn, "PART %s", target);
		state_remove_buffer(st, target);
	} else if (strcasecmp(buf, "close") == 0) {
		if (active && active->type != BUF_SERVER) {
			if (active->type == BUF_CHANNEL) {
				irc_send_raw(conn, "PART %s", active->name);
			}
			state_remove_buffer(st, active->name);
		}
	} else if (strcasecmp(buf, "clear") == 0) {
		if (active) {
			active->line_count = 0;
			active->line_head = 0;
		}
	} else if (strcasecmp(buf, "msg") == 0 || strcasecmp(buf, "query") == 0) {
		if (!arg) {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /msg <target> [message]", false);
			return true;
		}
		char *text = strchr(arg, ' ');
		if (!text) {
			switch_to_buffer_ptr(st, state_get_or_create_buffer(st, arg, arg[0] == '#' ? BUF_CHANNEL : BUF_QUERY));
			return true;
		}
		*text++ = '\0';
		while (*text == ' ') {
			text++;
		}
		irc_send_privmsg(conn, arg, text);
		Buffer *q = state_get_or_create_buffer(st, arg, arg[0] == '#' ? BUF_CHANNEL : BUF_QUERY);
		buffer_push_line(q, LINE_MSG, st->nick, text, false);
	} else if (strcasecmp(buf, "notice") == 0) {
		if (!arg) {
			return true;
		}
		char *text = strchr(arg, ' ');
		if (!text) {
			return true;
		}
		*text++ = '\0';
		while (*text == ' ') {
			text++;
		}
		irc_send_raw(conn, "NOTICE %s :%s", arg, text);
		buffer_push_line(active, LINE_EVENT, "", text, false);
	} else if (strcasecmp(buf, "me") == 0) {
		if (!arg || !active || active->type == BUF_SERVER) {
			return true;
		}
		irc_send_raw(conn, "PRIVMSG %s :\001ACTION %s\001", active->name, arg);
		buffer_push_line(active, LINE_ACTION, st->nick, arg, false);
	} else if (strcasecmp(buf, "nick") == 0) {
		if (!arg) {
			return true;
		}
		irc_send_raw(conn, "NICK %s", arg);
	} else if (strcasecmp(buf, "whois") == 0) {
		if (!arg) {
			return true;
		}
		irc_send_raw(conn, "WHOIS %s", arg);
	} else if (strcasecmp(buf, "topic") == 0) {
		if (!active || active->type != BUF_CHANNEL) {
			return true;
		}
		if (arg) {
			irc_send_raw(conn, "TOPIC %s :%s", active->name, arg);
		} else {
			irc_send_raw(conn, "TOPIC %s", active->name);
		}
	} else if (strcasecmp(buf, "kick") == 0) {
		if (!arg || !active || active->type != BUF_CHANNEL) {
			return true;
		}
		char *reason = strchr(arg, ' ');
		if (reason) {
			*reason++ = '\0';
			irc_send_raw(conn, "KICK %s %s :%s", active->name, arg, reason);
		} else {
			irc_send_raw(conn, "KICK %s %s", active->name, arg);
		}
	} else if (strcasecmp(buf, "mode") == 0) {
		if (!arg) {
			return true;
		}
		irc_send_raw(conn, "MODE %s", arg);
	} else if (strcasecmp(buf, "names") == 0) {
		if (!active || active->type != BUF_CHANNEL) {
			return true;
		}
		char list[STATE_TEXT_LEN] = "Users: ";
		size_t pos = strlen(list);
		for (size_t i = 0; i < active->user_count && pos + 40 < sizeof(list); i++) {
			if (active->users[i].prefix) {
				list[pos++] = active->users[i].prefix;
			}
			pos += (size_t)snprintf(list + pos, sizeof(list) - pos, "%s ", active->users[i].nick);
		}
		buffer_push_line(active, LINE_EVENT, "", list, false);
	} else if (strcasecmp(buf, "raw") == 0 || strcasecmp(buf, "quote") == 0) {
		if (!arg) {
			return true;
		}
		irc_send_raw(conn, "%s", arg);
	} else if (strcasecmp(buf, "reload") == 0) {
		if (lua && lua_reload(lua)) {
			buffer_push_line(active, LINE_EVENT, "", "Lua configuration reloaded", false);
		} else {
			buffer_push_line(active, LINE_ERROR, "", "Failed to reload Lua configuration", false);
		}
	} else if (strcasecmp(buf, "lua") == 0) {
		if (!arg || !lua) {
			return true;
		}
		lua_exec_string(lua, arg);
	} else if (strcasecmp(buf, "quit") == 0) {
		irc_send_raw(conn, "QUIT :%s", arg ? arg : "lic - Lunar IRC Client");
		return false;
	} else {
		if (lua && lua_emit_command(lua, buf, arg ? arg : "")) {
			return true;
		}
		buffer_push_line(active, LINE_ERROR, "", "Unknown command", false);
	}

	return true;
}
