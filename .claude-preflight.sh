#!/usr/bin/env bash
# .claude-preflight.sh
# -----------------------------------------------------------------------------
# Mandatory preflight for any agent (Claude, human, CI) doing analysis or edits
# against this repo. Hard-fails if the working tree could be stale.
#
# WHY THIS EXISTS
#   2026-05-12 (S34 session): an agent worked an entire session off a stale
#   /tmp clone that was 7 commits behind origin/main, mis-reported that the
#   UstecTrendFollow5m engine did not exist, and almost shipped a logging
#   fix against the wrong tree. This script is the mechanical guard that
#   prevents that exact failure mode from recurring.
#
# WHAT IT CHECKS (in order, fail-fast)
#   1. We are inside a git work tree.
#   2. `git fetch origin --quiet` succeeds (network reachable, creds valid).
#   3. Local `HEAD` == `origin/main` SHA.
#   4. `git status --porcelain` is empty (no uncommitted changes).
#   5. The fetch we just ran was less than PREFLIGHT_FETCH_MAX_AGE_SEC
#      seconds ago (default 60). Belt-and-braces against re-running this
#      script with `--skip-fetch` and getting a stale ref.
#
# USAGE
#   bash .claude-preflight.sh           # full preflight, default settings
#   bash .claude-preflight.sh --strict  # also forbid working in detached HEAD
#   PREFLIGHT_FETCH_MAX_AGE_SEC=300 bash .claude-preflight.sh  # 5 min slack
#
# EXIT CODES
#   0  preflight passed; safe to proceed
#   2  stale tree (local HEAD != origin/main)
#   3  uncommitted changes present
#   4  fetch failed (network / auth)
#   5  fetch timestamp too old (must rerun)
#   6  not inside a git work tree
#   7  detached HEAD under --strict
#
# AGENT CONTRACT
#   No Claude session may run a single grep, Read, Edit, or Bash command
#   against this repo before this preflight returns 0. If the preflight
#   fails, the agent must surface the failure to the operator and refuse
#   to proceed until the tree is brought to a clean origin/main state.
# -----------------------------------------------------------------------------

set -u
set -o pipefail

PREFLIGHT_FETCH_MAX_AGE_SEC="${PREFLIGHT_FETCH_MAX_AGE_SEC:-60}"
STRICT=0
for arg in "$@"; do
    case "$arg" in
        --strict)      STRICT=1 ;;
        --skip-fetch)  SKIP_FETCH=1 ;;
        *)             echo "[PREFLIGHT] unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# ── 1. Are we in a git work tree? ────────────────────────────────────────────
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "[PREFLIGHT-FAIL] not inside a git work tree (cwd=$(pwd))" >&2
    exit 6
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

# ── 2. Fetch origin (unless explicitly skipped) ──────────────────────────────
if [ "${SKIP_FETCH:-0}" -ne 1 ]; then
    if ! git fetch origin --quiet 2>/tmp/preflight_fetch_err; then
        echo "[PREFLIGHT-FAIL] git fetch origin failed:" >&2
        cat /tmp/preflight_fetch_err >&2
        exit 4
    fi
fi

# ── 3. Local HEAD must equal origin/main ─────────────────────────────────────
LOCAL_SHA="$(git rev-parse HEAD)"
REMOTE_SHA="$(git rev-parse origin/main)"

if [ "$STRICT" -eq 1 ]; then
    BRANCH="$(git symbolic-ref --short -q HEAD || true)"
    if [ -z "$BRANCH" ]; then
        echo "[PREFLIGHT-FAIL] strict mode: HEAD is detached at $LOCAL_SHA" >&2
        exit 7
    fi
fi

if [ "$LOCAL_SHA" != "$REMOTE_SHA" ]; then
    echo "[PREFLIGHT-FAIL] tree is not at origin/main" >&2
    echo "    local  HEAD : $LOCAL_SHA" >&2
    echo "    origin/main : $REMOTE_SHA" >&2
    echo "    fix       : git pull --ff-only origin main   (or checkout origin/main)" >&2
    exit 2
fi

# ── 4. No uncommitted changes ────────────────────────────────────────────────
if [ -n "$(git status --porcelain)" ]; then
    echo "[PREFLIGHT-FAIL] uncommitted changes present:" >&2
    git status --short >&2
    echo "    fix : commit, stash, or discard before proceeding" >&2
    exit 3
fi

# ── 5. Fetch was recent ──────────────────────────────────────────────────────
FETCH_HEAD_FILE="$REPO_ROOT/.git/FETCH_HEAD"
if [ -f "$FETCH_HEAD_FILE" ]; then
    NOW_S="$(date +%s)"
    if stat -c '%Y' "$FETCH_HEAD_FILE" >/dev/null 2>&1; then
        FETCH_MTIME="$(stat -c '%Y' "$FETCH_HEAD_FILE")"   # GNU/Linux
    else
        FETCH_MTIME="$(stat -f '%m' "$FETCH_HEAD_FILE")"   # BSD/macOS
    fi
    AGE=$(( NOW_S - FETCH_MTIME ))
    if [ "$AGE" -gt "$PREFLIGHT_FETCH_MAX_AGE_SEC" ]; then
        echo "[PREFLIGHT-FAIL] last fetch is ${AGE}s old (limit ${PREFLIGHT_FETCH_MAX_AGE_SEC}s)" >&2
        echo "    fix : rerun this script without --skip-fetch" >&2
        exit 5
    fi
fi

echo "[PREFLIGHT-OK] tree at $LOCAL_SHA, clean, fetched <${PREFLIGHT_FETCH_MAX_AGE_SEC}s ago"
exit 0
