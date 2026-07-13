#!/usr/bin/env bash
# DEPLOY-DRIFT GUARD — RED when origin/main has commits the running box binary hasn't got.
#
# Why (2026-07-13, recurring): `git commit`+`push` update ORIGIN; `tools/omega_deploy.sh` is a
# SEPARATE step that updates the BOX binary. When an interruption or a wrong "this doesn't need
# a deploy" judgement drops the deploy, origin advances while the running binary lags — and the
# divergence is INVISIBLE. This session that left 5 oversized bracket engines live ~40min after
# their disable was already committed. CLAUDE.md documents the 2026-05-14 version of the same
# class. This guard compares the RUNNING BINARY's baked Git hash to origin/main and REDs on any
# undeployed engine/config commit, listing exactly what's not live.
#
# Usage:  bash tools/deploy_drift_check.sh            # exit 0 = in sync, 1 = drift (undeployed commits)
set -uo pipefail

HOST="${VPS_HOST:-omega-new}"
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 3

git fetch origin main --quiet 2>/dev/null || { echo "DEPLOY-DRIFT: git fetch failed"; exit 3; }
ORIGIN=$(git rev-parse --short origin/main)

# The running binary bakes its Git hash into the boot stderr line "[Omega] Git hash: <hash>".
BOX=$(ssh "$HOST" "powershell -NoProfile -Command \"(Select-String -Path C:\\Omega\\logs\\omega_service_stderr.log -Pattern 'Git hash:' | Select-Object -Last 1).Line\"" 2>/dev/null \
      | grep -oE '[0-9a-f]{7,40}' | head -1)

if [[ -z "$BOX" ]]; then
    echo "DEPLOY-DRIFT: ❓ could not read running-binary hash from $HOST (ssh/boot-line) — cannot verify. Treat as RED."
    exit 1
fi

# normalize to comparable short length
BOXN="${BOX:0:7}"
if git merge-base --is-ancestor "$BOX" origin/main 2>/dev/null && [[ "$BOXN" != "${ORIGIN:0:7}" ]]; then
    # Only build-relevant paths require a box redeploy. Tooling/docs/backtest commits run
    # from the Mac repo and are NEVER baked into the box binary -> not drift (avoid crying
    # wolf on every script/doc commit = alarm fatigue). Build inputs = include/, src/, CMake.
    BUILD_CHANGES=$(git diff --name-only "${BOX}..origin/main" 2>/dev/null \
        | grep -E '^(include/|src/|CMakeLists\.txt|cmake/|.*\.hpp$|.*\.cpp$)' || true)
    if [[ -z "$BUILD_CHANGES" ]]; then
        echo "DEPLOY-DRIFT: ✅ in sync (binary-wise) — box $BOXN behind origin $ORIGIN by tooling/docs only, no build inputs changed."
        exit 0
    fi
    N=$(printf '%s\n' "$BUILD_CHANGES" | wc -l | tr -d ' ')
    echo "DEPLOY-DRIFT: ❌ RED — running binary $BOXN is BEHIND origin/main $ORIGIN with $N undeployed BUILD file(s):"
    printf '%s\n' "$BUILD_CHANGES" | sed 's/^/    /'
    echo "  Undeployed commits touching the binary:"
    git log --oneline "${BOX}..origin/main" -- include/ src/ CMakeLists.txt 2>/dev/null | sed 's/^/    /'
    echo "  -> run: bash tools/omega_deploy.sh   (then re-check)"
    exit 1
fi

if [[ "$BOXN" == "${ORIGIN:0:7}" ]]; then
    echo "DEPLOY-DRIFT: ✅ in sync — running binary $BOXN == origin/main $ORIGIN."
    exit 0
fi

# box hash not an ancestor of origin/main => box ahead / detached / unknown build -> flag it
echo "DEPLOY-DRIFT: ⚠️  running binary $BOXN is NOT an ancestor of origin/main $ORIGIN"
echo "  (box ahead, detached, or built out-of-band — CLAUDE.md build-path discipline). Investigate."
exit 1
