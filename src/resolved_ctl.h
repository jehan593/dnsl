/* Port of dnsw's Net/AdapterDnsManager.cs: points every active network link's DNS at dnsl's own
 * local proxy (127.0.0.1 / ::1) via systemd-resolved's D-Bus API, or reverts them to automatic —
 * this is the "instant revert to default" behavior the whole app exists for. Scoped to
 * systemd-resolved only (see CLAUDE.md "Why systemd-resolved only") — on a system without it
 * active, every call below simply fails and is reported as a best-effort error per link, the same
 * way dnsw reports a failed netsh call. */
#ifndef DNSL_RESOLVED_CTL_H
#define DNSL_RESOLVED_CTL_H

#include <glib.h>

/* A link's exact pre-redirect systemd-resolved config, captured so restore can put it back
 * verbatim instead of relying on the link's owner to re-push after a plain RevertLink() — which
 * a non-NetworkManager VPN client (Windscribe, wg-quick) never does. Opaque; owned by the caller
 * via g_ptr_array style arrays holding it and resolved_ctl_snapshot_free(). */
typedef struct LinkConfigSnapshot LinkConfigSnapshot;

/* Redirects every currently-active (up, non-loopback) link — point-to-point/tunnel (VPN)
 * interfaces included — to the local proxy: SetLinkDNS(127.0.0.1, ::1) + SetLinkDomains(["~."])
 * so systemd-resolved routes *every* query through it rather than merely offering it as one of
 * several resolvers. Tunnel links are redirected deliberately: NetworkManager ranks an active
 * VPN's link as the highest-precedence DNS route in resolved (dns-priority 50 vs 100, VPNs win
 * ties, and a non-~. link still captures default-route traffic), so a tunnel left with its own
 * real DNS server wins over the proxy on the physical link — redirecting every link leaves no
 * competing resolver at all. Only the link's DNS config is touched, never its routes, so a VPN's
 * tunnel keeps working while protection is on. Best-effort across links — returns a newly
 * allocated array of newly allocated (gchar*) error messages (free with g_ptr_array_free(errors,
 * TRUE) — element free func is g_free), empty (not NULL) on full success. */
GPtrArray *resolved_ctl_redirect_to_local_proxy(void);

/* Captures the exact current resolved config (DNS servers, routing/search domains, default-route
 * flag) of every active link, so restore_dhcp() can hand each link back to its owner unchanged.
 * Must be called BEFORE redirect_to_local_proxy() clobbers the config. Best-effort: a GVariant
 * inside a link whose GetLink/properties can't be read is simply skipped (restore then falls back
 * to RevertLink for it). Returns a newly allocated GPtrArray of LinkConfigSnapshot* (free with
 * g_ptr_array_free(arr, TRUE); element free func is resolved_ctl_snapshot_free). */
GPtrArray *resolved_ctl_capture_snapshots(void);

/* Brings an existing snapshot array (from capture_snapshots()) in line with each active link's
 * live resolved state, for links that changed after the initial capture — e.g. a VPN tunnel that
 * connected, and configured its DNS, while protection was already live. Also *refreshes* a link's
 * stored snapshot whenever its current DNS is not dnsl's own redirect (someone external — a VPN
 * client like Windscribe pushing its resolver, NetworkManager re-pushing after DHCP — just set
 * this link), so restore hands back the freshest external config rather than a stale or empty one
 * captured in the race window before the owner finished configuring DNS. Links parked on our
 * redirect keep their existing snapshot untouched. Must be called before the caller's redirect
 * re-asserts over a just-seen external config. */
void resolved_ctl_refresh_snapshots(GPtrArray *snapshots);

/* Per-tick update for the reconnect watch while protection is live — what the caller runs instead
 * of refresh_snapshots()+redirect_to_local_proxy() so a brand-new link gets its owner time to
 * configure DNS BEFORE it's parked on the proxy. New link: capture and wait out the grace window;
 * as soon as a non-empty owner DNS is seen (or the window expires on a genuinely no-DNS link) the
 * snapshot is marked settled and the link is redirected within the same tick. Settled links whose
 * live DNS isn't our redirect get their snapshot refreshed from that live config. Deferred (still
 * in grace) links are left unredirected. Returns the redirect error-collection contract. */
GPtrArray *resolved_ctl_reassert(GPtrArray *snapshots);

void resolved_ctl_snapshot_free(LinkConfigSnapshot *snap);

/* Restores every currently-active link to its pre-protection state: from the captured snapshot
 * (exact prior DNS/domains/default-route back on the link, correct even for links whose owner
 * won't re-push after a bare RevertLink — the whole point of the snapshot approach) when one
 * exists, else RevertLink, then one NM-wide dns-rc reload. `snapshots` may be NULL. Same
 * error-collection contract as redirect_to_local_proxy(). */
GPtrArray *resolved_ctl_restore_dhcp(GPtrArray *snapshots);

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
