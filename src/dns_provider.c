#include "dns_provider.h"
#include <uuid/uuid.h>
#include <string.h>

DnsProvider *dns_provider_new(const gchar *id, const gchar *name, const gchar *tls_host,
                               const gchar *const *ips, guint ip_count, int port, gboolean is_custom)
{
    DnsProvider *p = g_new0(DnsProvider, 1);
    p->id = g_strdup(id);
    p->name = g_strdup(name);
    p->tls_host = g_strdup(tls_host);
    p->ips = g_new0(gchar *, ip_count + 1);
    for (guint i = 0; i < ip_count; i++) p->ips[i] = g_strdup(ips[i]);
    p->ip_count = ip_count;
    p->port = port;
    p->is_custom = is_custom;
    return p;
}

DnsProvider *dns_provider_copy(const DnsProvider *src)
{
    return dns_provider_new(src->id, src->name, src->tls_host,
                             (const gchar *const *)src->ips, src->ip_count, src->port, src->is_custom);
}

void dns_provider_free(DnsProvider *provider)
{
    if (!provider) return;
    g_free(provider->id);
    g_free(provider->name);
    g_free(provider->tls_host);
    g_strfreev(provider->ips);
    g_free(provider);
}

static gchar *new_uuid_id(void)
{
    uuid_t uuid;
    uuid_generate(uuid);
    char text[37];
    uuid_unparse_lower(uuid, text);
    return g_strdup_printf("custom:%s", text);
}

DnsProvider *dns_provider_new_custom(const gchar *name, const gchar *tls_host,
                                      const gchar *const *ips, guint ip_count, int port)
{
    gchar *id = new_uuid_id();
    DnsProvider *p = dns_provider_new(id, name, tls_host, ips, ip_count, port, TRUE);
    g_free(id);
    return p;
}

DnsProvider *dns_provider_new_nextdns(const gchar *config_id)
{
    gchar *id = new_uuid_id();
    gchar *name = g_strdup_printf("NextDNS (%s)", config_id);
    gchar *tls_host = g_strdup_printf("%s.dns.nextdns.io", config_id);
    const gchar *ips[] = { "45.90.28.0", "45.90.30.0" };
    DnsProvider *p = dns_provider_new(id, name, tls_host, ips, 2, 853, TRUE);
    g_free(id);
    g_free(name);
    g_free(tls_host);
    return p;
}

static DnsProvider *g_cloudflare, *g_quad9, *g_mullvad;
static const DnsProvider *g_builtins[4];
static gsize g_builtins_init = 0;

static void ensure_builtins(void)
{
    if (g_once_init_enter(&g_builtins_init)) {
        const gchar *cf_ips[] = { "1.1.1.1", "1.0.0.1" };
        const gchar *q9_ips[] = { "9.9.9.9", "149.112.112.112" };
        const gchar *mv_ips[] = { "194.242.2.2", "194.242.2.3" };

        g_cloudflare = dns_provider_new("cloudflare", "Cloudflare", "cloudflare-dns.com", cf_ips, 2, 853, FALSE);
        g_quad9 = dns_provider_new("quad9", "Quad9", "dns.quad9.net", q9_ips, 2, 853, FALSE);
        g_mullvad = dns_provider_new("mullvad", "Mullvad", "dns.mullvad.net", mv_ips, 2, 853, FALSE);

        g_builtins[0] = g_cloudflare;
        g_builtins[1] = g_quad9;
        g_builtins[2] = g_mullvad;
        g_builtins[3] = NULL;

        g_once_init_leave(&g_builtins_init, 1);
    }
}

const DnsProvider *dns_provider_builtin_cloudflare(void) { ensure_builtins(); return g_cloudflare; }
const DnsProvider *dns_provider_builtin_quad9(void) { ensure_builtins(); return g_quad9; }
const DnsProvider *dns_provider_builtin_mullvad(void) { ensure_builtins(); return g_mullvad; }
const DnsProvider *const *dns_provider_builtins(void) { ensure_builtins(); return g_builtins; }
