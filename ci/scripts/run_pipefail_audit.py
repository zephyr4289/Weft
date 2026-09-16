#!/usr/bin/env python3
"""
ci/scripts/run_pipefail_audit.py — the no-silent-green guard.

WHY EXISTS (the lead's gate: "no silent-green, set -o pipefail everywhere"):
a pipeline's exit status is its LAST command unless pipefail is set. The
tree already died once from this class: `set -e` steps that piped into tee
swallowed the build's failure (the pipefail-resurrection commits). This
audit makes the whole discipline mechanical and permanent:

  1. Every ci/scripts/*.sh must carry `set -euo pipefail` (or an explicit
     `set -o pipefail`).
  2. Every workflow `run:` block that contains a PIPELINE (`cmd | cmd`)
     must establish pipefail (`set -o pipefail`, `set -euo pipefail`) —
     otherwise a failure upstream of tee/head/tail/grep goes green.
  3. Workflows pinned to `shell: bash` default are still checked — the
     default GitHub bash shell runs with `set +e`-ish semantics for
     pipelines exactly like a login shell would.

Exit 0 only when every script and every pipeline-bearing run block is
covered. Findings name the file, the step, and the fix.
"""
import re
import sys
from pathlib import Path

# This file lives at ci/scripts/ — three levels up is the repo root. Assert
# the layout loudly: an audit that finds nothing is a lied audit.
ROOT = Path(__file__).resolve().parent.parent.parent
if not (ROOT / 'ci' / 'scripts').is_dir() or not (ROOT / '.github' / 'workflows').is_dir():
    print('FATAL: run_pipefail_audit.py must live at <repo>/ci/scripts/; '
          f'resolved ROOT={ROOT} does not look like the repo root.')
    sys.exit(2)

fail = 0

# ---------------------------------------------------------------------------
# 1. Shell shards: pipefail mandatory.
# ---------------------------------------------------------------------------
print("=== ci/scripts/*.sh: set -euo pipefail required ===")
for sh in sorted((ROOT / 'ci' / 'scripts').glob('*.sh')):
    src = sh.read_text()
    has = ('set -euo pipefail' in src) or ('set -o pipefail' in src) or \
          ('set -eo pipefail' in src)
    mark = 'ok ' if has else 'RED'
    print(f"  [{mark}] {sh.name}")
    if not has:
        fail += 1
        print(f"        FIX: add 'set -euo pipefail' near the top of {sh.name}")

# ---------------------------------------------------------------------------
# 2. Workflows: pipeline-bearing run blocks must establish pipefail.
# ---------------------------------------------------------------------------
print("=== .github/workflows/*.yml: pipeline run blocks need pipefail ===")

# A real pipeline: a SINGLE '|' not part of '||' (the OR-operator's
# explicit "ignore failure" is a declared decision, not silent-green).
PIPE_RE = re.compile(r'(?<![|])\|\s*\w[^\n]*(?<!\|)')
SET_PF = re.compile(r'set\s+(?:-[a-z]+\s+)*-o\s+pipefail|set\s+-[a-z]*e[a-z]*u[a-z]*o\s+pipefail')

wf_root = ROOT / '.github' / 'workflows'
for wf in sorted(wf_root.glob('*.yml')):
    lines = wf.read_text().splitlines()
    # Walk the file; a "run:" key starts a (block|inline) scalar. Track the
    # enclosing step name (nearest preceding 'name:' at deeper indent) for
    # actionable findings.
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r'^(\s*)(?:-\s+)?run:\s*(.*)$', line)
        if not m:
            i += 1
            continue
        indent = len(m.group(1))
        inline = m.group(2).strip()
        # step label: nearest preceding line with a name: at >= indent
        label = ''
        for j in range(i - 1, max(0, i - 12), -1):
            nm = re.match(r'^\s*(?:-\s+)?name:\s*(.+)$', lines[j])
            if nm:
                label = nm.group(1).strip()
                break
        # Collect the block scalar (or the inline remainder)
        block_lines = []
        if inline and inline not in ('|', '>-', '>', '|-'):
            block_lines = [inline]
            i += 1
        else:
            i += 1
            while i < len(lines):
                nxt = lines[i]
                if nxt.strip() == '' or nxt.strip().startswith('#'):
                    block_lines.append(nxt)
                    i += 1
                    continue
                ni = len(nxt) - len(nxt.lstrip())
                if ni > indent:
                    block_lines.append(nxt)
                    i += 1
                else:
                    break
        block = '\n'.join(block_lines)
        # Any real pipeline in the block must sit under pipefail.
        if PIPE_RE.search(block):
            if not SET_PF.search(block):
                print(f"  [RED] {wf.name} — step '{label or '<unnamed>'}' "
                      f"has a pipeline without set -o pipefail")
                # show the offending pipeline lines
                for bl in block_lines:
                    if PIPE_RE.search(bl):
                        print(f"        {bl.strip()[:110]}")
                fail += 1
            else:
                print(f"  [ok ] {wf.name} — '{label or '<unnamed>'}' pipelines are pipefail-covered")

if fail:
    print()
    print(f"❌ SILENT-GREEN AUDIT: {fail} finding(s). Pipelines swallow failures "
          "without pipefail — the exact class that already broke this repo once.")
    sys.exit(1)
print()
print("✅ SILENT-GREEN AUDIT: every shard carries pipefail; every pipeline "
      "in every workflow run block is covered.")
