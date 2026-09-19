#include "intercept_ctl.h"
#include <gio/gio.h>
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

static gboolean run_nft(const gchar *rules, GError **error)
{
    GSubprocess *child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        error, "nft", "-f", "-", NULL);
    if (!child) return FALSE;
    gchar *diagnostic = NULL;
    gboolean ok = g_subprocess_communicate_utf8(child, rules, NULL, NULL, &diagnostic, error);
    if (ok && !g_subprocess_get_successful(child)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DNS interception: %s",
                    diagnostic && *diagnostic ? diagnostic : "nft failed");
        ok = FALSE;
    }
    g_free(diagnostic);
    g_object_unref(child);
    return ok;
}

typedef struct {
    gboolean enabling;
    struct ifaddrs *addresses;
    GPtrArray *flows;
} FlowScan;

static gboolean source_is_local(const struct nf_conntrack *ct, struct ifaddrs *addresses)
{
    int family = nfct_get_attr_u8(ct, ATTR_L3PROTO);
    const void *source = nfct_get_attr(ct, family == AF_INET ? ATTR_IPV4_SRC : ATTR_IPV6_SRC);
    if (!source) return FALSE;
    if (family == AF_INET && (ntohl(nfct_get_attr_u32(ct, ATTR_IPV4_SRC)) >> 24) == 127)
        return TRUE;
    for (struct ifaddrs *ifa = addresses; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != family) continue;
        const void *addr = family == AF_INET
            ? (const void *)&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr
            : (const void *)&((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
        if (memcmp(source, addr, family == AF_INET ? 4 : 16) == 0) return TRUE;
    }
    return FALSE;
}

static gboolean is_our_flow(const struct nf_conntrack *ct)
{
    if (nfct_get_attr_u16(ct, ATTR_REPL_PORT_SRC) != htons(DNSL_PROXY_PORT)) return FALSE;
    int family = nfct_get_attr_u8(ct, ATTR_L3PROTO);
    if (family == AF_INET)
        return nfct_get_attr_u32(ct, ATTR_REPL_IPV4_SRC) == htonl(INADDR_LOOPBACK);
    const void *ip = nfct_get_attr(ct, ATTR_REPL_IPV6_SRC);
    return family == AF_INET6 && ip && memcmp(ip, &in6addr_loopback, 16) == 0;
}

static int collect_flow(enum nf_conntrack_msg_type type, struct nf_conntrack *ct, void *data)
{
    (void)type;
    FlowScan *scan = data;
    guint8 protocol = nfct_get_attr_u8(ct, ATTR_L4PROTO);
    if ((protocol != IPPROTO_UDP && protocol != IPPROTO_TCP) ||
        nfct_get_attr_u16(ct, ATTR_PORT_DST) != htons(53)) return NFCT_CB_CONTINUE;
    gboolean ours = is_our_flow(ct);
    /* On enable invalidate old direct DNS sessions, but don't invalidate newly
     * intercepted sessions created after the atomic rule installation. On disable
     * remove only our translations, even if their source interface has gone away. */
    if ((scan->enabling && !ours && source_is_local(ct, scan->addresses)) ||
        (!scan->enabling && ours))
        g_ptr_array_add(scan->flows, nfct_clone(ct));
    return NFCT_CB_CONTINUE;
}

static gboolean clear_flows(gboolean enabling, GError **error)
{
    FlowScan scan = { .enabling = enabling,
        .flows = g_ptr_array_new_with_free_func((GDestroyNotify)nfct_destroy) };
    struct nfct_handle *handle = nfct_open(CONNTRACK, 0);
    gboolean ok = FALSE;
    if (!handle) goto done;
    struct timeval timeout = { .tv_sec = 3 };
    setsockopt(nfct_fd(handle), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (enabling && getifaddrs(&scan.addresses) != 0) goto done;
    nfct_callback_register(handle, NFCT_T_ALL, collect_flow, &scan);
    guint32 family = AF_UNSPEC;
    if (nfct_query(handle, NFCT_Q_DUMP, &family) < 0) goto done;
    nfct_callback_unregister(handle);
    for (guint i = 0; i < scan.flows->len; i++) {
        if (nfct_query(handle, NFCT_Q_DESTROY, g_ptr_array_index(scan.flows, i)) < 0 && errno != ENOENT)
            goto done;
    }
    ok = TRUE;
done:
    if (!ok) g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                         "Couldn't clear DNS connection tracking: %s", g_strerror(errno));
    if (handle) nfct_close(handle);
    if (scan.addresses) freeifaddrs(scan.addresses);
    g_ptr_array_free(scan.flows, TRUE);
    return ok;
}

gboolean intercept_ctl_disable(GError **error)
{
    /* add is idempotent; the batch removes an absent table successfully too.
     * Never flush the machine's ruleset or any VPN-owned table. */
    if (!run_nft("add table inet " DNSL_NFT_TABLE "\n"
                 "delete table inet " DNSL_NFT_TABLE "\n", error)) return FALSE;
    return clear_flows(FALSE, error);
}

gboolean intercept_ctl_enable(GError **error)
{
    /* Replacement is one netlink transaction; IPv4/IPv6 and TCP/UDP activate
     * together. No interface names, route changes, or VPN-specific exceptions. */
    const gchar *rules =
        "add table inet " DNSL_NFT_TABLE "\n"
        "delete table inet " DNSL_NFT_TABLE "\n"
        "add table inet " DNSL_NFT_TABLE "\n"
        "add chain inet " DNSL_NFT_TABLE " output { type nat hook output priority -101; policy accept; }\n"
        "add rule inet " DNSL_NFT_TABLE " output udp dport 53 redirect to :" G_STRINGIFY(DNSL_PROXY_PORT) "\n"
        "add rule inet " DNSL_NFT_TABLE " output tcp dport 53 redirect to :" G_STRINGIFY(DNSL_PROXY_PORT) "\n";
    if (!run_nft(rules, error)) return FALSE;
    return clear_flows(TRUE, error);
}

void intercept_ctl_flush_cache(void)
{
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (bus) {
        GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.resolve1",
            "/org/freedesktop/resolve1", "org.freedesktop.resolve1.Manager", "FlushCaches",
            NULL, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, &error);
        if (reply) g_variant_unref(reply);
        g_object_unref(bus);
    }
    g_clear_error(&error);
}

int intercept_ctl_cleanup_entry(void)
{
    if (geteuid() != 0) { fprintf(stderr, "DNS cleanup requires root.\n"); return 1; }
    GError *error = NULL;
    if (!intercept_ctl_disable(&error)) {
        fprintf(stderr, "%s\n", error->message);
        g_clear_error(&error);
        return 1;
    }
    intercept_ctl_flush_cache();
    return 0;
}
