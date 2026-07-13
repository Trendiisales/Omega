#!/usr/bin/env bash
# HARD DATA GATE — the ONLY sanctioned way to put tick/bar data into a backtest harness.
#
# Why this exists (2026-07-13): the silver up-jump backtest reported +1645% "CERTIFIED z=5"
# on data the integrity gate REJECTED (111 x1000-glitch ticks). A subagent saw "REJECTED",
# decided it was "a real squeeze / false-positive precedent", OVERRODE it, and ran anyway.
# The operator (rightly) called it shit data. The integrity gate was already correct
# (exit 1 on reject) — the hole was that nothing STOPPED a rejected file from being used.
#
# This wrapper closes that hole: it runs data_integrity_gate.py on the SOURCE and copies to
# the harness ONLY on exit 0 (CERTIFIED CLEAN). A REJECTED verdict is TERMINAL — there is NO
# --force, NO override, NO "false-positive precedent" bypass. If you believe a reject is a
# false positive, FIX THE DATA (de-glitch / re-pull) until it passes; do not bypass the gate.
#
# Usage:  bash backtest/stage_certified_data.sh <src.csv> <dest.csv>
#   e.g.  bash backtest/stage_certified_data.sh /Users/jo/Tick/XAUUSD_2022_2026.h1.csv \
#             /Users/jo/Crypto/backtest/data/GOLDUSDT_1h.csv
# On success it also writes <dest.csv>.certified (source path + sha) so a harness can verify.
set -euo pipefail

GATE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/data_integrity_gate.py"
SRC="${1:?usage: stage_certified_data.sh <src.csv> <dest.csv>}"
DEST="${2:?usage: stage_certified_data.sh <src.csv> <dest.csv>}"

if [[ ! -f "$SRC" ]]; then echo "STAGE-GATE: source not found: $SRC" >&2; exit 1; fi

echo "STAGE-GATE: integrity-gating $SRC ..." >&2
if ! python3 "$GATE" "$SRC" >&2; then
    echo "" >&2
    echo "STAGE-GATE: ❌ REJECTED — $SRC failed the integrity gate. REFUSING to stage it." >&2
    echo "STAGE-GATE: this verdict is TERMINAL. No override. Fix the data (de-glitch/re-pull)" >&2
    echo "STAGE-GATE: until it passes, or use a different clean source. NOT backtesting on it." >&2
    exit 2
fi

mkdir -p "$(dirname "$DEST")"
cp "$SRC" "$DEST"
SHA=$(shasum -a 256 "$SRC" | awk '{print $1}')
printf 'source=%s\nsha256=%s\nverdict=CERTIFIED_CLEAN\n' "$SRC" "$SHA" > "$DEST.certified"
echo "STAGE-GATE: ✅ CERTIFIED CLEAN — staged $SRC -> $DEST (+ .certified stamp)" >&2
