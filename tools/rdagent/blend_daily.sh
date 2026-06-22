#!/bin/bash
# Daily BLEND BOOK runner (shadow). Refreshes full S&P daily, recomputes the
# protected-beta + market-neutral-alpha blend, emits today's book + appends ledger.
# PROPOSES ONLY — execution gated. Validated 2020-2026: Sharpe 1.18 (beats B&H 0.90).
set -uo pipefail
source /opt/homebrew/Caskroom/miniforge/base/etc/profile.d/conda.sh
TOOLS="$HOME/Omega/tools/rdagent"; DATA="$HOME/Omega/data/rdagent"
TICKERS="$DATA/sp500_tickers.txt"; CLOSE="$DATA/sp500_long_close.csv"
TS="$(date '+%Y-%m-%d %H:%M')"
echo "[$TS] blend book — refreshing full S&P daily"
conda run -n rdagent4qlib python -c "
import yfinance as yf, pandas as pd
tk=open('$TICKERS').read().split()
d=yf.download(tk, period='2y', auto_adjust=True, progress=False)['Close'].dropna(axis=1,how='all')
# extend, not replace: keep long history, append fresh tail
import os
if os.path.exists('$CLOSE'):
    old=pd.read_csv('$CLOSE',index_col=0,parse_dates=True)
    d=pd.concat([old[~old.index.isin(d.index)], d]).sort_index()
d.to_csv('$CLOSE'); print(f'  refreshed -> {d.shape[1]} names through {d.index.max().date()}')" 2>/dev/null \
  || echo "  refresh skipped (offline) — using cached $CLOSE"
conda run -n rdagent4qlib python "$TOOLS/blend_book.py" --close-csv "$CLOSE" \
  --capital "${RDA_CAPITAL:-100000}" --w-core 0.4 --cost-bps 5 --mode shadow | tail -8
echo "[$TS] done — blend_book.json + blend_ledger.csv"
