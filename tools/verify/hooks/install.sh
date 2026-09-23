#!/bin/sh
# install.sh — wire the weft-verify governance hooks into a git repository.
#
#   sh tools/verify/hooks/install.sh [GIT_DIR]
#
# Default GIT_DIR is <repo>/.git (resolves the repo root from the caller's
# location when invoked from inside the worktree).
set -eu

HOOKS_SRC=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "${1:-}" != "" ]; then
  GIT_DIR_TARGET="$1"
else
  REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || true)
  if [ -z "$REPO_ROOT" ]; then
    echo "install.sh: not inside a git worktree; pass a .git path explicitly" >&2
    exit 2
  fi
  GIT_DIR_TARGET="$REPO_ROOT/.git"
fi

if [ ! -d "$GIT_DIR_TARGET/hooks" ]; then
  echo "install.sh: $GIT_DIR_TARGET/hooks not found (is $GIT_DIR_TARGET a git dir?)" >&2
  exit 2
fi

cp "$HOOKS_SRC/pre-commit-alloc-lint.sh" "$GIT_DIR_TARGET/hooks/pre-commit"
cp "$HOOKS_SRC/pre-push-alloc-lint.sh" "$GIT_DIR_TARGET/hooks/pre-push"
chmod +x "$GIT_DIR_TARGET/hooks/pre-commit" "$GIT_DIR_TARGET/hooks/pre-push"

echo "installed: $GIT_DIR_TARGET/hooks/pre-commit  (fast hot-path alloc gate, <50ms)"
echo "installed: $GIT_DIR_TARGET/hooks/pre-push   (full engine lint over the push range)"
