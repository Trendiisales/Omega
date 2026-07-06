#!/usr/bin/env python3
"""ladder_vs_trail_real.py — REAL-fill, REAL-cost comparison of the three exit schemes
inside the validated jump-window detector, on every symbol (operator ask 2026-07-07):

  TRAIL5  : the shipped BE-floor trail tiers r20/50/100/150/400 (per-tier trail giveback,
            floor at entry, reclip-from-exit).
  TP7     : the operator's proposed take-profit LADDER 50/100/150/200/250/300/400 —
            all legs arm together at BE+cost, each closes ONE BY ONE at its fixed target
            (limit fill AT the target), remaining legs all close on the reversal
            (window end) or at their BE floor.
  RIDER   : the JumpRider baseline — one position, in at the window signal, out at the
            symmetric reverse, BE-ratchet (arm thr/2 -> floor BE) + hard stop 2x thr.

FILL MODEL (identical to the live engines' honest-accounting semantics):
  * bars = H1 closes only. Trail/BE-floor/hard-stop exits fill at the OBSERVED CLOSE
    (worse-of floor/close — a stop is an order target, not a fill).
  * TP exits fill AT the target level once a close trades through it (a resting limit
    at the target fills at target-or-better; conservative for closes beyond it).
  * every clip is debited the symbol's REAL round-trip cost (the rt_cost_bp table
    wired in engine_init / Binance taker+slip for crypto).
Windows: detect_ semantics (ei = i+1 ref anchor, exit at xi = i+1, contiguity guard).
Both flavors run: Pos (up-windows, long) and Neg (down-windows, short) — except crypto
(spot venue, long-only), where the bear test is the calendar-year split instead.

Data: phase1/signal_discovery/warmup_*_H1.csv (repo-bundled real H1) + Binance Vision
monthly 1h for BTC/ETH. This is a REAL backtest, not a model: all figures net of cost.
"""
import csv, glob, os

HERE = os.path.dirname(os.path.abspath(__file__))
WARM = os.path.join(HERE, "..", "phase1", "signal_discovery")
CDATA = os.environ.get("CDATA", "")   # dir of Binance Vision monthly CSVs (optional)

TRAIL5 = [20.0, 50.0, 100.0, 150.0, 400.0]
TP7    = [50.0, 100.0, 150.0, 200.0, 250.0, 300.0, 400.0]

# sym, file, W, thr, be_bp(arm), rt_cost_bp, allow_short, bar_sec
SYMS = [
    ("XAUUSD", "warmup_XAUUSD_H1.csv", 2, 0.010, 6.0,  6.0, True, 3600),
    ("XAGUSD", "warmup_XAGUSD_H1.csv", 2, 0.010, 6.0,  6.0, True, 3600),
    ("USOIL",  "warmup_USOIL_H1.csv",  2, 0.010, 10.0, 8.0, True, 3600),
    ("EURUSD", "warmup_EURUSD_H1.csv", 2, 0.003, 2.0,  4.0, True, 3600),
    ("GBPUSD", "warmup_GBPUSD_H1.csv", 2, 0.003, 2.0,  3.5, True, 3600),
    ("USDJPY", "warmup_USDJPY_H1.csv", 2, 0.003, 2.0,  3.0, True, 3600),
    ("AUDUSD", "warmup_AUDUSD_H1.csv", 2, 0.003, 2.0,  6.0, True, 3600),
    ("NZDUSD", "warmup_NZDUSD_H1.csv", 2, 0.003, 2.0,  7.0, True, 3600),
    ("US500",  "warmup_US500_H1.csv",  2, 0.003, 6.0,  4.0, True, 3600),
    ("NAS100", "warmup_NAS100_H1.csv", 2, 0.003, 6.0,  3.0, True, 3600),
    ("DJ30",   "warmup_DJ30_H1.csv",   2, 0.003, 6.0,  2.0, True, 3600),
    ("GER40",  "warmup_GER40_H1.csv",  2, 0.003, 6.0,  2.0, True, 3600),
]
CRYPTO = [   # UpJump parent definition: W=24 bars(1h)=24h, thr=2%; Binance real RT 23bp
    ("BTCUSDT", 24, 0.02, 25.0, 23.0, False, 3600),
    ("ETHUSDT", 24, 0.02, 25.0, 23.0, False, 3600),
]

FILL = os.environ.get("FILL", "close")   # close = engine-as-implemented (H1-close polling)
                                         # resting = resting broker orders (stop/limit AT levels,
                                         #           gap-open fills at the open; uses bar O/H/L)
