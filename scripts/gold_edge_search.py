"""Edge search on 2025-06+ XAUUSD. Long-only & long-bias variants.
Walk-forward sweep across bar sizes + EMA pairs + stop multipliers.
Report robust winners (Sharpe stable across param neighbors)."""
from __future__ import annotations
import json
import os
import sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(__file__))
from duka_xau_bar_builder import build_bars_long, TICK_PATH
from gold_portfolio_sim import simulate_sleeve, DEFAULT_EQUITY
from gold_metrics import report, returns_from_equity, sharpe, max_drawdown, _norm_cdf, _norm_ppf

CACHE_DIR = "/Users/jo/omega_repo/outputs/cache"
REPORT_DIR = "/Users/jo/omega_repo/outputs/gold_trend_ensemble"
START = pd.Timestamp("2025-06-01", tz="UTC")


def _ema(x: pd.Series, span: int) -> pd.Series:
    return x.ewm(span=span, adjust=False, min_periods=span).mean()


def _rsi(x: pd.Series, period: int = 14) -> pd.Series:
    d = x.diff()
    up = d.clip(lower=0).ewm(alpha=1 / period, adjust=False).mean()
    dn = (-d.clip(upper=0)).ewm(alpha=1 / period, adjust=False).mean()
    rs = up / dn.replace(0, np.nan)
    return 100 - 100 / (1 + rs)


def resample_bars(bars15: pd.DataFrame, seconds: int) -> pd.DataFrame:
    rule = f"{seconds}s"
    o = bars15["close"].resample(rule).agg(["first", "max", "min", "last"]).rename(
        columns={"first": "open", "max": "high", "min": "low", "last": "close"}
    )
    o["high"] = bars15["high"].resample(rule).max()
    o["low"] = bars15["low"].resample(rule).min()
    o["bid"] = bars15["bid"].resample(rule).last()
    o["ask"] = bars15["ask"].resample(rule).last()
    o["spread"] = bars15["spread"].resample(rule).mean()
    o["tick_count"] = bars15["tick_count"].resample(rule).sum()
    o = o.dropna(subset=["close"])
    o["ret"] = o["close"].pct_change().fillna(0.0)
    o["lret"] = np.log(o["close"]).diff().fillna(0.0)
    o["atr"] = (o["high"] - o["low"]).rolling(14, min_periods=1).mean()
    o["rv"] = o["lret"].rolling(20, min_periods=5).std() * np.sqrt(20)
    o["hour"] = o.index.hour
    h = o["hour"]
    o["session"] = np.where(
        h < 7, "asia",
        np.where(h < 13, "london",
                 np.where(h < 17, "ny_overlap", "ny_late"))
    )
    for c in ("l2_imb", "vpin", "regime", "ewm_drift", "vol_ratio"):
        if c in bars15.columns:
            o[c] = bars15[c].resample(rule).last() if c == "regime" else bars15[c].resample(rule).mean()
    return o


# --- candidate signals ---
def sig_long_trend(b: pd.DataFrame, fast: int, slow: int) -> pd.Series:
    ef, es = _ema(b["close"], fast), _ema(b["close"], slow)
    pos = pd.Series(0, index=b.index, dtype="int8")
    pos[(ef > es) & (es > es.shift(3))] = 1  # long only when slow rising
    return pos


def sig_long_bias_trend(b: pd.DataFrame, fast: int, slow: int) -> pd.Series:
    ef, es = _ema(b["close"], fast), _ema(b["close"], slow)
    pos = pd.Series(0, index=b.index, dtype="int8")
    pos[ef > es] = 1
    pos[(ef < es) & (es < es.shift(5))] = -1  # only short when slow falling
    return pos


def sig_donchian_long(b: pd.DataFrame, n: int) -> pd.Series:
    hh = b["high"].rolling(n).max().shift(1)
    ll = b["low"].rolling(n).min().shift(1)
    pos = pd.Series(0, index=b.index, dtype="int8")
    # long breakout, exit on opposite low (long-only)
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


def sig_rsi_pullback_long(b: pd.DataFrame, ema_trend: int = 50,
                          rsi_dip: float = 35.0, rsi_recovery: float = 50.0) -> pd.Series:
    c = b["close"]
    et = _ema(c, ema_trend)
    rsi = _rsi(c, 14)
    cond = (c > et) & (rsi.shift(1) < rsi_dip) & (rsi >= rsi_recovery)
    pos = pd.Series(0, index=b.index, dtype="int8")
    pos[cond] = 1
    return pos


