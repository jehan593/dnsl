#include "installer.h"
#include "app_identity.h"
#include <glib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <string.h>

static gboolean run_command(const gchar *const *argv, gint *exit_status, gchar **stdout_str, GError **error)
{
    return g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                         stdout_str, NULL, exit_status, error);
}

static gboolean is_unit_active(void)
{
    const gchar *argv[] = { "systemctl", "is-active", DNSL_SERVICE_UNIT, NULL };
    gint status = -1;
    gchar *out = NULL;
    GError *error = NULL;
    if (!run_command(argv, &status, &out, &error)) { g_clear_error(&error); return FALSE; }
    gboolean active = out && g_str_has_prefix(g_strstrip(out), "active");
    g_free(out);
    return active;
}

static gchar *get_own_exe_path(void)
{
    return g_file_read_link("/proc/self/exe", NULL);
}

InstallerResult installer_ensure_installed_and_running(void)
{
    if (is_unit_active()) return INSTALLER_ALREADY_RUNNING;

    if (!g_find_program_in_path("pkexec")) return INSTALLER_FAILED;

    gchar *exe_path = get_own_exe_path();
    if (!exe_path) return INSTALLER_FAILED;

    const gchar *arg = g_file_test(DNSL_SYSTEMD_UNIT_PATH, G_FILE_TEST_EXISTS) ? "--start-service" : "--install-service";
    const gchar *argv[] = { "pkexec", exe_path, arg, NULL };

    gint status = -1;
    GError *error = NULL;
    gboolean spawned = run_command(argv, &status, NULL, &error);
    g_free(exe_path);
    if (error) g_clear_error(&error);

    if (!spawned) return INSTALLER_FAILED;
    /* pkexec's own documented exit codes: 126 = the user dismissed/denied the auth dialog,
     * 127 = the target program couldn't even be executed. */
    if (WIFEXITED(status) && WEXITSTATUS(status) == 126) return INSTALLER_CANCELLED;
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) return INSTALLER_FAILED;

    /* The elevated helper's own "systemctl start" call already waits for a running/failed
     * result, so this is just giving systemd a moment to reflect that back to us here. */
    for (int i = 0; i < 10; i++) {
        if (is_unit_active()) return INSTALLER_STARTED;
        g_usleep(300 * 1000);
    }
    return INSTALLER_FAILED;
}

static gboolean write_unit_file(const gchar *exe_path, GError **error)
{
    gchar *contents = g_strdup_printf(
        "[Unit]\n"
        "Description=dnsl DNS-over-TLS proxy\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=%s --daemon\n"
        "ExecStopPost=%s --cleanup-network\n"
        "Restart=on-failure\n"
        "RestartSec=5\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        exe_path, exe_path);

    gboolean ok = g_file_set_contents(DNSL_SYSTEMD_UNIT_PATH, contents, -1, error);
    g_free(contents);
    return ok;
}

/* The user who asked for the install: pkexec sets PKEXEC_UID, install.sh runs under sudo. */
static gchar *invoking_username(void)
{
    const gchar *uid_text = g_getenv("PKEXEC_UID");
    if (uid_text) {
        struct passwd *pw = getpwuid((uid_t)g_ascii_strtoull(uid_text, NULL, 10));
        return pw ? g_strdup(pw->pw_name) : NULL;
    }
    const gchar *sudo_user = g_getenv("SUDO_USER");
    return sudo_user ? g_strdup(sudo_user) : NULL;
}

/* Creates the control group and adds the installing user. Membership applies to new login
 * sessions only, so a tray that's already running must be restarted after the first install. */
static gboolean ensure_control_group(void)
{
    gint status = -1;
    GError *error = NULL;
    const gchar *group_argv[] = { "groupadd", "-f", DNSL_GROUP, NULL };
    if (!run_command(group_argv, &status, NULL, &error) || status != 0) { g_clear_error(&error); return FALSE; }

    gchar *user = invoking_username();
    gboolean ok = TRUE;
    if (user && *user) {
        const gchar *usermod_argv[] = { "usermod", "-aG", DNSL_GROUP, user, NULL };
        ok = run_command(usermod_argv, &status, NULL, &error) && status == 0;
        g_clear_error(&error);
    }
    g_free(user);
    return ok;
}

static gboolean install_or_update(void)
{
    if (!ensure_control_group()) return FALSE;

    gchar *exe_path = get_own_exe_path();
    if (!exe_path) return FALSE;

    GError *error = NULL;
    gboolean ok = write_unit_file(exe_path, &error);
    g_free(exe_path);
    if (!ok) { g_clear_error(&error); return FALSE; }

    gint status;
    const gchar *reload_argv[] = { "systemctl", "daemon-reload", NULL };
    if (!run_command(reload_argv, &status, NULL, &error) || status != 0) { g_clear_error(&error); return FALSE; }

    const gchar *enable_argv[] = { "systemctl", "enable", DNSL_SERVICE_UNIT, NULL };
    if (!run_command(enable_argv, &status, NULL, &error) || status != 0) { g_clear_error(&error); return FALSE; }

    return TRUE;
}

static gboolean start_unit(void)
{
    gint status;
    GError *error = NULL;
    const gchar *argv[] = { "systemctl", "start", DNSL_SERVICE_UNIT, NULL };
    gboolean ok = run_command(argv, &status, NULL, &error) && status == 0;
    if (error) g_clear_error(&error);
    return ok;
}

int installer_run_elevated_helper_entry_point(int argc, char **argv)
{
    gboolean install = FALSE, start = FALSE, registration = FALSE;
    for (int i = 0; i < argc; i++) {
        if (g_strcmp0(argv[i], "--install-service") == 0) install = TRUE;
        if (g_strcmp0(argv[i], "--start-service") == 0) start = TRUE;
        if (g_strcmp0(argv[i], "--register-service") == 0) registration = TRUE;
    }

    if (registration) return install_or_update() ? 0 : 1;
    if (install || start) {
        if (!install_or_update()) return 1;
        return start_unit() ? 0 : 1;
    }
    return 1;
}
