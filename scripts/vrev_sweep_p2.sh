#!/usr/bin/env bash
# =============================================================================
# vrev_sweep_p2.sh -- Phase 2 of the VWR USTEC.F retune sweep
# =============================================================================
# Refinement around the Phase 1 winner (ext=0.80 produced avg_pnl=+0.002820
# with 1145 trades). Two stages:
#
#   Phase 2A: 1D fine sweep on --ext at 6 levels around the Phase 1 winner.
#             Robustness check -- does +0.002820 hold across neighbouring
#             levels or was it a single-cell artifact?
#
#   Phase 2B: 2D ext x max-ext mini-grid (9 cells). Checks whether tightening
#             the upper extension cap (max-ext) interacts with the entry
#             threshold (ext). Max-ext=1.50 and max-ext=2.00 were the only
#             other marginally positive cells in Phase 1.
#
# Plan reference: outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md §5 Phase 2
#                 outputs/VWR_USTEC_PHASE1_RESULTS_2026-05-14e.md §5 Option C
# Phase 1 results live at outputs/vrev_p1_20260514_105824/phase1_summary.csv.
#
# Usage:
#   scripts/vrev_sweep_p2.sh <tape.csv> [output_dir]
#
# Wall-clock estimate: 15 cells * ~27s = ~7 min. (Phase 1 ran at 26-28s/cell
# on the 4.4GB NSXUSD tape.)
#
# Output structure mirrors Phase 1:
#   <output_dir>/
#     phase2_summary.csv          One row per cell. Columns add 'stage' and
#                                 'max_ext' so 2A and 2B rows are comparable.
#     cells/
#       2a_ext_0.70_report.csv    Phase 2A cell report
#       2b_ext_0.80_mx_1.50_report.csv  Phase 2B cell report
#       ...                       (plus matching _trades.csv / _stderr.log)
#
# Decision rules:
#   - Phase 2A pass: at least 4 of 6 fine-sweep cells produce avg_pnl > 0
#     AND the swing across the 6 levels is < 50% of the mean positive value
#     (i.e. the +ve expectancy isn't concentrated in one cell).
#   - Phase 2B pass: the 2D grid's best cell improves over Phase 2A's best
#     ext-only cell by >10% AND the second-best 2D cell is within 30% of best
#     (independence check -- adjacent cells should agree).
#
# If 2A fails: escalate per plan §8 to structural rework. Don't bother with 2B.
# If 2A passes but 2B fails: ext-only retune is the recommendation; carry the
#     best 2A cell forward to Phase 3 WF.
# If both pass: carry the best 2B cell forward to Phase 3 WF.
# -----------------------------------------------------------------------------

set -euo pipefail

# ---- Args -------------------------------------------------------------------

