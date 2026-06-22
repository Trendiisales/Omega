#!/bin/bash
# Daily market-neutral reversal sleeve: refresh S&P500 data + regenerate the
# long/short book + rebalance diff. Schedule pre-open. PROPOSES ONLY — execution gated.
#
# HONEST: forward (2021-26) Sharpe ~0.30, not the in-sample 0.72. Size as a small
# diversifier sleeve and PAPER-TRADE first. See reversal_sleeve.json for today's book.
set -uo pipefail
source /opt/homebrew/Caskroom/miniforge/base/etc/profile.d/conda.sh
TOOLS="$HOME/Omega/tools/rdagent"
DATA="$HOME/Omega/data/rdagent"
TICKERS="$DATA/sp500_tickers.txt"
CLOSE="$DATA/sp500_close.csv"
mkdir -p "$DATA"
TS="$(date '+%Y-%m-%d %H:%M')"

# 1. ticker universe (once; refresh manually if S&P membership changes)
if [ ! -f "$TICKERS" ]; then
  conda run -n rdagent4qlib python -c "
import qlib; from qlib.data import D
qlib.init(provider_uri='$HOME/.qlib/qlib_data/us_data', region='us', kernels=1)
ts=sorted(set(x.upper().replace('.','-') for x in D.list_instruments(D.instruments('sp500'),as_list=True)))
open('$TICKERS','w').write('\n'.join(ts))" 2>/dev/null
fi

# 2. refresh recent daily closes (6mo is plenty for L=5 + 20d hold)
echo "[$TS] refreshing S&P500 closes"
conda run -n rdagent4qlib python -c "
import yfinance as yf, pandas as pd
tk=open('$TICKERS').read().split()
d=yf.download(tk, period='6mo', auto_adjust=True, progress=False)['Close'].dropna(axis=1, how='all')
d.to_csv('$CLOSE'); print(f'  refreshed {d.shape[1]} names through {d.index.max().date()}')"

# 3. regenerate today's long/short book + rebalance diff
conda run -n rdagent4qlib python "$TOOLS/reversal_sleeve.py" --close-csv "$CLOSE"

# 4. APPEND to the forward shadow ledger (immutable; the real treat-as-live proof).
#    Holds the book, marks forward, rebalances weekly — appends only NEW trading days.
conda run -n rdagent4qlib python "$TOOLS/shadow_ledger.py" --close-csv "$CLOSE" --cost-bps 3 | tail -4

# 5. LIVE-IN-SHADOW execution: emit the exact sized orders that would hit IBKR today.
#    --mode shadow logs them; flip to --mode live --i-confirm (+ IB gateway) to send for real.
conda run -n rdagent4qlib python "$TOOLS/execute.py" --capital "${RDA_CAPITAL:-100000}" --mode shadow | tail -4
echo "[$TS] done — book + shadow ledger + shadow orders ($DATA/shadow_orders.csv)"
