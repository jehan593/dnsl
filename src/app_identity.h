/* Shared app identity constants: paths, names, ids. */
#ifndef DNSL_APP_IDENTITY_H
#define DNSL_APP_IDENTITY_H

#define DNSL_SERVICE_NAME       "dnsl"
#define DNSL_SERVICE_UNIT       "dnsl.service"
#define DNSL_SYSTEMD_UNIT_PATH  "/etc/systemd/system/dnsl.service"

#define DNSL_RUNTIME_DIR        "/run/dnsl"
#define DNSL_SOCKET_PATH        "/run/dnsl/control.sock"

#define DNSL_SETTINGS_DIR       "/etc/dnsl"
#define DNSL_SETTINGS_PATH      "/etc/dnsl/settings.json"

#define DNSL_DESKTOP_ID         "dnsl-tray.desktop"

/* GApplication id — also enforces single-instance. */
#define DNSL_APP_ID             "io.github.dnsl.Tray"

/* Passed by XDG autostart to skip opening the providers window. */
#define DNSL_AUTOSTART_ARG      "--autostart"

#endif
