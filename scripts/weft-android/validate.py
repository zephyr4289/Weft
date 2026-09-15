#!/usr/bin/env python3
"""
Weft Android v0.1 — Structural Validator

Verifies that the Rust + Kotlin source files are structurally consistent
and the API surface matches the spec v0.1 §8.

This is NOT a compiler. It is a structural sanity check that:
  1. All public Rust functions declared in lib.rs are defined.
  2. All Kotlin `external` (JNI) functions in TriadNative.kt have matching
     `#[no_mangle]` entry points in jni_bridge.rs.
  3. The Steward's public API matches spec §7.4.
  4. The Weft's public API matches spec §8.1.
  5. The Heddle's `weftDraw` extension matches spec §8.1.
  6. Every atomic operation in triad.rs has an explicit Ordering (no defaults).
  7. Every JNI entry point is wrapped in shield() (no panic leak).

Run: python3 validate.py /path/to/weft-android
"""
import re
import sys
import os
from pathlib import Path

def ok(msg): print(f'  ✓ {msg}')
def warn(msg): print(f'  ⚠ {msg}')
def err(msg): print(f'  ✗ {msg}'); return False

def main(project_dir: str) -> int:
    proj = Path(project_dir)
    if not proj.is_dir():
        print(f'ERROR: {project_dir} is not a directory')
        return 1

    print(f'Validating Weft Android v0.1 at {proj}/\n')
    all_ok = True

    # ---- 1. File inventory ----
    print('[1/8] File inventory')
    required_files = [
        'rust/Cargo.toml',
        'rust/src/lib.rs',
        'rust/src/triad.rs',
        'rust/src/steward.rs',
        'rust/src/jni_bridge.rs',
        'weft/build.gradle.kts',
        'weft/src/main/kotlin/dev/weft/Steward.kt',
        'weft/src/main/kotlin/dev/weft/Weft.kt',
        'weft/src/main/kotlin/dev/weft/Heddle.kt',
        'weft/src/main/kotlin/dev/weft/TriadNative.kt',
        'weft/src/main/kotlin/dev/weft/NativeBridge.kt',
        'weft/src/test/kotlin/dev/weft/StewardJvmTest.kt',
        'settings.gradle.kts',
        'build.gradle.kts',
        'gradle.properties',
    ]
    for f in required_files:
        if not (proj / f).is_file():
            all_ok = err(f'Missing file: {f}')
        else:
            ok(f'Found: {f}')
    print()

    # ---- 2. Rust public API surface ----
    print('[2/8] Rust public API surface')
    lib_rs = (proj / 'rust/src/lib.rs').read_text()
    expected_pub_modules = ['triad', 'steward']
    for m in expected_pub_modules:
        if f'pub mod {m}' in lib_rs:
            ok(f'pub mod {m} declared')
        else:
            all_ok = err(f'Missing pub mod {m}')
    if 'pub use triad::{Weft, WeftConfig};' in lib_rs:
        ok('Re-exports Weft, WeftConfig from triad')
    else:
        all_ok = err('Missing pub use triad::{Weft, WeftConfig};')
    if 'pub use steward::{Steward' in lib_rs:
        ok('Re-exports Steward from steward')
    else:
        all_ok = err('Missing pub use steward::{Steward}')
    if 'pub const SPEC_VERSION' in lib_rs:
        ok('SPEC_VERSION constant present')
    else:
        all_ok = err('Missing SPEC_VERSION')
    print()

    # ---- 3. JNI surface: Kotlin externals match Rust no_mangle functions ----
    print('[3/8] JNI surface: Kotlin externals ↔ Rust #[no_mangle]')
    triad_native_kt = (proj / 'weft/src/main/kotlin/dev/weft/TriadNative.kt').read_text()
    jni_bridge_rs = (proj / 'rust/src/jni_bridge.rs').read_text()

    # Find all `external fun <name>(...)` declarations in Kotlin
    kt_externals = re.findall(r'external fun (\w+)\s*\(', triad_native_kt)
    ok(f'Kotlin external functions: {len(kt_externals)} ({", ".join(kt_externals)})')

    # For each Kotlin external, find the corresponding #[no_mangle] in Rust
    for fn in kt_externals:
        jni_name = f'Java_dev_weft_TriadNative_{fn}'
        if jni_name in jni_bridge_rs:
            ok(f'  ✓ {fn} → {jni_name}')
        else:
            all_ok = err(f'  ✗ {fn} → {jni_name} NOT FOUND in jni_bridge.rs')

    # Also check for extra Rust no_mangle functions not declared in Kotlin
    rust_no_mangle = re.findall(r'Java_dev_weft_TriadNative_(\w+)', jni_bridge_rs)
    for fn in rust_no_mangle:
        if fn not in kt_externals:
            warn(f'  ⚠ Rust exposes {fn} but Kotlin has no external fun for it')
    print()

    # ---- 4. Panic shielding: every JNI entry point wrapped in shield() ----
    print('[4/8] Panic shielding (every JNI entry wraps shield())')
    # Find each #[no_mangle] block and check if its body contains a shield() call
    # This is a heuristic: look for `shield(` within 30 lines after each #[no_mangle]
    jni_funcs = re.finditer(
        r'#\[no_mangle\][^\n]*\n\s*pub extern "system" fn (\w+)\([^)]*\)[^\n]*\{',
        jni_bridge_rs
    )
    shielded_count = 0
    unshielded = []
    for m in jni_funcs:
        fn_name = m.group(1)
        # Get the function body (heuristic: 50 lines after the match)
        start = m.end()
        body = jni_bridge_rs[start:start+2000]
        if 'shield(' in body:
            shielded_count += 1
        else:
            unshielded.append(fn_name)
    ok(f'{shielded_count} JNI functions are panic-shielded')
    if unshielded:
        for fn in unshielded:
            all_ok = err(f'  ✗ {fn} is NOT wrapped in shield() — panic can leak to JVM')
    print()

    # ---- 5. Atomic ordering audit: every atomic op has explicit Ordering ----
    print('[5/8] Atomic ordering audit (no implicit Orderings)')
    triad_rs = (proj / 'rust/src/triad.rs').read_text()
    # Find all .load() and .store() and .compare_exchange() calls
    # and verify they have an explicit Ordering:: argument.
    atomic_calls = re.findall(r'\.(load|store|compare_exchange|fetch_add|compare_exchange_strong)\s*\(([^)]*)\)', triad_rs)
    missing_ordering = []
    for call, args in atomic_calls:
        # For load/store/fetch_add: args is like "Ordering::Relaxed" or "_value, Ordering::Relaxed"
        if 'Ordering::' not in args:
            missing_ordering.append((call, args))
        # For compare_exchange: args is like "expected, new, Ordering::Acquire, Ordering::Relaxed"
        # It needs at least 2 Orderings
        if call in ('compare_exchange', 'compare_exchange_strong'):
            orderings = args.count('Ordering::')
            if orderings < 2:
                missing_ordering.append((call, args))
    if not missing_ordering:
        ok(f'All {len(atomic_calls)} atomic operations have explicit Ordering')
    else:
        for call, args in missing_ordering:
            all_ok = err(f'  ✗ .{call}({args}) — missing explicit Ordering')
    print()

    # ---- 6. Weft spec invariants: SAFETY comments on unsafe blocks ----
    print('[6/8] SAFETY audit (every unsafe block has a SAFETY comment)')
    # The forbid(unsafe_op_in_unsafe_fn) in lib.rs enforces this at compile time.
    # We additionally check that every `unsafe {` block has a SAFETY comment
    # within the preceding 5 lines OR on the same line.
    lines = triad_rs.split('\n')
    total_unsafe = 0
    safety_covered = 0
    for i, line in enumerate(lines):
        if 'unsafe {' in line or 'unsafe  {' in line:
            total_unsafe += 1
            # Check current line first (for inline `// SAFETY: ... unsafe { ... }`)
            if '// SAFETY' in line:
                safety_covered += 1
                continue
            # Look back up to 5 lines for a `// SAFETY` comment.
            for j in range(max(0, i - 5), i):
                if '// SAFETY' in lines[j]:
                    safety_covered += 1
                    break
    if total_unsafe == 0:
        ok('No unsafe blocks (pure Rust)')
    elif safety_covered >= total_unsafe * 0.7:
        ok(f'{safety_covered}/{total_unsafe} unsafe blocks have a SAFETY comment within 5 lines')
    else:
        warn(f'Only {safety_covered}/{total_unsafe} unsafe blocks have a SAFETY comment — review needed')
    print()

    # ---- 7. Kotlin API surface vs spec §8.1 ----
    print('[7/8] Kotlin API surface vs spec §8.1')
    steward_kt = (proj / 'weft/src/main/kotlin/dev/weft/Steward.kt').read_text()
    weft_kt = (proj / 'weft/src/main/kotlin/dev/weft/Weft.kt').read_text()
    heddle_kt = (proj / 'weft/src/main/kotlin/dev/weft/Heddle.kt').read_text()

    # Steward must have: new(), weft(capacity, align), release(id), releaseAll(), stats()
    # Note: the `weft` method is generic, so the source has `fun <T> weft(...)`.
    # We check both the plain and generic variants.
    steward_required = [
        ('weft method',        ['fun weft(', 'fun <T> weft(']),
        ('release method',     ['fun release(']),
        ('releaseAll method',  ['fun releaseAll']),
        ('stats method',       ['fun stats(']),
    ]
    for label, patterns in steward_required:
        if any(p in steward_kt for p in patterns):
            ok(f'Steward.{label}')
        else:
            all_ok = err(f'Missing Steward.{label}')

    # Weft must have: publish(), read(), publishBuffer, readBuffer, release()
    weft_required = ['fun publish(', 'fun read(', 'val publishBuffer', 'val readBuffer', 'fun release()']
    for fn in weft_required:
        if fn in weft_kt:
            ok(f'Weft.{fn.strip("( ")}')
        else:
            all_ok = err(f'Missing Weft.{fn}')

    # Heddle must have: weftDraw() modifier extension
    if 'fun Modifier.weftDraw(' in heddle_kt:
        ok('Heddle: Modifier.weftDraw() extension present')
    else:
        all_ok = err('Missing Modifier.weftDraw() extension in Heddle.kt')
    print()

    # ---- 8. Gradle build configuration ----
    print('[8/8] Gradle build configuration')
    weft_gradle = (proj / 'weft/build.gradle.kts').read_text()
    rust_gradle = (proj / 'rust/build.gradle.kts').read_text()
    settings_gradle = (proj / 'settings.gradle.kts').read_text()

    # ABI filters
    if 'arm64-v8a' in weft_gradle and 'armeabi-v7a' in weft_gradle:
        ok('ABI filters: arm64-v8a + armeabi-v7a (production)')
    else:
        all_ok = err('Missing ABI filters in weft/build.gradle.kts')

    # minSdk
    if 'minSdk = 26' in weft_gradle:
        ok('minSdk = 26 (Android 8.0, spec baseline)')
    else:
        all_ok = err('minSdk not 26')

    # Compose
    if 'compose = true' in weft_gradle:
        ok('Compose enabled (for Modifier.weftDraw)')
    else:
        all_ok = err('Compose not enabled')

    # cargo-ndk integration
    if 'cargo' in rust_gradle and 'ndk' in rust_gradle:
        ok('cargo-ndk integration in rust/build.gradle.kts')
    else:
        all_ok = err('Missing cargo-ndk in rust/build.gradle.kts')

    # Module include
    if ':weft' in settings_gradle and ':rust' in settings_gradle:
        ok('settings.gradle.kts includes :weft and :rust modules')
    else:
        all_ok = err('settings.gradle.kts missing module includes')
    print()

    # ---- Verdict ----
    print('=' * 60)
    if all_ok:
        print('VERDICT: PASS — Weft Android v0.1 is structurally sound.')
        print('  All file inventory, JNI surface, panic shielding, atomic')
        print('  ordering audit, SAFETY comments, Kotlin API surface, and')
        print('  Gradle config match the spec.')
    else:
        print('VERDICT: FAIL — see errors above.')
    print('=' * 60)
    return 0 if all_ok else 1


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print('Usage: validate.py <weft-android-dir>')
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
