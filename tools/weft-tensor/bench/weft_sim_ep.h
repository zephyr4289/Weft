// weft_sim_ep.h — RFC-0017 §7: the deterministic software execution
// provider (the honest [SIM-EP] label).
//
// WHY EXISTS: the benchmark's execution stage needs a model-shaped
// workload on hosts with no ORT/ggml — but a FAKE (no-op) stage would be
// speed theater. SIM-EP is a real 64x64 f32 matmul (logits = A * x) with
// an LCG-deterministic weight matrix and a deterministic input
// projection: bit-identical on every run, every host, every ISA without
// FMA variance (adds-only accumulation in fixed order). Every SIM-EP
// line in the evidence carries the [SIM-EP] label — it is NEVER
// presented as hardware inference (Law 4).

#ifndef WEFT_SIM_EP_H
#define WEFT_SIM_EP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEFT_SIM_EP_N 64u   ///< logits dimension (and the input dimension)

/// Fill the deterministic weight matrix (setup; once).
void weft_sim_ep_init(void);

/// One execution: x (64 f32, the projected input) -> logits (64 f32,
/// written to out). Deterministic, allocation-free, ~4096 adds+mults.
void weft_sim_ep_run(const float* x, float* out);

/// Project 64 input values from a normalized tensor (deterministic
/// strided sample — every 1024th element; fixed order, no reduction
/// variance). n_src must be >= 64 * 1024.
void weft_sim_ep_project(const float* src, size_t n_src, float* out64);

#ifdef __cplusplus
}
#endif

#endif // WEFT_SIM_EP_H
