#!/bin/bash
# ============================================================
#  Walk-forward parameter grid for GoldFlow
#  Runs OmegaBacktest_mac across ATR multiplier + drift combos
#  on 3 time periods to find robust parameters.
#
#  Usage:
#    bash backtest/run_param_grid.sh /path/to/xauusd_2024_2025.csv
#
#  Output: backtest/grid_results.csv
# ============================================================
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BT="$REPO/backtest/OmegaBacktest_mac"
TICK_FILE="${1:-/Users/jo/tick/data/xauusd_2024_2025.csv}"
OUT="$REPO/backtest/grid_results.csv"
TMPDIR="$REPO/backtest/grid_tmp"
mkdir -p "$TMPDIR"

if [ ! -f "$BT" ]; then
    echo "Build first: bash backtest/build_mac.sh"
    exit 1
fi

if [ ! -f "$TICK_FILE" ]; then
    echo "Tick file not found: $TICK_FILE"
    exit 1
fi

echo "tick_file=$TICK_FILE"
echo "output=$OUT"
echo ""

# Split data into 3 periods for walk-forward validation
# Period 1: 2024-01 to 2024-06  (ts < 1719792000000)
# Period 2: 2024-07 to 2024-12  (ts 1719792000000 - 1735689600000)
# Period 3: 2025-01 to 2025-10  (ts > 1735689600000)
echo "Splitting tick data into 3 periods..."
python3 - "$TICK_FILE" "$TMPDIR" << 'PYEOF'
import sys, os
src = sys.argv[1]
dst = sys.argv[2]
cuts = [1719792000000, 1735689600000]  # Jul 2024, Jan 2025
files = [open(f"{dst}/period_{i+1}.csv", 'w') for i in range(3)]
with open(src) as f:
    header = f.readline()
    for fh in files: fh.write(header)
    for line in f:
        ts = int(line.split(',')[0])
        if   ts < cuts[0]: files[0].write(line)
        elif ts < cuts[1]: files[1].write(line)
        else:              files[2].write(line)
for fh in files: fh.close()
for i in range(3):
    n = sum(1 for _ in open(f"{dst}/period_{i+1}.csv")) - 1
    print(f"  Period {i+1}: {n:,} ticks")
PYEOF

echo ""
echo "Running parameter grid..."
echo "atr_sl_mult,drift_min,compression_vol_floor,vwap_room_r,period,trades,wr_pct,pnl,sharpe,maxdd" > "$OUT"

# Note: OmegaBacktest uses engine headers directly -- params are compiled in.
# For the grid, we rebuild with different #defines each time.
# This is slower but gives real results with actual GoldFlowEngine logic.

WARMUP=10000

# Grid dimensions
ATR_MULTS="0.8 1.0 1.2"          # GFE_ATR_SL_MULT
DRIFT_MINS="0.3 0.5 0.8"         # GFE_ASIA_DRIFT_MIN  
COMP_FLOORS="1.0 2.0 3.0"        # GF_COMPRESSION_VOL_FLOOR
VWAP_ROOMS="1.0 1.5 2.0"         # GF_MIN_VWAP_ROOM_R

TOTAL=$(echo "$ATR_MULTS $DRIFT_MINS $COMP_FLOORS $VWAP_ROOMS" | wc -w)
COMBOS=$(( $(echo $ATR_MULTS | wc -w) * $(echo $DRIFT_MINS | wc -w) * $(echo $COMP_FLOORS | wc -w) * $(echo $VWAP_ROOMS | wc -w) ))
echo "Running $COMBOS parameter combinations × 3 periods = $(($COMBOS * 3)) backtests"
echo "Estimated time: $(($COMBOS * 3 * 15 / 60)) minutes at ~15s/run"
echo ""