RIDER_BE = os.environ.get("RIDER_BE", "1") != "0"   # BE-ratchet+hard-stop on the rider (provisional
                                                    # defaults); 0 = pure ride-to-symmetric-flip

def load_csv(path):
    rows = {}
    with open(path) as f:
        for row in csv.reader(f):
            if not row or not row[0][:1].isdigit(): continue
            t = float(row[0])
            while t > 4e10: t /= 1000.0          # ms or us -> sec
            rows[int(t)] = (float(row[1]), float(row[2]), float(row[3]), float(row[4]))
    ts = sorted(rows)
    o = [rows[t][0] for t in ts]; h = [rows[t][1] for t in ts]
    l = [rows[t][2] for t in ts]; c = [rows[t][3] for t in ts]
    return ts, o, h, l, c

def windows(ts, c, W, thr, up, bar_sec):
    """detect_ semantics: (ei, xi) with ei=i+1 anchor, exit xi=i+1; contiguity-gated entries."""
    out = []; pos = False; ei = 0
    for i in range(W, len(c)):
        j = c[i] / c[i - W] - 1.0
        contig = (ts[i] - ts[i - W]) <= W * bar_sec * 2
        enter = contig and (j >= thr if up else j <= -thr)
        exit_ = (j <= -thr) if up else (j >= thr)
        if not pos and enter:
            if i + 1 < len(c): pos = True; ei = i + 1
        elif pos and exit_:
            out.append((ei, min(i + 1, len(c) - 1))); pos = False
    if pos: out.append((ei, len(c) - 1))
    return out

def trail_scheme(o, h, l, c, wins_, is_long, gb_bp, be_bp, cost_bp):
    """BE-floor trail leg. close mode: engine-as-implemented (close-polled, fill at close).
    resting mode: stop order re-placed each bar close (close-based wm); intrabar touch fills
    AT the stop, gap-open through it fills at the open."""
    clips = []
    d = 1.0 if is_long else -1.0
    for ei, xi in wins_:
        ref = c[ei]; has = False; entry = wm = 0.0
        for i in range(ei, xi):
            cur = c[i]
            if not has:
                if d * (cur / ref - 1.0) * 1e4 >= be_bp:
                    entry = wm = cur; has = True
                continue
            stop = max(entry, wm * (1 - gb_bp / 1e4)) if is_long else min(entry, wm * (1 + gb_bp / 1e4))
            if FILL == "resting":
                touched = (l[i] <= stop) if is_long else (h[i] >= stop)
                if touched:
                    fill = min(o[i], stop) if is_long else max(o[i], stop)
                    clips.append(d * (fill - entry) / entry - cost_bp / 1e4)
                    has = False; wm = 0.0; ref = stop
                    continue
                wm = max(wm, cur) if is_long else min(wm, cur)   # re-place order off the new close
            else:
                wm = max(wm, cur) if is_long else min(wm, cur)
                stop = max(entry, wm * (1 - gb_bp / 1e4)) if is_long else min(entry, wm * (1 + gb_bp / 1e4))
                hit = cur <= stop if is_long else cur >= stop
                if hit:
                    clips.append(d * (cur - entry) / entry - cost_bp / 1e4)   # fill at close
                    has = False; wm = 0.0; ref = stop
        if has:
            last = c[xi - 1] if xi - 1 >= ei else c[ei]
            clips.append(d * (last - entry) / entry - cost_bp / 1e4)          # window flush at close
    return clips

def tp_scheme(o, h, l, c, wins_, is_long, targets_bp, be_bp, cost_bp):
    """operator's TP ladder: all legs arm together at BE+cost; each exits at ITS fixed target;
    BE floor stop at entry; reversal (window end) closes the rest. Per-leg reclip from exit.
    close mode: decisions+fills at closes (TP fills AT target once a close trades through).
    resting mode: limit AT target / stop AT entry, intrabar touch via H/L, gap-open at open;
    stop checked FIRST when both touch in one bar (conservative)."""
    d = 1.0 if is_long else -1.0
    clips = {t: [] for t in targets_bp}
    for ei, xi in wins_:
        state = {t: [c[ei], False, 0.0] for t in targets_bp}
        for i in range(ei, xi):
            cur = c[i]
            for t in targets_bp:
                ref, has, entry = state[t]
                if not has:
                    if d * (cur / ref - 1.0) * 1e4 >= be_bp:
                        state[t] = [ref, True, cur]
                    continue
                tgt = entry * (1 + d * t / 1e4)
                if FILL == "resting":
                    be_touch = (l[i] <= entry) if is_long else (h[i] >= entry)
                    tp_touch = (h[i] >= tgt) if is_long else (l[i] <= tgt)
                    if be_touch:                              # stop first (conservative)
                        fill = min(o[i], entry) if is_long else max(o[i], entry)
                        clips[t].append(d * (fill - entry) / entry - cost_bp / 1e4)
                        state[t] = [fill, False, 0.0]
                    elif tp_touch:                            # limit fills AT the target
                        clips[t].append(t / 1e4 - cost_bp / 1e4)
                        state[t] = [tgt, False, 0.0]
                else:
                    hit_tp = cur >= tgt if is_long else cur <= tgt
                    hit_be = cur <= entry if is_long else cur >= entry
                    if hit_tp:
                        clips[t].append(t / 1e4 - cost_bp / 1e4)
                        state[t] = [tgt, False, 0.0]
                    elif hit_be:                              # close-polled stop: fill at close
                        clips[t].append(d * (cur - entry) / entry - cost_bp / 1e4)
                        state[t] = [cur, False, 0.0]
        last = c[xi - 1]
        for t in targets_bp:                                  # reversal: all remaining close at close
            ref, has, entry = state[t]
            if has: clips[t].append(d * (last - entry) / entry - cost_bp / 1e4)
    return clips

