#!/bin/bash
# 2026-05-29 discovery sweep: 12 sym x {5m,15m,30m,1h,4h} x 6 families
# Per-symbol cost from OmegaCostGuard production values (default in multi_tf_sweep).
OUT_DIR=outputs/discovery_2026-05-29
mkdir -p "$OUT_DIR"
BIN=backtest/multi_tf_sweep
TFS=(--tf 5m --tf 15m --tf 30m --tf 1h --tf 4h)

SYMS=(XAUUSD EURUSD GBPUSD USDJPY AUDUSD NZDUSD USDCAD EURGBP GER40 BCOUSD NSXUSD SPXUSD)

csv_for() {
  case "$1" in
    XAUUSD) echo /Users/jo/Tick/2yr_XAUUSD_format_a.csv ;;
    *)      echo /Users/jo/Tick/${1}_merged.csv ;;
  esac
}

for sym in "${SYMS[@]}"; do
  csv=$(csv_for "$sym")
  if [ ! -f "$csv" ]; then
    echo "[skip] $sym: $csv not found" >&2
    continue
  fi
  out_full="$OUT_DIR/full_${sym}.csv"
  echo "[run] $sym -> $out_full"
  "$BIN" --csv "$csv" "${TFS[@]}" --symbol-override "$sym" \
         --out "$out_full" 2>"$OUT_DIR/full_${sym}.log"
done

# Concat to leaderboard
LB="$OUT_DIR/leaderboard_full.csv"
echo "symbol,timeframe,family,params,n_trades,n_wins,win_rate,net_pnl,mean_r,sharpe,max_dd" > "$LB"
for sym in "${SYMS[@]}"; do
  f="$OUT_DIR/full_${sym}.csv"
  [ -f "$f" ] && tail -n +2 "$f" >> "$LB"
done

echo "[done] leaderboard: $LB"
wc -l "$LB"
