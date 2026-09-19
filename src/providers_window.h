/* Main window for DNS protection, service setup, and provider selection. */
#ifndef DNSL_PROVIDERS_WINDOW_H
#define DNSL_PROVIDERS_WINDOW_H

#include <gtk/gtk.h>
#include "remote_controller.h"

typedef void (*ProvidersWindowAutostartChangedFn)(gpointer user_data);
typedef void (*ProvidersWindowInstallFn)(gpointer user_data);
typedef enum {
    PROVIDERS_INSTALL_IDLE,
    PROVIDERS_INSTALL_RUNNING,
    PROVIDERS_INSTALL_CONNECTING,
} ProvidersInstallState;

/* Caller shows the window and refreshes it when the service state changes.
 * The callbacks share user_data: autostart changes sync the tray's local setting,
 * and install requests use the tray's shared setup flow. */
GtkWidget *providers_window_new(GtkWindow *transient_parent, RemoteController *remote,
                                 ProvidersWindowAutostartChangedFn on_autostart_changed,
                                 ProvidersWindowInstallFn on_install, gpointer user_data);
void providers_window_refresh(GtkWidget *window);
void providers_window_set_install_state(GtkWidget *window, ProvidersInstallState state, const gchar *message);

#endif
