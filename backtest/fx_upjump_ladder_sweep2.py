#!/usr/bin/env python3
"""FX upjump ladder EXPANDED entry/exit sweep (operator order S-2026-07-07x-resume:
"try everything, all entry and exit points").

Stage 1 — ENTRY grid: thr x W, default research exits, on the 4 FX candidates
(EURUSD/GBPUSD/NZDUSD survivors + AUDUSD marginal). WF halves + 2x-cost +
5-seed random-window control per cell.
Stage 2 — EXIT variants on each pair's LOCKED research cell: giveback fraction,
LOSS_CUT multiple, TIGHT trail width, WIDE arm, reclip step, cap.

Mechanism identical to backtest/omega_upjump_ladder_bt.py run() (parity-tested
against the C++ engine). Verdict standard: plateau (neighbors +), WF both
halves, 2x-cost +, beats random.

Usage: python3 backtest/fx_upjump_ladder_sweep2.py
"""
import random, os

TICK = "/Users/jo/Tick"
FILES = [
    ("EURUSD", f"{TICK}/EURUSD_merged.h1.csv",  2.0),
    ("GBPUSD", f"{TICK}/GBPUSD_befloor_h1.csv", 2.0),
    ("NZDUSD", f"{TICK}/NZDUSD_befloor_h1.csv", 2.5),
    ("AUDUSD", f"{TICK}/AUDUSD_befloor_h1.csv", 2.0),
]
LOCKED = {"EURUSD": (48, 0.5), "GBPUSD": (48, 1.0), "NZDUSD": (24, 1.5), "AUDUSD": (96, 1.0)}

def load(path):
    out = []
    with open(path) as f:
        for ln in f:
            p = ln.strip().split(',')
            if len(p) < 5 or not p[0][:1].isdigit(): continue
            try: out.append((int(float(p[0])), float(p[1]), float(p[2]), float(p[3]), float(p[4])))
            except ValueError: continue
    return out

class Clip:
    __slots__ = ("entry", "peak", "armed", "g", "arm_px", "trail_abs")
    def __init__(self, entry, arm_pct, g, trail_abs_pct=None):
        self.entry = entry; self.peak = entry; self.armed = False; self.g = g
        self.arm_px = entry * (1 + arm_pct / 100.0)
        self.trail_abs = entry * (trail_abs_pct / 100.0) if trail_abs_pct is not None else 0.0

def run(bars, W, thr, cost_bp, seed_windows=None,
        g=0.5, lc_m=5.0, t_trail_m=0.67, w_arm_m=2.7, reclip_m=1.67, cap=5):
    a_t, s_t   = 0.17 * thr, t_trail_m * thr
    a_w        = w_arm_m * thr
    stacked    = [0.67 * thr, 1.33 * thr, 2.0 * thr]
    reclip_pct = reclip_m * thr
    lc_pct     = lc_m * thr if lc_m > 0 else 0.0
    rets = []; win_starts = []
    open_clips = []; win_active = False; win_end = 0; nclips = 0; last_reclip_px = 0.0
    seed = set(seed_windows or ())
    for i in range(W, len(bars)):
        ts, o, h, l, c = bars[i]
        still = []
        for cl in open_clips:
            closed = False
            for px in (l, h, c):
                if closed: break
                if not cl.armed:
                    if lc_pct > 0 and px <= cl.entry * (1 - lc_pct / 100.0):
                        rets.append(-(lc_pct) - cost_bp / 100.0); closed = True; break
                    if px >= cl.arm_px: cl.armed = True; cl.peak = max(cl.peak, px)
                else:
                    cl.peak = max(cl.peak, px)
                    if cl.trail_abs > 0: stop = cl.peak - cl.trail_abs
                    else:                stop = cl.entry + g * (cl.peak - cl.entry)
                    if px <= stop:
                        rets.append((stop - cl.entry) / cl.entry * 100.0 - cost_bp / 100.0); closed = True; break
            if not closed: still.append(cl)
        open_clips = still
        if win_active and i >= win_end:
            for cl in open_clips:
                rets.append((c - cl.entry) / cl.entry * 100.0 - cost_bp / 100.0)
            open_clips = []; win_active = False
        trigger = False
        if seed_windows is None:
            wl = min(b[3] for b in bars[i - W:i])
            if wl > 0 and (c - wl) / wl * 100.0 >= thr: trigger = True
        else:
            trigger = i in seed
        if trigger and not win_active:
            win_active = True; win_end = i + W; nclips = 0; last_reclip_px = c; win_starts.append(i)
        if win_active and nclips < cap:
            if nclips == 0:
                open_clips.append(Clip(c, a_t, g, s_t))
                open_clips.append(Clip(c, a_w, g))
                for sa in stacked: open_clips.append(Clip(c, sa, g))
                nclips = 1; last_reclip_px = c
            elif c >= last_reclip_px * (1 + reclip_pct / 100.0):
                open_clips.append(Clip(c, a_w, g))
                nclips += 1; last_reclip_px = c
    for cl in open_clips:
        rets.append((bars[-1][4] - cl.entry) / cl.entry * 100.0 - cost_bp / 100.0)
    return rets, win_starts

