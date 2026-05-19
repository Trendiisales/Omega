"""Full backtest driver: run all 4 sleeves on 5m bars, build portfolio,
single hold-out (train May 8-13, test May 14-15), full metric report."""
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
from l2_xau_loader import get_bars
from gold_sleeves import SLEEVE_FNS
from gold_portfolio_sim import simulate_sleeve, combine_sleeves, DEFAULT_EQUITY
from gold_metrics import report, BARS_PER_YEAR_5M, returns_from_equity, sharpe

REPORT_DIR = "/Users/jo/omega_repo/outputs/gold_trend_ensemble"
os.makedirs(REPORT_DIR, exist_ok=True)


def filter_dense_days(bars: pd.DataFrame, min_bars_per_day: int = 50) -> pd.DataFrame:
    g = bars.groupby(bars.index.date).size()
    keep = set(g[g >= min_bars_per_day].index)
    return bars[bars.index.map(lambda t: t.date() in keep)].copy()


def run_sleeve(name: str, bars: pd.DataFrame, fn) -> dict:
    pos = fn(bars)
    res = simulate_sleeve(bars, pos, sleeve_vol_target=0.08,
                          atr_stop_mult=2.5, atr_take_mult=4.0,
                          bars_per_year=BARS_PER_YEAR_5M)
    res["pos"] = pos
    return res


def fmt(v):
    if isinstance(v, float):
        return f"{v:.3f}" if abs(v) < 1000 else f"{v:,.2f}"
    return str(v)


def md_table(rows: list[dict], cols: list[str]) -> str:
    out = ["| " + " | ".join(cols) + " |", "| " + " | ".join(["---"] * len(cols)) + " |"]
    for r in rows:
        out.append("| " + " | ".join(fmt(r.get(c, "")) for c in cols) + " |")
    return "\n".join(out)


