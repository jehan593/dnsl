#include "theme.h"

static const NordPalette DARK_PALETTE = {
    .primary = "#88C0D0", .on_primary = "#2E3440", .primary_container = "#5E81AC", .on_primary_container = "#ECEFF4",
    .secondary = "#8FBCBB", .on_secondary = "#2E3440", .secondary_container = "#434C5E", .on_secondary_container = "#ECEFF4",
    .tertiary = "#81A1C1", .on_tertiary = "#2E3440", .tertiary_container = "#434C5E", .on_tertiary_container = "#ECEFF4",
    .background = "#2E3440", .on_background = "#ECEFF4",
    .surface = "#3B4252", .on_surface = "#D8DEE9", .surface_variant = "#434C5E", .on_surface_variant = "#D8DEE9",
    .surface_dim = "#2E3440", .surface_bright = "#434C5E",
    .surface_container_lowest = "#2E3440", .surface_container_low = "#2E3440", .surface_container = "#3B4252",
    .surface_container_high = "#3B4252", .surface_container_highest = "#434C5E",
    .error = "#BF616A", .on_error = "#ECEFF4", .error_container = "#BF616A", .on_error_container = "#ECEFF4",
    .outline = "#4C566A", .outline_variant = "#434C5E",
    .inverse_surface = "#ECEFF4", .inverse_on_surface = "#2E3440", .inverse_primary = "#5E81AC",
    .row_hover = "#434C5E",
};

static const NordPalette LIGHT_PALETTE = {
    .primary = "#5E81AC", .on_primary = "#ECEFF4", .primary_container = "#88C0D0", .on_primary_container = "#2E3440",
    .secondary = "#8FBCBB", .on_secondary = "#2E3440", .secondary_container = "#E5E9F0", .on_secondary_container = "#2E3440",
    .tertiary = "#81A1C1", .on_tertiary = "#ECEFF4", .tertiary_container = "#E5E9F0", .on_tertiary_container = "#2E3440",
    .background = "#ECEFF4", .on_background = "#2E3440",
    .surface = "#E5E9F0", .on_surface = "#3B4252", .surface_variant = "#D8DEE9", .on_surface_variant = "#3B4252",
    .surface_dim = "#D8DEE9", .surface_bright = "#ECEFF4",
    .surface_container_lowest = "#ECEFF4", .surface_container_low = "#E5E9F0", .surface_container = "#D8DEE9",
    .surface_container_high = "#D8DEE9", .surface_container_highest = "#D8DEE9",
    .error = "#BF616A", .on_error = "#ECEFF4", .error_container = "#BF616A", .on_error_container = "#ECEFF4",
    .outline = "#4C566A", .outline_variant = "#D8DEE9",
    .inverse_surface = "#2E3440", .inverse_on_surface = "#ECEFF4", .inverse_primary = "#88C0D0",
    .row_hover = "#E5E9F0",
};

static GtkCssProvider *g_provider = NULL;
static gboolean g_is_dark = TRUE;
static GDBusConnection *g_session_bus = NULL;
static GSettings *g_gnome_settings = NULL;

const NordPalette *dnsl_theme_palette(void) {
    return g_is_dark ? &DARK_PALETTE : &LIGHT_PALETTE;
}

gboolean dnsl_theme_is_dark(void) {
    return g_is_dark;
}

