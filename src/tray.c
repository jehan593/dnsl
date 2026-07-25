#include "tray.h"
#include "providers_window.h"
#include "installer.h"
#include "autostart.h"
#include <libayatana-appindicator/app-indicator.h>

struct TrayController {
    GtkApplication *app;
    RemoteController *remote;
    AppIndicator *indicator;
    gchar *icon_dir;
    GtkWidget *menu;
    GtkWidget *providers_window;
    gchar *pending_error; /* shown once on the next rebuild, then cleared — never sticky */
};

typedef struct {
    TrayController *tc;
    gchar *provider_id;
} ProviderMenuData;

static void provider_menu_data_free(gpointer p, GClosure *closure)
{
    (void)closure;
    ProviderMenuData *d = p;
    g_free(d->provider_id);
    g_free(d);
}

static void rebuild_menu(TrayController *tc);

static void on_state_changed(gpointer user_data)
{
    TrayController *tc = user_data;
    rebuild_menu(tc);
    if (tc->providers_window) providers_window_refresh(tc->providers_window);
}

static void on_remote_error(const gchar *message, gpointer user_data)
{
    TrayController *tc = user_data;
    g_free(tc->pending_error);
    tc->pending_error = g_strdup(message);
    rebuild_menu(tc);
}

static void on_toggle_clicked(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    TrayController *tc = user_data;
    IpcStatus *status = remote_controller_snapshot(tc->remote);
    gboolean enabled = status && status->enabled;
    if (status) ipc_status_free(status);
    if (enabled) remote_controller_disable(tc->remote);
    else remote_controller_enable(tc->remote);
}

static void on_provider_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
    ProviderMenuData *data = user_data;
    if (gtk_check_menu_item_get_active(item)) remote_controller_select_provider(data->tc->remote, data->provider_id);
}

static void on_manage_clicked(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    tray_controller_show_providers_window(user_data);
}

static void on_autostart_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
    TrayController *tc = user_data;
    autostart_set_enabled(gtk_check_menu_item_get_active(item));
    /* "Start with this session" is a purely local file toggle, not a daemon round-trip — nothing
     * else would otherwise tell the providers window (if open) to pick up the new state. */
    if (tc->providers_window) providers_window_refresh(tc->providers_window);
}

static gboolean idle_install_failed(gpointer user_data)
{
    TrayController *tc = user_data;
    g_free(tc->pending_error);
    tc->pending_error = g_strdup("Couldn't install/start the background service.");
    rebuild_menu(tc);
    return G_SOURCE_REMOVE;
}

static gpointer install_thread(gpointer user_data)
{
    TrayController *tc = user_data;
    InstallerResult result = installer_ensure_installed_and_running();
    if (result == INSTALLER_FAILED) {
        g_idle_add(idle_install_failed, tc);
    }
    /* On success (or user cancellation), RemoteController's own reconnect loop notices the
     * now-running daemon and fires the state-changed callback on its own — nothing else to do. */
    return NULL;
}

static void on_install_clicked(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    GThread *t = g_thread_new("dnsl-install", install_thread, user_data);
    g_thread_unref(t);
}

static void on_exit_clicked(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    TrayController *tc = user_data;
    /* Exit only closes the tray — deliberately does not touch protection state, which lives in
     * the daemon independent of whether the tray happens to be open. */
    app_indicator_set_status(tc->indicator, APP_INDICATOR_STATUS_PASSIVE);
    g_application_quit(G_APPLICATION(tc->app));
}

static GtkWidget *append_item(GtkWidget *menu, const gchar *label, gboolean sensitive)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    gtk_widget_set_sensitive(item, sensitive);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static void append_separator(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static void rebuild_disconnected(TrayController *tc, GtkWidget *menu)
{
    append_item(menu, "Background service not connected", FALSE);
    append_separator(menu);

    GtkWidget *install_item = gtk_menu_item_new_with_label("Install / start background service…");
    g_signal_connect(install_item, "activate", G_CALLBACK(on_install_clicked), tc);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), install_item);
    append_separator(menu);

    GtkWidget *exit_item = gtk_menu_item_new_with_label("Exit");
    g_signal_connect(exit_item, "activate", G_CALLBACK(on_exit_clicked), tc);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), exit_item);

    gchar *icon = g_strdup_printf("dnsl-disabled-48");
    app_indicator_set_icon_full(tc->indicator, icon, "dnsl");
    g_free(icon);
    app_indicator_set_title(tc->indicator, "dnsl — background service not connected");
}

