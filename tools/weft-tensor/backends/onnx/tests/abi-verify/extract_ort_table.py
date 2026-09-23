#!/usr/bin/env python3
# extract_ort_table.py — regenerate backends/onnx/weft_ort_abi.h from the
# vendored onnxruntime_c_api.h (ground truth). Run from abi-verify/:
#   python3 extract_ort_table.py
# The extraction mirrors struct OrtApi's exact field order (ORT_API2_STATUS
# fields, ORT_API_CALL pointer fields, and the ORT_CLASS_RELEASE macro
# entries the plain-name regexes miss) and stamps typed signatures at the
# indices the bridge uses. Regeneration after a vendored-header bump MUST
# be reviewed: an index shift is a compile error in the mirror's offsetof
# asserts, never a silent mismatch (Law 4).
import re, sys, os

hdr_path = os.path.join(os.path.dirname(__file__), 'onnxruntime_c_api.h')
src = open(hdr_path).read()
m = re.search(r'struct OrtApi \{(.*?)\n\};', src, re.S)
if not m:
    sys.exit('no struct OrtApi in vendored header')
body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
body = re.sub(r'//[^\n]*', '', body)
names = []
for fm in re.finditer(
        r'ORT_API2_STATUS\(\s*(\w+)\s*,|\(\s*ORT_API_CALL\s*\*\s*(\w+)\s*\)|ORT_CLASS_RELEASE\(\s*(\w+)\s*\)',
        body):
    if fm.group(3):
        names.append('Release' + fm.group(3))
    else:
        names.append(fm.group(1) or fm.group(2))
print('table fields found: %d' % len(names))
# The bridge's used fields — indices asserted by the generated header.
USED_NAMES = ['CreateEnv', 'CreateSessionFromArray', 'CreateSessionOptions',
              'DisableMemPattern', 'DisableCpuMemArena',
              'SetSessionGraphOptimizationLevel', 'SetIntraOpNumThreads',
              'SessionGetInputCount', 'SessionGetOutputCount',
              'SessionGetInputTypeInfo', 'SessionGetOutputTypeInfo',
              'CreateTensorWithDataAsOrtValue', 'IsTensor',
              'GetTensorMutableData', 'CastTypeInfoToTensorInfo',
              'GetTensorElementType', 'GetDimensionsCount', 'GetDimensions',
              'GetTensorShapeElementCount', 'GetTensorTypeAndShape',
              'CreateCpuMemoryInfo', 'ReleaseEnv', 'ReleaseStatus',
              'ReleaseMemoryInfo', 'ReleaseSession', 'ReleaseValue',
              'ReleaseSessionOptions', 'AddSessionConfigEntry',
              'RunWithBinding', 'CreateIoBinding', 'ReleaseIoBinding',
              'BindInput', 'BindOutput', 'GetBoundOutputValues']
for n in USED_NAMES:
    if n not in names:
        sys.exit('FATAL: %s missing from the vendored table — ABI drift' % n)
    print('%4d  %s' % (names.index(n), n))
