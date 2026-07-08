#!/usr/bin/env python3
"""fx_bothways_sweep.py — FX majors BOTH-DIRECTIONS mechanism sweep (S-2026-07-08, RESEARCH ONLY).

Operator order: find a per-pair tradeable mechanism for the FX majors, both long and
short, ranked by return. No wiring. Tombstone respected: NO naked Donchian/turtle
archetype is tested (FxTurtleH4 is tombstoned cross-regime).

MECHANISMS
  A. UPJUMP  — up-jump giveback ladder LONG. Mechanics identical to
     backtest/fx_upjump_ladder_sweep2.py run() (parity-tested vs the C++ engine).
     Anchors reproduced first: EURUSD W48/0.5, GBPUSD W48/1.0, NZDUSD W24/1.5,
     AUDUSD W96/1.0.
  B. DNJUMP  — the mirror-image DOWN-jump ladder SHORT: close <= -thr% under the
     prior-W-bar HIGH opens a W-bar short window; short clips with the same
     arm/giveback/loss-cut/reclip machinery, sign-mirrored.
     Sanity control: mirror cells with WIDE arm <= 1% (2.7*thr <= 1%) are
     historically ALL negative — tagged 'arm<=1%' in output; expect red.
  C. RIDER   — jump-rider, inherently both-ways. W in {2,4}, thr 0.25-0.5%.
     Exits: FLIP (symmetric jump-out flips direction — the XAUUSD GO pattern) and
     BE-LOCK floors (+5/+10/+25bp resting stop once peak >= 0.5*thr) — NEVER the
     known-trap close-eval BE (JumpRiderNAS100: all 164 close-eval BE exits red).
     All stops are RESTING orders: intrabar H/L touch fills at level, gap-open at open.
     Catastrophe stop 2x thr adverse, always resting. Anchor: USDJPY W2/0.3 FLIP.
  D. MRH4    — H4 z-score band fade, BOTH sides, with a per-side sustained-trend
     veto (the ConnorsNas asym-veto machinery): veto longs while close<SMA100(H4)
     for >=24 consecutive H4 bars (sustained downtrend), mirror for shorts.
     Exit: z reversion to -0.5/+0.5 or 20-bar timeout; resting 150bp catastrophe
     loss-cut (engine loss-protection provision).

COST MODEL (stated per order): base 2.0 bp round-trip on majors — the validated
figure the wired FX upjump ladder uses (rt_cost_bp ~2.0; ExecutionCostGuard charges
$6/lot RT commission + spread, ~= 0.6bp commission + ~1-1.5bp spread on a 100k-unit
lot for EURUSD-class pairs). Stress leg: 4.0 bp (2x).

VALIDATION per candidate (BACKTEST_TRUTH): WF halves (time split), 2x-cost,
ex-best-trade, 5-seed random-entry control (over-random = net - rnd), plateau
(thr neighbors), and a bear/rate-cycle regime split where data exists:
GBPUSD 2022-H2 (dollar rally + Truss), USDJPY 2018/2020/2022/2024 yearly H1.

DATA (all gate-CERTIFIED by backtest/data_integrity_gate.py this session):
  /Users/jo/Tick/{EURUSD_merged,GBPUSD_befloor,USDJPY_befloor,AUDUSD_befloor,
                  USDCAD_befloor,EURGBP_befloor}*h1.csv
  /Users/jo/Tick/fx_bothways_deriv/NZDUSD_2025_h1.csv        (rebuilt: the stock
      NZDUSD_befloor_h1.csv is gate-REJECTED — 90d hole Jan-Mar 2026)
  /Users/jo/Tick/GBPUSD2022H2_befloor_h1.csv                  (regime)
  /Users/jo/Tick/fx_bothways_deriv/USDJPY_{2018,2020,2022,2024}_h1.csv (regime)
USDCHF: no local multiyear data — skipped, not downloaded (per order).

Usage: python3 backtest/fx_bothways_sweep.py [MECH ...]   (default: all)
Output: pipe-delimited rows for machine parsing + human summary.
"""
import os, random, statistics, sys

