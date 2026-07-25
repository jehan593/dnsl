#include "dns_proxy.h"
#include "dot_pool.h"

#include <gio/gio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define MAX_DNS_MESSAGE 4096

struct DnsProxy {
    int fd_v4;
    int fd_v6;
    GThread *thread_v4;
    GThread *thread_v6;
    GMutex pool_mutex;
    DotPool *pool; /* current pool, one ref held by the proxy itself */
    gboolean running;
};

typedef struct {
    DnsProxy *proxy;
    int fd;
} ListenerArgs;

typedef struct {
    DotPool *pool; /* ref taken for this task, released when done */
    int fd;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    guint8 *query;
    gsize query_len;
} QueryTask;

static gpointer handle_query_thread(gpointer data)
{
    QueryTask *task = data;

    guint8 *response = NULL;
    gsize response_len = 0;
    GError *error = NULL;
    if (dot_pool_forward(task->pool, task->query, task->query_len, &response, &response_len, &error)) {
        sendto(task->fd, response, response_len, 0, (struct sockaddr *)&task->addr, task->addr_len);
        g_free(response);
    } else {
        /* A dropped query looks like an ordinary DNS timeout to whatever asked, which already
         * knows how to retry/fail over — nothing further to do. */
        g_clear_error(&error);
    }

    dot_pool_unref(task->pool);
    g_free(task->query);
    g_free(task);
    return NULL;
}

static gpointer listener_loop(gpointer data)
{
    ListenerArgs *args = data;
    DnsProxy *proxy = args->proxy;
    int fd = args->fd;
    g_free(args);

    guint8 buf[MAX_DNS_MESSAGE];
    while (TRUE) {
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addr_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            /* EBADF (socket closed under us by dns_proxy_stop) or any other error ends the loop —
             * transient errors on a UDP socket are rare enough not to warrant a continue-loop. */
            return NULL;
        }

        g_mutex_lock(&proxy->pool_mutex);
        DotPool *pool = proxy->pool ? dot_pool_ref(proxy->pool) : NULL;
        g_mutex_unlock(&proxy->pool_mutex);
        if (!pool) continue; /* stopping */

        QueryTask *task = g_new0(QueryTask, 1);
        task->pool = pool;
        task->fd = fd;
        task->addr = addr;
        task->addr_len = addr_len;
        task->query = g_memdup2(buf, (gsize)n);
        task->query_len = (gsize)n;

        /* Fire-and-forget, same as DnsProxyServer.cs's `_ = HandleQueryAsync(...)` — dot_pool's own
         * gate (MAX_CONNECTIONS) bounds real concurrency; this just lets independent queries not
         * wait on each other's TLS round trip. */
        GThread *worker = g_thread_new("dnsl-query", handle_query_thread, task);
        g_thread_unref(worker);
    }
}

DnsProxy *dns_proxy_new(void)
{
    DnsProxy *proxy = g_new0(DnsProxy, 1);
    proxy->fd_v4 = -1;
    proxy->fd_v6 = -1;
    g_mutex_init(&proxy->pool_mutex);
    return proxy;
}

static int bind_udp(int family, int port)
{
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    if (family == AF_INET) {
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((guint16)port) };
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    } else {
        struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons((guint16)port) };
        addr.sin6_addr = in6addr_loopback;
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    }
    return fd;
}

gboolean dns_proxy_start(DnsProxy *proxy, const DnsProvider *provider, int port, GError **error)
{
    if (proxy->running) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Proxy is already running.");
        return FALSE;
    }

    proxy->fd_v4 = bind_udp(AF_INET, port);
    if (proxy->fd_v4 < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Couldn't bind 127.0.0.1:%d: %s (something else may already be using it)", port, g_strerror(errno));
        return FALSE;
    }
    /* IPv6 disabled/unavailable on this machine isn't fatal — the IPv4 listener alone still
     * protects every IPv4 DNS query. */
    proxy->fd_v6 = bind_udp(AF_INET6, port);

    proxy->pool = dot_pool_new(provider);
    proxy->running = TRUE;

    ListenerArgs *args_v4 = g_new0(ListenerArgs, 1);
    args_v4->proxy = proxy;
    args_v4->fd = proxy->fd_v4;
    proxy->thread_v4 = g_thread_new("dnsl-listen4", listener_loop, args_v4);

    if (proxy->fd_v6 >= 0) {
        ListenerArgs *args_v6 = g_new0(ListenerArgs, 1);
        args_v6->proxy = proxy;
        args_v6->fd = proxy->fd_v6;
        proxy->thread_v6 = g_thread_new("dnsl-listen6", listener_loop, args_v6);
    }

    return TRUE;
}

void dns_proxy_switch_provider(DnsProxy *proxy, const DnsProvider *provider)
{
    if (!proxy->running) return;
    DotPool *new_pool = dot_pool_new(provider);

    g_mutex_lock(&proxy->pool_mutex);
    DotPool *old_pool = proxy->pool;
    proxy->pool = new_pool;
    g_mutex_unlock(&proxy->pool_mutex);

    dot_pool_unref(old_pool);
}

gboolean dns_proxy_is_running(DnsProxy *proxy)
{
    return proxy->running;
}

void dns_proxy_stop(DnsProxy *proxy)
{
    if (!proxy->running) return;
    proxy->running = FALSE;

    g_mutex_lock(&proxy->pool_mutex);
    DotPool *pool = proxy->pool;
    proxy->pool = NULL;
    g_mutex_unlock(&proxy->pool_mutex);

    /* shutdown() BEFORE close() is required here, not just belt-and-suspenders: a plain close()
     * from this thread does NOT reliably unblock a *different* thread already parked inside a
     * blocking recvfrom() on the same fd — the kernel keeps the underlying socket alive as long
     * as that other thread's syscall is in flight, so the listener thread's recvfrom() would
     * simply never return and the g_thread_join() below would hang forever (confirmed by hand:
     * this exact omission deadlocked a live daemon process on first real end-to-end testing,
     * wedged holding protection_controller's mutex — every other IPC command hung too, and even
     * SIGTERM couldn't recover it since the terminate handler blocks on the same mutex). shutdown()
     * operates on the socket object itself, not just this thread's fd reference, and reliably
     * wakes a blocked recvfrom() in any thread — same effect .NET's UdpClient.Dispose() gets for
     * free via its own cancellation-aware ReceiveAsync. */
    if (proxy->fd_v4 >= 0) { shutdown(proxy->fd_v4, SHUT_RDWR); close(proxy->fd_v4); proxy->fd_v4 = -1; }
    if (proxy->fd_v6 >= 0) { shutdown(proxy->fd_v6, SHUT_RDWR); close(proxy->fd_v6); proxy->fd_v6 = -1; }

    if (proxy->thread_v4) { g_thread_join(proxy->thread_v4); proxy->thread_v4 = NULL; }
    if (proxy->thread_v6) { g_thread_join(proxy->thread_v6); proxy->thread_v6 = NULL; }

    dot_pool_unref(pool);
}

void dns_proxy_free(DnsProxy *proxy)
{
    if (!proxy) return;
    dns_proxy_stop(proxy);
    g_mutex_clear(&proxy->pool_mutex);
    g_free(proxy);
}
