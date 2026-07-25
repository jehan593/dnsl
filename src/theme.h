/* Nord dark/light palette (hex-for-hex port of the Windows app's Colors.axaml /
 * NordColors.cs) plus the GTK3 CSS stylesheet generated from it, and OS dark/light
 * detection (xdg-desktop-portal, with a GSettings fallback) with live watching so
 * flipping the desktop theme updates the app without a restart — mirrors the Windows
 * app following AppsUseLightTheme via SystemEvents.UserPreferenceChanged. */
#ifndef DNSL_THEME_H
#define DNSL_THEME_H

#include <gtk/gtk.h>

typedef struct {
    const char *primary, *on_primary, *primary_container, *on_primary_container;
    const char *secondary, *on_secondary, *secondary_container, *on_secondary_container;
    const char *tertiary, *on_tertiary, *tertiary_container, *on_tertiary_container;
    const char *background, *on_background;
    const char *surface, *on_surface, *surface_variant, *on_surface_variant;
    const char *surface_dim, *surface_bright;
    const char *surface_container_lowest, *surface_container_low, *surface_container;
    const char *surface_container_high, *surface_container_highest;
    const char *error, *on_error, *error_container, *on_error_container;
    const char *outline, *outline_variant;
    const char *inverse_surface, *inverse_on_surface, *inverse_primary;
    const char *row_hover;
} NordPalette;

/* Fixed, non-theme-swapped search-highlight colors (Nord13 bg / Nord0 fg). */
#define DNSL_HIGHLIGHT_BG "#EBCB8B"
#define DNSL_HIGHLIGHT_FG "#2E3440"

/* Sets up the shared GtkCssProvider on the default screen, detects the initial OS
 * dark/light preference, and starts watching for live changes. Call once at startup
 * after gtk_init. */
void dnsl_theme_init(void);

gboolean dnsl_theme_is_dark(void);

const NordPalette *dnsl_theme_palette(void);

#endif
