#!/usr/bin/env python3
"""
tools/port_validator.py — structural validator + PROOF driver for the ports.

Per WO-P4-PORTS T2, upgraded per the Series-7 mandate ("validator -> proof"):
substring checks prove a file MENTIONS the right primitives; proofs prove
the primitives WORK. Three proof classes, opt-in via --prove (the CI shard
runs all three):

  1. COMPILE-AND-LOAD — build the C kernel into a shared object and drive a
     real publish/claim roundtrip through it (ctypes). Kotlin/Dart/Swift
     compile-or-delegate proofs follow the same shape where a toolchain
     exists; a missing toolchain is a DECLARED skip (loud), never silent.
  2. ORDERING-SITE PROOFS — the fan-out rule packs upgrade from "the marker
     string exists somewhere" to "the right ordering primitive appears at
     the right SITES": extract each protocol method's body (brace matching)
     and verify the access-mode discipline per docs/PORTS.md §6 (no raw
     ByteBuffer.getLong on ctrl sites in Kotlin, SC-only stamps + relaxed
     payload in Swift, Endian.little on every word access in Dart).
  3. TORTURE — the C F10 100k multi-reader torture runs as part of the
     validator's own gate (--torture, wired in CI).

Usage: python3 tools/port_validator.py [--target kotlin|swift|dart|ts|all]
                                       [--prove] [--torture]
Output: JSON-line report per target/proof. Exit 0/1.
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Pinned kernel symbols — every port must have a 1:1 counterpart
REQUIRED_SYMBOLS = {
    'kernel': ['init', 'publish', 'claim', 'revoke', 'reclaim', 'destroy', 'debug'],
    'envelope': ['encode', 'decode'],
}

# Exchange-site markers per language
EXCHANGE_MARKERS = {
    'kotlin': ['getAndSet', 'AtomicReference'],
    'swift': ['exchange', 'acquiringAndReleasing', 'ManagedAtomic'],
    'dart': [],  # plain assignment — single-isolate
    'ts': ['Atomics.exchange'],
}

# Fan-out rule pack (RFC 0004 driver layer, Series 5): each VM port ships a
# BYTE-COMPATIBLE ring alongside the C/Rust/TS rings. The validator checks
# existence + spec-citing header + the port-specific ordering markers (the
# memory-model row of docs/PORTS.md §6, mechanically) + the shared protocol
# API surface (begin/publish/claim/view/stats + the ring-bytes formula).
FANOUT_FILES = {
    'kotlin': {
        'path': ROOT / 'core' / 'kotlin' / 'Fanout.kt',
        'markers': ['VarHandle', 'byteBufferViewVarHandle', 'fullFence',
                    'setRelease', 'getAcquire', 'getOpaque'],
    },
    'swift': {
        'path': ROOT / 'core' / 'swift' / 'Fanout.swift',
        'markers': ['UnsafeAtomic', 'sequentiallyConsistent', '.relaxed',
                    'loadThenWrappingIncrement'],
    },
    'dart': {
        'path': ROOT / 'core' / 'dart' / 'fanout.dart',
        'markers': ['single-isolate', 'Endian.little', 'sublistView'],
    },
}

# API<33 compat regime rule pack (Series 7): the Kotlin fan-out carries a
# SECOND regime for Android API levels below VarHandle's 33 floor —
# AtomicLongArray SC stamps + bracket-discipline payload (the TS-port
# stance), routed by WeftFanoutFactory. Existence + honesty markers are
# validated alongside the primary ring.
FANOUT_COMPAT_FILES = {
    'kotlin': {
        'path': ROOT / 'core' / 'kotlin' / 'FanoutCompat.kt',
        'markers': ['AtomicLongArray', 'varHandleAvailable',
                    'bracket discipline', 'API < 33'],
    },
}

# Protocol API surface every fan-out port must expose (case-insensitive;
# the ring-bytes helper is named per language but contains 'ringbytes').
FANOUT_API = ['begin', 'publish', 'claim', 'view', 'stats', 'ringbytes']

# Series 6 — RFC-0005 VerifiedWeft rule pack: every VM port must carry the
# authenticated-frame module with the shared wire format, key schedule, and
# batch stream API (PORTS.md §7). Same shape as the fan-out rule pack.
VERIFIED_FILES = {
    'kotlin': ROOT / 'core' / 'kotlin' / 'Verified.kt',
    'swift': ROOT / 'core' / 'swift' / 'Verified.swift',
    'dart': ROOT / 'core' / 'dart' / 'verified.dart',
}
VERIFIED_MARKERS = [
    'Weft-VerifiedWeft-v1:key',   # domain-separated key schedule (shared)
    'VW_ENVELOPE_LEN',            # 16-byte signed envelope prefix (v1)
]
VERIFIED_API = ['derivekey', 'signer', 'verifier', 'cteq', 'recordencode',
                'batchdecodeverify', 'errtag']

def check_file_header(filepath, lang):
    """Check that the file has a 'why exists' paragraph citing a spec section."""
    try:
        with open(filepath) as f:
            first_lines = f.read(2000)
        # Look for a spec citation in the first 30 lines
        lines = first_lines.split('\n')[:30]
        for line in lines:
            if re.search(r'(§|Section|02-KERNEL|03-ENVELOPE|RFC-0001|WHITEPAPER|PORTS)', line):
                return True, f"header cites spec section"
        return False, "no spec section citation in header"
    except:
        return False, f"cannot read file"

def check_kotlin(filepath):
    checks = []
    with open(filepath) as f:
        src = f.read()
    
    # Exchange-site marker
    has_exchange = any(m in src for m in EXCHANGE_MARKERS['kotlin'])
    checks.append({'check': 'exchange_marker', 'pass': has_exchange,
                   'detail': 'getAndSet/AtomicReference found' if has_exchange else 'MISSING exchange marker'})
    
    # Panic shielding (JNI entries catch Throwable)
    if 'external fun' in src or 'JNIEXPORT' in src:
        has_shield = 'catch' in src and ('Throwable' in src or 'Exception' in src)
        checks.append({'check': 'panic_shield', 'pass': has_shield,
                       'detail': 'JNI entries catch Throwable' if has_shield else 'MISSING panic shield on JNI entries'})
    
    # API surface
    for category, symbols in REQUIRED_SYMBOLS.items():
        for sym in symbols:
            found = sym.lower() in src.lower()
            checks.append({'check': f'api_{category}_{sym}', 'pass': found,
                           'detail': f'{sym} found' if found else f'MISSING {sym}'})
    
    return checks

def check_swift(filepath):
    checks = []
    with open(filepath) as f:
        src = f.read()
    
    has_exchange = any(m in src for m in EXCHANGE_MARKERS['swift'])
    checks.append({'check': 'exchange_marker', 'pass': has_exchange,
                   'detail': 'ManagedAtomic.exchange(.acquiringAndReleasing)' if has_exchange else 'MISSING'})
    
    # SAFETY comments on unsafe blocks
    unsafe_blocks = re.findall(r'unsafe\s*\{', src)
    if unsafe_blocks:
        has_safety = 'SAFETY' in src or 'safety' in src.lower()
        checks.append({'check': 'safety_comments', 'pass': has_safety,
                       'detail': f'{len(unsafe_blocks)} unsafe blocks, SAFETY comments present' if has_safety
                       else f'{len(unsafe_blocks)} unsafe blocks WITHOUT SAFETY comments'})
    
    for category, symbols in REQUIRED_SYMBOLS.items():
        for sym in symbols:
            found = sym.lower() in src.lower()
            checks.append({'check': f'api_{category}_{sym}', 'pass': found,
                           'detail': f'{sym} found' if found else f'MISSING {sym}'})
    
    return checks

def check_dart(filepath):
    checks = []
    with open(filepath) as f:
        src = f.read()
    
    # No exchange marker expected (single-isolate, plain assignment)
    # But check for the single-isolate banner
    has_banner = 'single-isolate' in src.lower() or 'single isolate' in src.lower()
    checks.append({'check': 'single_isolate_banner', 'pass': has_banner,
                   'detail': 'single-isolate banner present' if has_banner else 'MISSING single-isolate banner'})
    
    for category, symbols in REQUIRED_SYMBOLS.items():
        for sym in symbols:
            found = sym.lower() in src.lower()
            checks.append({'check': f'api_{category}_{sym}', 'pass': found,
                           'detail': f'{sym} found' if found else f'MISSING {sym}'})
    
    return checks

def check_ts(filepath):
    checks = []
    with open(filepath) as f:
        src = f.read()
    
    has_exchange = any(m in src for m in EXCHANGE_MARKERS['ts'])
    checks.append({'check': 'exchange_marker', 'pass': has_exchange,
                   'detail': 'Atomics.exchange found' if has_exchange else 'MISSING'})
    
    for category, symbols in REQUIRED_SYMBOLS.items():
        for sym in symbols:
            found = sym.lower() in src.lower()
            checks.append({'check': f'api_{category}_{sym}', 'pass': found,
                           'detail': f'{sym} found' if found else f'MISSING {sym}'})
    
def check_fanout(target):
    """Validate a port's RFC-0004 fan-out ring module (Series 5 rule pack).
    Emits one JSON result line like validate_target; same shape so the CI
    aggregator (run_ports_validate_shard.sh) consumes it unchanged."""
    spec = FANOUT_FILES[target]
    path = spec['path']
    checks = []
    if not path.exists():
        checks.append({'file': str(path.relative_to(ROOT)), 'check': 'file_exists',
                       'pass': False, 'detail': 'fan-out ring module not found'})
        result = {'target': f'fanout-{target}', 'pass': False, 'checks': checks}
        print(json.dumps(result))
        return False
    header_ok, header_detail = check_file_header(path, target)
    checks.append({'file': str(path.relative_to(ROOT)), 'check': 'header',
                   'pass': header_ok, 'detail': header_detail})
    with open(path) as f:
        src = f.read()
    for m in spec['markers']:
        checks.append({'file': str(path.relative_to(ROOT)), 'check': f'fanout_marker_{m}',
                       'pass': m in src,
                       'detail': f'{m} found' if m in src else f'MISSING {m}'})
    for sym in FANOUT_API:
        found = sym.lower() in src.lower()
        checks.append({'file': str(path.relative_to(ROOT)), 'check': f'fanout_api_{sym}',
                       'pass': found,
                       'detail': f'{sym} found' if found else f'MISSING {sym}'})
    # --- ORDERING-SITE PROOFS (Series-7 upgrade: sites, not substrings) ---
    checks.extend(ordering_site_checks(target, src, path))
    all_pass = all(c.get('pass', False) for c in checks)
    result = {'target': f'fanout-{target}', 'pass': all_pass, 'checks': checks}
    print(json.dumps(result))
    return all_pass

<<<<<<< HEAD
def check_verified(target):
    """Validate a port's RFC-0005 VerifiedWeft module (Series 6 rule pack).
    Same JSON result shape as check_fanout."""
    path = VERIFIED_FILES[target]
    checks = []
    if not path.exists():
        checks.append({'file': str(path.relative_to(ROOT)), 'check': 'file_exists',
                       'pass': False, 'detail': 'verified module not found'})
        result = {'target': f'verified-{target}', 'pass': False, 'checks': checks}
=======

# ---------------------------------------------------------------------------
# Ordering-site proofs (Series 7 — the substring -> site upgrade)
# ---------------------------------------------------------------------------

def extract_function_body(src, signature_regex):
    """Extract a function/method body via brace matching. Returns the body
    text or None when the signature doesn't appear. String/char/comment
    literals are scrubbed first so braces inside them don't derail the
    match."""
    scrubbed = re.sub(r'//.*', '', src)
    scrubbed = re.sub(r'/\*.*?\*/', '', scrubbed, flags=re.S)
    scrubbed = re.sub(r'"(?:\\.|[^"\\])*"', '""', scrubbed)
    scrubbed = re.sub(r"'(?:\\.|[^'\\])*'", "''", scrubbed)
    m = re.search(signature_regex, scrubbed, flags=re.M)
    if not m:
        return None
    i = scrubbed.find('{', m.end())
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(scrubbed)):
        if scrubbed[j] == '{':
            depth += 1
        elif scrubbed[j] == '}':
            depth -= 1
            if depth == 0:
                return scrubbed[i:j + 1]
    return None


def ordering_site_checks(target, src, path):
    """Per-port ordering-site proofs. Each check pins WHICH ordering
    primitive must appear (or must NOT appear) inside a specific protocol
    method's body — docs/PORTS.md §6's memory-model row, mechanically."""
    rel = str(path.relative_to(ROOT))
    checks = []

    def site(name, ok, detail):
        checks.append({'file': rel, 'check': f'site_{name}', 'pass': bool(ok),
                       'detail': detail})

    if target == 'kotlin':
        begin = extract_function_body(src, r'fun begin\(\): ByteBuffer')
        publish = extract_function_body(src, r'fun publish\(\): Long')
        claim = extract_function_body(src, r'fun claim\(\): FanoutClaim')
        fill = extract_function_body(src, r'fun fill\(src: IntArray, words: Int\): Int')
        if begin is None or publish is None or claim is None or fill is None:
            site('kotlin_protocol_methods_present', False,
                 'one or more protocol methods not found')
            return checks
        site('kotlin_begin_invalidate_sc',
             'stampStoreVolatile' in begin and 'fullFence' in begin,
             'begin() must invalidate via stampStoreVolatile + fullFence (P1)')
        site('kotlin_begin_no_raw_ctrl_writes',
             not re.search(r'\.putLong\(', begin),
             'begin() must not write ctrl bytes with raw putLong')
        site('kotlin_publish_release_stamps',
             'stampStoreRelease' in publish and 'publishesAdd' in publish,
             'publish() must stamp via stampStoreRelease (publication point)')
        site('kotlin_publish_no_raw_ctrl_access',
             not re.search(r'\.(getLong|putLong)\(', publish),
             'publish() must not touch ctrl bytes raw')
        site('kotlin_claim_acquire_stamps',
             'stampLoadAcquire' in claim and 'fullFence' in claim,
             'claim() must load stamps acquire and fence before revalidation (P2)')
        site('kotlin_claim_opaque_words',
             'wordLoadOpaque' in claim,
             'claim() payload copy must use wordLoadOpaque (coherence-only)')
        site('kotlin_claim_no_raw_payload_access',
             not re.search(r'\.(getLong|getInt)\(', claim),
             'claim() must not read ring words raw (opaque view only)')
        site('kotlin_fill_opaque_stores',
             'wordStoreOpaque' in fill,
             'fill() must use wordStoreOpaque (race-free fill path)')
    elif target == 'swift':
        begin = extract_function_body(src, r'public func begin\(\) -> UnsafeMutableRawPointer')
        publish = extract_function_body(src, r'public func publish\(\) -> UInt64')
        claim = extract_function_body(src, r'public func claim\(\) -> FanoutClaim')
        fill = extract_function_body(src, r'public func fill\(_ src: \[UInt32\], _ words: Int\) -> Int')
        if begin is None or publish is None or claim is None or fill is None:
            site('swift_protocol_methods_present', False,
                 'one or more protocol methods not found')
            return checks
        site('swift_begin_sc_invalidate',
             '.sequentiallyConsistent' in begin,
             'begin() invalidate must be an SC store (the Swift stamp regime)')
        site('swift_publish_sc_stamps_only',
             publish.count('.sequentiallyConsistent') >= 2,
             'publish() stamp + latestSeq must both be SC stores')
        site('swift_claim_sc_stamps',
             claim.count('.sequentiallyConsistent') >= 3,
             'claim() latest/sB/sA stamp loads must all be SC')
        site('swift_claim_relaxed_payload',
             '.relaxed' in claim,
             'claim() payload copy must be relaxed-atomic words (the C-port stance)')
        site('swift_fill_relaxed_payload',
             '.relaxed' in fill,
             'fill() payload stores must be relaxed-atomic (race-free path)')
    elif target == 'dart':
        begin = extract_function_body(src, r'ByteData begin\(\)')
        publish = extract_function_body(src, r'int publish\(\)')
        claim = extract_function_body(src, r'FanoutClaim claim\(\)')
        if begin is None or publish is None or claim is None:
            site('dart_protocol_methods_present', False,
                 'one or more protocol methods not found')
            return checks
        for name, body in (('begin', begin), ('publish', publish), ('claim', claim)):
            bare = re.findall(r'(?:get|set)U(?:int|Int)(?:32|64)\((?![^)]*Endian)', body)
            site(f'dart_{name}_endian_explicit',
                 len(bare) == 0,
                 f'{name}(): {len(bare)} accessor(s) missing Endian.little — '
                 'the wire contract requires byte-order at every site' if bare else
                 'every word accessor carries the explicit wire endianness')
        site('dart_claim_bracket_retained',
             claim.count('getUint64(16 + 8 * k') >= 2 or claim.count('getUint64(16 + 8*k') >= 2,
             'claim() must retain the stamp bracket (sB before copy, sA after)')
    return checks


def _emit_proof(name, ok, detail, extra=None):
    doc = {'target': f'proof-{name}', 'pass': bool(ok), 'detail': detail}
    if extra:
        doc['extra'] = extra
    print(json.dumps(doc))
    return bool(ok)


def prove_c():
    """Compile the C kernel + fan-out into a .so, load it via ctypes, and
    drive a real begin/publish/claim roundtrip. The 'substring -> proof'
    flagship: the validator now EXECUTES the kernel it validates."""
    core_c = ROOT / 'core' / 'c'
    cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if cc is None:
        return _emit_proof('c-compile-load', False,
                           'SKIPPED (declared): no C compiler on host')
    with tempfile.TemporaryDirectory(prefix='weft-prove-') as td:
        so = Path(td) / 'libweft_prove.so'
        build = subprocess.run(
            [cc, '-shared', '-fPIC', '-O1', '-I', str(core_c),
             str(core_c / 'weft.c'), str(core_c / 'fanout.c'),
             str(core_c / 'frame_cursor.c'), '-o', str(so)],
            capture_output=True, text=True)
        if build.returncode != 0:
            return _emit_proof('c-compile-load', False,
                               f'compile failed: {build.stderr[:400]}')
        try:
            lib = ctypes.CDLL(str(so))
        except OSError as e:
            return _emit_proof('c-compile-load', False, f'dlopen failed: {e}')

        # --- kernel roundtrip: weft_t is a header-defined struct —
        # allocate storage, init, drive begin/publish/claim for real.
        lib.weft_init.restype = ctypes.c_int
        lib.weft_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        lib.weft_destroy.restype = None
        lib.weft_destroy.argtypes = [ctypes.c_void_p]
        wbuf = (ctypes.c_ubyte * 512)()
        h = ctypes.addressof(wbuf)
        if lib.weft_init(h, 256) != 0:
            return _emit_proof('c-compile-load', False, 'weft_init failed')
        try:
            lib.weft_w_begin.restype = ctypes.c_void_p
            lib.weft_w_begin.argtypes = [ctypes.c_void_p]
            cur = lib.weft_w_begin(h)
            if not cur:
                return _emit_proof('c-compile-load', False, 'weft_w_begin returned NULL')
            buf = (ctypes.c_ubyte * 256).from_address(cur)
            for i in range(256):
                buf[i] = (i * 7 + 1) & 0xFF
            lib.weft_publish.restype = ctypes.c_int
            lib.weft_publish.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
            rc = lib.weft_publish(h, 1, 256)
            if rc != 0:
                return _emit_proof('c-compile-load', False, f'weft_publish rc={rc}')
            lib.weft_r_claim.restype = ctypes.c_uint32
            lib.weft_r_claim.argtypes = [ctypes.c_void_p]
            seq = lib.weft_r_claim(h)
            if seq != 1:
                return _emit_proof('c-compile-load', False, f'claim seq={seq} expected 1')
            lib.weft_r_live_ptr.restype = ctypes.c_void_p
            lib.weft_r_live_ptr.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            rptr = lib.weft_r_live_ptr(h, 16)
            if not rptr:
                return _emit_proof('c-compile-load', False, 'r_live_ptr NULL')
            rbuf = (ctypes.c_ubyte * 256).from_address(rptr)
            for i in range(0, 256, 5):
                if rbuf[i] != (i * 7 + 1) & 0xFF:
                    return _emit_proof('c-compile-load', False,
                                       f'payload mismatch at byte {i}')
        finally:
            lib.weft_destroy(h)

        # --- fan-out roundtrip through the same .so ---
        lib.weft_fanout_new.restype = ctypes.c_void_p
        lib.weft_fanout_new.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        fh = lib.weft_fanout_new(256, 4)
        if not fh:
            return _emit_proof('c-compile-load', False, 'weft_fanout_new NULL')
        lib.weft_fanout_begin.restype = ctypes.c_void_p
        lib.weft_fanout_begin.argtypes = [ctypes.c_void_p]
        fcur = lib.weft_fanout_begin(fh)
        fbuf = (ctypes.c_ubyte * 256).from_address(fcur)
        for i in range(256):
            fbuf[i] = (i * 13 + 3) & 0xFF
        lib.weft_fanout_publish.restype = ctypes.c_int64
        lib.weft_fanout_publish.argtypes = [ctypes.c_void_p]
        pub = lib.weft_fanout_publish(fh)
        if pub != 1:
            return _emit_proof('c-compile-load', False, f'fanout publish={pub}')
        lib.weft_fanout_ring_bytes.restype = ctypes.c_size_t
        lib.weft_fanout_ring_bytes.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        rb = lib.weft_fanout_ring_bytes(256, 4)
        lib.weft_fanout_ring.restype = ctypes.c_void_p
        lib.weft_fanout_ring.argtypes = [ctypes.c_void_p]
        lib.weft_fanout_reader_new.restype = ctypes.c_void_p
        lib.weft_fanout_reader_new.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                               ctypes.c_uint32, ctypes.c_uint32]
        rr = lib.weft_fanout_reader_new(lib.weft_fanout_ring(fh), rb, 256, 4)
        if not rr:
            return _emit_proof('c-compile-load', False, 'weft_fanout_reader_new NULL')
        lib.weft_fanout_claim.restype = ctypes.c_void_p
        lib.weft_fanout_claim.argtypes = [ctypes.c_void_p]
        rec = lib.weft_fanout_claim(rr)

        class Claim(ctypes.Structure):
            _fields_ = [('fresh', ctypes.c_int32), ('_pad', ctypes.c_int32),
                        ('seq', ctypes.c_uint64), ('dropped', ctypes.c_int64)]

        c = Claim.from_address(rec)
        if c.fresh != 1 or c.seq != 1:
            return _emit_proof('c-compile-load', False,
                               f'fanout claim fresh={c.fresh} seq={c.seq}')
        return _emit_proof('c-compile-load', True,
                           'kernel + fan-out roundtrips live through the compiled .so',
                           {'payload_check': 'stride 5', 'fanout_claim': 'fresh=1 seq=1'})


def prove_kotlin():
    """Compile the canonical Kotlin port (kotlinc) when a compiler exists;
    otherwise record the CI delegation (the android gradle leg compiles and
    runs the full F-series for the same SHA). Delegation is declared in the
    report, never silent."""
    kotlinc = shutil.which('kotlinc')
    if kotlinc is None:
        return _emit_proof('kotlin-compile', True,
                           'DELEGATED: no kotlinc on host — the android gradle '
                           'leg compiles core/kotlin mirrors and runs the full '
                           'F-series (incl. compat matrix) for this SHA')
    with tempfile.TemporaryDirectory(prefix='weft-kt-') as td:
        srcs = [str(ROOT / 'core' / 'kotlin' / f)
                for f in ('Fanout.kt', 'FanoutCompat.kt', 'FrameCursor.kt',
                          'Heddle.kt', 'Steward.kt', 'Weft.kt', 'TriadNative.kt')]
        build = subprocess.run([kotlinc] + srcs + ['-d', td], capture_output=True,
                               text=True, timeout=600)
        if build.returncode != 0:
            return _emit_proof('kotlin-compile', False,
                               f'kotlinc failed: {build.stderr[:400]}')
        return _emit_proof('kotlin-compile', True,
                           'canonical Kotlin port (both fan-out regimes) compiles clean')


def prove_dart():
    """The Dart proof: a real single-isolate fan-out roundtrip via the
    canonical core/dart module (pure dart:typed_data — any dart VM)."""
    dart = shutil.which('dart')
    if dart is None:
        return _emit_proof('dart-roundtrip', True,
                           'DELEGATED: no dart on host — the flutter leg runs '
                           'dart analyze --fatal-infos + the F-series for this SHA')
    script = f'''
import 'dart:typed_data';
import 'dart:io';
import '{(ROOT / "core" / "dart" / "fanout.dart").asUri()}';

void main() {{
  final b = WeftFanoutBroadcaster(64, 4);
  final r = b.createReader();
  if (r.claim().fresh) throw StateError('fresh on empty ring');
  final view = b.begin();
  for (var w = 0; w < 16; w++) view.setUint32(4 * w, w * 7, Endian.little);
  if (b.publish() != 1) throw StateError('publish seq');
  final c = r.claim();
  if (!c.fresh || c.seq != 1) throw StateError('claim $c');
  for (var w = 0; w < 16; w++) {{
    if (r.view()[w] != w * 7) throw StateError('payload word $w');
  }}
  stderr.writeln('PROOF-OK');
  exit(0);
}}
'''
    with tempfile.NamedTemporaryFile('w', suffix='.dart', delete=False) as tf:
        tf.write(script)
        path = tf.name
    try:
        run = subprocess.run([dart, path], capture_output=True, text=True,
                             timeout=120, cwd=str(ROOT))
        ok = run.returncode == 0 and 'PROOF-OK' in run.stderr
        return _emit_proof('dart-roundtrip', ok,
                           'canonical Dart fan-out roundtrip live' if ok
                           else f'dart run failed: {(run.stderr or run.stdout)[:400]}')
    finally:
        os.unlink(path)


def prove_swift():
    """Typecheck-proof the canonical Swift port when swiftc exists; the full
    F-series + Metal probe battery delegates to the apple leg. Fanout.swift
    imports swift-atomics, which cannot resolve standalone — the typecheck
    therefore covers the non-atomic kernel files and the atomic battery is
    the apple leg's job (declared split, not a silent pass)."""
    swiftc = shutil.which('swiftc')
    if swiftc is None:
        return _emit_proof('swift-typecheck', True,
                           'DELEGATED: no swiftc on host — the apple leg runs '
                           'swift build + swift test (F-series + Metal probe) '
                           'for this SHA')
    srcs = [str(ROOT / 'core' / 'swift' / f)
            for f in ('Weft.swift', 'FrameCursor.swift', 'Heddle.swift', 'Steward.swift')]
    srcs = [s for s in srcs if Path(s).exists()]
    build = subprocess.run([swiftc, '-typecheck', '-swift-version', '5'] + srcs,
                           capture_output=True, text=True, timeout=600)
    if build.returncode != 0:
        return _emit_proof('swift-typecheck', False,
                           f'swiftc -typecheck failed: {build.stderr[:400]}')
    return _emit_proof('swift-typecheck', True,
                       'Swift kernel files typecheck clean (Fanout.swift battery '
                       'delegates to the apple leg — swift-atomics dep)')