static void rebuild_connected(TrayController *tc, GtkWidget *menu, IpcStatus *status)
{
    const DnsProvider *current = NULL;
    for (guint i = 0; i < status->providers->len; i++) {
        DnsProvider *p = g_ptr_array_index(status->providers, i);
        if (g_strcmp0(p->id, status->selected_provider_id) == 0) { current = p; break; }
    }

    gchar *status_text = tc->pending_error
        ? g_strdup(tc->pending_error)
        : (status->enabled ? g_strdup_printf("Protected via %s", current ? current->name : "?")
                            : g_strdup("Not protected"));
    g_clear_pointer(&tc->pending_error, g_free);

    append_item(menu, status_text, FALSE);
    append_separator(menu);

    GtkWidget *toggle_item = gtk_menu_item_new_with_label(status->enabled ? "Disable protection" : "Enable protection");
    g_signal_connect(toggle_item, "activate", G_CALLBACK(on_toggle_clicked), tc);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), toggle_item);

    GtkWidget *provider_item = gtk_menu_item_new_with_label("DNS Provider");
    GtkWidget *submenu = gtk_menu_new();
    GSList *group = NULL;
    for (guint i = 0; i < status->providers->len; i++) {
        DnsProvider *p = g_ptr_array_index(status->providers, i);
        GtkWidget *radio = gtk_radio_menu_item_new_with_label(group, p->name);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
        if (g_strcmp0(p->id, status->selected_provider_id) == 0) {
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(radio), TRUE);
        }
        ProviderMenuData *data = g_new0(ProviderMenuData, 1);
        data->tc = tc;
        data->provider_id = g_strdup(p->id);
        g_signal_connect_data(radio, "toggled", G_CALLBACK(on_provider_toggled), data,
                               provider_menu_data_free, 0);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), radio);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(provider_item), submenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), provider_item);

    GtkWidget *manage_item = gtk_menu_item_new_with_label("Manage Providers…");
    g_signal_connect(manage_item, "activate", G_CALLBACK(on_manage_clicked), tc);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), manage_item);
    append_separator(menu);

    GtkWidget *autostart_item = gtk_check_menu_item_new_with_label("Start with this session");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(autostart_item), autostart_is_enabled());
    g_signal_connect(autostart_item, "toggled", G_CALLBACK(on_autostart_toggled), tc);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), autostart_item);
    append_separator(menu);

    GtkWidget *exit_item = gtk_menu_item_new_with_label("Exit");
    g_signal_connect(exit_item, "activate", G_CALLBACK(on_exit_clicked), tc);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), exit_item);

    gchar *icon = g_strdup_printf(status->enabled ? "dnsl-enabled-48" : "dnsl-disabled-48");
    app_indicator_set_icon_full(tc->indicator, icon, "dnsl");
    g_free(icon);
    app_indicator_set_title(tc->indicator, status_text);
    g_free(status_text);
}

static void rebuild_menu(TrayController *tc)
{
    if (tc->menu) gtk_widget_destroy(tc->menu);
    GtkWidget *menu = gtk_menu_new();
    tc->menu = menu;

    if (!remote_controller_is_connected(tc->remote)) {
        rebuild_disconnected(tc, menu);
    } else {
        IpcStatus *status = remote_controller_snapshot(tc->remote);
        if (status) {
            rebuild_connected(tc, menu, status);
            ipc_status_free(status);
        } else {
            rebuild_disconnected(tc, menu);
        }
    }

    gtk_widget_show_all(menu);
    app_indicator_set_menu(tc->indicator, GTK_MENU(menu));
    app_indicator_set_status(tc->indicator, APP_INDICATOR_STATUS_ACTIVE);
}

static void on_providers_window_destroyed(GtkWidget *window, gpointer user_data)
{
    (void)window;
    TrayController *tc = user_data;
    tc->providers_window = NULL;
}

static void on_providers_window_autostart_changed(gpointer user_data)
{
    /* Mirror of tray.c's own on_autostart_toggled, in the other direction: the providers window's
     * checkbox is a purely local file toggle too, so nothing else would tell the tray menu's own
     * copy of this checkbox to catch up otherwise. */
    rebuild_menu((TrayController *)user_data);
}

void tray_controller_show_providers_window(TrayController *tc)
{
    if (!tc->providers_window) {
        tc->providers_window = providers_window_new(NULL, tc->remote, on_providers_window_autostart_changed, tc);
        g_signal_connect(tc->providers_window, "destroy", G_CALLBACK(on_providers_window_destroyed), tc);
    }
    providers_window_refresh(tc->providers_window);
    gtk_widget_show_all(tc->providers_window);
    gtk_window_present(GTK_WINDOW(tc->providers_window));
}

TrayController *tray_controller_new(GtkApplication *app, RemoteController *remote, const gchar *asset_dir)
{
    TrayController *tc = g_new0(TrayController, 1);
    tc->app = app;
    tc->remote = remote;
    tc->icon_dir = asset_dir ? g_build_filename(asset_dir, "icons", NULL) : NULL;

    tc->indicator = app_indicator_new("dnsl", "dnsl-disabled-48", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (tc->icon_dir) app_indicator_set_icon_theme_path(tc->indicator, tc->icon_dir);
    app_indicator_set_status(tc->indicator, APP_INDICATOR_STATUS_ACTIVE);

    remote_controller_set_callbacks(remote, on_state_changed, on_remote_error, tc);
    rebuild_menu(tc);

    return tc;
}

void tray_controller_free(TrayController *tc)
{
    if (!tc) return;
    if (tc->menu) gtk_widget_destroy(tc->menu);
    if (tc->providers_window) gtk_widget_destroy(tc->providers_window);
    g_free(tc->icon_dir);
    g_free(tc->pending_error);
    g_object_unref(tc->indicator);
    g_free(tc);
}
