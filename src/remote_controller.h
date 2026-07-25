/* Port of dnsw's Ui/RemoteProtectionController.cs + Ipc/ServiceClient.cs combined: runs inside the
 * unprivileged tray process. Owns a background reconnect-and-read thread against DNSL_SOCKET_PATH;
 * holds no DNS-related state of its own beyond the last IpcStatus it received, so the tray is
 * always showing exactly what the daemon last reported. Callbacks fire on the GLib main-loop
 * thread (via g_idle_add) so tray.c/providers_window.c can touch GTK widgets directly from them. */
#ifndef DNSL_REMOTE_CONTROLLER_H
#define DNSL_REMOTE_CONTROLLER_H

#include "ipc_protocol.h"

typedef struct RemoteController RemoteController;

typedef void (*RemoteStateChangedFn)(gpointer user_data);
typedef void (*RemoteErrorFn)(const gchar *message, gpointer user_data);

RemoteController *remote_controller_new(void);
void remote_controller_free(RemoteController *rc); /* stops the background thread first */

void remote_controller_set_callbacks(RemoteController *rc,
                                      RemoteStateChangedFn on_state_changed,
                                      RemoteErrorFn on_error,
                                      gpointer user_data);

/* Starts the background connect-and-keep-connected thread. Never fails outright — a daemon that
 * isn't running yet (e.g. right after first install) is a normal, expected state. */
void remote_controller_start(RemoteController *rc);

gboolean remote_controller_is_connected(RemoteController *rc);

/* Owned deep-copy snapshot of the last status received, or NULL if never connected — free with
 * ipc_status_free. Safe to call from the main thread at any time (reads under an internal lock). */
IpcStatus *remote_controller_snapshot(RemoteController *rc);

void remote_controller_enable(RemoteController *rc);
void remote_controller_disable(RemoteController *rc);
void remote_controller_select_provider(RemoteController *rc, const gchar *provider_id);
/* Takes ownership of `provider`. */
void remote_controller_add_custom_provider(RemoteController *rc, DnsProvider *provider);
void remote_controller_remove_custom_provider(RemoteController *rc, const gchar *provider_id);

#endif
