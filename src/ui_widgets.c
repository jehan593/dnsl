#include "ui_widgets.h"

static void add_class(GtkWidget *widget, const gchar *class_name) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), class_name);
}

GtkWidget *ui_pill_button_new(const gchar *label_text) {
    GtkWidget *button = gtk_button_new_with_label(label_text);
    add_class(button, "flat");
    add_class(button, "pill-button");
    return button;
}

GtkWidget *ui_text_button_new(const gchar *label_text, const gchar *color_class) {
    GtkWidget *button = gtk_button_new_with_label(label_text);
    add_class(button, "flat");
    add_class(button, "dnsl-text-button");
    add_class(button, color_class);
    return button;
}

GtkWidget *ui_icon_button_new(const gchar *icon_name, const gchar *tooltip, gboolean small) {
    GtkWidget *image = gtk_image_new_from_icon_name(icon_name, small ? GTK_ICON_SIZE_MENU : GTK_ICON_SIZE_BUTTON);
    GtkWidget *button = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(button), image);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    add_class(button, "flat");
    add_class(button, "icon-button");
    if (small) add_class(button, "icon-button-small");
    if (tooltip) gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

GtkWidget *ui_title_label_new(const gchar *text) {
    GtkWidget *label = gtk_label_new(text);
    add_class(label, "dnsl-title");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

GtkWidget *ui_label_label_new(const gchar *text) {
    GtkWidget *label = gtk_label_new(text);
    add_class(label, "dnsl-label");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

GtkWidget *ui_body_small_label_new(const gchar *text) {
    GtkWidget *label = gtk_label_new(text);
    add_class(label, "dnsl-body-small");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

GtkWidget *ui_outlined_entry_new(void) {
    GtkWidget *entry = gtk_entry_new();
    add_class(entry, "outlined");
    return entry;
}

GtkWidget *ui_outlined_textview_new(GtkWidget **out_textview) {
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    add_class(scroller, "outlined");

    GtkWidget *textview = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(textview), FALSE);
    /* CSS padding on the "text" node (theme.c) only affects that node's background/clip box —
     * GtkTextView positions its actual text via these margin properties regardless of CSS
     * padding, so both are needed to match the outlined entry's inset. */
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textview), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textview), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(textview), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(textview), 10);
    add_class(textview, "outlined");

    gtk_container_add(GTK_CONTAINER(scroller), textview);
    if (out_textview) *out_textview = textview;
    return scroller;
}

GtkWidget *ui_hairline_new(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(box, -1, 1);
    add_class(box, "hairline");
    return box;
}

gchar *ui_textview_get_text(GtkTextView *view) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

void ui_textview_set_text(GtkTextView *view, const gchar *text) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    gtk_text_buffer_set_text(buffer, text ? text : "", -1);
}
