/* Local fake upstream: exercise transport and interception without Internet access. */
#include "dns_proxy.h"
#include "dot_pool.h"
#include "intercept_ctl.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    DnsProvider provider = { .ip_count = 1, .port = 853 };
    DnsProxy *proxy = dns_proxy_new();
    GError *error = NULL;
    char command[64];
    while (fgets(command, sizeof(command), stdin)) {
        gboolean ok = TRUE;
        if (g_str_has_prefix(command, "start")) ok = dns_proxy_start(proxy, &provider, DNSL_PROXY_PORT, &error);
        else if (g_str_has_prefix(command, "stop")) dns_proxy_stop(proxy);
        else if (g_str_has_prefix(command, "switch")) { provider.port = 854; dns_proxy_switch_provider(proxy, &provider); }
        else if (g_str_has_prefix(command, "enable")) ok = intercept_ctl_enable(&error);
        else if (g_str_has_prefix(command, "disable")) ok = intercept_ctl_disable(&error);
        else if (g_str_has_prefix(command, "quit")) { puts("OK"); fflush(stdout); break; }
        else ok = FALSE;
        if (!ok) { printf("ERROR %s\n", error ? error->message : "unknown command"); g_clear_error(&error); }
        else puts("OK");
        fflush(stdout);
    }
    dns_proxy_free(proxy);
    return 0;
}
