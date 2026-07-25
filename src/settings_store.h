/* Port of dnsw's Data/AppSettings.cs + Data/SettingsStore.cs: flat JSON at DNSL_SETTINGS_PATH,
 * owned exclusively by the daemon process (root) — the tray never touches this file directly,
 * only ever reaches it through the IPC protocol. No cross-process lock needed: the daemon is
 * inherently single-instance (systemd), so there's never a second writer to race. */
#ifndef DNSL_SETTINGS_STORE_H
#define DNSL_SETTINGS_STORE_H

#include "dns_provider.h"

typedef struct {
    /* Whether the proxy + link redirection was on at last shutdown/pause-cause-of-disconnect
     * that was actually an explicit choice. Read on daemon startup so a reboot with a tray
     * autostart resumes protection automatically. */
    gboolean enabled;
    gchar *selected_provider_id;
    GPtrArray *custom_providers; /* of DnsProvider* */
} AppSettings;

AppSettings *app_settings_new_default(void);
void app_settings_free(AppSettings *settings);

/* Every built-in followed by every custom provider, in that order. Returned array's elements
 * are borrowed (not owned) — do not free them individually. Free the array with g_ptr_array_free
 * (element_free_func is NULL). */
GPtrArray *app_settings_all_providers(const AppSettings *settings);

/* Borrowed pointer into settings, or NULL if selected_provider_id doesn't match anything. */
const DnsProvider *app_settings_selected_provider(const AppSettings *settings);

AppSettings *settings_store_load(void);
gboolean settings_store_save(const AppSettings *settings, GError **error);

#endif
