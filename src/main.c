#include "irc.h"
#include "net.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void handle_irc_message(NetConn *conn, IrcMessage *msg) {
    if (strcmp(msg->command, "PING") == 0 && msg->param_count > 0) {
        irc_send_raw(conn, "PONG :%s", msg->params[0]);
        return;
    }

    if (strcmp(msg->command, "CAP") == 0 && msg->param_count >= 2) {
        if (strcmp(msg->params[1], "LS") == 0) {
            irc_send_raw(conn, "CAP REQ :server-time message-tags");
        } else if (strcmp(msg->params[1], "ACK") == 0 || strcmp(msg->params[1], "NAK") == 0) {
            irc_send_raw(conn, "CAP END");
        }
        return;
    }

    if (strcmp(msg->command, "PRIVMSG") == 0 && msg->param_count >= 2) {
        printf("[%s] <%s> %s\n", msg->params[0], msg->nick ? msg->nick : "?", msg->params[1]);
    } else {
        printf("[SRV] %s", msg->command);
        for (int i = 0; i < msg->param_count; i++) {
            printf(" | %s", msg->params[i]);
        }
        printf("\n");
    }
    fflush(stdout);
}

int main(void) {
    NetConn conn;
    if (!net_connect(&conn, "irc.libera.chat", "6697", true)) {
        fprintf(stderr, "lic: connection failed\n");
        return 1;
    }

    irc_send_handshake(&conn, "lic_dev_test", "lic", "Lunar IRC Client");

    IrcLineBuf line_buf = {0};
    struct pollfd pfds[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = conn.fd,      .events = POLLIN}
    };

    char raw_buf[4096];
    char line[IRC_MAX_LINE];

    while (conn.connected) {
        if (poll(pfds, 2, -1) < 0) break;

        if (pfds[1].revents & POLLIN) {
            ssize_t n = net_read(&conn, raw_buf, sizeof(raw_buf));
            if (n < 0) break;
            if (n > 0) {
                irc_buf_push(&line_buf, raw_buf, (size_t)n);
                while (irc_buf_pop_line(&line_buf, line, sizeof(line))) {
                    IrcMessage msg;
                    if (irc_parse_line(line, &msg)) {
                        handle_irc_message(&conn, &msg);
                    }
                }
            }
        }

        if (pfds[0].revents & POLLIN) {
            if (fgets(raw_buf, sizeof(raw_buf), stdin)) {
                size_t len = strlen(raw_buf);
                if (len > 0 && raw_buf[len - 1] == '\n') raw_buf[len - 1] = '\0';
                irc_send_raw(&conn, "%s", raw_buf);
            }
        }
    }

    net_close(&conn);
    return 0;
}
