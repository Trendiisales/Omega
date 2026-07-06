#!/usr/bin/env python3
"""daily_edge_scan.py — IS/OOS scan of the canonical daily strategy families on the
same real data + real costs as rider_sweep_wf.py (operator ask 2026-07-07: "what other
suggestion do you have that may be a winner or edge we can use").

Strategies (daily bars, UTC-resampled from 1h; decide at close, enter next open):
  EMAx20/50, Kelt20x2, TSMom50, TSMom90, Donch40 (hold), IBS .15/.85, RSI2 (Connors <10 / >90)
Crypto: long-only (spot venue). CFDs: long+short. Costs debited per round trip.
Selection discipline: IS = first 60%; OOS = last 40% reported untouched.
Run on the Mac with full history:  TICKDIR=~/Tick CDATA=~/Crypto/backtest/data python3 backtest/daily_edge_scan.py
"""
import csv, glob, os

HERE = os.path.dirname(os.path.abspath(__file__))
WARM = os.path.join(HERE, "..", "phase1", "signal_discovery")
CDATA = os.environ.get("CDATA", "")
TICKDIR = os.environ.get("TICKDIR", "")

SYMS = [  # sym, rt_cost_bp, allow_short   (repo warmup H1 -> D1; SHORT samples, Tick rerun owed)
    ("XAUUSD", 6.0, True), ("XAGUSD", 6.0, True), ("USOIL", 8.0, True),
    ("EURUSD", 4.0, True), ("GBPUSD", 3.5, True), ("USDJPY", 3.0, True),
    ("US500", 4.0, True), ("NAS100", 3.0, True), ("DJ30", 2.0, True), ("GER40", 2.0, True),
]
CRYPTO = [("BTCUSDT", 23.0, False), ("ETHUSDT", 23.0, False), ("SOLUSDT", 25.0, False)]

def load_csv(path, rows=None):
    rows = {} if rows is None else rows
    with open(path) as f:
        for row in csv.reader(f):
            if not row or not row[0][:1].isdigit(): continue
            t = float(row[0])
            while t > 4e10: t /= 1000.0
            rows[int(t)] = (float(row[1]), float(row[2]), float(row[3]), float(row[4]))
    return rows

def to_daily(rows):
    days = {}
    for t in sorted(rows):
        d = t // 86400
        o, h, l, c = rows[t]
        if d not in days: days[d] = [t, o, h, l, c]
        else:
            e = days[d]
            e[2] = max(e[2], h); e[3] = min(e[3], l); e[4] = c
    ks = sorted(days)
    return ([days[d][0] for d in ks], [days[d][1] for d in ks], [days[d][2] for d in ks],
            [days[d][3] for d in ks], [days[d][4] for d in ks])

def ema(c, i, p):
    st = max(0, i - 4 * p); a = 2.0 / (p + 1); e = c[st]
    for j in range(st + 1, i + 1): e = a * c[j] + (1 - a) * e
    return e

def sig_emax(o, h, l, c, i):   return 1 if ema(c, i, 20) > ema(c, i, 50) else -1
def sig_kelt(o, h, l, c, i):
    N = 20
    if i < N + 1: return 0
    e = ema(c, i, N); atr = sum(max(h[j]-l[j], abs(h[j]-c[j-1]), abs(l[j]-c[j-1])) for j in range(i-N+1, i+1)) / N
    if atr <= 0: return 0
    if c[i] > e + 2*atr: return 1
    if c[i] < e - 2*atr: return -1
    return 0
def sig_tsmom(L):
    def f(o, h, l, c, i):
        if i < L: return 0
        r = c[i] - c[i-L]; return 1 if r > 0 else (-1 if r < 0 else 0)
    return f
def sig_donch(o, h, l, c, i):
    N = 40
    if i < N: return 0
    hh = max(h[i-N:i+1]); ll = min(l[i-N:i+1])
    if c[i] >= hh: return 1
    if c[i] <= ll: return -1
    for k in range(i-1, max(N-1, i-N), -1):
        h2 = max(h[k-N:k+1]); l2 = min(l[k-N:k+1])
        if c[k] >= h2: return 1
        if c[k] <= l2: return -1
    return 0