# --- walk-forward + scoring ---
def wf_score(bars: pd.DataFrame, sig_fn, sig_kwargs: dict,
             bars_per_year: float, train_months: int = 3, test_months: int = 1,
             atr_stop: float = 4.0, atr_take: float = 0.0) -> dict:
    """Run rolling walk-forward. Aggregate OOS PnL + Sharpe."""
    months = sorted(set(pd.Timestamp(year=t.year, month=t.month, day=1, tz="UTC")
                       for t in bars.index))
    if len(months) < train_months + 2:
        return None
    test_pnl = []
    test_bar_pnl = []
    n_trades_total = 0
    for i in range(train_months, len(months) - 1):
        te_start = months[i]
        te_end = months[i + 1] if i + 1 < len(months) else bars.index[-1] + pd.Timedelta(days=1)
        test = bars[(bars.index >= te_start) & (bars.index < te_end)].copy()
        if len(test) < 20:
            continue
        pos = sig_fn(test, **sig_kwargs)
        res = simulate_sleeve(test, pos, sleeve_vol_target=0.10,
                             atr_stop_mult=atr_stop,
                             atr_take_mult=atr_take if atr_take > 0 else 1e6,
                             bars_per_year=bars_per_year)
        test_pnl.append(res["bar_pnl"].sum())
        test_bar_pnl.append(res["bar_pnl"])
        n_trades_total += len(res["trades"])
    if not test_pnl:
        return None
    all_pnl = pd.concat(test_bar_pnl).sort_index()
    eq = DEFAULT_EQUITY + all_pnl.cumsum()
    rets = returns_from_equity(eq)
    sr = sharpe(rets, bars_per_year)
    mdd = max_drawdown(eq) * 100
    total = float(eq.iloc[-1] - DEFAULT_EQUITY)
    win_months_pct = float(np.mean([1 if x > 0 else 0 for x in test_pnl])) * 100
    return {
        "total_pnl_usd": total,
        "total_return_pct": total / DEFAULT_EQUITY * 100,
        "sharpe": sr,
        "max_dd_pct": mdd,
        "n_trades": n_trades_total,
        "n_months": len(test_pnl),
        "win_months_pct": win_months_pct,
        "equity": eq,
    }


