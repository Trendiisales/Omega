#!/usr/bin/env bash
# =============================================================================
# vrev_sweep_t1_p2.sh -- Tier 1 Phase 2 session-window refinement for VWR USTEC.F
# =============================================================================
# Refines the only Phase 1 winning axis -- session window. Phase 1 produced
# two positive interior cells against a negative baseline:
#   (open=10, close=21) -> avg_pnl=+0.002067, trades=4651
#   (open=13, close=21) -> avg_pnl=+0.003306, trades=3731  (Phase 1 winner)
# vs default (8,22): avg_pnl=-0.001473, trades=4943.
#
# This Phase 2 maps the boundary of the positive region with 14 cells in
# three stages:
#
#   Stage 2a: 1D open-hour sweep at close=21 (7 cells).
#             Brackets the (13,21) winner. (10,21) and (13,21) double as
#             Phase 1 cross-run consistency checks -- they should produce
#             byte-identical metrics to the Phase 1 cells.
#
#   Stage 2b: 1D close-hour sweep at open=13 (4 cells).
#             Maps the transition from (13,17) Phase-1 fail through to
#             (13,22). (13,21) is in Stage 2a so excluded here.
#
#   Stage 2c: Off-diagonal 2D probe (3 cells).
#             Tests whether the positive region is an island around
#             (13,21) or extends off-axis. Cells: (12,20), (11,22), (14,20).
#
# Plan reference:
#   outputs/VWR_USTEC_TIER1_PHASE1_RESULTS_2026-05-14.md §5
#   docs/handoffs/SESSION_HANDOFF_2026-05-14g.md §"Tier 1 sweep"
#   outputs/VWR_USTEC_STRUCTURAL_REWORK_SCOPING_2026-05-14f.md §5 (WF gate)
#
# Predecessor: scripts/vrev_sweep_t1_p1.sh (S71 Phase 1, 30-cell univariate).
# Mode: --mode baseline (S63 trio zeroed), matching Phase 1 protocol.
#
# Wall-clock estimate: 14 * ~28s = ~6.5 min.
#
# Usage:
#   scripts/vrev_sweep_t1_p2.sh <tape.csv> [output_dir]
#
# Args:
#   tape.csv     Path to the tick CSV. Recommended:
#                  /Users/jo/Tick/NSXUSD_merged.csv  (matches Phase 1 tape
#                                                     for like-for-like
#                                                     comparison)
#   output_dir   Optional. Default: outputs/vrev_t1_p2_<timestamp>/.
#
# Output structure:
#   <output_dir>/
#     phase2_summary.csv          One row per cell + header. Columns:
#                                   stage, open_h, close_h, trades, wins,
#                                   win_rate_pct, gross_pnl, avg_pnl,
#                                   n_tp_hit, n_sl_hit, n_timeout,
#                                   n_mae_early_exit, worst_trade,
#                                   p95_worst_loss
#                                 (Tier 1 echoed-config columns are dropped
#                                 -- redundant for this sweep since only
#                                 session_open/close vary.)
#     cells/
#       2a_open_09_report.csv
#       2b_close_18_report.csv
#       2c_off_12x20_report.csv
#       ...                       (plus matching _trades.csv / _stderr.log)
#
# Self-consistency checks the operator can run on the summary:
#   1. (open=10, close=21) in Stage 2a should match Phase 1 cell:
#         trades=4651, avg_pnl=+0.002067, gross_pnl=+9.614396
#   2. (open=13, close=21) in Stage 2a should match Phase 1 cell:
#         trades=3731, avg_pnl=+0.003306, gross_pnl=+12.336335
#   If either drifts, an override-apply bug crept in between sessions.
#
# Decision rule (per Phase 1 results memo §5 + scoping memo §5):
#
#   Stage 2a PASS: at least 3 of 7 cells produce avg_pnl >= +0.001 AND
#                  the positive cells form a contiguous range on the
#                  open axis (no isolated points). The 'monotonic
#                  positive' bar is relaxed for Phase 2 because Phase 1
#                  already showed a bell-shape; we're refining the peak,
#                  not establishing a trend.
#
#   Stage 2b PASS: at least 2 of 4 cells produce avg_pnl >= +0.001 AND
#                  (13,21) remains the peak or is within 20% of the
#                  Stage 2b peak.
#
#   Stage 2c PASS: at least 1 of 3 off-diagonal cells produces
#                  avg_pnl >= +0.001. (Lenient -- this stage is
#                  exploratory, not gating.)
#
#   Overall PASS for Phase 2: Stages 2a AND 2b both pass. Stage 2c is
#                  informational only.
#
#   If Phase 2 overall PASSES:
#     -> Phase 3 walk-forward validation on the best (open, close) pair.
#        See scoping memo §5 for the WF protocol.
#   If Phase 2 overall FAILS:
#     -> The (13,21) edge from Phase 1 was a single-cell artifact, not
#        a smooth surface. Recommend Tier 4 (signal-shape redesign)
#        per scoping memo §4 reasoning. Close the VWR USTEC.F retune
#        track.
# -----------------------------------------------------------------------------

