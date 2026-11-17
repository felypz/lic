#include "ui.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig_termios;
static bool termios_saved = false;
static volatile sig_atomic_t resized_flag = 0;

static const char *builtin_commands[] = {
	"join", "part", "msg", "query", "me", "nick", "whois", "topic",
	"kick", "mode", "notice", "names", "clear", "close", "raw", "reload", "lua", "quit"
};

static void handle_sigwinch(int sig) {
	(void)sig;
	resized_flag = 1;
}

void ui_shutdown(void) {
	if (termios_saved) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
		termios_saved = false;
	}
	const char *cleanup = "\x1b[?25h\x1b[0m\x1b[?1049l";
	write(STDOUT_FILENO, cleanup, strlen(cleanup));
}

void ui_update_size(UiState *ui) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
		ui->rows = ws.ws_row;
		ui->cols = ws.ws_col;
	} else {
		ui->rows = 24;
		ui->cols = 80;
	}
	ui->dirty = true;
}

bool ui_init(UiState *ui) {
	memset(ui, 0, sizeof(*ui));

	if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
		return false;
	}
	termios_saved = true;
	atexit(ui_shutdown);

	struct termios raw = orig_termios;
	raw.c_iflag &= ~(unsigned int)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(unsigned int)(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(unsigned int)(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
		return false;
	}

	struct sigaction sa = {0};
	sa.sa_handler = handle_sigwinch;
	sigaction(SIGWINCH, &sa, NULL);

	const char *init_seq = "\x1b[?1049h\x1b[2J\x1b[H";
	write(STDOUT_FILENO, init_seq, strlen(init_seq));

	ui_update_size(ui);
	return true;
}

