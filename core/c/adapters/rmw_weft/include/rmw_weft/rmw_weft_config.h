// rmw_weft_config.h — compile-time + runtime configuration surface for the
// rmw_weft engine (shared by ring, registry, and the rmw C-API shim).
//
// WHY EXISTS: every tunable that affects shared-memory GEOMETRY is pinned
// here so creator and attacher agree without negotiation; every tunable
// that affects only local behavior is an environment knob with a sane
// default. Nothing in this header may change a wire layout silently.

#ifndef RMW_WEFT__RMW_WEFT_CONFIG_H_
#define RMW_WEFT__RMW_WEFT_CONFIG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- shared-memory geometry (wire-visible; DO NOT change casually) --- */

/// Ring depth per (topic, subscriber) pair. Power of two. 32 slots keeps a
/// 64 KiB-message ring at ~2.1 MiB — sized for /dev/shm-constrained CI
/// containers while absorbing sensor bursts (see D-62 §B.2 sizing math).
#define RMW_WEFT_RING_SLOTS_DEFAULT 32u

/// Payload capacity per slot. The directive's SLA message is 64 KiB.
#define RMW_WEFT_RING_PAYLOAD_DEFAULT 65536u

/// Topic registry capacity (/dev/shm/weft_rmw_d<domain>_registry).
#define RMW_WEFT_MAX_TOPICS 32u
#define RMW_WEFT_MAX_SUBS_PER_TOPIC 16u
#define RMW_WEFT_MAX_PUBS_PER_TOPIC 8u

/// Max topic name length (bytes, NUL included) in the registry.
#define RMW_WEFT_TOPIC_NAME_MAX 96u

/// Max ring name length (bytes, NUL included).
#define RMW_WEFT_RING_NAME_MAX 80u

/* --- local behavior knobs (environment overrides, honest defaults) --- */

/// Reader spin budget before futex parking: WEFT_RMW_SPIN_US (default 40).
/// The SLA path (< 1.5 us RTT) rides the spin phase; the parked path is
/// the container-friendly fallback (bench reports both).
int64_t rmw_weft_cfg_spin_ns(void);

/// Publisher full-ring zero-progress budget: WEFT_RMW_PUB_WAIT_US (default
/// 50000). RELIABLE publishers backpressure while the consumer's tail_ack
/// keeps advancing (the budget re-arms on every observed advance) and
/// return RMW_RET_TIMEOUT only after one full window of ZERO consumer
/// progress. 50 ms bridges the largest CFS throttle stalls measured in
/// the x86_64-sandbox CI container (~22-30 ms, D-62 §C.5); tight enough
/// to be an honest dead-peer detector (DDS liveliness windows are
/// typically 100 ms-seconds, so this remains aggressive).
int64_t rmw_weft_cfg_pub_wait_ns(void);

/// Reader take deadline default: WEFT_RMW_SUB_WAIT_US (default 100000).
int64_t rmw_weft_cfg_sub_wait_ns(void);

/// Ring geometry from env (WEFT_RMW_SLOTS / WEFT_RMW_PAYLOAD), validated
/// against the power-of-two / bound rules; falls back to the defaults.
uint32_t rmw_weft_cfg_slots(void);
uint32_t rmw_weft_cfg_payload(void);

/// Domain id clamp (registry file naming).
uint32_t rmw_weft_cfg_domain(uint32_t domain_id);

#ifdef __cplusplus
}
#endif

#endif  // RMW_WEFT__RMW_WEFT_CONFIG_H_
