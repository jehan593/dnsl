#define _GNU_SOURCE
#include "dns_proxy.h"
#include "dot_pool.h"
#include <gio/gio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define MAX_DNS_MESSAGE 65535
#define MAX_PENDING 128
#define WORKERS 12

typedef struct {
    gint refs;
    GMutex mutex;
    gboolean stopped;
    int fd[4]; /* UDP4, UDP6, TCP4, TCP6 */
    GThread *listeners[4];
    GThreadPool *workers;
    GArray *clients; /* accepted TCP sockets, owned and closed by their task */
    DotPool *pool;
    gint pending;
} ProxyRun;

struct DnsProxy { ProxyRun *run; };
typedef struct { ProxyRun *run; int slot; } ListenerArgs;
typedef struct {
    ProxyRun *run;
    int slot, client;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    guint8 *query;
    gsize query_len;
} QueryTask;

static void run_unref(ProxyRun *run)
{
    if (!g_atomic_int_dec_and_test(&run->refs)) return;
    dot_pool_unref(run->pool);
    g_array_free(run->clients, TRUE);
    g_mutex_clear(&run->mutex);
    g_free(run);
}

static gboolean transfer(int fd, guint8 *bytes, gsize length, gboolean writing)
{
    while (length) {
        ssize_t n = writing ? send(fd, bytes, length, MSG_NOSIGNAL) : recv(fd, bytes, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return FALSE;
        bytes += n;
        length -= n;
    }
    return TRUE;
}

static guint16 read_u16(const guint8 *p) { return ((guint16)p[0] << 8) | p[1]; }

/* Skip a wire name without following compression pointers. Bounds every read. */
static gboolean skip_name(const guint8 *q, gsize n, gsize *offset)
{
    while (*offset < n) {
        guint8 size = q[(*offset)++];
        if (!size) return TRUE;
        if ((size & 0xc0) == 0xc0) {
            if (*offset >= n) return FALSE;
            (*offset)++;
            return TRUE;
        }
        if (size > 63 || size > n - *offset) return FALSE;
        *offset += size;
    }
    return FALSE;
}

static gsize question_end(const guint8 *q, gsize n)
{
    if (n < 12) return 0;
    gsize offset = 12;
    for (guint i = 0; i < read_u16(q + 4); i++) {
        if (!skip_name(q, n, &offset) || n - offset < 4) return 0;
        offset += 4;
    }
    return offset;
}

static gsize udp_limit(const guint8 *q, gsize n)
{
    gsize offset = question_end(q, n);
    if (!offset) return 512;
    guint other = read_u16(q + 6) + read_u16(q + 8);
    guint total = other + read_u16(q + 10);
    for (guint i = 0; i < total; i++) {
        if (!skip_name(q, n, &offset) || n - offset < 10) return 512;
        guint16 type = read_u16(q + offset), payload = read_u16(q + offset + 2);
        guint16 size = read_u16(q + offset + 8);
        offset += 10;
        if (size > n - offset) return 512;
        if (i >= other && type == 41) return CLAMP(payload, 512, 65507);
        offset += size;
    }
    return 512;
}

static guint8 *short_response(const guint8 *query, gsize query_len,
                              guint8 flags_hi, guint8 flags_lo, gsize *length)
{
    *length = question_end(query, query_len);
    if (!*length || *length > 512) *length = 12;
    guint8 *response = g_memdup2(query, *length);
    response[2] = flags_hi;
    response[3] = flags_lo;
    memset(response + 6, 0, 6); /* no answer/authority/additional records */
    if (*length == 12) memset(response + 4, 0, 2);
    return response;
}

static gboolean forward(ProxyRun *run, const guint8 *query, gsize length,
                         guint8 **response, gsize *response_len)
{
    if (length < 12 || (query[2] & 0x80)) return FALSE;
    g_mutex_lock(&run->mutex);
    DotPool *pool = run->stopped ? NULL : dot_pool_ref(run->pool);
    g_mutex_unlock(&run->mutex);
    if (!pool) return FALSE;
    GError *error = NULL;
    gboolean ok = dot_pool_forward(pool, query, length, response, response_len, &error);
    dot_pool_unref(pool);
    g_clear_error(&error);
    if (!ok) {
        *response = short_response(query, length, 0x80 | (query[2] & 0x79),
                                   0x82, response_len); /* RA + SERVFAIL */
    }
    return TRUE;
}

static void process_task(gpointer data, gpointer unused)
{
    (void)unused;
    QueryTask *task = data;
    ProxyRun *run = task->run;
    if (task->client < 0) {
        guint8 *response = NULL;
        gsize length = 0;
        if (forward(run, task->query, task->query_len, &response, &length)) {
            if (length > udp_limit(task->query, task->query_len)) {
                guint8 hi = response[2] | 0x82, lo = response[3];
                g_free(response);
                response = short_response(task->query, task->query_len, hi, lo, &length);
            }
            g_mutex_lock(&run->mutex);
            if (!run->stopped)
                sendto(run->fd[task->slot], response, length, 0,
                       (struct sockaddr *)&task->addr, task->addr_len);
            g_mutex_unlock(&run->mutex);
            g_free(response);
        }
    } else {
        /* DNS over TCP permits several framed queries on one connection. */
        guint8 header[2];
        while (transfer(task->client, header, 2, FALSE)) {
            gsize length = ((gsize)header[0] << 8) | header[1];
            if (length < 12) break;
            guint8 *query = g_malloc(length), *response = NULL;
            gsize response_len = 0;
            gboolean ok = transfer(task->client, query, length, FALSE) &&
                          forward(run, query, length, &response, &response_len);
            g_free(query);
            if (ok) {
                header[0] = response_len >> 8;
                header[1] = response_len & 255;
                ok = transfer(task->client, header, 2, TRUE) &&
                     transfer(task->client, response, response_len, TRUE);
            }
            g_free(response);
            if (!ok) break;
        }
        g_mutex_lock(&run->mutex);
        for (guint i = 0; i < run->clients->len; i++) {
            if (g_array_index(run->clients, int, i) == task->client) {
                g_array_remove_index_fast(run->clients, i);
                break;
            }
        }
        close(task->client);
        g_mutex_unlock(&run->mutex);
    }
    g_free(task->query);
    g_free(task);
    g_atomic_int_add(&run->pending, -1);
    run_unref(run);
}

static gpointer listener_loop(gpointer data)
{
    ListenerArgs *args = data;
    ProxyRun *run = args->run;
    int slot = args->slot, fd = run->fd[slot];
    g_free(args);
    guint8 buf[MAX_DNS_MESSAGE];
    while (TRUE) {
        QueryTask *task = g_new0(QueryTask, 1);
        task->run = run;
        task->slot = slot;
        task->client = -1;
        task->addr_len = sizeof(task->addr);
        ssize_t n;
        if (slot < 2) {
            n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&task->addr, &task->addr_len);
            if (n >= 12) { task->query = g_memdup2(buf, n); task->query_len = n; }
        } else {
            task->client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
            n = task->client;
            if (n >= 0) {
                struct timeval timeout = { .tv_sec = 5 };
                setsockopt(task->client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(task->client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            }
        }
        int saved_errno = errno;
        g_mutex_lock(&run->mutex);
        gboolean stopped = run->stopped;
        gboolean accepted = !stopped && n >= 0 && (slot >= 2 || n >= 12) &&
                            g_atomic_int_get(&run->pending) < MAX_PENDING;
        if (accepted) {
            if (task->client >= 0) g_array_append_val(run->clients, task->client);
            g_atomic_int_inc(&run->pending);
            g_atomic_int_inc(&run->refs);
            g_thread_pool_push(run->workers, task, NULL);
        }
        g_mutex_unlock(&run->mutex);
        if (!accepted) {
            if (task->client >= 0) close(task->client);
            g_free(task->query);
            g_free(task);
        }
        if (stopped || (n < 0 && saved_errno != EINTR)) break;
    }
    return NULL;
}

DnsProxy *dns_proxy_new(void) { return g_new0(DnsProxy, 1); }

static int bind_listener(int family, int type, int port)
{
    int fd = socket(family, type | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    if (type == SOCK_STREAM) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    int rc;
    if (family == AF_INET) {
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port),
                                    .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    } else {
        struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons(port),
                                    .sin6_addr = IN6ADDR_LOOPBACK_INIT };
        rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    }
    if (rc == 0 && type == SOCK_STREAM) rc = listen(fd, 32);
    if (rc < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    return fd;
}

gboolean dns_proxy_start(DnsProxy *proxy, const DnsProvider *provider, int port, GError **error)
{
    if (proxy->run) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Proxy is already running.");
        return FALSE;
    }
    if (!provider->ip_count || provider->port < 1 || provider->port > 65535 || provider->port == 53) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The DoT provider needs an IP address and a TLS port other than 53.");
        return FALSE;
    }
    ProxyRun *run = g_new0(ProxyRun, 1);
    g_mutex_init(&run->mutex);
    run->refs = 1;
    run->clients = g_array_new(FALSE, FALSE, sizeof(int));
    for (int i = 0; i < 4; i++) run->fd[i] = -1;
    for (int i = 0; i < 4; i++) {
        run->fd[i] = bind_listener(i % 2 ? AF_INET6 : AF_INET, i < 2 ? SOCK_DGRAM : SOCK_STREAM, port);
        if (run->fd[i] < 0) {
            if (i % 2 && (errno == EAFNOSUPPORT || errno == EADDRNOTAVAIL)) continue;
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                        "Couldn't bind %s %s:%d: %s", i < 2 ? "UDP" : "TCP",
                        i % 2 ? "[::1]" : "127.0.0.1", port, g_strerror(errno));
            for (int j = 0; j < 4; j++) if (run->fd[j] >= 0) close(run->fd[j]);
            run_unref(run);
            return FALSE;
        }
    }
    run->pool = dot_pool_new(provider);
    run->workers = g_thread_pool_new(process_task, NULL, WORKERS, FALSE, error);
    if (!run->workers) {
        for (int i = 0; i < 4; i++) if (run->fd[i] >= 0) close(run->fd[i]);
        run_unref(run);
        return FALSE;
    }
    proxy->run = run;
    for (int i = 0; i < 4; i++) {
        if (run->fd[i] < 0) continue;
        ListenerArgs *args = g_new0(ListenerArgs, 1);
        args->run = run;
        args->slot = i;
        run->listeners[i] = g_thread_new("dnsl-listen", listener_loop, args);
    }
    return TRUE;
}

