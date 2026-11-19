#include "cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool is_utf8_cont(unsigned char c) {
	return (c & 0xC0) == 0x80;
}

static size_t safe_utf8_split_len(const char *s, size_t max_bytes) {
	size_t len = strlen(s);
	if (len <= max_bytes) {
		return len;
	}

	size_t cut = max_bytes;
	while (cut > 0 && is_utf8_cont((unsigned char)s[cut])) {
		cut--;
	}
	if (cut == 0) {
		cut = max_bytes;
	}

	for (size_t i = cut; i > 0; i--) {
		if (s[i] == ' ') {
			return i;
		}
	}
	return cut;
}

static void send_chunked_privmsg(AppState *st, NetConn *conn, const char *target, const char *text) {
	if (!target || !text || text[0] == '\0') {
		return;
	}

	/*
	 * Reserva 120 bytes para o prefixo retransmitido pelo servidor
	 * (:nick!user@host PRIVMSG <target> :<text>\r\n <= 512 bytes)
	 */
	size_t overhead = strlen("PRIVMSG  :\r\n") + strlen(target) + 120;
	size_t max_payload = overhead < 480 ? (510 - overhead) : 256;

	Buffer *b = state_get_or_create_buffer(st, target,
	                                       (target[0] == '#' || target[0] == '&') ? BUF_CHANNEL : BUF_QUERY);

	const char *p = text;
	while (*p != '\0') {
		size_t take = safe_utf8_split_len(p, max_payload);
		char chunk[512];
		snprintf(chunk, sizeof(chunk), "%.*s", (int)take, p);

		irc_send_privmsg(conn, target, chunk);
		buffer_push_line(b, LINE_MSG, st->nick, chunk, false);

		p += take;
		while (*p == ' ') {
			p++;
		}
	}
}

