/* Small widget factories applying the Nord control classes generated in theme.c —
 * kept in one place so every dialog/view builds visually-identical controls instead
 * of re-deriving padding/class names ad hoc. */
#ifndef DNSL_UI_WIDGETS_H
#define DNSL_UI_WIDGETS_H

#include <gtk/gtk.h>

/* FilledButtonTheme equivalent: pill shape, Primary bg. */
GtkWidget *ui_pill_button_new(const gchar *label_text);

/* TextButtonTheme equivalent. color_class is one of "text-button-primary",
 * "text-button-neutral", "text-button-error" (see theme.c). */
GtkWidget *ui_text_button_new(const gchar *label_text, const gchar *color_class);

/* IconButtonTheme equivalent: circular, transparent, hover fill. `small` selects the
 * 24x24 variant used for the browser list's move-up/move-down buttons. */
GtkWidget *ui_icon_button_new(const gchar *icon_name, const gchar *tooltip, gboolean small);

GtkWidget *ui_title_label_new(const gchar *text);   /* .dnsl-title */
GtkWidget *ui_label_label_new(const gchar *text);   /* .dnsl-label */
GtkWidget *ui_body_small_label_new(const gchar *text); /* .dnsl-body-small */

/* TextBox.outlined equivalent single-line entry. */
GtkWidget *ui_outlined_entry_new(void);

/* TextBox.outlined equivalent multiline text view, wrapped in its own scroller.
 * *out_textview receives the inner GtkTextView for get/set text access. */
GtkWidget *ui_outlined_textview_new(GtkWidget **out_textview);

/* 1px OutlineVariant divider. */
GtkWidget *ui_hairline_new(void);

/* Convenience: get/set plain text on a GtkTextView's buffer. Free the result with g_free. */
gchar *ui_textview_get_text(GtkTextView *view);
void ui_textview_set_text(GtkTextView *view, const gchar *text);

#endif