if [[ $# -lt 1 ]]; then
    cat <<EOF >&2
Usage: $0 <tape.csv> [output_dir]

  tape.csv     Path to the tick CSV (should match the Phase 1 tape for
               like-for-like comparison: /Users/jo/Tick/NSXUSD_merged.csv).
  output_dir   Optional. Default: outputs/vrev_p2_<timestamp>/.

See header of $0 for full documentation.
EOF
    exit 1
fi

TAPE="$1"
OUT_DIR="${2:-outputs/vrev_p2_$(date +%Y%m%d_%H%M%S)}"
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

mkdir -p "$OUT_DIR/cells"

SUMMARY="$OUT_DIR/phase2_summary.csv"
echo "stage,ext,max_ext,trades,gross_pnl,avg_pnl,n_tp_hit,n_mae_early_exit,worst_trade,p95_worst_loss" > "$SUMMARY"

# ---- Cell runner ------------------------------------------------------------

# run_cell <stage> <ext_level> <max_ext_level_or_default>
# If max_ext is "preset" we don't pass --max-ext; the harness uses the USTEC.F
# preset value (1.20). Otherwise we pass --max-ext <value>.

run_cell() {
    local stage="$1"          # "2a" or "2b"
    local ext="$2"            # e.g. "0.80"
    local mx="$3"             # e.g. "1.50" or "preset"

    local cell_id
    if [[ "$mx" == "preset" ]]; then
        cell_id="${stage}_ext_${ext}"
    else
        cell_id="${stage}_ext_${ext}_mx_${mx}"
    fi

    local report="$OUT_DIR/cells/${cell_id}_report.csv"
    local trades="$OUT_DIR/cells/${cell_id}_trades.csv"
    local stderr_log="$OUT_DIR/cells/${cell_id}_stderr.log"

    printf "[%s] %s ext=%s mx=%-7s ..." "$(date +%H:%M:%S)" "$stage" "$ext" "$mx" >&2

    local t0 t1 dur
    t0=$(date +%s)

    if [[ "$mx" == "preset" ]]; then
        "$HARNESS" "$TAPE" \
            --symbol "$SYMBOL" \
            --mode baseline \
            --quiet \
            --ext "$ext" \
            --report "$report" \
            --trades "$trades" \
            2> "$stderr_log"
    else
        "$HARNESS" "$TAPE" \
            --symbol "$SYMBOL" \
            --mode baseline \
            --quiet \
            --ext "$ext" \
            --max-ext "$mx" \
            --report "$report" \
            --trades "$trades" \
            2> "$stderr_log"
    fi

    t1=$(date +%s)
    dur=$((t1 - t0))

    local trades_n gross avg tp mae worst p95
    trades_n=$(awk -F, '/^trades,/        {print $2; exit}' "$report")
    gross=$(   awk -F, '/^gross_pnl,/     {print $2; exit}' "$report")
    avg=$(     awk -F, '/^avg_pnl,/       {print $2; exit}' "$report")
    tp=$(      awk -F, '/^n_tp_hit,/      {print $2; exit}' "$report")
    mae=$(     awk -F, '/^n_mae_early_exit,/ {print $2; exit}' "$report")
    worst=$(   awk -F, '/^worst_trade,/   {print $2; exit}' "$report")
    p95=$(     awk -F, '/^p95_worst_loss,/{print $2; exit}' "$report")

    # In the summary CSV, write the actual max_ext value (preset -> 1.20 for
    # USTEC.F) so 2A and 2B rows can be compared by column.
    local mx_for_csv
    if [[ "$mx" == "preset" ]]; then
        mx_for_csv="1.20"
    else
        mx_for_csv="$mx"
    fi

    echo "${stage},${ext},${mx_for_csv},${trades_n},${gross},${avg},${tp},${mae},${worst},${p95}" >> "$SUMMARY"

    printf " trades=%-6s gross=%-12s avg=%-12s  (%ds)\n" \
        "${trades_n}" "${gross}" "${avg}" "${dur}" >&2
}

# ---- Sweep header -----------------------------------------------------------

echo "===============================================================" >&2
echo "  Phase 2 sweep -- VWR USTEC.F refinement" >&2
echo "  Tape    : $TAPE" >&2
echo "  Symbol  : $SYMBOL" >&2
echo "  Output  : $OUT_DIR" >&2
echo "  Summary : $SUMMARY" >&2
echo "  Mode    : --mode baseline (S63 trio = 0.0)" >&2
echo "===============================================================" >&2
echo "" >&2

SWEEP_T0=$(date +%s)

# ---- Phase 2A: 1D fine sweep on --ext (6 cells) -----------------------------
# Robustness check around Phase 1 winner (ext=0.80, +0.002820).
echo "--- Phase 2A: --ext fine sweep (max-ext at preset 1.20) ---" >&2

for L in 0.70 0.75 0.80 0.85 0.90 1.00; do
    run_cell "2a" "$L" "preset"
done

echo "" >&2

# ---- Phase 2B: 2D ext x max-ext mini-grid (9 cells) -------------------------
# Independence check. Phase 1 found max-ext=1.50/2.00 marginally positive at
# the preset trade volume; here we test whether widening max-ext interacts
# with tightening ext.
echo "--- Phase 2B: ext x max-ext mini-grid ---" >&2

for E in 0.70 0.80 0.90; do
    for X in 1.20 1.50 2.00; do
        run_cell "2b" "$E" "$X"
    done
done

SWEEP_T1=$(date +%s)
SWEEP_DUR=$((SWEEP_T1 - SWEEP_T0))

# ---- Summary ----------------------------------------------------------------

echo "" >&2
echo "===============================================================" >&2
echo "  Phase 2 sweep complete -- ${SWEEP_DUR}s wall-clock (15 cells)" >&2
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

echo "Phase 2A decision hint:" >&2
echo "  Robustness pass: >=4 of 6 ext-only cells with avg_pnl > 0 AND the" >&2
echo "  swing across the 6 cells is < 50% of the mean positive value." >&2
echo "" >&2
echo "Phase 2B decision hint:" >&2
echo "  Independence pass: best 2D cell improves over best 2A cell by >10%" >&2
echo "  AND the second-best 2D cell is within 30% of the best." >&2
echo "" >&2
echo "If 2A fails: escalate to structural rework (plan §8). Don't bother" >&2
echo "             with 2B's results -- the ext axis itself isn't a stable" >&2
echo "             edge." >&2
echo "If 2A passes, 2B fails: carry best 2A cell to Phase 3 WF." >&2
echo "If both pass: carry best 2B cell to Phase 3 WF." >&2
echo "" >&2

# Self-consistency: 2b_ext_0.80_mx_1.20 reuses the Phase 1 ext=0.80 run since
# 1.20 is the preset. They should produce identical numbers across both
# sweeps (modulo Phase 1's own preset-cell identity).
echo "Self-consistency vs Phase 1 (these cells should be identical):" >&2
awk -F, '
    $1=="2b" && $2=="0.80" && $3=="1.20" { print "  2b_ext_0.80_mx_1.20 :", $0 }
' "$SUMMARY" >&2
echo "  phase1_ext=0.80     : (Phase 1 baseline)  ext,0.80,1145,3.229369,0.002820,25,97,-1.801615,-0.874505" >&2
echo "" >&2
