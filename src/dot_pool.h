/* Port of dnsw's Dns/DotConnectionPool.cs: a pool of warm TLS connections to one DnsProvider,
 * speaking RFC 7858 DNS-over-TLS (2-byte big-endian length prefix + raw DNS message, the same
 * framing DNS-over-TCP uses). One query occupies one connection for its full round trip (no
 * ID-multiplexing) — simpler and can't ever mismatch a response to the wrong in-flight query, at
 * the cost of needing MAX_CONNECTIONS connections instead of one for that much concurrency. */
#ifndef DNSL_DOT_POOL_H
#define DNSL_DOT_POOL_H

#include "dns_provider.h"

typedef struct DotPool DotPool;

/* Returned with refcount 1 — see dot_pool_ref/unref. Concurrent query-handler threads each take
 * their own ref while forwarding, so dns_proxy can swap the "current" pool (SelectProvider) out
 * from under in-flight queries without a use-after-free: the old pool stays alive until every
 * worker still using it has called dot_pool_unref. */
DotPool *dot_pool_new(const DnsProvider *provider);

DotPool *dot_pool_ref(DotPool *pool);
void dot_pool_unref(DotPool *pool);

/* Forwards `query` (raw DNS message, `query_len` bytes) and blocks for the response. Returns a
 * newly allocated buffer via out_response/out_response_len on success (caller frees with g_free),
 * or FALSE with *error set on failure (timeout, TLS/cert failure, upstream closed mid-response —
 * one retry against a fresh connection already happened internally). */
gboolean dot_pool_forward(DotPool *pool, const guint8 *query, gsize query_len,
                           guint8 **out_response, gsize *out_response_len, GError **error);

#endif
