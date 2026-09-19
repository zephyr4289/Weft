#include <stdint.h>
#include <stdatomic.h>

_Atomic uint32_t g32;
_Atomic uint64_t g64;

void st_rel64(_Atomic uint64_t* a, uint64_t v) {
    atomic_store_explicit(a, v, memory_order_release);
}
void ld_acq64(_Atomic uint64_t* a) {
    (void)atomic_load_explicit(a, memory_order_acquire);
}
uint64_t xchg_acqrel(_Atomic uint64_t* a, uint64_t v) {
    return atomic_exchange_explicit(a, v, memory_order_acq_rel);
}
void fence_sc(void) {
    atomic_thread_fence(memory_order_seq_cst);
}
void st_rel32(_Atomic uint32_t* a, uint32_t v) {
    atomic_store_explicit(a, v, memory_order_release);
}
uint32_t ld_relaxed32(_Atomic uint32_t* a) {
    return atomic_load_explicit(a, memory_order_relaxed);
}
void st_relaxed32(_Atomic uint32_t* a, uint32_t v) {
    atomic_store_explicit(a, v, memory_order_relaxed);
}
