#!/usr/bin/env bash
# =============================================================================
# utf5m_sweep_p1.sh -- UTF5m Phase 1 univariate sweep across the S63 trio
# =============================================================================
# Implements the Phase 1 step prescribed by
# `docs/handoffs/SESSION_HANDOFF_2026-05-14h.md` §"Recommended next-session
# focus" item 3, against the S63 wiring landed in part-L
# (UstecTrendFollow5mEngine.hpp:323-357) and the dedicated harness landed
# in S72 P0 (backtest/UstecTrendFollow5mBacktest.cpp, commit 51487fa).
#
# Investigation question (per engine_init.hpp:964-967):
#   Is the S63 + S37 widened SL/TP profile net-positive on USTEC fresh tape?
#
# This sweep isolates each of the three S63 axes individually while holding
# the other two at engine class defaults (LOSS_CUT_PCT=0.08, BE_ARM_PCT=0.05,
# BE_BUFFER_PCT=0.02 as of part-L). Each axis is swept across a 5-7 cell
# grid that includes the engine default level + an "off" cell + ~3-5
# offset cells in each direction.
#
# The 2 reference cells (baseline, tuned) plus the default-level cell of
# each axis are mathematically the same run (default-level only overrides
# the swept axis to its own engine default). If the three axis-default
# cells don't match the tuned reference cell, that's an override-apply
# bug -- see "self-consistency check" at the end.
#
# Sweep design (19 cells total):
#
#   Reference cells (2):
#     baseline                   --mode baseline                  (S63 trio = 0/0/0)
#     tuned                      --mode tuned                     (engine defaults 0.08/0.05/0.02)
#
#   Axis A -- --loss-cut (7 cells):
#     --mode tuned --loss-cut    0.00 0.04 0.06 0.08 [default] 0.10 0.12 0.16
#
#   Axis B -- --be-arm (5 cells):
#     --mode tuned --be-arm      0.00 0.03 0.05 [default] 0.07 0.10
#
#   Axis C -- --be-buffer (5 cells):
#     --mode tuned --be-buffer   0.00 0.01 0.02 [default] 0.04 0.06
#
# Wall-clock estimate: 19 cells * ~10-15 min/cell on the 4.4 GB NSXUSD tape
# (calibrate from Phase 0 timing on the first run; the Phase 0 full-tape
# baseline+tuned pair done in this session gives the per-cell baseline).
# Plan for 3-5 hours. If timing is materially shorter or longer, adjust
# Phase 2 fine-sweep scope accordingly.
#
# Usage:
#   scripts/utf5m_sweep_p1.sh <tape.csv> [output_dir]
#
# Args:
#   tape.csv    Path to the tick CSV. Recommended:
#                 /Users/jo/Tick/NSXUSD_merged.csv  (4.4 GB, same tape as
#                                                    the VWR sweep used in
#                                                    parts P-R)
#   output_dir  Optional. Default: outputs/utf5m_p1_<timestamp>/.
#               outputs/ is gitignored so artifacts stay local; the
#               results memo is the only artifact that gets committed.
#
# Output structure:
#   <output_dir>/
#     phase1_summary.csv          One row per cell + header. Columns:
#                                   axis, level, trades, wins, win_rate_pct,
#                                   gross_pnl, avg_pnl, n_tp_hit, n_sl_hit,
#                                   n_prove_it_fail, n_loss_cut, n_be_cut,
#                                   worst_trade, p95_worst_loss,
#                                   loss_cut_pct, be_arm_pct, be_buffer_pct
#                                 The trailing 3 columns echo the effective
#                                 S63 engine config the run actually used,
#                                 so the CSV is self-describing.
#     cells/
#       <axis>_<level>_report.csv  Full per-cell report CSV.
#       <axis>_<level>_trades.csv  Full per-cell trades CSV.
#       <axis>_<level>_stderr.log  Harness stderr (banner + summary).
#
# Decision rule (per part-S handoff §"Recommended next-session focus" item 5
# read against the promotion-gate text):
#   A cell PASSES if it meets the basic plumbing-sane thresholds:
#     - trades       > 0   (some entries fired)
#     - gross_pnl    > 0   (net positive on the full tape)
#   An AXIS shows EDGE if its best cell has:
#     - gross_pnl    >= 1.5x the baseline-reference gross_pnl
#                          (or simply gross_pnl > 0 if baseline is also < 0)
#     - n_loss_cut or n_be_cut > 0 (the S63 path actually fired -- otherwise
#                          the "tuned" cell is functionally identical to
#                          baseline and the test isn't measuring S63 at all)
#   If the BEST cell across all three axes does not show edge over baseline,
#   the promotion gate fails on this tape and the recommended next move is
#   a closure memo + Tier 4 redesign (signal-shape rework, mirroring the
#   VWR S71 outcome).
#
# Decision artifact: outputs/UTF5M_PHASE1_RESULTS_<date>.md
# (operator authors this from the summary CSV + cell stderr logs).
#
# Walk-forward note: per the VWR S71 Phase 3 lesson (handoff §"Important
# lessons / don't-repeat" item 2), full-tape gross_pnl alone is not
# sufficient. Even if a Phase 1 axis cell looks strong here, a Phase 3
# walk-forward must confirm 3+ of 4 windows positive with aggregate
# PF >= 1.20 before the enabled flip is justified. Phase 1 only narrows
# the search; it does not grant the promotion.
# -----------------------------------------------------------------------------

