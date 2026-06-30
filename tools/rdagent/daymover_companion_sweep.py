#!/usr/bin/env python3
"""
Day-mover momentum + COMPANION EXIT SWEEP (S-2026-06-30).

Goal (operator): the :7799 basket should trade BIG DAY MOVERS (>=gate% in a day),
ride them, and run an independent COMPANION that trails aggressively and exits on
stagnation/turn. This harness finds the OPTIMUM companion exit by brute-force sweep
over giveback% / stall-days / ATR-mult / max-hold, vs a WIDE-trail baseline (the
'real engine' proxy), so the give-back captured is quantified -- not assumed.

DATA: sp500_long_close.csv (565 names, DAILY closes, 2019-06 -> 2026-06, 7yr multi-
regime: 2020 crash, 2022 bear, 2023-26 bull). CLOSE-ONLY -> this is DAILY-CLOSE grade,
NOT the intraday 15m grade BigCapMomo was validated at. Honest provenance.

Entry  : day return >= gate%  [+ optional continuation: close is a new K-day high]
Exit   : first of { giveback% from peak | stall N days no-new-high | ATR-trail | max-hold }
Cost   : round-trip % deducted per trade (commission+slippage proxy; base + stress).
Metrics: PF, win-rate, avg/median trade %, n, worst, equity-curve maxDD, H1/H2 (both-halves).
"""
from __future__ import annotations
import numpy as np, pandas as pd, itertools, sys
from pathlib import Path

CSV = Path.home() / "Omega" / "data" / "rdagent" / "sp500_long_close.csv"
GLITCH = {"POM", "CPWR", "MI"}          # dead tickers w/ x1000/split artifacts
COST_RT_BASE = 0.002                    # 20bp round-trip (IBKR commission + 5bp/side slip proxy)
COST_RT_STRESS = 0.004                  # 40bp stress
ATR_WIN = 14


def load() -> pd.DataFrame:
    df = pd.read_csv(CSV, index_col=0, parse_dates=True).sort_index()
    cov = df.notna().mean()
    keep = [c for c in df.columns[cov >= 0.90] if c not in GLITCH]
    return df[keep]


def simulate(closes: np.ndarray, dates, gate: float, contin_k: int,
             giveback: float | None, stall: int | None, atr_mult: float | None,
             max_hold: int, cost_rt: float):
    """One name's series -> list of (entry_date, exit_date, pnl_pct)."""
    n = len(closes)
    ret = np.empty(n); ret[0] = 0.0
    ret[1:] = closes[1:] / closes[:-1] - 1.0
    # daily ATR proxy = rolling mean of |dClose| over ATR_WIN
    dabs = np.abs(np.diff(closes, prepend=closes[0]))
    atr = pd.Series(dabs).rolling(ATR_WIN).mean().to_numpy()
    trades = []
    i = ATR_WIN + 1
    while i < n - 1:
        if not (ret[i] >= gate and np.isfinite(closes[i])):
            i += 1; continue
        if contin_k > 0 and closes[i] < np.nanmax(closes[max(0, i - contin_k):i + 1]):
            i += 1; continue
        entry_px = closes[i]; entry_atr = atr[i] if np.isfinite(atr[i]) else entry_px * 0.02
        peak = entry_px; since_high = 0; exit_i = None
        for k in range(i + 1, min(i + 1 + max_hold, n)):
            c = closes[k]
            if not np.isfinite(c):
                continue
            if c > peak:
                peak = c; since_high = 0
            else:
                since_high += 1
            hit = False
            if giveback is not None and c <= peak * (1 - giveback): hit = True
            if stall is not None and since_high >= stall: hit = True
            if atr_mult is not None and c <= peak - atr_mult * entry_atr: hit = True
            if hit:
                exit_i = k; break
        if exit_i is None:
            exit_i = min(i + max_hold, n - 1)
        ex_px = closes[exit_i]
        if not np.isfinite(ex_px):                       # walk back to last finite close
            j = exit_i
            while j > i and not np.isfinite(closes[j]):
                j -= 1
            ex_px = closes[j]; exit_i = j
        if not np.isfinite(ex_px) or ex_px <= 0:
            i = exit_i + 1; continue
        pnl = ex_px / entry_px - 1.0 - cost_rt
        trades.append((dates[i], dates[exit_i], pnl))
        i = exit_i + 1          # no pyramiding same name; re-arm after exit
    return trades


