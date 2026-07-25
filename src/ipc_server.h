/* Port of dnsw's Ipc/IpcServer.cs: daemon-side half of the IPC link over DNSL_SOCKET_PATH. Accepts
 * connections in a loop (so the tray can disconnect/reconnect freely) and applies every received
 * IpcCommand to the shared ProtectionController. First client connecting resumes protection if it
 * was last left on; last client disconnecting pauses it — see CLAUDE.md "Protection tracks the
 * app, not the daemon". */
#ifndef DNSL_IPC_SERVER_H
#define DNSL_IPC_SERVER_H

#include "protection_controller.h"

typedef struct IpcServer IpcServer;

IpcServer *ipc_server_new(ProtectionController *controller);
gboolean ipc_server_start(IpcServer *server, GError **error);
void ipc_server_stop(IpcServer *server);
void ipc_server_free(IpcServer *server);

#endif
