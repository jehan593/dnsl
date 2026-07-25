/* Port of dnsw's Ui/AddCustomProviderDialog.axaml(.cs) + Ui/AddNextDnsProviderDialog.axaml(.cs):
 * two small modal dialogs that hand back a freshly built custom DnsProvider* (owned by the
 * caller), or NULL if cancelled. */
#ifndef DNSL_ADD_PROVIDER_DIALOG_H
#define DNSL_ADD_PROVIDER_DIALOG_H

#include <gtk/gtk.h>
#include "dns_provider.h"

DnsProvider *add_provider_dialog_run_custom(GtkWindow *parent);
DnsProvider *add_provider_dialog_run_nextdns(GtkWindow *parent);

#endif
