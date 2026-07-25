#include "add_provider_dialog.h"
#include "ui_widgets.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

static gboolean looks_like_ip(const gchar *s)
{
    struct in_addr a4;
    struct in6_addr a6;
    return inet_pton(AF_INET, s, &a4) == 1 || inet_pton(AF_INET6, s, &a6) == 1;
}

static GtkWidget *labeled_field(GtkWidget *box, const gchar *label_text, GtkWidget *field)
{
    gtk_box_pack_start(GTK_BOX(box), ui_label_label_new(label_text), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), field, FALSE, FALSE, 0);
    return field;
}

/* GtkDialog's action area (holding Cancel/Add) is a separate widget from the content area, a
 * sibling in the dialog's own outer box — the "content-pad-20" class applied to our own content
 * box has no effect on it at all, which is why the buttons sat flush against the window's own
 * edges. gtk_container_set_border_width() is a documented no-op for child layout on this GTK
 * build (see ../linker-linux/CLAUDE.md), so margin on the action area widget itself (offsets a
 * widget relative to its own parent — exactly what's needed here) is the fix, not padding. */
static void pad_dialog_action_area(GtkDialog *dialog)
{
    GtkWidget *action_area = gtk_dialog_get_action_area(dialog);
    gtk_widget_set_margin_start(action_area, 20);
    gtk_widget_set_margin_end(action_area, 20);
    gtk_widget_set_margin_top(action_area, 4);
    gtk_widget_set_margin_bottom(action_area, 16);
}

/* gtk_entry_get_text() returns a pointer straight into the GtkEntry's own internal
 * GtkEntryBuffer — const-qualified specifically because callers must not mutate it. g_strstrip()
 * mutates its argument in place (shifts bytes left, writes a '\0' mid-buffer), which desyncs the
 * buffer's real content from the length GtkEntryBuffer separately tracks for it. Confirmed by
 * hand: calling g_strstrip() directly on gtk_entry_get_text()'s return value (as this code
 * originally did in five places) crashed the app on submitting the "Add NextDNS" dialog. Always
 * copy first — g_strdup() then g_strstrip() the owned copy, never the widget's own buffer. */
static gchar *entry_text_stripped(GtkEntry *entry)
{
    gchar *copy = g_strdup(gtk_entry_get_text(entry));
    g_strstrip(copy);
    return copy;
}

