#include "ipc_server.h"
#include "ipc_protocol.h"
#include "app_identity.h"

#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int fd;         /* kept only for shutdown() — either fd below works, they share one socket */
    FILE *read_f;   /* owns `fd`; only ever touched by this client's own client_thread */
    FILE *write_f;  /* owns a dup() of `fd`; touched by this client's own thread AND by any
                     * thread broadcasting a state change — write_mutex serializes those */
    GMutex write_mutex;
} ClientConn;

struct IpcServer {
    ProtectionController *controller;
    int listen_fd;
    GThread *accept_thread;
    volatile gboolean running;

    GMutex clients_mutex;
    GPtrArray *clients; /* of ClientConn* */
};

typedef struct {
    IpcServer *server;
    ClientConn *client;
} ClientThreadArgs;

static IpcStatus *build_status(IpcServer *server, const gchar *error_message)
{
    IpcStatus *status = ipc_status_new();
    status->enabled = protection_controller_is_enabled(server->controller);
    const gchar *selected = protection_controller_selected_provider_id(server->controller);
    if (selected) status->selected_provider_id = g_strdup(selected);

    GPtrArray *all = protection_controller_all_providers(server->controller);
    for (guint i = 0; i < all->len; i++)
        g_ptr_array_add(status->providers, dns_provider_copy(g_ptr_array_index(all, i)));
    g_ptr_array_free(all, TRUE);

    if (error_message) status->error_message = g_strdup(error_message);
    return status;
}

static void send_status(IpcServer *server, ClientConn *client, const gchar *error_message)
{
    IpcStatus *status = build_status(server, error_message);
    gchar *line = ipc_serialize_status(status);
    ipc_status_free(status);

    g_mutex_lock(&client->write_mutex);
    if (fprintf(client->write_f, "%s\n", line) >= 0) fflush(client->write_f);
    g_mutex_unlock(&client->write_mutex);

    g_free(line);
}

static void broadcast(IpcServer *server, const gchar *error_message)
{
    g_mutex_lock(&server->clients_mutex);
    for (guint i = 0; i < server->clients->len; i++) {
        send_status(server, g_ptr_array_index(server->clients, i), error_message);
    }
    g_mutex_unlock(&server->clients_mutex);
}

static void on_state_changed(gpointer user_data) { broadcast((IpcServer *)user_data, NULL); }
static void on_error(const gchar *message, gpointer user_data) { broadcast((IpcServer *)user_data, message); }

static const DnsProvider *find_provider(IpcServer *server, const gchar *id, GPtrArray **out_all)
{
    GPtrArray *all = protection_controller_all_providers(server->controller);
    *out_all = all;
    if (!id) return NULL;
    for (guint i = 0; i < all->len; i++) {
        DnsProvider *p = g_ptr_array_index(all, i);
        if (g_strcmp0(p->id, id) == 0) return p;
    }
    return NULL;
}

static void handle_command(IpcServer *server, IpcCommand *cmd)
{
    GPtrArray *all = NULL;
    switch (cmd->type) {
        case IPC_CMD_ENABLE:
            protection_controller_enable(server->controller);
            break;
        case IPC_CMD_DISABLE:
            protection_controller_disable(server->controller);
            break;
        case IPC_CMD_SELECT_PROVIDER: {
            const DnsProvider *p = find_provider(server, cmd->provider_id, &all);
            if (p) protection_controller_select_provider(server->controller, p);
            g_ptr_array_free(all, TRUE);
            break;
        }
        case IPC_CMD_ADD_CUSTOM_PROVIDER:
            if (cmd->provider) {
                DnsProvider *owned = cmd->provider;
                cmd->provider = NULL; /* detach — add_custom_provider takes ownership */
                protection_controller_add_custom_provider(server->controller, owned);
            }
            break;
        case IPC_CMD_REMOVE_CUSTOM_PROVIDER:
            if (cmd->provider_id) protection_controller_remove_custom_provider(server->controller, cmd->provider_id);
            break;
        case IPC_CMD_GET_STATUS:
            break; /* status is sent unconditionally by the caller regardless of command type */
    }
}

static gpointer client_thread(gpointer data)
{
    ClientThreadArgs *args = data;
    IpcServer *server = args->server;
    ClientConn *client = args->client;
    g_free(args);

    g_mutex_lock(&server->clients_mutex);
    g_ptr_array_add(server->clients, client);
    guint count = server->clients->len;
    g_mutex_unlock(&server->clients_mutex);
    /* First client to connect (tray launching, by any means) re-applies protection if that's what
     * the user last chose — see protection_controller_resume_if_desired. */
    if (count == 1) protection_controller_resume_if_desired(server->controller);

    send_status(server, client, NULL); /* greet with current status */

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, client->read_f)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        IpcCommand *cmd = ipc_parse_command(line);
        if (cmd) {
            handle_command(server, cmd);
            ipc_command_free(cmd);
        }
        send_status(server, client, NULL);
    }
    free(line);

    g_mutex_lock(&server->clients_mutex);
    g_ptr_array_remove_fast(server->clients, client);
    guint remaining = server->clients->len;
    g_mutex_unlock(&server->clients_mutex);
    /* Last client disconnecting (tray closed, crashed, or a log-off) pauses live protection
     * without forgetting the preference — the counterpart to the resume above. */
    if (remaining == 0) protection_controller_pause(server->controller);

    fclose(client->read_f);
    fclose(client->write_f);
    g_mutex_clear(&client->write_mutex);
    g_free(client);
    return NULL;
}