TICK = "/Users/jo/Tick"
DERIV = f"{TICK}/fx_bothways_deriv"
BASE_COST = 2.0    # bp RT
PAIRS = [
    ("EURUSD", f"{TICK}/EURUSD_merged.h1.csv"),
    ("GBPUSD", f"{TICK}/GBPUSD_befloor_h1.csv"),
    ("USDJPY", f"{TICK}/USDJPY_befloor_h1.csv"),
    ("AUDUSD", f"{TICK}/AUDUSD_befloor_h1.csv"),
    ("NZDUSD", f"{DERIV}/NZDUSD_2025_h1.csv"),
    ("USDCAD", f"{TICK}/USDCAD_befloor_h1.csv"),
    ("EURGBP", f"{TICK}/EURGBP_befloor_h1.csv"),
]
REGIME = {   # pair -> [(tag, path)]
    "GBPUSD": [("2022H2", f"{TICK}/GBPUSD2022H2_befloor_h1.csv")],
    "USDJPY": [(f"Y{y}", f"{DERIV}/USDJPY_{y}_h1.csv") for y in (2018, 2020, 2022, 2024)],
}
ANCHORS = {"EURUSD": (48, 0.5), "GBPUSD": (48, 1.0), "NZDUSD": (24, 1.5), "AUDUSD": (96, 1.0)}


def load(path):
    out = []
    with open(path) as f:
        for ln in f:
            p = ln.strip().split(',')
            if len(p) < 5 or not p[0][:1].isdigit():
                continue
            try:
                out.append((int(float(p[0])), float(p[1]), float(p[2]), float(p[3]), float(p[4])))
            except ValueError:
                continue
    return out


# ── A/B: giveback ladder, direction-parameterized ───────────────────────────
# dir=+1 is line-for-line the fx_upjump_ladder_sweep2.run() mechanics (anchor parity);
# dir=-1 mirrors every comparison. Returns [(ts, ret_bp)], [win_start_idx].
class Clip:
    __slots__ = ("entry", "ext", "armed", "arm_px", "trail_abs")

    def __init__(self, entry, arm_pct, d, trail_abs_pct=None):
        self.entry = entry
        self.ext = entry                      # peak (long) / trough (short)
        self.armed = False
        self.arm_px = entry * (1 + d * arm_pct / 100.0)
        self.trail_abs = entry * (trail_abs_pct / 100.0) if trail_abs_pct is not None else 0.0


def ladder(bars, W, thr, cost_bp, d=1, seed_windows=None,
           g=0.5, lc_m=5.0, t_trail_m=0.67, w_arm_m=2.7, reclip_m=1.67, cap=5):
    a_t, s_t = 0.17 * thr, t_trail_m * thr
    a_w = w_arm_m * thr
    stacked = [0.67 * thr, 1.33 * thr, 2.0 * thr]
    reclip_pct = reclip_m * thr
    lc_pct = lc_m * thr if lc_m > 0 else 0.0
    rets, win_starts = [], []
    open_clips = []
    win_active = False; win_end = 0; nclips = 0; last_reclip_px = 0.0
    seed = set(seed_windows or ())

    def pnl(exit_px, entry):          # ret in bp, cost debited
        return d * (exit_px - entry) / entry * 1e4 - cost_bp

    for i in range(W, len(bars)):
        ts, o, h, l, c = bars[i]
        pxs = (l, h, c) if d > 0 else (h, l, c)   # adverse extreme first (orig order)
        still = []
        for cl in open_clips:
            closed = False
            for px in pxs:
                if closed:
                    break
                if not cl.armed:
                    if lc_pct > 0 and d * (px - cl.entry * (1 - d * lc_pct / 100.0)) <= 0:
                        rets.append((ts, -lc_pct * 100.0 - cost_bp)); closed = True; break
                    if d * (px - cl.arm_px) >= 0:
                        cl.armed = True
                        cl.ext = max(cl.ext, px) if d > 0 else min(cl.ext, px)
                else:
                    cl.ext = max(cl.ext, px) if d > 0 else min(cl.ext, px)
                    if cl.trail_abs > 0:
                        stop = cl.ext - d * cl.trail_abs
                    else:
                        stop = cl.entry + g * (cl.ext - cl.entry)
                    if d * (px - stop) <= 0:
                        rets.append((ts, pnl(stop, cl.entry))); closed = True; break
            if not closed:
                still.append(cl)
        open_clips = still
        if win_active and i >= win_end:
            for cl in open_clips:
                rets.append((ts, pnl(c, cl.entry)))
            open_clips = []; win_active = False
        trigger = False
        if seed_windows is None:
            if d > 0:
                wl = min(b[3] for b in bars[i - W:i])
                if wl > 0 and (c - wl) / wl * 100.0 >= thr:
                    trigger = True
            else:
                wh = max(b[2] for b in bars[i - W:i])
                if wh > 0 and (wh - c) / wh * 100.0 >= thr:
                    trigger = True
        else:
            trigger = i in seed
        if trigger and not win_active:
            win_active = True; win_end = i + W; nclips = 0; last_reclip_px = c
            win_starts.append(i)
        if win_active and nclips < cap:
            if nclips == 0:
                open_clips.append(Clip(c, a_t, d, s_t))
                open_clips.append(Clip(c, a_w, d))
                for sa in stacked:
                    open_clips.append(Clip(c, sa, d))
                nclips = 1; last_reclip_px = c
            elif d * (c - last_reclip_px * (1 + d * reclip_pct / 100.0)) >= 0:
                open_clips.append(Clip(c, a_w, d))
                nclips += 1; last_reclip_px = c
    for cl in open_clips:
        rets.append((bars[-1][0], pnl(bars[-1][4], cl.entry)))
    return rets, win_starts


