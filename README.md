# dnsl

A small Linux tray app for system-wide DNS-over-TLS. Pick an encrypted resolver, flip it on, flip it off — that's it.

Works with **systemd-resolved** (default on Ubuntu, Fedora, Arch, Debian testing).

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

```sh
git clone https://github.com/jehan593/dnsl
cd dnsl
make
sudo make install
```

See [CLAUDE.md](CLAUDE.md) for architecture details.

## License

[MIT](LICENSE)
