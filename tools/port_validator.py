#!/usr/bin/env python3
"""
tools/port_validator.py — structural validator for Phase 4 ports.

Per WO-P4-PORTS T2. One driver, per-language rule packs.
Checks: API surface, exchange-site markers, panic shielding (Kotlin),
SAFETY comments (Swift), headers, banners.

Usage: python3 tools/port_validator.py [--target kotlin|swift|dart|ts|all]
Output: JSON-line report per target. Exit 0/1.
"""
import argparse
import os
import re
import sys
import json
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
    all_pass = all(c.get('pass', False) for c in checks)
    result = {'target': f'fanout-{target}', 'pass': all_pass, 'checks': checks}
    print(json.dumps(result))
    return all_pass

def check_verified(target):
    """Validate a port's RFC-0005 VerifiedWeft module (Series 6 rule pack).
    Same JSON result shape as check_fanout."""
    path = VERIFIED_FILES[target]
    checks = []
    if not path.exists():
        checks.append({'file': str(path.relative_to(ROOT)), 'check': 'file_exists',
                       'pass': False, 'detail': 'verified module not found'})
        result = {'target': f'verified-{target}', 'pass': False, 'checks': checks}
        print(json.dumps(result))
        return False
    header_ok, header_detail = check_file_header(path, target)
    checks.append({'file': str(path.relative_to(ROOT)), 'check': 'header',
                   'pass': header_ok, 'detail': header_detail})
    with open(path) as f:
        src = f.read()
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
    
    return 0 if all_pass else 1

if __name__ == '__main__':
    sys.exit(main())