# ── C: jump-rider, FLIP / BE-lock exits, resting intrabar stops ─────────────
def rider(bars, W, thr_pct, cost_bp, exit_mode, bar_sec=3600):
    thr = thr_pct / 100.0
    ts = [b[0] for b in bars]; o = [b[1] for b in bars]
    h = [b[2] for b in bars]; l = [b[3] for b in bars]; c = [b[4] for b in bars]
    trades = []
    pos = 0; entry = peak = 0.0; block_up = block_dn = False
    be_floor = {"BEL05": 5.0, "BEL10": 10.0, "BEL25": 25.0}.get(exit_mode)  # bp
    for i in range(W, len(c)):
        cur = c[i]
        j = cur / c[i - W] - 1.0
        contig = (ts[i] - ts[i - W]) <= W * bar_sec * 2
        up = j >= thr; dn = j <= -thr
        if not up: block_up = False
        if not dn: block_dn = False
        if pos != 0:
            closed = False
            stops = [entry * (1 - pos * 2 * thr)]                    # catastrophe
            if be_floor is not None and peak >= 0.5 * thr:
                stops.append(entry * (1 + pos * be_floor / 1e4))     # BE-lock floor
            lvl = max(stops) if pos > 0 else min(stops)
            touched = (l[i] <= lvl) if pos > 0 else (h[i] >= lvl)
            if touched:
                fill = min(o[i], lvl) if pos > 0 else max(o[i], lvl)
                trades.append((ts[i], pos, pos * (fill - entry) / entry * 1e4 - cost_bp))
                pos = 0; closed = True
                if up: block_up = True
                if dn: block_dn = True
            if not closed:
                ret = pos * (cur - entry) / entry
                peak = max(peak, ret)
                flip = dn if pos > 0 else up
                if flip:
                    trades.append((ts[i], pos, ret * 1e4 - cost_bp)); pos = 0
                    nd = -1 if dn else 1
                    if contig and ((nd > 0 and not block_up) or (nd < 0 and not block_dn)):
                        pos = nd; entry = cur; peak = 0.0
        elif contig:
            if up and not block_up:
                pos = 1; entry = cur; peak = 0.0
            elif dn and not block_dn:
                pos = -1; entry = cur; peak = 0.0
    if pos != 0:
        trades.append((ts[-1], pos, pos * (c[-1] - entry) / entry * 1e4 - cost_bp))
    return trades


