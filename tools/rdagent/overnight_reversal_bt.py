#!/usr/bin/env python3
"""
EDGE HUNT: overnight -> intraday reversal (Lou-Polk-Skouras "tug of war").

Hypothesis: a name that gaps UP overnight tends to fade during the day; one that
gaps DOWN tends to bounce. So go LONG the biggest overnight losers, SHORT the
biggest overnight gainers, hold INTRADAY ONLY, flat by close. This is:
  - market-neutral (cross-sectional long-short)
  - flat-by-close (zero overnight risk — fits the mandate)
  - a standalone signal (overnight gap), independent of the bull-beta ML signal

Validates with the truth protocol: IC of (-gap) vs intraday return, long-short
spread Sharpe, market beta, up/down-day split, gross->net cost curve.

    python overnight_reversal_bt.py --m15 /tmp/omega_15m --cost-bps 5
"""
from __future__ import annotations

import argparse

import numpy as np
import pandas as pd
from scipy.stats import spearmanr

import intraday_bt as ib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--m15", default="/tmp/omega_15m")
    ap.add_argument("--frac", type=float, default=0.2, help="top/bottom fraction to trade (0.2=quintile)")
    ap.add_argument("--cost-bps", type=float, default=5.0)
    ap.add_argument("--oos-frac", type=float, default=0.5)
    a = ap.parse_args()
    sess = ib._sessions(a.m15)
    syms = list(sess)
    gap = pd.DataFrame({s: sess[s]["overnight"] for s in syms})
    intr = pd.DataFrame({s: sess[s]["intraday"] for s in syms})
    mkt = intr.mean(axis=1)
    dates = sorted(set(gap.index) & set(intr.index))
    split = dates[int(len(dates) * (1 - a.oos_frac))]

    for tag, ds in [("FULL", dates), ("OOS", [d for d in dates if d >= split])]:
        for cost_bps in ([a.cost_bps] if tag == "OOS" else [0.0, a.cost_bps]):
            cost = cost_bps / 1e4
            spread, ics, pl, ps = {}, {}, set(), set()
            for d in ds:
                g = gap.loc[d].dropna()
                r = intr.loc[d].dropna()
                common = g.index.intersection(r.index)
                if len(common) < 8:
                    continue
                g, r = g[common], r[common]
                k = max(1, int(len(common) * a.frac))
                longs = list(g.sort_values().head(k).index)      # gapped DOWN -> long
                shorts = list(g.sort_values(ascending=False).head(k).index)  # gapped UP -> short
                turn = (len(set(longs) ^ pl) + len(set(shorts) ^ ps)) / (2 * k)
                spread[d] = r[longs].mean() - r[shorts].mean() - turn * cost
                ic = spearmanr(-g.values, r.values).correlation
                if ic == ic:
                    ics[d] = ic
                pl, ps = set(longs), set(shorts)
            s = pd.Series(spread)
            ic = pd.Series(ics)
            if len(s) < 20:
                continue
            m = mkt.reindex(s.index)
            beta = float(np.polyfit(m.fillna(0), s, 1)[0]) if m.std() > 0 else 0.0
            up, dn = s[m > 0], s[m < 0]
            sh = s.mean() / s.std() * np.sqrt(252)
            print(f"[{tag} {cost_bps:.0f}bps] Sharpe {sh:5.2f} | ann {s.mean()*252*100:6.1f}% | "
                  f"IC {ic.mean():+.4f} IC_IR {ic.mean()/ic.std()*np.sqrt(252):5.2f} | "
                  f"beta {beta:+.2f} | up {up.mean()*1e4:+.1f}bps dn {dn.mean()*1e4:+.1f}bps | n {len(s)}")
    print("\nlong gapped-down / short gapped-up, intraday only. IC>0 (of -gap) = reversal real. beta~0 = neutral.")


if __name__ == "__main__":
    main()