set -euo pipefail

# ---- Args -------------------------------------------------------------------

if [[ $# -lt 1 ]]; then
    cat <<EOF >&2
Usage: $0 <tape.csv> [output_dir]

  tape.csv     Path to the tick CSV.
               Recommended: /Users/jo/Tick/NSXUSD_merged.csv
  output_dir   Optional. Default: outputs/vrev_t1_p2_<timestamp>/.

See header of $0 for full documentation.
EOF
    exit 1
fi

TAPE="$1"
OUT_DIR="${2:-outputs/vrev_t1_p2_$(date +%Y%m%d_%H%M%S)}"
SYMBOL="USTEC.F"
HARNESS="./build/VWAPReversionBacktest"

# ---- Pre-flight -------------------------------------------------------------

if [[ ! -f "$TAPE" ]]; then
    echo "[ERROR] Tape file not found: $TAPE" >&2
    exit 1
fi

if [[ ! -x "$HARNESS" ]]; then
    echo "[ERROR] Harness binary not found or not executable: $HARNESS" >&2
    echo "        Build with:" >&2
    echo "          cmake --build build --target VWAPReversionBacktest --config Release -j" >&2
    exit 1
fi

# Sanity: confirm the harness understands --session-open-hour. Older binaries
# would reject this flag with "Unknown option" and crash the sweep early.
if "$HARNESS" /dev/null --symbol USTEC.F --session-open-hour 8 --limit 1 \
        --quiet --report /dev/null --trades /dev/null \
        2>&1 | grep -q "Unknown option"; then
    echo "[ERROR] Harness does not understand --session-open-hour. The S70" >&2
    echo "        Tier 1 CLI flags are not in this build. Rebuild:" >&2
    echo "          cmake --build build --target VWAPReversionBacktest --config Release -j" >&2
    exit 1
fi

mkdir -p "$OUT_DIR/cells"

SUMMARY="$OUT_DIR/phase2_summary.csv"
echo "stage,open_h,close_h,trades,wins,win_rate_pct,gross_pnl,avg_pnl,n_tp_hit,n_sl_hit,n_timeout,n_mae_early_exit,worst_trade,p95_worst_loss" > "$SUMMARY"

# ---- Cell runner ------------------------------------------------------------
#
# Args:
#   stage      "2a", "2b", or "2c".
#   open_h     UTC hour the session opens (integer 0-23).
#   close_h    UTC hour the session closes (integer 1-24).
# Cell IDs:
#   2a_open_<NN>             -- stage 2a, close fixed at 21
#   2b_close_<NN>            -- stage 2b, open fixed at 13
#   2c_off_<NN>x<NN>         -- stage 2c, both vary

run_cell() {
    local stage="$1"
    local open_h="$2"
    local close_h="$3"

    local cell_id
    case "$stage" in
        2a) cell_id=$(printf "2a_open_%02d"      "$open_h") ;;
        2b) cell_id=$(printf "2b_close_%02d"     "$close_h") ;;
        2c) cell_id=$(printf "2c_off_%02dx%02d"  "$open_h" "$close_h") ;;
        *)  echo "[ERROR] unknown stage: $stage" >&2; return 1 ;;
    esac

    local report="$OUT_DIR/cells/${cell_id}_report.csv"
    local trades="$OUT_DIR/cells/${cell_id}_trades.csv"
    local stderr_log="$OUT_DIR/cells/${cell_id}_stderr.log"

    printf "[%s] %s open=%-2d close=%-2d ..." \
        "$(date +%H:%M:%S)" "$stage" "$open_h" "$close_h" >&2

    local t0 t1 dur
    t0=$(date +%s)

    "$HARNESS" "$TAPE" \
        --symbol "$SYMBOL" \
        --mode baseline \
        --quiet \
        --session-open-hour "$open_h" \
        --session-close-hour "$close_h" \
        --report "$report" \
        --trades "$trades" \
        2> "$stderr_log"

    t1=$(date +%s)
    dur=$((t1 - t0))

    local trades_n wins win_rate gross avg tp sl tmo mae worst p95
    trades_n=$(awk -F, '/^trades,/             {print $2; exit}' "$report")
    wins=$(    awk -F, '/^wins,/               {print $2; exit}' "$report")
    win_rate=$(awk -F, '/^win_rate_pct,/       {print $2; exit}' "$report")
    gross=$(   awk -F, '/^gross_pnl,/          {print $2; exit}' "$report")
    avg=$(     awk -F, '/^avg_pnl,/            {print $2; exit}' "$report")
    tp=$(      awk -F, '/^n_tp_hit,/           {print $2; exit}' "$report")
    sl=$(      awk -F, '/^n_sl_hit,/           {print $2; exit}' "$report")
    tmo=$(     awk -F, '/^n_timeout,/          {print $2; exit}' "$report")
    mae=$(     awk -F, '/^n_mae_early_exit,/   {print $2; exit}' "$report")
    worst=$(   awk -F, '/^worst_trade,/        {print $2; exit}' "$report")
    p95=$(     awk -F, '/^p95_worst_loss,/     {print $2; exit}' "$report")

    echo "${stage},${open_h},${close_h},${trades_n},${wins},${win_rate},${gross},${avg},${tp},${sl},${tmo},${mae},${worst},${p95}" >> "$SUMMARY"

    printf " trades=%-6s tp=%-4s gross=%-12s avg=%-12s  (%ds)\n" \
        "${trades_n}" "${tp}" "${gross}" "${avg}" "${dur}" >&2
}