static gchar *generate_css(const NordPalette *p) {
    GString *css = g_string_new(NULL);

#define ADD(...) g_string_append_printf(css, __VA_ARGS__)

    ADD("window, dialog { background-color: %s; color: %s; font-family: 'MartianMono NF'; font-size: 13px; }\n",
        p->background, p->on_background);
    ADD("window:backdrop, dialog:backdrop { background-color: %s; color: %s; }\n", p->background, p->on_background);

    /* Suppress GTK's default dashed keyboard-focus ring on our custom controls — the
     * border-color change on entries/textviews and the checked-state underline on
     * tabs are already sufficient focus indication, and the box-drawn ring reads as
     * a rendering glitch against the custom pill/tab shapes. */
    ADD("button:focus, switch:focus, entry:focus, textview:focus { outline-style: none; outline-width: 0; }\n");

    ADD(".dnsl-title { font-family: 'MartianMono NF Med'; font-size: 16px; }\n");
    ADD(".dnsl-label { font-family: 'MartianMono NF Med'; font-size: 12px; color: %s; }\n", p->on_surface_variant);
    ADD(".dnsl-body-small { font-size: 12px; color: %s; }\n", p->on_surface_variant);
    ADD(".dnsl-url-text { color: %s; font-size: 13px; }\n", p->primary);
    ADD(".dnsl-url-text:backdrop { color: %s; }\n", p->primary);

    ADD(".top-bar { background-color: %s; background-image: none; border-bottom: 1px solid %s; }\n",
        p->surface_container, p->outline_variant);
    ADD(".top-bar:backdrop { background-color: %s; background-image: none; }\n", p->surface_container);
    ADD(".top-bar-title { font-family: 'MartianMono NF Med'; font-size: 18px; }\n");

    ADD(".banner { background-color: %s; background-image: none; border-radius: 12px; padding: 16px; }\n", p->primary_container);
    ADD(".banner:backdrop { background-color: %s; background-image: none; }\n", p->primary_container);
    ADD(".banner-title { color: %s; font-family: 'MartianMono NF Med'; font-size: 14px; }\n", p->on_primary_container);
    ADD(".banner-body { color: %s; font-size: 12px; }\n", p->on_primary_container);
    ADD(".banner-title:backdrop, .banner-body:backdrop { color: %s; }\n", p->on_primary_container);

    ADD(".tab-row { background-color: %s; border-bottom: 1px solid %s; }\n", p->surface, p->outline_variant);
    ADD(".tab-button { background-color: transparent; background-image: none; color: %s; border: none; "
        "border-bottom: 3px solid transparent; border-radius: 0; font-family: 'MartianMono NF Med'; "
        "font-size: 13px; box-shadow: none; }\n",
        p->on_surface_variant);
    ADD(".tab-button:checked { color: %s; border-bottom: 3px solid %s; }\n", p->primary, p->primary);
    ADD(".tab-button:checked:backdrop { color: %s; border-bottom-color: %s; }\n", p->primary, p->primary);
    ADD(".tab-button:hover { background-color: %s; }\n", p->surface_container_high);

    ADD(".pill-button { background-color: %s; background-image: none; color: %s; border-radius: 20px; padding: 8px 16px; "
        "font-family: 'MartianMono NF Med'; border: none; box-shadow: none; text-shadow: none; }\n", p->primary, p->on_primary);
    ADD(".pill-button:backdrop { background-color: %s; background-image: none; color: %s; }\n", p->primary, p->on_primary);
    ADD(".pill-button:hover { opacity: 0.88; }\n");
    ADD(".pill-button:active { opacity: 0.74; }\n");

    ADD(".dnsl-text-button { background-color: %s; background-image: none; border: 1px solid %s; border-radius: 8px; "
        "padding: 7px 14px; margin: 0 4px; font-family: 'MartianMono NF'; box-shadow: none; text-shadow: none; }\n",
        p->surface_container_highest, p->outline_variant);
    ADD(".dnsl-text-button:backdrop { background-color: %s; background-image: none; border-color: %s; }\n",
        p->surface_container_highest, p->outline_variant);
    ADD(".dnsl-text-button:hover { border-color: %s; }\n", p->outline);
    ADD(".dnsl-text-button:disabled { opacity: 0.4; }\n");
    ADD(".text-button-primary { color: %s; }\n", p->primary);
    ADD(".text-button-neutral { color: %s; }\n", p->on_surface_variant);
    ADD(".text-button-error { color: %s; }\n", p->error);
    ADD(".text-button-primary:backdrop { color: %s; }\n", p->primary);
    ADD(".text-button-neutral:backdrop { color: %s; }\n", p->on_surface_variant);
    ADD(".text-button-error:backdrop { color: %s; }\n", p->error);

    ADD(".icon-button { background-color: transparent; background-image: none; border: none; border-radius: 20px; "
        "min-width: 40px; min-height: 40px; padding: 0; box-shadow: none; }\n");
    ADD(".icon-button:hover { background-color: %s; background-image: none; }\n", p->surface_container_high);
    ADD(".icon-button:active { background-color: %s; background-image: none; }\n", p->surface_container_highest);
    ADD(".icon-button:disabled { opacity: 0.4; }\n");
    ADD(".icon-button-small { border-radius: 12px; min-width: 24px; min-height: 24px; }\n");
    ADD(".icon-tint-primary { color: %s; }\n", p->primary);
    ADD(".icon-tint-error { color: %s; }\n", p->error);
    ADD(".icon-tint-primary:backdrop { color: %s; }\n", p->primary);
    ADD(".icon-tint-error:backdrop { color: %s; }\n", p->error);

    ADD("switch { background-color: %s; border: 1px solid %s; border-radius: 11px; "
        "min-width: 40px; min-height: 22px; background-image: none; box-shadow: none; }\n", p->surface_variant, p->outline);
    ADD("switch:checked { background-color: %s; border-color: %s; }\n", p->primary, p->primary);
    ADD("switch slider { background-color: %s; border-radius: 8px; min-width: 16px; min-height: 16px; box-shadow: none; }\n",
        p->on_surface_variant);
    ADD("switch:checked slider { background-color: %s; }\n", p->on_primary);

    ADD(".row-hairline { border-bottom: 1px solid %s; }\n", p->outline_variant);
    ADD(".day-header { background-color: %s; background-image: none; }\n", p->surface);
    ADD(".hairline { background-color: %s; min-height: 1px; }\n", p->outline_variant);
    ADD(".row-hover:hover { background-color: %s; background-image: none; }\n", p->surface_container);
    ADD(".chooser-row { border-radius: 8px; }\n");
    ADD(".chooser-row:hover { background-color: %s; background-image: none; }\n", p->row_hover);

    ADD(".card { background-color: %s; background-image: none; border: 1px solid %s; border-radius: 20px; "
        "box-shadow: 0 4px 24px 0 rgba(0,0,0,0.35); }\n", p->surface_container_high, p->outline_variant);

    ADD("entry.outlined, textview.outlined text { background-color: transparent; }\n");
    ADD("entry.outlined { border: 1px solid %s; border-radius: 8px; padding: 10px 12px; caret-color: %s; box-shadow: none; }\n",
        p->outline, p->primary);
    ADD("entry.outlined:focus { border-color: %s; border-width: 2px; }\n", p->primary);
    ADD("textview.outlined { border: 1px solid %s; border-radius: 8px; caret-color: %s; }\n", p->outline, p->primary);
    ADD("textview.outlined text { padding: 10px 12px; }\n");
    ADD("textview.outlined:focus-within { border-color: %s; border-width: 2px; }\n", p->primary);
    ADD("entry.outlined selection, textview.outlined text selection { background-color: %s; }\n", p->primary_container);

    ADD("scrollbar { background-color: transparent; }\n");
    ADD("scrollbar slider { background-color: %s; border-radius: 4px; min-width: 6px; min-height: 6px; }\n", p->outline);
    ADD("scrollbar slider:hover { background-color: %s; }\n", p->on_surface_variant);
    ADD("scrollbar slider:active { background-color: %s; }\n", p->primary);

    ADD(".toast-bubble { background-color: %s; background-image: none; border-radius: 6px; padding: 6px 12px; }\n",
        p->inverse_surface);
    ADD(".toast-text { color: %s; font-size: 11px; }\n", p->inverse_on_surface);

    ADD("list, listbox { background-color: transparent; }\n");
    ADD("list row, listbox row { background-color: transparent; padding: 0; }\n");
    ADD("list row:selected, listbox row:selected { background-color: transparent; }\n");

    /* Provider list rows (providers_window.c) — the generic listbox rule above deliberately
     * leaves :selected transparent for other lists in this codebase, so these need their own
     * higher-specificity rules (an added class beats a bare type+pseudo-class selector) to get an
     * actual hover effect and a visibly-different "this one's active" highlight, rather than the
     * list looking like plain unclickable text. Must target "list row...", not "listbox row..." —
     * GtkListBox's real CSS node name is "list" (confirmed: the generic rule above only works via
     * its "list row:selected" half; "listbox" never matches anything on this GTK version, it's
     * dead weight kept only for whatever originally motivated the belt-and-suspenders in the
     * generic rule). Using only "listbox row.provider-row..." here originally meant these rules
     * silently matched nothing at all, and the generic transparent rule won by default — no hover,
     * no highlight, confirmed by hand and fixed by switching to "list row.provider-row...". */
    ADD("list row.provider-row { border-radius: 12px; margin: 2px 8px; "
        "transition: background-color 120ms ease, border-color 120ms ease; "
        "border: 1px solid transparent; }\n");
    ADD("list row.provider-row:hover { background-color: %s; background-image: none; }\n",
        p->surface_container_high);
    ADD("list row.provider-row:selected { background-color: %s; background-image: none; "
        "border-color: %s; }\n", p->primary_container, p->primary);
    ADD("list row.provider-row:selected:hover { background-color: %s; background-image: none; }\n",
        p->primary_container);
    ADD("list row.provider-row:selected .dnsl-title, "
        "list row.provider-row:selected .dnsl-body-small { color: %s; }\n", p->on_primary_container);

    /* gtk_container_set_border_width() is a no-op for GtkBox/GtkDialog content-area
     * child layout on this GTK build (confirmed via allocation dump: children land
     * flush with the container's own edge regardless of border-width). Use real CSS
     * padding instead everywhere inner-content inset is needed. */
    ADD(".content-pad-16 { padding: 16px; }\n");
    ADD(".content-pad-20 { padding: 20px; }\n");

#undef ADD
    return g_string_free(css, FALSE);
}

