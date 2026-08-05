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
