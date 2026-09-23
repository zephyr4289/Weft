// weft_tensor_dialect.h — RFC-0017 §2.1: the dtype dialect seam.
//
// WHY EXISTS: the view ABI's dtype field carries the WTS1 numeric codes
// (RFC-0016 §5 — frozen public constants, u8=0 .. f64=10). On any tree
// that carries core/c/weft_tensor.h (post-Series-10) the dialect IS that
// header's enum and the equivalence is asserted at compile time; on a
// tree without it (pre-Series-10 base, downstream forks) the same codes
// are defined here so the accelerator backends compile unchanged. One
// dialect, one numeric space, zero silent divergence — the __has_include
// seam the browser ports use for optional surfaces.

#ifndef WEFT_TENSOR_DIALECT_H
#define WEFT_TENSOR_DIALECT_H

#include <stdint.h>

#if defined(__has_include)
#  if __has_include("weft_tensor.h")
#    include "weft_tensor.h"
#    define WEFT_TENSOR_HAVE_WTS1 1
#  endif
#endif

#ifdef WEFT_TENSOR_HAVE_WTS1
// The dialect is the WTS1 enum — assert the frozen numeric space so a
// future WTS2 code shift is a compile error here, not a silent mismatch.
_Static_assert(WEFT_TENSOR_U8 == 0,  "dialect: u8==0 frozen");
_Static_assert(WEFT_TENSOR_U32 == 2, "dialect: u32==2 frozen");
_Static_assert(WEFT_TENSOR_I8 == 4,  "dialect: i8==4 frozen");
_Static_assert(WEFT_TENSOR_I32 == 6, "dialect: i32==6 frozen");
_Static_assert(WEFT_TENSOR_F16 == 8, "dialect: f16==8 frozen");
_Static_assert(WEFT_TENSOR_F32 == 9, "dialect: f32==9 frozen");
_Static_assert(WEFT_TENSOR_F64 == 10, "dialect: f64==10 frozen");
#else
/// WTS1 dtype dialect (identical numeric values — RFC-0016 §5).
typedef enum {
    WEFT_TENSOR_U8  = 0,
    WEFT_TENSOR_U16 = 1,
    WEFT_TENSOR_U32 = 2,
    WEFT_TENSOR_U64 = 3,
    WEFT_TENSOR_I8  = 4,
    WEFT_TENSOR_I16 = 5,
    WEFT_TENSOR_I32 = 6,
    WEFT_TENSOR_I64 = 7,
    WEFT_TENSOR_F16 = 8,   ///< IEEE 754 binary16, the weft_f16_codec dialect
    WEFT_TENSOR_F32 = 9,
    WEFT_TENSOR_F64 = 10,
} weft_tensor_dtype_t;
#endif

#endif // WEFT_TENSOR_DIALECT_H
