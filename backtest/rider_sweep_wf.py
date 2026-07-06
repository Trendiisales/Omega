#!/usr/bin/env python3
"""rider_sweep_wf.py — WALK-FORWARD entry x exit sweep for the JumpRider family
(operator ask 2026-07-07: "look into both entry and exit settings to see if there
is something better" — with the no-overfitting discipline).

METHOD: settings are ranked ONLY on the IN-SAMPLE window (first 60% of each
symbol's history); the OUT-OF-SAMPLE window (last 40%) is never touched by the
selection and is reported as the verdict. A config is a candidate only if
IS trades >= 30 and IS net > 0; the OOS column decides.

ENTRY GRID : jump window W (bars) x threshold thr.
EXIT GRID  : FLIP  = pure symmetric-jump flip (the UpJump original)
             BE05 / BE10 = flip + BE-ratchet armed at 0.5x / 1.0x thr (resting stop at entry)
             TR33 / TR50 = flip + chandelier trail: once peak ret >= 0.5x thr, resting stop
                           at entry*(1 + dir*peak*(1-gb)) with BE floor (gb = 33% / 50% giveback)
             every exit also carries a resting catastrophe stop at 2x thr adverse.
FILLS      : entries/flip at observed closes (market on signal); stops are RESTING
             orders — intrabar H/L touch fills at the level, gap-open fills at the open.
COSTS      : per-symbol real RT (engine_init table / Binance taker+slip) debited per trade.

DATA: repo warmup H1 (12 CFD symbols; SHORT samples — rerun on the full Tick history!),
Binance Vision 1h 2017-2026 (BTC/ETH; SOL 2020-). On the Mac run with the full history:
    TICKDIR=~/Tick CDATA=~/Crypto/backtest/data python3 backtest/rider_sweep_wf.py
TICKDIR globs <SYM>*h1*.csv / <SYM>*H1*.csv per symbol and takes the longest match.
"""
import csv, glob, os

HERE = os.path.dirname(os.path.abspath(__file__))
WARM = os.path.join(HERE, "..", "phase1", "signal_discovery")
CDATA = os.environ.get("CDATA", "")
TICKDIR = os.environ.get("TICKDIR", "")
TOPN = int(os.environ.get("TOPN", "3"))

# sym, warmup file, thr grid, W grid, rt_cost_bp, allow_short
FX_THR  = [0.002, 0.003, 0.005, 0.0075, 0.010]
MET_THR = [0.005, 0.0075, 0.010, 0.015, 0.020]
WS      = [2, 4, 8]
SYMS = [
    ("XAUUSD", MET_THR, WS,  6.0, True), ("XAGUSD", MET_THR, WS, 6.0, True),
    ("USOIL",  MET_THR, WS,  8.0, True),
    ("EURUSD", FX_THR,  WS,  4.0, True), ("GBPUSD", FX_THR, WS, 3.5, True),
    ("USDJPY", FX_THR,  WS,  3.0, True), ("AUDUSD", FX_THR, WS, 6.0, True),
    ("NZDUSD", FX_THR,  WS,  7.0, True),
    ("US500",  FX_THR,  WS,  4.0, True), ("NAS100", FX_THR, WS, 3.0, True),
    ("DJ30",   FX_THR,  WS,  2.0, True), ("GER40",  FX_THR, WS, 2.0, True),
]
CRYPTO = [
    ("BTCUSDT", [0.015, 0.02, 0.03, 0.04, 0.05], [12, 24, 48], 23.0, False),
    ("ETHUSDT", [0.015, 0.02, 0.03, 0.04, 0.05], [12, 24, 48], 23.0, False),
    ("SOLUSDT", [0.02, 0.03, 0.04, 0.05, 0.07],  [12, 24, 48], 25.0, False),
]
EXITS = ["FLIP", "BE05", "BE10", "TR33", "TR50"]

def load_csv(path, rows=None):
    rows = {} if rows is None else rows
    with open(path) as f:
        for row in csv.reader(f):
            if not row or not row[0][:1].isdigit(): continue
            t = float(row[0])
            while t > 4e10: t /= 1000.0
            rows[int(t)] = (float(row[1]), float(row[2]), float(row[3]), float(row[4]))
    return rows

def to_series(rows):
    ts = sorted(rows)
    return (ts, [rows[t][0] for t in ts], [rows[t][1] for t in ts],
            [rows[t][2] for t in ts], [rows[t][3] for t in ts])

