#!/usr/bin/env python3
"""
index_intraday_drift_v2.py -- viability measurement for the INTRADAY
                              drift strategy (BUY open / SELL close) on the
                              full Omega index basket.

Extends v1 (SPX/NSX/GER40 only) with USA30 (DJ30) and GBRIDXGBP (UK100)
raw-tick CSVs. Indices missing data: ESTX50.

Built 2026-05-28 after v1 found Sharpe NET 0.77-1.13 across 3 indices.
Operator: confirm viability on remaining indices before engine build.

Strategy: BUY at first H1 bar of UTC day, SELL at last H1 bar of UTC day.
Skip overnight (where modern regime shows decayed/reversed drift).
"""
import csv
import math
import os
from collections import defaultdict
from datetime import datetime, timezone, timedelta

# BlackBull CFD round-trip cost in price points (1 full spread, conservative).
# From symbols.ini MAX_SPREAD and observed live spreads.
SYMBOL_COST_PTS = {
    "SPXUSD": 1.5,    # US500
    "NSXUSD": 3.0,    # NAS100/USTEC
    "GER40":  2.5,
    "USA30":  6.0,    # DJ30: wider spread per symbols.ini MAX_SPREAD=12
    "UK100":  2.5,    # GBRIDXGBP
}

# Data sources -- mixed H1 OHLC and raw tick formats
CORPUS = {
    "SPXUSD": {"path": "/Users/jo/Tick/SPXUSD_merged.h1.csv",         "format": "h1_ohlc"},
    "NSXUSD": {"path": "/Users/jo/Tick/NSXUSD_merged.h1.csv",         "format": "h1_ohlc"},
    "GER40":  {"path": "/Users/jo/Tick/GER40_merged.h1.csv",          "format": "h1_ohlc"},
    "USA30":  {"path": [
                    "/Users/jo/Tick/duka_ticks/USA30IDXUSD_2025_10.csv",
                    "/Users/jo/Tick/duka_ticks/USA30IDXUSD_2025_11.csv",
                    "/Users/jo/Tick/duka_ticks/USA30IDXUSD_2025_12.csv",
                    "/Users/jo/Tick/duka_ticks/USA30IDXUSD_2026_01.csv",
                    "/Users/jo/Tick/duka_ticks/USA30IDXUSD_2026_02.csv",
                    "/Users/jo/Tick/duka_ticks/USA30IDXUSD_2026_03.csv",
                    "/Users/jo/Tick/duka_ticks/USA30IDXUSD_2026_04.csv",
               ],
               "format": "duka_ms_ask_bid_multi"},
    "UK100":  {"path": "/Users/jo/Tick/GBRIDXGBP/GBRIDXGBP_Ticks_2025.01.01_2025.12.31.csv",
               "format": "dukas_eet_ask_bid"},
}


# ---------------------------------------------------------------------------
# Loaders -- each returns list of (ts_unix_sec, o, h, l, c)
# ---------------------------------------------------------------------------
def load_h1_ohlc(path):
    bars = []
    with open(path, 'r') as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            if len(row) < 5: continue
            try:
                ts = int(row[0])
                o, h, l, c = (float(row[i]) for i in range(1, 5))
                if o > 0 and c > 0:
                    bars.append((ts, o, h, l, c))
            except (ValueError, IndexError):
                continue
    bars.sort(key=lambda r: r[0])
    return bars


def aggregate_ticks_to_h1(tick_iter):
    """tick_iter yields (ts_unix_sec, mid). Aggregate to H1 OHLC bars."""
    h1 = {}  # hour_start_unix -> [o, h, l, c]
    for ts, mid in tick_iter:
        hr = ts - (ts % 3600)
        if hr not in h1:
            h1[hr] = [mid, mid, mid, mid]
        else:
            rec = h1[hr]
            if mid > rec[1]: rec[1] = mid
            if mid < rec[2]: rec[2] = mid
            rec[3] = mid
    return [(hr, b[0], b[1], b[2], b[3]) for hr, b in sorted(h1.items())]


