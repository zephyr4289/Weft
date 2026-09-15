#!/usr/bin/env python3
"""
tools/validate_catalog.py — schema check for litmus/catalog.yaml.

Per 05-CONTRACTS §3. The driver refuses to run against an invalid catalog.

Checks:
  1. Required top-level keys: catalog, version, defaults, ordering_matrix, tests
  2. defaults has payload_max and seed
  3. ordering_matrix keys are exactly: latest, revoked, epoch, telemetry, t_publish_reads
  4. Each ordering_matrix entry has the subkeys specified in 05-CONTRACTS §3
  5. tests has exactly 8 entries with the required IDs in order
  6. Each test has id, adversary, verdict, params
  7. Each test's params match the catalog param table in 05-CONTRACTS §1
  8. No unknown top-level keys

Usage: python3 tools/validate_catalog.py path/to/catalog.yaml
Exit: 0 valid · 1 invalid · 2 usage error
"""
import sys
import re

# YAML not available in stdlib; we use a minimal parser inline because the catalog
# is structurally simple. If we had PyYAML available we'd use it. (We do — but
# pulling it in for ~60 lines is overkill.)
try:
    import yaml
    HAVE_YAML = True
except ImportError:
    HAVE_YAML = False
    # Minimal hand-rolled parser for the catalog. Handles only the subset the
    # catalog uses: scalars, lists, dicts, comments, hex literals.
    pass

# ---------------------------------------------------------------------------
# Required schema (per 05-CONTRACTS §3)
# ---------------------------------------------------------------------------

REQUIRED_TOP = {"catalog", "version", "defaults", "ordering_matrix", "tests"}
ORDERING_KEYS = {"latest", "revoked", "epoch", "telemetry", "t_publish_reads"}
REQUIRED_TEST_IDS = [
    "L1-tear", "L2-writer-steps", "L3-reader-steps", "L4-freshness",
    "L5-progress", "L6-ownership", "L7-revocation", "L8-envelope",
]

# Per-test param set (05-CONTRACTS §1, v1.1: L1 gains min_claims)
TEST_PARAMS = {
    "L1-tear":         {"holds_ms", "writer_hz", "frames", "payload_max", "min_claims"},
    "L2-writer-steps": {"holds_ms", "publishes", "writer_hz", "payload_max", "bound"},
    "L3-reader-steps": {"writer_hz", "claims", "payload_max", "bound"},
    "L4-freshness":    {"writer_hz", "reader_hz", "frames", "payload_max"},
    "L5-progress":     {"holds_ms", "window_s", "writer_hz", "payload_max", "tolerance"},
    "L6-ownership":    {"trials", "frames", "max_delay_us", "payload_max", "seed"},
    "L7-revocation":   {"timeout_ms", "payload_max"},
    "L8-envelope":     set(),
}

# Ordering matrix subkey check (05-CONTRACTS §3 schema)
ORDERING_SUBKEYS = {
    "latest":         {"op", "ordering", "note"},
    "revoked":        {"writer_load", "releaser_store", "note"},
    "epoch":          {"writer_ack", "reclaim_poll", "note"},
    "telemetry":      {"op", "ordering", "note"},
    "t_publish_reads":{"op", "ordering", "note"},
}

# ---------------------------------------------------------------------------
# Minimal YAML parser (used only if PyYAML not available)
# ---------------------------------------------------------------------------

