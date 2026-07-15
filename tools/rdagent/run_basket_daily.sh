#!/usr/bin/env bash
# Daily auto-execute of the RD-Agent buy basket (paper/shadow). Nothing manual.
# 1) freshen close prices (yfinance, no IBKR needed)  2) paper-fill the top-5 basket at current prices.
# The model RANKING (latest.json) is refreshed separately by refresh_gui.sh every 5min from the qlib mlruns.
set -uo pipefail
T="$HOME/Omega/tools/rdagent"; TS="$(date '+%Y-%m-%d %H:%M')"
# S-2026-07-15: freshen the ACTIVE (held+ranked) names FIRST, in a clean process. The 744-name
# bulk refresh_close_yf.py chokes on Mac DNS/thread throttling and its anti-thin guard KEEPS the
# file stale -> the book marked DELL at a 2-day-old close (427.11 vs the real 457.54) and the desk
# showed a phantom -$19. This targeted per-name pull reliably lands the ~2 dozen names the book
# actually marks against, so the STOCK BASKET P&L is always current regardless of broad coverage.
python3 "$T/refresh_active_closes.py" >/dev/null 2>&1 || true
python3 "$T/refresh_close_yf.py" >/dev/null 2>&1 || true
python3 "$T/execute_basket.py" --topk 5 --capital 10000 --mode shadow >/tmp/rda_basket.json 2>>/tmp/rda_basket.err
echo "[$TS] basket executed -> $(python3 -c "import json;d=json.load(open('/tmp/rda_basket.json'));print('book',d.get('book'),'deployed',d.get('deployed_usd'))" 2>/dev/null)"