void dns_proxy_switch_provider(DnsProxy *proxy, const DnsProvider *provider)
{
    if (!proxy->run) return;
    DotPool *pool = dot_pool_new(provider);
    g_mutex_lock(&proxy->run->mutex);
    DotPool *old = proxy->run->pool;
    proxy->run->pool = pool;
    g_mutex_unlock(&proxy->run->mutex);
    dot_pool_cancel(old);
    dot_pool_unref(old);
}

gboolean dns_proxy_is_running(DnsProxy *proxy) { return proxy->run != NULL; }

void dns_proxy_stop(DnsProxy *proxy)
{
    ProxyRun *run = proxy->run;
    if (!run) return;
    proxy->run = NULL;
    g_mutex_lock(&run->mutex);
    run->stopped = TRUE;
    dot_pool_cancel(run->pool);
    for (int i = 0; i < 4; i++) if (run->fd[i] >= 0) shutdown(run->fd[i], SHUT_RDWR);
    for (guint i = 0; i < run->clients->len; i++)
        shutdown(g_array_index(run->clients, int, i), SHUT_RDWR);
    g_mutex_unlock(&run->mutex);
    for (int i = 0; i < 4; i++) {
        if (run->listeners[i]) g_thread_join(run->listeners[i]);
        if (run->fd[i] >= 0) close(run->fd[i]);
    }
    /* In-flight upstream requests own the old run. They cannot send replies or
     * touch new sockets after stop, and do not delay restoring normal DNS. */
    g_thread_pool_free(run->workers, FALSE, FALSE);
    run_unref(run);
}

void dns_proxy_free(DnsProxy *proxy)
{
    if (!proxy) return;
    dns_proxy_stop(proxy);
    g_free(proxy);
}
