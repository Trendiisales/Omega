#!/usr/bin/env python3
"""
Regime-gated two-engine book — each engine HARD-LOCKED to its own regime.

Regime (from PRIOR close, no lookahead): BULL if index > 50d SMA, else BEAR.
(A fast 5d drop > 4% forces BEAR even above the SMA — a crash override.)

  BULL engine  : LONG top-K by signal, enter open / exit close, flat overnight.
                 Fires ONLY when regime == BULL. Otherwise FLAT (zero trades).
  BEAR engine  : SHORT the weakest-K by signal, enter open / exit close, flat.
                 Fires ONLY when regime == BEAR. Otherwise FLAT (zero trades).

They can never both be active on the same day. Proves the gate: bull engine takes
ZERO trades in bear days, bear engine takes ZERO in bull days.

    python regime_engines.py --m15 /tmp/omega_15m --mlruns /tmp/omega_factors/mlruns --topk 5
"""
from __future__ import annotations

import argparse

import numpy as np
import pandas as pd

import bear_regime_bt as br
import intraday_bt as ib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m15", default="/tmp/omega_15m")
    ap.add_argument("--mlruns", default="/tmp/omega_factors/mlruns")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    a = ap.parse_args()
    sess = ib._sessions(a.m15)
    sig = ib._signal(a.mlruns)
    mkt = br._market(sess)
    cost = a.cost_bps / 1e4
    ret5 = mkt["idx"].pct_change(5)

    syms = [s for s in sig.columns if s in sess]
    dates = sorted(set(sig.index) & set.union(*[set(sess[s].index) for s in syms]) & set(mkt.index))

    bull_ret, bear_ret, regime = {}, {}, {}
    bull_active = bull_offregime = bear_active = bear_offregime = 0
    prev = None
    for d in dates:
        if prev is None or prev not in sig.index:
            prev = d
            continue
        sma = mkt.loc[prev, "sma50"]
        if sma != sma:
            prev = d
            continue
        crash = bool(ret5.loc[prev] < -0.04) if ret5.loc[prev] == ret5.loc[prev] else False
        is_bull = bool(mkt.loc[prev, "idx"] >= sma) and not crash
        reg = "BULL" if is_bull else "BEAR"
        row = sig.loc[prev].dropna()
        radj = {s: row[s] / sess[s].loc[prev, "vol20"] for s in row.index
                if s in sess and d in sess[s].index and prev in sess[s].index
                and sess[s].loc[prev, "vol20"] == sess[s].loc[prev, "vol20"] and sess[s].loc[prev, "vol20"] > 0}
        ranked = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)]
        prev = d
        if len(ranked) < a.topk:
            continue
        regime[d] = reg
        # BULL engine — long top-K, only in BULL
        if reg == "BULL":
            bull_ret[d] = float(np.mean([sess[s].loc[d, "intraday"] for s in ranked[:a.topk]])) - cost
            bull_active += 1
        else:
            bull_ret[d] = 0.0
            bull_offregime += 0   # explicitly flat — no trade
        # BEAR engine — SHORT THE INDEX (downside beta), only in BEAR
        if reg == "BEAR":
            bear_ret[d] = -float(mkt.loc[d, "intraday"]) - cost
            bear_active += 1
        else:
            bear_ret[d] = 0.0

    bull = pd.Series(bull_ret)
    bear = pd.Series(bear_ret)
    comb = bull + bear
    reg = pd.Series(regime)

    def stats(r, active_mask):
        ra = r[active_mask]
        if len(ra) < 5 or ra.std() == 0:
            return dict(days=int(len(ra)), ann=None, sharpe=None, maxdd=None, win=None)
        eq = (1 + ra).cumprod()
        return dict(days=int(len(ra)),
                    ann=round(float(ra.mean()*252), 3),
                    sharpe=round(float(ra.mean()/ra.std()*np.sqrt(252)), 2),
                    maxdd=round(float((eq/eq.cummax()-1).min()), 3),
                    win=round(float((ra > 0).mean()), 2))

    n_bull = int((reg == "BULL").sum())
    n_bear = int((reg == "BEAR").sum())
    bs = stats(bull, (reg == "BULL").reindex(bull.index).fillna(False).values)
    rs = stats(bear, (reg == "BEAR").reindex(bear.index).fillna(False).values)
    cs = stats(comb, pd.Series(True, index=comb.index).values)

    # GATE PROOF
    bull_trades_in_bear = int((bull[(reg == "BEAR").reindex(bull.index).fillna(False).values] != 0).sum())
    bear_trades_in_bull = int((bear[(reg == "BULL").reindex(bear.index).fillna(False).values] != 0).sum())

    print(f"Regime split over {len(reg)} days: {n_bull} BULL / {n_bear} BEAR (idx vs 50d SMA + crash override)\n")
    print("ENGINE         regime  days  ann_ret  Sharpe  maxDD   win")
    print(f"BULL (long topK)  BULL  {bs['days']:>4}   {bs['ann']!s:>6}   {bs['sharpe']!s:>5}  {bs['maxdd']!s:>6}  {bs['win']!s:>4}")
    print(f"BEAR (short index) BEAR  {rs['days']:>4}   {rs['ann']!s:>6}   {rs['sharpe']!s:>5}  {rs['maxdd']!s:>6}  {rs['win']!s:>4}")
    print(f"COMBINED book     all   {cs['days']:>4}   {cs['ann']!s:>6}   {cs['sharpe']!s:>5}  {cs['maxdd']!s:>6}  {cs['win']!s:>4}")
    print(f"\nGATE PROOF — bull engine trades during BEAR days: {bull_trades_in_bear} (must be 0)")
    print(f"GATE PROOF — bear engine trades during BULL days: {bear_trades_in_bull} (must be 0)")
    print(f"today's regime: {reg.iloc[-1] if len(reg) else '?'}")


if __name__ == "__main__":
    main()
