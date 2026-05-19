"""Run deployable ensemble strategy.

Usage:
  python3 src/run_strategy.py              # full backtest + current position on 2y data
  python3 src/run_strategy.py --live       # only current position decision
  python3 src/run_strategy.py --since 2025-01-01   # backtest from date
  python3 src/run_strategy.py --csv path.csv       # use external tick CSV (cols: timestamp,askPrice,bidPrice)
"""
from __future__ import annotations
import argparse
import json
import os
import sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(__file__))
from duka_xau_bar_builder import build_bars_long
from gold_trend_ensemble_strategy import Strategy, SUB_STRATEGIES, BPY_H1
from gold_portfolio_sim import DEFAULT_EQUITY
from gold_edge_search import resample_bars

REPORT_DIR = "/Users/jo/omega_repo/outputs/gold_trend_ensemble"


def build_h1_h4(bars15: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
    h1 = resample_bars(bars15, 3600)
    h4 = resample_bars(bars15, 14400)
    return h1, h4


def fmt_money(x):
    return f"${x:+,.2f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--live", action="store_true", help="only emit current decision, no backtest")
    ap.add_argument("--since", type=str, default=None, help="backtest start date YYYY-MM-DD")
    ap.add_argument("--equity", type=float, default=DEFAULT_EQUITY)
    ap.add_argument("--vol-target", type=float, default=0.10)
    args = ap.parse_args()

    print("loading bars...")
    bars15 = build_bars_long(900)
    if args.since:
        cutoff = pd.Timestamp(args.since, tz="UTC")
        bars15 = bars15[bars15.index >= cutoff]
    print(f"bars: {len(bars15):,}  {bars15.index[0]} -> {bars15.index[-1]}")

    h1, h4 = build_h1_h4(bars15)
    print(f"H1 bars: {len(h1):,}, H4 bars: {len(h4):,}")

    strat = Strategy(vol_target_per_sub=args.vol_target)

    # --- live decision ---
    decision = strat.current_position(h1, h4)
    print("\n=== CURRENT TARGET POSITION ===")
    print(f"as of: {decision['as_of']}")
    print(f"ensemble long-share: {decision['ensemble_long_share']*100:.1f}% (1/3 per sub)")
    print(f"\n{'sub':<22} {'tf':<3} {'pos':>4} {'last_close':>11} {'atr':>6} {'stop':>11}  bar_ts")
    print("-" * 88)
    for d in decision["decisions"]:
        stop = f"{d['suggested_stop']:.2f}" if d["suggested_stop"] is not None else "—"
        print(f"{d['sub']:<22} {d['tf']:<3} {d['target_position']:>4} "
              f"{d['last_close']:>11.2f} {d['atr']:>6.2f} {stop:>11}  {d['last_bar_ts']}")

    if args.live:
        live_path = os.path.join(REPORT_DIR, "live_decision.json")
        with open(live_path, "w") as f:
            json.dump(decision, f, indent=2, default=str)
        print(f"\nwrote: {live_path}")
        return

    # --- backtest ---
    print("\nrunning ensemble backtest...")
    result = strat.run(h1, h4)
    m = result["metrics"]
    print("\n=== ENSEMBLE METRICS ===")
    for k in ("total_pnl_usd", "total_return_pct", "cagr_pct", "sharpe", "sortino",
              "max_dd_pct", "bars"):
        v = m.get(k)
        if v is None:
            continue
        if isinstance(v, float):
            print(f"  {k:20s} {v:>+12.3f}")
        else:
            print(f"  {k:20s} {v}")

    print("\n=== PER-SUB ===")
    print(f"{'sub':<22} {'pnl':>11} {'return %':>9} {'Sh':>6} {'MDD %':>7} {'trd':>5}  weight")
    print("-" * 80)
    weights = result["weights"]
    for name, r in result["subs"].items():
        eq = r["equity"]
        ret = (eq.iloc[-1] / eq.iloc[0] - 1) * 100
        rets = eq.pct_change().fillna(0.0)
        sr = rets.mean() / rets.std() * np.sqrt(r["sub"].bars_per_year) if rets.std() > 0 else 0.0
        peak = eq.cummax()
        mdd = ((eq - peak) / peak).min() * 100
        n_trades = len(r["trades"])
        print(f"{name:<22} {fmt_money(eq.iloc[-1]-eq.iloc[0]):>11} {ret:>+9.2f} "
              f"{sr:>+6.2f} {mdd:>+7.2f} {n_trades:>5}  {weights[name]:.3f}")

    # benchmark
    bh_start = h1["close"].iloc[0]
    bh_end = h1["close"].iloc[-1]
    bh_pct = (bh_end / bh_start - 1) * 100
    bh_units = args.equity / bh_start
    bh_eq = args.equity + (h1["close"] - bh_start) * bh_units
    bh_peak = bh_eq.cummax()
    bh_mdd = ((bh_eq - bh_peak) / bh_peak).min() * 100
    print(f"\nBuy & hold over same window: {bh_pct:+.2f}%  MDD={bh_mdd:+.2f}%")

    # plot
    fig, ax = plt.subplots(2, 1, figsize=(12, 8))
    ax[0].plot(result["ensemble_equity"].index, result["ensemble_equity"].values,
               label="ensemble", color="black", linewidth=1.6)
    for name, r in result["subs"].items():
        eq_on_h1 = r["equity"].reindex(h1.index, method="pad")
        ax[0].plot(eq_on_h1.index, eq_on_h1.values, label=name, alpha=0.55, linewidth=1.0)
    ax[0].plot(bh_eq.index, bh_eq.values, label=f"buy&hold ({bh_pct:+.1f}%)",
               color="gray", linestyle="--", alpha=0.55)
    ax[0].axhline(args.equity, color="red", linestyle=":", alpha=0.4)
    ax[0].set_title("Deployable Strategy — ensemble equity vs subs vs B&H")
    ax[0].legend(loc="best", fontsize=8)
    ax[0].grid(True, alpha=0.3)
    # drawdown
    eq = result["ensemble_equity"]
    dd = (eq - eq.cummax()) / eq.cummax() * 100
    ax[1].fill_between(dd.index, dd.values, 0, color="red", alpha=0.45)
    ax[1].set_title("Ensemble drawdown (%)")
    ax[1].grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(REPORT_DIR, "strategy_equity.png"), dpi=110)
    plt.close(fig)

    # write live decision + backtest summary
    out = {
        "data_window": [str(h1.index[0]), str(h1.index[-1])],
        "bars": {"H1": len(h1), "H4": len(h4)},
        "ensemble_metrics": {k: v for k, v in m.items() if k != "session_breakdown"},
        "weights": weights,
        "benchmark_bh": {"return_pct": bh_pct, "max_dd_pct": bh_mdd},
        "live_decision": decision,
    }
    out_path = os.path.join(REPORT_DIR, "strategy_run.json")
    with open(out_path, "w") as f:
        json.dump(out, f, indent=2, default=str)
    print(f"\nwrote: {out_path}")
    print(f"wrote: {os.path.join(REPORT_DIR, 'strategy_equity.png')}")


if __name__ == "__main__":
    main()
