#include "app_identity.h"
#include "dns_proxy.h"
#include "intercept_ctl.h"
#include <gio/gio.h>
static gchar *test_dir, *settings_path;
#undef DNSL_SETTINGS_DIR
#undef DNSL_SETTINGS_PATH
#define DNSL_SETTINGS_DIR test_dir
#define DNSL_SETTINGS_PATH settings_path
#include "../src/settings_store.c"
#include "resolved_ctl.h"
ResolvedCtlWatch *resolved_ctl_watch_start(guint interval, void (*cb)(gpointer), gpointer data) { (void)interval; (void)cb; (void)data; return NULL; }
void resolved_ctl_watch_stop(ResolvedCtlWatch *watch) { (void)watch; }
GPtrArray *resolved_ctl_capture_snapshots(void) { return g_ptr_array_new(); }
GPtrArray *resolved_ctl_reassert(GPtrArray *snapshots) { (void)snapshots; return g_ptr_array_new_with_free_func(g_free); }
GPtrArray *resolved_ctl_restore_dhcp(GPtrArray *snapshots) { (void)snapshots; return g_ptr_array_new_with_free_func(g_free); }
#include "../src/protection_controller.c"

struct DnsProxy { gboolean running; };
static GString *events;
static guint enable_failures, disable_failures;
static gboolean rules;
DnsProxy *dns_proxy_new(void) { return g_new0(DnsProxy, 1); }
void dns_proxy_free(DnsProxy *p) { g_assert_false(p->running); g_free(p); }
gboolean dns_proxy_is_running(DnsProxy *p) { return p->running; }
gboolean dns_proxy_start(DnsProxy *p, const DnsProvider *provider, int port, GError **error)
{
    (void)provider; (void)error; g_assert_cmpint(port, ==, DNSL_PROXY_PORT);
    g_assert_false(p->running); p->running = TRUE; g_string_append_c(events, 'S'); return TRUE;
}
void dns_proxy_stop(DnsProxy *p)
{
    g_assert_false(rules); p->running = FALSE; g_string_append_c(events, 'T');
}
void dns_proxy_switch_provider(DnsProxy *p, const DnsProvider *provider) { (void)p; (void)provider; }
gboolean intercept_ctl_enable(GError **error)
{
    g_string_append_c(events, 'E'); rules = TRUE;
    if (!enable_failures) return TRUE;
    enable_failures--;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected activation failure");
    return FALSE;
}
gboolean intercept_ctl_disable(GError **error)
{
    g_string_append_c(events, 'D');
    if (!disable_failures) { rules = FALSE; return TRUE; }
    disable_failures--;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected cleanup failure");
    return FALSE;
}
void intercept_ctl_flush_cache(void) { g_string_append_c(events, 'C'); }

static void lifecycle(void)
{
    ProtectionController *pc = protection_controller_new();
    protection_controller_enable(pc);
    g_assert_cmpstr(events->str, ==, "SEC");
    g_assert_true(protection_controller_is_enabled(pc));
    protection_controller_pause(pc);
    g_assert_cmpstr(events->str, ==, "SECDTC");
    g_assert_false(protection_controller_is_enabled(pc));
    g_assert_true(pc->settings->enabled);
    protection_controller_resume_if_desired(pc);
    g_assert_true(protection_controller_is_enabled(pc));
    protection_controller_disable(pc);
    g_assert_false(pc->settings->enabled);
    protection_controller_resume_if_desired(pc);
    g_assert_false(protection_controller_is_enabled(pc));
    protection_controller_free(pc);
}
static void failure_cleanup(void)
{
    ProtectionController *pc = protection_controller_new();
    enable_failures = 1;
    g_string_truncate(events, 0);
    protection_controller_enable(pc);
    g_assert_cmpstr(events->str, ==, "SEDT");
    g_assert_false(protection_controller_is_enabled(pc));
    protection_controller_enable(pc);
    disable_failures = 1;
    protection_controller_disable(pc);
    g_assert_true(protection_controller_is_enabled(pc)); /* keep serving until cleanup works */
    g_assert_false(pc->settings->enabled);
    gint64 deadline = g_get_monotonic_time() + 4 * G_TIME_SPAN_SECOND;
    while (pc->cleanup_retry && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE));
        g_usleep(1000);
    }
    g_assert_cmpuint(pc->cleanup_retry, ==, 0);
    g_assert_false(protection_controller_is_enabled(pc));
    g_assert_false(rules);
    protection_controller_free(pc);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    events = g_string_new(NULL);
    test_dir = g_dir_make_tmp("dnsl-controller-XXXXXX", NULL);
    settings_path = g_build_filename(test_dir, "settings.json", NULL);
    g_test_add_func("/controller/lifecycle", lifecycle);
    g_test_add_func("/controller/failure-cleanup", failure_cleanup);
    int result = g_test_run();
    g_unlink(settings_path); g_rmdir(test_dir);
    g_free(settings_path); g_free(test_dir); g_string_free(events, TRUE);
    return result;
}
