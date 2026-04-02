#!/bin/bash
# =============================================================================
# walk_forward.sh -- Walk-Forward OOS Validation for Omega GoldFlow
# =============================================================================
# True walk-forward: train on IS window, test on OOS window, roll forward.
#
# Structure (each fold):
#   IS  = 3 months in-sample  (optimise params)
#   OOS = 1 month out-of-sample (validate -- no fitting)
#   Step = 1 month roll
#
# 2024-01 to 2026-01 = 24 months -> 21 folds
#
# Output: backtest/wf_results.csv
#   fold, is_start, is_end, oos_start, oos_end, params*, is_pnl, oos_pnl,
#   is_wr, oos_wr, is_sharpe, oos_sharpe, degradation_pct
#
# Degradation < 40% = robust (OOS keeps >60% of IS performance)
# Degradation > 60% = overfit (don't use those params)
#
# Usage:
#   bash backtest/walk_forward.sh /path/to/xauusd_ticks.csv
#   bash backtest/walk_forward.sh /path/to/xauusd_ticks.csv --quick  (fewer params)
#
# Requires OmegaBacktest binary (build with: bash backtest/build_mac.sh)
# =============================================================================
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BT="$REPO/backtest/OmegaBacktest_mac"
TICK_FILE="${1:-/Users/jo/tick/data/xauusd_2024_2025.csv}"
QUICK="${2:-}"
OUT="$REPO/backtest/wf_results.csv"
TMPDIR="$REPO/backtest/wf_tmp"
mkdir -p "$TMPDIR"

if [ ! -f "$BT" ]; then
    echo "Build first: bash backtest/build_mac.sh"
    exit 1
fi
if [ ! -f "$TICK_FILE" ]; then
    echo "Tick file not found: $TICK_FILE"
    exit 1
fi

# =============================================================================
# STEP 1: Split tick data into monthly chunks
# =============================================================================
echo "[WF] Splitting tick data into monthly chunks..."
python3 - "$TICK_FILE" "$TMPDIR" << 'PYEOF'
import sys, os
from datetime import datetime, timezone

src  = sys.argv[1]
dst  = sys.argv[2]

# Monthly boundaries from 2024-01-01 to 2026-02-01
months = []
for year in [2024, 2025, 2026]:
    for month in range(1, 13):
        if year == 2026 and month > 2: break
        dt = datetime(year, month, 1, tzinfo=timezone.utc)
        months.append(int(dt.timestamp() * 1000))

# Open file handles for each month
handles = {}
for i in range(len(months) - 1):
    fname = f"{dst}/month_{i:02d}_{months[i]}.csv"
    handles[i] = open(fname, 'w')

counts = {i: 0 for i in range(len(months) - 1)}

with open(src) as f:
    header = f.readline()
    for h in handles.values():
        h.write(header)
    for line in f:
        try:
            ts = int(line.split(',')[0])
        except:
            continue
        for i in range(len(months) - 1):
            if months[i] <= ts < months[i + 1]:
                handles[i].write(line)
                counts[i] += 1
                break

for h in handles.values():
    h.close()

# Write month manifest
with open(f"{dst}/manifest.txt", 'w') as mf:
    for i in range(len(months) - 1):
        dt = datetime.fromtimestamp(months[i] / 1000, tz=timezone.utc)
        mf.write(f"{i:02d},{months[i]},{dt.strftime('%Y-%m')},{counts[i]}\n")
    print(f"  {len(months)-1} months, manifest written")
    for i, c in counts.items():
        if c > 0:
            dt = datetime.fromtimestamp(months[i]/1000, tz=timezone.utc)
            print(f"  Month {i:02d} ({dt.strftime('%Y-%m')}): {c:,} ticks")
PYEOF

# =============================================================================
# STEP 2: Define parameter grid (quick vs full)
# =============================================================================
if [ "$QUICK" = "--quick" ]; then
    ATR_MULTS="1.0 1.2"
    DRIFT_MINS="0.3 0.5"
    COMP_FLOORS="0.8 2.0"
    echo "[WF] QUICK mode: reduced grid"
else
    ATR_MULTS="0.8 1.0 1.2 1.5"
    DRIFT_MINS="0.3 0.5 0.8 1.0"
    COMP_FLOORS="0.5 0.8 1.5 2.0"
    echo "[WF] FULL mode: $(( $(echo $ATR_MULTS|wc -w) * $(echo $DRIFT_MINS|wc -w) * $(echo $COMP_FLOORS|wc -w) )) param combos per fold"
