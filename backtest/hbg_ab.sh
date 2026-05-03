#!/usr/bin/env bash
# =============================================================================
# hbg_ab.sh -- HBG A/B harness with cached baseline + windowed runs
# 2026-05-01 -- replaces the ad-hoc shell pipe that re-ran A from scratch
#               every iteration (4h 30m wall time per A/B).
#
# DESIGN
#   * A (BASELINE, pre-S52, commit 1e0d50f0) is run AT MOST ONCE EVER.
#     Results cached to backtest/baselines/. Subsequent runs reuse the
#     cache. Force re-run with --rebuild-baseline (rare; use when the
#     baseline include/ tree itself changes).
#
#   * B (current main, or whatever HEAD is checked out) is built fresh
#     and run every invocation. This is the only thing that ever changes
#     between iterations, so it's the only thing that needs to re-run.
#
#   * Default window is the last 6 months (2025-11-01 -> 2026-04-30) where
#     ~80% of trades concentrate per the by-month breakdown. Yields a
#     ~30min B run instead of 2h 15m. Use --window full for the full
#     26-month dataset (final acceptance gate only).
#
#   * Both runs use the same window. Apples-to-apples.
#
# USAGE
#   hbg_ab.sh                              # cached baseline, recent window B
#   hbg_ab.sh --window full                # cached baseline, full-window B
#   hbg_ab.sh --from 2026-01-01 --to 2026-04-30
#   hbg_ab.sh --rebuild-baseline           # nuke cache, re-run A from 1e0d50f0
#   hbg_ab.sh --duka /path/to/ticks.csv    # override default Dukascopy path
#
# OUTPUT
#   /tmp/hbg_ab_<window>/{baseline,s52}_summary.txt
#   /tmp/hbg_ab_<window>/{baseline,s52}_trades.csv
#   stdout: side-by-side headline + exit-reason comparison
#
# REPO LAYOUT
#   ${REPO}/backtest/baselines/hbg_baseline_1e0d50f0_<window>.txt   # cached
#   ${REPO}/backtest/baselines/hbg_baseline_1e0d50f0_<window>.csv   # cached
#   ${REPO}/backtest/hbg_duka_bt.cpp                                # source
# =============================================================================

set -euo pipefail

# ---- defaults ---------------------------------------------------------------
REPO="${OMEGA_REPO:-$HOME/omega_repo}"
DUKA_DEFAULT="$HOME/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv"
DUKA="$DUKA_DEFAULT"
BASELINE_COMMIT="1e0d50f0"
WINDOW_MODE="recent"            # recent | full | custom
FROM=""                         # YYYY-MM-DD (custom) or auto-set by mode
TO=""
REBUILD_BASELINE="0"

# ---- arg parsing ------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --window)
            shift
            case "$1" in
                full|recent) WINDOW_MODE="$1" ;;
                *) echo "ERROR: --window must be 'full' or 'recent', got '$1'" >&2; exit 1 ;;
            esac
            ;;
        --from)             shift; FROM="$1"; WINDOW_MODE="custom" ;;
        --to)               shift; TO="$1";   WINDOW_MODE="custom" ;;
        --duka)             shift; DUKA="$1" ;;
        --rebuild-baseline) REBUILD_BASELINE="1" ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "ERROR: unknown arg '$1' (try --help)" >&2; exit 1 ;;
    esac
    shift
done

# Resolve window mode -> FROM/TO
case "$WINDOW_MODE" in
    recent) FROM="2025-11-01"; TO="2026-04-30" ;;
    full)   FROM="";           TO="" ;;
    custom) : ;;  # FROM/TO already set by --from/--to
esac

# Compose --from/--to args for hbg_duka_bt
HBG_ARGS=("$DUKA")
[[ -n "$FROM" ]] && HBG_ARGS+=("--from" "$FROM")
[[ -n "$TO"   ]] && HBG_ARGS+=("--to"   "$TO")

# Window tag for filenames + scratch dir
WINDOW_TAG="${WINDOW_MODE}"
[[ "$WINDOW_MODE" == "custom" ]] && WINDOW_TAG="${FROM:-start}_${TO:-end}"

SCRATCH="/tmp/hbg_ab_${WINDOW_TAG}"
BASELINE_DIR="${REPO}/backtest/baselines"
BASELINE_TXT="${BASELINE_DIR}/hbg_baseline_${BASELINE_COMMIT}_${WINDOW_TAG}.txt"
BASELINE_CSV="${BASELINE_DIR}/hbg_baseline_${BASELINE_COMMIT}_${WINDOW_TAG}.csv"

mkdir -p "$SCRATCH" "$BASELINE_DIR"

