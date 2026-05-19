"""Deployable XAUUSD long-only trend ensemble.

Three sub-strategies, equal-vol weighted:
  A) H1 EMA(20, 80), ATR(14)x4 stop, long-only, slow-EMA-rising filter
  B) H4 EMA(8, 21),  ATR(14)x2.5 stop, long-only, slow-EMA-rising filter
  C) H1 Donchian-40 long breakout, ATR(14)x5 stop

Each verified OOS on 25 months of XAUUSD walk-forward (2024-03 → 2026-04).

Public API:
  - `compute_signals(bars_h1, bars_h4)` -> dict of per-strategy target position {-1,0,+1}
  - `Strategy.run(bars_h1, bars_h4)` -> full backtest result (equity, trades, metrics)
  - `Strategy.current_position(bars_h1, bars_h4)` -> live decision dict
"""
from __future__ import annotations
import dataclasses
import os
import sys
from typing import Callable

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(__file__))
from gold_portfolio_sim import simulate_sleeve, DEFAULT_EQUITY
from gold_metrics import report, returns_from_equity

H1_SECONDS = 3600
H4_SECONDS = 14400
BPY_H1 = 252 * 24
BPY_H4 = 252 * 6


def _ema(x: pd.Series, span: int) -> pd.Series:
    return x.ewm(span=span, adjust=False, min_periods=span).mean()


# --- signals ---
def sig_h1_ema(b: pd.DataFrame, fast: int = 20, slow: int = 80) -> pd.Series:
    ef, es = _ema(b["close"], fast), _ema(b["close"], slow)
    pos = pd.Series(0, index=b.index, dtype="int8")
    pos[(ef > es) & (es > es.shift(3))] = 1
    return pos


def sig_h4_ema(b: pd.DataFrame, fast: int = 8, slow: int = 21) -> pd.Series:
    ef, es = _ema(b["close"], fast), _ema(b["close"], slow)
    pos = pd.Series(0, index=b.index, dtype="int8")
    pos[(ef > es) & (es > es.shift(3))] = 1
    return pos


def sig_h1_donchian(b: pd.DataFrame, n: int = 40) -> pd.Series:
    hh = b["high"].rolling(n).max().shift(1)
    ll = b["low"].rolling(n).min().shift(1)
    pos = pd.Series(0, index=b.index, dtype="int8")
    state = 0
    for i in range(len(b)):
        c = b["close"].iat[i]
        if state == 0:
            if not np.isnan(hh.iat[i]) and c > hh.iat[i]:
                state = 1
        elif state == 1:
            if not np.isnan(ll.iat[i]) and c < ll.iat[i]:
                state = 0
        pos.iat[i] = state
    return pos


@dataclasses.dataclass
class SubStrategy:
    name: str
    tf: str
    sig_fn: Callable
    sig_kwargs: dict
    atr_stop_mult: float
    bars_per_year: float


SUB_STRATEGIES: list[SubStrategy] = [
    SubStrategy("A_H1_EMA20_80",  "H1", sig_h1_ema,      {"fast": 20, "slow": 80}, 4.0, BPY_H1),
    SubStrategy("B_H4_EMA8_21",   "H4", sig_h4_ema,      {"fast": 8,  "slow": 21}, 2.5, BPY_H4),
    SubStrategy("C_H1_Donchian40","H1", sig_h1_donchian, {"n": 40},                5.0, BPY_H1),
]


@dataclasses.dataclass
class Strategy:
    """Ensemble of 3 long-only trend sub-strategies, equal-vol weighted.
    Vol target per sub = 10% annualized."""
    vol_target_per_sub: float = 0.10
    max_units_per_sub: float = 1.0  # cap as fraction of starting equity / price

    def run_sub(self, sub: SubStrategy, bars: pd.DataFrame) -> dict:
        pos = sub.sig_fn(bars, **sub.sig_kwargs)
        res = simulate_sleeve(
            bars, pos,
            sleeve_vol_target=self.vol_target_per_sub,
            atr_stop_mult=sub.atr_stop_mult,
            atr_take_mult=1e6,  # no take profit
            bars_per_year=sub.bars_per_year,
            use_bid_ask=True,
        )
        res["pos"] = pos
        res["sub"] = sub
        return res

    def run(self, bars_h1: pd.DataFrame, bars_h4: pd.DataFrame) -> dict:
        """Run full backtest. Returns sub results + ensemble equity + metrics."""
        sub_results = {}
        for sub in SUB_STRATEGIES:
            b = bars_h1 if sub.tf == "H1" else bars_h4
            sub_results[sub.name] = self.run_sub(sub, b)
        # ensemble: equal-vol normalize then sum; rebase all to H1 timeline
        ensemble_pnl_h1 = pd.Series(0.0, index=bars_h1.index)
        vols = {}
        for name, r in sub_results.items():
            rets = returns_from_equity(r["equity"])
            v = rets.std()
            vols[name] = v if v > 0 else 1e-9
        inv_vol_sum = sum(1.0 / v for v in vols.values())
        weights = {n: (1.0 / vols[n]) / inv_vol_sum for n in vols}
        for name, r in sub_results.items():
            w = weights[name]
            # rebase PnL onto H1 timeline
            pnl_resampled = r["bar_pnl"].reindex(bars_h1.index, method="pad").fillna(0.0)
            # for H4 sub, reindex from H4 to H1 by forward-fill cumulative then diff
            if r["sub"].tf == "H4":
                cum = r["bar_pnl"].cumsum().reindex(bars_h1.index, method="pad").fillna(0.0)
                pnl_resampled = cum.diff().fillna(cum)
            ensemble_pnl_h1 = ensemble_pnl_h1.add(pnl_resampled * w, fill_value=0.0)
        equity = DEFAULT_EQUITY + ensemble_pnl_h1.cumsum()
        rep = report("ensemble", equity, ensemble_pnl_h1,
                     bars_per_year=BPY_H1)
        return {
            "subs": sub_results,
            "weights": weights,
            "ensemble_equity": equity,
            "ensemble_pnl": ensemble_pnl_h1,
            "metrics": rep,
        }

    def current_position(self, bars_h1: pd.DataFrame, bars_h4: pd.DataFrame) -> dict:
        """Live decision: emit current target position for each sub + ensemble target.
        Returns latest bar timestamp and per-sub long/flat decision + suggested entry/stop."""
        decisions = []
        ensemble_long_share = 0.0
        total_w = 0.0
        for sub in SUB_STRATEGIES:
            b = bars_h1 if sub.tf == "H1" else bars_h4
            pos = sub.sig_fn(b, **sub.sig_kwargs)
            last_pos = int(pos.iloc[-1])
            last_close = float(b["close"].iloc[-1])
            last_atr = float(b["atr"].iloc[-1])
            stop_px = last_close - sub.atr_stop_mult * last_atr if last_pos == 1 else None
            decisions.append({
                "sub": sub.name,
                "tf": sub.tf,
                "target_position": last_pos,
                "last_close": last_close,
                "atr": last_atr,
                "suggested_stop": stop_px,
                "last_bar_ts": str(b.index[-1]),
            })
            w = 1.0 / 3  # placeholder; will be replaced by run() weighting if user wants exact
            if last_pos == 1:
                ensemble_long_share += w
            total_w += w
        return {
            "as_of": str(bars_h1.index[-1]),
            "decisions": decisions,
            "ensemble_long_share": ensemble_long_share / total_w if total_w else 0.0,
        }
