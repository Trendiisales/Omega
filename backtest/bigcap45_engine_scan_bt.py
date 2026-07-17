#!/usr/bin/env python3
"""
bigcap45_engine_scan_bt.py — full scan of remaining engine candidates on the 45-name
BIGCAP daily-OHLC corpus (S-2026-07-17k, operator item-2: "full check of any other
engine we can run on these 45 names").

Candidate space after prior kills (XS rotation DEAD 17j; overnight-gap/VWAP/shorts
rejected prior session; PEAD blocked, no earnings dataset; pairs excluded = needs a
short leg, shorts rejected for this book):

  PER-NAME trade archetypes (pooled per-trade %, prior-sweep convention):
    A. RSI2 dip extension    — close>SMA200 & RSI2<10 -> exit close>SMA5 or 10d.
                               Live StockDip covers 11 names; report NEW-34 separately.
    B. Donchian 20/10 turtle — 20d-close-high entry, 10d-close-low exit.
                               Live StockTurtle covers 11 names; report NEW-34 separately.
    F. Gap-continuation o->c — open >= prevC*(1+g%) & close>SMA200 -> buy open, sell
                               close same day. NOTE: needs intraday (open) execution;
                               current stock book is a daily-close poller — wiring
                               caveat rides any PASS.

  PORTFOLIO archetypes (equity-compounded, vs gated-EW45 control, ex-2024 explicit —
  the XS-rotation kill showed 2024 mania concentration masquerading as alpha):
    C. 52wk-high proximity   — hold names within X% of 52wk high, SPY-200DMA gate.
    D. Dual momentum         — abs (12m>0) AND rel (top-K by 6m), monthly.
    E. Low-vol tilt          — top-K lowest vol60, monthly, SPY gate.

HONESTY: point-in-time inclusion (a name enters when its own history suffices);
survivorship caveat rides every readout (universe chosen 2026 after these names won —
decision metric = alpha vs the SAME-universe gated-EW45 control, not absolute return);
costs 8bp/side on turnover (portfolio) / 16bp RT (per-name trades) + 2x stress;
both WF halves; 2022 explicit; per-year decomposition incl ex-2024 for portfolio cells.
"""
import os, math

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "bigcap_daily_ohlc")
BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO WDC STX DD TPR BMY SWKS").split()
assert len(BIGCAP) == 45
LIVE_DIP    = set("MU NVDA AVGO DELL CRDO STX INTC AMD AAPL TPR MSFT".split())      # StockDip live 11
LIVE_TURTLE = set("NVDA AVGO STX DD AMD AAPL TPR BMY SWKS MSFT QCOM".split())       # StockTurtle live 11

def load(sym):
    out = {}
    with open(os.path.join(DIR, f"{sym}.csv")) as f:
        next(f)
        for ln in f:
            p = ln.strip().split(",")
            if len(p) == 5 and float(p[4]) > 0:
                out[p[0]] = (float(p[1]), float(p[2]), float(p[3]), float(p[4]))
    return out

bars = {s: load(s) for s in BIGCAP}
spyb = load("SPY")
dates = sorted(spyb.keys())
n = len(dates)
O = {s: [bars[s].get(d, (None,)*4)[0] for d in dates] for s in BIGCAP}
C = {s: [bars[s].get(d, (None,)*4)[3] for d in dates] for s in BIGCAP}
spyc = [spyb[d][3] for d in dates]

# SPY 200DMA gate on the spine
sma_spy = [None]*n; run = 0.0
for i in range(n):
    run += spyc[i]
    if i >= 200: run -= spyc[i-200]
    if i >= 199: sma_spy[i] = run/200.0
gate_on = [sma_spy[i] is not None and spyc[i] > sma_spy[i] for i in range(n)]

# per-name daily returns (close-close)
RET = {s: [None]*n for s in BIGCAP}
for s in BIGCAP:
    for i in range(1, n):
        if C[s][i] and C[s][i-1]:
            RET[s][i] = C[s][i]/C[s][i-1] - 1.0

def sma_series(cl, w):
    out = [None]*n; run = 0.0; cnt = 0
    for i in range(n):
        if cl[i] is None:            # gap in history: restart window
            run = 0.0; cnt = 0; out[i] = None; continue
        run += cl[i]; cnt += 1
        if cnt > w:
            # subtract the value w steps back (contiguous since restart)
            run -= cl[i-w]; cnt = w
        out[i] = run/w if cnt == w else None
    return out

def rsi2_series(cl):
    out = [None]*n; ag = None; al = None
    prev = None
    for i in range(n):
        if cl[i] is None: prev = None; ag = al = None; out[i] = None; continue
        if prev is None: prev = cl[i]; continue
        ch = cl[i]-prev; g = max(ch,0.0); l = max(-ch,0.0)
        if ag is None: ag, al = g, l
        else: ag = (ag*1+g)/2.0; al = (al*1+l)/2.0     # Wilder a=1/2 for RSI2
        out[i] = 100.0 if al == 0 else 100.0 - 100.0/(1.0 + ag/al)
        prev = cl[i]
    return out

