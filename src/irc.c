#include "irc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool irc_buf_push(IrcLineBuf *lb, const char *data, size_t data_len) {
    if (lb->len + data_len >= IRC_BUF_SIZE) {
        lb->len = 0;
        return false;
    }
    memcpy(lb->buf + lb->len, data, data_len);
    lb->len += data_len;
    lb->buf[lb->len] = '\0';
    return true;
}

bool irc_buf_pop_line(IrcLineBuf *lb, char *out_line, size_t max_len) {
    char *crlf = strstr(lb->buf, "\r\n");
    if (!crlf) return false;

    size_t line_len = (size_t)(crlf - lb->buf);
    size_t copy_len = line_len < max_len - 1 ? line_len : max_len - 1;

    memcpy(out_line, lb->buf, copy_len);
    out_line[copy_len] = '\0';

    size_t consumed = line_len + 2;
    memmove(lb->buf, lb->buf + consumed, lb->len - consumed);
    lb->len -= consumed;
    lb->buf[lb->len] = '\0';

    return true;
}

bool irc_parse_line(char *line, IrcMessage *msg) {
    memset(msg, 0, sizeof(*msg));
    char *p = line;

    if (*p == '@') {
        msg->tags = p + 1;
        char *space = strchr(p, ' ');
        if (!space) return false;
        *space = '\0';
        p = space + 1;
        while (*p == ' ') p++;
    }

    if (*p == ':') {
        msg->prefix = p + 1;
        char *space = strchr(p, ' ');
        if (!space) return false;
        *space = '\0';
        p = space + 1;
        while (*p == ' ') p++;

        char *bang = strchr(msg->prefix, '!');
        if (bang) {
            *bang = '\0';
            msg->nick = msg->prefix;
        } else {
            msg->nick = msg->prefix;
        }
    }

    if (*p == '\0') return false;
    msg->command = p;
    char *space = strchr(p, ' ');
    if (!space) return true;
    *space = '\0';
    p = space + 1;

    while (*p != '\0' && msg->param_count < IRC_MAX_PARAMS) {
        while (*p == ' ') p++;
        if (*p == '\0') break;

        if (*p == ':') {
            msg->params[msg->param_count++] = p + 1;
            break;
        }

        msg->params[msg->param_count++] = p;
        space = strchr(p, ' ');
        if (!space) break;
        *space = '\0';
        p = space + 1;
    }

    return true;
}

void irc_send_raw(NetConn *conn, const char *fmt, ...) {
    char out[IRC_MAX_LINE];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(out, sizeof(out) - 2, fmt, args);
    va_end(args);

    if (len > 0) {
        out[len++] = '\r';
        out[len++] = '\n';
        net_write(conn, out, (size_t)len);
    }
}

void irc_send_handshake(NetConn *conn, const char *nick, const char *user, const char *realname) {
    irc_send_raw(conn, "CAP LS 302");
    irc_send_raw(conn, "NICK %s", nick);
    irc_send_raw(conn, "USER %s 0 * :%s", user, realname);
}

void irc_send_privmsg(NetConn *conn, const char *target, const char *text) {
    irc_send_raw(conn, "PRIVMSG %s :%s", target, text);
}

void irc_send_join(NetConn *conn, const char *channel) {
    irc_send_raw(conn, "JOIN %s", channel);
}
