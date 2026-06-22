#!/usr/bin/env python3
"""
Trailing-stop / risk-adjusted-ranking backtest for the RD-Agent bigcap signal.

Question: take the highest RISK-ADJUSTED ranked names, hold K of them, and protect
open profit with a trailing stop. How aggressive should the trail be?

Signal: daily model score (from the qlib run's pred.pkl), risk-adjusted by dividing
by 20d realized vol. Each freed slot is filled by the best-ranked name not held.
Positions exit ONLY on their trailing stop (except the rebalance baseline). Daily
bars; the stop is checked intrabar against the day's low; fills at next open.

Compares several trail policies and reports, per policy:
  net ann return, Sharpe, max drawdown, win rate, avg hold days, avg profit GIVEBACK
  (peak-unrealized minus realized — the "don't give back profit" metric).

    python backtest_trail.py --bars /tmp/omega_bars_real --mlruns /tmp/omega_factors/mlruns \
        --topk 5 --cost-bps 10
"""
from __future__ import annotations

import argparse
import glob
import os
from pathlib import Path

import numpy as np
import pandas as pd


def _load_bars(bars_dir: str) -> dict[str, pd.DataFrame]:
    import re
    out = {}
    for f in sorted(glob.glob(os.path.join(bars_dir, "*.csv"))):
        sym = re.sub(r"_(d1|15m|5m|30m|1h|h1|h4)$", "", Path(f).stem, flags=re.I)
        df = pd.read_csv(f).rename(columns=str.lower)
        df["date"] = pd.to_datetime(df["ts"], unit="s") if "ts" in df else pd.to_datetime(df["date"])
        df = df.set_index("date").sort_index()
        tr = pd.concat([df.high - df.low, (df.high - df.close.shift()).abs(), (df.low - df.close.shift()).abs()], axis=1).max(axis=1)
        df["atr"] = tr.rolling(14).mean()
        df["vol20"] = df.close.pct_change().rolling(20).std()
        out[sym] = df
    return out


def _signal(mlruns: str) -> pd.DataFrame:
    p = max(glob.glob(os.path.join(mlruns, "**", "pred.pkl"), recursive=True), key=os.path.getmtime)
    s = pd.read_pickle(p)
    return s[s.columns[0]].unstack("instrument")  # date x instrument


def _stop(policy: str, entry: float, peak: float, atr: float) -> float:
    if policy == "trail_wide":
        return peak - 4.0 * atr
    if policy == "trail_med":
        return peak - 2.5 * atr
    if policy == "trail_aggr":
        return peak - 1.5 * atr
    if policy == "trail_pct3":
        return peak * 0.97
    if policy == "be_then_wide":  # breakeven once +5%, else 4xATR from peak
        be = entry if peak >= entry * 1.05 else -np.inf
        return max(peak - 4.0 * atr, be)
    if policy == "rebal_catstop":  # daily rebalance + WIDE catastrophic stop only
        return peak - 6.0 * atr
    return -np.inf  # rebalance baseline: no stop