static void apply_css(void) {
    gchar *css = generate_css(dnsl_theme_palette());
    GError *css_error = NULL;
    gtk_css_provider_load_from_data(g_provider, css, -1, &css_error);
    if (css_error) {
        g_warning("CSS PARSE ERROR: %s", css_error->message);
        g_clear_error(&css_error);
    }
    g_free(css);
}

/* Returns 1 = dark, 0 = light, -1 = unknown/no preference expressed. */
static gint detect_dark_via_portal(GDBusConnection *bus) {
    if (!bus) return -1;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", "Read",
        g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, &error);
    if (!result) {
        g_clear_error(&error);
        return -1;
    }
    GVariant *inner = NULL;
    g_variant_get(result, "(v)", &inner);
    guint32 value = 0;
    gint outcome = -1;
    if (inner) {
        GVariant *unwrapped = inner;
        while (unwrapped && g_variant_is_of_type(unwrapped, G_VARIANT_TYPE_VARIANT)) {
            GVariant *next = g_variant_get_variant(unwrapped);
            g_variant_unref(unwrapped);
            unwrapped = next;
        }
        if (unwrapped && g_variant_is_of_type(unwrapped, G_VARIANT_TYPE_UINT32)) {
            value = g_variant_get_uint32(unwrapped);
            outcome = (value == 1) ? 1 : (value == 2) ? 0 : -1;
        }
        if (unwrapped) g_variant_unref(unwrapped);
    }
    g_variant_unref(result);
    return outcome;
}

