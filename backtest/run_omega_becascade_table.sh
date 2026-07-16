#!/usr/bin/env bash
# Research-only: Omega becascade effect table. Gate each file, sweep locked shape over thr grid.
set -uo pipefail
cd /Users/jo/Omega/backtest
BIN=./omega_becascade_bt
T=/Users/jo/Tick
export UM_W=4 UM_G=0.5 UM_LEGS=8 UM_LOSSCUT=150

# symbol | path | RT_bp | thr-grid(csv fractions)
ROWS=(
 "XAUUSD|$T/XAUUSD_2022_2026.h1.csv|5|0.003,0.005,0.008,0.010"
 "MGC|$T/mgc_2024_2026.h1.csv|5|0.003,0.005,0.008,0.010"
 "SPX(US500)|$T/SPXUSD_2022_2026.h1.csv|3|0.004,0.006,0.009,0.012"
 "NDX(USTEC)|$T/NSXUSD_2022_2026.h1.csv|3|0.004,0.006,0.009,0.012"
 "GER40|$T/GRXEUR_merged.h1.csv|4|0.004,0.006,0.009,0.012"
 "EURUSD|$T/EURUSD_merged.h1.csv|3|0.0015,0.0025,0.004,0.006"
 "GBPUSD|$T/GBPUSD_befloor_h1.csv|3|0.0015,0.0025,0.004,0.006"
 "AUDUSD|$T/AUDUSD_befloor_h1.csv|4|0.0015,0.0025,0.004,0.006"
 "NZDUSD|$T/NZDUSD_befloor_h1.csv|4|0.0015,0.0025,0.004,0.006"
 "USDJPY|$T/USDJPY_befloor_h1.csv|4|0.0015,0.0025,0.004,0.006"
 "USDCAD|$T/USDCAD_befloor_h1.csv|4|0.0015,0.0025,0.004,0.006"
)

echo "######## DATA INTEGRITY GATE ########"
for r in "${ROWS[@]}"; do
  IFS='|' read -r sym path rt grid <<< "$r"
  out=$(python3 data_integrity_gate.py "$path" 2>&1 | grep -iE 'ACCEPT|REJECT|PASS|FAIL|verdict' | head -3 | tr '\n' ' ')
  echo "GATE $sym: ${out:-<no verdict line> $(python3 data_integrity_gate.py "$path" 2>&1 | tail -1)}"
done

echo ""
echo "######## BECASCADE SWEEP (W4 g0.5 leg8 lc150; per-symbol RT; thr grid) ########"
for r in "${ROWS[@]}"; do
  IFS='|' read -r sym path rt grid <<< "$r"
  [ -f "$path" ] || { echo "== $sym : MISSING $path"; continue; }
  echo "======================== $sym  (RT=${rt}bp) ========================"
  IFS=',' read -ra THRS <<< "$grid"
  for thr in "${THRS[@]}"; do
    SYM="$sym" SYM_PATH="$path" UM_RT="$rt" UM_THR="$thr" $BIN 2>/dev/null | sed -n '1p;3p'
  done
done