def run_one_tf(seconds: int, bars_per_year: float, label: str):
    bars = get_bars(seconds)
    bars = filter_dense_days(bars, min_bars_per_day=max(10, 240 // (seconds // 60)))
    if len(bars) < 60:
        return None
    cut = int(len(bars) * 0.75)
    train, test = bars.iloc[:cut], bars.iloc[cut:]
    sleeve_reports = []
    train_results, test_results = {}, {}
    for name, fn in SLEEVE_FNS.items():
        pos = fn(train)
        tr = simulate_sleeve(train, pos, sleeve_vol_target=0.08, atr_stop_mult=2.5,
                             atr_take_mult=4.0, bars_per_year=bars_per_year)
        pos_t = fn(test)
        te = simulate_sleeve(test, pos_t, sleeve_vol_target=0.08, atr_stop_mult=2.5,
                             atr_take_mult=4.0, bars_per_year=bars_per_year)
        train_results[name], test_results[name] = tr, te
        sleeve_reports.append({"tf": label, "split": "train",
                               **report(f"{name}_train", tr["equity"], tr["bar_pnl"],
                                        tr["trades"], sessions=train["session"],
                                        bars_per_year=bars_per_year)})
        sleeve_reports.append({"tf": label, "split": "test",
                               **report(f"{name}_test", te["equity"], te["bar_pnl"],
                                        te["trades"], n_trials=4, sessions=test["session"],
                                        bars_per_year=bars_per_year)})
    return {
        "tf": label, "bars_per_year": bars_per_year,
        "train": train, "test": test,
        "train_results": train_results, "test_results": test_results,
        "sleeve_reports": sleeve_reports,
    }


def pick_weights(train_results, sleeve_reports, tf_label):
    keep = {}
    for n, r in train_results.items():
        tr_rep = next((x for x in sleeve_reports
                       if x["tf"] == tf_label and x["split"] == "train" and x["name"].startswith(n + "_")), None)
        if tr_rep is None:
            continue
        if tr_rep.get("total_pnl_usd", -1) <= 0 or tr_rep.get("n_trades", 0) < 5:
            continue
        ret = returns_from_equity(r["equity"])
        v = ret.std()
        keep[n] = v if v > 0 else 1e-9
    if not keep:
        return {k: 0.0 for k in train_results}
    inv = {k: 1.0 / v for k, v in keep.items()}
    s = sum(inv.values())
    w = {k: min(v / s, 0.45) for k, v in inv.items()}
    tot = sum(w.values())
    if tot > 0:
        w = {k: v / tot for k, v in w.items()}
    for k in train_results:
        w.setdefault(k, 0.0)
    return w


def main():
    print("loading bars...")
    bars5 = get_bars(300)
    bars5 = filter_dense_days(bars5, min_bars_per_day=50)
    days = sorted(set(bars5.index.date))
    print(f"dense days: {days}, total 5m bars: {len(bars5):,}")

    runs = {}
    for seconds, label, bpy in [(300, "5m", 72576.0), (900, "15m", 24192.0)]:
        r = run_one_tf(seconds, bpy, label)
        if r is not None:
            runs[label] = r
            print(f"[{label}] train={len(r['train'])} test={len(r['test'])}")

    # build per-TF portfolios
    portfolios = {}
    for label, r in runs.items():
        w = pick_weights(r["train_results"], r["sleeve_reports"], label)
        print(f"[{label}] weights: { {k: round(v, 3) for k, v in w.items()} }")
        pt = combine_sleeves(r["train_results"], w)
        pe = combine_sleeves(r["test_results"], w)
        pt["equity"] = DEFAULT_EQUITY + pt["bar_pnl"].cumsum()
        pe["equity"] = DEFAULT_EQUITY + pe["bar_pnl"].cumsum()
        portfolios[label] = {
            "weights": w,
            "train": pt, "test": pe,
            "train_rep": report(f"portfolio_train_{label}", pt["equity"], pt["bar_pnl"],
                                pt["trades"], sessions=r["train"]["session"],
                                bars_per_year=r["bars_per_year"]),
            "test_rep": report(f"portfolio_test_{label}", pe["equity"], pe["bar_pnl"],
                              pe["trades"], n_trials=4, sessions=r["test"]["session"],
                              bars_per_year=r["bars_per_year"]),
        }

    # use 5m as primary for back-compat with existing report layout
    train_sleeve_results = runs["5m"]["train_results"]
    test_sleeve_results = runs["5m"]["test_results"]
    train = runs["5m"]["train"]
    test = runs["5m"]["test"]
    sleeve_reports = []
    for r in runs.values():
        sleeve_reports.extend(r["sleeve_reports"])
    w = portfolios["5m"]["weights"]
    port_train = portfolios["5m"]["train"]
    port_test = portfolios["5m"]["test"]
    port_rep_tr = portfolios["5m"]["train_rep"]
    port_rep_te = portfolios["5m"]["test_rep"]

    # write JSON
    out_path = os.path.join(REPORT_DIR, "results.json")
    summary = {
        "split": {
            "train_start": str(train.index[0]),
            "train_end": str(train.index[-1]),
            "test_start": str(test.index[0]),
            "test_end": str(test.index[-1]),
            "train_bars": len(train),
            "test_bars": len(test),
        },
        "timeframes": list(runs.keys()),
        "weights_by_tf": {lbl: p["weights"] for lbl, p in portfolios.items()},
        "sleeves": [
            {k: v for k, v in r.items() if k != "session_breakdown"}
            for r in sleeve_reports
        ],
        "portfolio_by_tf": {
            lbl: {
                "train": {k: v for k, v in p["train_rep"].items() if k != "session_breakdown"},
                "test": {k: v for k, v in p["test_rep"].items() if k != "session_breakdown"},
            }
            for lbl, p in portfolios.items()
        },
    }
    with open(out_path, "w") as f:
        json.dump(summary, f, indent=2, default=str)

    # equity curve PNG
    fig, ax = plt.subplots(2, 1, figsize=(11, 8), sharex=False)
    for name, r in train_sleeve_results.items():
        ax[0].plot(r["equity"].index, r["equity"].values, label=name, alpha=0.7)
    ax[0].plot(port_train["equity"].index, port_train["equity"].values,
               label="portfolio", color="black", linewidth=2)
    ax[0].set_title("Train: equity by sleeve")
    ax[0].legend(loc="best", fontsize=8)
    ax[0].grid(True, alpha=0.3)
    for name, r in test_sleeve_results.items():
        ax[1].plot(r["equity"].index, r["equity"].values, label=name, alpha=0.7)
    ax[1].plot(port_test["equity"].index, port_test["equity"].values,
               label="portfolio", color="black", linewidth=2)
    ax[1].set_title("Test: equity by sleeve (out-of-sample)")
    ax[1].legend(loc="best", fontsize=8)
    ax[1].grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(REPORT_DIR, "equity.png"), dpi=110)
    plt.close(fig)

    # write Markdown report
    md_lines = [
        "# Gold Engine — Backtest Results",
        "",
        f"**Data**: XAUUSD L2 ticks (Downloads/l2_ticks_XAUUSD_2026-05-*.csv)",
        f"**Bars**: 5-minute, dense-day filtered",
        f"**Train**: {summary['split']['train_start']} → {summary['split']['train_end']} ({summary['split']['train_bars']} bars)",
        f"**Test**:  {summary['split']['test_start']} → {summary['split']['test_end']} ({summary['split']['test_bars']} bars)",
        f"**Costs**: bid/ask exec on entry/exit (real spread per tick, mean ~0.51 pts).",
        f"**Sizing**: 1% risk-of-equity per trade × vol-target scalar (target 8% ann.), ATR(14) stops (2.5x), takes (4.0x).",
        "",
        "## Portfolio weights (inverse-vol, fitted on train)",
        "",
        md_table([{"sleeve": k, "weight": v} for k, v in w.items()],
                 ["sleeve", "weight"]),
        "",
        "## Per-sleeve metrics",
        "",
        md_table(
            sleeve_reports,
            ["name", "split", "total_return_pct", "sharpe", "sortino", "max_dd_pct",
             "cagr_pct", "n_trades", "hit_rate_pct", "payoff", "avg_trade_pnl"],
        ),
        "",
        "## Portfolio metrics (5m + 15m)",
        "",
        md_table(
            [portfolios[lbl][f"{split}_rep"] for lbl in portfolios for split in ("train", "test")],
            ["name", "total_return_pct", "sharpe", "sortino", "max_dd_pct",
             "cagr_pct", "bars"],
        ),
        "",
        "## Sleeve descriptions",
        "",
        "- **trend**: multi-horizon EMA20/EMA80 cross with slope z-score filter (>0.4), vol-managed (skip top-8% RV).",
        "- **pullback**: H-of-day session-gated (London + NY overlap), RSI-dip + EMA10 reclaim, EMA50 trend filter.",
        "- **meanrev**: VWAP z-score fade (|z|>2.0), only in regime==0 AND |ewm_drift|<1.2.",
        "- **micro**: pre-engineered features (l2_imb >0.62 or <0.38, ewm_drift align, vpin <0.13), London/NY only.",
        "",
        "## Caveats",
        "",
        "- Data window very short: ~6 dense trading days. Numbers are directional, not definitive.",
        "- Raw L2 depth columns (depth_*, l2_*_vol) all zero in source files — sleeves use pre-computed features only.",
        "- Test = last 25% of dense data (single hold-out). Walk-forward needs more days.",
        "- Costs include displayed bid/ask spread only — no slippage, financing, or market-impact model.",
        "- Vol-target & DSR assume 5m-bars-per-year = 72576; recalibrate for live horizons.",
        "",
        "## Files",
        "",
        "- `reports/results.json` — machine-readable summary",
        "- `reports/equity.png` — equity curves",
        "- `src/loader.py` — tick → bar pipeline",
        "- `src/sleeves.py` — 4 signal generators",
        "- `src/portfolio.py` — bid/ask simulator + vol-target sizing",
        "- `src/metrics.py` — Sharpe/Sortino/MDD/CAGR/Deflated SR",
    ]
    md_path = os.path.join(REPORT_DIR, "RESULTS.md")
    with open(md_path, "w") as f:
        f.write("\n".join(md_lines))

    print(f"\n--- portfolio test ---")
    for k, v in port_rep_te.items():
        if k == "session_breakdown":
            print("  session_breakdown:")
            print(v.to_string())
        else:
            print(f"  {k}: {v}")

    print(f"\nwrote: {out_path}")
    print(f"wrote: {md_path}")
    print(f"wrote: {os.path.join(REPORT_DIR, 'equity.png')}")


if __name__ == "__main__":
    main()