def prove_torture():
    """The F10 100k torture as part of the validator itself (build + run)."""
    cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if cc is None:
        return _emit_proof('torture-f10', False,
                           'SKIPPED (declared): no C compiler on host')
    core_c = ROOT / 'core' / 'c'
    with tempfile.TemporaryDirectory(prefix='weft-torture-') as td:
        runner = Path(td) / 'fanout-runner'
        build = subprocess.run(
            [cc, '-O2', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-pthread',
             str(core_c / 'fanout_runner.c'), str(core_c / 'fanout.c'),
             str(core_c / 'weft.c'), '-o', str(runner)],
            capture_output=True, text=True)
        if build.returncode != 0:
            return _emit_proof('torture-f10', False,
                               f'torture build failed: {build.stderr[:400]}')
        run = subprocess.run([str(runner), 'torture', '100000', '4', '64', '3'],
                             capture_output=True, text=True, timeout=300)
        ok = run.returncode == 0 and 'verdict=PASS' in run.stdout
        m = re.search(r'rate=\S+', run.stdout)
        rate = m.group(0) if m else ''
        return _emit_proof('torture-f10', ok,
                           f'100k-frame 3-reader torture {"PASS" if ok else "FAILED"} {rate}')

def check_fanout_compat(target):
    """Validate a port's API<33 compat fan-out regime (Series-7 rule pack).
    Same output shape as check_fanout so the CI aggregator consumes both."""
    spec = FANOUT_COMPAT_FILES[target]
    path = spec['path']
    checks = []
    if not path.exists():
        checks.append({'file': str(path.relative_to(ROOT)), 'check': 'file_exists',
                       'pass': False, 'detail': 'fan-out compat module not found'})
        result = {'target': f'fanout-compat-{target}', 'pass': False, 'checks': checks}
