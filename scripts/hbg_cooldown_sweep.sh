#!/usr/bin/env bash
# =============================================================================
# hbg_cooldown_sweep.sh -- second pass after trail sweep.
# Holds trail params at the winning values from hbg_trail_sweep.sh:
#     arm=20, frac=0.10, be=5.0
# Sweeps post-close cooldown and same-level block range / time.
# =============================================================================
set -e
cd "$(dirname "$0")/.."

CSV=/tmp/duka_format/all_xauusd.csv
RESULTS=/tmp/hbg_cooldown_results.csv

if [ ! -f "$CSV" ]; then
  echo "ERROR: $CSV not found."
  exit 1
fi

echo "cooldown_s,same_level_pts,post_sl_block_s,trades,wr_pct,net_usd,sl_n,sl_pnl,trail_n,trail_pnl" > "$RESULTS"

# Trail params fixed at trail-sweep winner: arm=20 effectively no-trail, be=5.0
TRAIL_FIXED=' -DHBG_TRAIL_ARM_PTS=20.0 -DHBG_MFE_TRAIL_FRAC=0.10 -DHBG_BE_TRIGGER_PTS=5.0 '

for cd in 60 300 600 900 1800; do
  for slp in 5.0 10.0 15.0 20.0; do
    for slt in 900 1800; do
      tag="cd${cd}_slp${slp}_slt${slt}"
      bin="/tmp/hbg_${tag}"

      printf "[%s] compiling..." "$tag"
      if ! g++ -O2 -std=c++17 -DOMEGA_BACKTEST \
              $TRAIL_FIXED \
              "-DHBG_COOLDOWN_S=${cd}" \
              "-DHBG_SAME_LEVEL_BLOCK_PTS=${slp}" \
              "-DHBG_SAME_LEVEL_POST_SL_BLOCK_S=${slt}" \
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

      echo "${cd},${slp},${slt},${trades:-0},${wr:-0},${net:-0},${sl_n:-0},${sl_pnl:-0},${tr_n:-0},${tr_pnl:-0}" >> "$RESULTS"
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
