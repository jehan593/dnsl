#include "remote_controller.h"
#include "app_identity.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

struct RemoteController {
    GMutex mutex;
    gboolean connected;
    int fd;         /* kept only for shutdown() — either fd below works, they share one socket */
    FILE *read_f;   /* owns `fd` */
    FILE *write_f;  /* owns a dup() of `fd` */
    IpcStatus *last_status; /* owned, NULL until the first status arrives */

    GThread *thread;
    volatile gboolean running;

    RemoteStateChangedFn on_state_changed;
    RemoteErrorFn on_error;
    gpointer user_data;
};

typedef struct { RemoteController *rc; gchar *message; } IdleErrorData;

static gboolean idle_state_changed(gpointer data)
{
    RemoteController *rc = data;
    if (rc->on_state_changed) rc->on_state_changed(rc->user_data);
    return G_SOURCE_REMOVE;
}

static void dispatch_state_changed(RemoteController *rc)
{
    g_idle_add(idle_state_changed, rc);
}

static gboolean idle_error(gpointer data)
{
    IdleErrorData *d = data;
    if (d->rc->on_error) d->rc->on_error(d->message, d->rc->user_data);
    g_free(d->message);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void dispatch_error(RemoteController *rc, const gchar *message)
{
    IdleErrorData *d = g_new0(IdleErrorData, 1);
    d->rc = rc;
    d->message = g_strdup(message);
    g_idle_add(idle_error, d);
}

static void mark_disconnected_locked(RemoteController *rc)
{
    if (rc->read_f) fclose(rc->read_f);
    if (rc->write_f) fclose(rc->write_f);
    rc->read_f = NULL;
    rc->write_f = NULL;
    rc->fd = -1;
    rc->connected = FALSE;
    if (rc->last_status) { ipc_status_free(rc->last_status); rc->last_status = NULL; }
}

static gpointer client_loop(gpointer data)
{
    RemoteController *rc = data;

    while (rc->running) {
        g_mutex_lock(&rc->mutex);
        gboolean already_connected = rc->connected;
        g_mutex_unlock(&rc->mutex);

        if (!already_connected) {
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct sockaddr_un addr = { .sun_family = AF_UNIX };
                g_strlcpy(addr.sun_path, DNSL_SOCKET_PATH, sizeof(addr.sun_path));
                if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    /* Separate FILE* per direction, each wrapping its own fd (one dup()'d) —
                     * NOT one shared fdopen(fd, "r+"). glibc serializes every stdio call on a
                     * given FILE* with one internal per-stream lock, held for the whole duration
                     * of a blocking call, not just buffer bookkeeping — this thread's getline()
                     * below sits inside that lock almost permanently (it's blocked waiting for
                     * the next status push). A single shared FILE* meant the GTK main thread's
                     * fprintf() (sending a command) blocked forever on that same lock, which
                     * only getline() could release, which only happens when a new line arrives,
                     * which can't happen until the command is sent — a real deadlock, confirmed
                     * by hand: clicking "Enable" wedged the whole tray, Cinnamon's WM eventually
                     * offered to force-quit it. Two independent FILE*s means two independent
                     * locks, so read and write never contend. */
                    int write_fd = dup(fd);
                    FILE *rf = fdopen(fd, "r");
                    FILE *wf = write_fd >= 0 ? fdopen(write_fd, "w") : NULL;
                    if (rf && wf) {
                        g_mutex_lock(&rc->mutex);
                        rc->fd = fd;
                        rc->read_f = rf;
                        rc->write_f = wf;
                        rc->connected = TRUE;
                        g_mutex_unlock(&rc->mutex);
                        dispatch_state_changed(rc);
                    } else {
                        if (rf) fclose(rf); else close(fd);
                        if (wf) fclose(wf); else if (write_fd >= 0) close(write_fd);
                    }
                } else {
                    close(fd);
                }
            }
        }

        g_mutex_lock(&rc->mutex);
        gboolean connected_now = rc->connected;
        FILE *rf = rc->read_f;
        g_mutex_unlock(&rc->mutex);

        if (connected_now) {
            char *line = NULL;
            size_t cap = 0;
            ssize_t n = getline(&line, &cap, rf); /* blocks for the next status push or EOF */
            if (n >= 0) {
                if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
                IpcStatus *status = ipc_parse_status(line);
                free(line);
                if (status) {
                    g_mutex_lock(&rc->mutex);
                    if (rc->last_status) ipc_status_free(rc->last_status);
                    rc->last_status = status;
                    gchar *error_message = status->error_message ? g_strdup(status->error_message) : NULL;
                    g_mutex_unlock(&rc->mutex);

                    dispatch_state_changed(rc);
                    if (error_message) { dispatch_error(rc, error_message); g_free(error_message); }
                }
                continue; /* loop straight back into the next read, no retry delay needed */
            }
            free(line);
            g_mutex_lock(&rc->mutex);
            mark_disconnected_locked(rc);
            g_mutex_unlock(&rc->mutex);
            dispatch_state_changed(rc);
        }

        /* ~3s retry delay, polled in short slices so remote_controller_stop() (which flips
         * `running`) doesn't have to wait the full delay out before joining this thread. */
        for (int i = 0; i < 30 && rc->running; i++) g_usleep(100 * 1000);
    }
    return NULL;
}

