/* Port of dnsw's Dns/DnsProxyServer.cs: the local stub resolver dnsl points systemd-resolved at —
 * binds UDP 127.0.0.1:53 (and [::1]:53, when available) and forwards every received query to the
 * currently-selected DnsProvider over DoT via dot_pool, relaying the raw response bytes straight
 * back to whichever process asked. */
#ifndef DNSL_DNS_PROXY_H
#define DNSL_DNS_PROXY_H

#include "dns_provider.h"

typedef struct DnsProxy DnsProxy;

DnsProxy *dns_proxy_new(void);

/* Binds the local listener(s) and starts forwarding to `provider`. Fails if already running or if
 * port 53 is already taken by something else (most commonly systemd-resolved's own stub sitting
 * on 127.0.0.53, so this is a real address:port conflict only if something else also grabbed
 * 127.0.0.1:53). `port` is a seam for tests — app code always passes 53. */
gboolean dns_proxy_start(DnsProxy *proxy, const DnsProvider *provider, int port, GError **error);

/* Points forwarding at a different upstream without rebinding the local socket(s). */
void dns_proxy_switch_provider(DnsProxy *proxy, const DnsProvider *provider);

gboolean dns_proxy_is_running(DnsProxy *proxy);

void dns_proxy_stop(DnsProxy *proxy);

void dns_proxy_free(DnsProxy *proxy);

#endif
