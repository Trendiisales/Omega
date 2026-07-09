#!/usr/bin/env python3
"""WIDE-runner TRAIL-TIGHTEN sweep on the H1 up-jump LADDER (operator pivot 2026-07-09).

NOT a new mechanism: this retunes the ladder's EXISTING WIDE-tier giveback trail so the
runner EXITS ON THE TURN instead of arming late (2.7*thr MFE) and giving back 50% / window-
flushing at breakeven. TIGHT leg + STACKED spawn logic are UNCHANGED — only the WIDE tier
(ti=1) and its reclip LADDER leg (ti=3, wide params) get the tightened arm + giveback.

  WIDE arm  (MFE %-of-entry to engage the trail): sweep {0.5,1.0,1.5,2.0}  (baseline 2.7*thr)
  WIDE gb   (giveback fraction of the gain-from-entry): sweep {0.15,0.20,0.25,0.30}  (baseline 0.50)
      -> stop = entry + (1-gb)*(peak-entry). gb=0.5 is the current g50. gb=0.20 => a runner
         that peaks +5% exits at +4.0% (gives back 20% of the gain).

Baseline math is a byte-faithful copy of index_upjump_ladder_sweep.run (parity-tested vs the
C++ engine). wide_arm=None + wide_gb=None reproduces the current config EXACTLY.

Per-cell vs the current-config (arm 2.7*thr / gb 0.50) baseline:
  net%, PF, WF halves, DD, 2x-cost, ex-best, bear(2022) where a file exists, plus
  (a) round-trip rate = % clips that hit >= +2% MFE then exited <= +0% (gross)
  (b) avg giveback    = mean(peak_MFE_pct - exit_pct) across winning clips (gross exit > 0)
Goal cell: cuts round-trip hard + small avg-giveback (exits on the turn), net within ~10%.
"""
import os

TICK = "/Users/jo/Tick"