>>>>>>> 4611ef0 (feat(validator): substring -> PROOF — compile-and-load, ordering-site proofs, torture)
        print(json.dumps(result))
        return False
    header_ok, header_detail = check_file_header(path, target)
    checks.append({'file': str(path.relative_to(ROOT)), 'check': 'header',
                   'pass': header_ok, 'detail': header_detail})
    with open(path) as f:
        src = f.read()
<<<<<<< HEAD
    # Port naming conventions differ (VW_ENVELOPE_LEN / vwEnvelopeLen /
    # vwEnvelopeLen): compare case- and underscore-insensitively so the rule
    # checks the SYMBOL, not the spelling.
    norm = src.lower().replace('_', '')
    for m in VERIFIED_MARKERS:
        found = m in src or m.lower().replace('_', '') in norm
        checks.append({'file': str(path.relative_to(ROOT)), 'check': f'verified_marker_{m}',
                       'pass': found,
                       'detail': f'{m} found' if found else f'MISSING {m}'})
    for sym in VERIFIED_API:
        found = sym.lower().replace('_', '') in norm
        checks.append({'file': str(path.relative_to(ROOT)), 'check': f'verified_api_{sym}',
                       'pass': found,
                       'detail': f'{sym} found' if found else f'MISSING {sym}'})
    all_pass = all(c.get('pass', False) for c in checks)
    result = {'target': f'verified-{target}', 'pass': all_pass, 'checks': checks}
    print(json.dumps(result))
    return all_pass

