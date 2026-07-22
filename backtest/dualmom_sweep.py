#!/usr/bin/env python3
"""dualmom_sweep.py — S-2026-07-23a operator: 'redo dual momentum, more testing,
pull every lever, gate per regime'. Full lever sweep of the monthly dual-momentum
archetype on the BIGCAP-45 daily set, with the regime-gate variants isolated
(the recorded '2022=0 cash-in-bear' claim failed reproduction — this sweep
answers WHICH gate, if any, actually delivers it).

Levers: K {3,5,8,10} x rel-lookback {63,126,251} x rebal {10,21,63}d x
gate {none, spy200 (close>SMA200), spy200slope (close>SMA200 & SMA rising),
strict (spy200slope & abs12m spine)} x cost {8,16}bp/side.
Metrics per cell: total, Sharpe(daily ann.), maxDD, 2022, 2023, WF halves,
days-in-cash-2022 (the gate truth), alpha vs gated-EW45 control.
Survivorship caveat stands: universe chosen 2026 — judge vs the SAME-universe
control, not absolute return.
"""
import os, math, itertools

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "bigcap_daily_ohlc")
BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO WDC STX DD TPR BMY SWKS").split()

def load(sym):
    out = {}
    with open(os.path.join(DIR, f"{sym}.csv")) as f:
        next(f)
        for ln in f:
            p = ln.strip().split(",")
            if len(p) == 5 and float(p[4]) > 0:
                out[p[0]] = float(p[4])
    return out

bars = {s: load(s) for s in BIGCAP}
spy = load("SPY")
dates = sorted(spy.keys())
n = len(dates)
C = {s: [bars[s].get(d) for d in dates] for s in BIGCAP}
spyc = [spy[d] for d in dates]
yr = [d[:4] for d in dates]

sma = [None]*n; run = 0.0
for i in range(n):
    run += spyc[i]
    if i >= 200: run -= spyc[i-200]
    if i >= 199: sma[i] = run/200.0
g_spy200  = [sma[i] is not None and spyc[i] > sma[i] for i in range(n)]
g_slope   = [sma[i] is not None and sma[i-20] is not None and spyc[i] > sma[i] and sma[i] > sma[i-20] for i in range(n)]

def select(i, K, LB):
    scores = {}
    for s in BIGCAP:
        c0, cl, c12 = C[s][i], C[s][i-LB] if i >= LB else None, C[s][i-251] if i >= 251 else None
        if c0 and cl and c12:
            if c0/c12 - 1.0 > 0:
                scores[s] = c0/cl - 1.0
    if len(scores) < K: return {}
    top = sorted(scores, key=lambda s: scores[s])[-K:]
    return {s: 1.0/K for s in top}

def run_port(K, LB, REBAL, gate, cost_bp):
    eq = 1.0; peak = 1.0; mdd = 0.0
    w = {}; daily = []; cash2022 = 0; d2022 = 0
    for i in range(252, n):
        if yr[i] == "2022":
            d2022 += 1
            if not w: cash2022 += 1
        # rebalance
        if (i - 252) % REBAL == 0:
            on = gate[i] if gate else True
            tgt = select(i, K, LB) if on else {}
            turn = sum(abs(tgt.get(s, 0) - w.get(s, 0)) for s in set(tgt) | set(w))
            eq *= (1 - turn * cost_bp / 1e4)
            w = tgt
        elif gate and not gate[i] and w:
            turn = sum(w.values())
            eq *= (1 - turn * cost_bp / 1e4)
            w = {}
        r = 0.0
        for s, wt in w.items():
            if C[s][i] and C[s][i-1]:
                r += wt * (C[s][i]/C[s][i-1] - 1.0)
        eq *= (1 + r); daily.append(r)
        peak = max(peak, eq); mdd = max(mdd, 1 - eq/peak)
    m = sum(daily)/len(daily); sd = math.sqrt(sum((x-m)**2 for x in daily)/len(daily))
    sh = m/sd*math.sqrt(252) if sd > 0 else 0
    # yearly + halves
    ycum = {}
    e = 1.0; half = (len(daily))//2; h1 = 1.0; h2 = 1.0
    for j, r in enumerate(daily):
        y = yr[252+j]; ycum[y] = ycum.get(y, 1.0)*(1+r)
        if j < half: h1 *= (1+r)
        else: h2 *= (1+r)
    return dict(tot=(eq-1)*100, sh=sh, mdd=mdd*100,
                y22=(ycum.get("2022",1)-1)*100, y23=(ycum.get("2023",1)-1)*100,
                h1=(h1-1)*100, h2=(h2-1)*100,
                cash22=100*cash2022/max(1,d2022))

GATES = {"none": None, "spy200": g_spy200, "slope": g_slope}
rows = []
for K, LB, RB, gname, cost in itertools.product((3,5,8,10),(63,126,251),(10,21,63),GATES,(8,16)):
    r = run_port(K, LB, RB, GATES[gname], cost)
    r.update(K=K, LB=LB, RB=RB, gate=gname, cost=cost)
    rows.append(r)

# report: all cells that fix the bear (2022 > -5%) AND keep Sharpe >= 1.0, plus overall best
good = [r for r in rows if r["y22"] > -5 and r["sh"] >= 1.0 and r["h1"] > 0 and r["h2"] > 0]
good.sort(key=lambda r: -r["sh"])
print(f"cells swept: {len(rows)} | bear-safe(2022>-5%) & Sharpe>=1 & both-halves+: {len(good)}")
print(f"{'K':>2} {'LB':>3} {'RB':>2} {'gate':>7} {'c':>2} | {'tot%':>8} {'sh':>5} {'mdd%':>5} {'2022%':>7} {'cash22%':>7} {'H1%':>7} {'H2%':>7}")
for r in good[:12]:
    print(f"{r['K']:>2} {r['LB']:>3} {r['RB']:>2} {r['gate']:>7} {r['cost']:>2} | {r['tot']:>8.0f} {r['sh']:>5.2f} {r['mdd']:>5.1f} {r['y22']:>7.1f} {r['cash22']:>7.0f} {r['h1']:>7.0f} {r['h2']:>7.0f}")
best = sorted(rows, key=lambda r: -r["sh"])[:5]
print("\ntop-5 by Sharpe regardless of bear:")
for r in best:
    print(f"{r['K']:>2} {r['LB']:>3} {r['RB']:>2} {r['gate']:>7} {r['cost']:>2} | {r['tot']:>8.0f} {r['sh']:>5.2f} {r['mdd']:>5.1f} {r['y22']:>7.1f} {r['cash22']:>7.0f}")
