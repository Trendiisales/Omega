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

# ── multi-format Tick loading (OMEGA.md §1 data inventory) ──────────────────
# Formats sniffed PER FILE (never mixed in one pass, per the OMEGA.md rule):
#   * bar CSV      : ts,o,h,l,c[,..]      (epoch s/ms/us)
#   * duka ticks   : ts,ask,bid[,..]      (epoch ms; ASK FIRST — mid=(ask+bid)/2)
#   * histdata     : "YYYYMMDD HHMMSSmmm,bid,ask" (',' or ';'; naive local ts — consistent offset)
# Every file folds into hourly OHLC buckets; groups of files merge as BARS.
TICK_SPECS = {   # glob groups per symbol, tried against TICKDIR (recursive); all matches merged
    "XAUUSD": ["XAUUSD*h1*.csv", "duka_ticks/XAUUSD_*combined*.csv", "xau_6mo_corrected.csv"],
    "XAGUSD": ["XAGUSD*h1*.csv", "XAGUSD*H1*.csv"],
    "USOIL":  ["USOIL*h1*.csv", "BCOUSD/**/*.csv", "BCOUSD*.csv"],   # BCOUSD = Brent proxy (flagged)
    "EURUSD": ["EURUSD*h1*.csv", "EURUSD/**/*.csv"],
    "GBPUSD": ["GBPUSD*h1*.csv", "GBPUSD/**/*.csv"],
    "USDJPY": ["USDJPY*h1*.csv", "USDJPY/**/*.csv"],
    "AUDUSD": ["AUDUSD*h1*.csv", "AUDUSD/**/*.csv"],
    "NZDUSD": ["NZDUSD*h1*.csv", "NZDUSD/**/*.csv"],
    "US500":  ["US500*h1*.csv", "xregime/US500_2426.csv", "duka_multiyear/usa500idxusd*.csv"],
    "NAS100": ["NAS100*h1*.csv", "xregime/NAS100_2426.csv", "NAS2022_bear_h1.csv", "duka_multiyear/usatechidxusd*.csv"],
    "DJ30":   ["DJ30*h1*.csv", "book_combined/DJ30_clean.csv"],
    "GER40":  ["GER40*h1*.csv", "book_combined/GER40*.csv", "histdata_book/GER40/**/*.csv"],
}

def _fold(buckets, ts, o, h, l, c):
    b = (ts // 3600) * 3600
    e = buckets.get(b)
    if e is None: buckets[b] = [o, h, l, c]
    else:
        if h > e[1]: e[1] = h
        if l < e[2]: e[2] = l
        e[3] = c

def _parse_hist_ts(tok):
    # "YYYYMMDD HHMMSSmmm" -> epoch (naive; consistent offset is fine for jump windows)
    import calendar, time as _t
    d, t = tok.split(" ")
    st = _t.struct_time((int(d[:4]), int(d[4:6]), int(d[6:8]),
                         int(t[:2]), int(t[2:4]), int(t[4:6]), 0, 0, 0))
    return calendar.timegm(st)

def parse_file_into(path, buckets):
    n = 0
    with open(path, errors="replace") as f:
        mode = None
        for line in f:
            line = line.strip()
            if not line: continue
            sep = ";" if (";" in line and "," not in line) else ","
            parts = line.split(sep)
            if mode is None:   # sniff on the first data-looking line
                p0 = parts[0]
                if len(p0) >= 17 and p0[:8].isdigit() and " " in p0: mode = "hist"
                elif p0[:1].isdigit() or p0[:1] == "-":
                    if len(parts) >= 5:
                        # bar (ts,o,h,l,c) vs 5-col duka (ts,ask,bid,vol,vol — DJ30 per OMEGA.md):
                        # a true bar has h >= max(o,c) and l <= min(o,c); duka has bid < ask
                        # and tiny volume cols. Shape-test, don't assume.
                        try:
                            v1, v2, v3, v4 = (float(x) for x in parts[1:5])
                            mode = "bar" if (v2 >= max(v1, v4) - 1e-12 and v3 <= min(v1, v4) + 1e-12) else "duka"
                        except ValueError: continue
                    elif len(parts) >= 3: mode = "duka"
                    else: continue
                else: continue
            try:
                if mode == "hist":
                    ts = _parse_hist_ts(parts[0])
                    bid = float(parts[1]); ask = float(parts[2])
                    m = (bid + ask) / 2.0
                    if m > 0: _fold(buckets, ts, m, m, m, m); n += 1
                elif mode == "duka":
                    t = float(parts[0])
                    while t > 4e10: t /= 1000.0
                    ask = float(parts[1]); bid = float(parts[2])   # ASK FIRST (OMEGA.md trap)
                    m = (bid + ask) / 2.0
                    if m > 0: _fold(buckets, int(t), m, m, m, m); n += 1
                else:      # bar csv ts,o,h,l,c
                    t = float(parts[0])
                    while t > 4e10: t /= 1000.0
                    o_, h_, l_, c_ = (float(parts[1]), float(parts[2]), float(parts[3]), float(parts[4]))
                    if c_ > 0: _fold(buckets, int(t), o_, h_, l_, c_); n += 1
            except (ValueError, IndexError):
                continue
    return n

def buckets_to_series(buckets):
    ks = sorted(buckets)
    return (ks, [buckets[k][0] for k in ks], [buckets[k][1] for k in ks],
            [buckets[k][2] for k in ks], [buckets[k][3] for k in ks])

def find_data(sym):
    if TICKDIR:
        files = []
        for pat in TICK_SPECS.get(sym, [f"{sym}*h1*.csv"]):
            files += glob.glob(os.path.join(TICKDIR, pat))
            files += glob.glob(os.path.join(TICKDIR, "**", pat), recursive=True)
        files = sorted(set(f for f in files if os.path.isfile(f)))
        if files:
            buckets = {}
            for fp in files:
                n = parse_file_into(fp, buckets)
                print(f"    [data] {sym}: {os.path.basename(fp)} -> {n} rows")
            if buckets:
                return buckets_to_series(buckets), f"{len(files)} file(s) merged"
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
    print(f"  {'rank':4} {'W':>2} {'thr%':>5} {'exit':>5} | {'IS net':>8} {'n':>4} {'dd':>7} | {'OOS net':>8} {'n':>4} {'dd':>7} {'wr%':>4}  plateau")
    for k, (W, thr, ex, si, so) in enumerate(cand[:TOPN]):
        # plateau PER ROW: thr-neighbors (same W/exit) must hold most of the edge
        neigh = [r for r in rows if r[0] == W and r[2] == ex and r[1] != thr]
        avg_is = sum(r[3]["net"] for r in neigh) / len(neigh) if neigh else 0.0
        ptag = "SPIKE" if (neigh and avg_is <= 0 < si["net"]) else "ok"
        print(f"  #{k+1:<3} {W:>2} {thr*100:>5.2f} {ex:>5} | {si['net']:>+8.0f} {si['n']:>4} {si['dd']:>7.0f} "
              f"| {so['net']:>+8.0f} {so['n']:>4} {so['dd']:>7.0f} {so['wr']:>4.0f}  {avg_is:+.0f}bp/{ptag}")

def main():
    only = set(s for s in os.environ.get("SYMS_ONLY", "").split(",") if s)   # e.g. SYMS_ONLY=XAUUSD,GER40
    for sym, thrs, ws, cost, short in SYMS:
        if only and sym not in only: continue
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
