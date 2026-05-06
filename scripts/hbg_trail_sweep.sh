#!/usr/bin/env bash
# =============================================================================
# hbg_trail_sweep.sh -- compile and run N variants of hbg_duka_bt with
# different trail parameters, summarise net PnL across combos.
#
# Pre-requisite: /tmp/duka_format/all_xauusd.csv already prepared.
# Usage: bash scripts/hbg_trail_sweep.sh
# =============================================================================
set -e
cd "$(dirname "$0")/.."

CSV=/tmp/duka_format/all_xauusd.csv
RESULTS=/tmp/hbg_sweep_results.csv

if [ ! -f "$CSV" ]; then
  echo "ERROR: $CSV not found. Run the converter first."
  exit 1
fi

echo "trail_arm,trail_frac,be_trigger,trades,wr_pct,net_usd,sl_n,sl_pnl,trail_n,trail_pnl,bin" > "$RESULTS"

# Sweep grid -- 5 arm thresholds x 3 frac x 2 BE = 30 combos
for arm in 5 8 12 20 1000; do   # 1000 = effectively no trail
  for frac in 0.55 0.30 0.10; do
    for be in 3.0 5.0; do
      tag="arm${arm}_frac${frac}_be${be}"
      bin="/tmp/hbg_${tag}"

      printf "[%s] compiling..." "$tag"
      if ! g++ -O2 -std=c++17 -DOMEGA_BACKTEST \
              "-DHBG_TRAIL_ARM_PTS=${arm}.0" \
              "-DHBG_MFE_TRAIL_FRAC=${frac}" \
              "-DHBG_BE_TRIGGER_PTS=${be}" \
              -I include -o "$bin" backtest/hbg_duka_bt.cpp 2>/tmp/hbg_compile_err; then
        echo " FAILED"
        cat /tmp/hbg_compile_err
        continue
      fi

      printf " running..."
      output=$("$bin" "$CSV" 2>&1)

      trades=$(echo "$output"  | awk '/^Trades/      {print $3}')
      wr=$(echo "$output"      | awk '/^Win rate/    {print $4}')
      net=$(echo "$output"     | awk '/^Net PnL/     {print $4}' | tr -d '$')
      sl_n=$(echo "$output"    | awk '/SL_HIT/       {print $2}' | tr -d 'n=')
      sl_pnl=$(echo "$output"  | awk '/SL_HIT/       {print $3}' | tr -d 'pnl=$')
      tr_n=$(echo "$output"    | awk '/TRAIL_HIT/    {print $2}' | tr -d 'n=')
      tr_pnl=$(echo "$output"  | awk '/TRAIL_HIT/    {print $3}' | tr -d 'pnl=$')

      echo "${arm},${frac},${be},${trades:-0},${wr:-0},${net:-0},${sl_n:-0},${sl_pnl:-0},${tr_n:-0},${tr_pnl:-0},${bin}" >> "$RESULTS"
      printf " net=%s\n" "${net:-?}"
    done
  done
done

echo
echo "============================================================"
echo "RESULTS (sorted by net descending):"
echo "============================================================"
( head -1 "$RESULTS"; tail -n +2 "$RESULTS" | sort -t, -k6,6gr ) | column -s, -t

echo
echo "Best combo:"
tail -n +2 "$RESULTS" | sort -t, -k6,6gr | head -1 | column -s, -t
echo
echo "Full results: $RESULTS"
