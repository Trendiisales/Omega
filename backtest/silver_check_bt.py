#!/usr/bin/env python3
"""SILVER independent-edge check (operator order, 2026-07-08). RESEARCH ONLY.

Data (built today):
  /Users/jo/Tick/XAGUSD_2022_2026.h1.csv  (histdata ticks 2022-01..2024-01 EST+5h,
      merged with /Users/jo/Tick/xag/XAGUSD_h1_clean.csv 2024-01-15..2026-07-06;
      seam median close diff 0.017%)
  /Users/jo/Tick/XAGUSD_2022_2026.h4.csv  (resampled from merged H1)
  /Users/jo/Tick/XAGUSD_2022_2024.m30.csv (tick-span only) -- gate CERTIFIED
  Gate note: full-span H1/H4 FAIL the 3x-median glitch heuristic ONLY because of
  the REAL Jan-2026 squeeze ($17.6 -> $121.7), cross-verified vs independent
  yahoo SI=F daily (max close 115.08 @ 2026-01-26); 0 h1-to-h1 jumps >10%
  (max 9.12%). Documented false positive per ENGINE_BACKTEST_REGISTRY §5.

COST BASIS (spot XAG, stated honestly):
  commission RT = 2*0.00025*px = 0.0005*px  (0.05% RT, silver spot basis)
  spread       = 0.03 (OBSERVED tick avg across 34.3M histdata ticks 2022-24;
                 worse than the 0.02 assumption -- silver spreads are
                 proportionally ~3-4x gold's 4bp)
  Engine-dump fills already embed spread via bid/ask = px -/+ 0.015; the scorer
  adds commission. Close-fill books debit full RT = spread + commission.
  2x stress doubles BOTH legs.

Tests:
  (a) XauTF-family trend clone (real XauTrendFollow4h mask 0xC9 + 2h engines,
      dumps from backtest/xag_tf_dump.cpp) on XAG H4/H1 + GOLD-SIGNAL control
      (gold trade windows mapped onto silver closes vs silver's own windows
      mapped identically -- identical fill machinery, overlap period only).
  (b) MGC-style Donchian breakout LONG on XAG M30 (certified file) + H1,
      Nin=20/40, Nout=Nin/2 close-exit, regime gate close>SMA200.
  (c) TSMOM {42,63,84}d vol-targeted (10% ann target) D1 both directions on
      10.5yr SI=F daily (yahoo cache). Futures cost basis: 2bp RT + 1 tick
      ($0.005) slippage per side on price -- stated as SI futures honest.
  (d) Up-jump ladder (omega_upjump_ladder_bt mechanics, thr-scaled clips,
      LONG) on XAG H1, W x thr sweep, random-window control (5 seeds).
"""
import csv, math, os, random, sys, importlib.util

TICK = "/Users/jo/Tick"
SCRATCH = sys.argv[1] if len(sys.argv) > 1 else "/tmp"
COMM = 0.0005          # RT commission as fraction of px
SPREAD = 0.03          # full spread (RT spread cost when crossing twice at close-fills)

def load_bars(path):
    out = []
    with open(path) as f:
        for ln in f:
            p = ln.strip().split(',')
            if len(p) < 5 or not p[0][:1].isdigit(): continue
            try: out.append((int(float(p[0])), float(p[1]), float(p[2]), float(p[3]), float(p[4])))
            except ValueError: continue
    return out

