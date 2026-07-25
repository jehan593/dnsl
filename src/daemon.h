/* Port of dnsw's Service/DnswBackgroundService.cs: the privileged half of the app, run under
 * `dnsl --daemon` (see main.c and CLAUDE.md "Process model"). Owns the one ProtectionController
 * for the whole machine and exposes it to however many tray clients connect via IpcServer. */
#ifndef DNSL_DAEMON_H
#define DNSL_DAEMON_H

/* Blocks running the daemon's GMainLoop until SIGTERM/SIGINT. Returns a process exit code.
 * Must be running as root (binds UDP 53 fails otherwise on Linux — see CLAUDE.md "Elevation" —
 * and the resolve1 D-Bus calls require the system bus policy only root satisfies). */
int daemon_run(void);

#endif
