#!/usr/bin/env python3
"""
index_overnight_drift_measure.py -- viability measurement for overnight-drift
                                    on index CFDs (SPX/NSX/GER40 corpus).

Built 2026-05-28 after the session-open bracket viability test killed the
straddle idea. Operator: shift focus to indices + non-microstructure quant.

Hypothesis (Beumer/Cliff 2018, Lou/Polk/Skouras 2019): equity-index futures
earn nearly all their long-run return in the overnight session and roughly
zero intraday. Strategy: buy at session close, sell at session open. Sharpe
~1+ on ES decades-long.

This script reads H1 bars per index, aggregates to UTC-daily, and measures:
  - overnight_ret  = today_open / yesterday_close - 1
  - intraday_ret   = today_close / today_open - 1
  - day_ret        = today_close / yesterday_close - 1
And reports Sharpe, hit rate, expectancy, cumulative return for each.

Input: /Users/jo/Tick/<SYM>_merged.h1.csv  (ts_unix_sec, o, h, l, c)
       SYM in {SPXUSD, NSXUSD, GER40}

USAGE:
    python3 index_overnight_drift_measure.py
"""
import csv
import math
import os
from collections import defaultdict
from datetime import datetime, timezone

# Realistic BlackBull spread cost in price points per round-trip.
# From symbols.ini MAX_SPREAD: US500=2.5, NSX/USTEC=5, GER40=4.
# Conservative cost = 1 full spread per round trip (entry crosses spread,
# exit at limit/market roughly mid; pessimistic = 1 full).
SYMBOL_COST_PTS = {
    "SPXUSD": 1.5,    # tighter than CFD on demo, allow 1.5pt friction
    "NSXUSD": 3.0,
    "GER40":  2.5,
}

# H1 corpus paths
CORPUS = {
    "SPXUSD": "/Users/jo/Tick/SPXUSD_merged.h1.csv",
    "NSXUSD": "/Users/jo/Tick/NSXUSD_merged.h1.csv",
    "GER40":  "/Users/jo/Tick/GER40_merged.h1.csv",
}


def load_h1(path):
    """Return list of (ts_unix_sec_int, o, h, l, c) sorted by ts."""
    bars = []
    with open(path, 'r') as f:
        reader = csv.reader(f)
        next(reader)  # header ts,o,h,l,c
        for row in reader:
            if len(row) < 5:
                continue
            try:
                ts = int(row[0])
                o, h, l, c = (float(row[i]) for i in range(1, 5))
                if o > 0 and c > 0:
                    bars.append((ts, o, h, l, c))
            except (ValueError, IndexError):
                continue
    bars.sort(key=lambda r: r[0])
    return bars


def h1_to_daily(bars):
    """Group H1 bars into UTC days. Returns list of (date_str, open, high, low, close, n_bars)."""
    daily = defaultdict(lambda: {"open": None, "high": -math.inf, "low": math.inf,
                                  "close": None, "n": 0, "first_ts": None, "last_ts": None})
    for ts, o, h, l, c in bars:
        d = datetime.fromtimestamp(ts, tz=timezone.utc).date().isoformat()
        rec = daily[d]
        if rec["open"] is None:
            rec["open"] = o
            rec["first_ts"] = ts
        rec["close"] = c
        rec["last_ts"] = ts
        if h > rec["high"]: rec["high"] = h
        if l < rec["low"]:  rec["low"]  = l
        rec["n"] += 1
    # Filter: keep only days with >= 5 H1 bars (drops half-day starts/ends)
    out = []
    for d in sorted(daily.keys()):
        rec = daily[d]
        if rec["n"] >= 5:
            out.append((d, rec["open"], rec["high"], rec["low"], rec["close"], rec["n"]))
    return out


def stats(rets):
    """Return dict of mean, std, sharpe (annualised, 252d), hit_rate, sum, max_dd."""
    n = len(rets)
    if n < 2:
        return dict(n=n, mean=0, std=0, sharpe=0, hit=0, sum=0, max_dd=0)
    mean = sum(rets) / n
    var = sum((r - mean) ** 2 for r in rets) / (n - 1)
    std = math.sqrt(var)
    sharpe = (mean / std * math.sqrt(252)) if std > 0 else 0.0
    hits = sum(1 for r in rets if r > 0)
    cum = 0.0; peak = 0.0; max_dd = 0.0
    for r in rets:
        cum += r
        if cum > peak: peak = cum
        dd = peak - cum
        if dd > max_dd: max_dd = dd
    return dict(n=n, mean=mean, std=std, sharpe=sharpe,
                hit=100.0 * hits / n, sum=cum, max_dd=max_dd)


def fmt_pct(x): return f"{x*100:+.4f}%"
def fmt_bp(x):  return f"{x*10000:+.2f}bp"


def main():
    print()
    print("Index overnight-drift viability measurement (2024-2026 H1 corpus)")
    print("=================================================================")
    print()
    print("  Trade hypothesis: BUY at daily close, SELL at next daily open.")
    print("  Cost assumption (BlackBull CFD round-trip in price points):")
    for sym, cost in SYMBOL_COST_PTS.items():
        print(f"    {sym:<8} -> {cost:.2f} pts/round-trip")
    print()

    for sym, path in CORPUS.items():
        if not os.path.exists(path):
            print(f"  [SKIP] {sym}: {path} missing")
            continue
        bars = load_h1(path)
        daily = h1_to_daily(bars)
        if len(daily) < 30:
            print(f"  [SKIP] {sym}: only {len(daily)} daily bars (need >=30)")
            continue

        # Compute overnight + intraday + full-day returns
        on_rets = []     # (open[t] - close[t-1]) / close[t-1]
        id_rets = []     # (close[t] - open[t]) / open[t]
        all_rets = []    # (close[t] - close[t-1]) / close[t-1]

        # Per-symbol cost in % (cost_pts / avg_price)
        avg_px = sum(d[4] for d in daily) / len(daily)  # avg close
        cost_pct = SYMBOL_COST_PTS[sym] / avg_px

        for i in range(1, len(daily)):
            _, o_t, _, _, c_t, _ = daily[i]
            _, _, _, _, c_prev, _ = daily[i - 1]
            on_rets.append((o_t - c_prev) / c_prev)
            id_rets.append((c_t - o_t) / o_t)
            all_rets.append((c_t - c_prev) / c_prev)

        # Net of cost: subtract one round-trip per overnight trade
        on_rets_net = [r - cost_pct for r in on_rets]
        # Intraday baseline net (in case operator wants to compare)
        id_rets_net = [r - cost_pct for r in id_rets]

        date0 = daily[0][0]; date_n = daily[-1][0]
        print(f"  {sym}  bars={len(daily)}  range={date0} -> {date_n}  avg_px=${avg_px:.2f}  cost={cost_pct*100:.4f}%/trade")

        for label, rets in [("overnight  gross", on_rets),
                            ("overnight  NET   ", on_rets_net),
                            ("intraday   gross", id_rets),
                            ("intraday   NET   ", id_rets_net),
                            ("full-day   gross", all_rets)]:
            s = stats(rets)
            print(f"    {label}  n={s['n']:>4}  mean={fmt_bp(s['mean']):>10}  "
                  f"std={s['std']*100:>6.3f}%  Sharpe={s['sharpe']:>+6.2f}  "
                  f"WR={s['hit']:>5.1f}%  cumret={fmt_pct(s['sum']):>10}  maxDD={fmt_pct(s['max_dd']):>9}")
        print()


if __name__ == "__main__":
    main()