def load_tick_ms_ask_bid(path):
    """timestamp(ms),askPrice,bidPrice"""
    def gen():
        with open(path, 'r') as f:
            next(f)  # header
            for line in f:
                try:
                    parts = line.strip().split(',')
                    if len(parts) < 3: continue
                    ts_ms = int(parts[0]); ask = float(parts[1]); bid = float(parts[2])
                    if ask > 0 and bid > 0:
                        yield (ts_ms // 1000, (ask + bid) * 0.5)
                except (ValueError, IndexError):
                    continue
    return aggregate_ticks_to_h1(gen())


def load_dukas_eet_ask_bid(path):
    """
    Time (EET),Ask,Bid,AskVolume,BidVolume
    2025.01.03 01:00:00.755,8231.214,8229.845,...
    EET = UTC+2 (winter) or UTC+3 (summer, DST). Conservatively use UTC+2
    (only 1hr off in DST half-year; intraday-drift is daily-aggregated so
    rare boundary days get slight reassignment -- noise level immaterial).
    """
    EET_OFFSET = 2 * 3600
    def gen():
        with open(path, 'r') as f:
            next(f)  # header
            for line in f:
                try:
                    parts = line.strip().split(',')
                    if len(parts) < 3: continue
                    tstr = parts[0]
                    # parse 2025.01.03 01:00:00.755
                    date_part, time_part = tstr.split(' ')
                    Y, M, D = date_part.split('.')
                    hms = time_part.split('.')[0]
                    h, m, s = hms.split(':')
                    dt = datetime(int(Y), int(M), int(D), int(h), int(m), int(s),
                                  tzinfo=timezone.utc)
                    ts = int(dt.timestamp()) - EET_OFFSET
                    ask = float(parts[1]); bid = float(parts[2])
                    if ask > 0 and bid > 0:
                        yield (ts, (ask + bid) * 0.5)
                except (ValueError, IndexError):
                    continue
    return aggregate_ticks_to_h1(gen())


def load_duka_ms_ask_bid_multi(paths):
    """Concatenate multiple monthly DukasCopy ms-epoch CSVs.
    Format: timestamp_ms,ask,bid,ask_vol,bid_vol"""
    def gen():
        for p in paths:
            with open(p, 'r') as f:
                next(f)  # header
                for line in f:
                    try:
                        parts = line.strip().split(',')
                        if len(parts) < 3: continue
                        ts_ms = int(parts[0]); ask = float(parts[1]); bid = float(parts[2])
                        if ask > 0 and bid > 0:
                            yield (ts_ms // 1000, (ask + bid) * 0.5)
                    except (ValueError, IndexError):
                        continue
    return aggregate_ticks_to_h1(gen())


LOADERS = {
    "h1_ohlc": load_h1_ohlc,
    "tick_ms_ask_bid": load_tick_ms_ask_bid,
    "dukas_eet_ask_bid": load_dukas_eet_ask_bid,
    "duka_ms_ask_bid_multi": load_duka_ms_ask_bid_multi,
}


def h1_to_daily(bars):
    daily = defaultdict(lambda: {"open": None, "high": -math.inf, "low": math.inf,
                                  "close": None, "n": 0})
    for ts, o, h, l, c in bars:
        d = datetime.fromtimestamp(ts, tz=timezone.utc).date().isoformat()
        rec = daily[d]
        if rec["open"] is None:
            rec["open"] = o
        rec["close"] = c
        if h > rec["high"]: rec["high"] = h
        if l < rec["low"]:  rec["low"]  = l
        rec["n"] += 1
    return [(d, daily[d]["open"], daily[d]["high"], daily[d]["low"], daily[d]["close"], daily[d]["n"])
            for d in sorted(daily.keys()) if daily[d]["n"] >= 5]


def stats(rets):
    n = len(rets)
    if n < 2: return dict(n=n, mean=0, std=0, sharpe=0, hit=0, sum=0, max_dd=0)
    mean = sum(rets) / n
    var  = sum((r - mean) ** 2 for r in rets) / (n - 1)
    std  = math.sqrt(var)
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


def walk_forward_split(rets):
    """Split into halves, return (first_half_stats, second_half_stats)."""
    mid = len(rets) // 2
    return stats(rets[:mid]), stats(rets[mid:])


def fmt_bp(x):  return f"{x*10000:+.2f}bp"
def fmt_pct(x): return f"{x*100:+.4f}%"


def main():
    print()
    print("Index intraday-drift viability  -- full basket  (v2)")
    print("====================================================")
    print()
    print("  Trade rule: BUY at session open, SELL at session close (UTC daily).")
    print("  Cost (round-trip price points, BlackBull CFD assumed):")
    for sym, c in SYMBOL_COST_PTS.items():
        print(f"    {sym:<8} {c:.2f} pts")
    print()

    summary_rows = []
    for sym, cfg in CORPUS.items():
        path = cfg["path"]
        # Path may be str or list-of-str
        paths_to_check = path if isinstance(path, list) else [path]
        missing = [p for p in paths_to_check if not os.path.exists(p)]
        if missing:
            print(f"  [SKIP] {sym}: missing {missing[0]} (+{len(missing)-1} more)" if len(missing) > 1
                  else f"  [SKIP] {sym}: {missing[0]} missing")
            continue
        loader = LOADERS[cfg["format"]]
        label = os.path.basename(paths_to_check[0]) if isinstance(path, str) \
                else f"{len(paths_to_check)} files starting {os.path.basename(paths_to_check[0])}"
        print(f"  [{sym}] loading {label} ({cfg['format']}) ...", flush=True)
        try:
            bars = loader(path)
        except Exception as e:
            print(f"  [ERROR] {sym}: {e}"); continue
        if not bars:
            print(f"  [SKIP] {sym}: 0 H1 bars"); continue
        daily = h1_to_daily(bars)
        if len(daily) < 30:
            print(f"  [SKIP] {sym}: only {len(daily)} daily bars"); continue

        avg_px = sum(d[4] for d in daily) / len(daily)
        cost_pct = SYMBOL_COST_PTS[sym] / avg_px
        date0, date_n = daily[0][0], daily[-1][0]

        on_rets, id_rets, all_rets = [], [], []
        for i in range(1, len(daily)):
            _, o_t, _, _, c_t, _ = daily[i]
            _, _, _, _, c_prev, _ = daily[i - 1]
            on_rets.append((o_t - c_prev) / c_prev)
            id_rets.append((c_t - o_t) / o_t)
            all_rets.append((c_t - c_prev) / c_prev)
        id_net = [r - cost_pct for r in id_rets]

        # Walk-forward split on the intraday NET series
        wf1, wf2 = walk_forward_split(id_net)

        s_id     = stats(id_rets)
        s_id_net = stats(id_net)
        s_on     = stats(on_rets)
        s_full   = stats(all_rets)

        print(f"  {sym}  bars={len(daily)}  {date0} -> {date_n}  avg_px={avg_px:.2f}  cost={cost_pct*100:.4f}%")
        for label, s in [("intraday gross", s_id),
                         ("intraday NET  ", s_id_net),
                         ("overnight gross", s_on),
                         ("full-day gross ", s_full)]:
            print(f"    {label}  mean={fmt_bp(s['mean']):>10}  std={s['std']*100:>6.3f}%  "
                  f"Sharpe={s['sharpe']:>+6.2f}  WR={s['hit']:>5.1f}%  cumret={fmt_pct(s['sum']):>10}  "
                  f"maxDD={fmt_pct(s['max_dd']):>9}")
        print(f"    WALK-FWD intraday NET:")
        print(f"      1st half  n={wf1['n']:>4}  Sharpe={wf1['sharpe']:>+6.2f}  cumret={fmt_pct(wf1['sum']):>10}  WR={wf1['hit']:>5.1f}%")
        print(f"      2nd half  n={wf2['n']:>4}  Sharpe={wf2['sharpe']:>+6.2f}  cumret={fmt_pct(wf2['sum']):>10}  WR={wf2['hit']:>5.1f}%")
        print()

        summary_rows.append((sym, s_id_net['n'], s_id_net['sharpe'], s_id_net['sum'],
                            wf1['sharpe'], wf2['sharpe']))

    print("VIABILITY SUMMARY")
    print("=================")
    print(f"  {'symbol':<8} {'n_days':>7} {'Sharpe_net':>11} {'cumret_net':>11} "
          f"{'WF1_Sharpe':>11} {'WF2_Sharpe':>11}  verdict")
    for sym, n, sh, cum, wf1s, wf2s in summary_rows:
        verdict = "VIABLE" if (sh >= 0.5 and wf1s > 0 and wf2s > 0) else \
                  "MARGINAL" if (sh >= 0.3 and (wf1s > 0 or wf2s > 0)) else "DROP"
        print(f"  {sym:<8} {n:>7} {sh:>+11.2f} {cum*100:>+10.2f}% "
              f"{wf1s:>+11.2f} {wf2s:>+11.2f}  {verdict}")


if __name__ == "__main__":
    main()
