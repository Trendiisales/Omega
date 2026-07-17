#!/usr/bin/env python3
"""
bigcap_xs_momentum_bt.py — cross-sectional momentum ROTATION on the 45-name BIGCAP book.
(S-2026-07-17j, operator-greenlit; handoff item 3 of S-2026-07-17i.)

Rank all 45 names against each other by trailing return (Jegadeesh-Titman, the
CrossSectionalIndexEngine / cross_sectional_relval.py archetype: score =
c[t-skip]/c[t-lb-skip]-1), long the top-K, inverse-vol or equal weight, flat when
SPY < 200DMA (daily gate). Long-only (rotation book, no shorts).

HONESTY / CONTROLS
- Point-in-time inclusion: a name is rankable only once it has lb+skip+1 bars
  (late IPOs enter when history exists — CRWV/ALAB/ARM/NBIS...).
- SURVIVORSHIP CAVEAT (must ride every readout): the 45-name universe was chosen
  in 2026 AFTER these names won — absolute returns are inflated by construction.
  The decision metric is therefore RANKING ALPHA: top-K vs the SAME 45-name
  equal-weight basket under the SAME gate (universe bias mostly cancels), plus
  2022 behaviour. Beating gated-EW = the ranking adds something; beating QQQ
  alone proves nothing.
- Costs: 8bp/side on TURNOVER (sum |w_new - w_old| per rebalance, gate flips =
  full round trip), 2x stress = 16bp/side.
- Returns close-to-close; gate/weights decided on close[t], applied to day t+1.

Gate to care (standing): net+, both WF halves+, 2022 explicit, 2x-cost robust,
AND top-K > gated-EW control on net + Sharpe (else it's just gated beta).
"""
import os, glob, math, sys

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "bigcap_daily_ohlc")
BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO WDC STX DD TPR BMY SWKS").split()
assert len(BIGCAP) == 45

def load(sym):
    out = {}
    with open(os.path.join(DIR, f"{sym}.csv")) as f:
        next(f)
        for ln in f:
            p = ln.strip().split(",")
            if len(p) == 5 and float(p[4]) > 0:
                out[p[0]] = float(p[4])
    return out

