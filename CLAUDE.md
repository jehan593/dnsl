# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

dnsl: a native C + GTK3 port of `../dnsw` (Avalonia/.NET) — itself a from-scratch minimal clone of
[YogaDNS](https://www.yogadns.com/) for one specific use case: system-wide DNS-over-TLS with an
instant, reliable way back to plain DHCP/automatic DNS, and (like dnsw) zero elevation prompts at
ordinary launch or login. Same Nord color palette, Martian Mono Nerd Font, and dependency
philosophy as `../linker-linux` (native C, minimal deps, plain Makefile, single binary). Read
`../dnsw/CLAUDE.md` first for the *why* behind ported logic — every module here corresponds to a
named dnsw file and documents what's Linux-specific.

It is **not** a general DNS tool — same restraint as dnsw: pick an encrypted upstream resolver,
flip protection on, flip it back off, and the machine is back to normal DNS within a second or
two. Resist the urge to grow it into a general-purpose DNS manager.

## Why systemd-resolved only

Linux has no single "set adapter DNS" API the way Windows has `netsh`. It's split across
systemd-resolved, NetworkManager, resolvconf/openresolv, or raw `/etc/resolv.conf` edits, and
fighting the wrong one for a given system breaks DNS outright. dnsl targets **systemd-resolved
specifically** — the default on most modern desktop distros (Ubuntu, Fedora, Arch+NetworkManager,
Debian testing) — via its `org.freedesktop.resolve1` D-Bus API, and simply refuses to enable
protection with a clear error on any system where it isn't active. This was a deliberate v1 scope
decision, not an oversight — see `resolved_ctl.c`. A raw-`/etc/resolv.conf` fallback for
resolved-less systems was considered and explicitly deferred: it's a second, cruder code path that
risks clobbering whatever else manages that file.

## Process model

One binary (`dnsl`), three roles, chosen by `main.c` based on argv — same shape as dnsw's
`Program.cs`:

- **`dnsl` (no args, or `--autostart`)** — the tray client. Unprivileged, talks to the daemon over
  a Unix domain socket (`DNSL_SOCKET_PATH`, `/run/dnsl/control.sock`) via `remote_controller.c`.
  Single-instance enforcement comes from `GtkApplication`'s default unique-per-bus-name mode
  (`DNSL_APP_ID`) — the Linux analogue of dnsw's named Mutex; a second launch just re-activates the
  first instance's tray instead of starting a new one. `--autostart` (passed only by the XDG
  autostart `.desktop` entry — see `autostart.c`) suppresses opening the providers window on
  launch, matching dnsw's `--autostart` arg.
