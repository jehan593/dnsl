#include "resolved_ctl.h"

#include <gio/gio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define RESOLVE1_BUS_NAME  "org.freedesktop.resolve1"
#define RESOLVE1_OBJ_PATH  "/org/freedesktop/resolve1"
#define RESOLVE1_INTERFACE "org.freedesktop.resolve1.Manager"

typedef struct {
    gint ifindex;
    gchar name[IF_NAMESIZE];
} ActiveLink;

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
        if (ifa->ifa_flags & IFF_POINTOPOINT) continue;

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

static gboolean call_set_link_dns(GDBusConnection *bus, gint ifindex, GError **error)
{
    GVariantBuilder addrs;
    g_variant_builder_init(&addrs, G_VARIANT_TYPE("a(iay)"));

    guint8 v4[4] = { 127, 0, 0, 1 };
    GVariantBuilder v4_bytes;
    g_variant_builder_init(&v4_bytes, G_VARIANT_TYPE("ay"));
    for (int i = 0; i < 4; i++) g_variant_builder_add(&v4_bytes, "y", v4[i]);
    g_variant_builder_add(&addrs, "(i@ay)", AF_INET, g_variant_builder_end(&v4_bytes));

    guint8 v6[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
    GVariantBuilder v6_bytes;
    g_variant_builder_init(&v6_bytes, G_VARIANT_TYPE("ay"));
    for (int i = 0; i < 16; i++) g_variant_builder_add(&v6_bytes, "y", v6[i]);
    g_variant_builder_add(&addrs, "(i@ay)", AF_INET6, g_variant_builder_end(&v6_bytes));

    GVariant *result = g_dbus_connection_call_sync(bus, RESOLVE1_BUS_NAME, RESOLVE1_OBJ_PATH, RESOLVE1_INTERFACE,
        "SetLinkDNS", g_variant_new("(i@a(iay))", ifindex, g_variant_builder_end(&addrs)),
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

GPtrArray *resolved_ctl_restore_dhcp(void)
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
        if (!call_revert_link(bus, link.ifindex, &error)) {
            g_ptr_array_add(errors, g_strdup_printf("RevertLink(%d) failed: %s", link.ifindex, error->message));
            g_clear_error(&error);
        }
    }
    /* One NM-wide reload after all links are reverted, not per-link — see
     * nudge_network_manager_dns_reload()'s doc comment for why this step exists at all. */
    nudge_network_manager_dns_reload();

    g_array_free(links, TRUE);
    g_object_unref(bus);
    return errors;
}