# ---- Sweep header -----------------------------------------------------------

echo "===============================================================" >&2
echo "  Tier 1 Phase 2 session-window refinement -- VWR USTEC.F" >&2
echo "  Tape    : $TAPE" >&2
echo "  Symbol  : $SYMBOL" >&2
echo "  Output  : $OUT_DIR" >&2
echo "  Summary : $SUMMARY" >&2
echo "  Mode    : --mode baseline (S63 trio = 0.0)" >&2
echo "  Cells   : 14 across 3 stages (~6.5 min wall-clock estimate)" >&2
echo "===============================================================" >&2
echo "" >&2

SWEEP_T0=$(date +%s)

# ---- Stage 2a: open-hour at close=21 (7 cells) ------------------------------
# Brackets the Phase 1 winner (13,21) on the open axis.
# (10,21) and (13,21) double as Phase 1 cross-run consistency checks.
echo "--- Stage 2a: --session-open-hour sweep (close=21 fixed) ---" >&2

for OPEN_H in 9 10 11 12 13 14 15; do
    run_cell "2a" "$OPEN_H" 21
done

echo "" >&2

# ---- Stage 2b: close-hour at open=13 (4 cells) ------------------------------
# Brackets the Phase 1 winner (13,21) on the close axis. (13,21) was
# already covered in Stage 2a so it's omitted here.
echo "--- Stage 2b: --session-close-hour sweep (open=13 fixed) ---" >&2

for CLOSE_H in 18 19 20 22; do
    run_cell "2b" 13 "$CLOSE_H"
done

echo "" >&2

# ---- Stage 2c: off-diagonal 2D probe (3 cells) ------------------------------
# Tests whether the positive region is an island or extends off-axis.
# (12,20) -- mid-mid, near the winner
# (11,22) -- early open, late close
# (14,20) -- late open, mid close
echo "--- Stage 2c: off-diagonal 2D probe ---" >&2

run_cell "2c" 12 20
run_cell "2c" 11 22
run_cell "2c" 14 20

SWEEP_T1=$(date +%s)
SWEEP_DUR=$((SWEEP_T1 - SWEEP_T0))

# ---- Summary ----------------------------------------------------------------

echo "" >&2
echo "===============================================================" >&2
echo "  Tier 1 Phase 2 sweep complete -- ${SWEEP_DUR}s wall-clock (14 cells)" >&2
echo "===============================================================" >&2
echo "" >&2

if command -v column >/dev/null 2>&1; then
    column -t -s, "$SUMMARY" >&2
else
    cat "$SUMMARY" >&2
fi

echo "" >&2
echo "Summary CSV : $SUMMARY" >&2
echo "Cell reports: $OUT_DIR/cells/" >&2
echo "" >&2

# ---- Decision-rule hint -----------------------------------------------------

echo "Decision rule (per Phase 1 memo §5 + scoping memo §5):" >&2
echo "  Stage 2a PASS: >=3 of 7 cells with avg_pnl >= +0.001 AND the" >&2
echo "                 positive cells form a contiguous range on the" >&2
echo "                 open axis (no isolated points)." >&2
echo "  Stage 2b PASS: >=2 of 4 cells with avg_pnl >= +0.001 AND" >&2
echo "                 (13,21) remains the peak or is within 20% of" >&2
echo "                 the Stage 2b peak." >&2
echo "  Stage 2c PASS: >=1 of 3 off-diagonal cells with avg_pnl >= +0.001." >&2
echo "                 (Informational -- not gating overall verdict.)" >&2
echo "" >&2
echo "  Overall Phase 2 PASS = Stage 2a PASS AND Stage 2b PASS." >&2
echo "  If PASS  -> Phase 3 walk-forward on the best (open,close) pair." >&2
echo "  If FAIL  -> Tier 4 (signal-shape redesign). Close the retune track." >&2
echo "" >&2

# Self-consistency: Stage 2a's (10,21) and (13,21) cells should match
# the Phase 1 sweep's cells exactly.
echo "Self-consistency vs Phase 1 (these cells should be identical):" >&2
awk -F, '
    ($1=="2a" && $2=="10" && $3=="21") { print "  2a_open_10 (vs Phase 1 (10,21)) :", $0 }
    ($1=="2a" && $2=="13" && $3=="21") { print "  2a_open_13 (vs Phase 1 (13,21)) :", $0 }
' "$SUMMARY" >&2
echo "  Phase 1 (10,21) -> trades=4651, wins=?,  gross=9.614396,    avg=0.002067" >&2
echo "  Phase 1 (13,21) -> trades=3731, wins=?,  gross=12.336335,   avg=0.003306" >&2

echo "" >&2
