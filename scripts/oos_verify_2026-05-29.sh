#!/bin/bash
# 2026-05-29 walk-forward verification: 70/30 IS/OOS split per symbol
# Run discovery sweep twice (IS-only, OOS-only) using --to-unix / --from-unix
# Output: outputs/discovery_2026-05-29/is_<SYM>.csv + oos_<SYM>.csv
OUT_DIR=outputs/discovery_2026-05-29
BIN=backtest/multi_tf_sweep
TFS=(--tf 5m --tf 15m --tf 30m --tf 1h --tf 4h)
SYMS=(XAUUSD EURUSD GBPUSD USDJPY AUDUSD NZDUSD USDCAD EURGBP GER40 BCOUSD NSXUSD SPXUSD)

csv_for() {
  case "$1" in
    XAUUSD) echo /Users/jo/Tick/2yr_XAUUSD_format_a.csv ;;
    *)      echo /Users/jo/Tick/${1}_merged.csv ;;
  esac
}

# Per-symbol 70/30 split unix-sec. Read first+last ts_ms from CSV (skip header).
split_ts() {
  local csv="$1"
  local first_ms last_ms span split_ms
  first_ms=$(sed -n '2p' "$csv" | cut -d, -f1)
  last_ms=$(tail -n 1 "$csv" | cut -d, -f1)
  span=$(( last_ms - first_ms ))
  split_ms=$(( first_ms + (span * 7 / 10) ))
  echo $(( split_ms / 1000 ))
}

for sym in "${SYMS[@]}"; do
  csv=$(csv_for "$sym")
  [ ! -f "$csv" ] && continue
  split=$(split_ts "$csv")
  echo "[$sym] split_unix=$split"
  "$BIN" --csv "$csv" "${TFS[@]}" --symbol-override "$sym" \
         --to-unix "$split" --out "$OUT_DIR/is_${sym}.csv" \
         2>"$OUT_DIR/is_${sym}.log" &
  "$BIN" --csv "$csv" "${TFS[@]}" --symbol-override "$sym" \
         --from-unix "$split" --out "$OUT_DIR/oos_${sym}.csv" \
         2>"$OUT_DIR/oos_${sym}.log" &
done
wait

# Concat IS + OOS leaderboards
for half in is oos; do
  LB="$OUT_DIR/leaderboard_${half}.csv"
  echo "symbol,timeframe,family,params,n_trades,n_wins,win_rate,net_pnl,mean_r,sharpe,max_dd" > "$LB"
  for sym in "${SYMS[@]}"; do
    f="$OUT_DIR/${half}_${sym}.csv"
    [ -f "$f" ] && tail -n +2 "$f" >> "$LB"
  done
done

echo "[done]"
wc -l "$OUT_DIR/leaderboard_is.csv" "$OUT_DIR/leaderboard_oos.csv"
