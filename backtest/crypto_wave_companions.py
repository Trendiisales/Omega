#!/usr/bin/env python3
"""
WAVE-STACKED COMPANIONS (operator ask 2026-07-04b) — the SIMPLE model.

"if the trend on ETH crosses that 2%, open a companion engine, and keep opening
companion engines to ride the wave until it stagnates or reverses. I want to know
HOW MANY companion engines we open per wave."

Model (long-only, bullish waves only):
  * WAVE arms when price advances +STEP% above the current watch reference (fresh advance).
  * Companion #1 opens there. Each further +STEP% new high above the last companion's arm
    price opens ANOTHER companion (stacked, independent, additive). So a big wave stacks many.
  * WAVE ends (ALL open companions close at that price) on either:
        - STAGNATION: no new wave high within STAG_H hours, OR
        - REVERSAL : price gives back REV% from the wave peak.
  * Each companion is a SEPARATE standalone book: pnl = exit_px - its_own_arm_px - cost.
    (operator hard rule: companion never touches the parent; judged STANDALONE / additive,
    never vs riding WIDE -> CompanionDominanceError.)
  * BULL-GATE: waves only arm while daily close > 200DMA (COMPANION_BULL_ONLY;
    BearSpotNoEdge tombstone -> no spot-long edge below 200DMA).

Primary output = the DISTRIBUTION of companions-opened-per-wave. Profit reported too.

Usage: crypto_wave_companions.py ETH   (or BTC)
Env: DATA_DIR (~/Crypto/backtest/data), COST_RT (0.002), STEP/STAG_H/REV to pin one config.
"""
import csv, os, sys, bisect

DATA = os.environ.get("DATA_DIR", os.path.expanduser("~/Crypto/backtest/data"))
COST_RT = float(os.environ.get("COST_RT", "0.002"))


def load_1h(sym):
    rows = []
    with open(f"{DATA}/{sym}USDT_1h.csv") as f:
        for r in csv.DictReader(f):
            rows.append((int(r["open_time_ms"]), float(r["close"])))
    rows.sort()
    return rows


def load_dma(sym):
    rows = []
    with open(f"{DATA}/{sym}USDT_1d.csv") as f:
        for r in csv.DictReader(f):
            rows.append((int(r["open_time_ms"]), float(r["close"])))
    rows.sort()
    closes = [r[1] for r in rows]
    day_ms, dma = [], {}
    for i, (ms, c) in enumerate(rows):
        day_ms.append(ms)
        if i >= 200:
            dma[ms] = sum(closes[i - 200:i]) / 200.0
    return day_ms, dma


def bull_asof(day_ms, dma, ms, cby):
    i = bisect.bisect_right(day_ms, ms) - 1
    while i >= 1 and day_ms[i] not in dma:
        i -= 1
    if i < 1:
        return True
    d = day_ms[i]
    return cby[d] > dma[d]


def waves(sym, step, stag_ms, rev):
    """Return list of waves. Each wave = dict(arms=[(ms,px),...], path=[(ms,px)...],
    peak, exit_px, exit_ms, bull). Bull-gated arm. path = all bars from first arm to exit
    (so per-companion cold-loss cuts can be simulated)."""
    h1 = load_1h(sym)
    day_ms, dma = load_dma(sym)
    cby = {}
    with open(f"{DATA}/{sym}USDT_1d.csv") as f:
        for r in csv.DictReader(f):
            cby[int(r["open_time_ms"])] = float(r["close"])
    out = []
    n = len(h1)
    i = 0
    ref = h1[0][1]
    while i < n:
        ms, px = h1[i]
        # while flat, track ref as a trailing low so a fresh +step advance can arm
        if px < ref:
            ref = px
        if px >= ref * (1 + step):
            if not bull_asof(day_ms, dma, ms, cby):       # bull-gate
                ref = px; i += 1; continue
            arms = [(ms, px)]                              # companion #1
            path = [(ms, px)]
            last_arm = px
            peak = px; peak_ms = ms
            j = i + 1
            exit_px = px; exit_ms = ms
            while j < n:
                mj, pj = h1[j]
                path.append((mj, pj))
                if pj > peak:
                    peak = pj; peak_ms = mj
                    while pj >= last_arm * (1 + step):     # each fresh +step -> new companion
                        last_arm = last_arm * (1 + step)
                        arms.append((mj, last_arm))
                if pj <= peak * (1 - rev):                 # REVERSAL exit (wave trail)
                    exit_px = pj; exit_ms = mj; break
                if mj - peak_ms >= stag_ms:                # STAGNATION exit (time stop)
                    exit_px = pj; exit_ms = mj; break
                exit_px = pj; exit_ms = mj
                j += 1
            out.append(dict(arms=arms, path=path, peak=peak,
                            exit_px=exit_px, exit_ms=exit_ms, bull=1))
            ref = exit_px
            i = j + 1
        else:
            i += 1
    return out


def clips(wv, cold=None):
    """One standalone clip per companion. Wave-level exit = REVERSAL(rev) or STAGNATION.
    Optional per-companion COLD cut: if a companion's own price falls cold% below its arm
    while the wave is still open, cut THAT companion early at its stop (independent book)."""
    out = []
    path = wv["path"]
    wexit_px, wexit_ms = wv["exit_px"], wv["exit_ms"]
    for a_ms, a_px in wv["arms"]:
        ex_px, ex_ms = wexit_px, wexit_ms
        if cold:
            stop = a_px * (1 - cold)
            for mj, pj in path:
                if mj < a_ms:
                    continue
                if mj >= wexit_ms:
                    break
                if pj <= stop:
                    ex_px, ex_ms = stop, mj      # cold-cut fill at stop
                    break
        out.append((ex_ms, ex_px - a_px - COST_RT * a_px))
    return out