static void split_first_arg(const char *input, char *arg1, size_t arg1_size, const char **rest) {
	while (*input == ' ') {
		input++;
	}
	size_t i = 0;
	while (*input && *input != ' ' && i + 1 < arg1_size) {
		arg1[i++] = *input++;
	}
	arg1[i] = '\0';
	while (*input == ' ') {
		input++;
	}
	*rest = input;
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
		if (!conn->connected) {
			buffer_push_line(active, LINE_ERROR, "", "Not connected", false);
			return true;
		}
		send_chunked_privmsg(st, conn, active->name, input);
		return true;
	}

	char cmd[32];
	const char *args = NULL;
	split_first_arg(input + 1, cmd, sizeof(cmd), &args);

	if (cmd[0] == '\0') {
		return true;
	}

	if (lua_emit_command(lua, cmd, args)) {
		return true;
	}

	if (strcasecmp(cmd, "quit") == 0) {
		const char *reason = args[0] ? args : "lic";
		if (conn->connected) {
			irc_send_raw(conn, "QUIT :%s", reason);
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
		Buffer *b = state_get_or_create_buffer(st, chan, BUF_CHANNEL);
		for (size_t i = 0; i < st->buffer_count; i++) {
			if (&st->buffers[i] == b) {
				state_switch_buffer(st, i);
				break;
			}
		}
		return true;
	}

	if (strcasecmp(cmd, "part") == 0 || strcasecmp(cmd, "leave") == 0) {
		char chan[STATE_NAME_LEN];
		const char *reason = NULL;
		if (args[0] != '\0') {
			split_first_arg(args, chan, sizeof(chan), &reason);
		} else if (active && active->type == BUF_CHANNEL) {
			snprintf(chan, sizeof(chan), "%s", active->name);
			reason = "";
		} else {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /part [#channel] [reason]", false);
			return true;
		}
		if (reason && reason[0] != '\0') {
			irc_send_raw(conn, "PART %s :%s", chan, reason);
		} else {
			irc_send_raw(conn, "PART %s", chan);
		}
		state_remove_buffer(st, chan);
		return true;
	}

	if (strcasecmp(cmd, "msg") == 0 || strcasecmp(cmd, "privmsg") == 0) {
		char target[STATE_NAME_LEN];
		const char *text = NULL;
		split_first_arg(args, target, sizeof(target), &text);
		if (target[0] == '\0' || text[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /msg <target> <message>", false);
			return true;
		}
		send_chunked_privmsg(st, conn, target, text);
		return true;
	}

	if (strcasecmp(cmd, "query") == 0 || strcasecmp(cmd, "q") == 0) {
		char target[STATE_NAME_LEN];
		const char *text = NULL;
		split_first_arg(args, target, sizeof(target), &text);
		if (target[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /query <nick> [message]", false);
			return true;
		}
		Buffer *b = state_get_or_create_buffer(st, target, BUF_QUERY);
		for (size_t i = 0; i < st->buffer_count; i++) {
			if (&st->buffers[i] == b) {
				state_switch_buffer(st, i);
				break;
			}
		}
		if (text[0] != '\0') {
			send_chunked_privmsg(st, conn, target, text);
		}
		return true;
	}

	if (strcasecmp(cmd, "me") == 0) {
		if (!active || active->type == BUF_SERVER || args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /me <action>", false);
			return true;
		}
		irc_send_raw(conn, "PRIVMSG %s :\001ACTION %s\001", active->name, args);
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
		const char *target = args[0] ? args : st->nick;
		irc_send_raw(conn, "WHOIS %s", target);
		return true;
	}

	if (strcasecmp(cmd, "topic") == 0) {
		if (!active || active->type != BUF_CHANNEL) {
			buffer_push_line(active, LINE_ERROR, "", "Not in a channel", false);
			return true;
		}
		if (args[0] == '\0') {
			irc_send_raw(conn, "TOPIC %s", active->name);
		} else {
			irc_send_raw(conn, "TOPIC %s :%s", active->name, args);
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
			buffer_push_line(active, LINE_ERROR, "", "Usage: /mode <target> <modes>", false);
			return true;
		}
		irc_send_raw(conn, "MODE %s", args);
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
		char ev[STATE_TEXT_LEN];
		snprintf(ev, sizeof(ev), "-> -%s- %s", target, text);
		buffer_push_line(active, LINE_EVENT, "", ev, false);
		return true;
	}

	if (strcasecmp(cmd, "names") == 0) {
		if (!active || active->type != BUF_CHANNEL) {
			buffer_push_line(active, LINE_ERROR, "", "Not in a channel", false);
			return true;
		}
		char list[STATE_TEXT_LEN];
		int pos = snprintf(list, sizeof(list), "Users (%zu): ", active->user_count);
		for (size_t i = 0; i < active->user_count && pos < (int)sizeof(list) - 32; i++) {
			if (active->users[i].prefix) {
				pos += snprintf(list + pos, sizeof(list) - (size_t)pos, "%c%s ",
				                active->users[i].prefix, active->users[i].nick);
			} else {
				pos += snprintf(list + pos, sizeof(list) - (size_t)pos, "%s ", active->users[i].nick);
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

	if (strcasecmp(cmd, "close") == 0) {
		if (!active || active->type == BUF_SERVER) {
			buffer_push_line(active, LINE_ERROR, "", "Cannot close server buffer", false);
			return true;
		}
		if (active->type == BUF_CHANNEL && conn->connected) {
			irc_send_raw(conn, "PART %s", active->name);
		}
		state_remove_buffer(st, active->name);
		return true;
	}

	if (strcasecmp(cmd, "raw") == 0 || strcasecmp(cmd, "quote") == 0) {
		if (args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /raw <IRC command>", false);
			return true;
		}
		irc_send_raw(conn, "%s", args);
		return true;
	}

	if (strcasecmp(cmd, "reload") == 0) {
		if (lua_reload(lua)) {
			buffer_push_line(active, LINE_EVENT, "", "Lua configuration reloaded", false);
		}
		return true;
	}

	if (strcasecmp(cmd, "lua") == 0) {
		if (args[0] == '\0') {
			buffer_push_line(active, LINE_ERROR, "", "Usage: /lua <code>", false);
			return true;
		}
		lua_exec_string(lua, args);
		return true;
	}

	char err[128];
	snprintf(err, sizeof(err), "Unknown command: /%s", cmd);
	buffer_push_line(active, LINE_ERROR, "", err, false);
	return true;
}
