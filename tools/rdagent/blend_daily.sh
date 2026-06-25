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
# S-2026-06-25b CLOSE REFRESH: the old single bulk yf.download returned ~75% of names under
# throttle -> ~230 froze at the 2024 build (the GUI "-" / 2yr-stale-column bug). Now: IBKR-first
# (reqHistoricalData, no throttle) when the gateway tunnel is up, else yfinance WITH
# retry-the-stragglers. Both extend-not-replace + freshness/coverage-gated (never write stale/thin).
if nc -z -G2 127.0.0.1 4001 2>/dev/null; then
  echo "  [close] IBKR gateway up -> reliable refresh (bigcap fast; run --tickers full overnight)"
  python3 "$TOOLS/refresh_close_ibkr.py" --tickers bigcap \
    || conda run -n rdagent4qlib python "$TOOLS/refresh_close_yf.py" \
    || echo "  refresh skipped — using cached $CLOSE"
else
  echo "  [close] no IBKR tunnel -> yfinance + retry-stragglers"
  conda run -n rdagent4qlib python "$TOOLS/refresh_close_yf.py" \
    || echo "  refresh skipped (stale/offline) — using cached $CLOSE"
fi
conda run -n rdagent4qlib python "$TOOLS/blend_book.py" --close-csv "$CLOSE" \
  --capital "${RDA_CAPITAL:-100000}" --w-core 0.4 --cost-bps 5 --mode shadow | tail -8
echo "[$TS] done — blend_book.json + blend_ledger.csv"
