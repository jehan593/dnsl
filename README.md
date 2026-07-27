# dnsl

A native C + GTK3 port of [dnsw](../dnsw) for Linux: system-wide DNS-over-TLS with an instant,
reliable way back to plain automatic DNS. Same Nord color palette and Martian Mono Nerd Font as
[linker-linux](../linker-linux). Pick an encrypted upstream resolver (Cloudflare, Quad9, Mullvad,
a custom DoT server, or NextDNS), flip protection on from the tray, flip it back off — that's the
whole feature set, deliberately. Not a general-purpose DNS manager.

Targets **systemd-resolved** specifically (the default DNS setup on most modern desktop distros —
Ubuntu, Fedora, Arch+NetworkManager). On a system without it active, dnsl declines to enable
protection with a clear error rather than guessing at NetworkManager/resolvconf/raw-`resolv.conf`
semantics. See [CLAUDE.md](CLAUDE.md) for why.

## How it works

A privileged background daemon (`dnsl.service`, a real systemd service) runs a local DNS-over-TLS
stub resolver on `127.0.0.1:53` and points every active network link at it via systemd-resolved's
D-Bus API. An unprivileged tray icon controls it over a local socket — no elevation prompt at
ordinary launch or login, ever. The *only* prompt in dnsl's whole lifecycle is a one-time `pkexec`
the first time it installs/starts the background service on a machine.

## Install / update

```sh
curl -fsSL https://raw.githubusercontent.com/jehan593/dnsl/main/scripts/get.sh | sudo bash
```

Downloads the latest release, installs to `/usr/local` (system-wide), and registers the app +
`.desktop` entry. Re-run the same command any time to update to the latest release.

Then run `dnsl` (or find it in your applications menu). The tray's first launch on a fresh machine
prompts once via `pkexec` to install and start the background service; after that it's silent.

Only prebuilt for `x86_64` right now. For other architectures, or if you'd rather not run a
prebuilt binary, build from source instead (see below).

## Uninstall

```sh
curl -fsSL https://raw.githubusercontent.com/jehan593/dnsl/main/scripts/get.sh | sudo bash -s -- --uninstall
```

Removes the binary, desktop entry, and bundled fonts/icons. Since this runs as root (via `sudo`),
it also stops, disables, and removes `dnsl.service` if it's installed — DNS reverts to normal
immediately as part of uninstalling. `/etc/dnsl/settings.json` is left alone either way, so a later
reinstall picks up your previous provider choice. Running the uninstaller unprivileged instead just
prints the two commands to remove the service yourself.

## Build from source

Requires GTK3, GLib/GIO, OpenSSL, libuuid, fontconfig, and `libayatana-appindicator3` development
headers (e.g. on Arch: `pacman -S gtk3 glib2 openssl util-linux-libs fontconfig
libayatana-appindicator`; on Debian/Ubuntu: `sudo apt install build-essential pkg-config
libgtk-3-dev libglib2.0-dev libssl-dev uuid-dev libfontconfig-dev libayatana-appindicator3-dev`).

```sh
git clone https://github.com/jehan593/dnsl
cd dnsl
make
sudo make install                # installs to /usr/local (system-wide)
sudo make uninstall
```

Then run `dnsl` (or find it in your applications menu). The tray's first launch on a fresh machine
prompts once via `pkexec` to install and start the background service; after that it's silent.

## Development

See [CLAUDE.md](CLAUDE.md) for module layout, architecture, and porting notes (every module names
the corresponding `../dnsw` file it was ported from).

## License

[MIT](LICENSE)
