/* Exercise real reconciliation/restore without a system bus or network changes. */
#include <gio/gio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <string.h>

static GVariant *live_dns, *live_domains;
static gboolean live_default_route;
static guint writes;

static int fake_getifaddrs(struct ifaddrs **out)
{
    static struct ifaddrs link;
    link.ifa_name = "testvpn";
    link.ifa_flags = IFF_UP;
    *out = &link;
    return 0;
}
static void fake_freeifaddrs(struct ifaddrs *links) { (void)links; }
static unsigned fake_if_nametoindex(const char *name) { (void)name; return 42; }
static gchar *fake_find_program(const gchar *name) { (void)name; return NULL; }
static GDBusConnection *fake_bus(GBusType type, GCancellable *cancel, GError **error)
{
    (void)type; (void)cancel; (void)error;
    return (GDBusConnection *)g_object_new(G_TYPE_OBJECT, NULL);
}
static GVariant *fake_call(GDBusConnection *bus, const gchar *destination,
    const gchar *path, const gchar *interface, const gchar *method,
    GVariant *parameters, const GVariantType *reply_type, GDBusCallFlags flags,
    gint timeout, GCancellable *cancel, GError **error)
{
    (void)bus; (void)destination; (void)path; (void)interface; (void)reply_type;
    (void)flags; (void)timeout; (void)cancel; (void)error;
    g_variant_ref_sink(parameters);
    GVariant *result = NULL;
    if (g_str_equal(method, "GetLink")) {
        result = g_variant_new("(o)", "/test/link");
    } else if (g_str_equal(method, "Get")) {
        const gchar *iface, *property;
        g_variant_get(parameters, "(&s&s)", &iface, &property);
        if (g_str_equal(property, "DefaultRoute")) {
            result = g_variant_new("(v)", g_variant_new_boolean(live_default_route));
        } else {
            GVariant *value = g_str_equal(property, "DNS") ? live_dns : live_domains;
            /* A D-Bus reply owns an independent value. Sharing the fake server's
             * reference would hide a use-after-free in the client. */
            gchar *printed = g_variant_print(value, TRUE);
            GVariant *copy = g_variant_parse(g_variant_get_type(value), printed, NULL, NULL, NULL);
            result = g_variant_new("(v)", copy);
            g_variant_unref(copy);
            g_free(printed);
        }
    } else {
        writes++;
        if (g_str_equal(method, "SetLinkDNS")) {
            g_variant_unref(live_dns);
            live_dns = g_variant_get_child_value(parameters, 1);
        } else if (g_str_equal(method, "SetLinkDomains")) {
            g_variant_unref(live_domains);
            live_domains = g_variant_get_child_value(parameters, 1);
        } else if (g_str_equal(method, "SetLinkDefaultRoute")) {
            gint index;
            g_variant_get(parameters, "(ib)", &index, &live_default_route);
        } else if (g_str_equal(method, "RevertLink")) {
            g_clear_pointer(&live_dns, g_variant_unref);
            g_clear_pointer(&live_domains, g_variant_unref);
            live_dns = g_variant_ref_sink(g_variant_new_array(G_VARIANT_TYPE("(iay)"), NULL, 0));
            live_domains = g_variant_ref_sink(g_variant_new_array(G_VARIANT_TYPE("(sb)"), NULL, 0));
        } else {
            g_error("Unexpected method: %s", method);
        }
        result = g_variant_new("()");
    }
    g_variant_unref(parameters);
    return g_variant_ref_sink(result);
}
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#define if_nametoindex fake_if_nametoindex
#define g_find_program_in_path fake_find_program
#define g_bus_get_sync fake_bus
#define g_dbus_connection_call_sync fake_call
#include "../src/resolved_ctl.c"

static void check_errors(GPtrArray *errors)
{
    g_assert_cmpuint(errors->len, ==, 0);
    g_ptr_array_free(errors, TRUE);
}

static void exercise_new_link(gconstpointer data)
{
    const gchar *kind = data;
    gboolean empty = g_str_equal(kind, "empty");
    gboolean stale = g_str_equal(kind, "stale");
    live_dns = g_variant_ref_sink(stale ? proxy_redirect_dns_variant() :
        g_variant_parse(G_VARIANT_TYPE("a(iay)"), empty ? "[]" :
                        "[(2, [byte 10, 0, 0, 1])]", NULL, NULL, NULL));
    live_domains = g_variant_ref_sink(g_variant_parse(G_VARIANT_TYPE("a(sb)"),
                            "[('corp.example', true)]", NULL, NULL, NULL));
    live_default_route = TRUE;
    writes = 0;
    /* Copies, not extra refs: the test must not keep freed source values alive. */
    gchar *expected_dns = g_variant_print(live_dns, TRUE);
    gchar *expected_domains = g_variant_print(live_domains, TRUE);
    GPtrArray *snapshots = g_ptr_array_new_with_free_func((GDestroyNotify)resolved_ctl_snapshot_free);
    check_errors(resolved_ctl_reassert(snapshots));
    g_assert_cmpuint(snapshots->len, ==, 1);
    LinkConfigSnapshot *snap = g_ptr_array_index(snapshots, 0);
    if (empty || stale) {
        g_assert_cmpuint(writes, ==, 0);
        snap->first_seen_mono -= NEW_LINK_GRACE_USEC;
        check_errors(resolved_ctl_reassert(snapshots));
    }
    g_assert_true(is_proxy_redirect_dns(live_dns));
    check_errors(resolved_ctl_reassert(snapshots));
    resolved_ctl_refresh_snapshots(snapshots);
    check_errors(resolved_ctl_restore_dhcp(snapshots));
    g_assert_false(is_proxy_redirect_dns(live_dns));
    if (!stale) {
        gchar *actual_dns = g_variant_print(live_dns, TRUE);
        gchar *actual_domains = g_variant_print(live_domains, TRUE);
        g_assert_cmpstr(actual_dns, ==, expected_dns);
        g_assert_cmpstr(actual_domains, ==, expected_domains);
        g_assert_true(live_default_route);
        g_free(actual_dns);
        g_free(actual_domains);
    }
    g_ptr_array_free(snapshots, TRUE);
    g_free(expected_dns);
    g_free(expected_domains);
    g_clear_pointer(&live_dns, g_variant_unref);
    g_clear_pointer(&live_domains, g_variant_unref);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_data_func("/resolved/new-vpn-with-dns", "configured", exercise_new_link);
    g_test_add_data_func("/resolved/new-link-awaiting-dns", "empty", exercise_new_link);
    g_test_add_data_func("/resolved/stale-redirect", "stale", exercise_new_link);
    return g_test_run();
}
