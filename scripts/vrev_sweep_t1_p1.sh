#!/usr/bin/env bash
# =============================================================================
# vrev_sweep_t1_p1.sh -- Tier 1 Phase 1 univariate sweep for VWR USTEC.F
# =============================================================================
# Implements the Phase 1 step prescribed by
# `docs/handoffs/SESSION_HANDOFF_2026-05-14g.md` §"Tier 1 sweep" against the
# Tier 1 structural-rework class additions landed in S70 (commit d77c597):
#   - TP_FRACTION             (new member, default 1.0)
#   - SESSION_OPEN_HOUR       (new member, default 8)
#   - SESSION_CLOSE_HOUR      (new member, default 22)
#   - EWM_VWAP_HALF_LIFE_SEC  (promoted to member, default 7200.0)
# plus three existing class fields newly exposed at the CLI:
#   - EXTENSION_SL_RATIO      (existing field, default 0.60)
#   - MAE_EXIT_RATIO          (existing field, default 0.50)
#   - MIN_SESSION_MIN         (existing field, default 120)
#
# Predecessor: scripts/vrev_sweep_p1.sh (S69 P1) probed the entry-side
# univariate axes (--ext, --max-ext, --max-hold, --cooldown) and concluded
# closed via outputs/VWR_USTEC_PHASE1_RESULTS_2026-05-14e.md + Phase 2
# closeout (S69 P2) -- no edge available from those axes. This Tier 1
# sweep probes the SL geometry, MAE early-exit, session window, EWM
# half-life, and TP geometry axes instead.
#
# Total cells: 30 univariate runs across 6 axes:
#   --min-session-min   : 120, 180, 240, 330, 540       (5 cells)
#   --ext-sl-ratio      : 0.40, 0.50, 0.60, 0.80, 1.00, 1.50  (6 cells)
#   --mae-exit-ratio    : 0.30, 0.40, 0.50, 0.65, 0.80  (5 cells)
#   --tp-fraction       : 0.50, 0.65, 0.75, 0.85, 1.00, 1.15 (6 cells)
#   --ewm-half-life-sec : 1800, 3600, 7200, 14400       (4 cells)
#   session window pair : (8,22), (10,21), (13,21), (13,17)  (4 cells)
#
# Each cell runs in --mode baseline (LOSS_CUT_PCT / BE_ARM_PCT /
# BE_BUFFER_PCT all zeroed) so we're isolating the Tier 1 axis from S63
# in-flight protection. This matches the Phase 1 protocol prior sweeps
# used.
#
# Wall-clock estimate: 30 * ~28s = ~14 min on the 4.4 GB NSXUSD tape
# (per S69 P1 cell timing).
#
# Usage:
#   scripts/vrev_sweep_t1_p1.sh <tape.csv> [output_dir]
#
# Args:
#   tape.csv    Path to the tick CSV. Recommended:
#                 /Users/jo/Tick/NSXUSD_merged.csv  (4.4 GB, matches
#                                                    the S69 P1/P2 sweeps)
#   output_dir  Optional. Default: outputs/vrev_t1_p1_<timestamp>/.
#               outputs/ is gitignored so artifacts stay local; the
#               results memo is the only artifact that gets committed.
#
# Output structure:
#   <output_dir>/
#     phase1_summary.csv          One row per cell + header. Columns:
#                                   axis, level, trades, wins, win_rate_pct,
#                                   gross_pnl, avg_pnl, n_tp_hit, n_sl_hit,
#                                   n_timeout, n_mae_early_exit,
#                                   worst_trade, p95_worst_loss,
#                                   extension_sl_ratio, mae_exit_ratio,
#                                   min_session_min, tp_fraction,
#                                   ewm_half_life_sec, session_open_hour,
#                                   session_close_hour
#                                 The trailing 7 columns echo the effective
#                                 Tier 1 engine config the run actually used,
#                                 so the CSV is self-describing.
#     cells/
#       <axis>_<level>_report.csv  Full per-cell report CSV.
#       <axis>_<level>_trades.csv  Full per-cell trades CSV.
#       <axis>_<level>_stderr.log  Harness stderr (banner + summary).
#
# Self-consistency check the operator can run on the summary:
#   The default-level cells in each axis (min-session-min=120,
#   ext-sl-ratio=0.60, mae-exit-ratio=0.50, tp-fraction=1.00,
#   ewm-half-life-sec=7200, session=(8,22)) are all the SAME run -- each
#   just hits the engine class default for the one swept axis while
#   leaving everything else at defaults. They MUST produce identical
#   (trades, gross_pnl, ...) values. If they don't, something is off in
#   the override-apply ordering and the Tier 1 commit needs investigation
#   BEFORE the Phase 1 results are trusted.
#
# Decision rule (per part-R handoff §"Tier 1 sweep" + scoping memo §8):
#   A cell passes the disqualifying threshold if it produces:
#     - trades       >= 60% of the default-cell trade count (the
#                       handoff cites 4943 as the absolute baseline
#                       reference; default-cell is the like-for-like
#                       comparator here)
#     - tp_rate      >= 5%   (n_tp_hit / trades)
#     - avg_pnl      >= +0.001 per trade
#   An AXIS passes if >= 4 of its (4-6) cells pass the above AND show
#   a monotonic-positive trend across the axis. If no axis passes,
#   Phase 2 refinement is skipped and the closure memo recommends
#   Tier 4 (signal-shape redesign) per scoping memo §4 reasoning.
#
# Decision artifact: outputs/VWR_USTEC_TIER1_PHASE1_RESULTS_<date>.md
# (operator authors this from the summary CSV + cell stderr logs).
# -----------------------------------------------------------------------------

