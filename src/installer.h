/* Port of dnsw's Util/ServiceInstaller.cs: the one place elevated rights ever enter this app from
 * the tray side. Checks whether dnsl.service exists and is active and, if not, relaunches this
 * same exe via `pkexec` (one polkit auth prompt — the Linux analogue of Verb=runas) to install/
 * start it. Called once at tray startup; after the unit is installed and enabled, this never needs
 * to run again on this machine — see CLAUDE.md "Why a systemd service". */
#ifndef DNSL_INSTALLER_H
#define DNSL_INSTALLER_H

typedef enum {
    INSTALLER_ALREADY_RUNNING,
    INSTALLER_STARTED,
    /* The user dismissed/denied the polkit auth prompt — not an error, just a choice to make
     * again next launch (the tray still runs, just can't reach a daemon yet). */
    INSTALLER_CANCELLED,
    INSTALLER_FAILED,
} InstallerResult;

/* Blocks (runs `pkexec` synchronously) — call from a background thread, never the GTK main
 * thread. */
InstallerResult installer_ensure_installed_and_running(void);

/* Entry point for the elevated helper invocation itself — main.c dispatches here when launched
 * with --install-service/--start-service, which only ever happens via the pkexec relaunch above,
 * so this process is guaranteed to be running as root by the time this runs. Returns a process
 * exit code. */
int installer_run_elevated_helper_entry_point(int argc, char **argv);

#endif
