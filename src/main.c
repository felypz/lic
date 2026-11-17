#include "cmd.h"
#include "irc.h"
#include "lua_api.h"
#include "net.h"
#include "state.h"
#include "ui.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void base64_encode(const char *src, size_t len, char *dst, size_t dst_size) {
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i = 0, j = 0;
	while (i < len && j + 4 < dst_size) {
		unsigned int a = (unsigned char)src[i++];
		unsigned int b = i < len ? (unsigned char)src[i++] : 0;
		unsigned int c = i < len ? (unsigned char)src[i++] : 0;
		unsigned int triple = (a << 16) | (b << 8) | c;

		dst[j++] = tbl[(triple >> 18) & 0x3F];
		dst[j++] = tbl[(triple >> 12) & 0x3F];
		dst[j++] = (i > len + 1) ? '=' : tbl[(triple >> 6) & 0x3F];
		dst[j++] = (i > len) ? '=' : tbl[triple & 0x3F];
	}
	dst[j] = '\0';
}

static bool contains_nick(const char *text, const char *nick) {
	if (!text || !nick || nick[0] == '\0') {
		return false;
	}
	size_t nlen = strlen(nick);
	for (const char *p = text; *p; p++) {
		if (strncasecmp(p, nick, nlen) == 0) {
			return true;
		}
	}
	return false;
}