DnsProvider *add_provider_dialog_run_custom(GtkWindow *parent)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Add Custom Provider", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, -1);

    GtkWidget *cancel = gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget *add = gtk_dialog_add_button(GTK_DIALOG(dialog), "Add", GTK_RESPONSE_OK);
    gtk_style_context_add_class(gtk_widget_get_style_context(cancel), "dnsl-text-button");
    gtk_style_context_add_class(gtk_widget_get_style_context(cancel), "text-button-neutral");
    gtk_style_context_add_class(gtk_widget_get_style_context(add), "pill-button");
    pad_dialog_action_area(GTK_DIALOG(dialog));

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "content-pad-20");
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget *name_entry = labeled_field(box, "Name", ui_outlined_entry_new());
    GtkWidget *host_entry = labeled_field(box, "TLS hostname (SNI)", ui_outlined_entry_new());
    GtkWidget *ips_entry = labeled_field(box, "IPs (comma-separated)", ui_outlined_entry_new());
    GtkWidget *port_entry = labeled_field(box, "Port", ui_outlined_entry_new());
    gtk_entry_set_text(GTK_ENTRY(port_entry), "853");

    GtkWidget *error_label = ui_body_small_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(error_label), "text-button-error");
    gtk_box_pack_start(GTK_BOX(box), error_label, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(name_entry);

    DnsProvider *result = NULL;
    while (TRUE) {
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response != GTK_RESPONSE_OK) break;

        gchar *name = entry_text_stripped(GTK_ENTRY(name_entry));
        gchar *host = entry_text_stripped(GTK_ENTRY(host_entry));
        gchar *ips_raw = g_strdup(gtk_entry_get_text(GTK_ENTRY(ips_entry)));
        gchar *port_raw = entry_text_stripped(GTK_ENTRY(port_entry));

        if (!*name) {
            gtk_label_set_text(GTK_LABEL(error_label), "Give it a name first");
            g_free(name); g_free(host); g_free(ips_raw); g_free(port_raw);
            continue;
        }
        if (!*host) {
            gtk_label_set_text(GTK_LABEL(error_label), "Enter the resolver's TLS hostname");
            g_free(name); g_free(host); g_free(ips_raw); g_free(port_raw);
            continue;
        }

        gchar **parts = g_strsplit(ips_raw, ",", -1);
        g_free(ips_raw);
        GPtrArray *ips = g_ptr_array_new();
        for (int i = 0; parts[i]; i++) {
            gchar *trimmed = g_strstrip(parts[i]);
            if (*trimmed && looks_like_ip(trimmed)) g_ptr_array_add(ips, trimmed);
        }
        if (ips->len == 0) {
            gtk_label_set_text(GTK_LABEL(error_label), "Enter at least one valid IP address");
            g_ptr_array_free(ips, TRUE);
            g_strfreev(parts);
            g_free(name); g_free(host); g_free(port_raw);
            continue;
        }

        gchar *end = NULL;
        long port = strtol(port_raw, &end, 10);
        if (*port_raw == '\0' || (end && *end != '\0') || port <= 0 || port > 65535) {
            gtk_label_set_text(GTK_LABEL(error_label), "Port must be a number between 1 and 65535");
            g_ptr_array_free(ips, TRUE);
            g_strfreev(parts);
            g_free(name); g_free(host); g_free(port_raw);
            continue;
        }

        result = dns_provider_new_custom(name, host, (const gchar *const *)ips->pdata, ips->len, (int)port);
        g_ptr_array_free(ips, TRUE);
        g_strfreev(parts);
        g_free(name); g_free(host); g_free(port_raw);
        break;
    }

    gtk_widget_destroy(dialog);
    return result;
}

DnsProvider *add_provider_dialog_run_nextdns(GtkWindow *parent)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Add NextDNS", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 360, -1);

    GtkWidget *cancel = gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget *add = gtk_dialog_add_button(GTK_DIALOG(dialog), "Add", GTK_RESPONSE_OK);
    gtk_style_context_add_class(gtk_widget_get_style_context(cancel), "dnsl-text-button");
    gtk_style_context_add_class(gtk_widget_get_style_context(cancel), "text-button-neutral");
    gtk_style_context_add_class(gtk_widget_get_style_context(add), "pill-button");
    pad_dialog_action_area(GTK_DIALOG(dialog));

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "content-pad-20");
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget *config_entry = labeled_field(box, "NextDNS config id", ui_outlined_entry_new());
    GtkWidget *name_entry = labeled_field(box, "Custom name (optional)", ui_outlined_entry_new());

    GtkWidget *error_label = ui_body_small_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(error_label), "text-button-error");
    gtk_box_pack_start(GTK_BOX(box), error_label, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(config_entry);

    DnsProvider *result = NULL;
    while (TRUE) {
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response != GTK_RESPONSE_OK) break;

        gchar *config_id = entry_text_stripped(GTK_ENTRY(config_entry));
        if (!*config_id) {
            gtk_label_set_text(GTK_LABEL(error_label), "Enter your NextDNS config id first");
            g_free(config_id);
            continue;
        }

        result = dns_provider_new_nextdns(config_id);
        g_free(config_id);

        gchar *custom_name = entry_text_stripped(GTK_ENTRY(name_entry));
        if (*custom_name) {
            g_free(result->name);
            result->name = g_strdup(custom_name);
        }
        g_free(custom_name);
        break;
    }

    gtk_widget_destroy(dialog);
    return result;
}