static unsigned int nick_color(const char *nick) {
	unsigned int hash = 5381;
	for (const char *p = nick; *p; p++) {
		hash = ((hash << 5) + hash) + (unsigned char)(*p);
	}
	static const unsigned int palette[] = {31, 32, 33, 34, 35, 36, 91, 92, 93, 94, 95, 96};
	return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

static const char *fg_seq(LicColor c, char *buf, size_t size) {
	if (c.kind == COLOR_RGB) {
		snprintf(buf, size, "\x1b[38;2;%u;%u;%um", c.r, c.g, c.b);
	} else if (c.kind == COLOR_ANSI) {
		snprintf(buf, size, "\x1b[38;5;%um", c.idx);
	} else {
		snprintf(buf, size, "\x1b[39m");
	}
	return buf;
}

static const char *bg_seq(LicColor c, char *buf, size_t size) {
	if (c.kind == COLOR_RGB) {
		snprintf(buf, size, "\x1b[48;2;%u;%u;%um", c.r, c.g, c.b);
	} else if (c.kind == COLOR_ANSI) {
		snprintf(buf, size, "\x1b[48;5;%um", c.idx);
	} else {
		snprintf(buf, size, "\x1b[49m");
	}
	return buf;
}

static void comp_add(UiState *ui, const char *item) {
	if (ui->comp_count >= UI_COMP_MAX) {
		return;
	}
	for (size_t i = 0; i < ui->comp_count; i++) {
		if (strcasecmp(ui->comp_matches[i], item) == 0) {
			return;
		}
	}
	snprintf(ui->comp_matches[ui->comp_count++], UI_COMP_STR_LEN, "%s", item);
}

static void apply_completion(UiState *ui) {
	if (!ui->comp_active || ui->comp_count == 0) {
		return;
	}
	const char *match = ui->comp_matches[ui->comp_idx];
	size_t mlen = strlen(match);
	size_t old_len = ui->comp_end - ui->comp_start;

	if (ui->input_len - old_len + mlen >= UI_INPUT_MAX) {
		return;
	}

	memmove(ui->input + ui->comp_start + mlen,
	        ui->input + ui->comp_end,
	        ui->input_len - ui->comp_end + 1);
	memcpy(ui->input + ui->comp_start, match, mlen);

	ui->input_len = ui->input_len - old_len + mlen;
	ui->comp_end = ui->comp_start + mlen;
	ui->cursor = ui->comp_end;
}

static void trigger_completion(UiState *ui, const AppState *st, struct LuaEngine *lua, bool reverse) {
	if (ui->comp_active && ui->comp_count > 0) {
		if (reverse) {
			ui->comp_idx = ui->comp_idx == 0 ? ui->comp_count - 1 : ui->comp_idx - 1;
		} else {
			ui->comp_idx = (ui->comp_idx + 1) % ui->comp_count;
		}
		apply_completion(ui);
		return;
	}

	size_t start = ui->cursor;
	while (start > 0 && !isspace((unsigned char)ui->input[start - 1])) {
		start--;
	}

	size_t prefix_len = ui->cursor - start;
	char prefix[UI_COMP_STR_LEN];
	if (prefix_len >= sizeof(prefix)) {
		return;
	}
	memcpy(prefix, ui->input + start, prefix_len);
	prefix[prefix_len] = '\0';

	ui->comp_count = 0;
	ui->comp_start = start;
	ui->comp_end = ui->cursor;
	ui->comp_idx = 0;

	if (start == 0 && prefix[0] == '/') {
		const char *cmd_prefix = prefix + 1;
		size_t clen = strlen(cmd_prefix);

		for (size_t i = 0; i < sizeof(builtin_commands) / sizeof(builtin_commands[0]); i++) {
			if (strncasecmp(builtin_commands[i], cmd_prefix, clen) == 0) {
				char full[UI_COMP_STR_LEN];
				snprintf(full, sizeof(full), "/%s ", builtin_commands[i]);
				comp_add(ui, full);
			}
		}

		const char *lua_cmds[64];
		size_t lcount = lua_list_commands(lua, lua_cmds, 64);
		for (size_t i = 0; i < lcount; i++) {
			if (strncasecmp(lua_cmds[i], cmd_prefix, clen) == 0) {
				char full[UI_COMP_STR_LEN];
				snprintf(full, sizeof(full), "/%s ", lua_cmds[i]);
				comp_add(ui, full);
			}
		}
	} else {
		const Buffer *buf = state_active_buffer((AppState *)st);
		if (buf) {
			for (size_t i = 0; i < buf->user_count; i++) {
				if (strncasecmp(buf->users[i].nick, prefix, prefix_len) == 0) {
					char full[UI_COMP_STR_LEN];
					if (start == 0) {
						snprintf(full, sizeof(full), "%s: ", buf->users[i].nick);
					} else {
						snprintf(full, sizeof(full), "%s ", buf->users[i].nick);
					}
					comp_add(ui, full);
				}
			}
		}
	}

	if (ui->comp_count > 0) {
		ui->comp_active = true;
		apply_completion(ui);
	}
}

static void delete_prev_word(UiState *ui) {
	if (ui->cursor == 0) {
		return;
	}
	size_t pos = ui->cursor;
	while (pos > 0 && isspace((unsigned char)ui->input[pos - 1])) {
		pos--;
	}
	while (pos > 0 && !isspace((unsigned char)ui->input[pos - 1])) {
		pos--;
	}
	memmove(ui->input + pos, ui->input + ui->cursor, ui->input_len - ui->cursor + 1);
	ui->input_len -= (ui->cursor - pos);
	ui->cursor = pos;
}

static size_t next_wrap_chunk(const char *text, size_t width) {
	size_t len = strlen(text);
	if (len <= width) {
		return len;
	}
	for (size_t i = width; i > 0; i--) {
		if (text[i] == ' ') {
			return i;
		}
	}
	return width;
}

void ui_render(UiState *ui, const AppState *st, const LicTheme *theme) {
	if (resized_flag) {
		resized_flag = 0;
		ui_update_size(ui);
	}

	const Buffer *buf = state_active_buffer((AppState *)st);
	if (!buf) {
		return;
	}

	char c_bar_bg[32], c_bar_fg[32], c_act_bg[32], c_act_fg[32];
	char c_unread[32], c_mention[32], c_ts[32], c_event[32], c_err[32], c_act[32], c_prompt[32];

	bg_seq(theme->bar_bg, c_bar_bg, sizeof(c_bar_bg));
	fg_seq(theme->bar_fg, c_bar_fg, sizeof(c_bar_fg));
	bg_seq(theme->active_bg, c_act_bg, sizeof(c_act_bg));
	fg_seq(theme->active_fg, c_act_fg, sizeof(c_act_fg));
	fg_seq(theme->unread, c_unread, sizeof(c_unread));
	fg_seq(theme->mention, c_mention, sizeof(c_mention));
	fg_seq(theme->timestamp, c_ts, sizeof(c_ts));
	fg_seq(theme->event, c_event, sizeof(c_event));
	fg_seq(theme->error, c_err, sizeof(c_err));
	fg_seq(theme->action, c_act, sizeof(c_act));
	fg_seq(theme->prompt, c_prompt, sizeof(c_prompt));

	char frame[131072];
	size_t pos = 0;

	#define EMIT(...) do { \
		if (pos < sizeof(frame) - 1) { \
			int w = snprintf(frame + pos, sizeof(frame) - pos, __VA_ARGS__); \
			if (w > 0) pos += (size_t)w; \
		} \
	} while (0)

	EMIT("\x1b[?25l\x1b[H");

	EMIT("%s%s", c_bar_bg, c_bar_fg);
	int top_used = 0;
	for (size_t i = 0; i < st->buffer_count; i++) {
		const Buffer *b = &st->buffers[i];
		char tag[STATE_NAME_LEN + 16];
		const char *flag = b->mention ? "!" : (b->unread ? "*" : "");
		int tlen = snprintf(tag, sizeof(tag), " %zu:%s%s ", i + 1, b->name, flag);
		if (top_used + tlen >= ui->cols) {
			break;
		}

		if (i == st->active_idx) {
			EMIT("\x1b[1m%s%s%s\x1b[0m%s%s", c_act_fg, c_act_bg, tag, c_bar_bg, c_bar_fg);
		} else if (b->mention) {
			EMIT("\x1b[1m%s%s\x1b[0m%s%s", c_mention, tag, c_bar_bg, c_bar_fg);
		} else if (b->unread) {
			EMIT("\x1b[1m%s%s\x1b[0m%s%s", c_unread, tag, c_bar_bg, c_bar_fg);
		} else {
			EMIT("%s", tag);
		}
		top_used += tlen;
	}
	while (top_used++ < ui->cols) {
		EMIT(" ");
	}
	EMIT("\x1b[0m\r\n");

	int chat_rows = ui->rows - 3;
	if (chat_rows < 1) {
		chat_rows = 1;
	}

	size_t total = buf->line_count;
	size_t end_idx = total > ui->scroll_offset ? total - ui->scroll_offset : 0;

	char visual_rows[256][1024];
	int vcount = 0;

	for (size_t i = end_idx; i > 0 && vcount < chat_rows; i--) {
		const ChatLine *cl = buffer_get_line(buf, i - 1);
		if (!cl) {
			continue;
		}

		struct tm *tm_info = localtime(&cl->timestamp);
		char ts[16];
		strftime(ts, sizeof(ts), "%H:%M", tm_info);

		int prefix_len = 6;
		if (cl->kind == LINE_MSG || cl->kind == LINE_ACTION) {
			prefix_len += (int)strlen(cl->sender) + 3;
		} else if (cl->kind == LINE_ERROR) {
			prefix_len += 2;
		} else {
			prefix_len += 3;
		}

		int text_width = ui->cols - prefix_len;
		if (text_width < 10) {
			text_width = 10;
		}

		const char *p = cl->text;
		char line_chunks[32][1024];
		int chunk_count = 0;

		if (*p == '\0') {
			line_chunks[chunk_count++][0] = '\0';
		} else {
			while (*p && chunk_count < 32) {
				size_t take = next_wrap_chunk(p, (size_t)text_width);
				snprintf(line_chunks[chunk_count++], sizeof(line_chunks[0]), "%.*s", (int)take, p);
				p += take;
				while (*p == ' ') {
					p++;
				}
			}
		}

		for (int c = chunk_count - 1; c >= 0 && vcount < chat_rows; c--) {
			if (c == 0) {
				if (cl->kind == LINE_MSG) {
					unsigned int col = nick_color(cl->sender);
					if (cl->highlight) {
						snprintf(visual_rows[vcount], sizeof(visual_rows[0]),
						         "%s%s\x1b[0m \x1b[1m%s<%s>\x1b[0m \x1b[1m%s\x1b[0m",
						         c_ts, ts, c_mention, cl->sender, line_chunks[c]);
					} else {
						snprintf(visual_rows[vcount], sizeof(visual_rows[0]),
						         "%s%s\x1b[0m \x1b[%um<%s>\x1b[0m %s",
						         c_ts, ts, col, cl->sender, line_chunks[c]);
					}
				} else if (cl->kind == LINE_ACTION) {
					snprintf(visual_rows[vcount], sizeof(visual_rows[0]),
					         "%s%s\x1b[0m %s* %s %s\x1b[0m",
					         c_ts, ts, c_act, cl->sender, line_chunks[c]);
				} else if (cl->kind == LINE_ERROR) {
					snprintf(visual_rows[vcount], sizeof(visual_rows[0]),
					         "%s%s\x1b[0m %s! %s\x1b[0m",
					         c_ts, ts, c_err, line_chunks[c]);
				} else {
					snprintf(visual_rows[vcount], sizeof(visual_rows[0]),
					         "%s%s\x1b[0m %s-- %s\x1b[0m",
					         c_ts, ts, c_event, line_chunks[c]);
				}
			} else {
				snprintf(visual_rows[vcount], sizeof(visual_rows[0]),
				         "%*s%s", prefix_len, "", line_chunks[c]);
			}
			vcount++;
		}
	}

	int drawn = 0;
	for (int i = vcount - 1; i >= 0; i--) {
		EMIT("\x1b[2K%s\r\n", visual_rows[i]);
		drawn++;
	}
	while (drawn < chat_rows) {
		EMIT("\x1b[2K\r\n");
		drawn++;
	}

	EMIT("%s%s", c_bar_bg, c_bar_fg);
	if (ui->comp_active && ui->comp_count > 1) {
		int wused = 0;
		for (size_t i = 0; i < ui->comp_count; i++) {
			char item[UI_COMP_STR_LEN + 4];
			int ilen = snprintf(item, sizeof(item), " %s ", ui->comp_matches[i]);
			if (wused + ilen >= ui->cols) {
				break;
			}
			if (i == ui->comp_idx) {
				EMIT("\x1b[1m%s%s%s\x1b[0m%s%s", c_act_fg, c_act_bg, item, c_bar_bg, c_bar_fg);
			} else {
				EMIT("%s", item);
			}
			wused += ilen;
		}
		EMIT("\x1b[K\x1b[0m\r\n");
	} else {
		EMIT(" [%s] %s (%zu users)%s%s%s\x1b[K\x1b[0m\r\n",
		     st->nick, buf->name, buf->user_count,
		     buf->topic[0] ? " | " : "",
		     buf->topic[0] ? buf->topic : "",
		     ui->scroll_offset > 0 ? " [SCROLL]" : "");
	}

	char prompt[STATE_NICK_LEN + 16];
	const char *plabel = ui->prompt_label[0] ? ui->prompt_label : st->nick;
	int plen = snprintf(prompt, sizeof(prompt), "[%s] ", plabel);
	int avail = ui->cols - plen - 1;
	if (avail < 1) {
		avail = 1;
	}

	size_t view_start = 0;
	if ((int)ui->cursor >= avail) {
		view_start = ui->cursor - (size_t)avail + 1;
	}

	EMIT("\x1b[2K\x1b[1m%s%s\x1b[0m%.*s", c_prompt, prompt, avail, ui->input + view_start);

	int cursor_col = plen + (int)(ui->cursor - view_start) + 1;
	EMIT("\x1b[%d;%dH\x1b[?25h", ui->rows, cursor_col);

	#undef EMIT

	write(STDOUT_FILENO, frame, pos);
	ui->dirty = false;
}