def rider_scheme(ts, o, h, l, c, W, thr, cost_bp, allow_short, bar_sec):
    """JumpRider: enter at window-signal close, exit at symmetric flip; optional BE-ratchet +
    hard stop (RIDER_BE): close-polled in close mode, resting stops (H/L touch) in resting mode."""
    trades = []
    pos = 0; entry = peak = 0.0; block_up = block_dn = False
    for i in range(W, len(c)):
        cur = c[i]
        j = cur / c[i - W] - 1.0
        contig = (ts[i] - ts[i - W]) <= W * bar_sec * 2
        up = j >= thr; dn = j <= -thr
        if not up: block_up = False
        if not dn: block_dn = False
        if pos != 0:
            closed = False
            if RIDER_BE:
                hs_lvl = entry * (1 - pos * 2 * thr)
                be_armed = peak >= thr / 2
                if FILL == "resting":
                    hs_touch = (l[i] <= hs_lvl) if pos > 0 else (h[i] >= hs_lvl)
                    be_touch = be_armed and ((l[i] <= entry) if pos > 0 else (h[i] >= entry))
                    if hs_touch:
                        fill = min(o[i], hs_lvl) if pos > 0 else max(o[i], hs_lvl)
                        trades.append((ts[i], pos * (fill - entry) / entry - cost_bp / 1e4)); closed = True
                    elif be_touch:
                        fill = min(o[i], entry) if pos > 0 else max(o[i], entry)
                        trades.append((ts[i], pos * (fill - entry) / entry - cost_bp / 1e4)); closed = True
                else:
                    ret = pos * (cur - entry) / entry
                    if ret <= -2 * thr or (peak >= thr / 2 and ret <= 0):
                        trades.append((ts[i], ret - cost_bp / 1e4)); closed = True
                if closed:
                    pos = 0
                    if up: block_up = True
                    if dn: block_dn = True
            if not closed:
                ret = pos * (cur - entry) / entry
                peak = max(peak, ret)
                flip = dn if pos > 0 else up
                if flip:
                    trades.append((ts[i], ret - cost_bp / 1e4)); pos = 0
                    if contig and ((dn and allow_short and not block_dn) or (up and not block_up)):
                        nd = -1 if dn else 1
                        if nd > 0 or allow_short:
                            pos = nd; entry = cur; peak = 0.0
        elif contig:
            if up and not block_up: pos = 1; entry = cur; peak = 0.0
            elif dn and allow_short and not block_dn: pos = -1; entry = cur; peak = 0.0
    if pos != 0: trades.append((ts[-1], pos * (c[-1] - entry) / entry - cost_bp / 1e4))
    return trades

def stats(rets):
    if not rets: return dict(n=0, net=0.0, wr=0.0, worst=0.0, dd=0.0)
    cum = peak = dd = 0.0
    for r in rets:
        cum += r; peak = max(peak, cum); dd = max(dd, peak - cum)
    return dict(n=len(rets), net=sum(rets) * 1e4, wr=100.0 * sum(1 for r in rets if r > 0) / len(rets),
                worst=min(rets) * 1e4, dd=dd * 1e4)

def fmt(s): return f"n={s['n']:<5d} net={s['net']:>+9.0f}bp wr={s['wr']:>4.0f}% worst={s['worst']:>+7.0f}bp maxDD={s['dd']:>7.0f}bp"

