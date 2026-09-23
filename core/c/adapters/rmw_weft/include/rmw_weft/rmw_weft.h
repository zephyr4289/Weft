// rmw_weft.h — the rmw_weft public extension surface (rmw C-API is in
// compat/include/rmw/rmw.h; this header adds the engine's own contracts).
//
// WHY EXISTS: the directive's fast path — "zero serialization overhead
// when types match .weft or POD descriptors" — needs a typesupport the
// engine can size and hash without generated ROS code. rmw_weft accepts
// a POD descriptor (message size + stable name); every other typesupport
// identifier fails closed with RMW_RET_UNSUPPORTED. Tests also need
// ring-map introspection to PROVE zero-copy pointer identity (Rule 4) —
// those seams are here, clearly marked, and used by the native batteries.

#ifndef RMW_WEFT__RMW_WEFT_H_
#define RMW_WEFT__RMW_WEFT_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "rmw/rmw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RMW_WEFT_IMPLEMENTATION_ID "rmw_weft"
#define RMW_WEFT_TYPESUPPORT_ID "rmw_weft_pod_v1"

#include "rmw_weft/rmw_ring.h"

// ---------------------------------------------------------------------------
// POD/.weft descriptor typesupport (the zero-serialization fast path)
// ---------------------------------------------------------------------------

typedef struct rmw_weft_pod_ts {
    size_t message_size;   /* exact byte size of the POD message          */
    uint64_t type_hash;    /* FNV-1a 64 of `name` (wire identity)         */
    char name[64];         /* stable type name, e.g. "sensor_imu_pod"     */
} rmw_weft_pod_ts_t;

/// Construct a heap-owned typesupport (cold path — create/destroy only).
/// Returns NULL on invalid arguments (size 0 or name too long).
const rosidl_message_type_support_t * rmw_weft_create_pod_type_support(
    size_t message_size, const char * name);

/// Release a typesupport created above. Idempotent per pointer.
void rmw_weft_destroy_pod_type_support(
    const rosidl_message_type_support_t * ts);

// ---------------------------------------------------------------------------
// Error state (official rmw thread-local error string semantics)
// ---------------------------------------------------------------------------

/// Format into the thread-local error buffer (truncated at 1023 bytes).
void rmw_weft_set_error(const char * fmt, ...);

/// The implementation identifier this engine reports.
const char * rmw_weft_identifier(void);

// ---------------------------------------------------------------------------
// Test / diagnostics seams (used by tests/adapters/native batteries)
// ---------------------------------------------------------------------------

/// The subscription's ring map (subscriber is the creator). The aliasing
/// test computes (loan - ring_base) on BOTH sides of a fork and asserts
/// byte equality — the cross-process zero-copy proof.
const rmw_ring_map_t * rmw_weft_sub_ring(const rmw_subscription_t * sub);

/// The publisher's primary (first attached) ring map, or NULL.
const rmw_ring_map_t * rmw_weft_pub_ring0(const rmw_publisher_t * pub);

/// Ring name for a subscription (the /dev/shm object identity).
int rmw_weft_sub_ring_name(const rmw_subscription_t * sub, char * buf,
                           size_t buflen);

/// Publisher-side honest counters.
uint64_t rmw_weft_pub_published(const rmw_publisher_t * pub);
uint64_t rmw_weft_pub_failed(const rmw_publisher_t * pub);

/// Attached fan-out ring count (0 = no subscriber matched yet). Tests use
/// this to wait for the graph match before streaming — publishing to zero
/// rings is a legitimate DDS-style no-op success, and request/reply
/// protocols must not assume the peer is attached at t=0.
uint32_t rmw_weft_pub_ring_count(const rmw_publisher_t * pub);

/// Subscriber-side honest counters (messages taken, detected losses).
uint64_t rmw_weft_sub_taken(const rmw_subscription_t * sub);
uint64_t rmw_weft_sub_missed(const rmw_subscription_t * sub);

/// Publisher-side dropped count aggregated across the fan-out rings.
uint64_t rmw_weft_pub_dropped(const rmw_publisher_t * pub);

/// Force the publisher to rescan the registry sub list (tests use this to
/// make fan-out attach deterministic before the first publish).
int rmw_weft_pub_refresh(rmw_publisher_t * pub);

#ifdef __cplusplus
}
#endif

#endif  // RMW_WEFT__RMW_WEFT_H_