=======
    for m in spec['markers']:
        checks.append({'file': str(path.relative_to(ROOT)), 'check': f'fanout_compat_marker_{m}',
                       'pass': m in src,
                       'detail': f'{m} found' if m in src else f'MISSING {m}'})
    all_pass = all(c.get('pass', False) for c in checks)
    result = {'target': f'fanout-compat-{target}', 'pass': all_pass, 'checks': checks}
    print(json.dumps(result))
    return all_pass


>>>>>>> 4611ef0 (feat(validator): substring -> PROOF — compile-and-load, ordering-site proofs, torture)
def validate_target(target, files):
    """Validate all files for a target language.
    Only the kernel file is checked for full API surface.
    Other files are checked for headers + relevant markers only."""
    all_checks = []
    kernel_file = files[0]  # First file is the kernel
    other_files = files[1:]  # Steward, Heddle, JNI, README

    for filepath in files:
        if not filepath.exists():
            all_checks.append({'file': str(filepath), 'check': 'file_exists', 'pass': False, 'detail': 'file not found'})
            continue

        # Header check (all files)
        header_ok, header_detail = check_file_header(filepath, target)
        all_checks.append({'file': str(filepath.relative_to(ROOT)), 'check': 'header', 'pass': header_ok, 'detail': header_detail})

        is_kernel = (filepath == kernel_file)
        is_readme = filepath.suffix == '.md'

        if is_readme:
            # READMEs: just check header + STATUS banner
            try:
                with open(filepath) as f:
                    src = f.read()
                has_banner = 'SOURCE-ONLY' in src or 'SOURCE ONLY' in src
                all_checks.append({'file': str(filepath.relative_to(ROOT)), 'check': 'status_banner', 'pass': has_banner,
                                   'detail': 'STATUS: SOURCE-ONLY banner present' if has_banner else 'MISSING STATUS banner'})
                if target == 'dart':
                    has_si = 'single-isolate' in src.lower() or 'single isolate' in src.lower()
                    all_checks.append({'file': str(filepath.relative_to(ROOT)), 'check': 'single_isolate_banner', 'pass': has_si,
                                       'detail': 'single-isolate banner present' if has_si else 'MISSING single-isolate banner'})
            except:
                pass
            continue

        # Language-specific checks
        if target == 'kotlin':
            checks = check_kotlin(filepath) if is_kernel else []
        elif target == 'swift':
            checks = check_swift(filepath) if is_kernel else []
        elif target == 'dart':
            if is_kernel:
                checks = check_dart(filepath)
            else:
                # Non-kernel Dart: just check single-isolate banner
                checks = []
                try:
                    with open(filepath) as f:
                        src = f.read()
                    has_si = 'single-isolate' in src.lower() or 'single isolate' in src.lower()
                    checks.append({'check': 'single_isolate_banner', 'pass': has_si,
                                   'detail': 'single-isolate banner present' if has_si else 'MISSING'})
                except:
                    pass
        elif target == 'ts':
            # TS Heddles: just check header + import of Weft
            checks = []
            try:
                with open(filepath) as f:
                    src = f.read()
                has_import = 'weft' in src.lower() and ('import' in src.lower())
                checks.append({'check': 'weft_import', 'pass': has_import,
                               'detail': 'imports Weft kernel' if has_import else 'MISSING Weft import'})
            except:
                pass
        else:
            checks = []

        all_checks.extend([{'file': str(filepath.relative_to(ROOT)), **c} for c in checks])

    all_pass = all(c.get('pass', False) for c in all_checks)
    result = {'target': target, 'pass': all_pass, 'checks': all_checks}
    print(json.dumps(result))
    return all_pass

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--target', default='all', choices=['kotlin', 'swift', 'dart', 'ts', 'all'])
    ap.add_argument('--prove', action='store_true',
                    help='run the Series-7 proofs: compile-and-load the C kernel '
                         '(.so roundtrip via ctypes), Kotlin/Dart/Swift '
                         'compile-or-delegate proofs')
    ap.add_argument('--torture', action='store_true',
                    help='run the C F10 100k 3-reader torture as part of the gate')
    args = ap.parse_args()
    
    targets = {
        'kotlin': [
            ROOT / 'core' / 'kotlin' / 'Weft.kt',
            ROOT / 'core' / 'kotlin' / 'Steward.kt',
            ROOT / 'core' / 'kotlin' / 'Heddle.kt',
            ROOT / 'core' / 'kotlin' / 'TriadNative.kt',
            ROOT / 'core' / 'kotlin' / 'README.md',
        ],
        'swift': [
            ROOT / 'core' / 'swift' / 'Weft.swift',
            ROOT / 'core' / 'swift' / 'Steward.swift',
            ROOT / 'core' / 'swift' / 'Heddle.swift',
            ROOT / 'core' / 'swift' / 'README.md',
        ],
        'dart': [
            ROOT / 'core' / 'dart' / 'weft.dart',
            ROOT / 'core' / 'dart' / 'steward.dart',
            ROOT / 'core' / 'dart' / 'heddle.dart',
            ROOT / 'core' / 'dart' / 'README.md',
        ],
        'ts': [
            ROOT / 'heddles' / 'react' / 'WeftCanvas.tsx',
            ROOT / 'heddles' / 'svelte' / 'weft-action.ts',
            ROOT / 'heddles' / 'vue' / 'useWeft.ts',
            ROOT / 'heddles' / 'react-native' / 'weft-rn.ts',
        ],
    }
    
    targets_to_run = [args.target] if args.target != 'all' else list(targets.keys())
    all_pass = True
    for target in targets_to_run:
        files = targets.get(target, [])
        if not validate_target(target, files):
            all_pass = False
        # Fan-out rule pack: kotlin/swift/dart ports carry the Series-5 ring.
        if target in FANOUT_FILES:
            if not check_fanout(target):
                all_pass = False
        # Verified rule pack (Series 6): every VM port carries RFC-0005.
        if target in VERIFIED_FILES:
            if not check_verified(target):
                all_pass = False
        # API<33 compat rule pack: kotlin carries the Series-7 second regime.
        if target in FANOUT_COMPAT_FILES:
            if not check_fanout_compat(target):
                all_pass = False

    # --- Series-7 proofs (validator -> proof) ---
    if args.prove:
        if not prove_c():
            all_pass = False
        if not prove_kotlin():
            all_pass = False
        if not prove_dart():
            all_pass = False
        if not prove_swift():
            all_pass = False
    if args.torture:
        if not prove_torture():
            all_pass = False
    return 0 if all_pass else 1

if __name__ == '__main__':
    sys.exit(main())