set -euo pipefail

# ---- Args -------------------------------------------------------------------

if [[ $# -lt 1 ]]; then
    cat <<EOF >&2
Usage: $0 <tape.csv> [output_dir]

  tape.csv     Path to the tick CSV (auto-detected format).
               Recommended: /Users/jo/Tick/NSXUSD_merged.csv
  output_dir   Optional. Default: outputs/utf5m_p1_<timestamp>/.

See header of $0 for full documentation.
EOF
    exit 1
fi

TAPE="$1"
OUT_DIR="${2:-outputs/utf5m_p1_$(date +%Y%m%d_%H%M%S)}"
HARNESS="./build/UstecTrendFollow5mBacktest"

# ---- Pre-flight -------------------------------------------------------------

if [[ ! -f "$TAPE" ]]; then
    echo "[ERROR] Tape file not found: $TAPE" >&2
    exit 1
fi

if [[ ! -x "$HARNESS" ]]; then
    echo "[ERROR] Harness binary not found or not executable: $HARNESS" >&2
    echo "        Build with:" >&2
    echo "          cmake --build build --target UstecTrendFollow5mBacktest --config Release -j" >&2
    exit 1
fi

# Sanity: confirm the harness understands the S63 trio CLI. Probe one flag
# minimally so we fail loud BEFORE committing to a multi-hour sweep.
if "$HARNESS" /dev/null --loss-cut 0.08 --limit 1 \
        --quiet --report /dev/null --trades /dev/null \
        2>&1 | grep -q "Unknown option"; then
    echo "[ERROR] Harness does not understand --loss-cut. The S72-P0 CLI flags" >&2
    echo "        are not in this build. Rebuild:" >&2
    echo "          cmake --build build --target UstecTrendFollow5mBacktest --config Release -j" >&2
    exit 1
fi

mkdir -p "$OUT_DIR/cells"

SUMMARY="$OUT_DIR/phase1_summary.csv"
echo "axis,level,trades,wins,win_rate_pct,gross_pnl,avg_pnl,n_tp_hit,n_sl_hit,n_prove_it_fail,n_loss_cut,n_be_cut,worst_trade,p95_worst_loss,loss_cut_pct,be_arm_pct,be_buffer_pct" > "$SUMMARY"

# ---- Cell runner ------------------------------------------------------------
#
# Args:
#   axis      Logical axis name used in the cell_id + summary CSV (e.g.
#             "loss-cut", "be-arm", "be-buffer", "ref").
#   level     Display string for the cell_id + summary CSV (e.g. "0.08",
#             "baseline", "tuned").
#   ...       Remaining args are passed verbatim to the harness. The caller
#             is responsible for the --mode flag and any axis override.

run_cell() {
    local axis="$1"; shift
    local level="$1"; shift

    local cell_id="${axis}_${level}"
    local report="$OUT_DIR/cells/${cell_id}_report.csv"
    local trades="$OUT_DIR/cells/${cell_id}_trades.csv"
    local stderr_log="$OUT_DIR/cells/${cell_id}_stderr.log"

    printf "[%s] cell %-24s ..." "$(date +%H:%M:%S)" "${axis}=${level}" >&2

    local t0 t1 dur
    t0=$(date +%s)

    "$HARNESS" "$TAPE" \
        --quiet \
        "$@" \
        --report "$report" \
        --trades "$trades" \
        2> "$stderr_log"

    t1=$(date +%s)
    dur=$((t1 - t0))

    # Parse the per-cell report (metric,value CSV). Grep one field at a
    # time so the script is resilient to column reordering by future
    # harness changes.
    local trades_n wins win_rate gross avg tp sl pi lc be worst p95
    local lcp bap bbp

    trades_n=$( awk -F, '/^trades,/             {print $2; exit}' "$report")
    wins=$(     awk -F, '/^wins,/               {print $2; exit}' "$report")
    win_rate=$( awk -F, '/^win_rate_pct,/       {print $2; exit}' "$report")
    gross=$(    awk -F, '/^gross_pnl,/          {print $2; exit}' "$report")
    avg=$(      awk -F, '/^avg_pnl,/            {print $2; exit}' "$report")
    tp=$(       awk -F, '/^n_tp_hit,/           {print $2; exit}' "$report")
    sl=$(       awk -F, '/^n_sl_hit,/           {print $2; exit}' "$report")
    pi=$(       awk -F, '/^n_prove_it_fail,/    {print $2; exit}' "$report")
    lc=$(       awk -F, '/^n_loss_cut,/         {print $2; exit}' "$report")
    be=$(       awk -F, '/^n_be_cut,/           {print $2; exit}' "$report")
    worst=$(    awk -F, '/^worst_trade,/        {print $2; exit}' "$report")
    p95=$(      awk -F, '/^p95_worst_loss,/     {print $2; exit}' "$report")
    lcp=$(      awk -F, '/^loss_cut_pct,/       {print $2; exit}' "$report")
    bap=$(      awk -F, '/^be_arm_pct,/         {print $2; exit}' "$report")
    bbp=$(      awk -F, '/^be_buffer_pct,/      {print $2; exit}' "$report")

    echo "${axis},${level},${trades_n},${wins},${win_rate},${gross},${avg},${tp},${sl},${pi},${lc},${be},${worst},${p95},${lcp},${bap},${bbp}" >> "$SUMMARY"

    printf " trades=%-6s lc=%-4s be=%-4s gross=%-12s avg=%-12s  (%ds)\n" \
        "${trades_n}" "${lc}" "${be}" "${gross}" "${avg}" "${dur}" >&2
}

# ---- Sweep header -----------------------------------------------------------

echo "===============================================================" >&2
echo "  UTF5m Phase 1 univariate sweep -- USTEC.F" >&2
echo "  Tape    : $TAPE" >&2
echo "  Harness : $HARNESS" >&2
echo "  Output  : $OUT_DIR" >&2
echo "  Summary : $SUMMARY" >&2
echo "  Cells   : 19 (2 ref + 7 loss-cut + 5 be-arm + 5 be-buffer)" >&2
echo "  Estimate: ~3-5 hours wall-clock (depends on Phase 0 per-cell timing)" >&2
echo "===============================================================" >&2
echo "" >&2

SWEEP_T0=$(date +%s)

# ---- Reference cells (2) ----------------------------------------------------
# baseline: S63 trio fully zeroed -- this is the "is S63 even helping?" pole.
# tuned: S63 trio at engine defaults (0.08/0.05/0.02) -- this is the "current
# production state" pole. The default-level cell in each axis below is the
# SAME run as `tuned` modulo the explicitly-overridden flag, so the
# self-consistency check at the end uses `tuned` as the canonical comparator.
echo "--- Reference cells (2) ---" >&2
run_cell "ref" "baseline" --mode baseline
run_cell "ref" "tuned"    --mode tuned

# ---- Axis A: --loss-cut (7 cells) -------------------------------------------
# Engine default 0.08. At USTEC ~$28K, this is ~22pt cold-loss cut. The S34-B
# floor sl_mult=3.0 * MIN_ATR_PTS=20 = 60pt SL, so LOSS_CUT only fires on
# outliers exceeding the ATR-based SL by slippage or gap. Tighter values
# (0.04 = ~11pt) cut more aggressively; looser values (0.16 = ~45pt) only
# fire on extreme outliers. Level 0.00 disables the cut entirely (the
# "no-cold-cut" pole).
echo "--- Axis A/3: --loss-cut (BE_ARM/BUFFER at engine defaults) ---" >&2
for L in 0.00 0.04 0.06 0.08 0.10 0.12 0.16; do
    run_cell "loss-cut" "$L" --mode tuned --loss-cut "$L"
done

# ---- Axis B: --be-arm (5 cells) ---------------------------------------------
# Engine default 0.05. At USTEC ~$28K, this is ~14pt mfe to arm the BE ratchet.
# Typical TP is tp_mult=7.0 * atr14_>=20 = >=140pt, so the ratchet only engages
# after a trade has earned ~10% of target. Lower values (0.03 = ~8pt) arm
# earlier (cuts more breakeven exits); higher values (0.10 = ~28pt) require
# more progress before ratcheting (lets winners run further). Level 0.00
# disables the ratchet (BE_BUFFER becomes inert under the engine's logic).
echo "--- Axis B/3: --be-arm (LOSS_CUT/BUFFER at engine defaults) ---" >&2
for L in 0.00 0.03 0.05 0.07 0.10; do
    run_cell "be-arm" "$L" --mode tuned --be-arm "$L"
done

# ---- Axis C: --be-buffer (5 cells) ------------------------------------------
# Engine default 0.02. At USTEC ~$28K, this is ~5.6pt buffer, roughly the
# typical USTEC spread. Tighter values (0.01) make the ratchet snap exactly
# at entry; looser values (0.06) let trades give back more before cutting.
# Level 0.00 means the ratchet fires the instant move <= 0 after arming
# (immediate giveback exit).
echo "--- Axis C/3: --be-buffer (LOSS_CUT/ARM at engine defaults) ---" >&2
for L in 0.00 0.01 0.02 0.04 0.06; do
    run_cell "be-buffer" "$L" --mode tuned --be-buffer "$L"
done

SWEEP_T1=$(date +%s)
SWEEP_DUR=$((SWEEP_T1 - SWEEP_T0))

# ---- Summary ----------------------------------------------------------------

echo "" >&2
echo "===============================================================" >&2
echo "  UTF5m Phase 1 sweep complete -- ${SWEEP_DUR}s wall-clock (19 cells)" >&2
echo "===============================================================" >&2
echo "" >&2

# Pretty-print only the key columns (axis, level, trades, lc, be, gross,
# avg) so the terminal output stays readable. Full data is in the summary
# CSV.
if command -v column >/dev/null 2>&1; then
    awk -F, 'BEGIN{OFS=","}{print $1,$2,$3,$11,$12,$6,$7}' "$SUMMARY" | column -t -s, >&2
else
    awk -F, 'BEGIN{OFS=","}{print $1,$2,$3,$11,$12,$6,$7}' "$SUMMARY" >&2
fi

echo "" >&2
echo "Summary CSV (full)  : $SUMMARY" >&2
echo "Cell reports        : $OUT_DIR/cells/" >&2
echo "" >&2

# ---- Decision-rule hint -----------------------------------------------------

echo "Decision rule (per part-S handoff + promotion-gate text):" >&2
echo "  A cell PASSES if all of:" >&2
echo "    trades   > 0" >&2
echo "    gross_pnl > 0   (net positive on full tape)" >&2
echo "  An AXIS shows EDGE if its best cell beats baseline gross_pnl by 1.5x" >&2
echo "  AND has n_loss_cut > 0 OR n_be_cut > 0 (S63 path actually firing)." >&2
echo "" >&2
echo "  If no axis shows edge: closure memo + Tier 4 redesign recommendation" >&2
echo "  (mirror VWR S71 outcome). The memo lives at:" >&2
echo "    outputs/UTF5M_PHASE1_RESULTS_$(date +%Y-%m-%d).md" >&2
echo "" >&2
echo "  CRITICAL: even if an axis looks strong on full-tape, a Phase 3" >&2
echo "  walk-forward MUST confirm 3+ of 4 windows positive with aggregate" >&2
echo "  PF >= 1.20 before the enabled flip. Phase 1 narrows; it does not" >&2
echo "  promote. See VWR S71 Phase 2->3 reversal for the lesson." >&2
echo "" >&2

# Self-consistency: the default-level cell of each axis (loss-cut=0.08,
# be-arm=0.05, be-buffer=0.02) when run with --mode tuned --<flag> <default>
# should produce IDENTICAL results to the `ref tuned` cell. Drift here is an
# override-apply bug -- diagnose BEFORE trusting the sweep.
echo "Self-consistency check (these 4 cells should be byte-identical --" >&2
echo "all run engine defaults; the explicit --flag X just sets X to its own" >&2
echo "engine default):" >&2
awk -F, '
    ($1=="ref"        && $2=="tuned") { print "  ref tuned                :", $0 }
    ($1=="loss-cut"   && $2=="0.08")  { print "  loss-cut=0.08 (default)  :", $0 }
    ($1=="be-arm"     && $2=="0.05")  { print "  be-arm=0.05   (default)  :", $0 }
    ($1=="be-buffer"  && $2=="0.02")  { print "  be-buffer=0.02 (default) :", $0 }
' "$SUMMARY" >&2

echo "" >&2
