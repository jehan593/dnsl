/* Real controller + IPC, isolated settings/socket paths and a fake DoT upstream. */
#include "app_identity.h"
#include "protection_controller.h"
#include <signal.h>
#include <stdio.h>
static gchar *test_dir, *settings_path, *socket_path;
#undef DNSL_RUNTIME_DIR
#undef DNSL_SOCKET_PATH
#undef DNSL_SETTINGS_DIR
#undef DNSL_SETTINGS_PATH
#define DNSL_RUNTIME_DIR test_dir
#define DNSL_SOCKET_PATH socket_path
#define DNSL_SETTINGS_DIR test_dir
#define DNSL_SETTINGS_PATH settings_path
#include "../src/settings_store.c"
#include "../src/ipc_server.c"

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_dir = g_dir_make_tmp("dnsl-lifecycle-XXXXXX", NULL);
    settings_path = g_build_filename(test_dir, "settings.json", NULL);
    socket_path = g_build_filename(test_dir, "control.sock", NULL);
    AppSettings *settings = app_settings_new_default();
    settings->enabled = TRUE;
    g_assert_true(settings_store_save(settings, NULL));
    app_settings_free(settings);
    ProtectionController *pc = protection_controller_new();
    IpcServer *server = ipc_server_new(pc);
    GError *error = NULL;
    if (!ipc_server_start(server, &error)) g_error("%s", error->message);
    puts(socket_path);
    fflush(stdout);
    char line[32];
    while (fgets(line, sizeof(line), stdin)) {
        if (g_str_has_prefix(line, "quit")) break;
        printf("%d\n", protection_controller_is_enabled(pc));
        fflush(stdout);
    }
    ipc_server_free(server);
    protection_controller_free(pc);
    g_unlink(settings_path);
    g_rmdir(test_dir);
    g_free(settings_path);
    g_free(socket_path);
    g_free(test_dir);
    return 0;
}
