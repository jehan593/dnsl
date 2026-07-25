#include "providers_window.h"
#include "add_provider_dialog.h"
#include "ui_widgets.h"
#include "autostart.h"

typedef struct {
    RemoteController *remote;
    GtkWidget *status_label;
    GtkWidget *toggle_button;
    GtkWidget *list_box;
    gulong list_box_selected_handler;
    GtkWidget *autostart_check;
    gulong autostart_handler;
    ProvidersWindowAutostartChangedFn on_autostart_changed;
    gpointer autostart_changed_user_data;
} ProvidersWindowData;

static void data_free(gpointer p) { g_free(p); }

static void destroy_widget_cb(GtkWidget *widget, gpointer user_data)
{
    (void)user_data;
    gtk_widget_destroy(widget);
}

static void on_toggle_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    ProvidersWindowData *data = user_data;
    IpcStatus *status = remote_controller_snapshot(data->remote);
    gboolean enabled = status && status->enabled;
    if (status) ipc_status_free(status);
    if (enabled) remote_controller_disable(data->remote);
    else remote_controller_enable(data->remote);
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    ProvidersWindowData *data = user_data;
    if (!row) return;
    const gchar *provider_id = g_object_get_data(G_OBJECT(row), "provider-id");
    if (provider_id) remote_controller_select_provider(data->remote, provider_id);
}

static void on_delete_clicked(GtkButton *button, gpointer user_data)
{
    ProvidersWindowData *data = g_object_get_data(G_OBJECT(button), "pw-data");
    (void)user_data;
    const gchar *provider_id = g_object_get_data(G_OBJECT(button), "provider-id");
    if (provider_id) remote_controller_remove_custom_provider(data->remote, provider_id);
}

static void on_add_custom_clicked(GtkButton *button, gpointer user_data)
{
    ProvidersWindowData *data = user_data;
    GtkWidget *window = gtk_widget_get_toplevel(GTK_WIDGET(button));
    DnsProvider *provider = add_provider_dialog_run_custom(GTK_WINDOW(window));
    if (provider) remote_controller_add_custom_provider(data->remote, provider);
}

static void on_add_nextdns_clicked(GtkButton *button, gpointer user_data)
{
    ProvidersWindowData *data = user_data;
    GtkWidget *window = gtk_widget_get_toplevel(GTK_WIDGET(button));
    DnsProvider *provider = add_provider_dialog_run_nextdns(GTK_WINDOW(window));
    if (provider) remote_controller_add_custom_provider(data->remote, provider);
}

static void on_autostart_toggled(GtkToggleButton *check, gpointer user_data)
{
    ProvidersWindowData *data = user_data;
    autostart_set_enabled(gtk_toggle_button_get_active(check));
    if (data->on_autostart_changed) data->on_autostart_changed(data->autostart_changed_user_data);
}

static void on_close_clicked(GtkButton *button, gpointer user_data)
{
    (void)user_data;
    gtk_widget_destroy(gtk_widget_get_toplevel(GTK_WIDGET(button)));
}

