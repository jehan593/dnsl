#include "resolved_ctl.h"

#include <gio/gio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define RESOLVE1_BUS_NAME  "org.freedesktop.resolve1"
#define RESOLVE1_OBJ_PATH  "/org/freedesktop/resolve1"
#define RESOLVE1_INTERFACE "org.freedesktop.resolve1.Manager"
#define RESOLVE1_LINK_IFACE "org.freedesktop.resolve1.Link"

typedef struct {
    gint ifindex;
    gchar name[IF_NAMESIZE];
} ActiveLink;

/* A link's pre-redirect config, restored verbatim on disable. */
struct LinkConfigSnapshot {
    gint ifindex;
    GVariant *dns;        /* a(iay): the link's real DNS servers, e.g. Windscribe's tunnel DNS */
    GVariant *domains;    /* a(sb): the link's routing/search domains pre-redirect */
    gboolean default_route;
    gint64 first_seen_mono; /* g_get_monotonic_time() when the link was first captured */
    gboolean settled;      /* a real owner config was captured (non-empty DNS), or grace expired */
};

/* Grace period for new links — VPN clients need a few seconds to push DNS after bringing
 * the interface up. 6s covers Windscribe's typical delay. */
#define NEW_LINK_GRACE_MS 6000
#define NEW_LINK_GRACE_USEC (NEW_LINK_GRACE_MS * 1000)

static gboolean dns_is_empty(GVariant *dns)
{
    return dns == NULL || g_variant_n_children(dns) == 0;
}

/* All up, non-loopback links — including VPN tunnels. Tunnels must be redirected too, or a VPN's
 * higher-priority DNS wins over the proxy on the physical link. Only DNS config is touched,
 * never routes. */
static GArray *get_active_links(void)
{
    GArray *result = g_array_new(FALSE, FALSE, sizeof(ActiveLink));
    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) != 0) return result;

    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        unsigned idx = if_nametoindex(ifa->ifa_name);
        if (idx == 0 || g_hash_table_contains(seen, GUINT_TO_POINTER(idx))) continue;
        g_hash_table_add(seen, GUINT_TO_POINTER(idx));

        ActiveLink link = { .ifindex = (gint)idx };
        g_strlcpy(link.name, ifa->ifa_name, sizeof(link.name));
        g_array_append_val(result, link);
    }
    g_hash_table_destroy(seen);
    freeifaddrs(ifaddr);
    return result;
}

/* NM doesn't re-push DNS after RevertLink(). `nmcli general reload dns-rc` unconditionally
 * re-pushes NM's DNS state everywhere. Runs once per restore batch, not per-link. */
static void nudge_network_manager_dns_reload(void)
{
    if (!g_find_program_in_path("nmcli")) return;
    const gchar *argv[] = { "nmcli", "general", "reload", "dns-rc", NULL };
    g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, NULL, NULL);
}

static GDBusConnection *get_system_bus(GError **error)
{
    return g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
}

static void append_proxy_address(gint family, const guint8 *bytes, gsize nbytes, GVariantBuilder *addrs)
{
    GVariantBuilder bytes_builder;
    g_variant_builder_init(&bytes_builder, G_VARIANT_TYPE("ay"));
    for (gsize i = 0; i < nbytes; i++) g_variant_builder_add(&bytes_builder, "y", bytes[i]);
    g_variant_builder_add(addrs, "(i@ay)", family, g_variant_builder_end(&bytes_builder));
}

static GVariant *proxy_redirect_dns_variant(void)
{
    GVariantBuilder addrs;
    g_variant_builder_init(&addrs, G_VARIANT_TYPE("a(iay)"));
    { guint8 v4[4] = { 127, 0, 0, 1 }; append_proxy_address(AF_INET, v4, 4, &addrs); }
    { guint8 v6[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }; append_proxy_address(AF_INET6, v6, 16, &addrs); }
    return g_variant_builder_end(&addrs);
}

static gboolean call_set_link_dns(GDBusConnection *bus, gint ifindex, GError **error)
{
    GVariant *addrs = proxy_redirect_dns_variant();

    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, RESOLVE1_OBJ_PATH, RESOLVE1_INTERFACE,
        "SetLinkDNS", g_variant_new("(i@a(iay))", ifindex, addrs),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (result) g_variant_unref(result);
    return result != NULL;
}

