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
#include <time.h>
#include <unistd.h>

#define PING_IDLE_SEC    90
#define PING_TIMEOUT_SEC 30
#define BACKOFF_MIN_SEC  1
#define BACKOFF_MAX_SEC  60

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

static bool prompt_for_nick(UiState *ui, AppState *st, const LicTheme *theme, char *out_nick, size_t max_len) {
	Buffer *srv = state_active_buffer(st);
	buffer_push_line(srv, LINE_EVENT, "", "No nickname configured. Enter your nickname below:", false);
	snprintf(ui->prompt_label, sizeof(ui->prompt_label), "nickname");
	ui_render(ui, st, theme);

	struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
	char line[UI_INPUT_MAX];

	while (true) {
		if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
			UiAction act = ui_handle_stdin(ui, st, NULL, line, sizeof(line));
			if (act == UI_ACTION_QUIT) {
				return false;
			}
			if (act == UI_ACTION_SUBMIT && line[0] != '\0') {
				snprintf(out_nick, max_len, "%.*s", (int)(max_len - 1), line);
				ui->prompt_label[0] = '\0';
				return true;
			}
		}
		if (ui->dirty) {
			ui_render(ui, st, theme);
		}
	}
}

static void rejoin_open_channels(AppState *st, NetConn *conn, const struct LuaEngine *lua) {
	for (size_t i = 0; i < st->buffer_count; i++) {
		if (st->buffers[i].type == BUF_CHANNEL) {
			irc_send_join(conn, st->buffers[i].name);
		}
	}
	for (size_t i = 0; i < lua->config.auto_join_count; i++) {
		bool already = false;
		for (size_t j = 0; j < st->buffer_count; j++) {
			if (st->buffers[j].type == BUF_CHANNEL &&
			    strcasecmp(st->buffers[j].name, lua->config.auto_join[i]) == 0) {
				already = true;
				break;
			}
		}
		if (!already) {
			irc_send_join(conn, lua->config.auto_join[i]);
			state_get_or_create_buffer(st, lua->config.auto_join[i], BUF_CHANNEL);
		}
	}
}

static bool process_ui_action(UiAction act, AppState *state, NetConn *conn, UiState *ui,
                              struct LuaEngine *lua, const char *input_line) {
	if (act == UI_ACTION_QUIT) {
		if (conn && conn->connected) {
			irc_send_raw(conn, "QUIT :lic");
		}
		return false;
	}
	if (act == UI_ACTION_NEXT_BUF) {
		state_next_buffer(state);
	} else if (act == UI_ACTION_PREV_BUF) {
		state_prev_buffer(state);
	} else if (act == UI_ACTION_SWITCH_BUF) {
		state_switch_buffer(state, ui->target_buf_idx);
	} else if (act == UI_ACTION_SUBMIT) {
		return cmd_execute(state, conn, lua, input_line);
	} else if (act == UI_ACTION_VIM_CMD) {
		return cmd_execute_vim(state, conn, ui, lua, input_line);
	}
	return true;
}

