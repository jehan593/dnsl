#include "autostart.h"
#include "app_identity.h"
#include <glib/gstdio.h>
#include <stdio.h>

static gchar *desktop_entry_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart", DNSL_DESKTOP_ID, NULL);
}

gboolean autostart_is_enabled(void)
{
    gchar *path = desktop_entry_path();
    gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return exists;
}

void autostart_set_enabled(gboolean enabled)
{
    gchar *path = desktop_entry_path();

    if (!enabled) {
        g_unlink(path);
        g_free(path);
        return;
    }

    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    gchar *exe_path = g_file_read_link("/proc/self/exe", NULL);
    if (!exe_path) exe_path = g_strdup("dnsl");

    gchar *contents = g_strdup_printf(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=dnsl\n"
        "Comment=DNS-over-TLS tray\n"
        "Exec=%s %s\n"
        "NoDisplay=true\n"
        "X-GNOME-Autostart-enabled=true\n",
        exe_path, DNSL_AUTOSTART_ARG);
    g_free(exe_path);

    g_file_set_contents(path, contents, -1, NULL);
    g_free(contents);
    g_free(path);
}
