#!/usr/bin/env python3
"""INDEX upjump ladder sweep (operator order S-2026-07-07x-resume: more index
companion/upjump trades, NAS100 or most lucrative index, with protections).

Same mechanism as backtest/omega_upjump_ladder_bt.py / fx_upjump_ladder_sweep2.py
(parity-tested vs the C++ engine) + GAP MASK: a window may only trigger when the
W-bar span is contiguous (<= W hours + 4 days). DATA NOTE S-2026-07-23: the NAS100 '7 missing months' claim is STALE/DEAD — NSXUSD_2022_2026.h1.csv rebuilt Jul-8, all 52 months present, integrity-gate CERTIFIED CLEAN (operator: the data is all there). Guard kept for other files
(2022-04, 2024-04, 2024-07..09, 2024-11..12; integrity gate REJECTED for holes),
US500 file is CERTIFIED CLEAN. GER40 runs its bull + bear files separately
(bull-gate candidate per feedback-companion-bull-gate-not-reject).

Controls: WF halves, 2x-cost, 5-seed random-window (gap-masked identically).
"""
import random, os

TICK = "/Users/jo/Tick"
FILES = [
    ("US500",      f"{TICK}/SPXUSD_2022_2026.h1.csv", 4.0, "mixed22-26"),
    ("NAS100",     f"{TICK}/NSXUSD_2022_2026.h1.csv", 3.0, "mixed22-26 GAPPY"),
    ("GER40_bull", f"{TICK}/GRXEUR_merged.h1.csv",    2.0, "bull"),
    ("GER40_bear", f"{TICK}/DAX2022_merged.h1.csv",   2.0, "bear"),
]

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
    __slots__ = ("entry", "peak", "armed", "arm_px", "trail_abs")
    def __init__(self, entry, arm_pct, trail_abs_pct=None):
        self.entry = entry; self.peak = entry; self.armed = False
        self.arm_px = entry * (1 + arm_pct / 100.0)
        self.trail_abs = entry * (trail_abs_pct / 100.0) if trail_abs_pct is not None else 0.0

def run(bars, W, thr, cost_bp, seed_windows=None):
    a_t, s_t   = 0.17 * thr, 0.67 * thr
    a_w        = 2.7 * thr
    stacked    = [0.67 * thr, 1.33 * thr, 2.0 * thr]
    reclip_pct = 1.67 * thr
    lc_pct     = 5.0 * thr
    CAP        = 5
    max_span   = W * 3600 + 4 * 86400          # GAP MASK (mirrors live engine guard)
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
                    if px <= cl.entry * (1 - lc_pct / 100.0):
                        rets.append(-(lc_pct) - cost_bp / 100.0); closed = True; break
                    if px >= cl.arm_px: cl.armed = True; cl.peak = max(cl.peak, px)
                else:
                    cl.peak = max(cl.peak, px)
                    if cl.trail_abs > 0: stop = cl.peak - cl.trail_abs
                    else:                stop = cl.entry + 0.5 * (cl.peak - cl.entry)
                    if px <= stop:
                        rets.append((stop - cl.entry) / cl.entry * 100.0 - cost_bp / 100.0); closed = True; break
            if not closed: still.append(cl)
        open_clips = still
        if win_active and i >= win_end:
            for cl in open_clips:
                rets.append((c - cl.entry) / cl.entry * 100.0 - cost_bp / 100.0)
            open_clips = []; win_active = False
        contig = (ts - bars[i - W][0]) <= max_span
        trigger = False
        if seed_windows is None:
            if contig:
                wl = min(b[3] for b in bars[i - W:i])
                if wl > 0 and (c - wl) / wl * 100.0 >= thr: trigger = True
        else:
            trigger = (i in seed) and contig
        if trigger and not win_active:
            win_active = True; win_end = i + W; nclips = 0; last_reclip_px = c; win_starts.append(i)
        if win_active and nclips < CAP:
            if nclips == 0:
                open_clips.append(Clip(c, a_t, s_t))
                open_clips.append(Clip(c, a_w))
                for sa in stacked: open_clips.append(Clip(c, sa))
                nclips = 1; last_reclip_px = c
            elif c >= last_reclip_px * (1 + reclip_pct / 100.0):
                open_clips.append(Clip(c, a_w))
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

def main():
    print(f"{'file':11s} {'W':>3s} {'thr':>4s} | {'n':>5s} {'net%':>8s} {'PF':>5s} {'DD%':>7s} {'WF':>3s} "
          f"{'H1%':>7s} {'H2%':>7s} | {'2x net/PF':>13s} | {'rand':>7s}", flush=True)
    for label, path, cost, regime in FILES:
        if not os.path.exists(path): print(f"{label}: MISSING {path}"); continue
        bars = load(path)
        if len(bars) < 500: print(f"{label}: thin ({len(bars)})"); continue
        print(f"-- {label} ({len(bars)} bars, {regime}) --", flush=True)
        for W in (12, 24, 48, 96):
            for thr in (0.5, 0.75, 1.0, 1.5, 2.0, 3.0):
                rets, wins = run(bars, W, thr, cost)
                s = stats(rets)
                if s['n'] == 0: continue
                s2 = stats(run(bars, W, thr, cost * 2.0)[0])
                rnd_nets = []
                for sd in range(5):
                    random.seed(1000 + sd)
                    cand = random.sample(range(W, len(bars)), min(len(wins), len(bars) - W)) if wins else []
                    rr, _ = run(bars, W, thr, cost, seed_windows=cand)
                    rnd_nets.append(stats(rr)['net'])
                rnd = sum(rnd_nets) / len(rnd_nets) if rnd_nets else 0.0
                print(f"{label:11s} {W:3d} {thr:4.1f} | {s['n']:5d} {s['net']:+8.1f} {s['pf']:5.2f} {s['dd']:7.1f} "
                      f"{'WF+' if s['wf'] else 'wf-':>3s} {s['h1']:+7.1f} {s['h2']:+7.1f} | "
                      f"{s2['net']:+7.1f}/{s2['pf']:4.2f} | {rnd:+7.1f}", flush=True)

if __name__ == '__main__':
    main()
