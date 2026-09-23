// weft_audit_stub.c — the ASAN-leg stand-in for the malloc interposer.
// ASAN owns malloc (compiling both in is a conflict — house rule), so
// the ASAN legs link this stub: same API, no interposition. The Law-1
// zero-alloc proof runs on the -O2 audit legs; ASAN legs prove memory
// safety (leaks/overflows), not allocation counts.

#include "weft_audit.h"

void weft_audit_reset(void) {}
void weft_audit_arm(int on) { (void)on; }
long weft_audit_count(void) { return 0; }
void weft_audit_report(const char* tag) { (void)tag; }