def metrics(rows):
    if not rows:
        return dict(n=0, pf=0, net=0, dd=0, mar=0)
    rows = sorted(rows, key=lambda x: x[0]); pnl = [r[1] for r in rows]
    w = sum(p for p in pnl if p > 0); l = sum(-p for p in pnl if p < 0)
    pf = w / l if l > 0 else float('inf')
    eq = pk = dd = 0.0
    for p in pnl:
        eq += p; pk = max(pk, eq); dd = min(dd, eq - pk)
    return dict(n=len(pnl), pf=pf, net=sum(pnl), dd=dd,
                mar=(sum(pnl) / abs(dd) if dd < 0 else float('inf')))


def dist(vals):
    if not vals:
        return "none"
    v = sorted(vals)
    def q(p): return v[min(len(v) - 1, int(p * len(v)))]
    return (f"mean {sum(v)/len(v):.1f}  p50 {q(.5)}  p75 {q(.75)}  p90 {q(.9)}  "
            f"max {v[-1]}  total {sum(v)}")


def run_one(sym, step, stag_h, rev, cold=None):
    wv = waves(sym, step, stag_h * 3600_000, rev)
    counts = [len(w["arms"]) for w in wv]
    reg = []
    for w in wv:
        for ms, p in clips(w, cold):
            reg.append((ms, p))
    m = metrics(reg)
    rows = sorted(reg, key=lambda x: x[0]); h = len(rows) // 2
    h1 = metrics(rows[:h]); h2 = metrics(rows[h:])
    pnls = [p for _, p in reg]
    return wv, counts, m, h1, h2, pnls


def main():
    sym = sys.argv[1] if len(sys.argv) > 1 else "ETH"
    print(f"\n########## {sym}  (cost_rt={COST_RT}, bull-gated, long-only) ##########")

    if os.environ.get("STEP"):
        step = float(os.environ["STEP"]) / 100
        stag_h = float(os.environ.get("STAG_H", "24"))
        rev = float(os.environ.get("REV", "10")) / 100
        print(f"\nconfig: STEP={step*100:.0f}%  STAG={stag_h:.0f}h  REV={rev*100:.0f}%  (bull-gated)")
        # PROTECTION verdict: baseline (wave trail+time-stop only) vs added per-companion cold cut
        print(f"\n  {'protection':22s} {'net$':>10} {'PF':>5} {'DD$':>10} {'MAR':>5} {'H1$':>9} "
              f"{'H2$':>9} {'%win':>5} {'worstClip$':>10} {'nClip':>6}  V")
        for name, cold in [("trail+stag only", None), ("+cold cut 8%", 0.08),
                           ("+cold cut 12%", 0.12), ("+cold cut 20%", 0.20)]:
            wv, counts, m, h1, h2, pnls = run_one(sym, step, stag_h, rev, cold)
            win = 100 * sum(1 for p in pnls if p > 0) / len(pnls) if pnls else 0
            worst = min(pnls) if pnls else 0
            ok = (m["net"] > 0 and m["pf"] > 1 and h1["net"] > 0 and h2["net"] > 0)
            print(f"  {name:22s} {m['net']:10.0f} {m['pf']:5.2f} {m['dd']:10.0f} {m['mar']:5.2f} "
                  f"{h1['net']:9.0f} {h2['net']:9.0f} {win:5.0f} {worst:10.0f} {m['n']:6d}  "
                  f"{'P' if ok else '.'}")
        wv, counts, *_ = run_one(sym, step, stag_h, rev)
        print(f"\n  waves: {len(wv)}   companions/wave: {dist(counts)}")
        return

    print(f"\n{'STEP':>4} {'STAG':>4} {'REV':>4} | {'waves':>5} {'compTot':>7} {'mean':>5} {'p50':>4} "
          f"{'p90':>4} {'max':>4} | {'net$':>9} {'PF':>5} {'DD$':>8} {'MAR':>5} {'H1$':>8} {'H2$':>8}  V")
    for step in (0.01, 0.02, 0.03, 0.05):
        for stag_h in (12, 24, 48):
            for rev in (0.05, 0.10, 0.15):
                wv, counts, m, h1, h2, _ = run_one(sym, step, stag_h, rev)
                if not wv:
                    continue
                v = sorted(counts)
                def q(p): return v[min(len(v) - 1, int(p * len(v)))]
                ok = (m["net"] > 0 and m["pf"] > 1 and h1["net"] > 0 and h2["net"] > 0)
                print(f"  {step*100:3.0f}% {stag_h:4.0f} {rev*100:3.0f}% | {len(wv):5d} {sum(counts):7d} "
                      f"{sum(counts)/len(counts):5.1f} {q(.5):4d} {q(.9):4d} {v[-1]:4d} | "
                      f"{m['net']:9.0f} {m['pf']:5.2f} {m['dd']:8.0f} {m['mar']:5.2f} "
                      f"{h1['net']:8.0f} {h2['net']:8.0f}  {'P' if ok else '.'}")


if __name__ == "__main__":
    main()
