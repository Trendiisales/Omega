#!/bin/bash
# Daily REGIME BOOK runner (shadow). Refreshes bigcap intraday data, classifies the
# regime, runs ONLY the active engine, emits flat-by-close orders + appends the
# immutable regime ledger. PROPOSES ONLY — execution gated. Schedule pre-open.
#
#   bash regime_daily.sh
#   launchd: com.omega.regime-book.plist (weekday 09:00)
set -uo pipefail
source /opt/homebrew/Caskroom/miniforge/base/etc/profile.d/conda.sh
TOOLS="$HOME/Omega/tools/rdagent"
M15="/tmp/omega_15m"
TS="$(date '+%Y-%m-%d %H:%M')"
echo "[$TS] regime book — refreshing intraday data + classifying regime"

# 1. best-effort refresh of bigcap 15m bars from the VPS (live data hookup)
tb=$(command -v gtimeout || command -v timeout || true)
if [ -n "$tb" ]; then
  $tb 90 scp -o BatchMode=yes -o ConnectTimeout=12 -P 2222 \
    "trader@185.167.119.59:C:/Omega/data/stocks/*_15m.csv" "$M15/" 2>/dev/null \
    && echo "  refreshed 15m bars from VPS" || echo "  VPS refresh skipped (offline) — using cached $M15"
fi

# 2. classify + run the active engine + append ledger + emit today's orders
conda run -n rdagent4qlib python "$TOOLS/regime_book.py" \
  --m15 "$M15" --mlruns /tmp/omega_factors/mlruns \
  --topk 5 --cost-bps 5 --capital "${RDA_CAPITAL:-100000}" --mode shadow | tail -10
echo "[$TS] done — book $HOME/Omega/data/rdagent/regime_book.json · ledger regime_ledger.csv"
