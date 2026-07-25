/* Port of dnsw's Net/AdapterDnsManager.cs: points every active network link's DNS at dnsl's own
 * local proxy (127.0.0.1 / ::1) via systemd-resolved's D-Bus API, or reverts them to automatic —
 * this is the "instant revert to default" behavior the whole app exists for. Scoped to
 * systemd-resolved only (see CLAUDE.md "Why systemd-resolved only") — on a system without it
 * active, every call below simply fails and is reported as a best-effort error per link, the same
 * way dnsw reports a failed netsh call. */
#ifndef DNSL_RESOLVED_CTL_H
#define DNSL_RESOLVED_CTL_H

#include <glib.h>

/* Redirects every currently-active (up, non-loopback, non-point-to-point) link to the local proxy:
 * SetLinkDNS(127.0.0.1, ::1) + SetLinkDomains(["~."]) so systemd-resolved routes *every* query
 * through it rather than merely offering it as one of several resolvers. Best-effort across links
 * — returns a newly allocated array of newly allocated (gchar*) error messages (free with
 * g_ptr_array_free(errors, TRUE) — element free func is g_free), empty (not NULL) on full success. */
GPtrArray *resolved_ctl_redirect_to_local_proxy(void);

/* Reverts every currently-active link back to systemd-resolved's automatic DNS (RevertLink) —
 * same error-collection contract as above. */
GPtrArray *resolved_ctl_restore_dhcp(void);

#endif
