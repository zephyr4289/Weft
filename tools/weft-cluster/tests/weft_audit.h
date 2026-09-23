// weft_audit.h — the Law-1 malloc-audit interposer (cluster variant of
// the weft-tensor accel-common discipline, RFC-0019 §7.3).
//
// WHY EXISTS: Law 1 says the steady-state transport loop performs ZERO
// heap allocations. Saying it is not evidence — the interposer IS the
// evidence: link this object into a gate binary and its malloc/calloc/
// realloc/free override the libc symbols for the WHOLE program (symbol
// interposition at executable scope), counting every allocation while
// the gate ARMS the window around its steady-state loop. A count > 0 at
// disarm is a gate failure with a number attached, not a vibe.
//
// BOOTSTRAP: dlsym(RTLD_NEXT, "malloc") itself allocates on some libc
// vintages — the classic recursion is broken by a static 64 KiB arena
// that serves (and only serves) allocations made before the real
// allocator is resolved. Arena-served frees are no-ops (range check).
// This is the standard interposer pattern; nothing clever is added.
//
// ASAN LEGS DO NOT LINK THIS FILE (ASAN owns malloc; compiling both in
// is a conflict — the house Makefile rule).

#ifndef WEFT_AUDIT_H
#define WEFT_AUDIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Zero the counter (call after warmup, before arming).
void weft_audit_reset(void);

/// Arm/disarm the counting window (the steady-state loop lives between
/// these two calls — arm AFTER every setup print, which may malloc its
/// stdio buffers on first use).
void weft_audit_arm(int on);

/// Allocations counted while armed (0 = Law 1 held).
long weft_audit_count(void);

/// One evidence line ("[malloc-audit] <tag>: N allocs while armed").
void weft_audit_report(const char* tag);

#ifdef __cplusplus
}
#endif

#endif // WEFT_AUDIT_H
