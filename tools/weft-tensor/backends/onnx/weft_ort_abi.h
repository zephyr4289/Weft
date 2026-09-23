// weft_ort_abi.h — GENERATED: the OrtApi table mirror (RFC-0017 §5).
//
// GROUND TRUTH: backends/onnx/tests/abi-verify/onnxruntime_c_api.h
// (microsoft/onnxruntime v1.16.3, ORT_API_VERSION 16 — vendored, MIT).
// The table order below is extracted FROM that header by
// tests/abi-verify/extract_ort_table.py; every USED field carries a
// typed signature and an offsetof assert against its verified index;
// unused positions are reserved pads (never called). The bridge
// requests GetApi(16): newer runtimes serve it via the stable
// append-only table contract; anything older refuses at load.
// DO NOT EDIT BY HAND — regenerate.
#ifndef WEFT_ORT_ABI_H
#define WEFT_ORT_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef struct weft_ort_api {
    void* _reserved_0;  // table position 0 (unused)
    void* _reserved_1;  // table position 1 (unused)
    const char* (*GetErrorMessage)(const void* status);
    void* (*CreateEnv)(int level, const char* logid, void** out);
    void* _reserved_4;  // table position 4 (unused)
    void* _reserved_5;  // table position 5 (unused)
    void* _reserved_6;  // table position 6 (unused)
    void* _reserved_7;  // table position 7 (unused)
    void* (*CreateSessionFromArray)(void* env, const void* model_data, size_t model_data_len, const void* options, void** out);
    void* _reserved_9;  // table position 9 (unused)
    void* (*CreateSessionOptions)(void** out);
    void* _reserved_11;  // table position 11 (unused)
    void* _reserved_12;  // table position 12 (unused)
    void* _reserved_13;  // table position 13 (unused)
    void* _reserved_14;  // table position 14 (unused)
    void* _reserved_15;  // table position 15 (unused)
    void* _reserved_16;  // table position 16 (unused)
    void* (*DisableMemPattern)(void* options);
    void* _reserved_18;  // table position 18 (unused)
    void* (*DisableCpuMemArena)(void* options);
    void* _reserved_20;  // table position 20 (unused)
    void* _reserved_21;  // table position 21 (unused)
    void* _reserved_22;  // table position 22 (unused)
    void* (*SetSessionGraphOptimizationLevel)(void* options, int level);
    void* (*SetIntraOpNumThreads)(void* options, int n);
    void* _reserved_25;  // table position 25 (unused)
    void* _reserved_26;  // table position 26 (unused)
    void* _reserved_27;  // table position 27 (unused)
    void* _reserved_28;  // table position 28 (unused)
    void* _reserved_29;  // table position 29 (unused)
    void* (*SessionGetInputCount)(const void* session, size_t* out);
    void* (*SessionGetOutputCount)(const void* session, size_t* out);
    void* _reserved_32;  // table position 32 (unused)
    void* (*SessionGetInputTypeInfo)(const void* session, size_t index, void** out);
    void* (*SessionGetOutputTypeInfo)(const void* session, size_t index, void** out);
    void* _reserved_35;  // table position 35 (unused)
    void* _reserved_36;  // table position 36 (unused)
    void* _reserved_37;  // table position 37 (unused)
    void* _reserved_38;  // table position 38 (unused)
    void* _reserved_39;  // table position 39 (unused)
    void* _reserved_40;  // table position 40 (unused)
    void* _reserved_41;  // table position 41 (unused)
    void* _reserved_42;  // table position 42 (unused)
    void* _reserved_43;  // table position 43 (unused)
    void* _reserved_44;  // table position 44 (unused)
    void* _reserved_45;  // table position 45 (unused)
    void* _reserved_46;  // table position 46 (unused)
    void* _reserved_47;  // table position 47 (unused)
    void* _reserved_48;  // table position 48 (unused)
    void* (*CreateTensorWithDataAsOrtValue)(const void* info, void* p_data, size_t p_data_len, const int64_t* shape, size_t shape_len, int onnx_type, void** out);
    void* (*IsTensor)(const void* value, int* out);
    void* (*GetTensorMutableData)(void* value, void** out);
    void* _reserved_52;  // table position 52 (unused)
    void* _reserved_53;  // table position 53 (unused)
    void* _reserved_54;  // table position 54 (unused)
    void* (*CastTypeInfoToTensorInfo)(void* type_info, const void** tensor_info);
    void* _reserved_56;  // table position 56 (unused)
    void* _reserved_57;  // table position 57 (unused)
    void* _reserved_58;  // table position 58 (unused)
    void* _reserved_59;  // table position 59 (unused)
    void* (*GetTensorElementType)(const void* tensor_info, int* out);
    void* (*GetDimensionsCount)(const void* tensor_info, size_t* out);
    void* (*GetDimensions)(const void* tensor_info, int64_t* dims, size_t count);
    void* _reserved_63;  // table position 63 (unused)
    void* (*GetTensorShapeElementCount)(const void* tensor_info, size_t* out);
    void* (*GetTensorTypeAndShape)(const void* value, void** out);
    void* _reserved_66;  // table position 66 (unused)
    void* _reserved_67;  // table position 67 (unused)
    void* _reserved_68;  // table position 68 (unused)
    void* (*CreateCpuMemoryInfo)(int alloc_type, int mem_type, void** out);
    void* _reserved_70;  // table position 70 (unused)
    void* _reserved_71;  // table position 71 (unused)
    void* _reserved_72;  // table position 72 (unused)
    void* _reserved_73;  // table position 73 (unused)
    void* _reserved_74;  // table position 74 (unused)
    void* _reserved_75;  // table position 75 (unused)
    void* _reserved_76;  // table position 76 (unused)
    void* _reserved_77;  // table position 77 (unused)
    void* _reserved_78;  // table position 78 (unused)
    void* _reserved_79;  // table position 79 (unused)
    void* _reserved_80;  // table position 80 (unused)
    void* _reserved_81;  // table position 81 (unused)
    void* _reserved_82;  // table position 82 (unused)
    void* _reserved_83;  // table position 83 (unused)
    void* _reserved_84;  // table position 84 (unused)
    void* _reserved_85;  // table position 85 (unused)
    void* _reserved_86;  // table position 86 (unused)
    void* _reserved_87;  // table position 87 (unused)
    void* _reserved_88;  // table position 88 (unused)
    void* _reserved_89;  // table position 89 (unused)
    void* _reserved_90;  // table position 90 (unused)
    void* _reserved_91;  // table position 91 (unused)
    void (*ReleaseEnv)(void* env);
    void (*ReleaseStatus)(void* status);
    void (*ReleaseMemoryInfo)(void* info);
    void (*ReleaseSession)(void* session);
    void (*ReleaseValue)(void* value);
    void* _reserved_97;  // table position 97 (unused)
    void* _reserved_98;  // table position 98 (unused)
    void* _reserved_99;  // table position 99 (unused)
    void (*ReleaseSessionOptions)(void* options);
    void* _reserved_101;  // table position 101 (unused)
    void* _reserved_102;  // table position 102 (unused)
    void* _reserved_103;  // table position 103 (unused)
    void* _reserved_104;  // table position 104 (unused)
    void* _reserved_105;  // table position 105 (unused)
    void* _reserved_106;  // table position 106 (unused)
    void* _reserved_107;  // table position 107 (unused)
    void* _reserved_108;  // table position 108 (unused)
    void* _reserved_109;  // table position 109 (unused)
    void* _reserved_110;  // table position 110 (unused)
    void* _reserved_111;  // table position 111 (unused)
    void* _reserved_112;  // table position 112 (unused)
    void* _reserved_113;  // table position 113 (unused)
    void* _reserved_114;  // table position 114 (unused)
    void* _reserved_115;  // table position 115 (unused)
    void* _reserved_116;  // table position 116 (unused)
    void* _reserved_117;  // table position 117 (unused)
    void* _reserved_118;  // table position 118 (unused)
    void* _reserved_119;  // table position 119 (unused)
    void* _reserved_120;  // table position 120 (unused)
    void* _reserved_121;  // table position 121 (unused)
    void* _reserved_122;  // table position 122 (unused)
    void* _reserved_123;  // table position 123 (unused)
    void* _reserved_124;  // table position 124 (unused)
    void* _reserved_125;  // table position 125 (unused)
    void* _reserved_126;  // table position 126 (unused)
    void* _reserved_127;  // table position 127 (unused)
    void* _reserved_128;  // table position 128 (unused)
    void* _reserved_129;  // table position 129 (unused)
    void* (*AddSessionConfigEntry)(void* options, const char* key, const char* value);
    void* _reserved_131;  // table position 131 (unused)
    void* _reserved_132;  // table position 132 (unused)
    void* (*RunWithBinding)(void* session, const void* run_options, const void* io_binding);
    void* (*CreateIoBinding)(void* session, void** out);
    void (*ReleaseIoBinding)(void* binding);
    void* (*BindInput)(void* binding, const char* name, const void* value);
    void* (*BindOutput)(void* binding, const char* name, const void* value);
    void* _reserved_138;  // table position 138 (unused)
    void* _reserved_139;  // table position 139 (unused)
    void* (*GetBoundOutputValues)(const void* binding, size_t* count, const void** out);
} weft_ort_api_t;

