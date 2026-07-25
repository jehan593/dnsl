/* Locates the bundled data/ directory (icons/fonts) relative to the running executable, or the
 * installed share dir — same resolution order as linker-linux's app_context.c. */
#ifndef DNSL_ASSETS_H
#define DNSL_ASSETS_H

#include <glib.h>

/* NULL if not found. Free with g_free. */
gchar *dnsl_find_asset_dir(void);

void dnsl_load_bundled_fonts(const gchar *asset_dir);

#endif
