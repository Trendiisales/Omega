#!/bin/sh
# Install repo-versioned git hooks into .git/hooks (idempotent).
# git does not version .git/hooks, so a fresh clone must run this once:
#   sh scripts/git_hooks/install.sh
# Currently installs: pre-commit (silent-partial-stage tripwire, S-2026-07-14ap).
# NOTE: .git/hooks/post-commit (graphify) and post-checkout are owned by
# tools/graphify/install-automation.sh — NOT touched here.
set -e
ROOT=$(git rev-parse --show-toplevel)
HOOKS_DIR="$ROOT/.git/hooks"
SRC="$ROOT/scripts/git_hooks/pre-commit"

if [ -f "$HOOKS_DIR/pre-commit" ] && ! grep -q 'silent-partial-stage tripwire' "$HOOKS_DIR/pre-commit"; then
    echo "[install_git_hooks] EXISTING FOREIGN pre-commit at $HOOKS_DIR/pre-commit -- not overwriting." >&2
    echo "[install_git_hooks] Merge scripts/git_hooks/pre-commit into it manually." >&2
    exit 1
fi
cp "$SRC" "$HOOKS_DIR/pre-commit"
chmod +x "$HOOKS_DIR/pre-commit"
echo "[install_git_hooks] pre-commit installed -> $HOOKS_DIR/pre-commit"
