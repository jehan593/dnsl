/* Port of dnsw's Net/StartupRegistration.cs: "Start with $DE" via a standard per-user XDG
 * autostart .desktop entry, pointed at the plain unprivileged tray binary (no args beyond
 * DNSL_AUTOSTART_ARG) — needs no elevation, shows no prompt, ever. The privileged work happens in
 * the separately-installed systemd service instead, which starts automatically at boot as root —
 * see CLAUDE.md "Why a systemd service". */
#ifndef DNSL_AUTOSTART_H
#define DNSL_AUTOSTART_H

#include <glib.h>

gboolean autostart_is_enabled(void);
void autostart_set_enabled(gboolean enabled);

#endif
