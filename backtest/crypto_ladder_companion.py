#!/usr/bin/env python3
"""
LADDERED MULTI-COMPANION lock-in backtest (operator ask 2026-07-04).

"we only trade LONG on crypto ... instead of 1 companion we now have MULTIPLE
companion engines per trade ... mimic the trade and LOCK IN after 30%, INCREMENT
BY 10% up to 90% -> multiple companion engines for each trade."

=> a scale-out LADDER: parent buys on up-jump; the companion ladder locks a SLICE
of the (mimicked) unit at each MFE rung +30/40/50/60/70/80/90%. Each locked slice
is a SEPARATE additive standalone clip (operator hard rule: companion never touches
the parent, judged STANDALONE / additive, NEVER vs riding WIDE -> CompanionDominanceError).

This harness answers the operator's design forks EMPIRICALLY:
  (Q1) rung metric : %-of-MFE-gain  vs  $-levels           (which banks more)
  (Q2) slice sizing: equal 1/7  vs  weight-HIGH  vs  weight-LOW
plus baselines (ride-to-flip whole unit, single %-clip) for context, and a
remainder-protection variant (ratchet floor at last locked rung).

Long-only. Faithful fills at the rung target (limit fill at +rung%). Round-trip
Binance spot cost charged per slice traded. Both WF halves + both regimes reported.

Usage: crypto_ladder_companion.py BTC   (or ETH)
Env: DATA_DIR (default ~/Crypto/backtest/data), COST_RT (default 0.002), UP
"""
import csv, os, sys, bisect

DATA = os.environ.get("DATA_DIR", os.path.expanduser("~/Crypto/backtest/data"))
COST_RT = float(os.environ.get("COST_RT", "0.002"))
UP_DEF = {"BTC": 100.0, "ETH": 15.0}
RUNGS = [0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90]   # %-MFE gain rungs
N = len(RUNGS)


def load_1h(sym):
    rows = []
    with open(f"{DATA}/{sym}USDT_1h.csv") as f:
        for r in csv.DictReader(f):
            rows.append((int(r["open_time_ms"]), float(r["close"])))
    rows.sort()
    return rows


def load_daily(sym):
    rows = []
    with open(f"{DATA}/{sym}USDT_1d.csv") as f:
        for r in csv.DictReader(f):
            rows.append((int(r["open_time_ms"]), float(r["close"])))
    rows.sort()
    closes = [r[1] for r in rows]
    day_ms, dma, cby = [], {}, {}
    for i, (ms, c) in enumerate(rows):
        day_ms.append(ms); cby[ms] = c
        if i >= 200:
            dma[ms] = sum(closes[i - 200:i]) / 200.0
    return day_ms, dma, cby


def bull_asof(day_ms, dma, cby, ms):
    i = bisect.bisect_right(day_ms, ms) - 1
    while i >= 1 and day_ms[i] not in dma:
        i -= 1
    if i < 1:
        return 1
    d = day_ms[i]
    return 1 if cby[d] > dma[d] else 0


def dma_asof_px(day_ms, dma, ms):
    i = bisect.bisect_right(day_ms, ms) - 1
    while i >= 1 and day_ms[i] not in dma:
        i -= 1
    return dma.get(day_ms[i]) if i >= 1 else None


def gen_parents(sym, UP, mode="downjump"):
    """LONG on first $UP up-jump (24h). Exit rule depends on mode:
      mode='downjump' -> ride WIDE until a $UP down-jump (momentum flip; tight, chops out fast).
      mode='trend'    -> ride the TREND: exit only when daily close < 200DMA (regime flip red).
                         Enter only in bull (close>200DMA) so there is a trend to ride; this is
                         the 'mimic and ride it up' parent the 30-90% ladder needs. One episode
                         per bull run. bull flag = entry regime (all bull here, kept for split).
    Ungated on entry threshold otherwise. One position at a time."""
    h1 = load_1h(sym)
    day_ms, dma, cby = load_daily(sym)
    W = 24
    trades, i, tid, n = [], W, 0, len(h1)
    while i < n:
        ms, px = h1[i]
        if px - h1[i - W][1] >= UP:
            d200 = dma_asof_px(day_ms, dma, ms)
            bull = bull_asof(day_ms, dma, cby, ms)
            if mode == "trend" and not (d200 is not None and px > d200):
                i += 1; continue                      # trend parent enters only in bull
            ent, ent_ms = px, ms
            path = [(0, ent_ms, ent)]
            j, seq = i + 1, 1
            while j < n:
                mj, pj = h1[j]
                path.append((seq, mj, pj))
                if mode == "downjump":
                    if pj - h1[j - W][1] <= -UP:
                        break
                else:  # trend: exit on 200DMA red
                    dd = dma_asof_px(day_ms, dma, mj)
                    if dd is not None and pj < dd:
                        break
                seq += 1; j += 1
            trades.append(dict(tid=tid, ent=ent, bull=bull, path=path))
            tid += 1; i = j + 1
        else:
            i += 1
    return trades


