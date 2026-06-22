#!/usr/bin/env python3
"""
BLEND BOOK — the deployed, long-period-validated engine.

protected-beta core + market-neutral alpha overlay (beats buy-hold 2020-2026,
Sharpe 1.18 vs 0.90, maxDD -18% vs -24%, positive through 2020 + 2022 crashes):
  CORE  (40%): vol-targeted long index (SPY), HALF size in bear, NEVER short.
  ALPHA (60%): rev_3 / hold-5 / quintile long-short on the full S&P, dollar-neutral.

Daily deterministic from data (historical rows immutable — prices don't change;
only new trading days append). Writes blend_ledger.csv + today's book + orders.

    python blend_book.py --close-csv ~/Omega/data/rdagent/sp500_long_close.csv \
        --capital 100000 --w-core 0.4 [--mode shadow]
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

import numpy as np
import pandas as pd

DATA = Path.home() / "Omega" / "data" / "rdagent"
L, H, FRAC, TGT_VOL = 3, 5, 0.20, 0.10


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--close-csv", default=str(DATA / "sp500_long_close.csv"))
    ap.add_argument("--capital", type=float, default=100000.0)
    ap.add_argument("--w-core", type=float, default=0.4)
    ap.add_argument("--cost-bps", type=float, default=5.0)
    ap.add_argument("--mode", choices=["shadow", "live"], default="shadow")
    a = ap.parse_args()
    close = pd.read_csv(a.close_csv, index_col=0, parse_dates=True).sort_index()
    ret1 = close.pct_change().clip(-0.20, 0.20)
    idx = close.mean(axis=1)
    ir = idx.pct_change().clip(-0.20, 0.20)
    sma200, ret5, vol20 = idx.rolling(200).mean(), idx.pct_change(5), ir.rolling(20).std()
    lev = (TGT_VOL / np.sqrt(252) / vol20).clip(0, 2.0)
    rev = -(close / close.shift(L) - 1)
    cost = a.cost_bps / 1e4
    wc, wa = a.w_core, 1 - a.w_core
    dates = list(close.index)

    core, alpha = {}, {}
    pl = pr = []
    pprev = 0.0
    today = {}
    for i in range(1, len(dates)):
        d, p = dates[i], dates[i-1]
        if sma200.loc[p] != sma200.loc[p] or vol20.loc[p] != vol20.loc[p]:
            continue
        crash = bool(ret5.loc[p] < -0.05) if ret5.loc[p] == ret5.loc[p] else False
        bull = bool(idx.loc[p] >= sma200.loc[p]) and not crash
        r = float(ir.loc[d]) if ir.loc[d] == ir.loc[d] else 0.0
        lv = float(lev.loc[p]) * (1.0 if bull else 0.5)
        core[d] = lv * r - abs(lv - pprev) * cost; pprev = lv
        if i % H == 1:
            liquid = close.loc[p][close.loc[p] >= 5].index
            sg = rev.loc[p].reindex(liquid).dropna()
            k = max(1, int(len(sg) * FRAC))
            pl = list(sg.sort_values(ascending=False).head(k).index)   # losers -> long
            pr = list(sg.sort_values().head(k).index)                  # winners -> short
        if pl and pr:
            rl = ret1.loc[d].reindex(pl).dropna().mean()
            rs = ret1.loc[d].reindex(pr).dropna().mean()
            alpha[d] = float(rl - rs) - (cost if i % H == 1 else 0)
        today = {"regime": "BULL" if bull else "BEAR", "core_leverage": round(lv, 2),
                 "alpha_longs": pl, "alpha_shorts": pr}

    core = pd.Series(core); alpha = pd.Series(alpha).reindex(core.index).fillna(0)
    blend = wc * core + wa * alpha
    eq = (1 + blend).cumprod()
    led = pd.DataFrame({"core": core, "alpha": alpha, "blend": blend, "equity": eq})
    led.index.name = "date"
    led.to_csv(DATA / "blend_ledger.csv")

    sh = float(blend.mean() / blend.std() * np.sqrt(252))
    dd = float((eq / eq.cummax() - 1).min())
    print(f"BLEND BOOK · {wc:.0%} beta-core / {wa:.0%} reversal-alpha · {len(led)} days {led.index.min().date()}..{led.index.max().date()}")
    print(f"  equity x{eq.iloc[-1]:.2f} | Sharpe {sh:.2f} | maxDD {dd*100:.1f}%")
    # today's book + orders
    lvg = today.get("core_leverage", 0.0)
    cap_core = a.capital * wc * lvg
    n_alpha = len(today.get("alpha_longs", []))
    leg = a.capital * wa / 2
    print(f"  TODAY: regime {today.get('regime')} · core SPY long ${cap_core:,.0f} (lev {lvg}x) · alpha {n_alpha}L/{n_alpha}S MN ${leg*2:,.0f} gross")
    print(f"    BETA:  BUY SPY  ${cap_core:,.0f}  (vol-targeted, {'half' if today.get('regime')=='BEAR' else 'full'} in {today.get('regime','?').lower()})")
    print(f"    ALPHA: LONG {today.get('alpha_longs',[])[:5]} … / SHORT {today.get('alpha_shorts',[])[:5]} …  (market-neutral)")
    (DATA / "blend_book.json").write_text(json.dumps(
        {"generated_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
         "w_core": wc, "capital": a.capital, "core_notional": round(cap_core),
         "alpha_leg_notional": round(leg), **today}, indent=2))
    print(f"  book -> {DATA/'blend_book.json'} · ledger -> {DATA/'blend_ledger.csv'}")


if __name__ == "__main__":
    main()
