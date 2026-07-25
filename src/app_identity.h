/* Shared constants naming this app's on-disk/on-wire identity: the systemd unit, the settings
 * file, the control socket, the desktop/autostart entry. Centralized here (mirroring dnsw's
 * IpcContract.PipeName / SettingsStore paths / ServiceInstaller.ServiceName) so every module
 * agrees without redefining strings. */
#ifndef DNSL_APP_IDENTITY_H
#define DNSL_APP_IDENTITY_H

#define DNSL_SERVICE_NAME       "dnsl"
#define DNSL_SERVICE_UNIT       "dnsl.service"
#define DNSL_SYSTEMD_UNIT_PATH  "/etc/systemd/system/dnsl.service"

/* Root-owned directory (created by the daemon, mode 0755) holding the control socket — mirrors
 * /run/<daemon>/ convention used by e.g. NetworkManager, systemd-resolved itself. */
#define DNSL_RUNTIME_DIR        "/run/dnsl"
#define DNSL_SOCKET_PATH        "/run/dnsl/control.sock"

/* Machine-wide settings, owned exclusively by the daemon (root) — the tray never touches this
 * file directly, same reasoning as dnsw's SettingsStore being service-owned. */
#define DNSL_SETTINGS_DIR       "/etc/dnsl"
#define DNSL_SETTINGS_PATH      "/etc/dnsl/settings.json"

#define DNSL_DESKTOP_ID         "dnsl-tray.desktop"

/* GApplication id — also doubles as the app's single-instance-enforcement key (GApplication
 * registers this on the session D-Bus and a second launch just activates the first instance
 * instead of opening a second tray icon), the equivalent of dnsw's named Mutex. */
#define DNSL_APP_ID             "io.github.dnsl.Tray"

/* Passed only on the autostart-launched command line (see autostart.c) — analogous to dnsw's
 * StartupRegistration.AutoStartArg: suppresses opening the providers window on launch so a
 * login-triggered start stays silent in the tray. */
#define DNSL_AUTOSTART_ARG      "--autostart"

#endif