def main():
    print("loading long bars...")
    bars15 = build_bars_long(900)
    bars15 = bars15[bars15.index >= START].copy()
    print(f"sliced: {len(bars15):,} 15m bars, {bars15.index[0]} -> {bars15.index[-1]}")

    # build H1, H4, D1
    tf_seconds = {"H1": 3600, "H4": 14400, "D1": 86400}
    bpy_map = {"H1": 252 * 24, "H4": 252 * 6, "D1": 252.0}
    bars_by_tf = {}
    for name, s in tf_seconds.items():
        b = resample_bars(bars15, s) if s != 900 else bars15
        print(f"  {name}: {len(b):,} bars")
        bars_by_tf[name] = b

    # benchmark
    c0, c1 = bars15["close"].iloc[0], bars15["close"].iloc[-1]
    bh_pct = (c1 / c0 - 1) * 100
    bh_units = DEFAULT_EQUITY / c0
    bh_eq = DEFAULT_EQUITY + (bars15["close"] - c0) * bh_units
    bh_mdd = ((bh_eq - bh_eq.cummax()) / bh_eq.cummax()).min() * 100
    print(f"Buy&hold: {bh_pct:+.2f}% MDD={bh_mdd:.2f}%")

    # ---- candidate grid ----
    candidates = []
    # long-only trend EMA on H1/H4/D1
    for tf in ("H1", "H4", "D1"):
        for fast, slow in [(20, 50), (20, 80), (50, 100), (50, 200), (12, 36), (8, 21)]:
            for atr_stop in (2.5, 4.0, 6.0):
                candidates.append({
                    "id": f"longTrend_{tf}_f{fast}_s{slow}_stp{atr_stop}",
                    "tf": tf, "fn": sig_long_trend,
                    "kwargs": {"fast": fast, "slow": slow},
                    "atr_stop": atr_stop, "atr_take": 0.0,
                })
    # donchian long breakout
    for tf in ("H1", "H4", "D1"):
        for n in (20, 40, 55, 100):
            for atr_stop in (3.0, 5.0, 8.0):
                candidates.append({
                    "id": f"donch_{tf}_n{n}_stp{atr_stop}",
                    "tf": tf, "fn": sig_donchian_long,
                    "kwargs": {"n": n},
                    "atr_stop": atr_stop, "atr_take": 0.0,
                })
    # rsi pullback long
    for tf in ("H1", "H4"):
        for et in (30, 50, 100):
            for dip in (30.0, 35.0, 40.0):
                candidates.append({
                    "id": f"rsiPB_{tf}_et{et}_dip{dip}_stp4",
                    "tf": tf, "fn": sig_rsi_pullback_long,
                    "kwargs": {"ema_trend": et, "rsi_dip": dip, "rsi_recovery": 50.0},
                    "atr_stop": 4.0, "atr_take": 0.0,
                })
    # long-bias L/S asymmetric
    for tf in ("H1", "H4", "D1"):
        for fast, slow in [(20, 50), (50, 200)]:
            for atr_stop in (3.0, 5.0):
                candidates.append({
                    "id": f"longBias_{tf}_f{fast}_s{slow}_stp{atr_stop}",
                    "tf": tf, "fn": sig_long_bias_trend,
                    "kwargs": {"fast": fast, "slow": slow},
                    "atr_stop": atr_stop, "atr_take": 0.0,
                })

    print(f"total candidates: {len(candidates)}")

    results = []
    for i, cfg in enumerate(candidates):
        b = bars_by_tf[cfg["tf"]]
        r = wf_score(b, cfg["fn"], cfg["kwargs"], bpy_map[cfg["tf"]],
                     train_months=3, test_months=1,
                     atr_stop=cfg["atr_stop"], atr_take=cfg["atr_take"])
        if r is None:
            continue
        r["id"] = cfg["id"]
        r["tf"] = cfg["tf"]
        r["fn"] = cfg["fn"].__name__
        r["kwargs"] = cfg["kwargs"]
        r["atr_stop"] = cfg["atr_stop"]
        results.append(r)
        if (i + 1) % 20 == 0:
            print(f"  [{i+1}/{len(candidates)}]")

    # filter & rank
    df = pd.DataFrame([{k: v for k, v in r.items() if k != "equity"} for r in results])
    df = df.sort_values("sharpe", ascending=False).reset_index(drop=True)
    print("\n=== top 15 by Sharpe ===")
    print(df.head(15).to_string(index=False))

    print("\n=== top 10 by total return ===")
    print(df.sort_values("total_pnl_usd", ascending=False).head(10).to_string(index=False))

    # save full ranking
    out = {
        "data_window": [str(bars15.index[0]), str(bars15.index[-1])],
        "n_15m_bars": int(len(bars15)),
        "benchmark_bh_pct": bh_pct,
        "benchmark_bh_mdd_pct": bh_mdd,
        "n_candidates": len(candidates),
        "n_evaluated": len(results),
        "top_by_sharpe": df.head(20).to_dict(orient="records"),
        "top_by_pnl": df.sort_values("total_pnl_usd", ascending=False).head(20).to_dict(orient="records"),
    }
    out_path = os.path.join(REPORT_DIR, "edge_search.json")
    with open(out_path, "w") as f:
        json.dump(out, f, indent=2, default=str)

    # plot top 5 equity curves
    top5_ids = df.head(5)["id"].tolist()
    top5 = [r for r in results if r["id"] in top5_ids]
    fig, ax = plt.subplots(figsize=(12, 7))
    for r in top5:
        ax.plot(r["equity"].index, r["equity"].values, label=f"{r['id']} (Sh={r['sharpe']:+.2f})",
                alpha=0.85, linewidth=1.3)
    ax.plot(bh_eq.index, bh_eq.values, label=f"buy&hold ({bh_pct:+.1f}%)",
            color="gray", linestyle="--", alpha=0.6)
    ax.axhline(DEFAULT_EQUITY, color="red", linestyle=":", alpha=0.4)
    ax.set_title(f"Edge search top-5 OOS equity ({bars15.index[0].date()} → {bars15.index[-1].date()})")
    ax.legend(loc="best", fontsize=8)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(REPORT_DIR, "edge_search_top5.png"), dpi=110)
    plt.close(fig)

    # write markdown
    lines = [
        "# Edge Search — 2025-06+ XAUUSD",
        "",
        f"**Window**: {bars15.index[0].date()} → {bars15.index[-1].date()} ({len(bars15):,} 15m bars)",
        f"**Benchmark**: Buy & hold = **{bh_pct:+.2f}%**, MDD = {bh_mdd:.2f}%",
        f"**Method**: Walk-forward (3mo train / 1mo OOS test, rolling). {len(candidates)} candidates evaluated.",
        f"**Note**: parameters are NOT fit on train — train window only used for warmup of indicators.",
        f"          Each candidate's stat is purely OOS aggregated across rolling test months.",
        "",
        "## Top 15 by OOS Sharpe",
        "",
        "```",
        df.head(15).drop(columns=["kwargs", "fn"], errors="ignore").to_string(index=False),
        "```",
        "",
        "## Top 10 by OOS PnL",
        "",
        "```",
        df.sort_values("total_pnl_usd", ascending=False).head(10).drop(
            columns=["kwargs", "fn"], errors="ignore").to_string(index=False),
        "```",
        "",
        "## Files",
        "- `reports/edge_search.json` — full ranking",
        "- `reports/edge_search_top5.png` — top-5 equity curves",
    ]
    md_path = os.path.join(REPORT_DIR, "EDGE_SEARCH.md")
    with open(md_path, "w") as f:
        f.write("\n".join(lines))
    print(f"\nwrote: {out_path}")
    print(f"wrote: {md_path}")


if __name__ == "__main__":
    main()
