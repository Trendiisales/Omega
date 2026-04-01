#!/bin/bash
# Parameter grid search for GoldFlowEngine
# Tests combinations of SL_MULT, STEP1_TRIGGER, MAX_HOLD
# Outputs ranked results to backtest/results/grid_results.csv

BINARY="backtest/OmegaBacktest_mac"
DATA="/Users/jo/tick/data/xauusd_2024_2025.csv"
RESULTS="backtest/results/grid_results.csv"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

mkdir -p "$REPO/backtest/results"
cd "$REPO"

echo "sl_mult,step1_trigger,max_hold_min,trades,wr_pct,pnl,avg_pnl,sl_hits,trail_hits,max_hold_exits,dd" > "$RESULTS"

SL_MULTS="1.0 1.5 2.0 2.5"
STEP1_TRIGGERS="15 20 30 40"
MAX_HOLDS="900000 1800000 3600000"   # ms: 15min 30min 60min

total=$(echo "$SL_MULTS" | wc -w)
total=$(($(echo "$SL_MULTS" | wc -w) * $(echo "$STEP1_TRIGGERS" | wc -w) * $(echo "$MAX_HOLDS" | wc -w)))
run=0

for sl in $SL_MULTS; do
for step1 in $STEP1_TRIGGERS; do
for hold in $MAX_HOLDS; do
    run=$((run+1))
    hold_min=$((hold/60000))
    echo -n "[$run/$total] SL=${sl}x STEP1=\$${step1} HOLD=${hold_min}min ... "

    # Patch constants via -D override flags in build
    EXTRA_FLAGS="-DGFE_ATR_SL_MULT_OVERRIDE=${sl} -DGFE_STEP1_OVERRIDE=${step1} -DGFE_MAX_HOLD_OVERRIDE=${hold}"

    # Build with overrides
    clang++ -O3 -std=c++17 \
        -DGFE_ATR_SL_MULT_OVERRIDE=${sl} \
        -DGFE_STEP1_OVERRIDE=${step1} \
        -DGFE_MAX_HOLD_OVERRIDE=${hold} \
        -I include \
        backtest/OmegaBacktest.cpp \
        -o backtest/OmegaBacktest_grid 2>/dev/null

    if [ $? -ne 0 ]; then
        echo "BUILD FAILED"
        continue
    fi

    TRD="backtest/results/grid_${sl}_${step1}_${hold}.csv"
    backtest/OmegaBacktest_grid "$DATA" --engine flow --warmup 10000 --quiet --trades "$TRD" 2>/dev/null

    # Parse results
    if [ ! -f "$TRD" ]; then echo "NO OUTPUT"; continue; fi

    stats=$(awk -F',' '
    NR>1 {
        total++
        pnl+=$7
        if($7>0){wins++; wpnl+=$7} else {lpnl+=$7}
        if($11=="SL_HIT") sl++
        if($11=="TRAIL_HIT") trail++
        if($11=="MAX_HOLD_TIMEOUT") mh++
        if(pnl>peak) peak=pnl
        dd_val = peak-pnl
        if(dd_val>dd) dd=dd_val
    }
    END {
        wr = total>0 ? 100*wins/total : 0
        avg = total>0 ? pnl/total : 0
        printf "%d,%.1f,%.2f,%.4f,%d,%d,%d,%.2f", total, wr, pnl, avg, sl, trail, mh, dd
    }' "$TRD")

    echo "$sl,$step1,$hold_min,$stats" >> "$RESULTS"
    echo "T=$(echo $stats | cut -d, -f1) WR=$(echo $stats | cut -d, -f2)% PnL=$(echo $stats | cut -d, -f3)"
    rm -f "$TRD"
done
done
done

echo ""
echo "=== TOP 10 BY PNL ==="
echo "sl_mult,step1,hold_min,trades,wr,pnl,avg,sl_hits,trail_hits,mh_exits,dd"
sort -t',' -k6 -rn "$RESULTS" | grep -v "sl_mult" | head -10

echo ""
echo "Full results: $RESULTS"
