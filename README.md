# dnsl

A small Linux tray app for system-wide DNS-over-TLS. Pick an encrypted resolver, flip it on, flip it off — that's it.

**Protection on:** intercepts ordinary DNS and sends it to your chosen provider.
**Protection off or tray exited:** removes interception and restores your network and VPN DNS settings.

Requires **nftables**, **libnetfilter_conntrack**, and Linux 5.2+ with conntrack/inet NAT support.
See [networking and VPN compatibility](docs/networking.md) for the supported scope and upgrade notes.

> **FYI:** This project is fully vibe coded

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/jehan593/dnsl/main/scripts/get.sh | sudo bash
```

Re-run the same command to update. Then launch `dnsl` from your app menu or terminal.

## Use

1. If prompted, click **Install service** in the main window and approve the authorization prompt. The button stays disabled while setup finishes and the app connects.
2. Pick a DNS provider, or add a custom provider or NextDNS profile.
3. Click **Enable** to turn on protection. Click **Disable** to return to your usual DNS settings.

Use the tray menu to reopen the providers window. In the add-provider forms, **Tab** moves between fields and **Enter** submits. The main window uses mouse controls.

## Uninstall

```sh
curl -fsSL https://raw.githubusercontent.com/jehan593/dnsl/main/scripts/get.sh | sudo bash -s -- --uninstall
```

## Build from source

Install the existing GTK3/GLib/OpenSSL build dependencies plus the
libnetfilter_conntrack development package and nftables.

```sh
git clone https://github.com/jehan593/dnsl
cd dnsl
make
sudo make install
```

Run `make test` for local tests and `make test-netns` for isolated kernel-networking tests.
See [networking.md](docs/networking.md) for the protection architecture.

## License

[MIT](LICENSE)
