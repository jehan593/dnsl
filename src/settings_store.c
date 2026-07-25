#include "settings_store.h"
#include "app_identity.h"
#include "json_min.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

AppSettings *app_settings_new_default(void)
{
    AppSettings *s = g_new0(AppSettings, 1);
    s->enabled = FALSE;
    s->selected_provider_id = g_strdup(dns_provider_builtin_cloudflare()->id);
    s->custom_providers = g_ptr_array_new_with_free_func((GDestroyNotify)dns_provider_free);
    return s;
}

void app_settings_free(AppSettings *settings)
{
    if (!settings) return;
    g_free(settings->selected_provider_id);
    if (settings->custom_providers) g_ptr_array_free(settings->custom_providers, TRUE);
    g_free(settings);
}

GPtrArray *app_settings_all_providers(const AppSettings *settings)
{
    GPtrArray *all = g_ptr_array_new();
    const DnsProvider *const *builtins = dns_provider_builtins();
    for (guint i = 0; builtins[i]; i++) g_ptr_array_add(all, (gpointer)builtins[i]);
    for (guint i = 0; i < settings->custom_providers->len; i++)
        g_ptr_array_add(all, g_ptr_array_index(settings->custom_providers, i));
    return all;
}

const DnsProvider *app_settings_selected_provider(const AppSettings *settings)
{
    if (!settings->selected_provider_id) return NULL;
    GPtrArray *all = app_settings_all_providers(settings);
    const DnsProvider *found = NULL;
    for (guint i = 0; i < all->len; i++) {
        const DnsProvider *p = g_ptr_array_index(all, i);
        if (g_strcmp0(p->id, settings->selected_provider_id) == 0) { found = p; break; }
    }
    g_ptr_array_free(all, TRUE);
    return found;
}

static JsonValue *provider_to_json(const DnsProvider *p)
{
    JsonValue *obj = json_new_object();
    json_object_set(obj, "id", json_new_string(p->id));
    json_object_set(obj, "name", json_new_string(p->name));
    json_object_set(obj, "tlsHost", json_new_string(p->tls_host));
    JsonValue *ips = json_new_array();
    for (guint i = 0; i < p->ip_count; i++) json_array_append(ips, json_new_string(p->ips[i]));
    json_object_set(obj, "ips", ips);
    json_object_set(obj, "port", json_new_number(p->port));
    return obj;
}

static DnsProvider *provider_from_json(const JsonValue *obj)
{
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    const gchar *id = json_get_string(obj, "id", NULL);
    const gchar *name = json_get_string(obj, "name", NULL);
    const gchar *tls_host = json_get_string(obj, "tlsHost", NULL);
    if (!id || !name || !tls_host) return NULL;

    JsonValue *ips_val = json_object_get(obj, "ips");
    GPtrArray *ips = g_ptr_array_new();
    if (ips_val && ips_val->type == JSON_ARRAY) {
        for (guint i = 0; i < ips_val->v.arr->len; i++) {
            JsonValue *item = g_ptr_array_index(ips_val->v.arr, i);
            if (item->type == JSON_STRING) g_ptr_array_add(ips, item->v.s);
        }
    }
    gint64 port = json_get_int(obj, "port", 853);
    DnsProvider *p = dns_provider_new(id, name, tls_host,
                                       (const gchar *const *)ips->pdata, ips->len, (int)port, TRUE);
    g_ptr_array_free(ips, TRUE);
    return p;
}

AppSettings *settings_store_load(void)
{
    AppSettings *defaults = app_settings_new_default();

    gchar *contents = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(DNSL_SETTINGS_PATH, &contents, NULL, &error)) {
        g_clear_error(&error);
        return defaults; /* missing file (first run) — fall through to defaults */
    }

    JsonValue *root = json_parse(contents, &error);
    g_free(contents);
    if (!root) {
        g_clear_error(&error);
        return defaults; /* corrupt JSON — don't crash a daemon the user expects to just work */
    }

    AppSettings *s = app_settings_new_default();
    s->enabled = json_get_bool(root, "enabled", FALSE);
    g_free(s->selected_provider_id);
    s->selected_provider_id = g_strdup(json_get_string(root, "selectedProviderId", defaults->selected_provider_id));

    JsonValue *custom = json_object_get(root, "customProviders");
    if (custom && custom->type == JSON_ARRAY) {
        for (guint i = 0; i < custom->v.arr->len; i++) {
            DnsProvider *p = provider_from_json(g_ptr_array_index(custom->v.arr, i));
            if (p) g_ptr_array_add(s->custom_providers, p);
        }
    }

    json_value_free(root);
    app_settings_free(defaults);
    return s;
}

gboolean settings_store_save(const AppSettings *settings, GError **error)
{
    if (g_mkdir_with_parents(DNSL_SETTINGS_DIR, 0755) != 0 && errno != EEXIST) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Couldn't create %s: %s", DNSL_SETTINGS_DIR, g_strerror(errno));
        return FALSE;
    }

    JsonValue *root = json_new_object();
    json_object_set(root, "enabled", json_new_bool(settings->enabled));
    json_object_set(root, "selectedProviderId", json_new_string(settings->selected_provider_id));
    JsonValue *custom = json_new_array();
    for (guint i = 0; i < settings->custom_providers->len; i++)
        json_array_append(custom, provider_to_json(g_ptr_array_index(settings->custom_providers, i)));
    json_object_set(root, "customProviders", custom);

    gchar *text = json_to_string(root);
    json_value_free(root);

    /* Atomic write: same reasoning as linker's flock-guarded writes, minus the flock (no
     * concurrent writers possible — see file doc comment) — write to a temp file then rename,
     * so a crash mid-write never leaves a truncated/corrupt settings.json behind. */
    gchar *tmp_path = g_strdup_printf("%s.tmp", DNSL_SETTINGS_PATH);
    gboolean ok = g_file_set_contents(tmp_path, text, -1, error);
    if (ok) {
        if (g_rename(tmp_path, DNSL_SETTINGS_PATH) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Couldn't rename %s -> %s: %s", tmp_path, DNSL_SETTINGS_PATH, g_strerror(errno));
            ok = FALSE;
        }
    }
    g_chmod(DNSL_SETTINGS_PATH, 0644);

    g_free(tmp_path);
    g_free(text);
    return ok;
}
