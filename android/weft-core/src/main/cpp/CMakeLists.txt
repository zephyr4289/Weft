cmake_minimum_required(VERSION 3.22.1)
project("weft_core")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core/c
)

add_library(weft_core SHARED
    weft_jni.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core/c/weft.c
    # RFC 0004 fan-out ring (driver layer — the JNI fanout* surface in
    # weft_jni.c binds it; byte-compatible with core/ts/fanout.ts).
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core/c/fanout.c
)

target_link_libraries(weft_core
    android
    log
)

# Same feature-macro stance as core/c/Makefile (posix_memalign under strict
# C11 needs it on glibc; bionic ignores it harmlessly).
target_compile_definitions(weft_core PRIVATE _GNU_SOURCE)
