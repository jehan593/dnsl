#include "daemon.h"
#include "protection_controller.h"
#include "ipc_server.h"

#include <glib-unix.h>
#include <unistd.h>
#include <stdio.h>

typedef struct {
    GMainLoop *loop;
    ProtectionController *controller;
    IpcServer *server;
} DaemonState;

static gboolean on_terminate_signal(gpointer data)
{
    DaemonState *state = data;
    /* Pause (not Disable) — restores every link to automatic DNS before the daemon actually goes
     * away, same as Disable, but deliberately preserves the user's on/off preference. A daemon
     * stop/restart isn't the user explicitly turning protection off, so it shouldn't be
     * remembered as one — the next time a tray connects (including right after this same daemon
     * restarts), protection resumes automatically if it was on before. */
    protection_controller_pause(state->controller);
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
}

int daemon_run(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "dnsl --daemon must run as root (binding UDP 53 and controlling "
                         "systemd-resolved both require it).\n");
        return 1;
    }

    DaemonState state = { 0 };
    state.loop = g_main_loop_new(NULL, FALSE);
    state.controller = protection_controller_new();
    state.server = ipc_server_new(state.controller);

    GError *error = NULL;
    if (!ipc_server_start(state.server, &error)) {
        fprintf(stderr, "Couldn't start the IPC server: %s\n", error->message);
        g_clear_error(&error);
        ipc_server_free(state.server);
        protection_controller_free(state.controller);
        g_main_loop_unref(state.loop);
        return 1;
    }

    /* No resume-on-startup here — protection is tied to the *tray* connecting, not to the
     * daemon's own lifetime (see ipc_server.c's first-client-connect hook and CLAUDE.md
     * "Protection tracks the app, not the daemon"). */

    g_unix_signal_add(SIGTERM, on_terminate_signal, &state);
    g_unix_signal_add(SIGINT, on_terminate_signal, &state);

    g_main_loop_run(state.loop);

    ipc_server_free(state.server);
    protection_controller_free(state.controller);
    g_main_loop_unref(state.loop);
    return 0;
}