- **`dnsl --daemon`** — the privileged half, run as a real `systemd` service (`dnsl.service`,
  `Type=simple`, started automatically at boot as root — see `daemon.c` and `installer.c`). Owns
  the one `ProtectionController` for the whole machine and exposes it over IPC via `ipc_server.c`.
  Must run as root: binding UDP 53 needs `CAP_NET_BIND_SERVICE`/root on Linux (unlike Windows —
  see dnsw's CLAUDE.md "Elevation"), and the resolve1 D-Bus calls require it too.
- **`dnsl --install-service` / `--start-service`** — a short-lived, one-shot elevated helper
  (`installer.c`) that the tray relaunches itself as via `pkexec` (the Linux analogue of
  `Verb=runas`) the first time it can't find a running daemon: writes `/etc/systemd/system/dnsl.service`
  pointing at `/proc/self/exe`'s resolved path, `systemctl daemon-reload && systemctl enable
  --now dnsl.service`, then exits.

## Core flow

Same shape as dnsw's — see `../dnsw/CLAUDE.md` "Core flow" for the full walkthrough. The only
substitutions: `netsh interface ip set dnsservers` → `resolved_ctl_redirect_to_local_proxy()`
(systemd-resolved `SetLinkDNS` + `SetLinkDomains(["~."])` per link over D-Bus), `sc create`/`sc
start` → `installer.c`'s `pkexec` + `systemctl` flow, and named pipes → a Unix domain socket
(`ipc_server.c`/`remote_controller.c`).

## Commands

```sh
make                                # builds ./dnsl
make icons                          # regenerates data/icons/dnsl-{enabled,disabled}-*.png
./dnsl                              # tray client — no elevation needed
./dnsl --daemon                     # daemon logic as a plain foreground process; needs root to
                                     # actually bind port 53 / call resolve1 successfully — see
                                     # "Testing" below for the unprivileged-port workaround
make install                        # installs to /usr/local (binary, fonts/icons, .desktop entry)
make PREFIX=$HOME/.local install    # per-user install instead
make uninstall                      # removes the above; never touches /etc/dnsl/settings.json
```

No test suite in this repo (matches dnsw's own state) — see "Testing" below for the throwaway
scratch harnesses used during development instead.

## Architecture

Every module below is a direct port of the named dnsw file unless noted:

- **`dot_pool.c`** (`Dns/DotConnectionPool.cs`) — RFC 7858 framing over OpenSSL `SSL`/raw sockets
  (2-byte big-endian length prefix + raw DNS message). A capped pool (`MAX_CONNECTIONS` = 6,
  gated by a `GAsyncQueue` used as a counting semaphore) of connections, each handling one query at
  a time. One retry on a stale/failed connection. IPs within a provider are used round-robin.
  Reference-counted (`dot_pool_ref`/`dot_pool_unref`, not in the C# original — GC does this for
  free there) so `dns_proxy.c` can swap the "current" pool out from under in-flight query-handler
  threads without a use-after-free.
- **`dns_proxy.c`** (`Dns/DnsProxyServer.cs`) — binds UDP `127.0.0.1:53` (+ `[::1]:53` if
  available) and forwards every received datagram through the pool for the currently-selected
  provider, one short-lived `GThread` per query (fire-and-forget; `dot_pool`'s own gate bounds real
  concurrency). Stopping closes the sockets to unblock each listener thread's blocking `recvfrom()`
  (the same "dispose the socket to unblock the read loop" trick `DnsProxyServer.cs` uses on its
  `UdpClient`).
- **`resolved_ctl.c`** (`Net/AdapterDnsManager.cs`) — enumerates active (up, non-loopback,
  non-point-to-point) links via `getifaddrs`, then calls `org.freedesktop.resolve1.Manager`'s
  `SetLinkDNS`/`SetLinkDomains`/`RevertLink` over `GDBusConnection` (system bus). A domain of `"."`
  with `routing_only=TRUE` is resolved's `"~."` marker (same as `resolvectl domain <link> '~.'`) —
  makes the link the resolver of last resort for every name, the direct equivalent of pointing
  every Windows adapter's DNS at the local proxy.
- **`dns_provider.c`** (`Data/DnsProvider.cs`) — built-in Cloudflare/Quad9/Mullvad + custom +
  NextDNS-template providers, `custom:<uuid>` ids via `libuuid`.
- **`settings_store.c`** (`Data/AppSettings.cs` + `Data/SettingsStore.cs`) — flat JSON at
  `/etc/dnsl/settings.json`, owned exclusively by the daemon (root) — the tray never touches it
  directly, only ever reaches it through IPC. Atomic write via temp-file + rename.
- **`ipc_protocol.c`** (`Ipc/IpcContract.cs`) — command/status types + JSON (de)serialization.
  Uses `json_to_string_compact()` (added to `json_min.c`, not present in linker-linux's copy) since
  `json_min`'s normal `json_to_string()` always pretty-prints with embedded newlines, which would
  break line-delimited framing — mirrors dnsw's own compact-IPC-vs-indented-settings split
  (`IpcJsonContext` vs `SettingsJsonContext`).
- **`ipc_server.c`** (`Ipc/IpcServer.cs`) — Unix domain socket at `DNSL_SOCKET_PATH`
  (`/run/dnsl/control.sock`, mode 0666 — see "IPC socket permissions" below), one `GThread` per
  connected client, first-client-connects/last-client-disconnects hooks drive
  `protection_controller_resume_if_desired`/`_pause` exactly like `IpcServer.cs`'s
  `_clients.Count == 1`/`.IsEmpty` checks. No named-pipe ACL dance needed — Unix sockets don't have
  Windows' session-isolation problem that motivated the `Global\` pipe-name prefix in dnsw. Each
  `ClientConn` holds **two** `FILE*` (one `fdopen`'d from the connection's own fd for reading, one
  from a `dup()` of it for writing) — never a single shared `fdopen(fd, "r+")`. See "A note on
  shared stdio FILE* across threads" below for why that one design mistake was a real,
  hand-confirmed deadlock, not a theoretical concern.
- **`remote_controller.c`** (`Ui/RemoteProtectionController.cs` + `Ipc/ServiceClient.cs` combined)
  — tray-side background reconnect-and-read thread; dispatches callbacks onto the GLib main-loop
  thread via `g_idle_add` (the `Dispatcher.UIThread.Post` equivalent) so `tray.c`/
  `providers_window.c` can touch GTK widgets directly from them. Same split-`FILE*`
  (`read_f`/`write_f`) shape as `ipc_server.c`'s `ClientConn`, for the same reason.
- **`protection_controller.c`** (`Service/LocalProtectionController.cs`) — the actual
  enable/disable/pause/resume/select-provider logic, mutex-guarded (single-threaded event-driven
  in the C# original; here multiple IPC client threads can call in concurrently).
- **`daemon.c`** (`Service/DnswBackgroundService.cs`) — `GMainLoop` + `g_unix_signal_add` for
  `SIGTERM`/`SIGINT`, calling `protection_controller_pause` (not disable) on shutdown — same
  "restore DNS but don't forget the preference" reasoning as the Windows Service's `StopAsync`.
- **`installer.c`** (`Util/ServiceInstaller.cs`) — `pkexec <exe> --install-service` in place of
  `Verb=runas` + `sc.exe`; writes the unit file directly (`g_file_set_contents`) instead of calling
  an OS service-manager API, since systemd has no equivalent of `sc create`'s single command.
- **`autostart.c`** (`Net/StartupRegistration.cs`) — XDG autostart `.desktop` entry in
  `~/.config/autostart/` instead of the `Run` registry key.
- **`tray.c`** (`Ui/TrayController.cs`) — GTK3 + `libayatana-appindicator3` (`AppIndicator`)
  instead of Avalonia's `TrayIcon`/`NativeMenu`. **Adaptation, not a bug**: AppIndicator shows the
  same menu on any click (no left-click-vs-right-click split like Windows tray icons), so there's
  no direct equivalent of dnsw's "left-click opens ProvidersWindow" — the menu's own "Manage
  Providers…" item is the only way in. **Requires a StatusNotifierItem-capable panel applet** —
  confirmed by hand that dnsl registers correctly with the session bus's `StatusNotifierWatcher`
  (`dbus-send ... org.kde.StatusNotifierWatcher ... RegisteredStatusNotifierItems` lists
  `.../org/ayatana/NotificationItem/dnsl` right alongside other real running apps) even when the
  icon isn't visually rendered anywhere — on this dev machine's Cinnamon panel specifically, that's
  because the "XApp Status Applet" isn't added to the panel by default, not an app bug. GNOME has
  the same requirement (needs the "AppIndicator and KStatusNotifierItem Support" extension); KDE
  and most other DEs with a systray applet work out of the box.
- **`providers_window.c`** (`Ui/ProvidersWindow.axaml(.cs)`) — a `GtkListBox` in single-selection
  mode instead of a `RadioButton` group + custom `ProviderRowTheme` retemplate; GTK3's own
  `row:selected` styling gives the "card row" look for free without fighting radio-indicator
  theming.
- **`add_provider_dialog.c`** (`Ui/AddCustomProviderDialog.axaml(.cs)` +
  `Ui/AddNextDnsProviderDialog.axaml(.cs)`) — combined into one file; same field validation
  (non-empty name/host, at least one parseable IP via `inet_pton`, port 1–65535). **Never call
  `g_strstrip()` directly on `gtk_entry_get_text()`'s return value** — it's a pointer into the
  `GtkEntry`'s own internal buffer (`const`-qualified for exactly this reason), and `g_strstrip()`
  mutates in place, desyncing GTK's separately-tracked buffer length from the actual bytes.
  Crashed the app on hand-testing, specifically on submitting "Add NextDNS" (five call sites had
  this bug; only got exercised there first). Always `entry_text_stripped()` — `g_strdup()` a copy,
  then `g_strstrip()` that owned copy, never the widget's own storage.
- **`theme.c`/`ui_widgets.c`** — copied verbatim from `../linker-linux` and renamed
  (`linker`→`dnsl`, `Linker`→`Dnsl`). Same Nord palette tables, same GTK3 CSS-generation approach,
  same `xdg-desktop-portal`/GSettings dark-mode detection.

## A note on shared stdio FILE* across threads (the "Enable" hang)

**Never `fdopen()` one shared `FILE*` in `"r+"` mode for a socket that's read on one thread and
written from another.** Both `ipc_server.c` and `remote_controller.c` originally did exactly that
(one `FILE*` per connection, `fdopen(fd, "r+")`), mirroring the shape of dnsw's single
`NamedPipeServerStream` per client — but glibc's stdio serializes *every* call on a given `FILE*`
with one internal per-stream lock, held for the entire duration of a call, including a blocking
one. `remote_controller.c`'s background thread spends nearly all its time inside a blocking
`getline()` on that shared `FILE*`, waiting for the daemon's next status push — which means it
holds the stream lock almost permanently. When the GTK main thread then called `fprintf()` on that
*same* `FILE*` to send a command (e.g. clicking "Enable"), it blocked forever trying to acquire a
lock that only `getline()` releases, and `getline()` only returns once a new line arrives —
which can't happen until the command is actually sent. A textbook circular deadlock.

Confirmed live, twice, by hand: clicking "Enable" in the real providers window wedged the GTK main
loop solid; Cinnamon's window manager eventually put up its own "Dnsl is not responding — Wait /
Force Quit" dialog. Reproduced deliberately with `xdotool` after the first two organic reports,
confirmed fixed the same way, and reverified with a second full click-through (Enable → real DNS
redirect confirmed via `resolvectl` → Disable → real DHCP DNS restored, no hang either direction).
`ipc_server.c`'s `ClientConn` has the same-shaped bug on the daemon side (a broadcast to client A,
running on client B's or the daemon's own thread, could deadlock against client A's own resting
`getline()`) — not yet organically hit, but fixed proactively the same way since the mechanism is
identical.

**The fix**: two independent `FILE*` per connection, not one — `read_f` from the connection's own
fd (`fdopen(fd, "r")`), `write_f` from a `dup()` of it (`fdopen(dup(fd), "w")`). Separate `FILE*`
objects have separate internal locks, so a blocking read on one never blocks a write on the other.
The underlying kernel socket fully supports concurrent independent reads/writes across two fds
referencing it — `dup()` costs nothing meaningful here. `ipc_server.c`'s existing per-client
`write_mutex` still matters on top of this (it serializes multiple *writer* threads targeting the
same client's `write_f` — a different, non-deadlocking data race that split `FILE*`s alone don't
address); `remote_controller.c` needs no equivalent since only the GTK main thread ever writes.

## IPC socket permissions

`DNSL_SOCKET_PATH` is created world-connectable (mode 0666), not locked to a group — deliberately:
redirecting DNS on a machine any local user can already interact with isn't a privilege escalation
(they don't gain access to anything they couldn't already reach), so there's no ACL dance needed
the way dnsw's named-pipe `PipeSecurity` grant was (that one existed to work around Windows'
per-session object-namespace isolation between the LocalSystem service and the interactive user,
which Unix domain sockets simply don't have).

## Elevation

Same reasoning as dnsw's CLAUDE.md "Why a Windows Service", substituting systemd for the SCM:
services started by systemd at boot run outside any interactive polkit/auth flow entirely — the
*only* prompt in dnsl's entire lifecycle is the one-time `pkexec` the first time it ever runs on a
machine (`installer.c`). After that, `dnsl.service` persists across reboots on its own and the tray
never needs to elevate again. Unlike Windows, binding UDP port 53 genuinely does need root on
Linux (`CAP_NET_BIND_SERVICE`) — confirmed by hand, this is a real platform difference from dnsw's
"port 53 needs no elevation" note, not a port oversight.

## Testing

Changing this machine's real systemd-resolved link config or binding the real port 53 is something
only you should trigger (via the tray icon / a real `sudo systemctl start dnsl.service`), not
something to do incidentally while iterating on the code — same caution as dnsw's own "Testing"
section. `./dnsl --daemon` run unprivileged as a plain foreground process will fail cleanly at the
`geteuid() != 0` check in `daemon_run()` before touching anything.

What was actually verified during development, with throwaway scratch harnesses (not checked into
this repo, same spirit as dnsw's `dottest`):

1. `dns_proxy_start()` on an unprivileged port (15353, not 53) against the real Cloudflare
   built-in provider — real TLS handshakes, RFC 7858 framing, and correct A-record responses for
   several real domains, including concurrent queries handled through separate pooled connections.
   Verified with hand-built DNS query packets over a raw UDP socket (no `dig`/`kdig`/`drill`
   available in this environment) rather than a checked-in test.
2. `ipc_protocol.c`'s serialize/parse round-trip for `IpcCommand` (including a full
   `AddCustomProvider` payload with a nested provider object) and `IpcStatus` (including an
   embedded-newline error message, verifying `json_to_string_compact()` correctly escapes it rather
   than breaking line-delimited framing).
3. The real tray binary launched against a live X11/Cinnamon session: theme/CSS applies correctly
   (screenshot-verified — Nord palette, Martian Mono font, card-style layout all render as
   intended), the providers window correctly shows the "Background service not connected" state
   with no daemon running, and `AppIndicator` registration was confirmed at the protocol level via
   `dbus-send ... org.kde.StatusNotifierWatcher ... RegisteredStatusNotifierItems` (see `tray.c`'s
   architecture note above for why the icon itself isn't visible on this particular panel).

4. **The full real privileged path, end to end, with the user's explicit go-ahead**: `pkexec
   dnsl --install-service` really installing/enabling/starting `dnsl.service` as root; the tray's
   real IPC `Enable` command really calling `resolved_ctl_redirect_to_local_proxy()` against this
   machine's actual NetworkManager+systemd-resolved `wlp0s20f3` link (confirmed via `resolvectl
   status` showing the link redirected and a live query for `example.com` actually resolving
   through the local proxy → real DoT → Cloudflare, with resolved itself reporting "acquired via
   ... encrypted transport: yes"); and `Disable` reverting it. This surfaced two real bugs, both
   now fixed and reverified with a second full cycle:
   - **`dns_proxy_stop()` deadlocked the whole daemon.** `close()` on a UDP socket does not
     reliably unblock a *different* thread already parked in a blocking `recvfrom()` on that fd —
     the kernel keeps the socket alive until that syscall returns, which it never does. This wedged
     `ProtectionController`'s mutex forever: the `Disable` call itself hung, every other IPC command
     hung waiting for the same lock, and even `SIGTERM` couldn't recover it (the terminate handler
     blocks on that same mutex) — required a manual `SIGKILL` to recover the live daemon. Fixed by
     calling `shutdown(fd, SHUT_RDWR)` before `close()`, which reliably wakes a blocked `recvfrom()`
     in any thread. See `dns_proxy.c`'s `dns_proxy_stop()` doc comment.
   - **`RevertLink()` alone didn't actually restore DNS.** It clears *our* systemd-resolved
     override back to "unset," but NetworkManager doesn't proactively re-push the link's real
     DHCP-learned DNS servers afterward — confirmed live: `resolvectl status` showed the link with
     no DNS scope at all post-revert (NM still correctly knew the router's DNS the whole time via
     `nmcli device show`, it just never re-told resolved), so ordinary resolution silently fell back
     to the global/fallback resolver instead of the actual DHCP server — a real gap in "instant,
     reliable way back," not cosmetic. `nmcli device reapply <link>` was tried first and does
     *not* fix it (exits 0, changes nothing — NM only re-pushes when it thinks the config itself
     changed). What actually works, confirmed as root (matching the daemon's own privilege level):
     `nmcli general reload dns-rc` — NetworkManager's documented equivalent of `SIGUSR1`, an
     unconditional DNS re-push with no connection interruption. See `resolved_ctl.c`'s
     `nudge_network_manager_dns_reload()`.

Also as part of this session's live testing, `/etc/systemd/resolved.conf` on this dev machine was
reset from a hand-configured global NextDNS-over-TLS setup back to the stock default shipped by the
installed `systemd` package (`/etc/systemd/resolved.conf.pacnew`, at the user's request, to test
against a "freshly set up" resolved rather than one with pre-existing global DNS overrides). The
original file was preserved at `/etc/systemd/resolved.conf.bak-with-nextdns` — restore it and
`systemctl restart systemd-resolved` to bring the NextDNS config back.
