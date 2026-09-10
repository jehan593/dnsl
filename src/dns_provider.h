/* A DNS-over-TLS upstream resolver: hostname + IPs + port. Built-ins have fixed ids
 * ("cloudflare"/"quad9"); user-added ones get "custom:<uuid>" ids. */
#ifndef DNSL_DNS_PROVIDER_H
#define DNSL_DNS_PROVIDER_H

#include <glib.h>

typedef struct {
    gchar *id;
    gchar *name;
    gchar *tls_host;
    gchar **ips;
    guint ip_count;
    int port;
    gboolean is_custom;
} DnsProvider;

DnsProvider *dns_provider_new(const gchar *id, const gchar *name, const gchar *tls_host,
                               const gchar *const *ips, guint ip_count, int port, gboolean is_custom);
DnsProvider *dns_provider_copy(const DnsProvider *src);
void dns_provider_free(DnsProvider *provider);

/* Generates a fresh "custom:<uuid>" id. */
DnsProvider *dns_provider_new_custom(const gchar *name, const gchar *tls_host,
                                      const gchar *const *ips, guint ip_count, int port);

/* NextDNS DoT endpoint: "<config_id>.dns.nextdns.io" via two anycast IPs. */
DnsProvider *dns_provider_new_nextdns(const gchar *config_id);

/* Built-in providers, in display order. Don't free — statically owned. */
const DnsProvider *dns_provider_builtin_cloudflare(void);
const DnsProvider *dns_provider_builtin_quad9(void);
const DnsProvider *const *dns_provider_builtins(void);

#endif
