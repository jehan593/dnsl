#include "protection_controller.h"
#include "settings_store.h"
#include "dns_proxy.h"
#include "intercept_ctl.h"
#include "resolved_ctl.h"

struct ProtectionController {
    GMutex mutex;
    AppSettings *settings;
    DnsProxy *proxy;
    ResolvedCtlWatch *watch;
    GPtrArray *link_snapshots;
    guint cleanup_retry;

    ProtectionStateChangedFn on_state_changed;
    ProtectionErrorFn on_error;
    gpointer user_data;
};

static gboolean stop_live(ProtectionController *pc);
static void on_possible_link_drift(gpointer user_data);
static gboolean retry_cleanup(gpointer data)
{
    ProtectionController *pc = data;
    g_mutex_lock(&pc->mutex);
    gboolean done = stop_live(pc);
    if (done) pc->cleanup_retry = 0;
    g_mutex_unlock(&pc->mutex);
    return done ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static void schedule_cleanup_retry(ProtectionController *pc)
{
    if (!pc->cleanup_retry) pc->cleanup_retry = g_timeout_add_seconds(1, retry_cleanup, pc);
}

ProtectionController *protection_controller_new(void)
{
    ProtectionController *pc = g_new0(ProtectionController, 1);
    g_mutex_init(&pc->mutex);
    pc->settings = settings_store_load();
    pc->proxy = dns_proxy_new();
    pc->watch = resolved_ctl_watch_start(2, on_possible_link_drift, pc);
    return pc;
}

void protection_controller_free(ProtectionController *pc)
{
    if (!pc) return;
    protection_controller_pause(pc);
    if (pc->cleanup_retry) g_source_remove(pc->cleanup_retry);
    resolved_ctl_watch_stop(pc->watch);
    if (pc->link_snapshots) g_ptr_array_free(pc->link_snapshots, TRUE);
    dns_proxy_free(pc->proxy);
    app_settings_free(pc->settings);
    g_mutex_clear(&pc->mutex);
    g_free(pc);
}

static void on_possible_link_drift(gpointer user_data)
{
    ProtectionController *pc = user_data;
    g_mutex_lock(&pc->mutex);
    if (dns_proxy_is_running(pc->proxy) && pc->link_snapshots) {
        GPtrArray *errors = resolved_ctl_reassert(pc->link_snapshots);
        for (guint i = 0; i < errors->len; i++)
            g_warning("DNSL resolved reassert: %s", (gchar *)g_ptr_array_index(errors, i));
        g_ptr_array_free(errors, TRUE);
    }
    g_mutex_unlock(&pc->mutex);
}

void protection_controller_set_callbacks(ProtectionController *pc,
                                          ProtectionStateChangedFn on_state_changed,
                                          ProtectionErrorFn on_error,
                                          gpointer user_data)
{
    pc->on_state_changed = on_state_changed;
    pc->on_error = on_error;
    pc->user_data = user_data;
}

static void fire_state_changed(ProtectionController *pc)
{
    if (pc->on_state_changed) pc->on_state_changed(pc->user_data);
}

static void fire_error(ProtectionController *pc, const gchar *message)
{
    g_message("%s", message);
    if (pc->on_error) pc->on_error(message, pc->user_data);
}

gboolean protection_controller_is_enabled(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    gboolean running = dns_proxy_is_running(pc->proxy);
    g_mutex_unlock(&pc->mutex);
    return running;
}

const gchar *protection_controller_selected_provider_id(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    const gchar *id = pc->settings->selected_provider_id;
    g_mutex_unlock(&pc->mutex);
    return id;
}

GPtrArray *protection_controller_all_providers(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    GPtrArray *all = app_settings_all_providers(pc->settings);
    g_mutex_unlock(&pc->mutex);
    return all;
}

void protection_controller_enable(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    if (dns_proxy_is_running(pc->proxy) && !pc->cleanup_retry) { g_mutex_unlock(&pc->mutex); return; }
    if (pc->cleanup_retry) { g_source_remove(pc->cleanup_retry); pc->cleanup_retry = 0; }

    const DnsProvider *provider = app_settings_selected_provider(pc->settings);
    if (!provider) provider = dns_provider_builtin_cloudflare();

    GError *error = NULL;
    if (!dns_proxy_is_running(pc->proxy) &&
        !dns_proxy_start(pc->proxy, provider, DNSL_PROXY_PORT, &error)) {
        gchar *message = g_strdup_printf("Couldn't start the DNS proxy: %s", error->message);
        g_clear_error(&error);
        g_mutex_unlock(&pc->mutex);
        fire_error(pc, message);
        g_free(message);
        return;
    }

    if (!intercept_ctl_enable(&error)) {
        gchar *message = g_strdup_printf("Couldn't enable DNS interception: %s", error->message);
        g_clear_error(&error);
        /* Enable can fail after rule installation (connection tracking). Only
         * stop the listener if rollback fully succeeds. Otherwise keep it alive
         * and let Disable retry cleanup instead of leaving a dead redirect. */
        if (intercept_ctl_disable(&error)) dns_proxy_stop(pc->proxy);
        else {
            gchar *combined = g_strdup_printf("%s\nCleanup also failed: %s. Retry Disable.", message, error->message);
            g_free(message);
            message = combined;
            g_clear_error(&error);
            schedule_cleanup_retry(pc);
        }
        g_mutex_unlock(&pc->mutex);
        fire_state_changed(pc);
        fire_error(pc, message);
        g_free(message);
        return;
    }
    pc->link_snapshots = resolved_ctl_capture_snapshots();
    GPtrArray *resolved_errors = resolved_ctl_reassert(pc->link_snapshots);
    /* A resolver-less system can still use the nftables path for applications
     * that issue ordinary DNS directly. Do not reject Enable solely because
     * systemd-resolved is absent; when links were captured, however, failure
     * to configure them would leave the primary NSS path unprotected. */
    if (resolved_errors->len > 0 && pc->link_snapshots->len > 0) {
        gchar *message = g_strdup_printf("Couldn't configure systemd-resolved: %s",
            (gchar *)g_ptr_array_index(resolved_errors, 0));
        g_ptr_array_free(resolved_errors, TRUE);
        GError *cleanup_error = NULL;
        intercept_ctl_disable(&cleanup_error);
        g_clear_error(&cleanup_error);
        g_ptr_array_free(pc->link_snapshots, TRUE);
        pc->link_snapshots = NULL;
        dns_proxy_stop(pc->proxy);
        g_mutex_unlock(&pc->mutex);
        fire_error(pc, message);
        g_free(message);
        return;
    }
    g_ptr_array_free(resolved_errors, TRUE);
    intercept_ctl_flush_cache();

    pc->settings->enabled = TRUE;
    GError *save_error = NULL;
    if (!settings_store_save(pc->settings, &save_error)) g_clear_error(&save_error);

    g_mutex_unlock(&pc->mutex);
    fire_state_changed(pc);
}

/* The listener remains alive until both rules and cached NAT mappings are gone. */
static gboolean stop_live(ProtectionController *pc)
{
    if (!dns_proxy_is_running(pc->proxy)) return TRUE;
    GError *error = NULL;
    if (!intercept_ctl_disable(&error)) {
        gchar *message = g_strdup_printf("Couldn't fully disable DNS interception: %s. Retry Disable.", error->message);
        g_clear_error(&error);
        g_mutex_unlock(&pc->mutex);
        fire_error(pc, message);
        g_free(message);
        g_mutex_lock(&pc->mutex);
        return FALSE;
    }
    if (pc->link_snapshots && pc->link_snapshots->len > 0) {
        GPtrArray *restore_errors = resolved_ctl_restore_dhcp(pc->link_snapshots);
        if (restore_errors->len > 0) {
            for (guint i = 0; i < restore_errors->len; i++)
                g_warning("DNSL resolved restore: %s", (gchar *)g_ptr_array_index(restore_errors, i));
            g_ptr_array_free(restore_errors, TRUE);
            g_mutex_unlock(&pc->mutex);
            fire_error(pc, "Couldn't restore systemd-resolved yet; DNSL remains available and will retry.");
            g_mutex_lock(&pc->mutex);
            return FALSE;
        }
        g_ptr_array_free(restore_errors, TRUE);
        g_ptr_array_free(pc->link_snapshots, TRUE);
        pc->link_snapshots = NULL;
    }
    if (pc->link_snapshots) {
        g_ptr_array_free(pc->link_snapshots, TRUE);
        pc->link_snapshots = NULL;
    }
    dns_proxy_stop(pc->proxy);
    intercept_ctl_flush_cache();
    g_mutex_unlock(&pc->mutex);
    fire_state_changed(pc);
    g_mutex_lock(&pc->mutex);
    return TRUE;
}

void protection_controller_disable(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    pc->settings->enabled = FALSE;
    if (!stop_live(pc)) schedule_cleanup_retry(pc);
    GError *save_error = NULL;
    if (!settings_store_save(pc->settings, &save_error)) g_clear_error(&save_error);
    g_mutex_unlock(&pc->mutex);
}

void protection_controller_pause(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    if (!stop_live(pc)) schedule_cleanup_retry(pc);
    g_mutex_unlock(&pc->mutex);
}

void protection_controller_resume_if_desired(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    gboolean desired = pc->settings->enabled;
    g_mutex_unlock(&pc->mutex);
    if (desired) protection_controller_enable(pc);
}

void protection_controller_select_provider(ProtectionController *pc, const DnsProvider *provider)
{
    if (!provider->ip_count || provider->port < 1 || provider->port > 65535 || provider->port == 53) {
        fire_error(pc, "Choose a DoT provider with an IP address and a TLS port other than 53.");
        return;
    }
    g_mutex_lock(&pc->mutex);
    g_free(pc->settings->selected_provider_id);
    pc->settings->selected_provider_id = g_strdup(provider->id);
    GError *save_error = NULL;
    if (!settings_store_save(pc->settings, &save_error)) g_clear_error(&save_error);

    if (dns_proxy_is_running(pc->proxy)) dns_proxy_switch_provider(pc->proxy, provider);
    g_mutex_unlock(&pc->mutex);
    fire_state_changed(pc);
}

void protection_controller_add_custom_provider(ProtectionController *pc, DnsProvider *provider)
{
    g_mutex_lock(&pc->mutex);
    g_ptr_array_add(pc->settings->custom_providers, provider);
    GError *save_error = NULL;
    if (!settings_store_save(pc->settings, &save_error)) g_clear_error(&save_error);
    g_mutex_unlock(&pc->mutex);
    fire_state_changed(pc);
}

/* Deleting the selected provider falls back to Cloudflare. */
void protection_controller_remove_custom_provider(ProtectionController *pc, const gchar *provider_id)
{
    g_mutex_lock(&pc->mutex);
    for (guint i = 0; i < pc->settings->custom_providers->len; i++) {
        DnsProvider *p = g_ptr_array_index(pc->settings->custom_providers, i);
        if (g_strcmp0(p->id, provider_id) == 0) { g_ptr_array_remove_index(pc->settings->custom_providers, i); break; }
    }

    gboolean was_selected = g_strcmp0(pc->settings->selected_provider_id, provider_id) == 0;
    g_mutex_unlock(&pc->mutex);

    if (was_selected) {
        protection_controller_select_provider(pc, dns_provider_builtin_cloudflare());
        return;
    }

    g_mutex_lock(&pc->mutex);
    GError *save_error = NULL;
    if (!settings_store_save(pc->settings, &save_error)) g_clear_error(&save_error);
    g_mutex_unlock(&pc->mutex);
    fire_state_changed(pc);
}
