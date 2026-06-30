#!/usr/bin/env python3
"""
Day-mover: verify PF is real (concentration audit) + sweep the LOCK-IN % (S-2026-06-30).

Operator questions:
  1. "is that PF 4.21 real?"  -> per-year + per-name concentration audit. A PF carried
     by 2 AI names in 2024-25 is NOT a forward edge.
  2. "what % should we lock in like gold?" -> sweep GATE_PCT (arm-after-capture) x
     REV_GB (giveback-from-peak) x N_STALL, report bear-entry + all-regime.
  3. "bear-gate shouldn't matter -- if it triggers in bear it MET our requirements" ->
     compare WIDE-bear vs GOLD-clip-bear vs exclude-bear. Test if clip alone fixes
     bear (his thesis) so we DON'T have to throw away qualified bear signals.

Faithful: equal-weight per-trade %, no pyramiding, real costs, close-only daily.
"""
from __future__ import annotations
import numpy as np, pandas as pd, itertools
from pathlib import Path
from collections import defaultdict

CSV = Path.home() / "Omega" / "data" / "rdagent" / "sp500_long_close.csv"
GLITCH = {"POM", "CPWR", "MI"}
COST_RT = 0.002
ATR_WIN = 14
GATE = 0.05; CONTIN_K = 20; MAX_HOLD = 60

LARGECAP = ("AAPL MSFT NVDA AMZN GOOGL GOOG META TSLA AVGO ORCL AMD MU INTC QCOM TXN ADI "
            "LRCX AMAT KLAC MRVL ARM SMCI PLTR CRWD PANW SNOW NOW CRM ADBE SHOP NFLX UBER "
            "ABNB COIN MSTR DELL ANET CDNS SNPS WDC ON MCHP NXPI HPQ").split()


def load():
    df = pd.read_csv(CSV, index_col=0, parse_dates=True).sort_index()
    cov = df.notna().mean()
    keep = [c for c in df.columns[cov >= 0.90] if c not in GLITCH]
    df = df[keep]
    return df[[c for c in dict.fromkeys(LARGECAP) if c in df.columns]]


def regime(df):
    norm = df / df.iloc[0]; basket = norm.mean(axis=1)
    sma = basket.rolling(200, min_periods=200).mean()
    return (basket > sma).reset_index(drop=True)


def simulate(name, closes, dates, bull, mode, gate_pct, rev_gb, n_stall):
    n = len(closes)
    ret = np.empty(n); ret[0] = 0.0; ret[1:] = closes[1:] / closes[:-1] - 1.0
    out = []; i = ATR_WIN + 1
    while i < n - 1:
        if not (ret[i] >= GATE and np.isfinite(closes[i])):
            i += 1; continue
        if closes[i] < np.nanmax(closes[max(0, i - CONTIN_K):i + 1]):
            i += 1; continue
        entry_px = closes[i]; be = bool(bull.iloc[i]) if i < len(bull) else False
        peak = entry_px; since_high = 0; exit_i = None
        for k in range(i + 1, min(i + 1 + MAX_HOLD, n)):
            c = closes[k]
            if not np.isfinite(c): continue
            if c > peak: peak = c; since_high = 0
            else: since_high += 1
            if mode == "gold":
                fav = peak / entry_px - 1.0
                if fav >= gate_pct and (since_high >= n_stall or c <= peak * (1 - rev_gb)):
                    exit_i = k; break
        if exit_i is None: exit_i = min(i + MAX_HOLD, n - 1)
        ex = closes[exit_i]
        if not np.isfinite(ex):
            j = exit_i
            while j > i and not np.isfinite(closes[j]): j -= 1
            ex = closes[j]; exit_i = j
        if not np.isfinite(ex) or ex <= 0: i = exit_i + 1; continue
        out.append((name, dates[i], dates[exit_i], ex / entry_px - 1 - COST_RT, be))
        i = exit_i + 1
    return out


def run(df, bull, mode, gate_pct=0.015, rev_gb=0.05, n_stall=2):
    dates = df.index.to_numpy(); allt = []
    for col in df.columns:
        allt += simulate(col, df[col].to_numpy(float), dates, bull, mode, gate_pct, rev_gb, n_stall)
    return allt


def pf_of(trades):
    if not trades: return (0, 0, 0, 0)
    pnl = np.array([t[3] for t in trades])
    w = pnl[pnl > 0].sum(); l = abs(pnl[pnl < 0].sum())
    pf = w / l if l else float("inf")
    order = np.argsort(pd.to_datetime([t[2] for t in trades]).values)
    eq = np.cumsum(pnl[order]); dd = (eq - np.maximum.accumulate(eq)).min()
    return (len(trades), pf, pnl.sum() * 100, dd * 100)