static gboolean call_set_link_domains_route_all(GDBusConnection *bus, gint ifindex, GError **error)
{
    /* A domain of "." with routing_only=TRUE is systemd-resolved's "~." marker (same as
     * `resolvectl domain <link> '~.'`) — makes this link the resolver of last resort for every
     * name, not just names under some suffix. */
    GVariantBuilder domains;
    g_variant_builder_init(&domains, G_VARIANT_TYPE("a(sb)"));
    g_variant_builder_add(&domains, "(sb)", ".", TRUE);

    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, RESOLVE1_OBJ_PATH, RESOLVE1_INTERFACE,
        "SetLinkDomains", g_variant_new("(i@a(sb))", ifindex, g_variant_builder_end(&domains)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (result) g_variant_unref(result);
    return result != NULL;
}

static gboolean call_revert_link(GDBusConnection *bus, gint ifindex, GError **error)
{
    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, RESOLVE1_OBJ_PATH, RESOLVE1_INTERFACE,
        "RevertLink", g_variant_new("(i)", ifindex),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (result) g_variant_unref(result);
    return result != NULL;
}

/* Value-based SetLinkDNS/SetLinkDomains with caller-supplied config (for restoring snapshots). */
static gboolean call_set_link_dns_values(GDBusConnection *bus, gint ifindex, GVariant *dns, GError **error)
{
    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, RESOLVE1_OBJ_PATH, RESOLVE1_INTERFACE,
        "SetLinkDNS", g_variant_new("(i@a(iay))", ifindex, g_variant_ref(dns)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (result) g_variant_unref(result);
    return result != NULL;
}

static gboolean call_set_link_domains_values(GDBusConnection *bus, gint ifindex, GVariant *domains, GError **error)
{
    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, RESOLVE1_OBJ_PATH, RESOLVE1_INTERFACE,
        "SetLinkDomains", g_variant_new("(i@a(sb))", ifindex, g_variant_ref(domains)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (result) g_variant_unref(result);
    return result != NULL;
}

static gboolean call_set_link_default_route(GDBusConnection *bus, gint ifindex, gboolean default_route, GError **error)
{
    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, RESOLVE1_OBJ_PATH, RESOLVE1_INTERFACE,
        "SetLinkDefaultRoute", g_variant_new("(ib)", ifindex, default_route),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
    if (result) g_variant_unref(result);
    return result != NULL;
}

/* GetLink returns the link's object path; DNS/Domains/DefaultRoute are read via Properties.Get. */
static gchar *get_link_object_path(GDBusConnection *bus, gint ifindex)
{
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, RESOLVE1_OBJ_PATH, RESOLVE1_INTERFACE,
        "GetLink", g_variant_new("(i)", ifindex),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (!result) { g_clear_error(&error); return NULL; }
    GVariant *child = g_variant_get_child_value(result, 0);
    gchar *path = g_strdup(g_variant_get_string(child, NULL));
    g_variant_unref(child);
    g_variant_unref(result);
    return path;
}

static GVariant *get_link_property(GDBusConnection *bus, const gchar *object_path, const gchar *property,
                                   const GVariantType *expected_type)
{
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, object_path, "org.freedesktop.DBus.Properties",
        "Get", g_variant_new("(ss)", RESOLVE1_LINK_IFACE, property),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (!result) { g_clear_error(&error); return NULL; }
    GVariant *container = g_variant_get_child_value(result, 0);
    GVariant *value = g_variant_get_variant(container);
    g_variant_unref(container);
    g_variant_unref(result);
    if (!g_variant_is_of_type(value, expected_type)) { g_variant_unref(value); return NULL; }
    return value;
}

static gboolean read_link_config(GDBusConnection *bus, gint ifindex,
                                 GVariant **dns_out, GVariant **domains_out, gboolean *default_route_out)
{
    gchar *object_path = get_link_object_path(bus, ifindex);
    if (!object_path) return FALSE;

    GVariant *dns = get_link_property(bus, object_path, "DNS", G_VARIANT_TYPE("a(iay)"));
    GVariant *domains = get_link_property(bus, object_path, "Domains", G_VARIANT_TYPE("a(sb)"));
    if (!dns || !domains) {
        if (dns) g_variant_unref(dns);
        if (domains) g_variant_unref(domains);
        g_free(object_path);
        return FALSE;
    }

    gboolean default_route = FALSE;
    GVariant *dr = get_link_property(bus, object_path, "DefaultRoute", G_VARIANT_TYPE("b"));
    if (dr) { default_route = g_variant_get_boolean(dr); g_variant_unref(dr); }
    g_free(object_path);

    *dns_out = dns;
    *domains_out = domains;
    *default_route_out = default_route;
    return TRUE;
}

/* TRUE when DNS is exactly {127.0.0.1, ::1} — our own redirect, not anyone's real config. */
static gboolean is_proxy_redirect_dns(GVariant *dns)
{
    if (!dns || !g_variant_is_of_type(dns, G_VARIANT_TYPE("a(iay)"))) return FALSE;

    gboolean saw_v4 = FALSE, saw_v6 = FALSE;
    GVariantIter iter;
    g_variant_iter_init(&iter, dns);
    GVariant *entry = NULL;
    while ((entry = g_variant_iter_next_value(&iter)) != NULL) {
        GVariant *addr = g_variant_get_child_value(entry, 1);
        gsize n = 0;
        const guint8 *bytes = g_variant_get_fixed_array(addr, &n, 1);
        if (n == 4 && bytes[0] == 127 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1) {
            saw_v4 = TRUE;
        } else if (n == 16) {
            gboolean is_loopback6 = TRUE;
            for (gsize i = 0; i < 15; i++) { if (bytes[i] != 0) { is_loopback6 = FALSE; break; } }
            if (is_loopback6 && bytes[15] == 1) saw_v6 = TRUE;
            else { g_variant_unref(addr); g_variant_unref(entry); return FALSE; }
        } else {
            g_variant_unref(addr); g_variant_unref(entry); return FALSE;
        }
        g_variant_unref(addr);
        g_variant_unref(entry);
    }
    return saw_v4 && saw_v6;
}

static LinkConfigSnapshot *capture_link_snapshot(gint ifindex,
                                                 GVariant *dns, GVariant *domains, gboolean default_route)
{
    LinkConfigSnapshot *snap = g_new0(LinkConfigSnapshot, 1);
    snap->ifindex = ifindex;
    snap->dns = dns;
    snap->domains = domains;
    snap->default_route = default_route;
    snap->first_seen_mono = g_get_monotonic_time();
    snap->settled = !dns_is_empty(dns);
    return snap;
}

void resolved_ctl_snapshot_free(LinkConfigSnapshot *snap)
{
    if (!snap) return;
    if (snap->dns) g_variant_unref(snap->dns);
    if (snap->domains) g_variant_unref(snap->domains);
    g_free(snap);
}

GPtrArray *resolved_ctl_capture_snapshots(void)
{
    GPtrArray *snapshots = g_ptr_array_new_with_free_func((GDestroyNotify)resolved_ctl_snapshot_free);
    GError *error = NULL;
    GDBusConnection *bus = get_system_bus(&error);
    if (!bus) { g_clear_error(&error); return snapshots; }

    GArray *links = get_active_links();
    for (guint i = 0; i < links->len; i++) {
        gint ifindex = g_array_index(links, ActiveLink, i).ifindex;
        GVariant *dns = NULL, *domains = NULL;
        gboolean default_route = FALSE;
        if (!read_link_config(bus, ifindex, &dns, &domains, &default_route)) continue;
/* A link whose live DNS is our redirect is leftover from a previous session — skip it. */
        if (is_proxy_redirect_dns(dns)) {
            g_variant_unref(dns);
            g_variant_unref(domains);
            continue;
        }
        g_ptr_array_add(snapshots, capture_link_snapshot(ifindex, dns, domains, default_route));
    }
    g_array_free(links, TRUE);
    g_object_unref(bus);
    return snapshots;
}

static LinkConfigSnapshot *find_snapshot(GPtrArray *snapshots, gint ifindex)
{
    if (!snapshots) return NULL;
    for (guint i = 0; i < snapshots->len; i++) {
        LinkConfigSnapshot *snap = g_ptr_array_index(snapshots, i);
        if (snap->ifindex == ifindex) return snap;
    }
    return NULL;
}

/* Bring snapshots up to date: new links get captured (unsettled, waiting for owner DNS);
 * links with non-proxy DNS get refreshed (external owner just re-asserted); links on our
 * redirect keep their stored snapshot. */
void resolved_ctl_refresh_snapshots(GPtrArray *snapshots)
{
    if (!snapshots) return;
    GError *error = NULL;
    GDBusConnection *bus = get_system_bus(&error);
    if (!bus) { g_clear_error(&error); return; }

    GArray *links = get_active_links();
    for (guint i = 0; i < links->len; i++) {
        gint ifindex = g_array_index(links, ActiveLink, i).ifindex;

        GVariant *dns = NULL, *domains = NULL;
        gboolean default_route = FALSE;
        if (!read_link_config(bus, ifindex, &dns, &domains, &default_route)) continue;

        LinkConfigSnapshot *snap = find_snapshot(snapshots, ifindex);
        if (!snap) {
            if (is_proxy_redirect_dns(dns)) {
                /* Stale parking from a previous session — skip. */
                g_variant_unref(dns);
                g_variant_unref(domains);
                continue;
            }
            g_ptr_array_add(snapshots, capture_link_snapshot(ifindex, dns, domains, default_route));
        } else if (is_proxy_redirect_dns(dns)) {
            g_variant_unref(dns);
            g_variant_unref(domains);
        } else {
            g_variant_unref(snap->dns);
            g_variant_unref(snap->domains);
            snap->dns = dns;
            snap->domains = domains;
            snap->default_route = default_route;
        }
    }
    g_array_free(links, TRUE);
    g_object_unref(bus);
}

/* Per-tick reassert while protection is live: reconcile snapshots, then re-redirect.
 * New links get a grace window before redirect; settled links with non-proxy DNS
 * get their snapshot refreshed then re-redirected. */
GPtrArray *resolved_ctl_reassert(GPtrArray *snapshots)
{
    GPtrArray *errors = g_ptr_array_new_with_free_func(g_free);
    if (!snapshots) return errors;

    GError *error = NULL;
    GDBusConnection *bus = get_system_bus(&error);
    if (!bus) {
        g_ptr_array_add(errors, g_strdup_printf("Couldn't reach the system D-Bus: %s", error->message));
        g_clear_error(&error);
        return errors;
    }

    GArray *links = get_active_links();
    for (guint i = 0; i < links->len; i++) {
        gint ifindex = g_array_index(links, ActiveLink, i).ifindex;

        GVariant *dns = NULL, *domains = NULL;
        gboolean default_route = FALSE;
        if (!read_link_config(bus, ifindex, &dns, &domains, &default_route)) continue;

        LinkConfigSnapshot *snap = find_snapshot(snapshots, ifindex);
        if (!snap) {
            /* The reconciliation below consumes the live read's references. The
             * snapshot must own separate references, including during the grace
             * window and when discarding a stale redirect. */
            snap = capture_link_snapshot(ifindex, g_variant_ref(dns),
                                         g_variant_ref(domains), default_route);
            /* New link already on our redirect — clear the empty snapshot, let grace handle it. */
            if (is_proxy_redirect_dns(snap->dns)) {
                g_variant_unref(snap->dns);
                g_variant_unref(snap->domains);
                snap->dns = NULL;
                snap->domains = NULL;
            }
            snap->settled = FALSE; /* first sighting: give the owner time to configure DNS */
            g_ptr_array_add(snapshots, snap);
        }

        if (!snap->settled) {
            if (!dns_is_empty(dns) && !is_proxy_redirect_dns(dns)) {
                g_variant_unref(snap->dns);
                g_variant_unref(snap->domains);
                snap->dns = dns;
                snap->domains = domains;
                snap->default_route = default_route;
                snap->settled = TRUE;
            } else {
                g_variant_unref(dns);
                g_variant_unref(domains);
                if (g_get_monotonic_time() - snap->first_seen_mono >= NEW_LINK_GRACE_USEC)
                    snap->settled = TRUE; /* no owner DNS ever came — a genuinely no-DNS link */
                else
                    continue; /* still giving the owner its window; not redirected this tick */
            }
        } else if (is_proxy_redirect_dns(dns)) {
            g_variant_unref(dns);
            g_variant_unref(domains);
        } else {
            g_variant_unref(snap->dns);
            g_variant_unref(snap->domains);
            snap->dns = dns;
            snap->domains = domains;
            snap->default_route = default_route;
        }

        if (!call_set_link_dns(bus, ifindex, &error)) {
            g_ptr_array_add(errors, g_strdup_printf("SetLinkDNS(%d) failed: %s", ifindex, error->message));
            g_clear_error(&error);
            continue;
        }
        if (!call_set_link_domains_route_all(bus, ifindex, &error)) {
            g_ptr_array_add(errors, g_strdup_printf("SetLinkDomains(%d) failed: %s", ifindex, error->message));
            g_clear_error(&error);
        }
    }

    g_array_free(links, TRUE);
    g_object_unref(bus);
    return errors;
}

GPtrArray *resolved_ctl_redirect_to_local_proxy(void)
{
    GPtrArray *errors = g_ptr_array_new_with_free_func(g_free);

    GError *error = NULL;
    GDBusConnection *bus = get_system_bus(&error);
    if (!bus) {
        g_ptr_array_add(errors, g_strdup_printf("Couldn't reach the system D-Bus: %s", error->message));
        g_clear_error(&error);
        return errors;
    }

    GArray *links = get_active_links();
    for (guint i = 0; i < links->len; i++) {
        gint ifindex = g_array_index(links, ActiveLink, i).ifindex;

        if (!call_set_link_dns(bus, ifindex, &error)) {
            g_ptr_array_add(errors, g_strdup_printf("SetLinkDNS(%d) failed: %s", ifindex, error->message));
            g_clear_error(&error);
            continue;
        }
        if (!call_set_link_domains_route_all(bus, ifindex, &error)) {
            g_ptr_array_add(errors, g_strdup_printf("SetLinkDomains(%d) failed: %s", ifindex, error->message));
            g_clear_error(&error);
        }
    }

    g_array_free(links, TRUE);
    g_object_unref(bus);
    return errors;
}

/* Restores every currently-active link's pre-protection config. For links we captured a snapshot
 * of (see capture_link_snapshot) the exact prior DNS servers + routing/search domains +
 * default-route flag are put back via SetLinkDNS/SetLinkDomains/SetLinkDefaultRoute — NOT
 * RevertLink: RevertLink only clears *our* override to "unset" and relies on the link's owner to
 * re-push, which a non-NetworkManager tunnel (e.g. Windscribe/wg-quick) never does, leaving
 * the link with no resolver at all (confirmed the hard way: disabling protection with a VPN up
 * broke DNS until dnsl was re-run or the VPN reconnected). Links we never touched (no snapshot)
 * fall back to RevertLink as before. So do snapshots whose stored DNS is our own redirect — those
 * were never a genuine original (see the capture sites) and writing them back would strand the
 * link on the now-dead local proxy. One NM-wide DNS-rc reload still runs afterward — harmless
 * on NM-managed links (same values; fresh lease if DHCP renewed) and irrelevant on tunnels. Same
 * error-collection contract as the other public functions. */
GPtrArray *resolved_ctl_restore_dhcp(GPtrArray *snapshots)
{
    GPtrArray *errors = g_ptr_array_new_with_free_func(g_free);

    GError *error = NULL;
    GDBusConnection *bus = get_system_bus(&error);
    if (!bus) {
        g_ptr_array_add(errors, g_strdup_printf("Couldn't reach the system D-Bus: %s", error->message));
        g_clear_error(&error);
        return errors;
    }

    GArray *links = get_active_links();
    for (guint i = 0; i < links->len; i++) {
        ActiveLink link = g_array_index(links, ActiveLink, i);
        LinkConfigSnapshot *snap = find_snapshot(snapshots, link.ifindex);
        /* Defense in depth: never write our own redirect back as restored DNS. */
        if (snap && snap->dns && !is_proxy_redirect_dns(snap->dns)) {
            if (!call_set_link_dns_values(bus, link.ifindex, snap->dns, &error)) {
                g_ptr_array_add(errors, g_strdup_printf("restoring SetLinkDNS(%d) failed: %s", link.ifindex, error->message));
                g_clear_error(&error);
            }
            if (!call_set_link_domains_values(bus, link.ifindex, snap->domains, &error)) {
                g_ptr_array_add(errors, g_strdup_printf("restoring SetLinkDomains(%d) failed: %s", link.ifindex, error->message));
                g_clear_error(&error);
            }
            /* Best-effort precision nicety, not core to DNS working again — ignore failures. */
            GError *dr_error = NULL;
            call_set_link_default_route(bus, link.ifindex, snap->default_route, &dr_error);
            g_clear_error(&dr_error);
        } else if (!call_revert_link(bus, link.ifindex, &error)) {
            g_ptr_array_add(errors, g_strdup_printf("RevertLink(%d) failed: %s", link.ifindex, error->message));
            g_clear_error(&error);
        }
    }
    /* One NM-wide reload after all links are restored. */
    nudge_network_manager_dns_reload();

    g_array_free(links, TRUE);
    g_object_unref(bus);
    return errors;
}

#define NM_BUS_NAME "org.freedesktop.NetworkManager"
#define NM_DEVICE_IFACE "org.freedesktop.NetworkManager.Device"
/* NM_DEVICE_STATE_ACTIVATED (100) — from nm-dbus-interface.h, no libnm needed. */
#define NM_DEVICE_STATE_ACTIVATED 100u

#define LOGIN1_BUS_NAME "org.freedesktop.login1"
#define LOGIN1_OBJ_PATH "/org/freedesktop/login1"
#define LOGIN1_MANAGER_IFACE "org.freedesktop.login1.Manager"

struct ResolvedCtlWatch {
    GDBusConnection *bus; /* NULL if the system bus couldn't be reached at start time */
    guint nm_signal_sub_id;
    guint logind_signal_sub_id;
    guint poll_source_id;
    void (*on_reconnect)(gpointer user_data);
    gpointer user_data;
};

static void on_nm_device_state_changed(GDBusConnection *connection, const gchar *sender_name,
                                        const gchar *object_path, const gchar *interface_name,
                                        const gchar *signal_name, GVariant *parameters,
                                        gpointer user_data)
{
    (void)connection; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
    ResolvedCtlWatch *watch = user_data;

    guint32 new_state = 0, old_state = 0, reason = 0;
    g_variant_get(parameters, "(uuu)", &new_state, &old_state, &reason);
    (void)old_state; (void)reason;
    if (new_state != NM_DEVICE_STATE_ACTIVATED) return;

    watch->on_reconnect(watch->user_data);
}

/* logind PrepareForSleep: TRUE before suspend, FALSE after resume. Only resume is interesting —
 * it catches links that never drop IFF_UP across suspend. */
static void on_logind_prepare_for_sleep(GDBusConnection *connection, const gchar *sender_name,
                                         const gchar *object_path, const gchar *interface_name,
                                         const gchar *signal_name, GVariant *parameters,
                                         gpointer user_data)
{
    (void)connection; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
    ResolvedCtlWatch *watch = user_data;

    gboolean about_to_sleep = FALSE;
    g_variant_get(parameters, "(b)", &about_to_sleep);
    if (about_to_sleep) return;

    watch->on_reconnect(watch->user_data);
}

static gboolean on_poll_tick(gpointer user_data)
{
    ResolvedCtlWatch *watch = user_data;
    watch->on_reconnect(watch->user_data);
    return G_SOURCE_CONTINUE;
}

ResolvedCtlWatch *resolved_ctl_watch_start(guint poll_interval_seconds,
                                            void (*on_reconnect)(gpointer user_data),
                                            gpointer user_data)
{
    ResolvedCtlWatch *watch = g_new0(ResolvedCtlWatch, 1);
    watch->on_reconnect = on_reconnect;
    watch->user_data = user_data;

    GError *error = NULL;
    watch->bus = get_system_bus(&error);
    if (!watch->bus) {
        g_warning("resolved_ctl_watch: couldn't reach the system D-Bus, reconnect detection "
                  "disabled (poll-only if enabled): %s", error->message);
        g_clear_error(&error);
    } else {
        /* NULL object_path matches all NM device objects. */
        watch->nm_signal_sub_id = g_dbus_connection_signal_subscribe(
            watch->bus, NM_BUS_NAME, NM_DEVICE_IFACE, "StateChanged", NULL, NULL,
            G_DBUS_SIGNAL_FLAGS_NONE, on_nm_device_state_changed, watch, NULL);
        watch->logind_signal_sub_id = g_dbus_connection_signal_subscribe(
            watch->bus, LOGIN1_BUS_NAME, LOGIN1_MANAGER_IFACE, "PrepareForSleep", LOGIN1_OBJ_PATH,
            NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_logind_prepare_for_sleep, watch, NULL);
    }

    if (poll_interval_seconds > 0) {
        watch->poll_source_id = g_timeout_add_seconds(poll_interval_seconds, on_poll_tick, watch);
    }

    return watch;
}

void resolved_ctl_watch_stop(ResolvedCtlWatch *watch)
{
    if (!watch) return;
    if (watch->poll_source_id) g_source_remove(watch->poll_source_id);
    if (watch->bus) {
        if (watch->nm_signal_sub_id) g_dbus_connection_signal_unsubscribe(watch->bus, watch->nm_signal_sub_id);
        if (watch->logind_signal_sub_id) g_dbus_connection_signal_unsubscribe(watch->bus, watch->logind_signal_sub_id);
        g_object_unref(watch->bus);
    }
    g_free(watch);
}