def mini_yaml(text):
    """Parse a YAML subset into Python objects. Handles:
    - comments (# to end of line, but not inside quotes)
    - key: value (scalar)
    - key: (nested block on next lines with deeper indent)
    - list items with "- " prefix
    - inline lists [a, b, c]
    - inline dicts {a: 1, b: 2} (not used in catalog but supported)
    - hex literals 0x...
    - integers, floats, strings (bare and quoted)
    """
    lines = text.split('\n')
    # Strip comments and blank lines, but keep indentation
    clean = []
    for ln in lines:
        # Strip trailing comment (naive — catalog has no # inside quotes)
        # Find first # that is not inside a string
        in_str = False
        quote_ch = None
        idx = -1
        for i, ch in enumerate(ln):
            if in_str:
                if ch == quote_ch: in_str = False
            else:
                if ch in ('"', "'"):
                    in_str = True; quote_ch = ch
                elif ch == '#':
                    idx = i; break
        if idx >= 0: ln = ln[:idx]
        if ln.strip(): clean.append(ln.rstrip())
    # Recursive descent
    pos = [0]
    def parse(indent):
        result = None
        while pos[0] < len(clean):
            ln = clean[pos[0]]
            # Compute indent
            cur = len(ln) - len(ln.lstrip())
            if cur < indent:
                return result
            stripped = ln.lstrip()
            if cur > indent and result is None:
                # Shouldn't happen at top of a block
                pos[0] += 1
                continue
            if cur > indent:
                # We shouldn't reach here normally — handled by recursion
                pos[0] += 1
                continue
            if stripped.startswith('- '):
                # List item
                if result is None: result = []
                item_str = stripped[2:]
                if ':' in item_str and not item_str.startswith('{'):
                    # Inline dict starting on this line — could be a multi-line dict
                    # Treat as the first key of a new dict; consume subsequent lines
                    pos[0] += 1
                    sub = parse(cur + 2)  # parse the block under this item
                    # First line had "key: value" which is part of the dict
                    # Re-parse by merging
                    inline_dict = {}
                    k, _, v = item_str.partition(':')
                    inline_dict[k.strip()] = parse_scalar(v.strip())
                    if isinstance(sub, dict):
                        inline_dict.update(sub)
                    result.append(inline_dict)
                else:
                    pos[0] += 1
                    result.append(parse_scalar(item_str))
            elif ':' in stripped:
                # Dict entry
                if result is None: result = {}
                k, _, v = stripped.partition(':')
                k = k.strip(); v = v.strip()
                pos[0] += 1
                if v == '':
                    # Nested block
                    sub = parse(cur + 2)
                    result[k] = sub
                else:
                    result[k] = parse_scalar(v)
            else:
                pos[0] += 1
        return result

    def parse_scalar(s):
        if not s: return ''
        # Inline list
        if s.startswith('[') and s.endswith(']'):
            inner = s[1:-1]
            return [parse_scalar(p.strip()) for p in inner.split(',')]
        # Inline dict
        if s.startswith('{') and s.endswith('}'):
            inner = s[1:-1]
            d = {}
            for part in inner.split(','):
                k, _, v = part.partition(':')
                d[k.strip()] = parse_scalar(v.strip())
            return d
        # Hex literal
        if re.match(r'^-?0x[0-9a-fA-F]+$', s):
            return int(s, 16)
        # Integer
        if re.match(r'^-?\d+$', s):
            return int(s)
        # Float
        if re.match(r'^-?\d+\.\d+$', s):
            return float(s)
        # Quoted string
        if (s.startswith('"') and s.endswith('"')) or (s.startswith("'") and s.endswith("'")):
            return s[1:-1]
        # Bare string
        return s

    return parse(0)

# ---------------------------------------------------------------------------
# Validator
# ---------------------------------------------------------------------------

def validate(catalog_path):
    with open(catalog_path, 'r') as f:
        text = f.read()

    if HAVE_YAML:
        cat = yaml.safe_load(text)
    else:
        cat = mini_yaml(text)

    errors = []

    # 1. Required top-level keys
    if not isinstance(cat, dict):
        return [f"Top-level must be a mapping, got {type(cat).__name__}"]
    top_keys = set(cat.keys())
    missing = REQUIRED_TOP - top_keys
    if missing:
        errors.append(f"Missing top-level keys: {sorted(missing)}")
    unknown = top_keys - REQUIRED_TOP
    if unknown:
        errors.append(f"Unknown top-level keys: {sorted(unknown)}")

    # 2. defaults
    if "defaults" in cat:
        d = cat["defaults"]
        if not isinstance(d, dict):
            errors.append("defaults must be a mapping")
        else:
            if "payload_max" not in d:
                errors.append("defaults missing payload_max")
            if "seed" not in d:
                errors.append("defaults missing seed")

    # 3. ordering_matrix keys exact
    if "ordering_matrix" in cat:
        om = cat["ordering_matrix"]
        if not isinstance(om, dict):
            errors.append("ordering_matrix must be a mapping")
        else:
            om_keys = set(om.keys())
            missing_om = ORDERING_KEYS - om_keys
            if missing_om:
                errors.append(f"ordering_matrix missing keys: {sorted(missing_om)}")
            extra_om = om_keys - ORDERING_KEYS
            if extra_om:
                errors.append(f"ordering_matrix has unknown keys: {sorted(extra_om)}")
            # Check subkeys
            for key, required_subkeys in ORDERING_SUBKEYS.items():
                if key in om and isinstance(om[key], dict):
                    actual_subkeys = set(om[key].keys())
                    missing_sub = required_subkeys - actual_subkeys
                    if missing_sub:
                        errors.append(f"ordering_matrix.{key} missing subkeys: {sorted(missing_sub)}")

    # 4. tests: exactly 8 with required IDs in order
    if "tests" in cat:
        tests = cat["tests"]
        if not isinstance(tests, list):
            errors.append("tests must be a list")
        else:
            if len(tests) != 8:
                errors.append(f"Expected 8 tests, got {len(tests)}")
            for i, t in enumerate(tests):
                if not isinstance(t, dict):
                    errors.append(f"tests[{i}] must be a mapping")
                    continue
                # Required fields
                for f in ("id", "adversary", "verdict", "params"):
                    if f not in t:
                        errors.append(f"tests[{i}] missing required field '{f}'")
                # ID check (order-sensitive)
                if i < len(REQUIRED_TEST_IDS):
                    expected_id = REQUIRED_TEST_IDS[i]
                    if t.get("id") != expected_id:
                        errors.append(f"tests[{i}].id = '{t.get('id')}' (expected '{expected_id}')")
                # Adversary and verdict non-empty
                if isinstance(t.get("adversary"), str) and not t["adversary"].strip():
                    errors.append(f"tests[{i}].adversary is empty")
                if isinstance(t.get("verdict"), str) and not t["verdict"].strip():
                    errors.append(f"tests[{i}].verdict is empty")
                # Params match the catalog table
                tid = t.get("id")
                if tid in TEST_PARAMS:
                    actual_params = set(t.get("params", {}).keys()) if isinstance(t.get("params"), dict) else set()
                    expected_params = TEST_PARAMS[tid]
                    missing_p = expected_params - actual_params
                    if missing_p:
                        errors.append(f"{tid} missing params: {sorted(missing_p)}")
                    extra_p = actual_params - expected_params
                    if extra_p:
                        errors.append(f"{tid} has unknown params: {sorted(extra_p)}")
                    # v1.1 (WO-P0A A4): min_claims, when present, is a map with
                    # exactly keys c/rust/ts and positive integer values.
                    if "min_claims" in actual_params:
                        mc = t["params"]["min_claims"]
                        if not isinstance(mc, dict):
                            errors.append(f"{tid}.min_claims must be a mapping, got {type(mc).__name__}")
                        else:
                            mc_keys = set(mc.keys())
                            expected_mc_keys = {"c", "rust", "ts"}
                            mc_missing = expected_mc_keys - mc_keys
                            if mc_missing:
                                errors.append(f"{tid}.min_claims missing keys: {sorted(mc_missing)}")
                            mc_extra = mc_keys - expected_mc_keys
                            if mc_extra:
                                errors.append(f"{tid}.min_claims has unknown keys: {sorted(mc_extra)}")
                            for k, v in mc.items():
                                if not isinstance(v, int) or v <= 0:
                                    errors.append(f"{tid}.min_claims.{k} must be a positive int, got {v!r}")

    return errors

