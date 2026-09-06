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

/* A link's systemd-resolved config as it existed *before* dnsl redirected it, so restore can put
 * it back exactly instead of relying on the link's owner (NetworkManager, a VPN client, ...) to
 * re-push — which is exactly what a non-NM-managed tunnel (e.g. Windscribe/wg-quick) never does after a
 * plain RevertLink(), leaving the link with no DNS at all and breaking resolution until something
 * (dnsl) re-adds a resolver or the VPN reconnects. */
struct LinkConfigSnapshot {
    gint ifindex;
    GVariant *dns;        /* a(iay): the link's real DNS servers, e.g. Windscribe's tunnel DNS */
    GVariant *domains;    /* a(sb): the link's routing/search domains pre-redirect */
    gboolean default_route;
    gint64 first_seen_mono; /* g_get_monotonic_time() when the link was first captured */
    gboolean settled;      /* a real owner config was captured (non-empty DNS), or grace expired */
};

/* How long a brand-new link is left alone (NOT redirected) so its owner can configure DNS on it
 * first. VPN clients bring the interface up and only push DNS a moment later — Windscribe is
 * reliably a few seconds — and if that window is missed the only chance to learn the owner's true
 * resolver is gone (afterwards the link always shows our redirect, so nothing external to catch is
 * ever visible again). 6s is comfortably past Windscribe's DNS-set delay while keeping the "queries
 * stay on the proxy" guarantee to within a few seconds for the new link (during the grace window it
 * resolves via its own freshly-set DNS, which is the same behavior as before the link existed). */
#define NEW_LINK_GRACE_MS 6000
#define NEW_LINK_GRACE_USEC (NEW_LINK_GRACE_MS * 1000)

static gboolean dns_is_empty(GVariant *dns)
{
    return dns == NULL || g_variant_n_children(dns) == 0;
}

/* Every up, non-loopback link — point-to-point/tunnel (VPN) interfaces included. Tunnel links
 * MUST be redirected too, or the whole effort is wasted while a VPN is up: NetworkManager ranks
 * an active VPN's link as the highest-precedence DNS route in systemd-resolved (dns-priority
 * defaults to 50 for VPN connections vs 100 for others, and privacy-VPN clients like wg-quick add
 * their own "~." default route to the tunnel link), so a tun/wg link left with its own real DNS
 * server either wins the routing tie or — when both links carry "~." — is queried in parallel and
 * answers first, silently capturing queries away from the proxy we put on the physical link.
 * Redirecting *every* link leaves no non-proxy resolver in resolved's whole table, so precedence
 * is moot: whichever link wins (or both are queried in parallel), the actual DNS server is our
 * local proxy. Only the link's DNS config is touched, never its routes, so a VPN's tunnel keeps
 * working unchanged while protection is on. */
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

/* Nudge for NetworkManager-managed links: RevertLink() only clears *our* override in
 * systemd-resolved back to "unset" — it does NOT make NetworkManager re-push the link's real
 * DHCP-learned DNS servers afterward. Confirmed by hand on a live system: without this, a link
 * stayed with no DNS scope at all post-revert (`resolvectl status` showed no "DNS" scope even
 * though `nmcli device show` still correctly knew the router's real DNS the whole time), so
 * ordinary resolution silently fell back to the *global* resolver config instead of actually
 * returning to this link's real DHCP DNS — a real gap in "instant, reliable way back to normal
 * DNS", not just a cosmetic status difference.
 *
 * `nmcli device reapply` was tried first and does NOT fix this (confirmed by hand: exits 0, does
 * nothing to resolved's per-link state) — NM only reapplies IP/DNS config when it thinks the
 * connection's config actually changed, which from its point of view it hasn't. What does work,
 * also confirmed by hand: `nmcli general reload dns-rc` ("Update DNS configuration" — the
 * documented equivalent of sending NetworkManager SIGUSR1), which unconditionally makes NM
 * re-push its DNS state everywhere it manages it, including back into systemd-resolved, with no
 * connection interruption. It's a *general* NM-wide operation, not per-link, so it only needs
 * calling once per restore_dhcp() batch, not once per link. Requires root — normal users get
 * `org.freedesktop.NetworkManager.PermissionDenied` — which is fine since this only ever runs
 * inside the (root) daemon. Best-effort: silently skipped on non-NetworkManager systems. */
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

/* Value-based variants of the above: SetLinkDNS/SetLinkDomains with a caller-supplied config
 * (used to put a captured snapshot back), as opposed to resolved_ctl_redirect_to_local_proxy()'s
 * hard-coded proxy addresses. */
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

/* resolve1's Manager.GetLink(ifindex) returns the link's object path; its per-link properties
 * (DNS, Domains, DefaultRoute on the org.freedesktop.resolve1.Link interface) are read through
 * the generic org.freedesktop.DBus.Properties.Get — there is no Manager-level "read a link's
 * current DNS" shortcut. */
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