# ---------- per-name trade archetypes ----------
def trade_stats(trades, tag):
    """trades: list of (exit_date, pct_net). Pooled prior-sweep convention."""
    if not trades:
        print(f"{tag:34s} n=0"); return None
    trades.sort()
    net = sum(p for _, p in trades)
    wins = sum(p for _, p in trades if p > 0); loss = -sum(p for _, p in trades if p < 0)
    pf = wins/loss if loss > 0 else 999.0
    half = len(trades)//2
    h1 = sum(p for _, p in trades[:half]); h2 = sum(p for _, p in trades[half:])
    y22 = sum(p for d, p in trades if d[:4] == "2022")
    y24 = sum(p for d, p in trades if d[:4] == "2024")
    worst = min(p for _, p in trades)
    st = dict(n=len(trades), net=net, pf=pf, h1=h1, h2=h2, y22=y22, ex24=net-y24, worst=worst,
              both=(h1 > 0 and h2 > 0))
    print(f"{tag:34s} n={st['n']:5d} net={st['net']:+8.1f}% PF={st['pf']:5.2f} "
          f"H1={st['h1']:+7.1f} H2={st['h2']:+7.1f} 2022={st['y22']:+7.1f} ex24={st['ex24']:+8.1f} "
          f"worst={st['worst']:+6.1f} both+={'YES' if st['both'] else 'NO'}")
    return st

def dip_trades(s, cost_rt):
    cl = C[s]; s200 = sma_series(cl, 200); s5 = sma_series(cl, 5); r2 = rsi2_series(cl)
    out = []; ei = None
    for i in range(n-1):
        if cl[i] is None: continue
        if ei is None:
            if s200[i] and r2[i] is not None and cl[i] > s200[i] and r2[i] < 10.0:
                ei = i
        else:
            days = i - ei
            if (s5[i] and cl[i] > s5[i]) or days >= 10:
                out.append((dates[i], (cl[i]/cl[ei]-1.0)*100.0 - cost_rt))
                ei = None
    return out

def turtle_trades(s, cost_rt):
    cl = C[s]; out = []; ei = None
    for i in range(20, n-1):
        if cl[i] is None: continue
        win20 = [c for c in cl[max(0,i-20):i] if c is not None]
        win10 = [c for c in cl[max(0,i-10):i] if c is not None]
        if ei is None:
            if len(win20) >= 15 and cl[i] > max(win20):
                ei = i
        else:
            if len(win10) >= 7 and cl[i] < min(win10):
                out.append((dates[i], (cl[i]/cl[ei]-1.0)*100.0 - cost_rt))
                ei = None
    return out

def gap_trades(s, gpct, cost_rt):
    cl = C[s]; op = O[s]; s200 = sma_series(cl, 200)
    out = []
    for i in range(1, n):
        if None in (cl[i], op[i], cl[i-1]) or not s200[i-1]: continue
        if cl[i-1] > s200[i-1] and op[i] >= cl[i-1]*(1.0+gpct/100.0):
            out.append((dates[i], (cl[i]/op[i]-1.0)*100.0 - cost_rt))
    return out

# ---------- portfolio archetypes ----------
def port_stats(daily, eq, mdd, tag):
    yrs = {}
    for d, r in daily: yrs.setdefault(d[:4], []).append(r)
    def comp(rs):
        e = 1.0
        for r in rs: e *= 1.0+r
        return (e-1.0)*100.0
    y22 = comp(yrs.get("2022", []))
    half = len(daily)//2
    h1 = comp([r for _, r in daily[:half]]); h2 = comp([r for _, r in daily[half:]])
    rs = [r for _, r in daily]
    m = sum(rs)/len(rs); sd = math.sqrt(sum((r-m)**2 for r in rs)/len(rs))
    sh = m/sd*math.sqrt(252) if sd > 0 else 0.0
    x = [r for d, r in daily if d[:4] != "2024"]
    mx = sum(x)/len(x); sdx = math.sqrt(sum((r-mx)**2 for r in x)/len(x))
    shx = mx/sdx*math.sqrt(252) if sdx > 0 else 0.0
    ex24 = comp(x)
    print(f"{tag:34s} tot={(eq-1)*100:+9.0f}% sh={sh:5.2f} shEx24={shx:5.2f} mdd={mdd*100:5.1f}% "
          f"2022={y22:+6.1f}% ex24tot={ex24:+8.0f}% H1={h1:+7.0f} H2={h2:+6.0f} "
          f"both+={'YES' if h1 > 0 and h2 > 0 else 'NO'}")
    return dict(sh=sh, shx=shx)

