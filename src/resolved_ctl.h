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

/* Watches for NetworkManager re-pushing its own DNS onto a link out from under us — e.g. on
 * reconnect after suspend, a WiFi roam/reassociation, a DHCP lease renewal, or a plain
 * `nmcli general reload dns-rc` — any of which silently overwrite our SetLinkDNS override with no
 * signal back to us otherwise (confirmed live: waking from sleep does this every time, and so does
 * a bare `nmcli general reload dns-rc` with no reconnect at all). Three layers, all firing
 * `on_reconnect(user_data)` on the caller's GLib main context — the caller decides whether
 * protection is actually live and worth re-asserting:
 *   1. Event-driven (NM): subscribes to NetworkManager's Device StateChanged signal, fires on any
 *      device reaching ACTIVATED. Near-instant recovery for every trigger above.
 *   2. Event-driven (logind): subscribes to login1's PrepareForSleep(b) signal, fires on the
 *      resume edge (b == FALSE). Lands the instant the kernel resumes, ahead of NM having
 *      necessarily finished reconnecting — covers links (e.g. wired ethernet) that never drop
 *      IFF_UP across suspend and so never produce an NM state transition at all, despite
 *      resolved's link config having reverted underneath us.
 *   3. Poll, as a backstop: fires every poll_interval_seconds regardless, so anything that
 *      changes resolved's link config *without* an NM device state transition or a sleep/resume
 *      cycle (some other tool calling resolved's D-Bus API directly, a future NM behavior change,
 *      etc.) still self-heals within one interval instead of needing a manual toggle forever.
 *      Pass 0 to disable this leg and keep only the event-driven ones.
 * Best-effort: if the system bus can't be reached, both event-driven legs are silently skipped (a
 * g_warning is logged) and only the poll leg (if enabled) runs. A machine without logind (rare
 * outside a systemd-resolved system, which dnsl already requires) simply never sees that signal —
 * no separate opt-out needed. */
typedef struct ResolvedCtlWatch ResolvedCtlWatch;

ResolvedCtlWatch *resolved_ctl_watch_start(guint poll_interval_seconds,
                                            void (*on_reconnect)(gpointer user_data),
                                            gpointer user_data);
void resolved_ctl_watch_stop(ResolvedCtlWatch *watch);

#endif