def stats(rets):
    if not rets: return dict(n=0, net=0, pf=0, dd=0, h1=0, h2=0, wf=False)
    n = len(rets); net = sum(rets)
    gw = sum(r for r in rets if r > 0); gl = -sum(r for r in rets if r < 0)
    pf = gw / gl if gl > 0 else 99.9
    cur = peak = dd = 0.0
    for r in rets:
        cur += r; peak = max(peak, cur); dd = max(dd, peak - cur)
    mid = n // 2; h1 = sum(rets[:mid]); h2 = sum(rets[mid:])
    return dict(n=n, net=net, pf=pf, dd=dd, h1=h1, h2=h2, wf=(net > 0 and h1 > 0 and h2 > 0))

def rand_ctl(bars, W, thr, cost, nwins, **kw):
    nets = []
    for sd in range(5):
        random.seed(1000 + sd)
        cand = random.sample(range(W, len(bars)), min(nwins, len(bars) - W)) if nwins else []
        rr, _ = run(bars, W, thr, cost, seed_windows=cand, **kw)
        nets.append(stats(rr)['net'])
    return sum(nets) / len(nets) if nets else 0.0

def line(tag, s, s2, rnd):
    print(f"{tag:34s} | {s['n']:5d} {s['net']:+8.1f} {s['pf']:5.2f} {s['dd']:7.1f} "
          f"{'WF+' if s['wf'] else 'wf-':>3s} | 2x {s2['net']:+7.1f}/{s2['pf']:4.2f} | rnd {rnd:+7.1f}", flush=True)

def main():
    data = {}
    for pair, path, cost in FILES:
        if not os.path.exists(path): print(f"{pair}: MISSING"); continue
        data[pair] = (load(path), cost)

    print("=== STAGE 1: ENTRY GRID (thr x W, research exits) ===", flush=True)
    for pair, (bars, cost) in data.items():
        print(f"\n-- {pair} ({len(bars)} bars) --", flush=True)
        for W in (12, 24, 36, 48, 72, 96):
            for thr in (0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0):
                rets, wins = run(bars, W, thr, cost)
                s = stats(rets)
                if s['n'] == 0: continue
                s2 = stats(run(bars, W, thr, cost * 2.0)[0])
                rnd = rand_ctl(bars, W, thr, cost, len(wins))
                line(f"{pair} W{W} thr{thr}", s, s2, rnd)

    print("\n=== STAGE 2: EXIT VARIANTS (locked entry cells) ===", flush=True)
    for pair, (bars, cost) in data.items():
        W, thr = LOCKED[pair]
        print(f"\n-- {pair} locked W{W} thr{thr} --", flush=True)
        variants = (
            [(f"g{int(g*100)}", dict(g=g)) for g in (0.35, 0.5, 0.65)] +
            [(f"LC{m}", dict(lc_m=m)) for m in (3.0, 5.0, 8.0, 0.0)] +
            [(f"Ttrail{m}", dict(t_trail_m=m)) for m in (0.5, 0.67, 1.0)] +
            [(f"Warm{m}", dict(w_arm_m=m)) for m in (2.0, 2.7, 3.5)] +
            [(f"reclip{m}", dict(reclip_m=m)) for m in (1.0, 1.67, 2.5)] +
            [(f"cap{cp}", dict(cap=cp)) for cp in (3, 5, 8)]
        )
        for tag, kw in variants:
            rets, wins = run(bars, W, thr, cost, **kw)
            s = stats(rets)
            s2 = stats(run(bars, W, thr, cost * 2.0, **kw)[0])
            rnd = rand_ctl(bars, W, thr, cost, len(wins), **kw)
            line(f"{pair} {tag}", s, s2, rnd)

if __name__ == '__main__':
    main()