static GtkWidget *build_provider_row(ProvidersWindowData *data, const DnsProvider *provider)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "provider-row");
    g_object_set_data_full(G_OBJECT(row), "provider-id", g_strdup(provider->id), g_free);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(hbox), "content-pad-16");
    gtk_container_add(GTK_CONTAINER(row), hbox);

    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *name_label = ui_title_label_new(provider->name);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
    gtk_box_pack_start(GTK_BOX(text_box), name_label, FALSE, FALSE, 0);

    GString *ips = g_string_new(NULL);
    for (guint i = 0; i < provider->ip_count; i++) {
        if (i > 0) g_string_append(ips, ", ");
        g_string_append(ips, provider->ips[i]);
    }
    gchar *detail = g_strdup_printf("%s · %s", provider->tls_host, ips->str);
    g_string_free(ips, TRUE);
    GtkWidget *detail_label = ui_body_small_label_new(detail);
    g_free(detail);
    gtk_label_set_line_wrap(GTK_LABEL(detail_label), TRUE);
    gtk_box_pack_start(GTK_BOX(text_box), detail_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), text_box, TRUE, TRUE, 0);

    if (provider->is_custom) {
        GtkWidget *delete_button = ui_icon_button_new("edit-delete-symbolic", "Remove", TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(delete_button), "icon-tint-error");
        g_object_set_data_full(G_OBJECT(delete_button), "provider-id", g_strdup(provider->id), g_free);
        g_object_set_data(G_OBJECT(delete_button), "pw-data", data);
        g_signal_connect(delete_button, "clicked", G_CALLBACK(on_delete_clicked), NULL);
        gtk_widget_set_valign(delete_button, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(hbox), delete_button, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(row);
    return row;
}

void providers_window_refresh(GtkWidget *window)
{
    ProvidersWindowData *data = g_object_get_data(G_OBJECT(window), "pw-data");
    IpcStatus *status = remote_controller_snapshot(data->remote);
    gboolean connected = remote_controller_is_connected(data->remote);

    if (!connected || !status) {
        gtk_label_set_text(GTK_LABEL(data->status_label), "Background service not connected — see the tray menu to install/start it.");
        gtk_button_set_label(GTK_BUTTON(data->toggle_button), "Enable");
        gtk_widget_set_sensitive(data->toggle_button, FALSE);
        gtk_container_foreach(GTK_CONTAINER(data->list_box), destroy_widget_cb, NULL);
        if (status) ipc_status_free(status);
        return;
    }

    gtk_widget_set_sensitive(data->toggle_button, TRUE);
    const DnsProvider *current = NULL;
    for (guint i = 0; i < status->providers->len; i++) {
        DnsProvider *p = g_ptr_array_index(status->providers, i);
        if (g_strcmp0(p->id, status->selected_provider_id) == 0) { current = p; break; }
    }

    gchar *status_text = status->enabled
        ? g_strdup_printf("Protected via %s", current ? current->name : "?")
        : g_strdup("Not protected — DNS is on automatic defaults");
    gtk_label_set_text(GTK_LABEL(data->status_label), status_text);
    g_free(status_text);
    gtk_button_set_label(GTK_BUTTON(data->toggle_button), status->enabled ? "Disable" : "Enable");

    g_signal_handler_block(data->autostart_check, data->autostart_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->autostart_check), autostart_is_enabled());
    g_signal_handler_unblock(data->autostart_check, data->autostart_handler);

    gtk_container_foreach(GTK_CONTAINER(data->list_box), destroy_widget_cb, NULL);
    g_signal_handler_block(data->list_box, data->list_box_selected_handler);
    GtkListBoxRow *selected_row = NULL;
    for (guint i = 0; i < status->providers->len; i++) {
        DnsProvider *p = g_ptr_array_index(status->providers, i);
        GtkWidget *row = build_provider_row(data, p);
        gtk_list_box_insert(GTK_LIST_BOX(data->list_box), row, -1);
        if (g_strcmp0(p->id, status->selected_provider_id) == 0) selected_row = GTK_LIST_BOX_ROW(row);
    }
    gtk_list_box_select_row(GTK_LIST_BOX(data->list_box), selected_row);
    g_signal_handler_unblock(data->list_box, data->list_box_selected_handler);

    ipc_status_free(status);
}

GtkWidget *providers_window_new(GtkWindow *transient_parent, RemoteController *remote,
                                 ProvidersWindowAutostartChangedFn on_autostart_changed, gpointer user_data)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "dnsl — DNS Providers");
    gtk_window_set_default_size(GTK_WINDOW(window), 420, 520);
    if (transient_parent) gtk_window_set_transient_for(GTK_WINDOW(window), transient_parent);

    ProvidersWindowData *data = g_new0(ProvidersWindowData, 1);
    data->remote = remote;
    data->on_autostart_changed = on_autostart_changed;
    data->autostart_changed_user_data = user_data;
    g_object_set_data_full(G_OBJECT(window), "pw-data", data, data_free);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(root), "content-pad-20");
    gtk_container_add(GTK_CONTAINER(window), root);

    data->status_label = ui_body_small_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(data->status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(root), data->status_label, FALSE, FALSE, 0);

    data->toggle_button = ui_pill_button_new("Enable");
    gtk_widget_set_halign(data->toggle_button, GTK_ALIGN_START);
    g_signal_connect(data->toggle_button, "clicked", G_CALLBACK(on_toggle_clicked), data);
    gtk_box_pack_start(GTK_BOX(root), data->toggle_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), ui_hairline_new(), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(root), ui_label_label_new("DNS Provider"), FALSE, FALSE, 0);

    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    data->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(data->list_box), GTK_SELECTION_SINGLE);
    data->list_box_selected_handler = g_signal_connect(data->list_box, "row-selected", G_CALLBACK(on_row_selected), data);
    gtk_container_add(GTK_CONTAINER(scroller), data->list_box);
    gtk_box_pack_start(GTK_BOX(root), scroller, TRUE, TRUE, 0);

    GtkWidget *add_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add_custom = ui_text_button_new("+ Custom", "text-button-neutral");
    g_signal_connect(add_custom, "clicked", G_CALLBACK(on_add_custom_clicked), data);
    GtkWidget *add_nextdns = ui_text_button_new("+ NextDNS", "text-button-neutral");
    g_signal_connect(add_nextdns, "clicked", G_CALLBACK(on_add_nextdns_clicked), data);
    gtk_box_pack_start(GTK_BOX(add_row), add_custom, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(add_row), add_nextdns, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), add_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), ui_hairline_new(), FALSE, FALSE, 4);

    data->autostart_check = gtk_check_button_new_with_label("Start with this session");
    data->autostart_handler = g_signal_connect(data->autostart_check, "toggled", G_CALLBACK(on_autostart_toggled), data);
    gtk_box_pack_start(GTK_BOX(root), data->autostart_check, FALSE, FALSE, 0);

    GtkWidget *bottom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *close_button = ui_text_button_new("Close", "text-button-neutral");
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(bottom_row), close_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), bottom_row, FALSE, FALSE, 0);

    return window;
}
