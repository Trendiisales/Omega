#!/bin/bash
# D7-FIX-2 — single-threaded re-run of the diag binary.
# Tests whether AsianRange centre-combo non-determinism disappears under
# single-thread execution (i.e., whether the bug is in cross-thread time-
# shim races).
#
# Pattern: same bulletproof structure as run_d7diag.sh that produced
# sweep_D7DIAG_20260428_235803/ successfully.
#
# Hypothesis: with --threads 1 (or with the engines invoked sequentially),
# combos 24/73/122/171/220/269 should produce IDENTICAL counter rows in
# asian_diag.tsv. If they do, threading is the bug. If they still diverge,
# bug is inside the AsianRange thread itself and we need a different probe.

set -euo pipefail

REPO="$HOME/omega_repo"
BIN="$REPO/build/OmegaSweepHarness"
TICKS="$HOME/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv"
TS=$(date +%Y%m%d_%H%M%S)
TAG="D7FIX2_ST"
OUTDIR="$REPO/sweep_${TAG}_${TS}"
LOG="$REPO/run_${TAG}_${TS}.log"

# --- pre-flight ---
[ -x "$BIN" ] || { echo "FAIL: binary missing at $BIN" >&2; exit 1; }
[ -f "$TICKS" ] || { echo "FAIL: ticks file missing at $TICKS" >&2; exit 1; }

# Verify diag instrumentation present in binary (same check the prior run uses).
if ! strings "$BIN" 2>/dev/null | grep -q "asian_diag.tsv"; then
    echo "FAIL: binary does not contain diag instrumentation. Rebuild with -DOMEGA_SWEEP_DIAG=ON." >&2
    exit 1
fi

mkdir -p "$OUTDIR"
echo "sentinel pre-launch $(date +%FT%T%z)" > "$OUTDIR/__sentinel_pre.txt"

echo "--- launching diag binary (SINGLE-THREADED) at $(date) ---" | tee "$LOG"

# Single-thread: invoke only the AsianRange engine. Per the harness CLI
# (OmegaSweepHarness.cpp:1329-1340), ticks is POSITIONAL and the flag is
# --engine (singular). engine_in_list treats the value as a comma-separated
# list, so a single value "asianrange" works correctly.
#
# With --engine asianrange, only ONE sweep thread is spawned (the launch
# lambda at OmegaSweepHarness.cpp:1416-1424 only emplaces threads for
# engines in the list). Effectively single-threaded.
#
# If centre combos 24/73/122/171/220/269 produce IDENTICAL counter rows
# under this run, the threading hypothesis is confirmed. If they still
# diverge, the bug is in the AsianRange thread's own per-engine iteration.
"$BIN" \
    "$TICKS" \
    --outdir "$OUTDIR" \
    --engine asianrange \
    --warmup 5000 \
    --verbose \
    2>&1 | tee -a "$LOG"

EXIT_CODE=${PIPESTATUS[0]}
echo "" | tee -a "$LOG"
echo "--- Exit code: $EXIT_CODE (wall: ${SECONDS}s) ---" | tee -a "$LOG"
echo "" | tee -a "$LOG"

# --- post-run verification ---
echo "--- Post-run verification ---" | tee -a "$LOG"
ls -la "$OUTDIR" | tee -a "$LOG"
echo "" | tee -a "$LOG"

# Confirm asian_diag.tsv exists and is non-empty.
if [ -s "$OUTDIR/asian_diag.tsv" ]; then
    echo "--- ASIAN DIAG DUMP (single-threaded run) ---" | tee -a "$LOG"
    cat "$OUTDIR/asian_diag.tsv" | tee -a "$LOG"
    echo "" | tee -a "$LOG"

    # The decisive test: are the 6 centre combos' counters all identical?
    echo "--- Centre-combo determinism check ---" | tee -a "$LOG"
    # Pull rows for combos 24, 73, 122, 171, 220, 269 and check whether the
    # six rows in columns 7..17 (the counters) are byte-identical.
    awk -F'\t' '
        NR==1 { next }
        $1==24 || $1==73 || $1==122 || $1==171 || $1==220 || $1==269 {
            # Build a fingerprint of all counter columns (7..17).
            fp = $7"|"$8"|"$9"|"$10"|"$11"|"$12"|"$13"|"$14"|"$15"|"$16"|"$17
            print "  combo "$1" fp="fp
            seen[fp]++
            count++
        }
        END {
            n_unique = 0
            for (k in seen) n_unique++
            print "  total rows checked: "count
            print "  unique counter fingerprints: "n_unique
            if (n_unique == 1) {
                print "  RESULT: DETERMINISTIC -- threading was the bug"
            } else {
                print "  RESULT: STILL DIVERGES under single-thread -- bug is inside AsianRange tuple-iteration, not cross-thread"
            }
        }
    ' "$OUTDIR/asian_diag.tsv" | tee -a "$LOG"
else
    echo "FAIL: asian_diag.tsv is missing or empty" | tee -a "$LOG"
    exit 1
fi

echo "" | tee -a "$LOG"
echo "--- Done ---" | tee -a "$LOG"
echo "Log:    $LOG"
echo "Outdir: $OUTDIR"