fi

IS_MONTHS=3   # in-sample window
OOS_MONTHS=1  # out-of-sample window
WARMUP=5000

# =============================================================================
# STEP 3: Walk-forward loop
# =============================================================================
MANIFEST="$TMPDIR/manifest.txt"
TOTAL_MONTHS=$(wc -l < "$MANIFEST")
MAX_FOLD=$(( TOTAL_MONTHS - IS_MONTHS - OOS_MONTHS ))

echo "[WF] Walk-forward: IS=${IS_MONTHS}mo OOS=${OOS_MONTHS}mo, ${MAX_FOLD} folds"
echo ""

# Output CSV header
echo "fold,is_start,is_end,oos_start,oos_end,atr_mult,drift_min,comp_floor,\
is_trades,is_wr,is_pnl,is_sharpe,\
oos_trades,oos_wr,oos_pnl,oos_sharpe,\
degradation_pct,robust" > "$OUT"

fold=0
for is_start_idx in $(seq 0 $MAX_FOLD); do
    is_end_idx=$(( is_start_idx + IS_MONTHS - 1 ))
    oos_start_idx=$(( is_end_idx + 1 ))
    oos_end_idx=$(( oos_start_idx + OOS_MONTHS - 1 ))

    if [ $oos_end_idx -ge $TOTAL_MONTHS ]; then break; fi

    # Get date labels from manifest
    is_start=$(sed -n "$((is_start_idx+1))p" "$MANIFEST" | cut -d, -f3)
    is_end=$(sed -n "$((is_end_idx+1))p"   "$MANIFEST" | cut -d, -f3)
    oos_start=$(sed -n "$((oos_start_idx+1))p" "$MANIFEST" | cut -d, -f3)
    oos_end=$(sed -n "$((oos_end_idx+1))p"     "$MANIFEST" | cut -d, -f3)

    echo "[WF] Fold $fold: IS=$is_start-$is_end  OOS=$oos_start-$oos_end"

    # Concatenate IS months into one file
    IS_FILE="$TMPDIR/fold_${fold}_is.csv"
    OOS_FILE="$TMPDIR/fold_${fold}_oos.csv"

    head -1 "$TMPDIR/month_$(printf '%02d' $is_start_idx)_"*.csv 2>/dev/null | head -1 > "$IS_FILE" 2>/dev/null || true
    for m in $(seq $is_start_idx $is_end_idx); do
        mfile="$TMPDIR/month_$(printf '%02d' $m)_"*.csv
        [ -f $mfile ] && tail -n +2 $mfile >> "$IS_FILE"
    done

    head -1 "$TMPDIR/month_$(printf '%02d' $oos_start_idx)_"*.csv 2>/dev/null | head -1 > "$OOS_FILE" 2>/dev/null || true
    for m in $(seq $oos_start_idx $oos_end_idx); do
        mfile="$TMPDIR/month_$(printf '%02d' $m)_"*.csv
        [ -f $mfile ] && tail -n +2 $mfile >> "$OOS_FILE"
    done

    # Grid search on IS to find best params
    BEST_IS_SHARPE=-999
    BEST_ATR="1.0"; BEST_DRIFT="0.5"; BEST_COMP="0.8"
    BEST_IS_STATS=""

    for ATR in $ATR_MULTS; do
    for DRIFT in $DRIFT_MINS; do
    for COMP in $COMP_FLOORS; do

        # Rebuild with these params
        RESULT=$(cmake --build "$REPO/build" --target OmegaBacktest \
            --config Release -- \
            -DGFE_ATR_SL_MULT_OVERRIDE=$ATR \
            -DGF_COMPRESSION_VOL_FLOOR_OVERRIDE=$COMP \
            2>/dev/null \
            && "$BT" "$IS_FILE" --warmup $WARMUP --drift-min $DRIFT \
            2>/dev/null | tail -1) || RESULT=""

        if [ -z "$RESULT" ]; then continue; fi

        IS_SHARPE=$(echo "$RESULT" | awk -F, '{print $NF}')
        if (( $(echo "$IS_SHARPE > $BEST_IS_SHARPE" | bc -l 2>/dev/null || echo 0) )); then
            BEST_IS_SHARPE=$IS_SHARPE
            BEST_ATR=$ATR; BEST_DRIFT=$DRIFT; BEST_COMP=$COMP
            BEST_IS_STATS=$RESULT
        fi
    done
    done
    done

    echo "  Best IS params: ATR=$BEST_ATR DRIFT=$BEST_DRIFT COMP=$BEST_COMP Sharpe=$BEST_IS_SHARPE"

    # Validate best IS params on OOS
    OOS_RESULT=$(cmake --build "$REPO/build" --target OmegaBacktest \
        --config Release -- \
        -DGFE_ATR_SL_MULT_OVERRIDE=$BEST_ATR \
        -DGF_COMPRESSION_VOL_FLOOR_OVERRIDE=$BEST_COMP \
        2>/dev/null \
        && "$BT" "$OOS_FILE" --warmup $WARMUP --drift-min $BEST_DRIFT \
        2>/dev/null | tail -1) || OOS_RESULT=""

    if [ -z "$OOS_RESULT" ]; then
        echo "  OOS backtest failed -- skipping fold"
        fold=$((fold+1))
        continue
    fi

    # Parse stats (trades,wr,pnl,sharpe)
    IS_T=$(echo "$BEST_IS_STATS" | awk -F, '{print $1}')
    IS_WR=$(echo "$BEST_IS_STATS" | awk -F, '{print $2}')
    IS_PNL=$(echo "$BEST_IS_STATS" | awk -F, '{print $3}')
    IS_SH=$(echo "$BEST_IS_STATS" | awk -F, '{print $4}')
    OOS_T=$(echo "$OOS_RESULT"    | awk -F, '{print $1}')
    OOS_WR=$(echo "$OOS_RESULT"   | awk -F, '{print $2}')
    OOS_PNL=$(echo "$OOS_RESULT"  | awk -F, '{print $3}')
    OOS_SH=$(echo "$OOS_RESULT"   | awk -F, '{print $4}')

    # Degradation: how much Sharpe dropped from IS to OOS
    DEGRAD=$(python3 -c "
is_sh=float('${IS_SH}' or 0)
oos_sh=float('${OOS_SH}' or 0)
if is_sh <= 0: print('N/A')
else: print(f'{(1 - oos_sh/is_sh)*100:.1f}')
" 2>/dev/null || echo "N/A")

    ROBUST="NO"
    if [ "$DEGRAD" != "N/A" ]; then
        ROBUST=$(python3 -c "print('YES' if float('$DEGRAD') < 40 else 'NO')" 2>/dev/null || echo "NO")
    fi

    echo "  OOS result: trades=$OOS_T wr=$OOS_WR pnl=$OOS_PNL sharpe=$OOS_SH degradation=$DEGRAD% robust=$ROBUST"

    echo "$fold,$is_start,$is_end,$oos_start,$oos_end,$BEST_ATR,$BEST_DRIFT,$BEST_COMP,\
$IS_T,$IS_WR,$IS_PNL,$IS_SH,\
$OOS_T,$OOS_WR,$OOS_PNL,$OOS_SH,\
$DEGRAD,$ROBUST" >> "$OUT"

    fold=$((fold+1))
done

# =============================================================================
# STEP 4: Summary
# =============================================================================
echo ""
echo "=== Walk-Forward Summary ==="
python3 - "$OUT" << 'PYEOF'
import sys, csv

rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    print("No results")
    sys.exit(0)

robust = [r for r in rows if r.get('robust') == 'YES']
print(f"Folds:    {len(rows)}")
print(f"Robust:   {len(robust)}/{len(rows)} ({len(robust)/len(rows)*100:.0f}%)")

# Best OOS params (most frequently selected)
from collections import Counter
param_combos = Counter((r['atr_mult'], r['drift_min'], r['comp_floor']) for r in robust)
if param_combos:
    best, count = param_combos.most_common(1)[0]
    print(f"Most robust params: ATR={best[0]} DRIFT={best[1]} COMP={best[2]} (selected {count}x)")

# Average degradation
try:
    degs = [float(r['degradation_pct']) for r in rows if r['degradation_pct'] not in ('N/A','')]
    print(f"Avg degradation:    {sum(degs)/len(degs):.1f}%")
    print(f"Max degradation:    {max(degs):.1f}%")
except:
    pass

# OOS aggregate PnL
try:
    oos_pnl = sum(float(r['oos_pnl']) for r in rows)
    print(f"Aggregate OOS PnL:  ${oos_pnl:.2f}")
except:
    pass

print(f"\nFull results: backtest/wf_results.csv")
PYEOF

echo ""
echo "[WF] Done."