static void dispatch_irc_message(AppState *st, NetConn *conn, struct LuaEngine *lua, IrcMessage *msg) {
	if (strcmp(msg->command, "PING") == 0 && msg->param_count > 0) {
		irc_send_raw(conn, "PONG :%s", msg->params[0]);
		return;
	}

	if (strcmp(msg->command, "CAP") == 0 && msg->param_count >= 2) {
		if (strcmp(msg->params[1], "LS") == 0) {
			if (lua->config.sasl_user[0] != '\0') {
				irc_send_raw(conn, "CAP REQ :server-time message-tags sasl");
			} else {
				irc_send_raw(conn, "CAP REQ :server-time message-tags");
			}
		} else if (strcmp(msg->params[1], "ACK") == 0) {
			if (msg->param_count >= 3 && strstr(msg->params[2], "sasl")) {
				irc_send_raw(conn, "AUTHENTICATE PLAIN");
			} else {
				irc_send_raw(conn, "CAP END");
			}
		} else if (strcmp(msg->params[1], "NAK") == 0) {
			irc_send_raw(conn, "CAP END");
		}
		return;
	}

	if (strcmp(msg->command, "AUTHENTICATE") == 0 && msg->param_count >= 1 && strcmp(msg->params[0], "+") == 0) {
		char raw_auth[256];
		size_t ulen = strlen(lua->config.sasl_user);
		size_t plen = strlen(lua->config.sasl_pass);
		if (ulen * 2 + plen + 2 < sizeof(raw_auth)) {
			size_t pos = 0;
			memcpy(raw_auth + pos, lua->config.sasl_user, ulen + 1);
			pos += ulen + 1;
			memcpy(raw_auth + pos, lua->config.sasl_user, ulen + 1);
			pos += ulen + 1;
			memcpy(raw_auth + pos, lua->config.sasl_pass, plen);
			pos += plen;

			char encoded[512];
			base64_encode(raw_auth, pos, encoded, sizeof(encoded));
			irc_send_raw(conn, "AUTHENTICATE %s", encoded);
		}
		return;
	}

	if (strcmp(msg->command, "903") == 0 || strcmp(msg->command, "904") == 0) {
		irc_send_raw(conn, "CAP END");
	}

	if (lua_emit_message(lua, msg)) {
		return;
	}

	Buffer *srv = &st->buffers[0];

	if (strcmp(msg->command, "001") == 0) {
		if (msg->param_count >= 1) {
			snprintf(st->nick, sizeof(st->nick), "%s", msg->params[0]);
		}
		if (msg->param_count >= 2) {
			buffer_push_line(srv, LINE_EVENT, "", msg->params[1], false);
		}
		for (size_t i = 0; i < lua->config.auto_join_count; i++) {
			irc_send_join(conn, lua->config.auto_join[i]);
			state_get_or_create_buffer(st, lua->config.auto_join[i], BUF_CHANNEL);
		}
	} else if (strcmp(msg->command, "PRIVMSG") == 0 && msg->param_count >= 2) {
		const char *target = msg->params[0];
		const char *text = msg->params[1];
		const char *sender = msg->nick ? msg->nick : "?";

		bool is_channel = (target[0] == '#' || target[0] == '&');
		const char *buf_name = is_channel ? target : sender;
		Buffer *b = state_get_or_create_buffer(st, buf_name, is_channel ? BUF_CHANNEL : BUF_QUERY);

		bool hl = !is_channel || contains_nick(text, st->nick);
		size_t tlen = strlen(text);

		if (tlen > 8 && strncmp(text, "\001ACTION ", 8) == 0) {
			char act[STATE_TEXT_LEN];
			snprintf(act, sizeof(act), "%s", text + 8);
			size_t alen = strlen(act);
			if (alen > 0 && act[alen - 1] == '\001') {
				act[alen - 1] = '\0';
			}
			buffer_push_line(b, LINE_ACTION, sender, act, hl);
		} else {
			buffer_push_line(b, LINE_MSG, sender, text, hl);
		}

		if (b != state_active_buffer(st)) {
			b->unread = true;
			if (hl) {
				b->mention = true;
			}
		}
	} else if (strcmp(msg->command, "JOIN") == 0 && msg->param_count >= 1) {
		const char *chan = msg->params[0];
		const char *who = msg->nick ? msg->nick : "?";
		Buffer *b = state_get_or_create_buffer(st, chan, BUF_CHANNEL);
		buffer_add_user(b, who, '\0');

		char ev[STATE_TEXT_LEN];
		snprintf(ev, sizeof(ev), "%s joined %s", who, chan);
		buffer_push_line(b, LINE_EVENT, "", ev, false);
	} else if (strcmp(msg->command, "PART") == 0 && msg->param_count >= 1) {
		const char *chan = msg->params[0];
		const char *who = msg->nick ? msg->nick : "?";
		if (strcasecmp(who, st->nick) == 0) {
			state_remove_buffer(st, chan);
		} else {
			Buffer *b = state_get_or_create_buffer(st, chan, BUF_CHANNEL);
			buffer_remove_user(b, who);
			char ev[STATE_TEXT_LEN];
			snprintf(ev, sizeof(ev), "%s left %s", who, chan);
			buffer_push_line(b, LINE_EVENT, "", ev, false);
		}
	} else if (strcmp(msg->command, "QUIT") == 0) {
		const char *who = msg->nick ? msg->nick : "?";
		state_remove_user_everywhere(st, who);
		char ev[STATE_TEXT_LEN];
		snprintf(ev, sizeof(ev), "%s quit (%s)", who, msg->param_count >= 1 ? msg->params[0] : "");
		buffer_push_line(srv, LINE_EVENT, "", ev, false);
	} else if (strcmp(msg->command, "NICK") == 0 && msg->param_count >= 1) {
		const char *old_n = msg->nick ? msg->nick : "?";
		const char *new_n = msg->params[0];
		if (strcasecmp(old_n, st->nick) == 0) {
			snprintf(st->nick, sizeof(st->nick), "%s", new_n);
		}
		state_rename_user_everywhere(st, old_n, new_n);
		char ev[STATE_TEXT_LEN];
		snprintf(ev, sizeof(ev), "%s is now known as %s", old_n, new_n);
		buffer_push_line(srv, LINE_EVENT, "", ev, false);
	} else if (strcmp(msg->command, "353") == 0 && msg->param_count >= 4) {
		Buffer *b = state_get_or_create_buffer(st, msg->params[2], BUF_CHANNEL);
		char names[STATE_TEXT_LEN];
		snprintf(names, sizeof(names), "%s", msg->params[3]);
		char *tok = strtok(names, " ");
		while (tok) {
			char prefix = '\0';
			if (*tok == '@' || *tok == '+' || *tok == '%' || *tok == '~' || *tok == '&') {
				prefix = *tok++;
			}
			buffer_add_user(b, tok, prefix);
			tok = strtok(NULL, " ");
		}
	} else if (strcmp(msg->command, "332") == 0 && msg->param_count >= 3) {
		Buffer *b = state_get_or_create_buffer(st, msg->params[1], BUF_CHANNEL);
		snprintf(b->topic, sizeof(b->topic), "%s", msg->params[2]);
		char ev[STATE_TEXT_LEN];
		snprintf(ev, sizeof(ev), "Topic: %s", msg->params[2]);
		buffer_push_line(b, LINE_EVENT, "", ev, false);
	} else if (msg->param_count >= 2) {
		buffer_push_line(srv, LINE_EVENT, "", msg->params[msg->param_count - 1], false);
	}
}