RemoteController *remote_controller_new(void)
{
    RemoteController *rc = g_new0(RemoteController, 1);
    rc->fd = -1;
    g_mutex_init(&rc->mutex);
    return rc;
}

void remote_controller_set_callbacks(RemoteController *rc,
                                      RemoteStateChangedFn on_state_changed,
                                      RemoteErrorFn on_error,
                                      gpointer user_data)
{
    rc->on_state_changed = on_state_changed;
    rc->on_error = on_error;
    rc->user_data = user_data;
}

void remote_controller_start(RemoteController *rc)
{
    rc->running = TRUE;
    rc->thread = g_thread_new("dnsl-ipc-client", client_loop, rc);
}

gboolean remote_controller_is_connected(RemoteController *rc)
{
    g_mutex_lock(&rc->mutex);
    gboolean connected = rc->connected;
    g_mutex_unlock(&rc->mutex);
    return connected;
}

IpcStatus *remote_controller_snapshot(RemoteController *rc)
{
    g_mutex_lock(&rc->mutex);
    IpcStatus *copy = NULL;
    if (rc->last_status) {
        copy = ipc_status_new();
        copy->enabled = rc->last_status->enabled;
        if (rc->last_status->selected_provider_id) copy->selected_provider_id = g_strdup(rc->last_status->selected_provider_id);
        for (guint i = 0; i < rc->last_status->providers->len; i++)
            g_ptr_array_add(copy->providers, dns_provider_copy(g_ptr_array_index(rc->last_status->providers, i)));
    }
    g_mutex_unlock(&rc->mutex);
    return copy;
}

static void send_command(RemoteController *rc, IpcCommand *cmd)
{
    g_mutex_lock(&rc->mutex);
    gboolean connected = rc->connected;
    FILE *wf = rc->write_f;
    g_mutex_unlock(&rc->mutex);

    if (!connected) {
        ipc_command_free(cmd);
        dispatch_error(rc, "dnsl's background service isn't running.");
        return;
    }

    gchar *line = ipc_serialize_command(cmd);
    ipc_command_free(cmd);
    gboolean write_failed = fprintf(wf, "%s\n", line) < 0 || fflush(wf) != 0;
    g_free(line);

    if (write_failed) {
        dispatch_error(rc, "Lost connection to dnsl's background service.");
        /* Don't tear the connection down here — the background thread's next getline() will
         * observe the same broken pipe and run the one true disconnect path. */
    }
}

void remote_controller_enable(RemoteController *rc) { send_command(rc, ipc_command_new(IPC_CMD_ENABLE)); }
void remote_controller_disable(RemoteController *rc) { send_command(rc, ipc_command_new(IPC_CMD_DISABLE)); }

void remote_controller_select_provider(RemoteController *rc, const gchar *provider_id)
{
    IpcCommand *cmd = ipc_command_new(IPC_CMD_SELECT_PROVIDER);
    cmd->provider_id = g_strdup(provider_id);
    send_command(rc, cmd);
}

void remote_controller_add_custom_provider(RemoteController *rc, DnsProvider *provider)
{
    IpcCommand *cmd = ipc_command_new(IPC_CMD_ADD_CUSTOM_PROVIDER);
    cmd->provider = provider;
    send_command(rc, cmd);
}

void remote_controller_remove_custom_provider(RemoteController *rc, const gchar *provider_id)
{
    IpcCommand *cmd = ipc_command_new(IPC_CMD_REMOVE_CUSTOM_PROVIDER);
    cmd->provider_id = g_strdup(provider_id);
    send_command(rc, cmd);
}

void remote_controller_free(RemoteController *rc)
{
    if (!rc) return;
    rc->running = FALSE;

    g_mutex_lock(&rc->mutex);
    if (rc->connected && rc->fd >= 0) shutdown(rc->fd, SHUT_RDWR);
    g_mutex_unlock(&rc->mutex);

    if (rc->thread) { g_thread_join(rc->thread); rc->thread = NULL; }

    g_mutex_lock(&rc->mutex);
    mark_disconnected_locked(rc);
    g_mutex_unlock(&rc->mutex);

    g_mutex_clear(&rc->mutex);
    g_free(rc);
}