def stats(trades, label, y2022=None):
    """trades: list of (exit_ts, net_pnl). Prints the all-6 panel."""
    if not trades:
        print(f"  {label}: NO TRADES"); return None
    trades = sorted(trades)
    n = len(trades); net = sum(t[1] for t in trades)
    gw = sum(t[1] for t in trades if t[1] > 0); gl = -sum(t[1] for t in trades if t[1] < 0)
    pf = gw/gl if gl > 0 else 99.9
    mid = trades[n//2][0]
    h1 = sum(t[1] for t in trades if t[0] < mid); h2 = net - h1
    ex_best = net - max(t[1] for t in trades)
    w22 = sum(t[1] for t in trades if 1640995200 <= t[0] < 1672531200) if y2022 is None else y2022
    n22 = sum(1 for t in trades if 1640995200 <= t[0] < 1672531200)
    print(f"  {label}: n={n} net={net:+.2f} PF={pf:.2f} WF[{h1:+.2f}/{h2:+.2f}] 2022[{w22:+.2f} n={n22}] exBest={ex_best:+.2f}")
    return dict(n=n, net=net, pf=pf, h1=h1, h2=h2, w2022=w22, exbest=ex_best)

# ---------------------------------------------------------------- (a) TF trend
def read_dump(path):
    out = []
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            out.append(dict(side=row['side'], ets=int(row['entry_ts']), xts=int(row['exit_ts']),  # engine emits SECONDS
                            epx=float(row['entry_px']), xpx=float(row['exit_px']), pts=float(row['pnl_pts'])))
    return out

def tf_score(dump_path, label, comm_frac, x2=False):
    tr = read_dump(dump_path)
    out = []
    for t in tr:
        c = comm_frac*t['epx'] + (comm_frac*t['epx'] + SPREAD if x2 else 0.0)  # 2x: add another commission+spread RT
        out.append((t['xts'], t['pts'] - c))
    return stats(out, label)

def close_map_book(windows, bars, comm_frac, spread, label):
    """windows: (ets, xts, side). Fill at first bar close with ts>=ets / >=xts."""
    ts_list = [b[0] for b in bars]
    import bisect
    out = []
    for ets, xts, side in windows:
        i = bisect.bisect_left(ts_list, ets); j = bisect.bisect_left(ts_list, xts)
        if i >= len(bars) or j >= len(bars): continue
        e, x = bars[i][4], bars[j][4]
        pnl = (x - e) if side in ('BUY', 'LONG') else (e - x)
        out.append((bars[j][0], pnl - spread - comm_frac*e))
    return stats(out, label)

# ---------------------------------------------------------------- (b) Donchian long
def donchian(bars, Nin, use_gate, comm_frac, spread, label):
    Nout = Nin//2
    sma_w = 200
    closes = [b[4] for b in bars]
    pos = None; out = []
    hh = ll = None
    for i in range(max(Nin, sma_w)+1, len(bars)):
        ts, o, h, l, c = bars[i]
        hh = max(b[2] for b in bars[i-Nin:i])
        ll = min(b[4] for b in bars[i-Nout:i])
        sma = sum(closes[i-sma_w:i])/sma_w
        if pos is None:
            gate_ok = (c > sma) if use_gate else True
            if gate_ok and h > hh:
                epx = max(o, hh) + spread/2  # buy at breakout level (or gap open), pay half-spread
                pos = (epx, ts)
        else:
            if c < ll:  # close-based exit (Nout-low close breach)
                xpx = c - spread/2
                out.append((ts, xpx - pos[0] - comm_frac*pos[0]))
                pos = None
    if pos is not None:
        out.append((bars[-1][0], bars[-1][4] - spread/2 - pos[0] - comm_frac*pos[0]))
    return stats(out, label)

# ---------------------------------------------------------------- (c) TSMOM daily
def tsmom(path, lb, comm_frac_rt, label):
    rows = []
    with open(path) as f:
        r = csv.reader(f); next(r)
        for p in r:
            try: rows.append((p[0], float(p[4])))
            except (ValueError, IndexError): continue
    closes = [c for _, c in rows]
    dates = [d for d, _ in rows]
    rets = [closes[i]/closes[i-1]-1 for i in range(1, len(closes))]
    VOL_W = 20; TGT = 0.10/math.sqrt(252)
    pos = 0.0; out = []; eq = 0.0
    import datetime
    for i in range(max(lb, VOL_W)+1, len(closes)):
        sig = 1.0 if closes[i-1] > closes[i-1-lb] else -1.0
        vol = (sum(r*r for r in rets[i-1-VOL_W:i-1])/VOL_W) ** 0.5
        tgt = sig * min(TGT/max(vol, 1e-6), 4.0)
        r_day = rets[i-1]  # position held from close i-1 to close i
        pnl = pos * r_day - abs(tgt - pos) * comm_frac_rt/2  # cost per side on turnover
        ts = int(datetime.datetime.strptime(dates[i], "%Y-%m-%d").replace(tzinfo=datetime.timezone.utc).timestamp())
        out.append((ts, pnl*100))  # % of notional
        pos = tgt
    return stats(out, label)

# ---------------------------------------------------------------- (d) upjump ladder
spec = importlib.util.spec_from_file_location("ujl", os.path.join(os.path.dirname(os.path.abspath(__file__)), "omega_upjump_ladder_bt.py"))
ujl = importlib.util.module_from_spec(spec); spec.loader.exec_module(ujl)

def upjump(bars, cost_bp, label):
    print(f"\n  -- upjump ladder {label} (rt={cost_bp}bp; W x thr sweep, rand ctrl 5 seeds) --")
    print(f"  {'W':>3} {'thr':>4} | {'n':>5} {'net%':>8} {'PF':>5} {'WF':>3} | {'2x net/PF':>13} | {'rand':>8}")
    best = []
    for W in (24, 48, 96):
        for thr in (1.0, 2.0, 3.0):
            rets, wins = ujl.run(bars, W, thr, cost_bp)
            s = ujl.stats(rets)
            if s['n'] == 0: continue
            s2 = ujl.stats(ujl.run(bars, W, thr, cost_bp*2)[0])
            rnd_nets = []
            for sd in range(5):
                random.seed(1000+sd)
                cand = random.sample(range(W, len(bars)), min(len(wins), len(bars)-W)) if wins else []
                rnd_nets.append(ujl.stats(ujl.run(bars, W, thr, cost_bp, seed_windows=cand)[0])['net'])
            rnd = sum(rnd_nets)/len(rnd_nets)
            print(f"  {W:3d} {thr:4.1f} | {s['n']:5d} {s['net']:+8.1f} {s['pf']:5.2f} {'Y' if s['wf'] else 'n':>3} | {s2['net']:+7.1f}/{s2['pf']:4.2f} | {rnd:+8.1f}")
            best.append((s['net'], W, thr, s, s2, rnd))
    return best

# ================================================================ main
if __name__ == "__main__":
    xag_h1 = load_bars(f"{TICK}/XAGUSD_2022_2026.h1.csv")
    xag_h4 = load_bars(f"{TICK}/XAGUSD_2022_2026.h4.csv")
    xag_m30 = load_bars(f"{TICK}/XAGUSD_2022_2024.m30.csv")

    print("=== (a) XauTF trend clone on XAG (engine dumps, spread-embedded fills + commission) ===")
    tf_score(f"{SCRATCH}/xag_tf4h.csv", "XAG 4h mask0xC9 1x", COMM)
    tf_score(f"{SCRATCH}/xag_tf4h.csv", "XAG 4h mask0xC9 2x", COMM, x2=True)
    tf_score(f"{SCRATCH}/xag_tf2h.csv", "XAG 2h defaults 1x", COMM)
    tf_score(f"{SCRATCH}/xag_tf2h.csv", "XAG 2h defaults 2x", COMM, x2=True)

    print("\n=== (a-control) GOLD-signal vs OWN-signal on silver (close-fill mapped, overlap 2022-01..2026-04) ===")
    xau4 = read_dump(f"{SCRATCH}/xau_tf4h.csv"); xag4 = read_dump(f"{SCRATCH}/xag_tf4h.csv")
    xau2 = read_dump(f"{SCRATCH}/xau_tf2h.csv"); xag2 = read_dump(f"{SCRATCH}/xag_tf2h.csv")
    end_overlap = 1777060800
    for tag, gold_d, silv_d in (("4h", xau4, xag4), ("2h", xau2, xag2)):
        gw = [(t['ets'], t['xts'], t['side']) for t in gold_d if t['xts'] <= end_overlap]
        sw = [(t['ets'], t['xts'], t['side']) for t in silv_d if t['xts'] <= end_overlap]
        close_map_book(gw, xag_h1, COMM, SPREAD, f"{tag} GOLD-signal on silver  (n_windows={len(gw)})")
        close_map_book(sw, xag_h1, COMM, SPREAD, f"{tag} OWN-signal  on silver  (n_windows={len(sw)})")

    print("\n=== (b) Donchian breakout LONG (MGC-style), spot-silver costs ===")
    for bars, tag in ((xag_m30, "M30 2022-24 CERT"), (xag_h1, "H1 2022-26")):
        for Nin in (20, 40):
            donchian(bars, Nin, True,  COMM, SPREAD, f"{tag} Nin={Nin} gated 1x")
            donchian(bars, Nin, True,  COMM*2, SPREAD*2, f"{tag} Nin={Nin} gated 2x")
            donchian(bars, Nin, False, COMM, SPREAD, f"{tag} Nin={Nin} ungated 1x")

    print("\n=== (c) TSMOM D1 SI=F 2015-2026, vol-target 10%ann, both directions (net % of notional) ===")
    for lb in (42, 63, 84):
        tsmom(f"{TICK}/SI_F_daily_2016_2026_yahoo.csv", lb, 0.0004, f"SI=F TSMOM {lb}d 1x(4bpRT)")
        tsmom(f"{TICK}/SI_F_daily_2016_2026_yahoo.csv", lb, 0.0008, f"SI=F TSMOM {lb}d 2x(8bpRT)")

    print("\n=== (d) Up-jump ladder on XAG H1 (rt = spread+comm in bp of px) ===")
    # honest bp: spread 0.03 + comm 0.0005*px, at median px 27.4 -> (0.03+0.0137)/27.4 = 16bp
    upjump(xag_h1, 16.0, "XAG H1 2022-26")
