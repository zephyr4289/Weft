// weft_cluster_internal.h — engine-internal shared surface between
// weft_cluster_ring.c and weft_cluster_consensus.c. NOT part of the public
// contract; do not consume outside core/c/cluster/.

#ifndef WEFT_CLUSTER_INTERNAL_H
#define WEFT_CLUSTER_INTERNAL_H

#include "weft_cluster.h"

/* Hook-respecting allocation (Law 1 accounting goes through these). */
void *wcr1_engine_alloc(size_t size, size_t alignment);
void  wcr1_engine_free(void *ptr);

/* Two-store register protocol (shared by seq, watermark, and any future
   hi/lo split register). Bounded retries -> WEFT_CLUSTER_E_SEQ_TORN. */
int wcr1_seqreg_read(const volatile uint32_t *hi, const volatile uint32_t *lo,
                     uint64_t max_retries, uint64_t *out);
int wcr1_seqreg_publish(const wcr1_transport_t *t, volatile uint32_t *hi,
                        volatile uint32_t *lo, uint64_t seq);

/* Consensus block write over the transport (producer side, single writer).
   Ordering: hb=odd -> fields (token LAST of the identity set) -> hb=even.
   Fenced per the seqlock read pattern in RFC 0018 §6.1. */
int wcr1_consensus_block_write(wcr1_ring_view_t *view,
                               const wcr1_transport_t *tr,
                               uint64_t term, uint32_t leader,
                               uint64_t lease_expire_ns, uint64_t token,
                               uint32_t cflags, uint64_t quorum_mask,
                               uint64_t *hb_cache_io);

#endif /* WEFT_CLUSTER_INTERNAL_H */