static void dispatch_irc_message(AppState *st, NetConn *conn, struct LuaEngine *lua, IrcMessage *msg, int *backoff_sec) {
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
		*backoff_sec = BACKOFF_MIN_SEC;
		if (msg->param_count >= 1) {
			snprintf(st->nick, sizeof(st->nick), "%s", msg->params[0]);
		}
		if (msg->param_count >= 2) {
			buffer_push_line(srv, LINE_EVENT, "", msg->params[1], false);
		}
		rejoin_open_channels(st, conn, lua);
	} else if (strcmp(msg->command, "433") == 0) {
		size_t len = strlen(st->nick);
		if (len + 1 < sizeof(st->nick)) {
			st->nick[len] = '_';
			st->nick[len + 1] = '\0';
			irc_send_raw(conn, "NICK %s", st->nick);
			buffer_push_line(srv, LINE_EVENT, "", "Nickname in use, retrying with underscore", false);
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
	} else if (strcmp(msg->command, "NOTICE") == 0 && msg->param_count >= 2) {
		char ev[STATE_TEXT_LEN];
		snprintf(ev, sizeof(ev), "-%s- %s", msg->nick ? msg->nick : "server", msg->params[1]);
		buffer_push_line(state_active_buffer(st), LINE_EVENT, "", ev, false);
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
	} else if (strcmp(msg->command, "KICK") == 0 && msg->param_count >= 2) {
		const char *chan = msg->params[0];
		const char *kicked = msg->params[1];
		const char *by = msg->nick ? msg->nick : "?";
		const char *reason = msg->param_count >= 3 ? msg->params[2] : "";
		if (strcasecmp(kicked, st->nick) == 0) {
			state_remove_buffer(st, chan);
			char ev[STATE_TEXT_LEN];
			snprintf(ev, sizeof(ev), "Kicked from %s by %s (%s)", chan, by, reason);
			buffer_push_line(srv, LINE_ERROR, "", ev, true);
		} else {
			Buffer *b = state_get_or_create_buffer(st, chan, BUF_CHANNEL);
			buffer_remove_user(b, kicked);
			char ev[STATE_TEXT_LEN];
			snprintf(ev, sizeof(ev), "%s was kicked by %s (%s)", kicked, by, reason);
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
	} else if (strcmp(msg->command, "TOPIC") == 0 && msg->param_count >= 2) {
		Buffer *b = state_get_or_create_buffer(st, msg->params[0], BUF_CHANNEL);
		snprintf(b->topic, sizeof(b->topic), "%s", msg->params[1]);
		char ev[STATE_TEXT_LEN];
		snprintf(ev, sizeof(ev), "Topic changed to: %s", msg->params[1]);
		buffer_push_line(b, LINE_EVENT, "", ev, false);
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

	state_init(state, "irc.libera.chat", "", 1024);
	lua_engine_init(&lua, state, &conn, "config/init.lua", "config/plugins");
	state_init(state, lua.config.host, lua.config.nick, lua.config.history_size);

	if (!ui_init(&ui)) {
		fprintf(stderr, "lic: failed to initialize terminal UI\n");
		state_free(state);
		free(state);
		return 1;
	}

	if (lua.config.nick[0] == '\0') {
		if (!prompt_for_nick(&ui, state, &lua.config.theme, lua.config.nick, sizeof(lua.config.nick))) {
			lua_engine_close(&lua);
			state_free(state);
			free(state);
			return 0;
		}
		snprintf(state->nick, sizeof(state->nick), "%s", lua.config.nick);
	}

	Buffer *srv = &state->buffers[0];
	char raw_buf[4096];
	char line[IRC_MAX_LINE];
	char input_line[UI_INPUT_MAX];
	int backoff_sec = BACKOFF_MIN_SEC;
	bool running = true;

	while (running) {
		char msg_buf[256];
		snprintf(msg_buf, sizeof(msg_buf), "Connecting to %s:%s...", lua.config.host, lua.config.port);
		buffer_push_line(srv, LINE_EVENT, "", msg_buf, false);
		ui_render(&ui, state, &lua.config.theme);

		if (!net_connect(&conn, lua.config.host, lua.config.port, lua.config.tls)) {
			snprintf(msg_buf, sizeof(msg_buf), "Connection failed. Reconnecting in %ds...", backoff_sec);
			buffer_push_line(srv, LINE_ERROR, "", msg_buf, false);
			ui.dirty = true;

			time_t wait_until = time(NULL) + backoff_sec;
			while (running && time(NULL) < wait_until) {
				struct pollfd spfd = {.fd = STDIN_FILENO, .events = POLLIN};
				if (poll(&spfd, 1, 100) > 0 && (spfd.revents & POLLIN)) {
					UiAction act = ui_handle_stdin(&ui, state, &lua, input_line, sizeof(input_line));
					running = process_ui_action(act, state, &conn, &ui, &lua, input_line);
				}
				lua_tick_timers(&lua, &ui.dirty);
				if (ui.dirty) {
					ui_render(&ui, state, &lua.config.theme);
				}
			}

			backoff_sec *= 2;
			if (backoff_sec > BACKOFF_MAX_SEC) {
				backoff_sec = BACKOFF_MAX_SEC;
			}
			continue;
		}

		irc_send_handshake(&conn, lua.config.nick, lua.config.user, lua.config.realname);

		IrcLineBuf line_buf = {0};
		time_t last_rx = time(NULL);
		time_t ping_sent_at = 0;

		while (running && conn.connected) {
			struct pollfd pfds[2] = {
				{.fd = STDIN_FILENO, .events = POLLIN},
				{.fd = conn.fd,      .events = POLLIN}
			};

			if (poll(pfds, 2, 100) < 0) {
				lua_tick_timers(&lua, &ui.dirty);
				if (ui.dirty) {
					ui_render(&ui, state, &lua.config.theme);
				}
				continue;
			}

			time_t now = time(NULL);

			if (pfds[1].revents & POLLIN) {
				ssize_t n = net_read(&conn, raw_buf, sizeof(raw_buf));
				if (n < 0) {
					buffer_push_line(srv, LINE_ERROR, "", "Disconnected from server", false);
					ui.dirty = true;
					break;
				}
				if (n > 0) {
					last_rx = now;
					ping_sent_at = 0;
					irc_buf_push(&line_buf, raw_buf, (size_t)n);
					while (irc_buf_pop_line(&line_buf, line, sizeof(line))) {
						IrcMessage msg;
						if (irc_parse_line(line, &msg)) {
							dispatch_irc_message(state, &conn, &lua, &msg, &backoff_sec);
							ui.dirty = true;
						}
					}
				}
			}

			if (pfds[0].revents & POLLIN) {
				UiAction act = ui_handle_stdin(&ui, state, &lua, input_line, sizeof(input_line));
				running = process_ui_action(act, state, &conn, &ui, &lua, input_line);
			}

			if (ping_sent_at == 0 && (now - last_rx) >= PING_IDLE_SEC) {
				irc_send_raw(&conn, "PING :lic-keepalive");
				ping_sent_at = now;
			} else if (ping_sent_at != 0 && (now - ping_sent_at) >= PING_TIMEOUT_SEC) {
				buffer_push_line(srv, LINE_ERROR, "", "Ping timeout from server", false);
				ui.dirty = true;
				break;
			}

			lua_tick_timers(&lua, &ui.dirty);

			if (ui.dirty) {
				ui_render(&ui, state, &lua.config.theme);
			}
		}

		net_close(&conn);

		if (running) {
			snprintf(msg_buf, sizeof(msg_buf), "Reconnecting in %ds...", backoff_sec);
			buffer_push_line(srv, LINE_EVENT, "", msg_buf, false);
			ui.dirty = true;

			time_t wait_until = time(NULL) + backoff_sec;
			while (running && time(NULL) < wait_until) {
				struct pollfd spfd = {.fd = STDIN_FILENO, .events = POLLIN};
				if (poll(&spfd, 1, 100) > 0 && (spfd.revents & POLLIN)) {
					UiAction act = ui_handle_stdin(&ui, state, &lua, input_line, sizeof(input_line));
					running = process_ui_action(act, state, &conn, &ui, &lua, input_line);
				}
				lua_tick_timers(&lua, &ui.dirty);
				if (ui.dirty) {
					ui_render(&ui, state, &lua.config.theme);
				}
			}

			backoff_sec *= 2;
			if (backoff_sec > BACKOFF_MAX_SEC) {
				backoff_sec = BACKOFF_MAX_SEC;
			}
		}
	}

	lua_engine_close(&lua);
	state_free(state);
	free(state);
	return 0;
}
