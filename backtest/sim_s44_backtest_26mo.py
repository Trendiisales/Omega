#!/usr/bin/env python3
# Apply S44 HTF-bias gate to 26-month GoldScalpPyramid backtest trades.
#
# Approach:
#   1. Read daily XAUUSD tape, compute SMA20.
#      HTF bias = BULLISH if close>SMA20, BEARISH if close<SMA20, else NEUTRAL.
#      (Real HTFBiasFilter uses daily+intraday agreement; this is a single-
#      timeframe proxy. Operator can re-run with full bar replay later if
#      this initial sweep shows the gate has signal.)
#   2. For each trade in best_trades.csv, look up the inferred bias for the
#      trade's entry date and filter trades that S44 would have blocked.
#   3. Report pre-filter vs post-filter PnL, trade count, WR, DD.

from __future__ import annotations
import csv
from datetime import datetime, date

TRADES = "/Users/jo/Omega/gold_scalp_pyramid_best_trades.csv"
DAILY = "/Users/jo/Tick/2yr_XAUUSD_daily.csv"

# --- Build daily SMA20 + HTF bias per date ----------------------------------
sma_n = 20
daily = []  # (date, close)
with open(DAILY) as f:
    for line in f:
        parts = line.strip().split(",")
        if len(parts) < 5: continue
        d = datetime.strptime(parts[0], "%Y%m%d").date()
        c = float(parts[4])
        daily.append((d, c))

bias_map: dict[date, str] = {}
closes = [c for _, c in daily]
for i, (d, c) in enumerate(daily):
    if i < sma_n:
        bias_map[d] = "NEUTRAL"
        continue
    sma = sum(closes[i-sma_n:i]) / sma_n
    # Wider deadband sweep handled by external script; default 0.5%.
    import os
    band_pct = float(os.environ.get("HTF_BAND_PCT", "0.5")) / 100.0
    band = band_pct * sma
    if c > sma + band:
        bias_map[d] = "BULLISH"
    elif c < sma - band:
        bias_map[d] = "BEARISH"
    else:
        bias_map[d] = "NEUTRAL"

# --- Apply S44 filter to trades --------------------------------------------
def blocks(side: str, bias: str) -> bool:
    if bias == "BEARISH" and side == "LONG":  return True
    if bias == "BULLISH" and side == "SHORT": return True
    return False

n_total = n_blocked = n_kept = 0
pnl_pre = pnl_post = 0.0
wins_pre = wins_post = 0
worst_pre = worst_post = 0.0
peak_pre = peak_post = 0.0
dd_pre = dd_post = 0.0
running_pre = running_post = 0.0
bias_count = {"BULLISH": 0, "BEARISH": 0, "NEUTRAL": 0}

with open(TRADES) as f:
    rdr = csv.DictReader(f)
    for row in rdr:
        try:
            entry_ts = datetime.strptime(row["entry_ts"], "%Y-%m-%d %H:%M:%S")
        except (KeyError, ValueError):
            continue
        side = row["side"]
        pnl = float(row["pnl_usd"])
        d = entry_ts.date()
        bias = bias_map.get(d, "NEUTRAL")
        bias_count[bias] = bias_count.get(bias, 0) + 1

        n_total += 1
        pnl_pre += pnl
        if pnl > 0: wins_pre += 1
        if pnl < worst_pre: worst_pre = pnl
        running_pre += pnl
        peak_pre = max(peak_pre, running_pre)
        dd_pre = min(dd_pre, running_pre - peak_pre)

        if blocks(side, bias):
            n_blocked += 1
            # blocked => no pnl impact on post stats
            running_post += 0
        else:
            n_kept += 1
            pnl_post += pnl
            if pnl > 0: wins_post += 1
            if pnl < worst_post: worst_post = pnl
            running_post += pnl
        peak_post = max(peak_post, running_post)
        dd_post = min(dd_post, running_post - peak_post)

def fmt(v): return f"${v:,.2f}"

print(f"Bias distribution: BULL={bias_count['BULLISH']} BEAR={bias_count['BEARISH']} NEUT={bias_count['NEUTRAL']}")
print()
print(f"{'Metric':<22} {'pre-S44':>15} {'post-S44':>15} {'delta':>15}")
print("-" * 70)
print(f"{'trades':<22} {n_total:>15} {n_kept:>15} {n_kept - n_total:>15}")
print(f"{'blocked':<22} {'':>15} {n_blocked:>15}")
print(f"{'wins':<22} {wins_pre:>15} {wins_post:>15}")
print(f"{'WR%':<22} {100*wins_pre/n_total:>14.1f}% {100*wins_post/max(n_kept,1):>14.1f}%")
print(f"{'net PnL':<22} {fmt(pnl_pre):>15} {fmt(pnl_post):>15} {fmt(pnl_post - pnl_pre):>15}")
print(f"{'avg PnL/trade':<22} {fmt(pnl_pre/n_total):>15} {fmt(pnl_post/max(n_kept,1)):>15}")
print(f"{'worst trade':<22} {fmt(worst_pre):>15} {fmt(worst_post):>15}")
print(f"{'max DD':<22} {fmt(dd_pre):>15} {fmt(dd_post):>15}")
