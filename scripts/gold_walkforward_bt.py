"""Long-window walk-forward backtest using 2y XAUUSD Duka ticks.
Rolling: 4mo train, 1mo test, step 1mo. Sleeves: trend, pullback, meanrev (no micro features)."""
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
from duka_xau_bar_builder import build_bars_long
from gold_sleeves import sleeve_trend, sleeve_pullback, sleeve_meanrev
from gold_portfolio_sim import simulate_sleeve, combine_sleeves, DEFAULT_EQUITY
from gold_metrics import report, returns_from_equity

SLEEVES = {"trend": sleeve_trend, "pullback": sleeve_pullback, "meanrev": sleeve_meanrev}
REPORT_DIR = "/Users/jo/omega_repo/outputs/gold_trend_ensemble"
BARS_PER_YEAR_15M = 252 * 24 * 4  # = 24192


def month_starts(idx: pd.DatetimeIndex) -> list:
    return sorted(set(pd.Timestamp(year=t.year, month=t.month, day=1, tz="UTC")
                     for t in idx))


def slice_window(bars: pd.DataFrame, start: pd.Timestamp, end: pd.Timestamp) -> pd.DataFrame:
    return bars[(bars.index >= start) & (bars.index < end)].copy()


def pick_weights(train_results, train_reports, mode: str = "always_on"):
    """mode='always_on': inverse-vol weight all sleeves regardless of train pnl.
    mode='gated': only weight sleeves with positive train pnl + >=5 trades."""
    if mode == "gated":
        keep = {}
        for n, r in train_results.items():
            tr_rep = next((x for x in train_reports if x["name"].startswith(n + "_")), None)
            if tr_rep is None or tr_rep.get("total_pnl_usd", -1) <= 0 or tr_rep.get("n_trades", 0) < 5:
                continue
            v = returns_from_equity(r["equity"]).std()
            keep[n] = v if v > 0 else 1e-9
    else:
        keep = {}
        for n, r in train_results.items():
            v = returns_from_equity(r["equity"]).std()
            keep[n] = v if v > 0 else 1e-9
    if not keep:
        return {k: 0.0 for k in train_results}
    inv = {k: 1.0 / v for k, v in keep.items()}
    s = sum(inv.values())
    w = {k: min(v / s, 0.5) for k, v in inv.items()}
    tot = sum(w.values())
    if tot > 0:
        w = {k: v / tot for k, v in w.items()}
    for k in train_results:
        w.setdefault(k, 0.0)
    return w


