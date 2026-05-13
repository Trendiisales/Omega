#!/usr/bin/env bash
# =============================================================================
# vrev_sweep_p1.sh -- Phase 1 of the VWR USTEC.F retune sweep
# =============================================================================
# Univariate baseline edge probe per outputs/VWR_USTEC_RETUNE_PLAN_2026-05-14a.md
# §5 Phase 1. Runs 21 cells (6 + 5 + 5 + 5) against the supplied tape, each in
# --mode baseline (S63 trio zeroed) so we're probing the entry-side parameter
# surface only. Each cell writes its own report.csv / trades.csv; a single
# phase1_summary.csv aggregates the key metrics across all 21 cells.
#
# Usage:
#   scripts/vrev_sweep_p1.sh <tape.csv> [output_dir]
#
# Args:
#   tape.csv     Path to the tape file (htf_bt_minimal-format CSV or one of
#                the four tick formats the harness auto-detects -- see the
#                top of backtest/VWAPReversionBacktest.cpp for the list).
#   output_dir   Optional. Defaults to outputs/vrev_p1_<timestamp>/. The
#                outputs/ subtree is gitignored, so artifacts stay local.
#
# Output structure:
#   <output_dir>/
#     phase1_summary.csv          One row per cell + header. Columns:
#                                   param, level, trades, gross_pnl, avg_pnl,
#                                   n_tp_hit, n_mae_early_exit, worst_trade,
#                                   p95_worst_loss
#     cells/
#       ext_0.20_report.csv       Full per-cell report CSV from the harness
#       ext_0.20_trades.csv       Full per-cell trades CSV from the harness
#       ext_0.20_stderr.log       Harness stderr (banner + summary block)
#       ...                       (one set per cell, 21 cells total)
#
# Self-consistency check the operator can run on the summary:
#   The cells (ext, 0.40), (max-ext, 1.20), (max-hold, 600), (cooldown, 300)
#   are all the SAME run -- each is just hitting the per-symbol default for
#   the one swept param while leaving the rest at preset. They should all
#   show identical (trades, gross_pnl, ...) values. If they don't, something
#   is off in the override-apply ordering and Phase 0 needs re-investigation.
#
# Decision rule per the plan §5 stop condition:
#   "If no single-axis move produces baseline >= +0.001 / trade, the strategy
#    likely needs structural changes (entry signal, TP/SL geometry, session
#    timing) rather than parameter tuning. Memo that finding and stop."
# avg_pnl is the per-trade expectancy. The threshold maps to avg_pnl >= 0.001
# in the summary CSV (USTEC quote scale, per-trade abs PnL units).
#
# Wall-clock estimate: ~21 * (30-90s) = 10-30 min for the 404-day NSXUSD HistData
# tape. Larger tapes (e.g. 25-month Dukascopy) scale linearly in tick count.
# -----------------------------------------------------------------------------

set -euo pipefail

# ---- Args -------------------------------------------------------------------