UiAction ui_handle_stdin(UiState *ui, const AppState *st, struct LuaEngine *lua, char *out_line, size_t max_len) {
	char seq[8];
	ssize_t n = read(STDIN_FILENO, seq, sizeof(seq));
	if (n <= 0) {
		return UI_ACTION_NONE;
	}

	ui->dirty = true;

	if (seq[0] == '\x1b' && n >= 2) {
		if (seq[1] == '[' && n >= 3) {
			ui->comp_active = false;
			if (seq[2] == 'Z') {
				ui->comp_active = true;
				trigger_completion(ui, st, lua, true);
			} else if (seq[2] == 'D' && ui->cursor > 0) {
				ui->cursor--;
			} else if (seq[2] == 'C' && ui->cursor < ui->input_len) {
				ui->cursor++;
			} else if (seq[2] == 'H') {
				ui->cursor = 0;
			} else if (seq[2] == 'F') {
				ui->cursor = ui->input_len;
			} else if (n >= 4 && seq[3] == '~') {
				if (seq[2] == '5') {
					ui->scroll_offset += 10;
				} else if (seq[2] == '6') {
					ui->scroll_offset = ui->scroll_offset > 10 ? ui->scroll_offset - 10 : 0;
				} else if (seq[2] == '3' && ui->cursor < ui->input_len) {
					memmove(ui->input + ui->cursor, ui->input + ui->cursor + 1, ui->input_len - ui->cursor);
					ui->input_len--;
				}
			}
			return UI_ACTION_NONE;
		}

		ui->comp_active = false;
		char alt = seq[1];
		if (alt == 'j') {
			ui->scroll_offset = ui->scroll_offset > 0 ? ui->scroll_offset - 1 : 0;
		} else if (alt == 'k') {
			ui->scroll_offset += 1;
		} else if (alt == 'h' && ui->cursor > 0) {
			ui->cursor--;
		} else if (alt == 'l' && ui->cursor < ui->input_len) {
			ui->cursor++;
		} else if (alt == 'n') {
			ui->scroll_offset = 0;
			return UI_ACTION_NEXT_BUF;
		} else if (alt == 'p') {
			ui->scroll_offset = 0;
			return UI_ACTION_PREV_BUF;
		} else if (alt >= '1' && alt <= '9') {
			ui->scroll_offset = 0;
			ui->target_buf_idx = (size_t)(alt - '1');
			return UI_ACTION_SWITCH_BUF;
		}
		return UI_ACTION_NONE;
	}

	for (ssize_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)seq[i];

		if (c == '\t') {
			trigger_completion(ui, st, lua, false);
			continue;
		}

		ui->comp_active = false;

		if (c == 3) {
			return UI_ACTION_QUIT;
		}
		if (c == 14) {
			ui->scroll_offset = 0;
			return UI_ACTION_NEXT_BUF;
		}
		if (c == 16) {
			ui->scroll_offset = 0;
			return UI_ACTION_PREV_BUF;
		}
		if (c == 1) {
			ui->cursor = 0;
			continue;
		}
		if (c == 5) {
			ui->cursor = ui->input_len;
			continue;
		}
		if (c == 21) {
			ui->input_len = 0;
			ui->cursor = 0;
			ui->input[0] = '\0';
			continue;
		}
		if (c == 23) {
			delete_prev_word(ui);
			continue;
		}

		if (c == '\r' || c == '\n') {
			snprintf(out_line, max_len, "%s", ui->input);
			ui->input_len = 0;
			ui->cursor = 0;
			ui->input[0] = '\0';
			ui->scroll_offset = 0;
			return UI_ACTION_SUBMIT;
		}

		if (c == 127 || c == 8) {
			if (ui->cursor > 0) {
				memmove(ui->input + ui->cursor - 1, ui->input + ui->cursor, ui->input_len - ui->cursor + 1);
				ui->cursor--;
				ui->input_len--;
			}
			continue;
		}

		if (c >= 32 && ui->input_len < UI_INPUT_MAX - 1) {
			memmove(ui->input + ui->cursor + 1, ui->input + ui->cursor, ui->input_len - ui->cursor + 1);
			ui->input[ui->cursor] = (char)c;
			ui->cursor++;
			ui->input_len++;
		}
	}

	return UI_ACTION_NONE;
}
