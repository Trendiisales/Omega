#!/bin/bash
# check_branch_freshness.sh -- session-start guard against stale working branches.
#
# Lesson learned 2026-05-20: an AI session worked for hours on s44-bt-validation,
# which had diverged from origin/main by 201 commits (S50 X3 purge, audit-fixes
# 32-42, FX cohort wiring, MacroCrash, ACTIVE_SYMBOLS_GATE, etc.). The session
# was unaware -- it thought engines retired on main (TSMomGold, PullbackCont)
# still existed, and added wiring that conflicted with main. Final commit had to
# be cherry-picked + manually resolved.
#
# This script catches that case at session start:
#   - Fetches origin/main (read-only, no merge).
#   - Reports how stale HEAD is relative to origin/main.
#   - Exits non-zero if HEAD is behind origin/main by >= STALE_THRESHOLD
#     commits, blocking automated workflows from proceeding without
#     operator acknowledgment.
#
# Override with --force or environment STALE_OK=1.
#
# Usage:
#   bash tools/check_branch_freshness.sh
#   STALE_OK=1 bash tools/check_branch_freshness.sh        # bypass
#   bash tools/check_branch_freshness.sh --force           # bypass

set -e

STALE_THRESHOLD=${STALE_THRESHOLD:-25}

if [[ "$1" == "--force" || "${STALE_OK:-}" == "1" ]]; then
    echo "[check_branch_freshness] override active, skipping check"
    exit 0
fi

cd "$(dirname "$0")/.."

BRANCH=$(git rev-parse --abbrev-ref HEAD)
echo "[check_branch_freshness] current branch: $BRANCH"

# Fetch origin/main without modifying any branch
git fetch origin main --quiet 2>/dev/null || {
    echo "[check_branch_freshness] WARN: could not fetch origin/main (no network?). Proceeding."
    exit 0
}

AHEAD=$(git rev-list --count origin/main..HEAD 2>/dev/null || echo 0)
BEHIND=$(git rev-list --count HEAD..origin/main 2>/dev/null || echo 0)

echo "[check_branch_freshness] HEAD: $AHEAD ahead, $BEHIND behind origin/main"

if [[ "$BEHIND" -ge "$STALE_THRESHOLD" ]]; then
    cat <<EOF

================================================================================
  STALE BRANCH WARNING -- $BEHIND commits behind origin/main (threshold $STALE_THRESHOLD)
================================================================================

  Current branch '$BRANCH' has not pulled origin/main in a long time.
  Working on it WILL cause merge conflicts and may resurrect engines that
  main has retired (see 2026-05-20 incident: TSMomGold / PullbackCont).

  Resolve before proceeding:

    git fetch origin
    git rebase origin/main         # if your work is short
    OR
    git merge origin/main          # if your work is large + you want a merge commit

  After rebase/merge, rebuild and re-run rigor harness to verify nothing
  broke in the integration.

  To bypass once (you know what you are doing):
    STALE_OK=1 bash tools/check_branch_freshness.sh

================================================================================
EOF
    exit 1
fi

echo "[check_branch_freshness] OK"
exit 0
