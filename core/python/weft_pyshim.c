// weft_pyshim.c — lifecycle shim for the Python (cffi) binding (issue #18-6).
//
// The kernel (weft.{h,c}, Tier-0 frozen) exposes weft_init/weft_destroy on
// caller-owned storage but no heap pair; the fan-out layer already ships
// weft_fanout_{new,free} / weft_fanout_reader_{new,free} precisely so
// foreign runtimes can bind allocation + release as ONE call each (the
// FFI-finalizer discipline, fanout.h). This shim extends the same
// discipline to the kernel — nothing else: allocation, init, destroy, free.
// Zero protocol logic lives here.

#include <stdlib.h>
#include "weft.h"

weft_t* weft_py_new(size_t payload_max) {
    weft_t* w = (weft_t*)calloc(1, sizeof(weft_t));
    if (!w) return NULL;
    if (weft_init(w, payload_max) != 0) {
        free(w);
        return NULL;
    }
    return w;
}

void weft_py_free(weft_t* w) {
    if (!w) return;
    weft_destroy(w);
    free(w);
}