def main():
    df = load(); bull = regime(df)
    print(f"names={df.shape[1]} rows={len(df)} bull={100*bull.mean():.0f}%\n")

    wide = run(df, bull, "wide")
    bullw = [t for t in wide if t[4]]

    # --- 1. concentration audit on the PF-4.21 bull-entry book ---
    print("=== IS PF 4.21 REAL? bull-entry WIDE book, by YEAR + top names ===")
    by_year = defaultdict(list)
    for t in bullw:
        by_year[pd.Timestamp(t[1]).year].append(t)
    print(f"{'year':6s} {'n':>4s} {'PF':>6s} {'tot%':>8s}")
    for y in sorted(by_year):
        n, pf, tot, _ = pf_of(by_year[y]); print(f"{y:6d} {n:4d} {pf:6.2f} {tot:8.1f}")
    by_name = defaultdict(float); cnt = defaultdict(int)
    for t in bullw: by_name[t[0]] += t[3] * 100; cnt[t[0]] += 1
    tot_all = sum(by_name.values())
    print(f"\ntop-6 names by tot% contribution (of {tot_all:.0f}% total):")
    for nm, v in sorted(by_name.items(), key=lambda x: -x[1])[:6]:
        print(f"  {nm:6s} n={cnt[nm]:2d} tot={v:7.1f}%  ({100*v/tot_all:4.1f}% of book)")

    # --- 2. lock-in sweep (gold logic) on bear-entries + all ---
    print("\n=== LOCK-IN SWEEP: GATE_PCT(arm) x REV_GB(giveback) x N_STALL ===")
    print(f"{'gate%':>6s} {'revgb':>6s} {'stall':>5s} | {'BEAR n/PF/tot/DD':>26s} | {'ALL n/PF/tot/DD':>26s}")
    print("-" * 80)
    gps = [0.01, 0.02, 0.03, 0.05, 0.08]; rgs = [0.03, 0.05, 0.10]; sts = [2, 3]
    best = None
    for gp, rg, st in itertools.product(gps, rgs, sts):
        g = run(df, bull, "gold", gp, rg, st)
        bear = [t for t in g if not t[4]]
        nb, pfb, totb, ddb = pf_of(bear)
        na, pfa, tota, dda = pf_of(g)
        print(f"{gp:6.2f} {rg:6.2f} {st:5d} | {nb:4d}/{pfb:4.2f}/{totb:6.0f}/{ddb:5.0f} "
              f"        | {na:4d}/{pfa:4.2f}/{tota:6.0f}/{dda:5.0f}")
        # rank bear book by PF then low DD (bear is where clip must earn its keep)
        score = (pfb, ddb)
        if best is None or score > best[0]: best = (score, (gp, rg, st), (nb, pfb, totb, ddb))
    print(f"\nbest bear lock-in: gate={best[1][0]} revgb={best[1][1]} stall={best[1][2]} "
          f"-> bear n={best[2][0]} PF={best[2][1]:.2f} tot={best[2][2]:.0f}% DD={best[2][3]:.0f}%")

    # --- 3. operator thesis: does bear-gate matter, or clip-alone fixes bear? ---
    print("\n=== DOES BEAR-GATE MATTER? (operator: bear entries ARE qualified) ===")
    gp, rg, st = best[1]
    g = run(df, bull, "gold", gp, rg, st)
    wbear = [t for t in wide if not t[4]]; gbear = [t for t in g if not t[4]]
    for lab, tr in (("WIDE bear (no clip)", wbear), ("GOLD-clip bear (best lock)", gbear)):
        n, pf, tot, dd = pf_of(tr); print(f"  {lab:30s} n={n:3d} PF={pf:4.2f} tot={tot:6.0f}% DD={dd:5.0f}%")
    switch = bullw + gbear
    n, pf, tot, dd = pf_of(switch)
    print(f"  {'SWITCH wide-bull+clip-bear':30s} n={n:3d} PF={pf:4.2f} tot={tot:6.0f}% DD={dd:5.0f}%")
    n, pf, tot, dd = pf_of(bullw)
    print(f"  {'EXCLUDE-bear (wide bull only)':30s} n={n:3d} PF={pf:4.2f} tot={tot:6.0f}% DD={dd:5.0f}%")


if __name__ == "__main__":
    main()
