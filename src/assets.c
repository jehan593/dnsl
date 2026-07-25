#include "assets.h"
#include <fontconfig/fontconfig.h>

gchar *dnsl_find_asset_dir(void)
{
    const gchar *candidates_relative_to_exe[] = { "data", "../data", "../share/dnsl", NULL };

    gchar *exe_path = g_file_read_link("/proc/self/exe", NULL);
    if (exe_path) {
        gchar *exe_dir = g_path_get_dirname(exe_path);
        g_free(exe_path);
        for (int i = 0; candidates_relative_to_exe[i]; i++) {
            gchar *candidate = g_build_filename(exe_dir, candidates_relative_to_exe[i], NULL);
            if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
                g_free(exe_dir);
                return candidate;
            }
            g_free(candidate);
        }
        g_free(exe_dir);
    }

    gchar *installed = g_build_filename(g_get_user_data_dir(), "dnsl", NULL);
    if (g_file_test(installed, G_FILE_TEST_IS_DIR)) return installed;
    g_free(installed);
    return NULL;
}

void dnsl_load_bundled_fonts(const gchar *asset_dir)
{
    if (!asset_dir) return;
    const char *files[] = { "martian_mono_regular.ttf", "martian_mono_medium.ttf", "martian_mono_bold.ttf" };
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++) {
        gchar *path = g_build_filename(asset_dir, "fonts", files[i], NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            if (!FcConfigAppFontAddFile(FcConfigGetCurrent(), (const FcChar8 *)path)) {
                g_warning("failed to register bundled font: %s", path);
            }
        }
        g_free(path);
    }
}