def sig_ibs(o, h, l, c, i):
    rng = h[i] - l[i]
    if rng <= 0: return 0
    v = (c[i] - l[i]) / rng
    if v < 0.15: return 1
    if v > 0.85: return -1
    return 0
def sig_rsi2(o, h, l, c, i):
    N = 2
    if i < N + 1: return 0
    g = s = 0.0
    for j in range(i-N+1, i+1):
        d = c[j] - c[j-1]
        if d > 0: g += d
        else: s -= d
    rs = (g/N) / (s/N) if s > 0 else 999.0
    rsi = 100 - 100/(1+rs)
    if rsi < 10: return 1
    if rsi > 90: return -1
    return 0

STRATS = [("EMAx20/50", sig_emax), ("Kelt20x2", sig_kelt), ("TSMom50", sig_tsmom(50)),
          ("TSMom90", sig_tsmom(90)), ("Donch40", sig_donch), ("IBS.15/.85", sig_ibs),
          ("RSI2", sig_rsi2)]

def run(ts, o, h, l, c, sig, cost_bp, allow_short):
    trades = []; pos = 0; entry = 0.0; ei = 0
    for i in range(1, len(c)):
        want = sig(o, h, l, c, i - 1)
        if want < 0 and not allow_short: want = 0
        if want != pos:
            if pos != 0:
                trades.append((ts[i], pos * (o[i] - entry) / entry - cost_bp / 1e4))
            if want != 0: entry = o[i]; ei = i
            pos = want
    if pos != 0: trades.append((ts[-1], pos * (c[-1] - entry) / entry - cost_bp / 1e4))
    return trades

def stats(rets):
    if not rets: return dict(n=0, net=0.0, dd=0.0)
    cum = pk = dd = 0.0
    for r in rets:
        cum += r; pk = max(pk, cum); dd = max(dd, pk - cum)
    return dict(n=len(rets), net=sum(rets)*1e4, dd=dd*1e4)

def scan(name, rows, cost, short):
    ts, o, h, l, c = to_daily(rows)
    if len(c) < 120:
        print(f"{name:9} insufficient daily bars ({len(c)})"); return
    cut = ts[0] + int((ts[-1] - ts[0]) * 0.60)
    line = [f"{name:9} ({len(c):>4}d)"]
    for sname, s in STRATS:
        tr = run(ts, o, h, l, c, s, cost, short)
        si = stats([r for t, r in tr if t < cut]); so = stats([r for t, r in tr if t >= cut])
        mark = "*" if (si['net'] > 0 and so['net'] > 0) else " "
        line.append(f"{sname}:{si['net']:+.0f}/{so['net']:+.0f}{mark}")
    print("  ".join(line))

def main():
    print("net bp IS/OOS per strategy ('*' = positive BOTH windows). CFD rows = SHORT samples (Tick rerun owed).")
    for sym, cost, short in SYMS:
        p = None
        if TICKDIR:
            cands = glob.glob(os.path.join(TICKDIR, f"{sym}*h1*.csv")) + glob.glob(os.path.join(TICKDIR, f"{sym}*H1*.csv"))
            if cands: p = max(set(cands), key=os.path.getsize)
        if not p: p = os.path.join(WARM, f"warmup_{sym}_H1.csv")
        try: scan(sym, load_csv(p), cost, short)
        except FileNotFoundError: print(f"{sym:9} no data")
    if CDATA:
        for sym, cost, short in CRYPTO:
            files = sorted(glob.glob(os.path.join(CDATA, sym + "-1h-*.csv")))   # Binance Vision monthly
            if not files:
                files = sorted(glob.glob(os.path.join(CDATA, sym + "*1h*.csv")))  # live layout BTCUSDT_1h.csv
            rows = {}
            for fp in files: load_csv(fp, rows)
            if rows: scan(sym, rows, cost, short)
            else: print(f"{sym:9} no data under CDATA")

if __name__ == "__main__":
    main()
