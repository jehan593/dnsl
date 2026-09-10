/* One binary, three roles: --daemon, --install-service/--start-service, or tray (no args). */
#include <gtk/gtk.h>
#include <string.h>

#include "app_identity.h"
#include "assets.h"
#include "theme.h"
#include "daemon.h"
#include "installer.h"
#include "remote_controller.h"
#include "tray.h"

typedef struct {
    gboolean autostart;
    RemoteController *remote;
    TrayController *tc;
} AppContext;

static void activate(GtkApplication *app, gpointer user_data)
{
    AppContext *ctx = user_data;

    if (ctx->tc) {
        /* A second launch re-activates the first instance. */
        tray_controller_show_providers_window(ctx->tc);
        return;
    }

    gchar *asset_dir = dnsl_find_asset_dir();
    dnsl_load_bundled_fonts(asset_dir);
    dnsl_theme_init();

    ctx->remote = remote_controller_new();
    remote_controller_start(ctx->remote);
    ctx->tc = tray_controller_new(app, ctx->remote, asset_dir);
    g_free(asset_dir);

    if (!ctx->autostart) tray_controller_show_providers_window(ctx->tc);

    /* Tray-only app — hold open so it doesn't quit when the untracked window closes. */
    g_application_hold(G_APPLICATION(app));
}

static gboolean has_arg(int argc, char **argv, const gchar *flag)
{
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], flag) == 0) return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    if (has_arg(argc, argv, "--daemon")) return daemon_run();

    if (has_arg(argc, argv, "--install-service") || has_arg(argc, argv, "--start-service")) {
        return installer_run_elevated_helper_entry_point(argc, argv);
    }

    AppContext ctx = { 0 };
    ctx.autostart = has_arg(argc, argv, DNSL_AUTOSTART_ARG);

    GtkApplication *app = gtk_application_new(DNSL_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &ctx);

    /* Run with just argv[0] — flags already parsed above. */
    int status = g_application_run(G_APPLICATION(app), 1, argv);

    if (ctx.tc) tray_controller_free(ctx.tc);
    if (ctx.remote) remote_controller_free(ctx.remote);
    g_object_unref(app);
    return status;
}