def main():
    if len(sys.argv) < 2:
        print("Usage: validate_catalog.py <catalog.yaml> [--kind litmus|bench]", file=sys.stderr)
        return 2
    catalog_path = sys.argv[1]
    kind = "litmus"
    if "--kind" in sys.argv:
        idx = sys.argv.index("--kind")
        if idx + 1 < len(sys.argv):
            kind = sys.argv[idx + 1]
    if kind == "bench":
        errs = validate_bench(catalog_path)
    else:
        errs = validate(catalog_path)
    if errs:
        print(f"INVALID catalog ({kind}): {catalog_path}", file=sys.stderr)
        for e in errs:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print(f"VALID catalog ({kind}): {catalog_path}")
    return 0

def validate_bench(catalog_path):
    """Validate a bench catalog per WO-P1 §3."""
    with open(catalog_path, 'r') as f:
        text = f.read()
    import yaml
    cat = yaml.safe_load(text)
    errors = []
    if not isinstance(cat, dict):
        return ["Top-level must be a mapping"]
    # Required keys
    for k in ("catalog", "version", "benchmarks"):
        if k not in cat:
            errors.append(f"Missing top-level key: {k}")
    if "benchmarks" not in cat:
        return errors
    bench = cat["benchmarks"]
    if not isinstance(bench, list):
        errors.append("benchmarks must be a list")
        return errors
    REQUIRED_BENCH_IDS = [
        "B1-pub-throughput", "B2-contended", "B3-scaling-fingerprint",
        "B4-display-adversarial", "B5-memory-contract",
    ]
    if len(bench) != 5:
        errors.append(f"Expected 5 benchmarks, got {len(bench)}")
    for i, b in enumerate(bench):
        if not isinstance(b, dict):
            errors.append(f"benchmarks[{i}] must be a mapping")
            continue
        for f in ("id", "question", "setup", "metrics", "params"):
            if f not in b:
                errors.append(f"benchmarks[{i}] missing required field '{f}'")
        if i < len(REQUIRED_BENCH_IDS):
            expected_id = REQUIRED_BENCH_IDS[i]
            if b.get("id") != expected_id:
                errors.append(f"benchmarks[{i}].id = '{b.get('id')}' (expected '{expected_id}')")
        if isinstance(b.get("question"), str) and not b["question"].strip():
            errors.append(f"benchmarks[{i}].question is empty")
        if isinstance(b.get("gate"), str) and b["gate"] not in ("informational", "structural"):
            errors.append(f"benchmarks[{i}].gate must be 'informational' or 'structural'")
    return errors

if __name__ == "__main__":
    sys.exit(main())