static gpointer accept_loop(gpointer data)
{
    IpcServer *server = data;
    while (server->running) {
        int fd = accept(server->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return NULL; /* listen socket closed by ipc_server_stop, or a real error */
        }

        /* Separate FILE* per direction — see the ClientConn doc comment and remote_controller.c's
         * matching fix for why a single shared fdopen(fd, "r+") deadlocks: this thread's own
         * getline() below sits inside glibc's per-stream lock for the whole time it's blocked
         * waiting for the client's next command, so any *other* thread's fprintf() to a shared
         * FILE* (e.g. a broadcast triggered by a different client's command) would block on that
         * same lock forever. */
        int write_fd = dup(fd);
        FILE *rf = fdopen(fd, "r");
        FILE *wf = write_fd >= 0 ? fdopen(write_fd, "w") : NULL;
        if (!rf || !wf) {
            if (rf) fclose(rf); else close(fd);
            if (wf) fclose(wf); else if (write_fd >= 0) close(write_fd);
            continue;
        }

        ClientConn *client = g_new0(ClientConn, 1);
        client->fd = fd;
        client->read_f = rf;
        client->write_f = wf;
        g_mutex_init(&client->write_mutex);

        ClientThreadArgs *args = g_new0(ClientThreadArgs, 1);
        args->server = server;
        args->client = client;
        GThread *t = g_thread_new("dnsl-ipc-client", client_thread, args);
        g_thread_unref(t);
    }
    return NULL;
}

IpcServer *ipc_server_new(ProtectionController *controller)
{
    IpcServer *server = g_new0(IpcServer, 1);
    server->controller = controller;
    server->listen_fd = -1;
    g_mutex_init(&server->clients_mutex);
    server->clients = g_ptr_array_new();
    return server;
}

gboolean ipc_server_start(IpcServer *server, GError **error)
{
    if (g_mkdir_with_parents(DNSL_RUNTIME_DIR, 0755) != 0 && errno != EEXIST) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Couldn't create %s: %s", DNSL_RUNTIME_DIR, g_strerror(errno));
        return FALSE;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "socket() failed: %s", g_strerror(errno));
        return FALSE;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    g_strlcpy(addr.sun_path, DNSL_SOCKET_PATH, sizeof(addr.sun_path));
    unlink(DNSL_SOCKET_PATH); /* stale socket from a previous run that didn't shut down cleanly */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "bind(%s) failed: %s", DNSL_SOCKET_PATH, g_strerror(errno));
        close(fd);
        return FALSE;
    }
    /* World-connectable: this isn't a real privilege boundary (any local user redirecting their
     * own already-root-controlled machine's DNS isn't an escalation), so no group/ACL dance. */
    chmod(DNSL_SOCKET_PATH, 0666);

    if (listen(fd, 16) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "listen() failed: %s", g_strerror(errno));
        close(fd);
        return FALSE;
    }

    server->listen_fd = fd;
    server->running = TRUE;
    protection_controller_set_callbacks(server->controller, on_state_changed, on_error, server);
    server->accept_thread = g_thread_new("dnsl-ipc-accept", accept_loop, server);
    return TRUE;
}

void ipc_server_stop(IpcServer *server)
{
    if (!server->running) return;
    server->running = FALSE;

    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
    server->listen_fd = -1;
    if (server->accept_thread) { g_thread_join(server->accept_thread); server->accept_thread = NULL; }

    /* Best-effort: nudge every still-connected client's blocking getline() to return so its
     * thread notices and cleans itself up promptly (the process is about to exit regardless, so
     * these aren't joined — see daemon.c's shutdown sequence). */
    g_mutex_lock(&server->clients_mutex);
    for (guint i = 0; i < server->clients->len; i++) {
        ClientConn *client = g_ptr_array_index(server->clients, i);
        shutdown(client->fd, SHUT_RDWR);
    }
    g_mutex_unlock(&server->clients_mutex);

    unlink(DNSL_SOCKET_PATH);
}

void ipc_server_free(IpcServer *server)
{
    if (!server) return;
    ipc_server_stop(server);
    g_ptr_array_free(server->clients, TRUE);
    g_mutex_clear(&server->clients_mutex);
    g_free(server);
}
