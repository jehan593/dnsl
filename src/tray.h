/* Port of dnsw's Ui/TrayController.cs: builds/rebuilds the tray icon's menu to reflect current
 * state (on/off, selected provider, custom providers, autostart, whether the daemon is even
 * reachable) and swaps the icon glyph between "protected"/"unprotected". Works against a
 * RemoteController talking to the daemon over IPC — this process never touches DNS itself. */
#ifndef DNSL_TRAY_H
#define DNSL_TRAY_H

#include <gtk/gtk.h>
#include "remote_controller.h"

typedef struct TrayController TrayController;

TrayController *tray_controller_new(GtkApplication *app, RemoteController *remote, const gchar *asset_dir);

/* Opens (or presents, if already open) the providers window — called once on a manual launch (not
 * autostart) so the window is visible immediately, and from the "Manage Providers…" menu item. */
void tray_controller_show_providers_window(TrayController *tc);

void tray_controller_free(TrayController *tc);

#endif
