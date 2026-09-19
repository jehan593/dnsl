# Protection lifecycle

Protection overrides ordinary DNS in the host network namespace only while it is
on. The daemon listens on TCP/UDP 127.0.0.1:15353 and [::1]:15353 and installs its
own `inet dnsl_dns` nftables output NAT table. Queries to port 53, including queries
to local DNS stubs and VPN DNS servers, go to the selected DNS-over-TLS provider.
No interface names or VPN brands are special-cased. The upstream connection uses
the machine's normal routing, including its VPN and kill switch.

DNSL does not write `/etc/resolv.conf`, modify NetworkManager profiles, alter
routes, or replace another application's firewall. While protection is on, it
temporarily points systemd-resolved's active links at the local DNSL listener;
those exact link settings are restored before DNSL stops.
While protection is off or the last tray client has exited, there is no DNSL NAT
table or listener. Saved preferences and an idle installed service may remain.
There is no saved network configuration to restore: the VPN/network manager has
continued owning it throughout. Local `/etc/hosts` and application caches retain
their normal semantics.

## Transitions and failures

* Enable binds all supported listeners before atomically installing the IPv4/IPv6
  TCP/UDP rules. Existing local port-53 connection-tracking entries are expired
  so reused sockets do not bypass the newly enabled protection.
* Disable removes the table, removes only DNSL's cached NAT translations, then
  stops listeners and cancels upstream sockets. Removing rules alone would leave
  established UDP connections using the old redirect until their NAT timeout.
* The last IPC client disconnecting uses the same cleanup, preserving the saved
  on/off preference for a subsequent tray launch. IPC workers are drained before
  the controller is freed on daemon exit.
* A failed enable rolls back. If cleanup fails, the listener stays alive and
  cleanup is retried every second. Disable failures are reported instead of
  claiming that DNSL is off while it still owns interception state.
* The generated systemd unit runs `dnsl --cleanup-network` in `ExecStopPost`,
  including after a crash or forced termination. Daemon startup also clears stale
  state before accepting clients. A manually started daemon killed with SIGKILL
  has no systemd cleanup hook: use the cleanup command before restarting it.
* systemd-resolved's cache is flushed at transitions when available. Application-
  owned caches cannot be flushed globally.
* Large UDP answers respect EDNS size limits and otherwise request TCP fallback.
  TCP connections support repeated length-prefixed queries. Pending tasks are
  bounded; old requests cannot send through sockets reused by a new proxy run.

## Compatibility boundaries

This is not a guarantee of compatibility with every VPN or DNS application.
Standard UDP/TCP DNS in the host namespace is covered. Application-owned DoH,
DoT, DNS inside another network namespace, and DNS carried inside an encrypted
application tunnel do not traverse these port-53 rules. Private VPN names are
also sent to the chosen provider while protection is on and may not resolve.

Applications that permanently use SO_BINDTODEVICE for their DNS sockets can
reject replies arriving over loopback. This differs from current systemd-resolved,
which selects the outgoing interface with IP_UNICAST_IF/IPV6_UNICAST_IF and drops
its temporary device binding after connect. Both IPv4 and IPv6 versions of that
resolved pattern are tested. VPN firewall rules may also block loopback or the
chosen DoT server; DNSL does not remove those policies. External firewall software
that flushes DNSL's table removes interception; re-enable protection after that.

The chosen provider must be reachable by IP and use a TLS port other than 53
(normally 853), avoiding recursive interception of DNSL's own upstream connection.
IPv6 listeners are mandatory when IPv6 loopback is available; a port conflict
fails Enable rather than silently intercepting into an absent listener.

## Installation and migration

Runtime requirements are nftables (`nft`), kernel support for inet-family NAT
(Linux 5.2+), and libnetfilter_conntrack/conntrack netlink support. The systemd
service runs as root. Builds additionally need libnetfilter_conntrack headers.
On a root update, install.sh stops an existing daemon before replacing its binary,
updates the systemd unit to include crash cleanup, and restarts it if it was active.
First installs still use the tray's service installation prompt.

Before migrating from the old resolver-rewriting version, disable its protection
and confirm normal DNS works. If that version already crashed and lost its saved
DNS settings, reconnect the affected network/VPN first. The new backend cannot
reconstruct DNS settings lost by the old process. It deliberately does not guess
VPN DNS or reset unrelated interfaces.

## Validation

`make test` runs the previous snapshot ownership regression and local proxy tests
using a fake upstream, plus activation/cleanup failure tests. A separate local TLS
server verifies real certificate checking, DNS framing, and cancellation of a
blocked upstream request. Tests bind only loopback ports.

`make test-netns` creates a new user/network namespace and tests actual nftables
and connection tracking without touching host DNS, routes, firewall or VPN:

* UDP/TCP, IPv4/IPv6, concurrent queries, provider changes, fragmented TCP framing,
  repeated TCP queries, and stop/start with an old query still in flight.
* Interface-selected DNS sockets using systemd-resolved's pattern.
* Repeated on/off transitions with a reused UDP socket returning to the original
  DNS server, and preservation of another owner's firewall table.
* Real controller/IPC lifecycle: last tray disconnect, reopen, and shutdown with
  connected clients, using temporary settings and socket paths.
* Process death followed by the actual `--cleanup-network` command.

These tests use local fake responses, not a commercial VPN service. User/network
namespaces must be enabled, or the isolated test must be run with suitable
privileges. Never run the `--netns` test script directly in the host namespace.

References: [nftables](https://netfilter.org/projects/nftables/manpage.html),
[systemd-resolved socket selection](https://github.com/systemd/systemd/blob/main/src/resolve/resolved-dns-scope.c).
