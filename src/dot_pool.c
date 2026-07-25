#include "dot_pool.h"

#include <gio/gio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#define MAX_CONNECTIONS 6
#define CONNECT_TIMEOUT_MS 5000
#define IO_TIMEOUT_MS 5000

typedef struct {
    int fd;
    SSL *ssl;
} DotConnection;

struct DotPool {
    DnsProvider *provider; /* owned deep copy, independent of the caller's settings lifetime */
    SSL_CTX *ssl_ctx;
    GAsyncQueue *gate;   /* MAX_CONNECTIONS tokens (GINT_TO_POINTER(1)) — bounds concurrency */
    GAsyncQueue *idle;   /* of DotConnection* */
    gint next_ip_index;
    gboolean disposed;
    gint ref_count;
};

static void dot_pool_free(DotPool *pool);

static void dot_connection_close(DotConnection *conn)
{
    if (!conn) return;
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->fd >= 0) close(conn->fd);
    g_free(conn);
}

static SSL_CTX *make_ssl_ctx(void)
{
    static gsize init = 0;
    if (g_once_init_enter(&init)) {
        SSL_library_init();
        g_once_init_leave(&init, 1);
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(ctx);
    return ctx;
}

DotPool *dot_pool_new(const DnsProvider *provider)
{
    DotPool *pool = g_new0(DotPool, 1);
    pool->provider = dns_provider_copy(provider);
    pool->ssl_ctx = make_ssl_ctx();
    pool->gate = g_async_queue_new();
    for (int i = 0; i < MAX_CONNECTIONS; i++) g_async_queue_push(pool->gate, GINT_TO_POINTER(1));
    pool->idle = g_async_queue_new();
    pool->ref_count = 1;
    return pool;
}

DotPool *dot_pool_ref(DotPool *pool)
{
    g_atomic_int_inc(&pool->ref_count);
    return pool;
}

void dot_pool_unref(DotPool *pool)
{
    if (!pool) return;
    if (g_atomic_int_dec_and_test(&pool->ref_count)) dot_pool_free(pool);
}

static gboolean connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) goto connected_blocking;
    if (errno != EINPROGRESS) return FALSE;

    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return FALSE;

    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) return FALSE;

connected_blocking:
    fcntl(fd, F_SETFL, flags); /* back to blocking for the TLS handshake + I/O below */
    struct timeval tv = { .tv_sec = IO_TIMEOUT_MS / 1000, .tv_usec = (IO_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return TRUE;
}

static DotConnection *open_connection(DotPool *pool, GError **error)
{
    guint idx = (guint)(g_atomic_int_add(&pool->next_ip_index, 1) + 1) % pool->provider->ip_count;
    const gchar *ip = pool->provider->ips[idx];

    gboolean is_v6 = strchr(ip, ':') != NULL;
    int fd = socket(is_v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "socket() failed: %s", g_strerror(errno));
        return NULL;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    gboolean connected;
    if (is_v6) {
        struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons((guint16)pool->provider->port) };
        inet_pton(AF_INET6, ip, &addr.sin6_addr);
        connected = connect_with_timeout(fd, (struct sockaddr *)&addr, sizeof(addr), CONNECT_TIMEOUT_MS);
    } else {
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((guint16)pool->provider->port) };
        inet_pton(AF_INET, ip, &addr.sin_addr);
        connected = connect_with_timeout(fd, (struct sockaddr *)&addr, sizeof(addr), CONNECT_TIMEOUT_MS);
    }
    if (!connected) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "TCP connect to %s:%d timed out/failed", ip, pool->provider->port);
        close(fd);
        return NULL;
    }

    SSL *ssl = SSL_new(pool->ssl_ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, pool->provider->tls_host);
    SSL_set1_host(ssl, pool->provider->tls_host);

    if (SSL_connect(ssl) != 1) {
        unsigned long e = ERR_get_error();
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "TLS handshake with %s failed: %s", pool->provider->tls_host, buf);
        SSL_free(ssl);
        close(fd);
        return NULL;
    }
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Certificate verification failed for %s", pool->provider->tls_host);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    DotConnection *conn = g_new0(DotConnection, 1);
    conn->fd = fd;
    conn->ssl = ssl;
    return conn;
}

static gboolean write_all(SSL *ssl, const guint8 *buf, gsize len)
{
    gsize off = 0;
    while (off < len) {
        int n = SSL_write(ssl, buf + off, (int)(len - off));
        if (n <= 0) return FALSE;
        off += (gsize)n;
    }
    return TRUE;
}

static gboolean read_exact(SSL *ssl, guint8 *buf, gsize len)
{
    gsize off = 0;
    while (off < len) {
        int n = SSL_read(ssl, buf + off, (int)(len - off));
        if (n <= 0) return FALSE; /* upstream closed mid-response, or a timeout */
        off += (gsize)n;
    }
    return TRUE;
}

static gboolean send_and_receive(DotConnection *conn, const guint8 *query, gsize query_len,
                                  guint8 **out_response, gsize *out_response_len)
{
    if (query_len > 0xFFFF) return FALSE;

    guint8 header[2] = { (guint8)(query_len >> 8), (guint8)(query_len & 0xFF) };
    if (!write_all(conn->ssl, header, 2)) return FALSE;
    if (!write_all(conn->ssl, query, query_len)) return FALSE;

    guint8 resp_header[2];
    if (!read_exact(conn->ssl, resp_header, 2)) return FALSE;
    gsize response_len = ((gsize)resp_header[0] << 8) | resp_header[1];

    guint8 *response = g_malloc(response_len > 0 ? response_len : 1);
    if (response_len > 0 && !read_exact(conn->ssl, response, response_len)) {
        g_free(response);
        return FALSE;
    }

    *out_response = response;
    *out_response_len = response_len;
    return TRUE;
}

gboolean dot_pool_forward(DotPool *pool, const guint8 *query, gsize query_len,
                           guint8 **out_response, gsize *out_response_len, GError **error)
{
    if (pool->disposed) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "DoT pool already disposed");
        return FALSE;
    }

    g_async_queue_pop(pool->gate);

    GError *last_error = NULL;
    gboolean ok = FALSE;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        DotConnection *conn = g_async_queue_try_pop(pool->idle);
        if (!conn) {
            g_clear_error(&last_error);
            conn = open_connection(pool, &last_error);
        }
        if (!conn) continue; /* couldn't even open a connection — try once more */

        ok = send_and_receive(conn, query, query_len, out_response, out_response_len);
        if (ok) {
            g_async_queue_push(pool->idle, conn);
        } else {
            /* Stale idle connection or a real failure — indistinguishable until used, so discard
             * and retry once against a guaranteed-fresh connection (mirrors DotConnectionPool.cs). */
            g_set_error(&last_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "DNS-over-TLS forward to %s failed", pool->provider->name);
            dot_connection_close(conn);
        }
    }

    g_async_queue_push(pool->gate, GINT_TO_POINTER(1));

    if (!ok && error) {
        *error = last_error;
    } else {
        g_clear_error(&last_error);
    }
    return ok;
}

static void dot_pool_free(DotPool *pool)
{
    if (!pool) return;
    pool->disposed = TRUE;
    DotConnection *conn;
    while ((conn = g_async_queue_try_pop(pool->idle)) != NULL) dot_connection_close(conn);
    g_async_queue_unref(pool->idle);
    g_async_queue_unref(pool->gate);
    SSL_CTX_free(pool->ssl_ctx);
    dns_provider_free(pool->provider);
    g_free(pool);
}