def run(policy: str, bars, sig, topk: int, cost_bps: float):
    cost = cost_bps / 1e4
    # risk-adjusted rank: score / vol20
    syms = [s for s in sig.columns if s in bars]
    dates = [d for d in sig.index if all(d in bars[s].index for s in syms[:1])]
    dates = sorted(set(sig.index) & set.union(*[set(bars[s].index) for s in syms]))
    held: dict[str, dict] = {}
    eq = 1.0
    eqs, trades = [], []
    prev_date = None
    for d in dates:
        # rank using yesterday's signal (act today)
        if prev_date is None or prev_date not in sig.index:
            prev_date = d
            continue
        row = sig.loc[prev_date].dropna()
        radj = {}
        for s in row.index:
            if s in bars and d in bars[s].index:
                v = bars[s].loc[:prev_date]["vol20"].iloc[-1] if prev_date in bars[s].index else np.nan
                if v and not np.isnan(v) and v > 0:
                    radj[s] = row[s] / v
        ranked = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)]

        day_ret = 0.0
        n_slots = topk
        # 1) manage held: update peak, check stop
        for s in list(held.keys()):
            b = bars[s]
            if d not in b.index:
                continue
            o, h, l, c = b.loc[d, ["open", "high", "low", "close"]]
            atr = b.loc[d, "atr"]
            pos = held[s]
            pos["peak"] = max(pos["peak"], h)
            stop = _stop(policy, pos["entry"], pos["peak"], atr if atr == atr else 0.0)
            prev_c = pos["last"]
            rebalance_drop = (policy in ("rebalance", "rebal_catstop")) and (s not in ranked[:topk])
            if l <= stop and stop > -np.inf:  # stopped intrabar
                exit_px = min(o, stop)
                r = exit_px / prev_c - 1
                day_ret += (1 / topk) * (r) - (1 / topk) * cost
                pos_peak_ret = pos["peak"] / pos["entry"] - 1
                realized = exit_px / pos["entry"] - 1
                trades.append({"sym": s, "hold": pos["days"], "ret": realized,
                               "giveback": pos_peak_ret - realized})
                del held[s]
            elif rebalance_drop:
                r = c / prev_c - 1
                day_ret += (1 / topk) * r - (1 / topk) * cost
                realized = c / pos["entry"] - 1
                trades.append({"sym": s, "hold": pos["days"], "ret": realized,
                               "giveback": (pos["peak"] / pos["entry"] - 1) - realized})
                del held[s]
            else:
                r = c / prev_c - 1
                day_ret += (1 / topk) * r
                pos["last"] = c
                pos["days"] += 1
        # 2) fill free slots with best-ranked not held
        free = topk - len(held)
        for s in ranked:
            if free <= 0:
                break
            if s in held or d not in bars[s].index:
                continue
            o = bars[s].loc[d, "open"]; c = bars[s].loc[d, "close"]
            held[s] = {"entry": o, "peak": bars[s].loc[d, "high"], "last": c, "days": 1}
            day_ret += (1 / topk) * (c / o - 1) - (1 / topk) * cost
            free -= 1
        eq *= (1 + day_ret)
        eqs.append((d, eq))
        prev_date = d

    eqs = pd.Series(dict(eqs))
    rets = eqs.pct_change().dropna()
    if len(rets) < 5:
        return None
    dd = float((eqs / eqs.cummax() - 1).min())
    tr = pd.DataFrame(trades)
    return {
        "policy": policy,
        "ann_return": round(float(rets.mean() * 252), 3),
        "sharpe": round(float(rets.mean() / rets.std() * np.sqrt(252)), 2),
        "max_dd": round(dd, 3),
        "win_rate": round(float((tr["ret"] > 0).mean()), 2) if len(tr) else None,
        "avg_hold_d": round(float(tr["hold"].mean()), 1) if len(tr) else None,
        "avg_giveback": round(float(tr["giveback"].mean()), 3) if len(tr) else None,
        "n_trades": len(tr),
    }


