"""Verify top edge candidates on out-of-window data (2024-03 → 2025-05).
If edge holds across regimes it's real. If only works in 2025-06+ it's regime-specific."""
from __future__ import annotations
import os
import sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(__file__))
from duka_xau_bar_builder import build_bars_long
from gold_portfolio_sim import simulate_sleeve, DEFAULT_EQUITY
from gold_metrics import report, returns_from_equity, sharpe, max_drawdown
from gold_edge_search import resample_bars, sig_long_trend, sig_donchian_long, wf_score

REPORT_DIR = "/Users/jo/omega_repo/outputs/gold_trend_ensemble"


def main():
    bars15 = build_bars_long(900)
    print(f"full: {len(bars15):,} bars  {bars15.index[0]} -> {bars15.index[-1]}")

    windows = [
        ("2024-03 → 2025-05 (early)", pd.Timestamp("2024-03-01", tz="UTC"), pd.Timestamp("2025-06-01", tz="UTC")),
        ("2025-06 → 2026-04 (search)", pd.Timestamp("2025-06-01", tz="UTC"), pd.Timestamp("2027-01-01", tz="UTC")),
        ("full 2024-03 → 2026-04", pd.Timestamp("2024-03-01", tz="UTC"), pd.Timestamp("2027-01-01", tz="UTC")),
    ]

    # top 5 from edge_search
    configs = [
        ("longTrend_H1_f20_s80_stp4.0",  "H1",  sig_long_trend,    {"fast": 20, "slow": 80},   4.0),
        ("longTrend_H1_f50_s100_stp2.5", "H1",  sig_long_trend,    {"fast": 50, "slow": 100},  2.5),
        ("longTrend_H1_f20_s50_stp4.0",  "H1",  sig_long_trend,    {"fast": 20, "slow": 50},   4.0),
        ("longTrend_H4_f8_s21_stp2.5",   "H4",  sig_long_trend,    {"fast": 8, "slow": 21},    2.5),
        ("longTrend_H1_f12_s36_stp4.0",  "H1",  sig_long_trend,    {"fast": 12, "slow": 36},   4.0),
        ("donch_H1_n40_stp5.0",          "H1",  sig_donchian_long, {"n": 40},                  5.0),
    ]
    tf_seconds = {"H1": 3600, "H4": 14400}
    bpy_map = {"H1": 252 * 24, "H4": 252 * 6}

    print(f"\n{'config':<35} {'window':<32} {'pnl':>10} {'ret%':>7} {'Sh':>6} {'MDD%':>7} {'wmo%':>6} {'trd':>5}")
    print("-" * 110)
    rows = []
    equity_curves = {}
    for label, fn, kwargs, stop in [(c[0], c[2], c[3], c[4]) for c in configs]:
        tf = next(c[1] for c in configs if c[0] == label)
        for wname, ws, we in windows:
            sub15 = bars15[(bars15.index >= ws) & (bars15.index < we)]
            if len(sub15) < 100:
                continue
            sub = resample_bars(sub15, tf_seconds[tf])
            r = wf_score(sub, fn, kwargs, bpy_map[tf], train_months=3, test_months=1,
                        atr_stop=stop, atr_take=0.0)
            if r is None:
                continue
            print(f"{label:<35} {wname:<32} ${r['total_pnl_usd']:>9,.0f} "
                  f"{r['total_return_pct']:>6.2f} {r['sharpe']:>6.2f} {r['max_dd_pct']:>7.2f} "
                  f"{r['win_months_pct']:>5.1f} {r['n_trades']:>5}")
            rows.append({"config": label, "window": wname, **{k: v for k, v in r.items() if k != "equity"}})
            equity_curves[(label, wname)] = r["equity"]

    # equity plot — full-window of each top config
    fig, ax = plt.subplots(figsize=(12, 7))
    bh = bars15["close"]
    bh_eq = DEFAULT_EQUITY * bh / bh.iloc[0]
    ax.plot(bh_eq.index, bh_eq.values, color="gray", linestyle="--", alpha=0.5,
            label=f"buy&hold ({(bh.iloc[-1]/bh.iloc[0]-1)*100:+.1f}%)")
    for cfg_label, _, _, _, _ in configs:
        key = (cfg_label, "full 2024-03 → 2026-04")
        if key in equity_curves:
            eq = equity_curves[key]
            ax.plot(eq.index, eq.values, label=cfg_label, alpha=0.85, linewidth=1.3)
    ax.axhline(DEFAULT_EQUITY, color="red", linestyle=":", alpha=0.4)
    ax.set_title("Edge robustness — full-period walk-forward equity")
    ax.legend(loc="best", fontsize=8)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(REPORT_DIR, "edge_verify_full.png"), dpi=110)
    plt.close(fig)

    df = pd.DataFrame(rows)
    df.to_csv(os.path.join(REPORT_DIR, "edge_verify.csv"), index=False)
    print(f"\nwrote: {os.path.join(REPORT_DIR, 'edge_verify.csv')}")
    print(f"wrote: {os.path.join(REPORT_DIR, 'edge_verify_full.png')}")


if __name__ == "__main__":
    main()