/* TRUE when the link's DNS list is exactly our own redirect ({127.0.0.1, ::1}). Anything else —
 * the owner's real config, a VPN client (e.g. Windscribe) pushing its DNS onto its tunnel after we
 * redirected it, DHCP re-assigning — means an external owner currently owns this link, and that
 * config is what the snapshot must reflect so restore puts it back verbatim. The interface-index
 * field of each a(iay) entry is ignored (resolvectl-family callers use 0; exact bytes decide). */
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
        /* A link whose live DNS is already our own redirect is a leftover of a *previous*
         * protection session, not anyone's genuine pre-protection config. Never store it as the
         * link's "original" — restore of such a snapshot would point the link at the (dead) local
         * proxy. Skip the snapshot entirely: restore then falls back to RevertLink + NM nudge. */
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

/* Brings an existing snapshot array up to date with the live resolved state of every active link:
 *   - a link with no snapshot yet (e.g. a VPN tunnel that came up while protection was already
 *     live) is captured from its current config, which may legitimately still be empty at that
 *     instant — Windscribe and friends take a moment to push DNS after bringing the interface up;
 *   - a link whose current DNS is NOT our own redirect is being (or was just) re-owned by some
 *     external owner — NetworkManager re-pushing after DHCP, a VPN client (Windscribe) writing its
 *     resolver onto its tunnel after our redirect, etc. — and the snapshot is refreshed to that
 *     live config so restore puts the freshest external value back, never a stale or empty one.
 * Links currently parked on our redirect keep their stored snapshot untouched.
 *
 * This is what makes "disable protection mid-VPN" reliable for clients that didn't exist (or
 * hadn't configured their DNS yet) when protection was enabled: whatever they were last seen with
 * is exactly what restore hands back. Must be called before the caller's redirect re-asserts over
 * a newly-seen external config. No-op on links whose config can't be read (transient D-Bus). */
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
                /* Stale parking from a dead previous session, not a real config — see
                 * resolved_ctl_capture_snapshots(). Don't record it; reassert handles the rest. */
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

/* Per-tick update the reconnect watch calls while protection is live: reconciles the snapshot
 * array with reality, then re-asserts our redirect — one pass, in this exact order:
 *
 *   1. New link (no snapshot yet): capture it WITHOUT redirecting, and wait out
 *      NEW_LINK_GRACE_MS for its owner to configure DNS on it. VPN clients bring the interface up
 *      and set DNS a moment later (Windscribe reliably does); if we redirected inside that window
 *      the link would sit on our 127.0.0.1 forever, the owner's true resolver would never be
 *      visible again, and disable later would hand the link back an empty snapshot — the dead-DNS
 *      mid-VPN bug. Once a non-empty owner DNS appears (or the grace expires on a genuinely no-DNS
 *      link) the snapshot is settled and the link is redirected.
 *   2. Settled link whose live DNS is not our redirect: an external owner (Windscribe re-pushing,
 *      NM re-applying after DHCP) just overwrote us — absorb that config back into the snapshot so
 *      disable restores the freshest value, then redirect again within this same tick.
 *   3. Settled link on our redirect: keep its snapshot untouched, re-assert the redirect.
 *
 * Every active link ends the tick either parked on the proxy or still inside its grace window.
 * Returns the same error-collection contract as redirect_to_local_proxy() (fresh gchar* array,
 * empty on full success). */
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
            snap = capture_link_snapshot(ifindex, dns, domains, default_route);
            /* A brand-new link that is already parked on our own redirect carries no genuine
             * "original" — it's a leftover from a previous protection session (a dead daemon's
             * redirect the link owner never cleared) or is coming up over an old one. An empty
             * snapshot then means restore falls back to RevertLink instead of handing the dead
             * local proxy back as the link's DNS. */
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
        /* Defense in depth: never write our own redirect back as a link's "restored" DNS, whatever
         * its origin. A snapshot whose stored DNS is exactly the proxy redirect can only mean the
         * link's true original was never learned (stale parking captured as "original"), and
         * restoring it would point the link at the now-dead local proxy — a DNS outage, not a
         * restore. Fall back to RevertLink + NM nudge for those, same as never-snapshotted links. */
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
    /* One NM-wide reload after all links are restored, not per-link — see
     * nudge_network_manager_dns_reload()'s doc comment for why this step exists at all. */
    nudge_network_manager_dns_reload();

    g_array_free(links, TRUE);
    g_object_unref(bus);
    return errors;
}

#define NM_BUS_NAME "org.freedesktop.NetworkManager"
#define NM_DEVICE_IFACE "org.freedesktop.NetworkManager.Device"
/* NM_DEVICE_STATE_ACTIVATED from NetworkManager's public D-Bus API (nm-dbus-interface.h) — stable
 * ABI value, not worth pulling in libnm just for this one constant. */
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

/* logind's PrepareForSleep(b) fires twice per sleep cycle: TRUE right before the system suspends,
 * FALSE right after it resumes. Only the resume edge is interesting here — it lands the instant
 * the kernel is back, ahead of NM having necessarily finished reassociating/renewing a lease, which
 * is what makes it useful for links (e.g. wired ethernet) that never drop IFF_UP across suspend and
 * so never generate an NM StateChanged transition at all despite resolved's link config having been
 * silently reverted underneath us. */
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
        /* NULL object_path matches the signal from every device object NM exposes, not just one. */
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