# ---- sizing schemes: 7 weights summing to 1 ----
def weights(scheme):
    if scheme == "equal":
        w = [1.0] * N
    elif scheme == "low":     # front-loaded: lock more at the low rungs
        w = [N - k for k in range(N)]        # 7,6,5,4,3,2,1
    elif scheme == "high":    # back-loaded: lock more at the high rungs
        w = [k + 1 for k in range(N)]        # 1,2,3,4,5,6,7
    else:
        raise ValueError(scheme)
    s = sum(w)
    return [x / s for x in w]


def ladder_clips(t, rungs, w, metric, protect):
    """Simulate the laddered scale-out on ONE parent path. Returns list of
    (exit_ms, pnl_dollars) standalone clips (one per locked slice + one remainder).

    metric='pct' -> rungs are %-MFE gain, slice locks (fills) at +rung% * entry.
    metric='usd' -> rungs are $ favourable move, slice locks at entry + rung$.
    protect: 'ride'   -> unlocked remainder rides to the parent flip-out (path end).
             'ratchet'-> once >=1 rung locked, if MFE falls back to the last-locked
                         rung level, cut the remainder there (banked floor).
    Costs: round-trip COST_RT on each slice's OWN notional (fraction of 1 unit)."""
    ent = t["ent"]
    # absolute price target per rung
    tgt = [ent * (1 + r) if metric == "pct" else ent + r for r in rungs]
    clips = []
    locked = [False] * N
    last_locked_px = None
    peak = ent
    for seq, ms, px in t["path"]:
        if seq == 0:
            continue
        peak = max(peak, px)
        # lock any rung whose target is now reached (fill at target price)
        for k in range(N):
            if not locked[k] and px >= tgt[k]:
                locked[k] = True
                gain = tgt[k] - ent
                clips.append((ms, w[k] * (gain - COST_RT * ent)))
                last_locked_px = tgt[k]
        # ratchet floor on the un-locked remainder
        if protect == "ratchet" and last_locked_px is not None and px <= last_locked_px:
            rem = sum(w[k] for k in range(N) if not locked[k])
            if rem > 0:
                clips.append((ms, rem * (last_locked_px - ent - COST_RT * ent)))
                for k in range(N):
                    locked[k] = True
                return clips
    # remainder exits at parent flip-out (path end)
    rem = sum(w[k] for k in range(N) if not locked[k])
    if rem > 0:
        lms, lpx = t["path"][-1][1], t["path"][-1][2]
        clips.append((lms, rem * (lpx - ent - COST_RT * ent)))
    return clips


def single_clip(t, rung, metric):
    """Baseline: lock the WHOLE unit at the first rung, else ride to flip."""
    ent = t["ent"]
    tgt = ent * (1 + rung) if metric == "pct" else ent + rung
    for seq, ms, px in t["path"]:
        if seq and px >= tgt:
            return [(ms, (tgt - ent) - COST_RT * ent)]
    lms, lpx = t["path"][-1][1], t["path"][-1][2]
    return [(lms, (lpx - ent) - COST_RT * ent)]


def ride_flip(t):
    ent = t["ent"]; lms, lpx = t["path"][-1][1], t["path"][-1][2]
    return [(lms, (lpx - ent) - COST_RT * ent)]


# ---- metrics ----
def metrics(rows):
    if not rows:
        return dict(n=0, pf=0, net=0, dd=0, mar=0)
    rows = sorted(rows, key=lambda x: x[0]); pnl = [r[1] for r in rows]
    w = sum(p for p in pnl if p > 0); l = sum(-p for p in pnl if p < 0)
    pf = w / l if l > 0 else float('inf')
    eq = pk = dd = 0.0
    for p in pnl:
        eq += p; pk = max(pk, eq); dd = min(dd, eq - pk)
    net = sum(pnl)
    return dict(n=len(pnl), pf=pf, net=net, dd=dd, mar=(net / abs(dd) if dd < 0 else float('inf')))


def verdict(reg):
    m = metrics([(x[0], x[1]) for x in reg])
    rows = sorted(reg, key=lambda x: x[0]); h = len(rows) // 2
    m1 = metrics([(x[0], x[1]) for x in rows[:h]])
    m2 = metrics([(x[0], x[1]) for x in rows[h:]])
    bl = metrics([(x[0], x[1]) for x in reg if x[2]])
    br = metrics([(x[0], x[1]) for x in reg if not x[2]])
    ok = (m["net"] > 0 and m["pf"] > 1 and m1["net"] > 0 and m2["net"] > 0
          and bl["net"] >= 0 and br["net"] >= 0)
    return m, m1, m2, bl, br, ok


