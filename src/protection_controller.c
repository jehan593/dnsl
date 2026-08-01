#include "protection_controller.h"
#include "settings_store.h"
#include "dns_proxy.h"
#include "resolved_ctl.h"

/* Reconnect-watch poll interval: see resolved_ctl.h's resolved_ctl_watch_start doc comment — this
 * is the backstop leg for drift the event-driven leg doesn't catch. */
#define RECONNECT_WATCH_POLL_SECONDS 30

struct ProtectionController {
    GMutex mutex;
    AppSettings *settings;
    DnsProxy *proxy;
    ResolvedCtlWatch *watch;

    ProtectionStateChangedFn on_state_changed;
    ProtectionErrorFn on_error;
    gpointer user_data;
};

static void on_possible_link_drift(gpointer user_data);

ProtectionController *protection_controller_new(void)
{
    ProtectionController *pc = g_new0(ProtectionController, 1);
    g_mutex_init(&pc->mutex);
    pc->settings = settings_store_load();
    pc->proxy = dns_proxy_new();
    pc->watch = resolved_ctl_watch_start(RECONNECT_WATCH_POLL_SECONDS, on_possible_link_drift, pc);
    return pc;
}

void protection_controller_free(ProtectionController *pc)
{
    if (!pc) return;
    resolved_ctl_watch_stop(pc->watch);
    dns_proxy_free(pc->proxy);
    app_settings_free(pc->settings);
    g_mutex_clear(&pc->mutex);
    g_free(pc);
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
    if (pc->on_error) pc->on_error(message, pc->user_data);
}

gboolean protection_controller_is_enabled(ProtectionController *pc)
{
    return dns_proxy_is_running(pc->proxy);
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

static gchar *join_errors(const gchar *prefix, GPtrArray *errors)
{
    GString *s = g_string_new(prefix);
    for (guint i = 0; i < errors->len; i++) {
        g_string_append(s, g_ptr_array_index(errors, i));
        if (i + 1 < errors->len) g_string_append_c(s, '\n');
    }
    return g_string_free(s, FALSE);
}

/* Fires from resolved_ctl's reconnect watch (NM device activation, or the periodic backstop poll —
 * see resolved_ctl.h) on the daemon's main-loop thread. If protection isn't actually live this is
 * a no-op; if it is, re-push the redirect unconditionally rather than trying to first detect
 * whether it actually drifted — SetLinkDNS/SetLinkDomains are idempotent and a few extra D-Bus
 * calls every reconnect/30s is free, so there's no reason to build a "did it actually change"
 * check when "just reapply it" is simpler and can't be wrong. */
static void on_possible_link_drift(gpointer user_data)
{
    ProtectionController *pc = user_data;
    g_mutex_lock(&pc->mutex);
    if (!dns_proxy_is_running(pc->proxy)) { g_mutex_unlock(&pc->mutex); return; }

    GPtrArray *errors = resolved_ctl_redirect_to_local_proxy();
    gchar *link_error = errors->len > 0
        ? join_errors("Protection is on, but re-applying it after a network change failed:\n", errors)
        : NULL;
    g_ptr_array_free(errors, TRUE);
    g_mutex_unlock(&pc->mutex);
    if (link_error) { fire_error(pc, link_error); g_free(link_error); }
}

/* Starts the proxy against the currently-selected provider and redirects every active link to it.
 * Links are only ever redirected once the proxy is confirmed listening, so a failed enable never
 * leaves the system pointed at a dead resolver. */
void protection_controller_enable(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    if (dns_proxy_is_running(pc->proxy)) { g_mutex_unlock(&pc->mutex); return; }

    const DnsProvider *provider = app_settings_selected_provider(pc->settings);
    if (!provider) provider = dns_provider_builtin_cloudflare();

    GError *error = NULL;
    if (!dns_proxy_start(pc->proxy, provider, 53, &error)) {
        gchar *message = g_strdup_printf("Couldn't start the DNS proxy: %s", error->message);
        g_clear_error(&error);
        g_mutex_unlock(&pc->mutex);
        fire_error(pc, message);
        g_free(message);
        return;
    }

    GPtrArray *errors = resolved_ctl_redirect_to_local_proxy();
    gchar *link_error = errors->len > 0
        ? join_errors("Protection is on, but some links couldn't be redirected:\n", errors)
        : NULL;
    g_ptr_array_free(errors, TRUE);

    pc->settings->enabled = TRUE;
    GError *save_error = NULL;
    if (!settings_store_save(pc->settings, &save_error)) g_clear_error(&save_error);

    g_mutex_unlock(&pc->mutex);
    fire_state_changed(pc);
    if (link_error) { fire_error(pc, link_error); g_free(link_error); }
}

/* Restores every active link to systemd-resolved's automatic DNS, then stops the proxy —
 * deliberately in that order, so there's never a window where a link still points at 127.0.0.1
 * after the proxy listening there has already gone away. */
static void stop_live(ProtectionController *pc)
{
    if (!dns_proxy_is_running(pc->proxy)) return;

    GPtrArray *errors = resolved_ctl_restore_dhcp();
    gchar *link_error = errors->len > 0
        ? join_errors("Some links couldn't be restored to automatic DNS:\n", errors)
        : NULL;
    g_ptr_array_free(errors, TRUE);

    dns_proxy_stop(pc->proxy);

    g_mutex_unlock(&pc->mutex);
    fire_state_changed(pc);
    if (link_error) { fire_error(pc, link_error); g_free(link_error); }
    g_mutex_lock(&pc->mutex);
}

void protection_controller_disable(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    stop_live(pc);
    pc->settings->enabled = FALSE;
    GError *save_error = NULL;
    if (!settings_store_save(pc->settings, &save_error)) g_clear_error(&save_error);
    g_mutex_unlock(&pc->mutex);
}

void protection_controller_pause(ProtectionController *pc)
{
    g_mutex_lock(&pc->mutex);
    stop_live(pc);
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

/* Deleting the currently-selected provider falls back to Cloudflare — silently switching upstream
 * mid-session rather than leaving Settings pointed at an id that no longer resolves to anything. */
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
