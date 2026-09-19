#include "dot_pool.h"
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
static gpointer blocked_query(gpointer data)
{
    guint8 query[12] = { 0xfe, 1, 1 };
    guint8 *response = NULL;
    gsize n = 0;
    GError *error = NULL;
    g_assert_false(dot_pool_forward(data, query, sizeof(query), &response, &n, &error));
    g_clear_error(&error);
    g_free(response);
    return NULL;
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 2);
    signal(SIGPIPE, SIG_IGN);
    const gchar *ips[] = { "127.0.0.1" };
    DnsProvider *provider = dns_provider_new("test", "test", "localhost", ips, 1, atoi(argv[1]), TRUE);
    DotPool *pool = dot_pool_new(provider);
    guint8 query[12] = { 0x12, 0x34, 1 };
    guint8 *response = NULL;
    gsize length = 0;
    GError *error = NULL;
    g_assert_true(dot_pool_forward(pool, query, sizeof(query), &response, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(length, ==, sizeof(query));
    g_assert_cmpuint(response[2], ==, 0x81);
    g_free(response);
    GThread *worker = g_thread_new("blocked-query", blocked_query, pool);
    /* Test driver sends this only after the TLS peer received the second query. */
    g_assert_cmpint(getchar(), ==, 'c');
    gint64 start = g_get_monotonic_time();
    dot_pool_cancel(pool);
    g_thread_join(worker);
    g_assert_cmpint(g_get_monotonic_time() - start, <, G_TIME_SPAN_SECOND);
    dot_pool_unref(pool);
    dns_provider_free(provider);
    return 0;
}