def main():
    print("loading long bars...")
    bars = build_bars_long(900)
    print(f"bars: {len(bars):,}, {bars.index[0]} -> {bars.index[-1]}")

    months = month_starts(bars.index)
    print(f"months in data: {len(months)}")
    if len(months) < 6:
        print("not enough months")
        return

    fold_records = []
    fold_test_equities = []  # to stitch a continuous OOS curve
    sleeve_oos_pnl = {k: [] for k in SLEEVES}
    weight_log = []

    TRAIN_MONTHS = 4
    for i in range(TRAIN_MONTHS, len(months) - 1):
        tr_start, tr_end = months[i - TRAIN_MONTHS], months[i]
        te_start, te_end = months[i], months[i + 1]
        train = slice_window(bars, tr_start, tr_end)
        test = slice_window(bars, te_start, te_end)
        if len(train) < 200 or len(test) < 20:
            continue

        train_results, test_results = {}, {}
        train_reports = []
        for name, fn in SLEEVES.items():
            pos = fn(train)
            tr = simulate_sleeve(train, pos, sleeve_vol_target=0.08, atr_stop_mult=2.5,
                                atr_take_mult=4.0, bars_per_year=BARS_PER_YEAR_15M)
            pos_t = fn(test)
            te = simulate_sleeve(test, pos_t, sleeve_vol_target=0.08, atr_stop_mult=2.5,
                                atr_take_mult=4.0, bars_per_year=BARS_PER_YEAR_15M)
            train_results[name] = tr
            test_results[name] = te
            tr_rep = report(f"{name}_tr", tr["equity"], tr["bar_pnl"], tr["trades"],
                            bars_per_year=BARS_PER_YEAR_15M)
            train_reports.append(tr_rep)
            te_rep = report(f"{name}_te", te["equity"], te["bar_pnl"], te["trades"],
                            bars_per_year=BARS_PER_YEAR_15M, n_trials=3)
            sleeve_oos_pnl[name].append(te_rep["total_pnl_usd"])

        w = pick_weights(train_results, train_reports, mode=os.environ.get("WEIGHT_MODE", "always_on"))
        weight_log.append({"test_month": str(te_start.date()), **w})
        port = combine_sleeves(test_results, w)
        port_pnl = port["bar_pnl"].sum()
        port_rep = report(f"port_{te_start.date()}", DEFAULT_EQUITY + port["bar_pnl"].cumsum(),
                          port["bar_pnl"], port.get("trades"),
                          bars_per_year=BARS_PER_YEAR_15M)
        fold_records.append({
            "train": f"{tr_start.date()}..{tr_end.date()}",
            "test": str(te_start.date()),
            "train_bars": len(train), "test_bars": len(test),
            "weights": w,
            "test_pnl_usd": port_pnl,
            "test_return_pct": port_rep["total_return_pct"],
            "test_sharpe": port_rep["sharpe"],
            "test_max_dd_pct": port_rep["max_dd_pct"],
            "test_n_trades": port_rep.get("n_trades", 0),
            "test_hit_pct": port_rep.get("hit_rate_pct", 0),
            "test_payoff": port_rep.get("payoff", 0),
        })
        fold_test_equities.append(port["bar_pnl"])
        print(f"fold {te_start.date()}: pnl={port_pnl:>+9.2f} sh={port_rep['sharpe']:>+6.2f} "
              f"mdd={port_rep['max_dd_pct']:>+6.2f}% trades={port_rep.get('n_trades', 0)} w={w}")

    # stitch continuous OOS equity
    if fold_test_equities:
        oos = pd.concat(fold_test_equities).sort_index()
        oos_eq = DEFAULT_EQUITY + oos.cumsum()
        oos_rep = report("oos_total", oos_eq, oos, bars_per_year=BARS_PER_YEAR_15M)

        fig, ax = plt.subplots(2, 1, figsize=(12, 8))
        ax[0].plot(oos_eq.index, oos_eq.values, color="black", linewidth=1.2)
        ax[0].set_title("Stitched OOS equity (rolling 4mo-train / 1mo-test, 15m bars)")
        ax[0].grid(True, alpha=0.3)
        ax[0].axhline(DEFAULT_EQUITY, color="red", linestyle=":", alpha=0.5)
        peak = oos_eq.cummax()
        dd = (oos_eq - peak) / peak * 100
        ax[1].fill_between(dd.index, dd.values, 0, color="red", alpha=0.5)
        ax[1].set_title("OOS drawdown (%)")
        ax[1].grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(REPORT_DIR, "oos_equity_long.png"), dpi=110)
        plt.close(fig)
    else:
        oos_rep = {}

    # per-sleeve OOS aggregate
    sleeve_oos_summary = {k: {
        "sum_pnl": float(sum(v)),
        "n_folds": len(v),
        "win_folds_pct": float(np.mean([1 if x > 0 else 0 for x in v])) * 100 if v else 0,
    } for k, v in sleeve_oos_pnl.items()}

    out = {
        "bars_range": [str(bars.index[0]), str(bars.index[-1])],
        "n_bars": len(bars),
        "tf": "15m",
        "train_months": TRAIN_MONTHS,
        "n_folds": len(fold_records),
        "oos_summary": {k: v for k, v in oos_rep.items() if k != "session_breakdown"} if oos_rep else {},
        "fold_records": fold_records,
        "weight_log": weight_log,
        "sleeve_oos_summary": sleeve_oos_summary,
    }
    with open(os.path.join(REPORT_DIR, "results_long.json"), "w") as f:
        json.dump(out, f, indent=2, default=str)

    # markdown
    lines = [
        "# Gold Engine — Long-Window Walk-Forward Results",
        "",
        f"**Data**: XAUUSD Duka tick stream, 154M ticks, {bars.index[0]} → {bars.index[-1]}",
        f"**Bars**: 15-minute ({len(bars):,} bars total)",
        f"**Walk-forward**: rolling {TRAIN_MONTHS}-month train, 1-month test, step 1 month",
        f"**Folds executed**: {len(fold_records)}",
        f"**Sleeves**: trend, pullback, meanrev (no L2/micro features in this stream)",
        f"**Costs**: bid/ask exec on entry/exit, real spread (mean ~0.56 pts)",
        "",
        "## Stitched OOS portfolio",
        "",
    ]
    if oos_rep:
        lines += [
            f"- Total OOS PnL: **${oos_rep['total_pnl_usd']:+,.2f}** on $100k = **{oos_rep['total_return_pct']:+.2f}%**",
            f"- CAGR: **{oos_rep['cagr_pct']:+.2f}%**",
            f"- Sharpe: **{oos_rep['sharpe']:+.2f}**",
            f"- Sortino: **{oos_rep['sortino']:+.2f}**",
            f"- Max drawdown: **{oos_rep['max_dd_pct']:+.2f}%**",
            "",
        ]
    lines += [
        "## Per-sleeve aggregate OOS",
        "",
        "| sleeve | sum OOS PnL | folds | win folds % |",
        "| --- | --- | --- | --- |",
    ]
    for k, v in sleeve_oos_summary.items():
        lines.append(f"| {k} | ${v['sum_pnl']:+,.2f} | {v['n_folds']} | {v['win_folds_pct']:.1f} |")
    lines += ["", "## Per-fold detail (last 12 folds)", ""]
    lines.append("| test month | pnl | return % | sharpe | mdd % | trades | hit % | payoff | weights |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in fold_records[-12:]:
        w = ", ".join(f"{k}={v:.2f}" for k, v in r["weights"].items() if v > 0) or "none"
        lines.append(f"| {r['test']} | ${r['test_pnl_usd']:+,.2f} | {r['test_return_pct']:+.2f} | "
                     f"{r['test_sharpe']:+.2f} | {r['test_max_dd_pct']:+.2f} | "
                     f"{r['test_n_trades']} | {r['test_hit_pct']:.1f} | "
                     f"{r['test_payoff']:.2f} | {w} |")
    lines += [
        "",
        "## Files",
        "- `reports/results_long.json` — full JSON",
        "- `reports/oos_equity_long.png` — stitched OOS equity curve & drawdown",
    ]
    with open(os.path.join(REPORT_DIR, "RESULTS_LONG.md"), "w") as f:
        f.write("\n".join(lines))

    print(f"\nwrote: {os.path.join(REPORT_DIR, 'results_long.json')}")
    print(f"wrote: {os.path.join(REPORT_DIR, 'RESULTS_LONG.md')}")
    if oos_rep:
        print(f"\nOOS total: ${oos_rep['total_pnl_usd']:+,.2f}  Sh={oos_rep['sharpe']:+.2f}  MDD={oos_rep['max_dd_pct']:+.2f}%")
    # buy & hold benchmark over OOS period
    if fold_test_equities:
        oos_start = fold_test_equities[0].index[0]
        oos_end = fold_test_equities[-1].index[-1]
        bh = bars.loc[oos_start:oos_end]
        if len(bh) > 1:
            bh_units = DEFAULT_EQUITY / bh["close"].iloc[0]
            bh_pnl = (bh["close"].iloc[-1] - bh["close"].iloc[0]) * bh_units
            bh_pct = (bh["close"].iloc[-1] / bh["close"].iloc[0] - 1) * 100
            bh_eq = DEFAULT_EQUITY + (bh["close"] - bh["close"].iloc[0]) * bh_units
            bh_dd = ((bh_eq - bh_eq.cummax()) / bh_eq.cummax()).min() * 100
            print(f"Buy & hold over same window: ${bh_pnl:+,.2f}  ({bh_pct:+.2f}%)  MDD={bh_dd:+.2f}%")


if __name__ == "__main__":
    main()
