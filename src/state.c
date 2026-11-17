#include "state.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static void copy_str(char *dst, const char *src, size_t dst_size) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

void state_init(AppState *st, const char *server_name, const char *nick) {
    memset(st, 0, sizeof(*st));
    copy_str(st->server_name, server_name, sizeof(st->server_name));
    copy_str(st->nick, nick, sizeof(st->nick));
    state_get_or_create_buffer(st, server_name, BUF_SERVER);
}

Buffer *state_active_buffer(AppState *st) {
    if (st->buffer_count == 0) return NULL;
    return &st->buffers[st->active_idx];
}

Buffer *state_get_or_create_buffer(AppState *st, const char *name, BufferType type) {
    for (size_t i = 0; i < st->buffer_count; i++) {
        if (strcasecmp(st->buffers[i].name, name) == 0) {
            return &st->buffers[i];
        }
    }

    if (st->buffer_count >= STATE_MAX_BUFFERS) return NULL;

    Buffer *buf = &st->buffers[st->buffer_count++];
    memset(buf, 0, sizeof(*buf));
    buf->type = type;
    copy_str(buf->name, name, sizeof(buf->name));
    return buf;
}

void state_remove_buffer(AppState *st, const char *name) {
    for (size_t i = 1; i < st->buffer_count; i++) {
        if (strcasecmp(st->buffers[i].name, name) == 0) {
            memmove(&st->buffers[i], &st->buffers[i + 1],
                    (st->buffer_count - i - 1) * sizeof(Buffer));
            st->buffer_count--;
            if (st->active_idx >= st->buffer_count) {
                st->active_idx = st->buffer_count - 1;
            }
            return;
        }
    }
}

void state_switch_buffer(AppState *st, size_t idx) {
    if (idx >= st->buffer_count) return;
    st->active_idx = idx;
    st->buffers[idx].unread = false;
    st->buffers[idx].mention = false;
}

void state_next_buffer(AppState *st) {
    if (st->buffer_count <= 1) return;
    state_switch_buffer(st, (st->active_idx + 1) % st->buffer_count);
}

void state_prev_buffer(AppState *st) {
    if (st->buffer_count <= 1) return;
    size_t idx = st->active_idx == 0 ? st->buffer_count - 1 : st->active_idx - 1;
    state_switch_buffer(st, idx);
}

void buffer_push_line(Buffer *buf, LineKind kind, const char *sender, const char *text, bool highlight) {
    if (!buf) return;

    size_t write_idx;
    if (buf->line_count < STATE_MAX_HISTORY) {
        write_idx = (buf->line_head + buf->line_count) % STATE_MAX_HISTORY;
        buf->line_count++;
    } else {
        write_idx = buf->line_head;
        buf->line_head = (buf->line_head + 1) % STATE_MAX_HISTORY;
    }

    ChatLine *line = &buf->lines[write_idx];
    line->timestamp = time(NULL);
    line->kind = kind;
    line->highlight = highlight;
    copy_str(line->sender, sender, sizeof(line->sender));
    copy_str(line->text, text, sizeof(line->text));
}

const ChatLine *buffer_get_line(const Buffer *buf, size_t offset_from_oldest) {
    if (!buf || offset_from_oldest >= buf->line_count) return NULL;
    size_t idx = (buf->line_head + offset_from_oldest) % STATE_MAX_HISTORY;
    return &buf->lines[idx];
}

void buffer_add_user(Buffer *buf, const char *nick, char prefix) {
    if (!buf || !nick) return;
    for (size_t i = 0; i < buf->user_count; i++) {
        if (strcasecmp(buf->users[i].nick, nick) == 0) {
            if (prefix != '\0') buf->users[i].prefix = prefix;
            return;
        }
    }
    if (buf->user_count >= STATE_MAX_USERS) return;
    ChannelUser *u = &buf->users[buf->user_count++];
    copy_str(u->nick, nick, sizeof(u->nick));
    u->prefix = prefix;
}

void buffer_remove_user(Buffer *buf, const char *nick) {
    if (!buf || !nick) return;
    for (size_t i = 0; i < buf->user_count; i++) {
        if (strcasecmp(buf->users[i].nick, nick) == 0) {
            memmove(&buf->users[i], &buf->users[i + 1],
                    (buf->user_count - i - 1) * sizeof(ChannelUser));
            buf->user_count--;
            return;
        }
    }
}

void buffer_rename_user(Buffer *buf, const char *old_nick, const char *new_nick) {
    if (!buf || !old_nick || !new_nick) return;
    for (size_t i = 0; i < buf->user_count; i++) {
        if (strcasecmp(buf->users[i].nick, old_nick) == 0) {
            copy_str(buf->users[i].nick, new_nick, sizeof(buf->users[i].nick));
            return;
        }
    }
}

void state_remove_user_everywhere(AppState *st, const char *nick) {
    for (size_t i = 0; i < st->buffer_count; i++) {
        if (st->buffers[i].type == BUF_CHANNEL) {
            buffer_remove_user(&st->buffers[i], nick);
        }
    }
}

void state_rename_user_everywhere(AppState *st, const char *old_nick, const char *new_nick) {
    for (size_t i = 0; i < st->buffer_count; i++) {
        if (st->buffers[i].type == BUF_CHANNEL) {
            buffer_rename_user(&st->buffers[i], old_nick, new_nick);
        }
    }
}
