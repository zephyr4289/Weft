cmake_minimum_required(VERSION 3.22.1)
project("weft_core")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/c
)

add_library(weft_core SHARED
    weft_jni.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/c/weft.c
)

target_link_libraries(weft_core
    android
    log
)