# (label, path, cost_bp, W, thr, gap_mask, bear_path)
CELLS = [
    ("NAS100",     f"{TICK}/NSXUSD_2022_2026.h1.csv", 3.0, 24, 1.5, True,  f"{TICK}/NAS2022_bear_h1.csv"),
    ("US500",      f"{TICK}/SPXUSD_2022_2026.h1.csv", 4.0, 24, 2.0, True,  None),
    ("NZDUSD",     f"{TICK}/NZDUSD_befloor_h1.csv",   2.5, 24, 1.5, False, None),
    ("GBPUSD",     f"{TICK}/GBPUSD_befloor_h1.csv",   2.0, 48, 1.0, False, None),
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
    __slots__ = ("entry", "peak", "armed", "arm_px", "trail_abs", "gb", "mfe")
    def __init__(self, entry, arm_pct, trail_abs_pct=None, gb=0.5):
        self.entry = entry; self.peak = entry; self.armed = False
        self.arm_px = entry * (1 + arm_pct / 100.0)
        self.trail_abs = entry * (trail_abs_pct / 100.0) if trail_abs_pct is not None else 0.0
        self.gb = gb              # giveback fraction of gain-from-entry (g50 == 0.5); ignored if trail_abs>0
        self.mfe = entry          # favourable extreme since entry (metrics)

def run(bars, W, thr, cost_bp, wide_arm=None, run_gb=None, gap_mask=True):
    """Operator peak-profit-trail spec (2026-07-09):
      run_gb  = giveback fraction of the GAIN on the runner tiers WIDE/STACKED/LADDER
                (stop = entry + (1-run_gb)*(peak-entry)); None -> 0.5 (baseline g50).
      wide_arm= MFE %-of-entry to ENGAGE the WIDE/LADDER trail; None -> 2.7*thr (baseline).
    STACKED keeps its staggered arm-to-engage (spawn logic as-is) but shares run_gb.
    TIGHT (abs trail) is always unchanged."""
    a_t, s_t   = 0.17 * thr, 0.67 * thr
    a_w        = wide_arm if wide_arm is not None else 2.7 * thr   # WIDE arm (abs % of entry)
    g_w        = run_gb   if run_gb   is not None else 0.5         # runner giveback fraction
    stacked    = [0.67 * thr, 1.33 * thr, 2.0 * thr]              # STACKED arms (thr-scaled, unchanged)
    reclip_pct = 1.67 * thr
    lc_pct     = 5.0 * thr
    CAP        = 5
    max_span   = W * 3600 + 4 * 86400
    recs = []
    open_clips = []; win_active = False; win_end = 0; nclips = 0; last_reclip_px = 0.0

    def book(cl, fill):
        gross = (fill - cl.entry) / cl.entry * 100.0
        mfe_pct = (cl.mfe - cl.entry) / cl.entry * 100.0
        recs.append((gross - cost_bp / 100.0, mfe_pct, gross))

    for i in range(W, len(bars)):
        ts, o, h, l, c = bars[i]
        still = []
        for cl in open_clips:
            closed = False
            for px in (l, h, c):
                if closed: break
                if px > cl.mfe: cl.mfe = px
                if not cl.armed:
                    if px <= cl.entry * (1 - lc_pct / 100.0):
                        book(cl, cl.entry * (1 - lc_pct / 100.0)); closed = True; break
                    if px >= cl.arm_px: cl.armed = True; cl.peak = max(cl.peak, px)
                else:
                    cl.peak = max(cl.peak, px)
                    if cl.trail_abs > 0: stop = cl.peak - cl.trail_abs
                    else:                stop = cl.entry + (1.0 - cl.gb) * (cl.peak - cl.entry)
                    if px <= stop:
                        book(cl, stop); closed = True; break
            if not closed: still.append(cl)
        open_clips = still
        if win_active and i >= win_end:
            for cl in open_clips: book(cl, c)
            open_clips = []; win_active = False
        contig = (not gap_mask) or ((ts - bars[i - W][0]) <= max_span)
        trigger = False
        if contig:
            wl = min(b[3] for b in bars[i - W:i])
            if wl > 0 and (c - wl) / wl * 100.0 >= thr: trigger = True
        if trigger and not win_active:
            win_active = True; win_end = i + W; nclips = 0; last_reclip_px = c
        if win_active and nclips < CAP:
            if nclips == 0:
                open_clips.append(Clip(c, a_t, s_t))                    # TIGHT (abs trail) — unchanged
                open_clips.append(Clip(c, a_w, gb=g_w))                 # WIDE  — tunable arm + gb
                for sa in stacked: open_clips.append(Clip(c, sa, gb=g_w))  # STACKED — staggered arm, shared gb
                nclips = 1; last_reclip_px = c
            elif c >= last_reclip_px * (1 + reclip_pct / 100.0):
                open_clips.append(Clip(c, a_w, gb=g_w))              # LADDER reclip = WIDE params
                nclips += 1; last_reclip_px = c
    for cl in open_clips:
        book(cl, bars[-1][4])
    return recs

def stats(recs):
    rets = [r[0] for r in recs]
    if not rets:
        return dict(n=0, net=0, pf=0, dd=0, h1=0, h2=0, wf=False, rt=0.0, gb=0.0, exbest=0.0)
    n = len(rets); net = sum(rets)
    gw = sum(r for r in rets if r > 0); gl = -sum(r for r in rets if r < 0)
    pf = gw / gl if gl > 0 else 99.9
    cur = peak = dd = 0.0
    for r in rets:
        cur += r; peak = max(peak, cur); dd = max(dd, peak - cur)
    mid = n // 2; h1 = sum(rets[:mid]); h2 = sum(rets[mid:])
    exbest = net - max(rets)
    rt = 100.0 * sum(1 for _, m, e in recs if m >= 2.0 and e <= 0.0) / n
    gbs = [m - e for _, m, e in recs if e > 0.0]
    gb = sum(gbs) / len(gbs) if gbs else 0.0
    # profit-capture-rate = mean(exit% / peak%) on winners (operator "keep 90%")
    caps = [e / m for _, m, e in recs if e > 0.0 and m > 0.05]
    cap = 100.0 * sum(caps) / len(caps) if caps else 0.0
    return dict(n=n, net=net, pf=pf, dd=dd, h1=h1, h2=h2, wf=(net > 0 and h1 > 0 and h2 > 0),
                rt=rt, gb=gb, exbest=exbest, cap=cap)

def main():
    ARMS = [0.0, 1.0, 2.0, 3.0]     # MFE %-of-entry to engage the runner trail
    GBS  = [0.10, 0.15, 0.20]       # giveback fraction of the gain (operator target 0.10)
    for label, path, cost, W, thr, gap, bear in CELLS:
        if not os.path.exists(path):
            print(f"{label}: MISSING {path}"); continue
        bars = load(path)
        if len(bars) < 500:
            print(f"{label}: thin ({len(bars)})"); continue
        base = stats(run(bars, W, thr, cost, gap_mask=gap))
        s2b = stats(run(bars, W, thr, cost * 2.0, gap_mask=gap))
        bear_bars = load(bear) if (bear and os.path.exists(bear)) else None
        b_base = stats(run(bear_bars, W, thr, cost, gap_mask=gap)) if bear_bars else None
        print(f"\n===== {label}  W{W} thr{thr}  cost{cost}bp  ({len(bars)} bars, gap_mask={gap}) =====")
        print(f"  BASELINE arm{2.7*thr:.2f}(=2.7*thr)/gb0.50: n={base['n']} net={base['net']:+.1f}% PF={base['pf']:.2f} "
              f"DD={base['dd']:.1f} WF={'+' if base['wf'] else '-'} H1={base['h1']:+.0f}/H2={base['h2']:+.0f} "
              f"| RT={base['rt']:.2f}% avgGB={base['gb']:.2f}pt cap={base['cap']:.1f}% exBest={base['exbest']:+.1f} 2x={s2b['net']:+.1f}"
              + (f" | bear net={b_base['net']:+.1f} PF={b_base['pf']:.2f}" if b_base else ""))
        hdr = (f"  {'arm':>4} {'gb':>4} | {'net%':>8} {'dNet%':>7} {'PF':>5} {'WF':>3} {'H1':>6} {'H2':>6} "
               f"{'RT%':>6} {'dRT':>6} {'avgGB':>6} {'cap%':>6} {'exBest':>7} {'2x':>7}")
        if b_base: hdr += f" {'bear':>7}"
        print(hdr)
        for arm in ARMS:
            for gb in GBS:
                s = stats(run(bars, W, thr, cost, wide_arm=arm, run_gb=gb, gap_mask=gap))
                s2 = stats(run(bars, W, thr, cost * 2.0, wide_arm=arm, run_gb=gb, gap_mask=gap))
                dnet_pct = 100.0 * (s['net'] - base['net']) / abs(base['net']) if base['net'] else 0.0
                drt = s['rt'] - base['rt']
                line = (f"  {arm:>4.1f} {gb:>4.2f} | {s['net']:+8.1f} {dnet_pct:+6.1f}% {s['pf']:5.2f} "
                        f"{'+' if s['wf'] else '-':>3} {s['h1']:+6.0f} {s['h2']:+6.0f} "
                        f"{s['rt']:5.2f}% {drt:+6.2f} {s['gb']:6.2f} {s['cap']:5.1f}% {s['exbest']:+7.1f} {s2['net']:+7.1f}")
                if b_base:
                    bs = stats(run(bear_bars, W, thr, cost, wide_arm=arm, run_gb=gb, gap_mask=gap))
                    line += f" {bs['net']:+7.1f}"
                print(line)

if __name__ == '__main__':
    main()