def run_symbol(name, ts, o, h, l, c, W, thr, be, cost, allow_short, bar_sec, tp_detail=False, quiet=False):
    wu = windows(ts, c, W, thr, True, bar_sec)
    wd = windows(ts, c, W, thr, False, bar_sec) if allow_short else []
    if not quiet:
        print(f"\n=== {name}  bars={len(c)}  span={(ts[-1]-ts[0])/86400.0:.0f}d  thr={thr*100:.2f}%  "
              f"rt_cost={cost:.1f}bp  fill={FILL}  winP={len(wu)} winN={len(wd)} ===")
    per_trail = {}
    for gb in TRAIL5:
        p = trail_scheme(o, h, l, c, wu, True, gb, be, cost)
        n = trail_scheme(o, h, l, c, wd, False, gb, be, cost) if wd else []
        per_trail[gb] = (p, n)
        if not quiet:
            sa = stats(p + n)
            print(f"  TRAIL r{int(gb):<4d} ALL {fmt(sa)}   | Pos {stats(p)['net']:>+8.0f}bp  Neg {stats(n)['net']:>+8.0f}bp")
    tot_tr = [r for gb in TRAIL5 for pn in per_trail[gb] for r in pn]
    tp_p = tp_scheme(o, h, l, c, wu, True, TP7, be, cost)
    tp_n = tp_scheme(o, h, l, c, wd, False, TP7, be, cost) if wd else {t: [] for t in TP7}
    if not quiet:
        print(f"  TRAIL5 BOOK      {fmt(stats(tot_tr))}")
        for t in TP7:
            sa = stats(tp_p[t] + tp_n[t])
            print(f"  TP    r{int(t):<4d} ALL {fmt(sa)}   | Pos {stats(tp_p[t])['net']:>+8.0f}bp  Neg {stats(tp_n[t])['net']:>+8.0f}bp")
    tot_tp = [r for t in TP7 for r in tp_p[t] + tp_n[t]]
    tr = rider_scheme(ts, o, h, l, c, W, thr, cost, allow_short, bar_sec)
    rr = [r for _, r in tr]
    if not quiet:
        print(f"  TP7   BOOK       {fmt(stats(tot_tp))}")
        print(f"  RIDER            {fmt(stats(rr))}")
    if tp_detail and not quiet:
        import datetime
        def ysplit(rows):
            out = {}
            for xts, r in rows: out.setdefault(datetime.datetime.fromtimestamp(xts, datetime.timezone.utc).year, []).append(r)
            return {y: round(sum(v) * 1e4) for y, v in sorted(out.items())}
        print(f"  RIDER by year (net bp): {ysplit(tr)}")
    return stats(tot_tr), stats(tot_tp), stats(rr)

def main():
    agg = {"TRAIL5": [], "TP7": [], "RIDER": []}
    for name, f, W, thr, be, cost, sh, bs in SYMS:
        ts, o, h, l, c = load_csv(os.path.join(WARM, f))
        a, b, r = run_symbol(name, ts, o, h, l, c, W, thr, be, cost, sh, bs)
        agg["TRAIL5"].append((name, a)); agg["TP7"].append((name, b)); agg["RIDER"].append((name, r))
    if CDATA:
        for sym, W, thr, be, cost, sh, bs in CRYPTO:
            files = sorted(glob.glob(os.path.join(CDATA, sym + "-1h-*.csv")))
            rows = {}
            for fp in files:
                t2, o2, h2, l2, c2 = load_csv(fp)
                for k in range(len(t2)): rows[t2[k]] = (o2[k], h2[k], l2[k], c2[k])
            ts = sorted(rows)
            o = [rows[t][0] for t in ts]; h = [rows[t][1] for t in ts]
            l = [rows[t][2] for t in ts]; c = [rows[t][3] for t in ts]
            a, b, r = run_symbol(sym, ts, o, h, l, c, W, thr, be, cost, sh, bs, tp_detail=True)
            agg["TRAIL5"].append((sym, a)); agg["TP7"].append((sym, b)); agg["RIDER"].append((sym, r))
    print(f"\n=== BOOK SUMMARY (net bp real, per symbol; fill={FILL} rider_be={int(RIDER_BE)}) ===")
    print(f"{'sym':8} {'TRAIL5':>10} {'TP7':>10} {'RIDER':>10}   {'TRAIL5 dd':>10} {'TP7 dd':>10} {'RIDER dd':>10}")
    for i, (name, a) in enumerate(agg["TRAIL5"]):
        b = agg["TP7"][i][1]; r = agg["RIDER"][i][1]
        print(f"{name:8} {a['net']:>+10.0f} {b['net']:>+10.0f} {r['net']:>+10.0f}   {a['dd']:>10.0f} {b['dd']:>10.0f} {r['dd']:>10.0f}")

if __name__ == "__main__":
    main()
