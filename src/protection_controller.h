/* Port of dnsw's Service/LocalProtectionController.cs: runs inside the privileged daemon and does
 * the actual work — turning "protection" on/off means starting/stopping the local DoT proxy AND
 * redirecting/restoring the active links' DNS together, since either alone is useless. ipc_server
 * is the only thing that drives this directly — the tray talks to it exclusively over IPC. */
#ifndef DNSL_PROTECTION_CONTROLLER_H
#define DNSL_PROTECTION_CONTROLLER_H

#include "dns_provider.h"

typedef struct ProtectionController ProtectionController;

typedef void (*ProtectionStateChangedFn)(gpointer user_data);
typedef void (*ProtectionErrorFn)(const gchar *message, gpointer user_data);

ProtectionController *protection_controller_new(void);
void protection_controller_free(ProtectionController *pc);

/* Exactly one subscriber (ipc_server) — mirrors the single StateChanged/ErrorOccurred event pair
 * on LocalProtectionController.cs, called with the mutex NOT held so the callback can safely call
 * back into this controller (e.g. to build a status snapshot) without deadlocking. */
void protection_controller_set_callbacks(ProtectionController *pc,
                                          ProtectionStateChangedFn on_state_changed,
                                          ProtectionErrorFn on_error,
                                          gpointer user_data);

gboolean protection_controller_is_enabled(ProtectionController *pc);
const gchar *protection_controller_selected_provider_id(ProtectionController *pc);
/* Newly allocated array of borrowed DnsProvider* — free the array (not its elements) with
 * g_ptr_array_free(result, TRUE); element_free_func is NULL. */
GPtrArray *protection_controller_all_providers(ProtectionController *pc);

/* Explicit user action (tray "Enable protection"): starts live protection and remembers the
 * choice for next launch. */
void protection_controller_enable(ProtectionController *pc);
/* Explicit user action (tray "Disable protection"): stops live protection and remembers it. */
void protection_controller_disable(ProtectionController *pc);
/* Called when the last IPC client disconnects: stops live protection like disable, but leaves
 * the persisted preference alone — see CLAUDE.md "Protection tracks the app, not the daemon". */
void protection_controller_pause(ProtectionController *pc);
/* Called when the first IPC client connects: re-applies the last explicit choice if it was on. */
void protection_controller_resume_if_desired(ProtectionController *pc);

void protection_controller_select_provider(ProtectionController *pc, const DnsProvider *provider);
/* Takes ownership of `provider`. */
void protection_controller_add_custom_provider(ProtectionController *pc, DnsProvider *provider);
void protection_controller_remove_custom_provider(ProtectionController *pc, const gchar *provider_id);

#endif
