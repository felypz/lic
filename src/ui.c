#include "ui.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig_termios;
static bool termios_saved = false;
static volatile sig_atomic_t resized_flag = 0;

static const char *builtin_commands[] = {
	"join", "part", "msg", "query", "me", "nick", "whois", "topic",
	"kick", "mode", "notice", "names", "clear", "close", "raw", "reload", "lua", "quit"
};

static const unsigned int mirc_ansi_palette[16] = {
	15, 0, 4, 2, 9, 1, 5, 3, 11, 10, 6, 14, 12, 13, 8, 7
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
	const char *cleanup = "\x1b[?25h\x1b[2 q\x1b[0m\x1b[?1049l";
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
	ui->mode = UI_MODE_INSERT;

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

static bool is_wayland(void) {
	const char *wd = getenv("WAYLAND_DISPLAY");
	return wd && wd[0] != '\0';
}

bool ui_clipboard_copy(const char *text) {
	if (!text) {
		return false;
	}
	int pipefd[2];
	if (pipe(pipefd) != 0) {
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return false;
	}

	if (pid == 0) {
		close(pipefd[1]);
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);

		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		if (is_wayland()) {
			execlp("wl-copy", "wl-copy", (char *)NULL);
		}
		execlp("xclip", "xclip", "-selection", "clipboard", (char *)NULL);
		execlp("xsel", "xsel", "--clipboard", "--input", (char *)NULL);
		_exit(127);
	}

	close(pipefd[0]);
	size_t len = strlen(text);
	size_t sent = 0;
	while (sent < len) {
		ssize_t w = write(pipefd[1], text + sent, len - sent);
		if (w <= 0) {
			break;
		}
		sent += (size_t)w;
	}
	close(pipefd[1]);

	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool ui_clipboard_paste(char *dst, size_t dst_size) {
	if (!dst || dst_size == 0) {
		return false;
	}
	int pipefd[2];
	if (pipe(pipefd) != 0) {
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return false;
	}

	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);

		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		if (is_wayland()) {
			execlp("wl-paste", "wl-paste", "--no-newline", (char *)NULL);
		}
		execlp("xclip", "xclip", "-selection", "clipboard", "-o", (char *)NULL);
		execlp("xsel", "xsel", "--clipboard", "--output", (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	size_t total = 0;
	while (total + 1 < dst_size) {
		ssize_t r = read(pipefd[0], dst + total, dst_size - 1 - total);
		if (r <= 0) {
			break;
		}
		total += (size_t)r;
	}
	dst[total] = '\0';
	close(pipefd[0]);

	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 && total > 0;
}

static void insert_text_at_cursor(UiState *ui, const char *text) {
	if (!text || text[0] == '\0') {
		return;
	}
	for (size_t i = 0; text[i] != '\0' && ui->input_len < UI_INPUT_MAX - 1; i++) {
		unsigned char c = (unsigned char)text[i];
		if (c == '\r' || c == '\n') {
			continue;
		}
		if (c >= 32 || (c & 0x80) != 0) {
			memmove(ui->input + ui->cursor + 1, ui->input + ui->cursor, ui->input_len - ui->cursor + 1);
			ui->input[ui->cursor] = (char)c;
			ui->cursor++;
			ui->input_len++;
		}
	}
}

static bool is_utf8_cont(unsigned char c) {
	return (c & 0xC0) == 0x80;
}

static size_t utf8_prev_cp(const char *s, size_t pos) {
	if (pos == 0) {
		return 0;
	}
	pos--;
	while (pos > 0 && is_utf8_cont((unsigned char)s[pos])) {
		pos--;
	}
	return pos;
}

static size_t utf8_next_cp(const char *s, size_t len, size_t pos) {
	if (pos >= len) {
		return len;
	}
	pos++;
	while (pos < len && is_utf8_cont((unsigned char)s[pos])) {
		pos++;
	}
	return pos;
}

static size_t utf8_visual_cols(const char *s, size_t byte_len) {
	size_t cols = 0;
	for (size_t i = 0; i < byte_len && s[i]; i++) {
		if (!is_utf8_cont((unsigned char)s[i])) {
			cols++;
		}
	}
	return cols;
}

static size_t parse_irc_color_seq(const char *s, int *fg_out, int *bg_out) {
	size_t idx = 1;
	*fg_out = -1;
	*bg_out = -1;

	if (!isdigit((unsigned char)s[idx])) {
		return idx;
	}

	int fg = s[idx++] - '0';
	if (isdigit((unsigned char)s[idx])) {
		fg = fg * 10 + (s[idx++] - '0');
	}
	*fg_out = fg;

	if (s[idx] == ',' && isdigit((unsigned char)s[idx + 1])) {
		idx++;
		int bg = s[idx++] - '0';
		if (isdigit((unsigned char)s[idx])) {
			bg = bg * 10 + (s[idx++] - '0');
		}
		*bg_out = bg;
	}
	return idx;
}

static void format_irc_text(const char *src, char *dst, size_t dst_size) {
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strike = false;
	bool reverse = false;
	int fg = -1;
	int bg = -1;

	size_t out = 0;
	#define APPEND(...) do { \
		if (out < dst_size - 1) { \
			int w = snprintf(dst + out, dst_size - out, __VA_ARGS__); \
			if (w > 0) out += (size_t)w; \
		} \
	} while (0)

	for (size_t i = 0; src[i] != '\0' && out < dst_size - 16; ) {
		unsigned char c = (unsigned char)src[i];
		bool state_changed = false;

		if (c == 0x02) {
			bold = !bold;
			i++;
			state_changed = true;
		} else if (c == 0x1D) {
			italic = !italic;
			i++;
			state_changed = true;
		} else if (c == 0x1F) {
			underline = !underline;
			i++;
			state_changed = true;
		} else if (c == 0x1E) {
			strike = !strike;
			i++;
			state_changed = true;
		} else if (c == 0x16) {
			reverse = !reverse;
			i++;
			state_changed = true;
		} else if (c == 0x0F) {
			bold = italic = underline = strike = reverse = false;
			fg = bg = -1;
			i++;
			state_changed = true;
		} else if (c == 0x03) {
			int nfg, nbg;
			i += parse_irc_color_seq(src + i, &nfg, &nbg);
			if (nfg < 0) {
				fg = -1;
				bg = -1;
			} else {
				fg = nfg;
				if (nbg >= 0) {
					bg = nbg;
				}
			}
			state_changed = true;
		} else if (c < 32 && c != '\t') {
			i++;
			continue;
		} else {
			dst[out++] = src[i++];
			continue;
		}

		if (state_changed) {
			APPEND("\x1b[0m");
			if (bold) APPEND("\x1b[1m");
			if (italic) APPEND("\x1b[3m");
			if (underline) APPEND("\x1b[4m");
			if (reverse) APPEND("\x1b[7m");
			if (strike) APPEND("\x1b[9m");
			if (fg >= 0 && fg <= 98) {
				unsigned int mapped = fg < 16 ? mirc_ansi_palette[fg] : (unsigned int)fg;
				APPEND("\x1b[38;5;%um", mapped);
			}
			if (bg >= 0 && bg <= 98) {
				unsigned int mapped = bg < 16 ? mirc_ansi_palette[bg] : (unsigned int)bg;
				APPEND("\x1b[48;5;%um", mapped);
			}
		}
	}

	APPEND("\x1b[0m");
	dst[out] = '\0';
	#undef APPEND
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

static void history_push(UiState *ui, const char *line) {
	if (!line || line[0] == '\0') {
		return;
	}
	if (ui->hist_count > 0 && strcmp(ui->history[ui->hist_count - 1], line) == 0) {
		ui->hist_pos = ui->hist_count;
		return;
	}
	if (ui->hist_count == UI_HIST_MAX) {
		memmove(ui->history[0], ui->history[1], (UI_HIST_MAX - 1) * sizeof(ui->history[0]));
		ui->hist_count--;
	}
	snprintf(ui->history[ui->hist_count++], UI_INPUT_MAX, "%s", line);
	ui->hist_pos = ui->hist_count;
}

static void history_prev(UiState *ui) {
	if (ui->hist_count == 0 || ui->hist_pos == 0) {
		return;
	}
	if (ui->hist_pos == ui->hist_count) {
		snprintf(ui->hist_draft, sizeof(ui->hist_draft), "%s", ui->input);
	}
	ui->hist_pos--;
	snprintf(ui->input, sizeof(ui->input), "%s", ui->history[ui->hist_pos]);
	ui->input_len = strlen(ui->input);
	ui->cursor = ui->input_len;
}

static void history_next(UiState *ui) {
	if (ui->hist_pos >= ui->hist_count) {
		return;
	}
	ui->hist_pos++;
	if (ui->hist_pos == ui->hist_count) {
		snprintf(ui->input, sizeof(ui->input), "%s", ui->hist_draft);
	} else {
		snprintf(ui->input, sizeof(ui->input), "%s", ui->history[ui->hist_pos]);
	}
	ui->input_len = strlen(ui->input);
	ui->cursor = ui->input_len;
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

static void replace_input_span(UiState *ui, size_t start, size_t end, const char *replacement) {
	size_t rlen = strlen(replacement);
	size_t old_len = end - start;
	if (ui->input_len - old_len + rlen >= UI_INPUT_MAX) {
		return;
	}
	memmove(ui->input + start + rlen, ui->input + end, ui->input_len - end + 1);
	memcpy(ui->input + start, replacement, rlen);
	ui->input_len = ui->input_len - old_len + rlen;
	ui->comp_end = start + rlen;
	ui->cursor = ui->comp_end;
}

static void apply_completion(UiState *ui) {
	if (!ui->comp_active || ui->comp_count == 0) {
		return;
	}
	replace_input_span(ui, ui->comp_start, ui->comp_end, ui->comp_matches[ui->comp_idx]);
}

static void cancel_completion(UiState *ui) {
	if (!ui->comp_active) {
		return;
	}
	replace_input_span(ui, ui->comp_start, ui->comp_end, ui->comp_orig);
	ui->comp_active = false;
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
	if (prefix_len >= UI_COMP_STR_LEN) {
		return;
	}

	memcpy(ui->comp_orig, ui->input + start, prefix_len);
	ui->comp_orig[prefix_len] = '\0';

	ui->comp_count = 0;
	ui->comp_start = start;
	ui->comp_end = ui->cursor;
	ui->comp_idx = 0;

	if (start == 0 && ui->comp_orig[0] == '/') {
		const char *cmd_prefix = ui->comp_orig + 1;
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
	} else if (ui->comp_orig[0] == '#' || ui->comp_orig[0] == '&') {
		for (size_t i = 0; i < st->buffer_count; i++) {
			if (st->buffers[i].type == BUF_CHANNEL &&
			    strncasecmp(st->buffers[i].name, ui->comp_orig, prefix_len) == 0) {
				char full[UI_COMP_STR_LEN];
				snprintf(full, sizeof(full), "%s ", st->buffers[i].name);
				comp_add(ui, full);
			}
		}
	} else {
		const Buffer *buf = state_active_buffer((AppState *)st);
		if (buf) {
			for (size_t i = 0; i < buf->user_count; i++) {
				if (strncasecmp(buf->users[i].nick, ui->comp_orig, prefix_len) == 0) {
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

static size_t word_prev_pos(const char *s, size_t pos) {
	while (pos > 0 && isspace((unsigned char)s[pos - 1])) {
		pos--;
	}
	while (pos > 0 && !isspace((unsigned char)s[pos - 1])) {
		pos = utf8_prev_cp(s, pos);
	}
	return pos;
}

static size_t word_next_pos(const char *s, size_t len, size_t pos) {
	while (pos < len && !isspace((unsigned char)s[pos])) {
		pos = utf8_next_cp(s, len, pos);
	}
	while (pos < len && isspace((unsigned char)s[pos])) {
		pos++;
	}
	return pos;
}

static size_t word_end_pos(const char *s, size_t len, size_t pos) {
	if (pos < len) {
		pos = utf8_next_cp(s, len, pos);
	}
	while (pos < len && isspace((unsigned char)s[pos])) {
		pos++;
	}
	while (pos < len && !isspace((unsigned char)s[pos])) {
		size_t next = utf8_next_cp(s, len, pos);
		if (next >= len || isspace((unsigned char)s[next])) {
			break;
		}
		pos = next;
	}
	return pos;
}

static void delete_prev_word(UiState *ui) {
	if (ui->cursor == 0) {
		return;
	}
	size_t pos = word_prev_pos(ui->input, ui->cursor);
	memmove(ui->input + pos, ui->input + ui->cursor, ui->input_len - ui->cursor + 1);
	ui->input_len -= (ui->cursor - pos);
	ui->cursor = pos;
}

static void delete_next_word(UiState *ui) {
	if (ui->cursor >= ui->input_len) {
		return;
	}
	size_t pos = word_next_pos(ui->input, ui->input_len, ui->cursor);
	memmove(ui->input + ui->cursor, ui->input + pos, ui->input_len - pos + 1);
	ui->input_len -= (pos - ui->cursor);
}

static void clamp_normal_cursor(UiState *ui) {
	if (ui->mode == UI_MODE_NORMAL && ui->input_len > 0 && ui->cursor >= ui->input_len) {
		ui->cursor = utf8_prev_cp(ui->input, ui->input_len);
	}
}

static size_t next_wrap_chunk(const char *text, size_t width) {
	size_t cols = 0;
	size_t last_space = 0;
	size_t i = 0;

	while (text[i] != '\0') {
		unsigned char c = (unsigned char)text[i];

		if (c == 0x03) {
			int fg, bg;
			i += parse_irc_color_seq(text + i, &fg, &bg);
			continue;
		}
		if (c < 32 && c != '\t') {
			i++;
			continue;
		}

		if (c == ' ') {
			last_space = i;
		}

		size_t next_i = utf8_next_cp(text, strlen(text + i) + i, i);
		if (cols + 1 > width) {
			return last_space > 0 ? last_space : (i > 0 ? i : next_i);
		}
		cols++;
		i = next_i;
	}
	return i;
}

static void render_buffer_pane(const Buffer *buf, int width, int chat_rows, size_t scroll_offset,
                               const char *c_ts, const char *c_mention, const char *c_event,
                               const char *c_err, const char *c_act,
                               char out_rows[][1536], int *out_count) {
	*out_count = 0;
	if (!buf || width < 12 || chat_rows < 1) {
		return;
	}

	size_t total = buf->line_count;
	size_t end_idx = total > scroll_offset ? total - scroll_offset : 0;
	char temp_rows[256][1536];
	int tcount = 0;

	for (size_t i = end_idx; i > 0 && tcount < chat_rows; i--) {
		const ChatLine *cl = buffer_get_line(buf, i - 1);
		if (!cl) {
			continue;
		}

		struct tm *tm_info = localtime(&cl->timestamp);
		char ts[16];
		strftime(ts, sizeof(ts), "%H:%M", tm_info);

		int prefix_len = 6;
		if (cl->kind == LINE_MSG || cl->kind == LINE_ACTION) {
			prefix_len += (int)utf8_visual_cols(cl->sender, strlen(cl->sender)) + 3;
		} else if (cl->kind == LINE_ERROR) {
			prefix_len += 2;
		} else {
			prefix_len += 3;
		}

		int text_width = width - prefix_len;
		if (text_width < 8) {
			text_width = 8;
		}

		const char *p = cl->text;
		char line_chunks[32][1536];
		int chunk_count = 0;

		if (*p == '\0') {
			line_chunks[chunk_count++][0] = '\0';
		} else {
			while (*p && chunk_count < 32) {
				size_t take = next_wrap_chunk(p, (size_t)text_width);
				char raw_slice[1024];
				snprintf(raw_slice, sizeof(raw_slice), "%.*s", (int)take, p);
				format_irc_text(raw_slice, line_chunks[chunk_count++], sizeof(line_chunks[0]));
				p += take;
				while (*p == ' ') {
					p++;
				}
			}
		}

		for (int c = chunk_count - 1; c >= 0 && tcount < chat_rows; c--) {
			if (c == 0) {
				if (cl->kind == LINE_MSG) {
					unsigned int col = nick_color(cl->sender);
					if (cl->highlight) {
						snprintf(temp_rows[tcount], sizeof(temp_rows[0]),
						         "%s%s\x1b[0m \x1b[1m%s<%s>\x1b[0m \x1b[1m%s\x1b[0m",
						         c_ts, ts, c_mention, cl->sender, line_chunks[c]);
					} else {
						snprintf(temp_rows[tcount], sizeof(temp_rows[0]),
						         "%s%s\x1b[0m \x1b[%um<%s>\x1b[0m %s",
						         c_ts, ts, col, cl->sender, line_chunks[c]);
					}
				} else if (cl->kind == LINE_ACTION) {
					snprintf(temp_rows[tcount], sizeof(temp_rows[0]),
					         "%s%s\x1b[0m %s* %s %s\x1b[0m",
					         c_ts, ts, c_act, cl->sender, line_chunks[c]);
				} else if (cl->kind == LINE_ERROR) {
					snprintf(temp_rows[tcount], sizeof(temp_rows[0]),
					         "%s%s\x1b[0m %s! %s\x1b[0m",
					         c_ts, ts, c_err, line_chunks[c]);
				} else {
					snprintf(temp_rows[tcount], sizeof(temp_rows[0]),
					         "%s%s\x1b[0m %s-- %s\x1b[0m",
					         c_ts, ts, c_event, line_chunks[c]);
				}
			} else {
				snprintf(temp_rows[tcount], sizeof(temp_rows[0]),
				         "%*s%s", prefix_len, "", line_chunks[c]);
			}
			tcount++;
		}
	}

	int idx = 0;
	for (int i = tcount - 1; i >= 0; i--) {
		snprintf(out_rows[idx++], 1536, "%s", temp_rows[i]);
	}
	while (idx < chat_rows) {
		out_rows[idx++][0] = '\0';
	}
	*out_count = idx;
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

	clamp_normal_cursor(ui);

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

	bool active_split = ui->split_enabled && st->buffer_count > 1 && ui->cols >= 90;
	if (active_split) {
		int left_w = ui->cols / 2;
		int right_w = ui->cols - left_w - 1;
		size_t sidx = ui->split_buf_idx % st->buffer_count;
		if (sidx == st->active_idx) {
			sidx = (st->active_idx + 1) % st->buffer_count;
		}
		const Buffer *sbuf = &st->buffers[sidx];

		char left_rows[256][1536];
		char right_rows[256][1536];
		int lcount = 0, rcount = 0;

		render_buffer_pane(buf, left_w, chat_rows, ui->scroll_offset,
		                   c_ts, c_mention, c_event, c_err, c_act, left_rows, &lcount);
		render_buffer_pane(sbuf, right_w, chat_rows, 0,
		                   c_ts, c_mention, c_event, c_err, c_act, right_rows, &rcount);

		for (int r = 0; r < chat_rows; r++) {
			EMIT("\x1b[2K%s\x1b[0m\x1b[%dG%s│%s\x1b[0m\r\n",
			     r < lcount ? left_rows[r] : "",
			     left_w + 1,
			     c_ts,
			     r < rcount ? right_rows[r] : "");
		}
	} else {
		char rows_buf[256][1536];
		int rcount = 0;
		render_buffer_pane(buf, ui->cols, chat_rows, ui->scroll_offset,
		                   c_ts, c_mention, c_event, c_err, c_act, rows_buf, &rcount);
		for (int r = 0; r < chat_rows; r++) {
			EMIT("\x1b[2K%s\r\n", r < rcount ? rows_buf[r] : "");
		}
	}

	EMIT("%s%s", c_bar_bg, c_bar_fg);
	if (ui->comp_active && ui->comp_count > 0) {
		int wused = 0;
		char count_tag[32];
		int clen = snprintf(count_tag, sizeof(count_tag), " [%zu/%zu] ", ui->comp_idx + 1, ui->comp_count);
		EMIT("%s", count_tag);
		wused += clen;

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
		const char *mode_tag = ui->mode == UI_MODE_NORMAL ? "NORMAL" : "INSERT";
		char clean_topic[STATE_TEXT_LEN];
		format_irc_text(buf->topic, clean_topic, sizeof(clean_topic));
		EMIT("\x1b[1m%s%s %s \x1b[0m%s%s [%s] %s (%zu users)%s%s%s%s\x1b[K\x1b[0m\r\n",
		     c_act_fg, c_act_bg, mode_tag,
		     c_bar_bg, c_bar_fg,
		     st->nick, buf->name, buf->user_count,
		     buf->topic[0] ? " | " : "",
		     buf->topic[0] ? clean_topic : "",
		     active_split ? " [SPLIT]" : "",
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
	while (utf8_visual_cols(ui->input + view_start, ui->cursor - view_start) >= (size_t)avail) {
		view_start = utf8_next_cp(ui->input, ui->input_len, view_start);
	}

	size_t view_end = view_start;
	size_t drawn_cols = 0;
	while (view_end < ui->input_len && drawn_cols < (size_t)avail) {
		view_end = utf8_next_cp(ui->input, ui->input_len, view_end);
		drawn_cols++;
	}

	EMIT("\x1b[2K\x1b[1m%s%s\x1b[0m%.*s",
	     c_prompt, prompt, (int)(view_end - view_start), ui->input + view_start);

	int cursor_col = plen + (int)utf8_visual_cols(ui->input + view_start, ui->cursor - view_start) + 1;
	const char *cursor_shape = ui->mode == UI_MODE_NORMAL ? "\x1b[2 q" : "\x1b[6 q";
	EMIT("\x1b[%d;%dH%s\x1b[?25h", ui->rows, cursor_col, cursor_shape);

	#undef EMIT

	write(STDOUT_FILENO, frame, pos);
	ui->dirty = false;
}

static UiAction handle_normal_key(UiState *ui, const AppState *st, unsigned char c, char *out_line, size_t max_len) {
	if (c == 3) {
		return UI_ACTION_QUIT;
	}

	char op = ui->pending_op;
	ui->pending_op = 0;

	if (op == 'd') {
		if (c == 'd') {
			ui->input[0] = '\0';
			ui->input_len = 0;
			ui->cursor = 0;
		} else if (c == 'w' || c == 'e') {
			delete_next_word(ui);
		} else if (c == 'b') {
			delete_prev_word(ui);
		} else if (c == '$') {
			ui->input[ui->cursor] = '\0';
			ui->input_len = ui->cursor;
		} else if (c == '0') {
			memmove(ui->input, ui->input + ui->cursor, ui->input_len - ui->cursor + 1);
			ui->input_len -= ui->cursor;
			ui->cursor = 0;
		}
		return UI_ACTION_NONE;
	}

	if (op == 'y') {
		if (c == 'y') {
			ui_clipboard_copy(ui->input);
		}
		return UI_ACTION_NONE;
	}

	if (op == 'g') {
		if (c == 'g') {
			const Buffer *buf = state_active_buffer((AppState *)st);
			if (buf && buf->line_count > 0) {
				ui->scroll_offset = buf->line_count - 1;
			}
		} else if (c == 't') {
			ui->scroll_offset = 0;
			return UI_ACTION_NEXT_BUF;
		} else if (c == 'T') {
			ui->scroll_offset = 0;
			return UI_ACTION_PREV_BUF;
		}
		return UI_ACTION_NONE;
	}

	if (op == 23) { /* Ctrl+W window prefix */
		if (c == 'v' || c == 's') {
			ui->split_enabled = !ui->split_enabled;
		} else if (c == 'c' || c == 'q') {
			ui->split_enabled = false;
		} else if (c == 'w' && st->buffer_count > 1) {
			ui->split_buf_idx = (ui->split_buf_idx + 1) % st->buffer_count;
		}
		return UI_ACTION_NONE;
	}

	if (c == 'd' || c == 'y' || c == 'g' || c == 23) {
		ui->pending_op = (char)c;
		return UI_ACTION_NONE;
	}

	if (c == 'i') {
		ui->mode = UI_MODE_INSERT;
	} else if (c == 'a') {
		if (ui->cursor < ui->input_len) {
			ui->cursor = utf8_next_cp(ui->input, ui->input_len, ui->cursor);
		}
		ui->mode = UI_MODE_INSERT;
	} else if (c == 'I') {
		ui->cursor = 0;
		ui->mode = UI_MODE_INSERT;
	} else if (c == 'A') {
		ui->cursor = ui->input_len;
		ui->mode = UI_MODE_INSERT;
	} else if (c == ':' || c == '/') {
		if (ui->input_len == 0) {
			ui->input[0] = '/';
			ui->input[1] = '\0';
			ui->input_len = 1;
			ui->cursor = 1;
		}
		ui->mode = UI_MODE_INSERT;
	} else if (c == 'h') {
		ui->cursor = utf8_prev_cp(ui->input, ui->cursor);
	} else if (c == 'l') {
		size_t next = utf8_next_cp(ui->input, ui->input_len, ui->cursor);
		if (next < ui->input_len) {
			ui->cursor = next;
		}
	} else if (c == 'w') {
		ui->cursor = word_next_pos(ui->input, ui->input_len, ui->cursor);
	} else if (c == 'e') {
		ui->cursor = word_end_pos(ui->input, ui->input_len, ui->cursor);
	} else if (c == 'b') {
		ui->cursor = word_prev_pos(ui->input, ui->cursor);
	} else if (c == '0') {
		ui->cursor = 0;
	} else if (c == '^') {
		size_t p = 0;
		while (p < ui->input_len && isspace((unsigned char)ui->input[p])) {
			p++;
		}
		ui->cursor = p;
	} else if (c == '$') {
		ui->cursor = ui->input_len;
	} else if (c == 'x') {
		if (ui->cursor < ui->input_len) {
			size_t next = utf8_next_cp(ui->input, ui->input_len, ui->cursor);
			memmove(ui->input + ui->cursor, ui->input + next, ui->input_len - next + 1);
			ui->input_len -= (next - ui->cursor);
		}
	} else if (c == 'X') {
		if (ui->cursor > 0) {
			size_t prev = utf8_prev_cp(ui->input, ui->cursor);
			memmove(ui->input + prev, ui->input + ui->cursor, ui->input_len - ui->cursor + 1);
			ui->input_len -= (ui->cursor - prev);
			ui->cursor = prev;
		}
	} else if (c == 'D') {
		ui->input[ui->cursor] = '\0';
		ui->input_len = ui->cursor;
	} else if (c == 'C') {
		ui->input[ui->cursor] = '\0';
		ui->input_len = ui->cursor;
		ui->mode = UI_MODE_INSERT;
	} else if (c == 'p') {
		char pasted[UI_INPUT_MAX];
		if (ui_clipboard_paste(pasted, sizeof(pasted))) {
			if (ui->cursor < ui->input_len) {
				ui->cursor = utf8_next_cp(ui->input, ui->input_len, ui->cursor);
			}
			insert_text_at_cursor(ui, pasted);
		}
	} else if (c == 'P') {
		char pasted[UI_INPUT_MAX];
		if (ui_clipboard_paste(pasted, sizeof(pasted))) {
			insert_text_at_cursor(ui, pasted);
		}
	} else if (c == 'j') {
		ui->scroll_offset = ui->scroll_offset > 0 ? ui->scroll_offset - 1 : 0;
	} else if (c == 'k') {
		ui->scroll_offset += 1;
	} else if (c == 4) { /* Ctrl+D: half page down */
		ui->scroll_offset = ui->scroll_offset > 10 ? ui->scroll_offset - 10 : 0;
	} else if (c == 21) { /* Ctrl+U: half page up */
		ui->scroll_offset += 10;
	} else if (c == 'G') {
		ui->scroll_offset = 0;
	} else if (c == 'J') {
		ui->scroll_offset = 0;
		return UI_ACTION_NEXT_BUF;
	} else if (c == 'K') {
		ui->scroll_offset = 0;
		return UI_ACTION_PREV_BUF;
	} else if (c == 16) { /* Ctrl+P: history prev */
		history_prev(ui);
	} else if (c == 14) { /* Ctrl+N: history next */
		history_next(ui);
	} else if (c >= '1' && c <= '9') {
		ui->scroll_offset = 0;
		ui->target_buf_idx = (size_t)(c - '1');
		return UI_ACTION_SWITCH_BUF;
	} else if (c == '\r' || c == '\n') {
		snprintf(out_line, max_len, "%s", ui->input);
		history_push(ui, ui->input);
		ui->input_len = 0;
		ui->cursor = 0;
		ui->input[0] = '\0';
		ui->scroll_offset = 0;
		ui->mode = UI_MODE_INSERT;
		return UI_ACTION_SUBMIT;
	}

	return UI_ACTION_NONE;
}

UiAction ui_handle_stdin(UiState *ui, const AppState *st, struct LuaEngine *lua, char *out_line, size_t max_len) {
	char seq[16];
	ssize_t n = read(STDIN_FILENO, seq, sizeof(seq));
	if (n <= 0) {
		return UI_ACTION_NONE;
	}

	ui->dirty = true;

	if (seq[0] == '\x1b') {
		if (n == 1) {
			ui->pending_op = 0;
			if (ui->comp_active) {
				cancel_completion(ui);
			} else if (ui->mode == UI_MODE_INSERT) {
				ui->mode = UI_MODE_NORMAL;
			}
			return UI_ACTION_NONE;
		}

		if (seq[1] == '[' && n >= 3) {
			if (seq[2] == 'Z') {
				trigger_completion(ui, st, lua, true);
				return UI_ACTION_NONE;
			}

			ui->comp_active = false;
			if (seq[2] == 'A') {
				history_prev(ui);
			} else if (seq[2] == 'B') {
				history_next(ui);
			} else if (seq[2] == 'D' && ui->cursor > 0) {
				ui->cursor = utf8_prev_cp(ui->input, ui->cursor);
			} else if (seq[2] == 'C' && ui->cursor < ui->input_len) {
				ui->cursor = utf8_next_cp(ui->input, ui->input_len, ui->cursor);
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
					size_t next = utf8_next_cp(ui->input, ui->input_len, ui->cursor);
					memmove(ui->input + ui->cursor, ui->input + next, ui->input_len - next + 1);
					ui->input_len -= (next - ui->cursor);
				}
			}
			return UI_ACTION_NONE;
		}

		return UI_ACTION_NONE;
	}

	if (ui->mode == UI_MODE_NORMAL) {
		for (ssize_t i = 0; i < n; i++) {
			UiAction act = handle_normal_key(ui, st, (unsigned char)seq[i], out_line, max_len);
			if (act != UI_ACTION_NONE) {
				return act;
			}
		}
		return UI_ACTION_NONE;
	}

	for (ssize_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)seq[i];

		if (c == '\t') {
			trigger_completion(ui, st, lua, false);
			continue;
		}

		if (ui->comp_active) {
			if (c == 14) {
				trigger_completion(ui, st, lua, false);
				continue;
			}
			if (c == 16) {
				trigger_completion(ui, st, lua, true);
				continue;
			}
			if (c == 25) {
				ui->comp_active = false;
				continue;
			}
			if (c == 5) {
				cancel_completion(ui);
				continue;
			}
			ui->comp_active = false;
		}

		if (c == 3) {
			return UI_ACTION_QUIT;
		}
		if (c == 14) {
			history_next(ui);
			continue;
		}
		if (c == 16) {
			history_prev(ui);
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
			history_push(ui, ui->input);
			ui->input_len = 0;
			ui->cursor = 0;
			ui->input[0] = '\0';
			ui->scroll_offset = 0;
			return UI_ACTION_SUBMIT;
		}

		if (c == 127 || c == 8) {
			if (ui->cursor > 0) {
				size_t prev = utf8_prev_cp(ui->input, ui->cursor);
				memmove(ui->input + prev, ui->input + ui->cursor, ui->input_len - ui->cursor + 1);
				ui->input_len -= (ui->cursor - prev);
				ui->cursor = prev;
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
