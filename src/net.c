#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/err.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool net_connect(NetConn *conn, const char *host, const char *port, bool use_tls) {
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->use_tls = use_tls;

    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) return false;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        conn->fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (conn->fd == -1) continue;
        if (connect(conn->fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(conn->fd);
        conn->fd = -1;
    }
    freeaddrinfo(res);

    if (conn->fd == -1) return false;

    if (use_tls) {
        conn->ctx = SSL_CTX_new(TLS_client_method());
        if (!conn->ctx) {
            close(conn->fd);
            return false;
        }
        SSL_CTX_set_default_verify_paths(conn->ctx);
        SSL_CTX_set_verify(conn->ctx, SSL_VERIFY_PEER, NULL);

        conn->ssl = SSL_new(conn->ctx);
        SSL_set_fd(conn->ssl, conn->fd);
        SSL_set_tlsext_host_name(conn->ssl, host);

        if (SSL_connect(conn->ssl) <= 0) {
            net_close(conn);
            return false;
        }
    }

    set_nonblocking(conn->fd);
    conn->connected = true;
    return true;
}

ssize_t net_read(NetConn *conn, char *buf, size_t len) {
    if (!conn->connected) return -1;
    if (conn->use_tls) {
        int r = SSL_read(conn->ssl, buf, (int)len);
        if (r > 0) return r;
        int err = SSL_get_error(conn->ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
        return -1;
    }
    ssize_t r = read(conn->fd, buf, len);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return r;
}

ssize_t net_write(NetConn *conn, const char *buf, size_t len) {
    if (!conn->connected) return -1;
    if (conn->use_tls) {
        int w = SSL_write(conn->ssl, buf, (int)len);
        return w > 0 ? w : -1;
    }
    return write(conn->fd, buf, len);
}

void net_close(NetConn *conn) {
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->ctx) SSL_CTX_free(conn->ctx);
    if (conn->fd != -1) close(conn->fd);
    conn->connected = false;
}
