#ifndef IRC_H
#define IRC_H

#include "net.h"
#include <stdbool.h>
#include <stddef.h>

#define IRC_MAX_LINE   4096
#define IRC_MAX_PARAMS 15
#define IRC_BUF_SIZE   16384

typedef struct {
    char *tags;
    char *prefix;
    char *nick;
    char *command;
    char *params[IRC_MAX_PARAMS];
    int param_count;
} IrcMessage;

typedef struct {
    char buf[IRC_BUF_SIZE];
    size_t len;
} IrcLineBuf;

bool irc_buf_push(IrcLineBuf *lb, const char *data, size_t data_len);
bool irc_buf_pop_line(IrcLineBuf *lb, char *out_line, size_t max_len);
bool irc_parse_line(char *line, IrcMessage *msg);

void irc_send_raw(NetConn *conn, const char *fmt, ...);
void irc_send_handshake(NetConn *conn, const char *nick, const char *user, const char *realname);
void irc_send_privmsg(NetConn *conn, const char *target, const char *text);
void irc_send_join(NetConn *conn, const char *channel);

#endif