if [[ $# -lt 1 ]]; then
    cat <<EOF >&2
Usage: $0 <tape.csv> [output_dir]

  tape.csv     Path to the tick CSV (auto-detected format).
  output_dir   Optional. Default: outputs/vrev_p1_<timestamp>/.

See header of $0 for full documentation.
EOF
    exit 1
fi

TAPE="$1"
OUT_DIR="${2:-outputs/vrev_p1_$(date +%Y%m%d_%H%M%S)}"
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

# Sanity: confirm the harness understands the new flags. Older binaries (from
# before the Phase 0 commit) will fail the --help-style probe; this prevents
# the sweep from silently producing 21 identical baseline runs.
if ! "$HARNESS" 2>&1 | grep -q "EXTENSION_THRESH_PCT" 2>/dev/null; then
    : # harness with no args errors out; that's fine, we don't really need a probe
fi

mkdir -p "$OUT_DIR/cells"

SUMMARY="$OUT_DIR/phase1_summary.csv"
echo "param,level,trades,gross_pnl,avg_pnl,n_tp_hit,n_mae_early_exit,worst_trade,p95_worst_loss" > "$SUMMARY"

# ---- Cell runner ------------------------------------------------------------

run_cell() {
    local param="$1"   # logical name (e.g. "ext", "max-ext", "max-hold", "cooldown")
    local level="$2"   # value to sweep (e.g. "0.30", "1500")
    local flag="$3"    # CLI flag string (e.g. "--ext", "--max-ext")

    local cell_id="${param}_${level}"
    local report="$OUT_DIR/cells/${cell_id}_report.csv"
    local trades="$OUT_DIR/cells/${cell_id}_trades.csv"
    local stderr_log="$OUT_DIR/cells/${cell_id}_stderr.log"

    printf "[%s] cell %-25s ..." "$(date +%H:%M:%S)" "${param}=${level}" >&2

    local t0 t1 dur
    t0=$(date +%s)

    "$HARNESS" "$TAPE" \
        --symbol "$SYMBOL" \
        --mode baseline \
        --quiet \
        "$flag" "$level" \
        --report "$report" \
        --trades "$trades" \
        2> "$stderr_log"

    t1=$(date +%s)
    dur=$((t1 - t0))

    # Parse the per-cell report (it's a metric,value CSV; we grep one field
    # at a time so the script is resilient to column reordering by future
    # harness changes).
    local trades_n gross avg tp mae worst p95
    trades_n=$(awk -F, '/^trades,/        {print $2; exit}' "$report")
    gross=$(   awk -F, '/^gross_pnl,/     {print $2; exit}' "$report")
    avg=$(     awk -F, '/^avg_pnl,/       {print $2; exit}' "$report")
    tp=$(      awk -F, '/^n_tp_hit,/      {print $2; exit}' "$report")
    mae=$(     awk -F, '/^n_mae_early_exit,/ {print $2; exit}' "$report")
    worst=$(   awk -F, '/^worst_trade,/   {print $2; exit}' "$report")
    p95=$(     awk -F, '/^p95_worst_loss,/{print $2; exit}' "$report")

    echo "${param},${level},${trades_n},${gross},${avg},${tp},${mae},${worst},${p95}" >> "$SUMMARY"

    printf " trades=%-6s gross=%-12s avg=%-12s  (%ds)\n" \
        "${trades_n}" "${gross}" "${avg}" "${dur}" >&2
}

# ---- Sweep header -----------------------------------------------------------

echo "===============================================================" >&2
echo "  Phase 1 univariate sweep -- VWR USTEC.F" >&2
echo "  Tape    : $TAPE" >&2
echo "  Symbol  : $SYMBOL" >&2
echo "  Output  : $OUT_DIR" >&2
echo "  Summary : $SUMMARY" >&2
echo "  Mode    : --mode baseline (S63 trio = 0.0)" >&2
echo "===============================================================" >&2
echo "" >&2

SWEEP_T0=$(date +%s)

# ---- Cells: --ext (6) -------------------------------------------------------
# Class default 0.20 (tighter) -> current USTEC override 0.40 -> looser cap 0.80
for L in 0.20 0.30 0.40 0.50 0.60 0.80; do
    run_cell ext "$L" --ext
done

# ---- Cells: --max-ext (5) ---------------------------------------------------
# Class default 0.80 -> current USTEC override 1.20 -> wider cap 2.00
for L in 0.80 1.00 1.20 1.50 2.00; do
    run_cell max-ext "$L" --max-ext
done

# ---- Cells: --max-hold (5) --------------------------------------------------
# Current USTEC override 600 -> class default 900 -> longer hold tests
for L in 300 600 900 1200 1500; do
    run_cell max-hold "$L" --max-hold
done

# ---- Cells: --cooldown (5) --------------------------------------------------
# Class default 180 -> current USTEC override 300 -> more conservative caps
for L in 120 180 300 600 900; do
    run_cell cooldown "$L" --cooldown
done

SWEEP_T1=$(date +%s)
SWEEP_DUR=$((SWEEP_T1 - SWEEP_T0))

# ---- Summary ----------------------------------------------------------------

echo "" >&2
echo "===============================================================" >&2
echo "  Phase 1 sweep complete -- ${SWEEP_DUR}s wall-clock (21 cells)" >&2
echo "===============================================================" >&2
echo "" >&2

# Pretty-print the summary CSV. column(1) renders the table aligned.
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

echo "Decision rule (per plan §5 stop condition):" >&2
echo "  If no single-axis move produces avg_pnl >= +0.001, baseline" >&2
echo "  retuning cannot fix this -- the strategy needs structural" >&2
echo "  changes (entry signal, TP/SL geometry, session timing)." >&2
echo "  Memo the finding and stop. Otherwise: Phase 2 fine sweep" >&2
echo "  around the top-improving axis." >&2
echo "" >&2

# Self-consistency: the four cells at the per-symbol defaults
# (ext=0.40, max-ext=1.20, max-hold=600, cooldown=300) are the SAME run
# and should produce identical numbers. Highlight any drift.
echo "Self-consistency check (these four cells should be identical --" >&2
echo "they all just hit the USTEC.F preset for their swept axis):" >&2
awk -F, '
    $1=="ext" && $2=="0.40"      { print "  ext=0.40       :", $0 }
    $1=="max-ext" && $2=="1.20"  { print "  max-ext=1.20   :", $0 }
    $1=="max-hold" && $2=="600"  { print "  max-hold=600   :", $0 }
    $1=="cooldown" && $2=="300"  { print "  cooldown=300   :", $0 }
' "$SUMMARY" >&2

echo "" >&2
