#!/usr/bin/env python3
"""gapper_momo_recert.py — S-2026-07-23a operator-ordered re-cert of the
small-cap gapper config (the only profitable BigCapMomo variant per the
2026-06-28 faithful retest, PF 1.98 on the uncapped universe).

DATA (the first POINT-IN-TIME small-cap set we have): tools/gapper_recorder.py
has appended the LIVE IBKR TOP_PERC_GAIN $3-20 scan + 1-minute bars daily since
2026-01-07 — recent_gappers.csv (scan truth per day, no survivorship
reconstruction) + gapper_minute.csv (1m bars). 115 days, 1,446 gapper-name-days.

FAITHFUL RULES (mirror of the validated BigCapMomo config, bigcap_momo_faithful
lineage): universe = that day's scanned gappers only (already gap-gated by the
scan) · 5m bars RTH · ignition = +IG% over the prior 30min · volume surge >=
VOLX x avg(20) · breadth >= 2 distinct ignited names before entries unlock ·
1 entry/name/day · MAXENT=1 concurrent book-wide · wide trail TRAIL% off peak
(5m closes) · BE ratchet arm +2% -> floor +1% · maxhold 4h · 15:55 ET flush.

INTEGRITY GUARD (per name-day): drop rows with v==0; require the RTH median bar
price within [0.4x, 2.5x] of the scan's open..close envelope (kills the
split-scale rows like ASBP scan $6.5 vs bars $129); require >= 60 RTH 1m bars.

COSTS (small-cap honest): commission max($1, $0.005/sh)/side + SLIP bp/side
(grid 15/25/50; these names are thin — 25 is the base case, 50 the stress).
$1,000 notional per entry; results in $ on that basis + R stats.

CAVEATS (stated, not hidden): 6.5 months, one regime (2026), n limited by
MAXENT=1. This is a VIABILITY re-cert, not a 10-year certification.
"""
import csv, sys, math
from collections import defaultdict
from datetime import datetime

SCRATCH = "/private/tmp/claude-501/-Users-jo-Omega/1899b075-2964-4430-9a68-95e5d0fff682/scratchpad"
GAP = f"{SCRATCH}/recent_gappers.csv"
MIN = f"{SCRATCH}/gapper_minute.csv"

IG      = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0    # ignition % over 30min
VOLX    = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0    # volume surge multiple
TRAIL   = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0    # trail % off peak
SLIP_BP = float(sys.argv[4]) if len(sys.argv) > 4 else 25.0   # per side
NOTIONAL = 1000.0

scan = defaultdict(dict)              # date -> ticker -> scan row
for r in csv.DictReader(open(GAP)):
    scan[r["date"]][r["ticker"]] = r

bars = defaultdict(list)              # (date,ticker) -> [(min_of_day, o,h,l,c,v)]
for r in csv.DictReader(open(MIN)):
    try:
        hh, mm = r["bar_ts"].split(":")
        t = int(hh) * 60 + int(mm)
        o, h, l, c, v = (float(r["o"]), float(r["h"]), float(r["l"]),
                         float(r["c"]), float(r["v"]))
    except (ValueError, KeyError):
        continue
    if v <= 0:
        continue
    bars[(r["date"], r["ticker"])].append((t, o, h, l, c, v))

RTH0, RTH1, FLUSH = 9 * 60 + 30, 16 * 60, 15 * 60 + 55

def five_min(rows):
    out, cur = [], None
    for t, o, h, l, c, v in sorted(rows):
        if t < RTH0 or t >= RTH1:
            continue
        k = (t - RTH0) // 5
        if cur is None or cur[0] != k:
            if cur: out.append(cur[1:])
            cur = [k, t, o, h, l, c, v]
        else:
            cur[3] = max(cur[3], h); cur[4] = min(cur[4], l)
            cur[5] = c; cur[6] += v
    if cur: out.append(cur[1:])
    return out                        # [t,o,h,l,c,v] 5m RTH

