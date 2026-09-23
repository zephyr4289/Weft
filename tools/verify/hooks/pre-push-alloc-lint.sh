#!/bin/sh
# pre-push-alloc-lint.sh — thorough pre-push hot-path allocation gate.
#
# Unlike the <50ms pre-commit hook, this one runs the full @weft/verify
# engine over every changed source file in the push range (latency not
# bounded; it runs before your commits leave the machine).
set -u

REMOTE="$1"; URL="$2" # git pre-push protocol

RANGE=""
if git rev-parse --verify -q "HEAD" >/dev/null; then
  # best effort: files introduced by commits not yet on any remote tracking branch
  UPSTREAM=$(git for-each-ref --format='%(refname:short)' refs/remotes 2>/dev/null | head -1)
  if [ -n "$UPSTREAM" ]; then
    RANGE=$(git diff --name-only "$UPSTREAM"..HEAD 2>/dev/null || true)
  fi
fi

# Collect staged+committed source files; fall back to the managed source tree.
if [ -z "$RANGE" ]; then
  FILES=$(git diff --name-only HEAD 2>/dev/null || true)
else
  FILES="$RANGE"
fi
SRC=$(echo "$FILES" | grep -E '\.(ts|tsx|mts|cts|js|mjs|cjs|jsx|c|h|cc|cpp|cxx|hpp|hh|rs|swift|dart)$' || true)
[ -z "$SRC" ] && exit 0

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
ARGS=""
for f in $SRC; do
  [ -f "$f" ] && ARGS="$ARGS $f"
done
[ -z "$ARGS" ] && exit 0

exec "$ROOT/tools/verify/weft-verify" --lint-alloc $ARGS