# ---- pre-flight -------------------------------------------------------------
[[ -f "$DUKA" ]] || { echo "ERROR: Dukascopy file not found: $DUKA" >&2; exit 1; }

echo "==================================================================="
echo "HBG A/B HARNESS"
echo "==================================================================="
echo "  Repo            : $REPO"
echo "  Duka file       : $DUKA"
echo "  Baseline commit : $BASELINE_COMMIT"
echo "  Window          : $WINDOW_MODE   (from=${FROM:-start}  to=${TO:-end})"
echo "  Scratch         : $SCRATCH"
echo "  Cached baseline : $BASELINE_TXT"
echo "==================================================================="

# ---- A: BASELINE ------------------------------------------------------------
if [[ "$REBUILD_BASELINE" == "1" ]] || [[ ! -f "$BASELINE_TXT" ]] || [[ ! -f "$BASELINE_CSV" ]]; then
    if [[ "$REBUILD_BASELINE" == "1" ]]; then
        echo
        echo ">>> A: rebuilding baseline cache (--rebuild-baseline)"
    else
        echo
        echo ">>> A: no cache for window '$WINDOW_TAG', running baseline ONCE"
    fi

    BLDIR="$SCRATCH/baseline_build"
    rm -rf "$BLDIR"
    mkdir -p "$BLDIR/include"
    cp -r "$REPO/include/." "$BLDIR/include/"
    # Overlay the pre-S52 GoldHybridBracketEngine.hpp on top of the current tree
    git -C "$REPO" show "${BASELINE_COMMIT}:include/GoldHybridBracketEngine.hpp" \
        > "$BLDIR/include/GoldHybridBracketEngine.hpp"

    g++ -O2 -std=c++17 -DOMEGA_BACKTEST -I "$BLDIR/include" \
        -o "$BLDIR/hbg_baseline" "$REPO/backtest/hbg_duka_bt.cpp"

    "$BLDIR/hbg_baseline" "${HBG_ARGS[@]}" | tee "$BASELINE_TXT"
    mv hbg_duka_bt_trades.csv "$BASELINE_CSV"
    echo "  [OK] cached baseline -> $BASELINE_TXT"
else
    echo
    echo ">>> A: using cached baseline ($BASELINE_TXT)"
fi
cp "$BASELINE_TXT" "$SCRATCH/baseline_summary.txt"
cp "$BASELINE_CSV" "$SCRATCH/baseline_trades.csv"

# ---- B: CURRENT MAIN --------------------------------------------------------
echo
echo ">>> B: building + running current HEAD"
HEAD_SHA=$(git -C "$REPO" rev-parse --short HEAD)
echo "  HEAD = $HEAD_SHA"

g++ -O2 -std=c++17 -DOMEGA_BACKTEST -I "$REPO/include" \
    -o "$SCRATCH/hbg_s52" "$REPO/backtest/hbg_duka_bt.cpp"

"$SCRATCH/hbg_s52" "${HBG_ARGS[@]}" | tee "$SCRATCH/s52_summary.txt"
mv hbg_duka_bt_trades.csv "$SCRATCH/s52_trades.csv"

# ---- COMPARISON -------------------------------------------------------------
echo
echo "==================================================================="
echo "HEADLINE NUMBERS  (Baseline ${BASELINE_COMMIT}  ->  HEAD ${HEAD_SHA})"
echo "==================================================================="
extract() { grep -F "$1" "$2" 2>/dev/null | head -1 | awk -F: '{print $2}' | xargs; }
for k in "Trades" "Win rate" "Net PnL" "Avg / trade" "Max DD"; do
    a=$(extract "$k" "$SCRATCH/baseline_summary.txt")
    b=$(extract "$k" "$SCRATCH/s52_summary.txt")
    printf "  %-13s %-20s  ->  %s\n" "$k" "${a:-?}" "${b:-?}"
done

echo
echo "EXIT REASONS"
echo "  --- Baseline ---"
sed -n '/^Exit reasons:/,/^$/p' "$SCRATCH/baseline_summary.txt" | sed 's/^/    /'
echo "  --- HEAD ---"
sed -n '/^Exit reasons:/,/^$/p' "$SCRATCH/s52_summary.txt"      | sed 's/^/    /'

echo
echo "FILES"
echo "  $SCRATCH/baseline_summary.txt"
echo "  $SCRATCH/baseline_trades.csv"
echo "  $SCRATCH/s52_summary.txt"
echo "  $SCRATCH/s52_trades.csv"
echo
echo "Trade-level diff:"
echo "  diff <(cut -d, -f1-5 '$SCRATCH/baseline_trades.csv' | sort) \\"
echo "       <(cut -d, -f1-5 '$SCRATCH/s52_trades.csv'      | sort)"
echo "==================================================================="
