# dnsl

A small Linux tray app for system-wide DNS-over-TLS. Pick an encrypted resolver, flip it on, flip it off — that's it.

Works with **systemd-resolved** (default on Ubuntu, Fedora, Arch, Debian testing).

> **FYI:** This project is fully vibe coded

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/jehan593/dnsl/main/scripts/get.sh | sudo bash
```

Re-run the same command to update. Then launch `dnsl` from your app menu or terminal.

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