BULLGATE = os.environ.get("BULLGATE", "0") == "1"   # companion arms only in bull (COMPANION_BULL_ONLY)


def run_scheme(trades, clipper):
    reg = []
    for t in trades:
        if BULLGATE and not t["bull"]:
            continue
        for ems, pnl in clipper(t):
            reg.append((ems, pnl, t["bull"]))
    return reg


def main():
    sym = sys.argv[1] if len(sys.argv) > 1 else "BTC"
    UP = float(os.environ.get("UP", UP_DEF[sym]))
    mode = os.environ.get("PARENT", "downjump")   # downjump | trend
    trades = gen_parents(sym, UP, mode)
    print(f"[parent mode = {mode}]")
    if not trades:
        print(f"{sym}: no parents."); return
    npts = sum(len(t["path"]) for t in trades)
    nbull = sum(t["bull"] for t in trades)
    peaks = sorted(max((px / t["ent"] - 1 for s, m, px in t["path"] if s), default=0.0)
                   for t in trades)
    def q(p): return peaks[min(len(peaks) - 1, int(p * len(peaks)))]
    print(f"\n===== {sym}  UP=${UP:.0f}  {len(trades)} parents ({nbull} bull/{len(trades)-nbull} bear), "
          f"{npts} 1h bars, cost_rt={COST_RT} =====")
    print(f"parent peak-MFE%: p25 {q(.25)*100:.0f}%  p50 {q(.5)*100:.0f}%  p75 {q(.75)*100:.0f}%  "
          f"p90 {q(.9)*100:.0f}%  max {peaks[-1]*100:.0f}%")
    print("  hosts reaching each %-MFE rung (INERT check):")
    print("   " + "  ".join(f"+{int(r*100)}%:{sum(1 for p in peaks if p>=r)}" for r in RUNGS))

    hdr = f"  {'scheme':28s} {'net$':>10} {'PF':>5} {'DD$':>10} {'MAR':>6} {'H1$':>9} {'H2$':>9} {'bull$':>9} {'bear$':>9} {'nClip':>6}  V"
    res = []

    def add(name, clipper):
        reg = run_scheme(trades, clipper)
        m, m1, m2, bl, br, ok = verdict(reg)
        res.append((name, m, m1, m2, bl, br, ok, len(reg)))

    # baselines
    add("ride-to-flip (whole unit)", ride_flip)
    add("single %clip @+30% (whole)", lambda t: single_clip(t, 0.30, "pct"))
    add("single %clip @+50% (whole)", lambda t: single_clip(t, 0.50, "pct"))

    # LADDER: %-MFE, three sizings, two protections
    for sc in ("equal", "low", "high"):
        w = weights(sc)
        add(f"LADDER %MFE {sc:5s} ride",    lambda t, w=w: ladder_clips(t, RUNGS, w, "pct", "ride"))
        add(f"LADDER %MFE {sc:5s} ratchet", lambda t, w=w: ladder_clips(t, RUNGS, w, "pct", "ratchet"))

    # LADDER: $-levels (rungs scaled off the up-jump size) for the metric comparison
    usd_rungs = [UP * m for m in (1, 2, 3, 4, 5, 6, 7)]   # $UP,2UP,...,7UP
    for sc in ("equal", "low", "high"):
        w = weights(sc)
        add(f"LADDER $lvl {sc:5s} ride", lambda t, w=w: ladder_clips(t, usd_rungs, w, "usd", "ride"))

    print("\n" + hdr)
    for name, m, m1, m2, bl, br, ok, nc in res:
        print(f"  {name:28s} {m['net']:10.0f} {m['pf']:5.2f} {m['dd']:10.0f} {m['mar']:6.2f} "
              f"{m1['net']:9.0f} {m2['net']:9.0f} {bl['net']:9.0f} {br['net']:9.0f} {nc:6d}  {'P' if ok else '.'}")

    best = max(res, key=lambda r: r[1]["net"])
    bestv = max((r for r in res if r[6]), key=lambda r: r[1]["net"], default=None)
    print(f"\n  MAX net$      : {best[0]}  -> ${best[1]['net']:.0f} (PF {best[1]['pf']:.2f}, "
          f"bear ${best[5]['net']:.0f}, all-6 {'PASS' if best[6] else 'FAIL'})")
    if bestv:
        print(f"  MAX net$ that PASSES all-6: {bestv[0]} -> ${bestv[1]['net']:.0f} (PF {bestv[1]['pf']:.2f})")
    return res


if __name__ == "__main__":
    main()