def find_data(sym):
    if TICKDIR:
        cands = []
        for pat in (f"{sym}*h1*.csv", f"{sym}*H1*.csv", f"{sym.lower()}*h1*.csv"):
            cands += glob.glob(os.path.join(TICKDIR, pat)) + glob.glob(os.path.join(TICKDIR, "**", pat), recursive=True)
        if cands:
            best = max(set(cands), key=os.path.getsize)
            return to_series(load_csv(best)), best
    p = os.path.join(WARM, f"warmup_{sym}_H1.csv")
    return to_series(load_csv(p)), p

def rider(ts, o, h, l, c, W, thr, cost_bp, allow_short, exit_mode, bar_sec=3600):
    trades = []
    pos = 0; entry = peak = 0.0; block_up = block_dn = False
    be_arm = {"BE05": 0.5 * thr, "BE10": 1.0 * thr}.get(exit_mode)
    tr_gb = {"TR33": 0.33, "TR50": 0.50}.get(exit_mode)
    for i in range(W, len(c)):
        cur = c[i]
        j = cur / c[i - W] - 1.0
        contig = (ts[i] - ts[i - W]) <= W * bar_sec * 2
        up = j >= thr; dn = j <= -thr
        if not up: block_up = False
        if not dn: block_dn = False
        if pos != 0:
            closed = False
            # resting stops: catastrophe always; BE/trail per mode
            stops = [entry * (1 - pos * 2 * thr)]
            if be_arm is not None and peak >= be_arm: stops.append(entry)
            if tr_gb is not None and peak >= 0.5 * thr:
                stops.append(entry * (1 + pos * max(0.0, peak * (1 - tr_gb))))
            lvl = max(stops) if pos > 0 else min(stops)
            touched = (l[i] <= lvl) if pos > 0 else (h[i] >= lvl)
            if touched:
                fill = min(o[i], lvl) if pos > 0 else max(o[i], lvl)
                trades.append((ts[i], pos * (fill - entry) / entry - cost_bp / 1e4))
                pos = 0; closed = True
                if up: block_up = True
                if dn: block_dn = True
            if not closed:
                ret = pos * (cur - entry) / entry
                peak = max(peak, ret)
                flip = dn if pos > 0 else up
                if flip:
                    trades.append((ts[i], ret - cost_bp / 1e4)); pos = 0
                    nd = -1 if dn else 1
                    if contig and (nd > 0 and not block_up or nd < 0 and allow_short and not block_dn):
                        pos = nd; entry = cur; peak = 0.0
        elif contig:
            if up and not block_up: pos = 1; entry = cur; peak = 0.0
            elif dn and allow_short and not block_dn: pos = -1; entry = cur; peak = 0.0
    if pos != 0: trades.append((ts[-1], pos * (c[-1] - entry) / entry - cost_bp / 1e4))
    return trades

def stats(rets):
    if not rets: return dict(n=0, net=0.0, dd=0.0, worst=0.0, wr=0.0)
    cum = peak = dd = 0.0
    for r in rets:
        cum += r; peak = max(peak, cum); dd = max(dd, peak - cum)
    return dict(n=len(rets), net=sum(rets) * 1e4, dd=dd * 1e4, worst=min(rets) * 1e4,
                wr=100.0 * sum(1 for r in rets if r > 0) / len(rets))