def run_port(select_fn, rebal, cost_side_bp, use_gate=True):
    """select_fn(i) -> target weight dict decided on close[i]."""
    cost = cost_side_bp/10000.0
    w = {}; eq = 1.0; peak = 1.0; mdd = 0.0; daily = []; next_reb = 0
    for i in range(260, n-1):
        if i >= next_reb:
            next_reb = i + rebal
            tgt = select_fn(i) if (not use_gate or gate_on[i]) else {}
            turn = sum(abs(tgt.get(s,0.0)-w.get(s,0.0)) for s in set(tgt)|set(w))
            eq *= (1.0 - turn*cost); w = tgt
        elif use_gate and not gate_on[i] and w:
            eq *= (1.0 - sum(w.values())*cost); w = {}
        pr = 0.0
        for s, ws in w.items():
            r = RET[s][i+1]
            if r is not None: pr += ws*r
        eq *= (1.0+pr)
        peak = max(peak, eq); mdd = max(mdd, 1.0-eq/peak)
        daily.append((dates[i+1], pr))
    return eq, mdd, daily

def ew_eligible(i):
    elig = [s for s in BIGCAP if C[s][i] and i >= 260 and C[s][i-259]]
    return {s: 1.0/len(elig) for s in elig} if elig else {}

def hi52_select(i, xpct):
    tgt = {}
    for s in BIGCAP:
        if not C[s][i]: continue
        win = [c for c in C[s][max(0,i-251):i+1] if c is not None]
        if len(win) < 200: continue
        if C[s][i] >= max(win)*(1.0-xpct/100.0): tgt[s] = 1.0
    k = sum(tgt.values())
    return {s: v/k for s, v in tgt.items()} if k else {}

def dualmom_select(i, K):
    scores = {}
    for s in BIGCAP:
        if i >= 251 and C[s][i] and C[s][i-251] and C[s][i-125]:
            r12 = C[s][i]/C[s][i-251]-1.0
            r6 = C[s][i]/C[s][i-125]-1.0
            if r12 > 0: scores[s] = r6
    if len(scores) < K: return {}
    top = sorted(scores, key=lambda s: scores[s])[-K:]
    return {s: 1.0/K for s in top}

def lowvol_select(i, K):
    vols = {}
    for s in BIGCAP:
        rs = [RET[s][k] for k in range(max(1,i-59), i+1) if RET[s][k] is not None]
        if len(rs) >= 40:
            m = sum(rs)/len(rs)
            v = math.sqrt(sum((r-m)**2 for r in rs)/len(rs))
            if v > 0: vols[s] = v
    if len(vols) < K: return {}
    low = sorted(vols, key=lambda s: vols[s])[:K]
    return {s: 1.0/K for s in low}

def main():
    print(f"spine {dates[0]}..{dates[-1]} n={n}  45 names point-in-time  "
          f"survivorship caveat: universe chosen 2026 — judge vs gated-EW45 control only")

    for mult, cost_rt, cost_side in ((1, 16.0/100.0, 8.0), (2, 32.0/100.0, 16.0)):
        print(f"\n########## cost x{mult} (per-name RT={cost_rt*100:.0f}bp, portfolio {cost_side:.0f}bp/side) ##########")

        print("\n-- A. RSI2 dip (close>SMA200 & RSI2<10 -> SMA5-bounce/10d) --")
        allt = []; newt = []; livet = []
        pn = {}
        for s in BIGCAP:
            t = dip_trades(s, cost_rt)
            pn[s] = t; allt += t
            (livet if s in LIVE_DIP else newt).extend(t)
        trade_stats(livet, "A dip LIVE-11 (sanity)")
        trade_stats(newt,  "A dip NEW-34 pooled")
        trade_stats(allt,  "A dip ALL-45 pooled")
        if mult == 1:
            print("   per-name NEW-34 (n>=8 only):")
            for s in BIGCAP:
                if s in LIVE_DIP or len(pn[s]) < 8: continue
                trade_stats(pn[s], f"     {s}")

        print("\n-- B. Donchian 20/10 turtle --")
        allt = []; newt = []; livet = []; pn = {}
        for s in BIGCAP:
            t = turtle_trades(s, cost_rt)
            pn[s] = t; allt += t
            (livet.extend(t) if s in LIVE_TURTLE else newt.extend(t))
        trade_stats(livet, "B turtle LIVE-11 (sanity)")
        trade_stats(newt,  "B turtle NEW-34 pooled")
        trade_stats(allt,  "B turtle ALL-45 pooled")
        if mult == 1:
            print("   per-name NEW-34 (n>=8 only):")
            for s in BIGCAP:
                if s in LIVE_TURTLE or len(pn[s]) < 8: continue
                trade_stats(pn[s], f"     {s}")

        print("\n-- F. Gap-continuation o->c (prevC>SMA200, open gap>=g%) --")
        for g in (1.0, 2.0, 3.0):
            allt = []
            for s in BIGCAP: allt += gap_trades(s, g, cost_rt)
            trade_stats(allt, f"F gap>={g:.0f}% ALL-45 pooled")

        print("\n-- portfolio controls --")
        eq, mdd, dl = run_port(ew_eligible, 21, cost_side, use_gate=True)
        port_stats(dl, eq, mdd, "CTRL gated-EW45")
        eq, mdd, dl = run_port(ew_eligible, 21, 0.0, use_gate=False)
        port_stats(dl, eq, mdd, "CTRL EW45 b&h(0c)")

        print("\n-- C. 52wk-high proximity (weekly rebal, SPY gate) --")
        for x in (5.0, 10.0, 15.0, 25.0):
            eq, mdd, dl = run_port(lambda i, x=x: hi52_select(i, x), 5, cost_side)
            port_stats(dl, eq, mdd, f"C hi52 within {x:.0f}%")

        print("\n-- D. Dual momentum abs12m>0 + rel top-K by 6m (monthly, SPY gate) --")
        for K in (5, 10):
            eq, mdd, dl = run_port(lambda i, K=K: dualmom_select(i, K), 21, cost_side)
            port_stats(dl, eq, mdd, f"D dualmom K={K}")

        print("\n-- E. Low-vol tilt top-K lowest vol60 (monthly, SPY gate) --")
        for K in (10, 15):
            eq, mdd, dl = run_port(lambda i, K=K: lowvol_select(i, K), 21, cost_side)
            port_stats(dl, eq, mdd, f"E lowvol K={K}")