N=0
for ATR in $ATR_MULTS; do
for DRIFT in $DRIFT_MINS; do
for COMP in $COMP_FLOORS; do
for VWAP in $VWAP_ROOMS; do
    N=$((N+1))
    printf "\r[%d/%d] ATR=%.1f DRIFT=%.1f COMP=%.1f VWAP=%.1f" $N $COMBOS $ATR $DRIFT $COMP $VWAP

    # Rebuild with these params
    TMPBIN="$TMPDIR/bt_${ATR}_${DRIFT}_${COMP}_${VWAP}"
    clang++ -O3 -std=c++20 \
        -o "$TMPBIN" \
        "$REPO/backtest/OmegaBacktest.cpp" \
        "$REPO/src/SymbolConfig.cpp" \
        -I"$REPO" -I"$REPO/include" -I"$REPO/src" -I"$REPO/backtest" \
        -include "$REPO/backtest/OmegaTimeShim.hpp" \
        -include "$REPO/backtest/mac_compat.hpp" \
        -DOMEGA_BACKTEST \
        -DGFE_ATR_SL_MULT_OVERRIDE=$ATR \
        -DGFE_ASIA_DRIFT_MIN_OVERRIDE=$DRIFT \
        -DGF_COMPRESSION_VOL_FLOOR_OVERRIDE=$COMP \
        -DGF_MIN_VWAP_ROOM_R_OVERRIDE=$VWAP \
        -Wno-unused-parameter -Wno-unused-function -Wno-reorder \
        -Wno-unknown-pragmas -Wno-deprecated-declarations \
        -pthread 2>/dev/null

    # Run on all 3 periods
    for P in 1 2 3; do
        PERIOD_FILE="$TMPDIR/period_${P}.csv"
        REPORT="$TMPDIR/report_${ATR}_${DRIFT}_${COMP}_${VWAP}_p${P}.csv"
        "$TMPBIN" "$PERIOD_FILE" --engine flow --warmup $WARMUP --quiet --report "$REPORT" > /dev/null 2>&1
        # Extract flow line from report
        if [ -f "$REPORT" ]; then
            FLOWLINE=$(grep "GoldFlow\|flow" "$REPORT" 2>/dev/null | head -1)
            if [ -n "$FLOWLINE" ]; then
                TRADES=$(echo "$FLOWLINE" | cut -d',' -f2)
                WR=$(echo "$FLOWLINE"    | cut -d',' -f3)
                PNL=$(echo "$FLOWLINE"   | cut -d',' -f4)
                SHARPE=$(echo "$FLOWLINE"| cut -d',' -f8)
                DD=$(echo "$FLOWLINE"   | cut -d',' -f6)
                echo "$ATR,$DRIFT,$COMP,$VWAP,$P,$TRADES,$WR,$PNL,$SHARPE,$DD" >> "$OUT"
            fi
        fi
    done
    rm -f "$TMPBIN"
done
done
done
done

echo ""
echo ""
echo "Grid complete. Results: $OUT"
echo ""

# Find best combinations (positive on all 3 periods)
python3 - "$OUT" << 'PYEOF'
import csv, sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1])))
# Group by params
combos = defaultdict(list)
for r in rows:
    key = (r['atr_sl_mult'], r['drift_min'], r['compression_vol_floor'], r['vwap_room_r'])
    combos[key].append(float(r['pnl']))

print("\n=== ROBUST PARAMETER COMBINATIONS (positive on all 3 periods) ===")
robust = [(k, v) for k, v in combos.items() if len(v)==3 and all(p>0 for p in v)]
robust.sort(key=lambda x: sum(x[1]), reverse=True)
print(f"{'ATR':>6} {'DRIFT':>6} {'COMP':>6} {'VWAP':>6}  {'P1':>8} {'P2':>8} {'P3':>8}  {'TOTAL':>9}")
print("-"*70)
for (atr,dr,comp,vwap), pnls in robust[:20]:
    print(f"{atr:>6} {dr:>6} {comp:>6} {vwap:>6}  "
          f"{pnls[0]:>+8.2f} {pnls[1]:>+8.2f} {pnls[2]:>+8.2f}  {sum(pnls):>+9.2f}")

if not robust:
    print("No combinations positive on all 3 periods.")
    print("\nTop 10 by total PnL:")
    all_combos = [(k, v) for k, v in combos.items() if len(v)==3]
    all_combos.sort(key=lambda x: sum(x[1]), reverse=True)
    for (atr,dr,comp,vwap), pnls in all_combos[:10]:
        wins = sum(1 for p in pnls if p>0)
        print(f"  ATR={atr} DRIFT={dr} COMP={comp} VWAP={vwap}  "
              f"periods={wins}/3  total={sum(pnls):+.2f}")
PYEOF