def main():
    closes = {s: load(s) for s in BIGCAP}
    spy = load("SPY")
    dates = sorted(spy.keys())                     # SPY = calendar spine
    di = {d: i for i, d in enumerate(dates)}
    # per-name aligned close series (None where absent), forward-index into dates
    px = {s: [closes[s].get(d) for d in dates] for s in BIGCAP}
    spyc = [spy[d] for d in dates]
    n = len(dates)

    # SPY 200DMA gate, computed on the spine
    sma = [None] * n
    run = 0.0
    for i in range(n):
        run += spyc[i]
        if i >= 200: run -= spyc[i - 200]
        if i >= 199: sma[i] = run / 200.0
    gate_on = [sma[i] is not None and spyc[i] > sma[i] for i in range(n)]

    # daily returns per name (None if either side missing)
    ret = {s: [None] * n for s in BIGCAP}
    for s in BIGCAP:
        for i in range(1, n):
            if px[s][i] and px[s][i - 1]:
                ret[s][i] = px[s][i] / px[s][i - 1] - 1.0

    def vol20(s, i):
        rs = [ret[s][k] for k in range(max(1, i - 19), i + 1) if ret[s][k] is not None]
        if len(rs) < 10: return None
        m = sum(rs) / len(rs)
        return math.sqrt(sum((r - m) ** 2 for r in rs) / len(rs)) or None

    def run_cell(lb, K, rebal, weight, cost_side_bp, use_gate=True, ew_control=False):
        cost = cost_side_bp / 10000.0
        w = {}                      # current weights
        eq = 1.0; peak = 1.0; mdd = 0.0
        daily = []                  # (date, port_ret_after_cost)
        next_reb = 0
        for i in range(200, n - 1):
            # rebalance decision on close[i]
            if i >= next_reb:
                next_reb = i + rebal
                tgt = {}
                if not use_gate or gate_on[i]:
                    scores = {}
                    for s in BIGCAP:
                        j0 = i - lb
                        if j0 >= 0 and px[s][i] and px[s][j0]:
                            scores[s] = px[s][i] / px[s][j0] - 1.0
                    if ew_control:
                        elig = list(scores)     # same eligibility as the ranked book
                        if elig:
                            tgt = {s: 1.0 / len(elig) for s in elig}
                    elif len(scores) >= K:
                        top = sorted(scores, key=lambda s: scores[s])[-K:]
                        if weight == "iv":
                            iv = {}
                            for s in top:
                                v = vol20(s, i)
                                iv[s] = 1.0 / v if v else 0.0
                            tot = sum(iv.values())
                            tgt = {s: iv[s] / tot for s in top} if tot > 0 else {s: 1.0 / K for s in top}
                        else:
                            tgt = {s: 1.0 / K for s in top}
                turnover = sum(abs(tgt.get(s, 0.0) - w.get(s, 0.0)) for s in set(tgt) | set(w))
                eq *= (1.0 - turnover * cost)
                w = tgt
            elif use_gate and not gate_on[i] and w:
                # daily gate exit between rebalances
                turnover = sum(w.values())
                eq *= (1.0 - turnover * cost)
                w = {}
            # earn day i+1 with weights w
            pr = 0.0
            for s, ws in w.items():
                r = ret[s][i + 1]
                if r is not None: pr += ws * r
            eq *= (1.0 + pr)
            if eq > peak: peak = eq
            dd = 1.0 - eq / peak
            if dd > mdd: mdd = dd
            daily.append((dates[i + 1], pr))
        return eq, mdd, daily

    def stats(daily, eq, mdd):
        yrs = {}
        for d, r in daily: yrs.setdefault(d[:4], []).append(r)
        y22 = yrs.get("2022", [])
        r22 = 1.0
        for r in y22: r22 *= (1.0 + r)
        half = len(daily) // 2
        h1 = 1.0; h2 = 1.0
        for _, r in daily[:half]: h1 *= (1.0 + r)
        for _, r in daily[half:]: h2 *= (1.0 + r)
        rs = [r for _, r in daily]
        m = sum(rs) / len(rs)
        sd = math.sqrt(sum((r - m) ** 2 for r in rs) / len(rs))
        sharpe = (m / sd * math.sqrt(252)) if sd > 0 else 0.0
        years = len(daily) / 252.0
        cagr = eq ** (1.0 / years) - 1.0 if eq > 0 else -1.0
        return dict(tot=(eq - 1) * 100, cagr=cagr * 100, sh=sharpe, mdd=mdd * 100,
                    y22=(r22 - 1) * 100, h1=(h1 - 1) * 100, h2=(h2 - 1) * 100,
                    both=(h1 > 1 and h2 > 1))

    def line(tag, st):
        print(f"{tag:26s} tot={st['tot']:8.0f}% cagr={st['cagr']:5.1f}% sh={st['sh']:5.2f} "
              f"mdd={st['mdd']:5.1f}% | 2022={st['y22']:6.1f}% | H1={st['h1']:7.0f}% H2={st['h2']:6.0f}% "
              f"both+={'YES' if st['both'] else 'NO '}")

    print(f"spine {dates[0]}..{dates[-1]} n={n}  universe=45 point-in-time  cost=8bp/side (2x=16)")
    for cbp in (8.0, 16.0):
        print(f"\n================ cost {cbp:.0f}bp/side ================")
        # controls first
        eq_, mdd_, dl_ = run_cell(120, 0, 21, "eq", cbp, use_gate=True, ew_control=True)
        line("CTRL gated-EW45", stats(dl_, eq_, mdd_))
        eq_, mdd_, dl_ = run_cell(120, 0, 21, "eq", 0.0, use_gate=False, ew_control=True)
        line("CTRL EW45 b&h(0c)", stats(dl_, eq_, mdd_))
        qqq = load("QQQ"); qs = [qqq.get(d) for d in dates]
        dl_ = [(dates[i + 1], (qs[i + 1] / qs[i] - 1.0) if qs[i] and qs[i + 1] else 0.0) for i in range(200, n - 1)]
        e = 1.0; pk = 1.0; md = 0.0
        for _, r in dl_:
            e *= 1 + r; pk = max(pk, e); md = max(md, 1 - e / pk)
        line("CTRL QQQ b&h(0c)", stats(dl_, e, md))
        print()
        for lb in (30, 60, 120, 250):
            for K in (3, 5, 8):
                for rebal in (10, 14, 21):
                    for weight in ("eq", "iv"):
                        eq_, mdd_, dl_ = run_cell(lb, K, rebal, weight, cbp)
                        line(f"lb={lb} K={K} rb={rebal} {weight}", stats(dl_, eq_, mdd_))

if __name__ == "__main__":
    main()
