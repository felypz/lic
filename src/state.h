#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define STATE_MAX_BUFFERS  64
#define STATE_MAX_HISTORY  1024
#define STATE_MAX_USERS    512
#define STATE_NAME_LEN     64
#define STATE_NICK_LEN     32
#define STATE_TEXT_LEN     1024

typedef enum {
    BUF_SERVER,
    BUF_CHANNEL,
    BUF_QUERY
} BufferType;

typedef enum {
    LINE_MSG,
    LINE_ACTION,
    LINE_EVENT,
    LINE_ERROR
} LineKind;

typedef struct {
    time_t timestamp;
    LineKind kind;
    char sender[STATE_NICK_LEN];
    char text[STATE_TEXT_LEN];
    bool highlight;
} ChatLine;

typedef struct {
    char nick[STATE_NICK_LEN];
    char prefix;
} ChannelUser;

typedef struct {
    BufferType type;
    char name[STATE_NAME_LEN];
    char topic[STATE_TEXT_LEN];

    ChatLine lines[STATE_MAX_HISTORY];
    size_t line_head;
    size_t line_count;

    ChannelUser users[STATE_MAX_USERS];
    size_t user_count;

    bool unread;
    bool mention;
} Buffer;

typedef struct {
    char nick[STATE_NICK_LEN];
    char server_name[STATE_NAME_LEN];
    Buffer buffers[STATE_MAX_BUFFERS];
    size_t buffer_count;
    size_t active_idx;
} AppState;

void state_init(AppState *st, const char *server_name, const char *nick);
Buffer *state_active_buffer(AppState *st);
Buffer *state_get_or_create_buffer(AppState *st, const char *name, BufferType type);
void state_remove_buffer(AppState *st, const char *name);
void state_switch_buffer(AppState *st, size_t idx);
void state_next_buffer(AppState *st);
void state_prev_buffer(AppState *st);

void buffer_push_line(Buffer *buf, LineKind kind, const char *sender, const char *text, bool highlight);
const ChatLine *buffer_get_line(const Buffer *buf, size_t offset_from_oldest);

void buffer_add_user(Buffer *buf, const char *nick, char prefix);
void buffer_remove_user(Buffer *buf, const char *nick);
void buffer_rename_user(Buffer *buf, const char *old_nick, const char *new_nick);
void state_remove_user_everywhere(AppState *st, const char *nick);
void state_rename_user_everywhere(AppState *st, const char *old_nick, const char *new_nick);

#endif
