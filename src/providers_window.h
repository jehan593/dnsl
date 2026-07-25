/* Port of dnsw's Ui/ProvidersWindow.axaml(.cs): the one real window in the app, opened from the
 * tray's "Manage Providers…" item. Built imperatively (no bindings/DataTemplates) — the provider
 * list is small and rebuilt wholesale on every change, same reasoning as the Windows app. */
#ifndef DNSL_PROVIDERS_WINDOW_H
#define DNSL_PROVIDERS_WINDOW_H

#include <gtk/gtk.h>
#include "remote_controller.h"

typedef void (*ProvidersWindowAutostartChangedFn)(gpointer user_data);

/* Returns a new, unshown GtkWindow wired up to `remote` — caller (tray.c) shows/presents it and
 * is responsible for noticing "destroy" to drop its own reference. Rebuilds its content whenever
 * remote_controller's state changes; call providers_window_refresh() once right after creating it
 * to seed the initial content, and again from the same state-changed callback tray.c already
 * subscribes to.
 *
 * `on_autostart_changed` fires whenever *this window's own* "Start with this session" checkbox is
 * toggled — "Start with this session" is a purely local XDG-autostart file toggle with no daemon
 * involved at all, so unlike every other control here it never triggers a state-changed broadcast
 * that would otherwise refresh the tray menu's own copy of the same checkbox. Without this hook
 * the two visibly desync (toggle it in one place, the other still shows the old state until some
 * unrelated daemon event happens to force a redraw) — tray.c passes a callback here that rebuilds
 * its own menu immediately instead. May be NULL. */
GtkWidget *providers_window_new(GtkWindow *transient_parent, RemoteController *remote,
                                 ProvidersWindowAutostartChangedFn on_autostart_changed, gpointer user_data);
void providers_window_refresh(GtkWidget *window);

#endif