int main(void) {
	AppState *state = calloc(1, sizeof(AppState));
	if (!state) {
		fprintf(stderr, "lic: out of memory\n");
		return 1;
	}

	NetConn conn = {0};
	struct LuaEngine lua;
	UiState ui;

	state_init(state, "irc.libera.chat", "lic_test", 1024);
	lua_engine_init(&lua, state, &conn, "config/init.lua");
	state_init(state, lua.config.host, lua.config.nick, lua.config.history_size);

	if (!ui_init(&ui)) {
		fprintf(stderr, "lic: failed to initialize terminal UI\n");
		state_free(state);
		free(state);
		return 1;
	}

	Buffer *srv = state_active_buffer(state);
	buffer_push_line(srv, LINE_EVENT, "", "Connecting...", false);
	ui_render(&ui, state);

	if (!net_connect(&conn, lua.config.host, lua.config.port, lua.config.tls)) {
		buffer_push_line(srv, LINE_ERROR, "", "Connection failed", false);
		ui_render(&ui, state);
		sleep(2);
		lua_engine_close(&lua);
		state_free(state);
		free(state);
		return 1;
	}

	irc_send_handshake(&conn, lua.config.nick, lua.config.user, lua.config.realname);

	IrcLineBuf line_buf = {0};
	struct pollfd pfds[2] = {
		{.fd = STDIN_FILENO, .events = POLLIN},
		{.fd = conn.fd,      .events = POLLIN}
	};

	char raw_buf[4096];
	char line[IRC_MAX_LINE];
	char input_line[UI_INPUT_MAX];
	bool running = true;

	while (running && conn.connected) {
		if (poll(pfds, 2, 100) < 0) {
			if (ui.dirty) {
				ui_render(&ui, state);
			}
			continue;
		}

		if (pfds[1].revents & POLLIN) {
			ssize_t n = net_read(&conn, raw_buf, sizeof(raw_buf));
			if (n < 0) {
				buffer_push_line(srv, LINE_ERROR, "", "Disconnected from server", false);
				ui.dirty = true;
				break;
			}
			if (n > 0) {
				irc_buf_push(&line_buf, raw_buf, (size_t)n);
				while (irc_buf_pop_line(&line_buf, line, sizeof(line))) {
					IrcMessage msg;
					if (irc_parse_line(line, &msg)) {
						dispatch_irc_message(state, &conn, &lua, &msg);
						ui.dirty = true;
					}
				}
			}
		}

		if (pfds[0].revents & POLLIN) {
			UiAction act = ui_handle_stdin(&ui, state, &lua, input_line, sizeof(input_line));
			if (act == UI_ACTION_QUIT) {
				irc_send_raw(&conn, "QUIT :lic");
				running = false;
			} else if (act == UI_ACTION_NEXT_BUF) {
				state_next_buffer(state);
			} else if (act == UI_ACTION_PREV_BUF) {
				state_prev_buffer(state);
			} else if (act == UI_ACTION_SWITCH_BUF) {
				state_switch_buffer(state, ui.target_buf_idx);
			} else if (act == UI_ACTION_SUBMIT) {
				if (!cmd_execute(state, &conn, &lua, input_line)) {
					running = false;
				}
			}
		}

		if (ui.dirty) {
			ui_render(&ui, state);
		}
	}

	net_close(&conn);
	lua_engine_close(&lua);
	state_free(state);
	free(state);
	return 0;
}
