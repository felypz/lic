#ifndef NET_H
#define NET_H

#include <openssl/ssl.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    bool use_tls;
    bool connected;
} NetConn;

bool net_connect(NetConn *conn, const char *host, const char *port, bool use_tls);
ssize_t net_read(NetConn *conn, char *buf, size_t len);
ssize_t net_write(NetConn *conn, const char *buf, size_t len);
void net_close(NetConn *conn);

#endif