_Static_assert(offsetof(weft_ort_api_t, GetErrorMessage) == 2 * sizeof(void*),
               "ort-abi: GetErrorMessage at table index 2 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, CreateEnv) == 3 * sizeof(void*),
               "ort-abi: CreateEnv at table index 3 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, CreateSessionFromArray) == 8 * sizeof(void*),
               "ort-abi: CreateSessionFromArray at table index 8 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, CreateSessionOptions) == 10 * sizeof(void*),
               "ort-abi: CreateSessionOptions at table index 10 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, DisableMemPattern) == 17 * sizeof(void*),
               "ort-abi: DisableMemPattern at table index 17 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, DisableCpuMemArena) == 19 * sizeof(void*),
               "ort-abi: DisableCpuMemArena at table index 19 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, SetSessionGraphOptimizationLevel) == 23 * sizeof(void*),
               "ort-abi: SetSessionGraphOptimizationLevel at table index 23 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, SetIntraOpNumThreads) == 24 * sizeof(void*),
               "ort-abi: SetIntraOpNumThreads at table index 24 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, SessionGetInputCount) == 30 * sizeof(void*),
               "ort-abi: SessionGetInputCount at table index 30 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, SessionGetOutputCount) == 31 * sizeof(void*),
               "ort-abi: SessionGetOutputCount at table index 31 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, SessionGetInputTypeInfo) == 33 * sizeof(void*),
               "ort-abi: SessionGetInputTypeInfo at table index 33 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, SessionGetOutputTypeInfo) == 34 * sizeof(void*),
               "ort-abi: SessionGetOutputTypeInfo at table index 34 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, CreateTensorWithDataAsOrtValue) == 49 * sizeof(void*),
               "ort-abi: CreateTensorWithDataAsOrtValue at table index 49 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, IsTensor) == 50 * sizeof(void*),
               "ort-abi: IsTensor at table index 50 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, GetTensorMutableData) == 51 * sizeof(void*),
               "ort-abi: GetTensorMutableData at table index 51 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, CastTypeInfoToTensorInfo) == 55 * sizeof(void*),
               "ort-abi: CastTypeInfoToTensorInfo at table index 55 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, GetTensorElementType) == 60 * sizeof(void*),
               "ort-abi: GetTensorElementType at table index 60 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, GetDimensionsCount) == 61 * sizeof(void*),
               "ort-abi: GetDimensionsCount at table index 61 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, GetDimensions) == 62 * sizeof(void*),
               "ort-abi: GetDimensions at table index 62 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, GetTensorShapeElementCount) == 64 * sizeof(void*),
               "ort-abi: GetTensorShapeElementCount at table index 64 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, GetTensorTypeAndShape) == 65 * sizeof(void*),
               "ort-abi: GetTensorTypeAndShape at table index 65 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, CreateCpuMemoryInfo) == 69 * sizeof(void*),
               "ort-abi: CreateCpuMemoryInfo at table index 69 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, ReleaseEnv) == 92 * sizeof(void*),
               "ort-abi: ReleaseEnv at table index 92 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, ReleaseStatus) == 93 * sizeof(void*),
               "ort-abi: ReleaseStatus at table index 93 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, ReleaseMemoryInfo) == 94 * sizeof(void*),
               "ort-abi: ReleaseMemoryInfo at table index 94 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, ReleaseSession) == 95 * sizeof(void*),
               "ort-abi: ReleaseSession at table index 95 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, ReleaseValue) == 96 * sizeof(void*),
               "ort-abi: ReleaseValue at table index 96 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, ReleaseSessionOptions) == 100 * sizeof(void*),
               "ort-abi: ReleaseSessionOptions at table index 100 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, AddSessionConfigEntry) == 130 * sizeof(void*),
               "ort-abi: AddSessionConfigEntry at table index 130 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, RunWithBinding) == 133 * sizeof(void*),
               "ort-abi: RunWithBinding at table index 133 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, CreateIoBinding) == 134 * sizeof(void*),
               "ort-abi: CreateIoBinding at table index 134 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, ReleaseIoBinding) == 135 * sizeof(void*),
               "ort-abi: ReleaseIoBinding at table index 135 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, BindInput) == 136 * sizeof(void*),
               "ort-abi: BindInput at table index 136 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, BindOutput) == 137 * sizeof(void*),
               "ort-abi: BindOutput at table index 137 (v1.16.3)");
_Static_assert(offsetof(weft_ort_api_t, GetBoundOutputValues) == 140 * sizeof(void*),
               "ort-abi: GetBoundOutputValues at table index 140 (v1.16.3)");

/// The loader entry point (the only symbol onnxruntime exports).
typedef const void* (*weft_ort_get_api_base_fn)(void);
/// OrtApiBase layout (verified): {GetApi@0, GetVersionString@1}.
typedef struct {
    const void* (*GetApi)(uint32_t version);
    const char* (*GetVersionString)(void);
} weft_ort_api_base_t;

#endif // WEFT_ORT_ABI_H
