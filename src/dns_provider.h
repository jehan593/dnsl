/* Port of dnsw's Data/DnsProvider.cs: one upstream DNS-over-TLS resolver — an SNI/certificate
 * hostname plus one or more IPs to dial on Port (853 for every provider offered, RFC 7858's
 * registered port). Built-ins have a fixed id ("cloudflare"/"quad9"/"mullvad"); anything the user
 * adds (including via the NextDNS quick-add template) gets a generated "custom:<uuid>" id and
 * is_custom = TRUE. */
#ifndef DNSL_DNS_PROVIDER_H
#define DNSL_DNS_PROVIDER_H

#include <glib.h>

typedef struct {
    gchar *id;
    gchar *name;
    gchar *tls_host;
    gchar **ips;       /* NULL-terminated */
    guint ip_count;
    int port;          /* always 853 currently */
    gboolean is_custom;
} DnsProvider;

DnsProvider *dns_provider_new(const gchar *id, const gchar *name, const gchar *tls_host,
                               const gchar *const *ips, guint ip_count, int port, gboolean is_custom);
DnsProvider *dns_provider_copy(const DnsProvider *src);
void dns_provider_free(DnsProvider *provider);

/* Generates a fresh "custom:<uuid>" id. */
DnsProvider *dns_provider_new_custom(const gchar *name, const gchar *tls_host,
                                      const gchar *const *ips, guint ip_count, int port);

/* NextDNS's DoT endpoint is "<config_id>.dns.nextdns.io" reached via its two anycast IPs — the
 * config id travels in the TLS SNI. If NextDNS ever changes these, update here (see dnsw's own
 * CLAUDE.md note "A note on NextDNS's anycast IPs"). */
DnsProvider *dns_provider_new_nextdns(const gchar *config_id);

/* Built-ins, in display order. Do not free the returned providers — owned statically. */
const DnsProvider *dns_provider_builtin_cloudflare(void);
const DnsProvider *dns_provider_builtin_quad9(void);
const DnsProvider *dns_provider_builtin_mullvad(void);
/* NULL-terminated static array of the three built-ins above. */
const DnsProvider *const *dns_provider_builtins(void);

#endif