# ── D: H4 z-band mean-reversion fade, per-side sustained-trend veto ─────────
def to_h4(bars):
    buckets = {}
    order = []
    for ts, o, h, l, c in bars:
        b = (ts // 14400) * 14400
        e = buckets.get(b)
        if e is None:
            buckets[b] = [o, h, l, c]; order.append(b)
        else:
            if h > e[1]: e[1] = h
            if l < e[2]: e[2] = l
            e[3] = c
    return [(b, *buckets[b]) for b in sorted(order)]


def mrh4(h4, z_in, veto_sides, cost_bp, k=20, z_out=0.5, timeout=20, lc_bp=150.0,
         sma_n=100, streak_n=24):
    ts = [b[0] for b in h4]; c = [b[4] for b in h4]
    h = [b[2] for b in h4]; l = [b[3] for b in h4]
    n = len(c)
    trades = []
    pos = 0; entry = 0.0; ei = 0
    sma_sum = 0.0
    dn_streak = up_streak = 0
    win = []
    for i in range(n):
        win.append(c[i])
        if len(win) > k: win.pop(0)
        # sustained-trend state vs SMA(sma_n)
        if i >= sma_n:
            sma = sum(c[i - sma_n + 1:i + 1]) / sma_n
            if c[i] < sma: dn_streak += 1; up_streak = 0
            elif c[i] > sma: up_streak += 1; dn_streak = 0
        if len(win) < k or i < sma_n:
            continue
        mu = sum(win) / k
        sd = statistics.pstdev(win)
        if sd <= 0:
            continue
        z = (c[i] - mu) / sd
        if pos != 0:
            # resting catastrophe loss-cut, intrabar
            lvl = entry * (1 - pos * lc_bp / 1e4)
            touched = (l[i] <= lvl) if pos > 0 else (h[i] >= lvl)
            if touched:
                trades.append((ts[i], pos, -lc_bp - cost_bp)); pos = 0; continue
            reverted = (z >= -z_out) if pos > 0 else (z <= z_out)
            if reverted or (i - ei) >= timeout:
                trades.append((ts[i], pos, pos * (c[i] - entry) / entry * 1e4 - cost_bp))
                pos = 0
            continue
        veto_long = ('L' in veto_sides) and dn_streak >= streak_n
        veto_short = ('S' in veto_sides) and up_streak >= streak_n
        if z <= -z_in and not veto_long:
            pos = 1; entry = c[i]; ei = i
        elif z >= z_in and not veto_short:
            pos = -1; entry = c[i]; ei = i
    if pos != 0:
        trades.append((ts[-1], pos, pos * (c[-1] - entry) / entry * 1e4 - cost_bp))
    return trades


# ── validation battery ───────────────────────────────────────────────────────
def stats(trs):
    rets = [r for *_, r in trs]
    if not rets:
        return dict(n=0, net=0.0, pf=0.0, h1=0.0, h2=0.0, exbest=0.0, wf=False)
    tss = [t[0] for t in trs]
    mid_t = tss[0] + (tss[-1] - tss[0]) / 2.0
    h1 = sum(r for t in trs if t[0] < mid_t for r in [t[-1]])
    h2 = sum(r for t in trs if t[0] >= mid_t for r in [t[-1]])
    net = sum(rets)
    gw = sum(r for r in rets if r > 0); gl = -sum(r for r in rets if r < 0)
    pf = gw / gl if gl > 0 else 99.9
    return dict(n=len(rets), net=net, pf=pf, h1=h1, h2=h2,
                exbest=net - max(rets), wf=(net > 0 and h1 > 0 and h2 > 0))


def rand_ctl_ladder(bars, W, thr, cost, nwins, d, **kw):
    nets = []
    for sd in range(5):
        random.seed(1000 + sd)
        cand = random.sample(range(W, len(bars)), min(nwins, len(bars) - W)) if nwins else []
        rr, _ = ladder(bars, W, thr, cost, d=d, seed_windows=cand, **kw)
        nets.append(stats(rr)['net'])
    return sum(nets) / len(nets) if nets else 0.0


def rand_ctl_hold(bars, trades, cost):
    """Random-entry control for rider/MR: same n, same dir mix, median hold, cost debited."""
    if not trades:
        return 0.0
    idx = {b[0]: i for i, b in enumerate(bars)}
    holds = []
    prev_t = None
    for t in trades:
        pass
    # median hold in bars from trade spacing is unreliable; approximate via ts deltas
    tss = [t[0] for t in trades]
    if len(tss) > 1:
        med_hold = max(1, int(statistics.median([(tss[i + 1] - tss[i]) for i in range(len(tss) - 1)]) // 3600 / 2))
    else:
        med_hold = 4
    dirs = [t[1] for t in trades if len(t) > 2]
    if not dirs:
        dirs = [1] * len(trades)
    nets = []
    for sd in range(5):
        random.seed(2000 + sd)
        tot = 0.0
        for d in dirs:
            i = random.randrange(0, len(bars) - med_hold - 1)
            e = bars[i][4]; x = bars[i + med_hold][4]
            tot += d * (x - e) / e * 1e4 - cost
        nets.append(tot)
    return sum(nets) / len(nets)


def row(mech, pair, cfg, dr, s, net2x, rnd, extra=""):
    over = s['net'] - rnd
    print(f"ROW|{mech}|{pair}|{cfg}|{dr}|{s['n']}|{s['net']:+.0f}|{s['pf']:.2f}"
          f"|{s['h1']:+.0f}|{s['h2']:+.0f}|{s['exbest']:+.0f}|{net2x:+.0f}|{rnd:+.0f}|{over:+.0f}|{extra}",
          flush=True)


# ── stages ───────────────────────────────────────────────────────────────────
def stage_anchors(data):
    print("\n=== STAGE 0: ANCHOR REPRODUCTION (upjump LONG, sweep2 costs) ===")
    sweep2_cost = {"EURUSD": 2.0, "GBPUSD": 2.0, "NZDUSD": 2.5, "AUDUSD": 2.0}
    for pair, (W, thr) in ANCHORS.items():
        if pair not in data:
            continue
        bars = data[pair]
        cost = sweep2_cost[pair]
        trs, wins = ladder(bars, W, thr, cost, d=1)
        s = stats(trs)
        s2 = stats(ladder(bars, W, thr, cost * 2, d=1)[0])
        rnd = rand_ctl_ladder(bars, W, thr, cost, len(wins), 1)
        row("ANCHOR", pair, f"W{W}/thr{thr}", "L", s, s2['net'], rnd)


def stage_ladder(data, d, mech):
    print(f"\n=== {mech}: giveback ladder {'LONG' if d > 0 else 'SHORT'} grid (cost {BASE_COST}bp / stress {BASE_COST*2}bp) ===")
    grid_rows = {}
    for pair, bars in data.items():
        for W in (12, 24, 48, 72, 96):
            for thr in (0.25, 0.5, 0.75, 1.0, 1.5, 2.0):
                trs, wins = ladder(bars, W, thr, BASE_COST, d=d)
                s = stats(trs)
                if s['n'] == 0:
                    continue
                s2 = stats(ladder(bars, W, thr, BASE_COST * 2, d=d)[0])
                rnd = rand_ctl_ladder(bars, W, thr, BASE_COST, len(wins), d)
                tag = "arm<=1%" if 2.7 * thr <= 1.0 else ""
                grid_rows.setdefault(pair, []).append((W, thr, s, s2['net'], rnd))
                row(mech, pair, f"W{W}/thr{thr}", "L" if d > 0 else "S", s, s2['net'], rnd, tag)
    # plateau tags for the best cell per pair
    for pair, rows_ in grid_rows.items():
        pos = [r for r in rows_ if r[2]['net'] > 0 and r[2]['wf']]
        if not pos:
            continue
        best = max(pos, key=lambda r: r[2]['net'])
        W0, t0 = best[0], best[1]
        neigh = [r for r in rows_ if (r[0] == W0 and r[1] != t0) or (r[1] == t0 and r[0] != W0)]
        avg = sum(r[2]['net'] for r in neigh) / len(neigh) if neigh else 0.0
        print(f"PLATEAU|{mech}|{pair}|W{W0}/thr{t0}|neigh_avg={avg:+.0f}|{'SPIKE' if avg <= 0 else 'ok'}")


def stage_rider(data):
    print(f"\n=== RIDER: flip / BE-lock grid (cost {BASE_COST}bp / stress {BASE_COST*2}bp) ===")
    for pair, bars in data.items():
        allr = {}
        for W in (2, 4):
            for thr in (0.25, 0.3, 0.4, 0.5):
                for ex in ("FLIP", "BEL05", "BEL10", "BEL25"):
                    trs = rider(bars, W, thr, BASE_COST, ex)
                    s = stats(trs)
                    if s['n'] < 5:
                        continue
                    s2 = stats(rider(bars, W, thr, BASE_COST * 2, ex))
                    rnd = rand_ctl_hold(bars, trs, BASE_COST)
                    nl = sum(1 for t in trs if t[1] > 0); ns = s['n'] - nl
                    netl = sum(t[2] for t in trs if t[1] > 0); nets = sum(t[2] for t in trs if t[1] < 0)
                    allr[(W, thr, ex)] = s
                    row("RIDER", pair, f"W{W}/thr{thr}/{ex}", "LS", s, s2['net'], rnd,
                        f"L:{nl}/{netl:+.0f} S:{ns}/{nets:+.0f}")
        # plateau on best
        pos = [(k, v) for k, v in allr.items() if v['net'] > 0 and v['wf']]
        if pos:
            (W0, t0, e0), bs = max(pos, key=lambda kv: kv[1]['net'])
            neigh = [v for (W, t, e), v in allr.items() if W == W0 and e == e0 and t != t0]
            avg = sum(v['net'] for v in neigh) / len(neigh) if neigh else 0.0
            print(f"PLATEAU|RIDER|{pair}|W{W0}/thr{t0}/{e0}|neigh_avg={avg:+.0f}|{'SPIKE' if avg <= 0 else 'ok'}")


def stage_mr(data):
    print(f"\n=== MRH4: z-band fade + asym veto (cost {BASE_COST}bp / stress {BASE_COST*2}bp) ===")
    for pair, bars in data.items():
        h4 = to_h4(bars)
        for z_in in (1.5, 2.0, 2.5):
            for veto in ("", "L", "S", "LS"):
                trs = mrh4(h4, z_in, veto, BASE_COST)
                s = stats(trs)
                if s['n'] < 5:
                    continue
                s2 = stats(mrh4(h4, z_in, veto, BASE_COST * 2))
                rnd = rand_ctl_hold(h4, trs, BASE_COST)
                nl = sum(1 for t in trs if t[1] > 0); ns = s['n'] - nl
                netl = sum(t[2] for t in trs if t[1] > 0); nets = sum(t[2] for t in trs if t[1] < 0)
                row("MRH4", pair, f"z{z_in}/veto{veto or '-'}", "LS", s, s2['net'], rnd,
                    f"L:{nl}/{netl:+.0f} S:{ns}/{nets:+.0f}")


def stage_regime():
    """Run key configs on the 2022 dollar-rally / multi-year regime files."""
    print("\n=== REGIME SPLIT (2022 dollar rally + USDJPY multi-year) ===")
    for pair, files in REGIME.items():
        for tag, path in files:
            if not os.path.exists(path):
                print(f"REGIME|{pair}|{tag}|MISSING")
                continue
            bars = load(path)
            # ladder long anchor / best + ladder short best + rider best come from
            # the main grids; here run the standard candidate set so the MD can quote
            for W, thr in ((24, 0.5), (48, 0.5), (48, 1.0), (96, 1.0)):
                for d, dr in ((1, "L"), (-1, "S")):
                    trs, wins = ladder(bars, W, thr, BASE_COST, d=d)
                    s = stats(trs)
                    if s['n'] == 0:
                        continue
                    s2 = stats(ladder(bars, W, thr, BASE_COST * 2, d=d)[0])
                    rnd = rand_ctl_ladder(bars, W, thr, BASE_COST, len(wins), d)
                    row(f"REG-LAD-{tag}", pair, f"W{W}/thr{thr}", dr, s, s2['net'], rnd)
            for W, thr, ex in ((2, 0.3, "FLIP"), (2, 0.3, "BEL10"), (4, 0.5, "FLIP"), (2, 0.25, "FLIP")):
                trs = rider(bars, W, thr, BASE_COST, ex)
                s = stats(trs)
                if s['n'] == 0:
                    continue
                s2 = stats(rider(bars, W, thr, BASE_COST * 2, ex))
                rnd = rand_ctl_hold(bars, trs, BASE_COST)
                row(f"REG-RID-{tag}", pair, f"W{W}/thr{thr}/{ex}", "LS", s, s2['net'], rnd)
            h4 = to_h4(bars)
            for z_in, veto in ((2.0, ""), (2.0, "L"), (2.0, "S"), (2.5, "LS")):
                trs = mrh4(h4, z_in, veto, BASE_COST)
                s = stats(trs)
                if s['n'] == 0:
                    continue
                s2 = stats(mrh4(h4, z_in, veto, BASE_COST * 2))
                rnd = rand_ctl_hold(h4, trs, BASE_COST)
                row(f"REG-MR-{tag}", pair, f"z{z_in}/veto{veto or '-'}", "LS", s, s2['net'], rnd)


def main():
    which = set(a.upper() for a in sys.argv[1:]) or {"ANCHOR", "UPJUMP", "DNJUMP", "RIDER", "MRH4", "REGIME"}
    data = {}
    for pair, path in PAIRS:
        if not os.path.exists(path):
            print(f"{pair}: MISSING {path}")
            continue
        bars = load(path)
        span_d = (bars[-1][0] - bars[0][0]) / 86400.0
        print(f"DATA|{pair}|{len(bars)} bars|{span_d:.0f}d|{os.path.basename(path)}")
        data[pair] = bars
    print("COLS: MECH|PAIR|CFG|DIR|n|net_bp|PF|half1_bp|half2_bp|exbest_bp|net2x_bp|rnd_bp|over_rnd_bp|extra")
    if "ANCHOR" in which: stage_anchors(data)
    if "UPJUMP" in which: stage_ladder(data, 1, "UPJUMP")
    if "DNJUMP" in which: stage_ladder(data, -1, "DNJUMP")
    if "RIDER" in which: stage_rider(data)
    if "MRH4" in which: stage_mr(data)
    if "REGIME" in which: stage_regime()


if __name__ == "__main__":
    main()