static gint detect_dark_via_gsettings(void) {
    if (!g_gnome_settings) return -1;
    gchar *scheme = g_settings_get_string(g_gnome_settings, "color-scheme");
    gint outcome = -1;
    if (g_strcmp0(scheme, "prefer-dark") == 0) outcome = 1;
    else if (g_strcmp0(scheme, "prefer-light") == 0) outcome = 0;
    g_free(scheme);
    return outcome;
}

static void redetect_and_apply(void) {
    gint outcome = detect_dark_via_portal(g_session_bus);
    if (outcome < 0) outcome = detect_dark_via_gsettings();
    g_is_dark = (outcome < 0) ? TRUE : (outcome == 1); /* default to dark, matching the Windows app */
    apply_css();
}

static void on_portal_setting_changed(GDBusConnection *connection, const gchar *sender_name,
                                       const gchar *object_path, const gchar *interface_name,
                                       const gchar *signal_name, GVariant *parameters, gpointer user_data) {
    (void) connection; (void) sender_name; (void) object_path; (void) interface_name;
    (void) signal_name; (void) user_data;
    const gchar *ns = NULL, *key = NULL;
    GVariant *value = NULL;
    g_variant_get(parameters, "(&s&sv)", &ns, &key, &value);
    if (g_strcmp0(ns, "org.freedesktop.appearance") == 0 && g_strcmp0(key, "color-scheme") == 0) {
        redetect_and_apply();
    }
    if (value) g_variant_unref(value);
}

static void on_gsettings_changed(GSettings *settings, const gchar *key, gpointer user_data) {
    (void) settings; (void) key; (void) user_data;
    redetect_and_apply();
}

void dnsl_theme_init(void) {
    g_provider = gtk_css_provider_new();
    /* Dnsl has its own fixed Nord design system and must render identically
     * regardless of the user's system GTK theme — but GTK auto-loads
     * ~/.config/gtk-3.0/gtk.css at GTK_STYLE_PROVIDER_PRIORITY_USER, which outranks
     * PRIORITY_APPLICATION and was silently overriding our button/card colors with
     * the user's own theme accent colors. Go one priority level above USER so our
     * stylesheet always wins. */
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(g_provider), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

    GError *error = NULL;
    g_session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!g_session_bus) {
        g_warning("no session D-Bus available for theme detection: %s", error ? error->message : "unknown");
        g_clear_error(&error);
    }

    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    if (source) {
        GSettingsSchema *schema = g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE);
        if (schema) {
            g_gnome_settings = g_settings_new("org.gnome.desktop.interface");
            g_settings_schema_unref(schema);
        }
    }

    redetect_and_apply();

    if (g_session_bus) {
        g_dbus_connection_signal_subscribe(
            g_session_bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings",
            "SettingChanged", "/org/freedesktop/portal/desktop", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_portal_setting_changed, NULL, NULL);
    }
    if (g_gnome_settings) {
        g_signal_connect(g_gnome_settings, "changed::color-scheme", G_CALLBACK(on_gsettings_changed), NULL);
    }
}
