"""Portfolio sim: vol-target sizing per sleeve, ATR stops, bid/ask exec, risk caps."""
from __future__ import annotations
import numpy as np
import pandas as pd

POINT_USD_PER_OZ = 1.0  # XAUUSD: 1 USD move = 1 USD per oz
DEFAULT_EQUITY = 100_000.0


def vol_target_size(bars: pd.DataFrame, target_ann_vol: float = 0.10,
                    lookback: int = 60, bars_per_year: float = 252 * 24 * 12) -> pd.Series:
    """Position scalar so that sleeve realized vol ~= target.
    bars_per_year for 5m: 252*24*12 = 72576."""
    rv = bars["lret"].rolling(lookback, min_periods=10).std() * np.sqrt(bars_per_year)
    scalar = (target_ann_vol / rv.replace(0, np.nan)).clip(upper=4.0).fillna(0.0)
    return scalar


def simulate_sleeve(bars: pd.DataFrame, pos: pd.Series, sleeve_vol_target: float = 0.08,
                    atr_stop_mult: float = 2.5, atr_take_mult: float = 4.0,
                    bars_per_year: float = 72576.0,
                    use_bid_ask: bool = True) -> dict:
    """Walk bars, simulate fills at bid/ask, ATR stop & take.
    Returns dict with equity curve, trades, per-bar pnl."""
    scalar = vol_target_size(bars, sleeve_vol_target, 60, bars_per_year).values
    pos_arr = pos.reindex(bars.index).fillna(0).astype(int).values
    close = bars["close"].values
    high = bars["high"].values
    low = bars["low"].values
    bid = bars["bid"].values
    ask = bars["ask"].values
    atr = bars["atr"].bfill().fillna(1.0).values
    n = len(bars)
    equity = np.zeros(n)
    equity[0] = DEFAULT_EQUITY
    notional = DEFAULT_EQUITY  # constant base notional; scalar adjusts

    # state
    cur_dir = 0
    entry_px = 0.0
    stop_px = 0.0
    take_px = 0.0
    units = 0.0  # ounces
    trades = []
    bar_pnl = np.zeros(n)

    for i in range(1, n):
        eq_prev = equity[i - 1]
        pnl = 0.0

        # mark-to-market if in position (mid close)
        if cur_dir != 0:
            # check stop/take using high/low intrabar
            hit_stop = (cur_dir == 1 and low[i] <= stop_px) or (cur_dir == -1 and high[i] >= stop_px)
            hit_take = (cur_dir == 1 and high[i] >= take_px) or (cur_dir == -1 and low[i] <= take_px)
            exit_now = False
            exit_px = close[i]
            exit_reason = ""
            if hit_stop and hit_take:
                # conservative: assume stop first
                exit_px = stop_px
                exit_now = True
                exit_reason = "stop"
            elif hit_stop:
                exit_px = stop_px
                exit_now = True
                exit_reason = "stop"
            elif hit_take:
                exit_px = take_px
                exit_now = True
                exit_reason = "take"
            elif pos_arr[i] != cur_dir:
                # signal reversal/flat -> exit at bid/ask
                exit_px = bid[i] if cur_dir == 1 and use_bid_ask else (ask[i] if use_bid_ask else close[i])
                exit_now = True
                exit_reason = "signal"

            if exit_now:
                pnl = (exit_px - entry_px) * cur_dir * units
                trades.append({
                    "entry_ts": entry_ts, "exit_ts": bars.index[i],
                    "dir": cur_dir, "entry": entry_px, "exit": exit_px,
                    "units": units, "pnl": pnl, "reason": exit_reason,
                    "bars_held": i - entry_i,
                })
                cur_dir = 0
                units = 0.0

        # entry check (flat only)
        if cur_dir == 0 and pos_arr[i] != 0 and scalar[i] > 0 and atr[i] > 0:
            d = pos_arr[i]
            entry_px = ask[i] if (d == 1 and use_bid_ask) else (bid[i] if use_bid_ask else close[i])
            stop_dist = atr[i] * atr_stop_mult
            take_dist = atr[i] * atr_take_mult
            stop_px = entry_px - d * stop_dist
            take_px = entry_px + d * take_dist
            risk_usd = eq_prev * 0.01 * scalar[i]  # 1% risk * scalar
            units = risk_usd / stop_dist if stop_dist > 0 else 0.0
            units = min(units, eq_prev * 0.5 / entry_px)  # 50% notional cap
            if units > 0:
                cur_dir = d
                entry_ts = bars.index[i]
                entry_i = i

        equity[i] = eq_prev + pnl
        bar_pnl[i] = pnl

    # close any open position at end
    if cur_dir != 0:
        exit_px = bid[-1] if cur_dir == 1 else ask[-1]
        pnl = (exit_px - entry_px) * cur_dir * units
        equity[-1] += pnl
        bar_pnl[-1] += pnl
        trades.append({
            "entry_ts": entry_ts, "exit_ts": bars.index[-1],
            "dir": cur_dir, "entry": entry_px, "exit": exit_px,
            "units": units, "pnl": pnl, "reason": "eod",
            "bars_held": n - 1 - entry_i,
        })

    return {
        "equity": pd.Series(equity, index=bars.index),
        "bar_pnl": pd.Series(bar_pnl, index=bars.index),
        "trades": pd.DataFrame(trades) if trades else pd.DataFrame(
            columns=["entry_ts", "exit_ts", "dir", "entry", "exit", "units", "pnl", "reason", "bars_held"]
        ),
    }


def combine_sleeves(sleeve_results: dict, weights: dict | None = None) -> dict:
    """Sum per-bar PnL across sleeves with weights, build portfolio equity, aggregate trades."""
    if weights is None:
        weights = {k: 1.0 / len(sleeve_results) for k in sleeve_results}
    idx = next(iter(sleeve_results.values()))["bar_pnl"].index
    total_pnl = pd.Series(0.0, index=idx)
    all_trades = []
    for name, res in sleeve_results.items():
        w = weights.get(name, 0.0)
        total_pnl = total_pnl.add(res["bar_pnl"].reindex(idx).fillna(0.0) * w, fill_value=0.0)
        if w > 0 and len(res["trades"]) > 0:
            t = res["trades"].copy()
            t["pnl"] = t["pnl"] * w
            t["sleeve"] = name
            all_trades.append(t)
    equity = DEFAULT_EQUITY + total_pnl.cumsum()
    trades = pd.concat(all_trades, ignore_index=True) if all_trades else pd.DataFrame(
        columns=["entry_ts", "exit_ts", "dir", "entry", "exit", "units", "pnl", "reason", "bars_held", "sleeve"]
    )
    return {"equity": equity, "bar_pnl": total_pnl, "trades": trades}