def peryear_mode():
    """--peryear: per-year compounded returns, hi52-5% + dualmom5 vs gated-EW45."""
    def yr_table(tag, daily):
        yrs = {}
        for d, r in daily: yrs.setdefault(d[:4], []).append(r)
        row = [tag]
        for y in sorted(yrs):
            e = 1.0
            for r in yrs[y]: e *= 1.0+r
            row.append(f"{y}={((e-1)*100):+7.1f}%")
        print("  ".join(row))
    for cbp in (8.0,):
        eq, mdd, dl = run_port(ew_eligible, 21, cbp, use_gate=True)
        yr_table("CTRL gated-EW45 ", dl)
        eq, mdd, dl = run_port(lambda i: hi52_select(i, 5.0), 5, cbp)
        yr_table("C hi52-5%       ", dl)
        eq, mdd, dl = run_port(lambda i: dualmom_select(i, 5), 21, cbp)
        yr_table("D dualmom K=5   ", dl)


def passlist_mode():
    """--passlist: per-name all-6 house gate (n>=30, net>0, PF>=1.3, H1>0, H2>0,
    ex-best>0) on the NEW-34 names, 1x cost, for turtle + dip extensions."""
    def all6(trades):
        if len(trades) < 30: return None
        trades = sorted(trades)
        net = sum(x for _, x in trades)
        wins = sum(x for _, x in trades if x > 0); loss = -sum(x for _, x in trades if x < 0)
        pf = wins/loss if loss > 0 else 999.0
        half = len(trades)//2
        h1 = sum(x for _, x in trades[:half]); h2 = sum(x for _, x in trades[half:])
        exb = net - max(x for _, x in trades)
        y22 = sum(x for d, x in trades if d[:4] == "2022")
        ok = net > 0 and pf >= 1.3 and h1 > 0 and h2 > 0 and exb > 0
        return dict(n=len(trades), net=net, pf=pf, h1=h1, h2=h2, exb=exb, y22=y22, ok=ok)
    for label, fn, live in (("TURTLE", turtle_trades, LIVE_TURTLE), ("DIP", dip_trades, LIVE_DIP)):
        print(f"\n== {label} extension all-6 gate (NEW names, 16bp RT) ==")
        for s in BIGCAP:
            if s in live: continue
            st = all6(fn(s, 16.0/100.0))
            if st is None:
                print(f"  {s:6s} n<30 — insufficient history/trades (late IPO or thin)")
                continue
            print(f"  {s:6s} n={st['n']:4d} net={st['net']:+8.1f}% PF={st['pf']:5.2f} "
                  f"H1={st['h1']:+7.1f} H2={st['h2']:+7.1f} exb={st['exb']:+7.1f} "
                  f"2022={st['y22']:+6.1f} {'PASS' if st['ok'] else '-'}")

import sys as _sys
if __name__ == "__main__" and "--peryear" in _sys.argv:
    peryear_mode()
elif __name__ == "__main__" and "--passlist" in _sys.argv:
    passlist_mode()
elif __name__ == "__main__":
    main()