def sweep(sym, thrs, ws, cost, short):
    (ts, o, h, l, c), src = find_data(sym)
    if len(c) < 500:
        print(f"\n=== {sym}: insufficient data ({len(c)} bars) ==="); return
    cut = ts[0] + int((ts[-1] - ts[0]) * 0.60)
    span_d = (ts[-1] - ts[0]) / 86400.0
    print(f"\n=== {sym}  bars={len(c)} span={span_d:.0f}d  IS=first 60% / OOS=last 40%  "
          f"cost={cost}bp  src={os.path.basename(src)} ===")
    rows = []
    for W in ws:
        for thr in thrs:
            for ex in EXITS:
                tr = rider(ts, o, h, l, c, W, thr, cost, short, ex)
                is_r  = [r for t, r in tr if t <  cut]
                oos_r = [r for t, r in tr if t >= cut]
                si, so = stats(is_r), stats(oos_r)
                rows.append((W, thr, ex, si, so))
    # candidates: IS-positive with enough trades, ranked by IS net (OOS untouched by selection)
    cand = [r for r in rows if r[3]["n"] >= 30 and r[3]["net"] > 0]
    cand.sort(key=lambda r: -r[3]["net"])
    if not cand:
        best_any = max(rows, key=lambda r: r[3]["net"])
        print(f"  NO IS-positive config with >=30 trades. Best IS anyway: "
              f"W={best_any[0]} thr={best_any[1]*100:.2f}% {best_any[2]} "
              f"IS {best_any[3]['net']:+.0f}bp (n={best_any[3]['n']}) OOS {best_any[4]['net']:+.0f}bp")
        return
    print(f"  {'rank':4} {'W':>2} {'thr%':>5} {'exit':>5} | {'IS net':>8} {'n':>4} {'dd':>7} | {'OOS net':>8} {'n':>4} {'dd':>7} {'wr%':>4}")
    for k, (W, thr, ex, si, so) in enumerate(cand[:TOPN]):
        print(f"  #{k+1:<3} {W:>2} {thr*100:>5.2f} {ex:>5} | {si['net']:>+8.0f} {si['n']:>4} {si['dd']:>7.0f} "
              f"| {so['net']:>+8.0f} {so['n']:>4} {so['dd']:>7.0f} {so['wr']:>4.0f}")
    # plateau check on the #1: neighbors in thr must hold most of the edge
    W0, t0, e0, _, _ = cand[0]
    neigh = [r for r in rows if r[0] == W0 and r[2] == e0 and r[1] != t0]
    if neigh:
        avg_is = sum(r[3]["net"] for r in neigh) / len(neigh)
        print(f"  plateau: #1 thr-neighbors (same W/exit) avg IS {avg_is:+.0f}bp "
              f"{'(SPIKE — treat #1 as overfit)' if avg_is <= 0 < cand[0][3]['net'] else '(plateau ok)'}")

def main():
    for sym, thrs, ws, cost, short in SYMS:
        try: sweep(sym, thrs, ws, cost, short)
        except FileNotFoundError: print(f"\n=== {sym}: no data file ===")
    if CDATA:
        for sym, thrs, ws, cost, short in CRYPTO:
            files = sorted(glob.glob(os.path.join(CDATA, sym + "-1h-*.csv")))
            if not files:
                files = sorted(glob.glob(os.path.join(CDATA, sym + "*1h*.csv")))
            if not files: print(f"\n=== {sym}: no data ==="); continue
            rows = {}
            for fp in files: load_csv(fp, rows)
            ts, o, h, l, c = to_series(rows)
            cut = ts[0] + int((ts[-1] - ts[0]) * 0.60)
            print(f"\n=== {sym}  bars={len(c)} span={(ts[-1]-ts[0])/86400.0:.0f}d  IS/OOS 60/40  cost={cost}bp ===")
            allrows = []
            for W in ws:
                for thr in thrs:
                    for ex in EXITS:
                        tr = rider(ts, o, h, l, c, W, thr, cost, short, ex)
                        si = stats([r for t, r in tr if t < cut]); so = stats([r for t, r in tr if t >= cut])
                        allrows.append((W, thr, ex, si, so))
            cand = [r for r in allrows if r[3]["n"] >= 30 and r[3]["net"] > 0]
            cand.sort(key=lambda r: -r[3]["net"])
            if not cand:
                b = max(allrows, key=lambda r: r[3]["net"])
                print(f"  NO IS-positive config >=30 trades. Best IS anyway: W={b[0]} thr={b[1]*100:.1f}% {b[2]} "
                      f"IS {b[3]['net']:+.0f}bp OOS {b[4]['net']:+.0f}bp")
                continue
            print(f"  {'rank':4} {'W':>2} {'thr%':>5} {'exit':>5} | {'IS net':>8} {'n':>4} {'dd':>7} | {'OOS net':>8} {'n':>4} {'dd':>7} {'wr%':>4}")
            for k, (W, thr, ex, si, so) in enumerate(cand[:TOPN]):
                print(f"  #{k+1:<3} {W:>2} {thr*100:>5.2f} {ex:>5} | {si['net']:>+8.0f} {si['n']:>4} {si['dd']:>7.0f} "
                      f"| {so['net']:>+8.0f} {so['n']:>4} {so['dd']:>7.0f} {so['wr']:>4.0f}")
            W0, t0, e0, _, _ = cand[0]
            neigh = [r for r in allrows if r[0] == W0 and r[2] == e0 and r[1] != t0]
            if neigh:
                avg_is = sum(r[3]["net"] for r in neigh) / len(neigh)
                print(f"  plateau: #1 thr-neighbors avg IS {avg_is:+.0f}bp "
                      f"{'(SPIKE — treat #1 as overfit)' if avg_is <= 0 < cand[0][3]['net'] else '(plateau ok)'}")

if __name__ == "__main__":
    main()