trades = []                           # (date, ticker, entry_t, exit_t, ret_net, $net)
skipped_scale = skipped_thin = 0
for date in sorted(scan):
    day_syms = {}
    for tk, srow in scan[date].items():
        rows = bars.get((date, tk))
        if not rows or len(rows) < 60:
            skipped_thin += 1; continue
        try:
            lo = min(float(srow["low"]), float(srow["open"]))
            hi = max(float(srow["high"]), float(srow["close"]))
        except ValueError:
            continue
        rth = [r for r in rows if RTH0 <= r[0] < RTH1]
        if len(rth) < 30:
            skipped_thin += 1; continue
        med = sorted(r[4] for r in rth)[len(rth) // 2]
        if not (0.4 * lo <= med <= 2.5 * hi):
            skipped_scale += 1; continue
        day_syms[tk] = five_min(rows)

    # pass 1: ignition times per name (breadth gate needs cross-name view)
    ign_time = {}
    for tk, b5 in day_syms.items():
        vols = []
        for i in range(len(b5)):
            t, o, h, l, c, v = b5[i]
            if i >= 6:
                base = b5[i - 6][4]
                surge = v >= VOLX * (sum(vols[-20:]) / max(1, len(vols[-20:])))
                if base > 0 and (c / base - 1) * 100 >= IG and surge:
                    ign_time.setdefault(tk, t)
            vols.append(v)
    # breadth >= 2: entries unlock at the SECOND distinct ignition time
    times = sorted(ign_time.values())
    unlock = times[1] if len(times) >= 2 else None
    if unlock is None:
        continue

    # pass 2: entries (MAXENT=1 book-wide, first-come), manage on 5m closes
    open_until = -1
    for tk in sorted(ign_time, key=lambda k: ign_time[k]):
        t0 = max(ign_time[tk], unlock)
        if t0 < open_until:
            continue                   # MAXENT=1
        b5 = day_syms[tk]
        idx = next((i for i, b in enumerate(b5) if b[0] > t0), None)
        if idx is None or idx + 1 >= len(b5):
            continue
        entry = b5[idx][1]             # next 5m open after signal
        if entry <= 0: continue
        sh = NOTIONAL / entry
        comm = max(1.0, 0.005 * sh)
        peak = entry; be_floor = None; exit_px = None; exit_t = None
        for j in range(idx, len(b5)):
            t, o, h, l, c, v = b5[j]
            peak = max(peak, c)
            if be_floor is None and peak >= entry * 1.02:
                be_floor = entry * 1.01                    # arm +2% -> floor +1%
            stop = peak * (1 - TRAIL / 100.0)
            if be_floor: stop = max(stop, be_floor)
            if l <= stop:
                exit_px = min(stop, o); exit_t = t; break  # worse-of gap
            if t - b5[idx][0] >= 240 or t >= FLUSH:        # 4h maxhold / EOD
                exit_px = c; exit_t = t; break
        if exit_px is None:
            exit_px = b5[-1][4]; exit_t = b5[-1][0]
        slip = (entry + exit_px) * SLIP_BP / 1e4
        net = (exit_px - entry) * sh - 2 * comm - slip * sh
        trades.append((date, tk, t0, exit_t, net / NOTIONAL, net))
        open_until = exit_t

def agg(rows, tag):
    if not rows:
        print(f"  {tag:14s} n=0"); return
    net = sum(r[5] for r in rows); wins = [r for r in rows if r[5] > 0]
    gw = sum(r[5] for r in wins); gl = -sum(r[5] for r in rows if r[5] <= 0)
    run = peak = dd = 0.0
    for r in rows:
        run += r[5]; peak = max(peak, run); dd = max(dd, peak - run)
    top3 = sum(sorted((r[5] for r in rows), reverse=True)[:3])
    print(f"  {tag:14s} n={len(rows):3d} net=${net:+8.0f} PF={gw/gl if gl>0 else 99:5.2f} "
          f"WR={100*len(wins)/len(rows):3.0f}% maxDD=${dd:6.0f} top3=${top3:+.0f} "
          f"({100*top3/max(1e-9,net):.0f}% of net)" if net > 0 else
          f"  {tag:14s} n={len(rows):3d} net=${net:+8.0f} PF={gw/gl if gl>0 else 99:5.2f} "
          f"WR={100*len(wins)/len(rows):3.0f}% maxDD=${dd:6.0f}")

print(f"[gapper-recert] IG={IG} VOLX={VOLX} TRAIL={TRAIL} SLIP={SLIP_BP}bp "
      f"($1k/entry, comm max($1,.005/sh)/side) | scale-skips={skipped_scale} thin-skips={skipped_thin}")
agg(trades, "ALL")
mid = "2026-04-15"
agg([t for t in trades if t[0] < mid], "WF-H1(Jan-Apr)")
agg([t for t in trades if t[0] >= mid], "WF-H2(Apr-Jul)")