def run_bracket(policy: str, bars, sig, topk: int, cost_bps: float, tp: float, sl: float):
    """Bracket policies.
    'bracket'        — each position has TP/SL, resolves independently, refill freed slots.
    'first_win_flat' — same brackets, but the first position to hit TP flattens the WHOLE book
                       that day; re-enter from signal next day.
    """
    cost = cost_bps / 1e4
    syms = [s for s in sig.columns if s in bars]
    dates = sorted(set(sig.index) & set.union(*[set(bars[s].index) for s in syms]))
    held: dict[str, dict] = {}
    eq = 1.0
    eqs, trades = [], []
    prev_date = None
    for d in dates:
        if prev_date is None or prev_date not in sig.index:
            prev_date = d
            continue
        row = sig.loc[prev_date].dropna()
        radj = {}
        for s in row.index:
            if s in bars and d in bars[s].index and prev_date in bars[s].index:
                v = bars[s].loc[prev_date, "vol20"]
                if v and v == v and v > 0:
                    radj[s] = row[s] / v
        ranked = [s for s, _ in sorted(radj.items(), key=lambda x: x[1], reverse=True)]
        day_ret = 0.0
        won_today = False
        for s in list(held.keys()):
            b = bars[s]
            if d not in b.index:
                continue
            o, h, l, c = b.loc[d, ["open", "high", "low", "close"]]
            pos = held[s]
            prev_c, e = pos["last"], pos["entry"]
            tp_px, sl_px = e * (1 + tp), e * (1 - sl)
            if l <= sl_px:                       # stop first if both touched (conservative)
                ex = min(o, sl_px)
                day_ret += (1 / topk) * (ex / prev_c - 1) - (1 / topk) * cost
                trades.append({"sym": s, "ret": ex / e - 1, "hold": pos["days"], "giveback": 0.0})
                del held[s]
            elif h >= tp_px:                     # take profit = "a winner"
                ex = max(o, tp_px)
                day_ret += (1 / topk) * (ex / prev_c - 1) - (1 / topk) * cost
                trades.append({"sym": s, "ret": ex / e - 1, "hold": pos["days"], "giveback": 0.0})
                del held[s]
                won_today = True
            else:
                day_ret += (1 / topk) * (c / prev_c - 1)
                pos["last"] = c
                pos["days"] += 1
        if policy == "first_win_flat" and won_today:   # one winner -> flatten everything
            for s in list(held.keys()):
                c = bars[s].loc[d, "close"] if d in bars[s].index else held[s]["last"]
                day_ret -= (1 / topk) * cost
                trades.append({"sym": s, "ret": c / held[s]["entry"] - 1, "hold": held[s]["days"], "giveback": 0.0})
                del held[s]
        if not (policy == "first_win_flat" and won_today):   # don't re-enter on a flatten day
            free = topk - len(held)
            for s in ranked:
                if free <= 0:
                    break
                if s in held or d not in bars[s].index:
                    continue
                o = bars[s].loc[d, "open"]; c = bars[s].loc[d, "close"]
                held[s] = {"entry": o, "peak": bars[s].loc[d, "high"], "last": c, "days": 1}
                day_ret += (1 / topk) * (c / o - 1) - (1 / topk) * cost
                free -= 1
        eq *= (1 + day_ret)
        eqs.append((d, eq))
        prev_date = d

    eqs = pd.Series(dict(eqs))
    rets = eqs.pct_change().dropna()
    if len(rets) < 5:
        return None
    dd = float((eqs / eqs.cummax() - 1).min())
    tr = pd.DataFrame(trades)
    return {
        "policy": policy,
        "ann_return": round(float(rets.mean() * 252), 3),
        "sharpe": round(float(rets.mean() / rets.std() * np.sqrt(252)), 2),
        "max_dd": round(dd, 3),
        "win_rate": round(float((tr["ret"] > 0).mean()), 2) if len(tr) else None,
        "avg_hold_d": round(float(tr["hold"].mean()), 1) if len(tr) else None,
        "avg_giveback": None,
        "n_trades": len(tr),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", default="/tmp/omega_bars_real")
    ap.add_argument("--mlruns", default="/tmp/omega_factors/mlruns")
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--cost-bps", type=float, default=10.0)
    ap.add_argument("--tp", type=float, default=0.08)
    ap.add_argument("--sl", type=float, default=0.08)
    a = ap.parse_args()
    bars = _load_bars(a.bars)
    sig = _signal(a.mlruns)
    trail = ["rebalance", "trail_wide", "trail_aggr", "trail_pct3"]
    rows = [run(p, bars, sig, a.topk, a.cost_bps) for p in trail]
    brk = [run_bracket(p, bars, sig, a.topk, a.cost_bps, a.tp, a.sl) for p in ["bracket", "first_win_flat"]]
    rows = [r for r in rows + brk if r]
    df = pd.DataFrame(rows).set_index("policy")
    print(f"\nBigcap RD-Agent signal · top-{a.topk} · {a.cost_bps}bps · TP +{a.tp:.0%}/SL -{a.sl:.0%}\n")
    print(df.to_string())
    print("\nbracket        = each position TP/SL, independent")
    print("first_win_flat = flatten the WHOLE book the first time any position hits TP")


if __name__ == "__main__":
    main()