def metrics(trades, label):
    if not trades:
        return None
    pnl = np.array([t[2] for t in trades])
    exit_dates = [t[1] for t in trades]
    wins = pnl[pnl > 0]; losses = pnl[pnl < 0]
    pf = wins.sum() / abs(losses.sum()) if losses.sum() != 0 else float("inf")
    wr = (pnl > 0).mean() * 100
    # equity-curve DD proxy: equal-weight, ordered by exit date
    order = np.argsort(pd.to_datetime(exit_dates).values)
    eq = np.cumsum(pnl[order]); peak = np.maximum.accumulate(eq)
    dd = (eq - peak).min()
    # both-halves: split by ENTRY date
    ent = pd.to_datetime([t[0] for t in trades]).values
    mid = np.median(ent.astype("int64"))
    h1 = pnl[ent.astype("int64") <= mid]; h2 = pnl[ent.astype("int64") > mid]
    def _pf(a):
        w = a[a > 0].sum(); l = abs(a[a < 0].sum())
        return w / l if l else float("inf")
    return {"label": label, "n": len(trades), "pf": pf, "wr": wr,
            "avg": pnl.mean() * 100, "med": np.median(pnl) * 100,
            "worst": pnl.min() * 100, "dd": dd * 100,
            "h1": _pf(h1), "h2": _pf(h2), "tot": pnl.sum() * 100}


def run_config(df, gate, contin_k, giveback, stall, atr_mult, max_hold, cost_rt, label):
    all_trades = []
    dates = df.index.to_numpy()
    for col in df.columns:
        s = df[col].to_numpy(dtype=float)
        all_trades += simulate(s, dates, gate, contin_k, giveback, stall, atr_mult, max_hold, cost_rt)
    return metrics(all_trades, label)


# curated liquid mega / large / semis universe (the validated tier; 2026 semis carry momentum)
LARGECAP = ("AAPL MSFT NVDA AMZN GOOGL GOOG META TSLA AVGO ORCL AMD MU INTC QCOM TXN ADI "
            "LRCX AMAT KLAC MRVL ARM SMCI PLTR CRWD PANW SNOW NOW CRM ADBE SHOP NFLX UBER "
            "ABNB COIN MSTR DELL ANET CDNS SNPS WDC ON MCHP NXPI HPQ DELL TSLA").split()


def hdr():
    print(f"\n{'config':30s} {'n':>5s} {'PF':>6s} {'WR%':>5s} {'avg%':>6s} {'worst%':>7s} {'H1':>5s} {'H2':>5s}")
    print("-" * 80)


def line(r):
    print(f"{r['label']:30s} {r['n']:5d} {r['pf']:6.2f} {r['wr']:5.0f} {r['avg']:6.2f} "
          f"{r['worst']:7.1f} {r['h1']:5.2f} {r['h2']:5.2f}")


def main():
    full = load()
    large = full[[c for c in dict.fromkeys(LARGECAP) if c in full.columns]]
    print(f"full universe={full.shape[1]}  largecap={large.shape[1]}  "
          f"dates={full.index.min().date()}->{full.index.max().date()}  rows={len(full)}")
    GATE = 0.05

    # STEP 1 -- does ENTRY type / universe recover a both-halves+ edge? (fixed WIDE exit)
    print("\n=== STEP 1: entry x universe @ WIDE exit (gb20/atr4/h20) -- looking for H1>1 AND H2>1 ===")
    hdr()
    for uname, u in (("full", full), ("large", large)):
        for ck in (0, 20, 60):
            lab = f"{uname}/contin{ck}"
            r = run_config(u, GATE, ck, 0.20, None, 4.0, 20, COST_RT_BASE, lab)
            if r: line(r)

    # STEP 2 -- pick the best entry, sweep the companion exit on it
    print("\n=== STEP 2: companion exit sweep on largecap + continuation(20) ===")
    U, CK = large, 20
    results = [run_config(U, GATE, CK, 0.20, None, 4.0, 20, COST_RT_BASE, "WIDE(gb20/atr4/h20)")]
    gbs = [None, 0.03, 0.05, 0.08, 0.12]; stalls = [None, 1, 2, 3, 5]
    atrs = [None, 1.0, 2.0, 3.0]; holds = [10, 20]; seen = set()
    for gb, st, am, h in itertools.product(gbs, stalls, atrs, holds):
        if gb is None and st is None and am is None: continue
        if (gb, st, am, h) in seen: continue
        seen.add((gb, st, am, h))
        lab = f"gb{gb if gb else '-'}/st{st if st else '-'}/atr{am if am else '-'}/h{h}"
        r = run_config(U, GATE, CK, gb, st, am, h, COST_RT_BASE, lab)
        if r: results.append(r)
    results = [r for r in results if r]; results.sort(key=lambda r: r["pf"], reverse=True)
    hdr()
    for r in results[:20]: line(r)


if __name__ == "__main__":
    main()
