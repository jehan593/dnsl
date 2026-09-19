#include "daemon.h"
#include "protection_controller.h"
#include "ipc_server.h"
#include "intercept_ctl.h"

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
    /* Drain IPC clients before the controller is paused/freed below. */
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
}

int daemon_run(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "dnsl --daemon must run as root.\n");
        return 1;
    }

    /* Also protects manual daemon starts and upgrades with stale NAT state. */
    if (intercept_ctl_cleanup_entry() != 0) return 1;

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

    /* No resume-on-startup — protection tracks the tray connecting, not the daemon. */

    g_unix_signal_add(SIGTERM, on_terminate_signal, &state);
    g_unix_signal_add(SIGINT, on_terminate_signal, &state);

    g_main_loop_run(state.loop);

    ipc_server_free(state.server);
    protection_controller_free(state.controller);
    g_main_loop_unref(state.loop);
    return 0;
}
