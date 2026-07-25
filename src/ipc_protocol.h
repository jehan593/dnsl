/* Port of dnsw's Ipc/IpcContract.cs: line-delimited JSON over DNSL_SOCKET_PATH between the
 * unprivileged tray process and the privileged daemon. No request/response correlation ids — the
 * tray sends at most one command at a time and the daemon always answers with a fresh IpcStatus
 * right after handling it, plus pushes an unprompted IpcStatus to every connected client whenever
 * state changes for any other reason (e.g. resuming protection on daemon start) — a single
 * "latest status wins" stream is all a one-tray-per-desktop app needs. */
#ifndef DNSL_IPC_PROTOCOL_H
#define DNSL_IPC_PROTOCOL_H

#include "dns_provider.h"

typedef enum {
    IPC_CMD_GET_STATUS,
    IPC_CMD_ENABLE,
    IPC_CMD_DISABLE,
    IPC_CMD_SELECT_PROVIDER,
    IPC_CMD_ADD_CUSTOM_PROVIDER,
    IPC_CMD_REMOVE_CUSTOM_PROVIDER,
} IpcCommandType;

typedef struct {
    IpcCommandType type;
    gchar *provider_id;     /* owned, NULL unless SelectProvider/RemoveCustomProvider */
    DnsProvider *provider;  /* owned, NULL unless AddCustomProvider */
} IpcCommand;

typedef struct {
    gboolean enabled;
    gchar *selected_provider_id; /* owned, may be NULL */
    GPtrArray *providers;        /* owned, of DnsProvider* */
    gchar *error_message;        /* owned, may be NULL — set only on the reply to whichever
                                   * command just failed, never sticky/resent unprompted */
} IpcStatus;

IpcCommand *ipc_command_new(IpcCommandType type);
void ipc_command_free(IpcCommand *cmd);

IpcStatus *ipc_status_new(void);
void ipc_status_free(IpcStatus *status);

/* Single-line JSON, no trailing newline — caller appends '\n' when writing to the socket. */
gchar *ipc_serialize_command(const IpcCommand *cmd);
gchar *ipc_serialize_status(const IpcStatus *status);

/* NULL on malformed JSON/missing required fields. */
IpcCommand *ipc_parse_command(const gchar *line);
IpcStatus *ipc_parse_status(const gchar *line);

#endif
