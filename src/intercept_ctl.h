/* Host-namespace DNS interception. Never changes resolver, VPN or route settings. */
#ifndef DNSL_INTERCEPT_CTL_H
#define DNSL_INTERCEPT_CTL_H
#include <glib.h>
#define DNSL_PROXY_PORT 15353
#define DNSL_NFT_TABLE "dnsl_dns"
/* Install/remove only our table. Removal also expires our NAT mappings, so a
 * reused UDP socket cannot keep talking to a stopped proxy after Disable. */
gboolean intercept_ctl_enable(GError **error);
gboolean intercept_ctl_disable(GError **error);
/* Flush systemd-resolved's cache if present; absence is supported. */
void intercept_ctl_flush_cache(void);
int intercept_ctl_cleanup_entry(void);
#endif