set -euo pipefail

# ---- Args -------------------------------------------------------------------

if [[ $# -lt 1 ]]; then
    cat <<EOF >&2
Usage: $0 <tape.csv> [output_dir]

  tape.csv     Path to the tick CSV (auto-detected format).
               Recommended: /Users/jo/Tick/NSXUSD_merged.csv
  output_dir   Optional. Default: outputs/vrev_t1_p1_<timestamp>/.

See header of $0 for full documentation.
EOF
    exit 1
fi

TAPE="$1"
OUT_DIR="${2:-outputs/vrev_t1_p1_$(date +%Y%m%d_%H%M%S)}"
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

# Sanity: confirm the harness understands the Tier 1 flags. Older binaries
# (from before the S70 commit) reject --tp-fraction with an "Unknown option"
# error which would cause every cell to exit non-zero and crash `set -e`. We
# do a minimal probe with a flag that's safe to set to its default.
if ! "$HARNESS" /dev/null --symbol USTEC.F --tp-fraction 1.0 --limit 1 \
        --quiet --report /dev/null --trades /dev/null \
        2>&1 | grep -q "Unknown option"; then
    : # the harness DID accept --tp-fraction (or failed for other reasons)
else
    echo "[ERROR] Harness does not understand --tp-fraction. The S70 Tier 1" >&2
    echo "        CLI flags are not in this build. Rebuild:" >&2
    echo "          cmake --build build --target VWAPReversionBacktest --config Release -j" >&2
    exit 1
fi

mkdir -p "$OUT_DIR/cells"

SUMMARY="$OUT_DIR/phase1_summary.csv"
echo "axis,level,trades,wins,win_rate_pct,gross_pnl,avg_pnl,n_tp_hit,n_sl_hit,n_timeout,n_mae_early_exit,worst_trade,p95_worst_loss,extension_sl_ratio,mae_exit_ratio,min_session_min,tp_fraction,ewm_half_life_sec,session_open_hour,session_close_hour" > "$SUMMARY"

# ---- Cell runner ------------------------------------------------------------
#
# Args:
#   axis      Logical axis name used in the cell_id + summary CSV (e.g.
#             "min-session-min", "ext-sl-ratio", "tp-fraction", "session").
#   level     Display string for the cell_id + summary CSV (e.g. "120",
#             "0.60", "1.00", "8x22").
#   ...       Remaining args are passed verbatim to the harness in
#             addition to the standard --symbol / --mode / --report /
#             --trades flags. The caller is responsible for matching
#             flag/value pairs and any --session-open-hour /
#             --session-close-hour combo for the session axis.

run_cell() {
    local axis="$1"; shift
    local level="$1"; shift

    local cell_id="${axis}_${level}"
    local report="$OUT_DIR/cells/${cell_id}_report.csv"
    local trades="$OUT_DIR/cells/${cell_id}_trades.csv"
    local stderr_log="$OUT_DIR/cells/${cell_id}_stderr.log"

    printf "[%s] cell %-28s ..." "$(date +%H:%M:%S)" "${axis}=${level}" >&2

    local t0 t1 dur
    t0=$(date +%s)

    "$HARNESS" "$TAPE" \
        --symbol "$SYMBOL" \
        --mode baseline \
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
    local trades_n wins win_rate gross avg tp sl tmo mae worst p95
    local sl_ratio mae_ratio min_sm tpf ewm soh sch

    trades_n=$( awk -F, '/^trades,/             {print $2; exit}' "$report")
    wins=$(     awk -F, '/^wins,/               {print $2; exit}' "$report")
    win_rate=$( awk -F, '/^win_rate_pct,/       {print $2; exit}' "$report")
    gross=$(    awk -F, '/^gross_pnl,/          {print $2; exit}' "$report")
    avg=$(      awk -F, '/^avg_pnl,/            {print $2; exit}' "$report")
    tp=$(       awk -F, '/^n_tp_hit,/           {print $2; exit}' "$report")
    sl=$(       awk -F, '/^n_sl_hit,/           {print $2; exit}' "$report")
    tmo=$(      awk -F, '/^n_timeout,/          {print $2; exit}' "$report")
    mae=$(      awk -F, '/^n_mae_early_exit,/   {print $2; exit}' "$report")
    worst=$(    awk -F, '/^worst_trade,/        {print $2; exit}' "$report")
    p95=$(      awk -F, '/^p95_worst_loss,/     {print $2; exit}' "$report")
    sl_ratio=$( awk -F, '/^extension_sl_ratio,/ {print $2; exit}' "$report")
    mae_ratio=$(awk -F, '/^mae_exit_ratio,/     {print $2; exit}' "$report")
    min_sm=$(   awk -F, '/^min_session_min,/    {print $2; exit}' "$report")
    tpf=$(      awk -F, '/^tp_fraction,/        {print $2; exit}' "$report")
    ewm=$(      awk -F, '/^ewm_half_life_sec,/  {print $2; exit}' "$report")
    soh=$(      awk -F, '/^session_open_hour,/  {print $2; exit}' "$report")
    sch=$(      awk -F, '/^session_close_hour,/ {print $2; exit}' "$report")

    echo "${axis},${level},${trades_n},${wins},${win_rate},${gross},${avg},${tp},${sl},${tmo},${mae},${worst},${p95},${sl_ratio},${mae_ratio},${min_sm},${tpf},${ewm},${soh},${sch}" >> "$SUMMARY"

    printf " trades=%-6s tp=%-4s gross=%-12s avg=%-12s  (%ds)\n" \
        "${trades_n}" "${tp}" "${gross}" "${avg}" "${dur}" >&2
}

# ---- Sweep header -----------------------------------------------------------

echo "===============================================================" >&2
echo "  Tier 1 Phase 1 univariate sweep -- VWR USTEC.F" >&2
echo "  Tape    : $TAPE" >&2
echo "  Symbol  : $SYMBOL" >&2
echo "  Output  : $OUT_DIR" >&2
echo "  Summary : $SUMMARY" >&2
echo "  Mode    : --mode baseline (S63 trio = 0.0)" >&2
echo "  Cells   : 30 across 6 axes (~14 min wall-clock estimate)" >&2
echo "===============================================================" >&2
echo "" >&2

SWEEP_T0=$(date +%s)

# ---- Axis 1: --min-session-min (5 cells) ------------------------------------
# Class default 120 (2h after London open). Higher levels delay entry into
# the NY/London overlap and beyond.
echo "--- Axis 1/6: --min-session-min ---" >&2
for L in 120 180 240 330 540; do
    run_cell "min-session-min" "$L" --min-session-min "$L"
done

# ---- Axis 2: --ext-sl-ratio (6 cells) ---------------------------------------
# Class default 0.60. Tightens (0.40) or loosens (1.50) SL distance beyond
# entry as a ratio of extension. Tighter SL = smaller stops, more SL hits.
echo "--- Axis 2/6: --ext-sl-ratio ---" >&2
for L in 0.40 0.50 0.60 0.80 1.00 1.50; do
    run_cell "ext-sl-ratio" "$L" --ext-sl-ratio "$L"
done

# ---- Axis 3: --mae-exit-ratio (5 cells) -------------------------------------
# Class default 0.50. The fraction of TP distance the trade is allowed to
# go adverse before MAE_EARLY_EXIT fires. Tighter (0.30) = quicker exit
# on hostile moves.
echo "--- Axis 3/6: --mae-exit-ratio ---" >&2
for L in 0.30 0.40 0.50 0.65 0.80; do
    run_cell "mae-exit-ratio" "$L" --mae-exit-ratio "$L"
done

# ---- Axis 4: --tp-fraction (6 cells) ----------------------------------------
# Class default 1.00 (TP at VWAP, full reversion). <1.0 = partial reversion
# target (shorter TP distance, easier to hit). >1.0 = overshoot VWAP.
echo "--- Axis 4/6: --tp-fraction ---" >&2
for L in 0.50 0.65 0.75 0.85 1.00 1.15; do
    run_cell "tp-fraction" "$L" --tp-fraction "$L"
done

# ---- Axis 5: --ewm-half-life-sec (4 cells) ----------------------------------
# Class default 7200.0 (2hr half-life). Lower = faster-decaying VWAP proxy,
# more sensitive to recent price action.
echo "--- Axis 5/6: --ewm-half-life-sec ---" >&2
for L in 1800 3600 7200 14400; do
    run_cell "ewm-half-life-sec" "$L" --ewm-half-life-sec "$L"
done

# ---- Axis 6: session window (4 cells) ---------------------------------------
# Class default (8, 22) -> London open through NY close UTC. Other pairs:
#   (10, 21)  : skip the London open hour, end one hour earlier
#   (13, 21)  : NY/London overlap onwards only (1:30pm UTC = NY open)
#   (13, 17)  : NY/London overlap window only -- the highest mean-rev edge
#               historically per the engine's confluence bonus.
echo "--- Axis 6/6: session window (open, close) ---" >&2
for PAIR in "8 22" "10 21" "13 21" "13 17"; do
    OPEN_H=$(echo "$PAIR" | awk '{print $1}')
    CLOSE_H=$(echo "$PAIR" | awk '{print $2}')
    LABEL="${OPEN_H}x${CLOSE_H}"
    run_cell "session" "$LABEL" --session-open-hour "$OPEN_H" --session-close-hour "$CLOSE_H"
done

SWEEP_T1=$(date +%s)
SWEEP_DUR=$((SWEEP_T1 - SWEEP_T0))

# ---- Summary ----------------------------------------------------------------

echo "" >&2
echo "===============================================================" >&2
echo "  Tier 1 Phase 1 sweep complete -- ${SWEEP_DUR}s wall-clock (30 cells)" >&2
echo "===============================================================" >&2
echo "" >&2

# Pretty-print only the key columns (axis, level, trades, tp_hit, gross,
# avg) so the terminal output stays readable. Full data is in the summary
# CSV.
if command -v column >/dev/null 2>&1; then
    awk -F, 'BEGIN{OFS=","}{print $1,$2,$3,$8,$6,$7}' "$SUMMARY" | column -t -s, >&2
else
    awk -F, 'BEGIN{OFS=","}{print $1,$2,$3,$8,$6,$7}' "$SUMMARY" >&2
fi

echo "" >&2
echo "Summary CSV (full)  : $SUMMARY" >&2
echo "Cell reports        : $OUT_DIR/cells/" >&2
echo "" >&2

# ---- Decision-rule hint -----------------------------------------------------

echo "Decision rule (per part-R handoff + scoping memo §8):" >&2
echo "  A cell PASSES if all of:" >&2
echo "    trades   >= 60% of the default-cell trade count" >&2
echo "    tp_rate  >= 5%   (n_tp_hit / trades)" >&2
echo "    avg_pnl  >= +0.001 per trade" >&2
echo "  An AXIS passes if >= 4 of its cells pass AND show a monotonic-" >&2
echo "  positive trend." >&2
echo "" >&2
echo "  If NO axis passes: skip Phase 2; recommend Tier 4 (signal-shape" >&2
echo "  redesign). The closure memo lives at:" >&2
echo "    outputs/VWR_USTEC_TIER1_PHASE1_RESULTS_$(date +%Y-%m-%d).md" >&2
echo "" >&2

# Self-consistency: the default-level cell in each axis is the SAME run
# (all axes at engine class defaults, only the one explicitly-overridden
# axis being at its default level). They should produce identical
# (trades, gross_pnl, avg_pnl, n_tp_hit, ...) values. Drift here means
# an override-apply bug.
echo "Self-consistency check (these 6 default-level cells should be identical --" >&2
echo "all run the engine class defaults; the explicit --flag X just sets X to" >&2
echo "its own class default):" >&2
awk -F, '
    ($1=="min-session-min"  && $2=="120")    { print "  min-session-min=120  :", $0 }
    ($1=="ext-sl-ratio"     && $2=="0.60")   { print "  ext-sl-ratio=0.60    :", $0 }
    ($1=="mae-exit-ratio"   && $2=="0.50")   { print "  mae-exit-ratio=0.50  :", $0 }
    ($1=="tp-fraction"      && $2=="1.00")   { print "  tp-fraction=1.00     :", $0 }
    ($1=="ewm-half-life-sec"&& $2=="7200")   { print "  ewm-half-life=7200   :", $0 }
    ($1=="session"          && $2=="8x22")   { print "  session=(8,22)       :", $0 }
' "$SUMMARY" >&2

echo "" >&2
