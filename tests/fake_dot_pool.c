#include "dot_pool.h"
#include <glib.h>
struct DotPool { gint refs; guint8 marker; };
DotPool *dot_pool_new(const DnsProvider *provider)
{
    DotPool *p = g_new0(DotPool, 1); p->refs = 1; p->marker = provider->port == 854 ? 0x42 : 0x41; return p;
}
void dot_pool_cancel(DotPool *p) { (void)p; }
DotPool *dot_pool_ref(DotPool *p) { g_atomic_int_inc(&p->refs); return p; }
void dot_pool_unref(DotPool *p) { if (p && g_atomic_int_dec_and_test(&p->refs)) g_free(p); }
gboolean dot_pool_forward(DotPool *p, const guint8 *q, gsize n, guint8 **r, gsize *rn, GError **e)
{
    (void)e;
    if (q[0] == 0xfe) g_usleep(300000); /* old in-flight response across stop/restart */
    *r = g_memdup2(q, n); *rn = n; (*r)[2] |= 0x80; (*r)[3] = p->marker; return TRUE;
}
