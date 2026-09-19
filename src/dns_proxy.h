/* Local TCP/UDP DNS proxy for kernel interception; IPv4 and IPv6 loopback. */
#ifndef DNSL_DNS_PROXY_H
#define DNSL_DNS_PROXY_H

#include "dns_provider.h"

typedef struct DnsProxy DnsProxy;

DnsProxy *dns_proxy_new(void);

/* Binds all supported transports before returning success. App port is 15353.
 * Calls on a DnsProxy are serialized by the protection controller. */
gboolean dns_proxy_start(DnsProxy *proxy, const DnsProvider *provider, int port, GError **error);

/* Points forwarding at a different upstream without rebinding the local socket(s). */
void dns_proxy_switch_provider(DnsProxy *proxy, const DnsProvider *provider);

gboolean dns_proxy_is_running(DnsProxy *proxy);

void dns_proxy_stop(DnsProxy *proxy);

void dns_proxy_free(DnsProxy *proxy);

#endif
