"""
C6 #1C Step 3c.2 — Deduped portfolio metrics for the gated Edge A + Edge B
portfolio.

Reads the regime_diagnostic_journal.csv produced by Step 3 pre-flight
(regime_diagnostic.py) and computes joint-portfolio statistics for the two
deployable survivor edges:
    Edge A: pid 57226 SHORT XAUUSD b5 (deduped from 57226/57230)
    Edge B: pid 51274 LONG  XAUUSD b5

Computes:
    1. Trade-level correlation between A and B
       - daily-aggregated pnl correlation (PRIMARY): sum each edge's
         pnl_pts per UTC day, correlate the two daily series with outer
         join (zero-fill missing days). This is the diversification number
         that matters at the portfolio level.
       - same-day overlap correlation (SECONDARY): subset to UTC days
         where BOTH edges fired, correlate within that subset.
       Both reported gated and ungated.

    2. Joint portfolio metrics, equal-weight per-trade (each trade weight=1,
       no vol scaling, no risk parity)
       - Per-trade: n_trades, sum_pnl, mean, std, sharpe (mean/std,
         no annualization), maxDD on equal-weight per-trade cumulative.
       - Daily: n_days, sum_pnl, mean, std, sharpe (mean/std on daily
         aggregates, no annualization), maxDD on daily cumulative.
       Both reported gated and ungated.

    3. Per-month breakdown (UTC calendar month)
       - For each (month, edge or joint, regime): n_trades, sum_pnl,
         mean, sharpe, maxDD on per-trade cumulative within that month.
       Output as long-format CSV plus stacked-bar PNG of monthly pnl
       (Edge A vs Edge B, gated portfolio).

Convention notes (consistent with Step 3a and Step 3c.1):
    - Sharpe = mean / std, NO annualization. Annualizing requires a
      trading-days assumption that is noisy on a ~3-month sample.
    - Drawdown = peak-to-trough on cumulative pnl_pts (points, no costs).
    - Edge A is deduped to pid 57226 only (57230 is identical).
    - Gate: vol_bar60 >= 0.000375 (Step 3a recommendation).

Sanity check: replicates Step 3c.1's per-edge gated/ungated stats from
gate_apply.py and asserts a match within tolerance.

Inputs:
    --diagnostic-journal  Path to regime_diagnostic_journal.csv. Required.
    --output-dir          Directory for outputs (created if missing).

Outputs:
    step3c2_portfolio_summary.csv
    step3c2_correlation.csv
    step3c2_monthly_breakdown.csv
    step3c2_monthly_pnl.png
    step3c2_joint_equity.png
    step3c2_portfolio_summary.md
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates


GATE_VOL_BAR60 = 0.000375  # Step 3a recommendation; absolute threshold,
# decile-7-of-OOS-population. Same value as gate_apply.py (3c.1).

# Deployable edges (deduped). Edge A = pid 57226 only; 57230 is its identical
# twin in the v2 STRONG cohort.
EDGE_A = {"label": "edge_a", "pid": 57226, "side": "SHORT", "bracket_id": 5,
          "title": "Edge A — pid 57226 SHORT XAUUSD b5"}
EDGE_B = {"label": "edge_b", "pid": 51274, "side": "LONG",  "bracket_id": 5,
          "title": "Edge B — pid 51274 LONG XAUUSD b5"}

# Tolerance for self-consistency check against Step 3c.1 numbers.
RECONCILE_TOL = 1e-6  # we recompute from the same input CSV, exact match expected


# Self-check expected numbers from Step 3c.1 (gate_apply.py output, Mac-verified).
# If any of these drift, something has changed in the input data and we want
# to know loudly before proceeding to portfolio aggregation.
EXPECTED_PER_EDGE = {
    "edge_a": {
        "ungated": {"n": 678, "mean": 7.9485, "sharpe": 0.2450,
                    "sum": 5389.08, "max_dd": -1560.46},
        "gated":   {"n": 302, "mean": 22.3555, "sharpe": 0.5278,
                    "sum": 6751.36, "max_dd": -842.01},
    },
    "edge_b": {
        "ungated": {"n": 648, "mean": 3.3036, "sharpe": 0.1940,
                    "sum": 2140.74, "max_dd": -896.36},
        "gated":   {"n": 220, "mean": 7.8810, "sharpe": 0.3154,
                    "sum": 1733.83, "max_dd": -663.38},
    },
}
# Looser tolerance for the printed-precision values from 3c.1
SELFCHECK_TOL = 1e-2


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--diagnostic-journal", required=True,
                   help="Path to regime_diagnostic_journal.csv")
    p.add_argument("--output-dir", required=True,
                   help="Directory for outputs (created if missing)")
    return p.parse_args()


def per_trade_sharpe(pnl: pd.Series) -> float:
    """Sharpe = mean / std, no annualization. Matches Step 3a / 3c.1."""
    if len(pnl) == 0:
        return float("nan")
    s = pnl.std()
    if s == 0 or np.isnan(s):
        return float("nan")
    return float(pnl.mean() / s)


def max_drawdown_pts(pnl: pd.Series) -> float:
    """Peak-to-trough on cumulative pnl. Returns non-positive number."""
    if len(pnl) == 0:
        return 0.0
    cum = pnl.cumsum()
    running_peak = cum.cummax()
    dd = cum - running_peak
    return float(dd.min())


def slice_edge(df: pd.DataFrame, edge: dict) -> pd.DataFrame:
    """Return atr_sl rows for the given edge, sorted by ts_close ascending,
    with parsed datetime + UTC date columns added."""
    sub = df[(df["pid"] == edge["pid"]) &
             (df["side"] == edge["side"]) &
             (df["bracket_id"] == edge["bracket_id"])].copy()
    if len(sub) == 0:
        raise RuntimeError(f"No rows found for edge {edge}")
    if "sl_mode" in sub.columns:
        sub = sub[sub["sl_mode"] == "atr_sl"].copy()
    sub["ts_close_dt"] = pd.to_datetime(sub["ts_close"], utc=True)
    sub["ts_close_date"] = sub["ts_close_dt"].dt.date
    sub["ts_close_month"] = sub["ts_close_dt"].dt.to_period("M")
    sub = sub.sort_values("ts_close_dt").reset_index(drop=True)
    sub["edge_label"] = edge["label"]
    return sub


def selfcheck_per_edge(df_edge: pd.DataFrame, edge: dict) -> dict:
    """Compute per-edge gated/ungated stats and assert match against
    Step 3c.1's published numbers. Returns the computed stats dict."""
    pnl_total = df_edge["pnl_pts"]
    gated_mask = df_edge["vol_bar60"] >= GATE_VOL_BAR60
    pnl_gated = df_edge.loc[gated_mask, "pnl_pts"]

    computed = {
        "ungated": {
            "n": len(pnl_total),
            "mean": float(pnl_total.mean()),
            "sharpe": per_trade_sharpe(pnl_total),
            "sum": float(pnl_total.sum()),
            "max_dd": max_drawdown_pts(pnl_total),
        },
        "gated": {
            "n": int(gated_mask.sum()),
            "mean": float(pnl_gated.mean()) if len(pnl_gated) else float("nan"),
            "sharpe": per_trade_sharpe(pnl_gated),
            "sum": float(pnl_gated.sum()),
            "max_dd": max_drawdown_pts(pnl_gated),
        },
    }

    expected = EXPECTED_PER_EDGE[edge["label"]]
    fails = []
    for regime in ("ungated", "gated"):
        for key, exp_val in expected[regime].items():
            got_val = computed[regime][key]
            if isinstance(exp_val, int):
                if exp_val != got_val:
                    fails.append(f"  {regime}.{key}: 3c.1={exp_val}, got={got_val}")
            else:
                if abs(exp_val - got_val) > SELFCHECK_TOL:
                    fails.append(f"  {regime}.{key}: 3c.1={exp_val:.4f}, got={got_val:.4f}, diff={abs(exp_val-got_val):.2e}")
    if fails:
        raise RuntimeError(
            f"Self-check failed for {edge['label']} pid={edge['pid']} "
            f"against Step 3c.1 published numbers:\n" + "\n".join(fails)
        )
    return computed


def build_portfolio(df_a: pd.DataFrame, df_b: pd.DataFrame, gated: bool) -> pd.DataFrame:
    """Concatenate Edge A + Edge B trades (gated or ungated), sorted by
    ts_close ascending. Equal-weight per trade (each row contributes
    pnl_pts directly)."""
    a = df_a.copy()
    b = df_b.copy()
    if gated:
        a = a[a["vol_bar60"] >= GATE_VOL_BAR60].copy()
        b = b[b["vol_bar60"] >= GATE_VOL_BAR60].copy()
    joint = pd.concat([a, b], ignore_index=True)
    joint = joint.sort_values("ts_close_dt").reset_index(drop=True)
    return joint


def daily_pnl(df_trades: pd.DataFrame) -> pd.Series:
    """Aggregate per-trade pnl to UTC-daily total. Returns Series indexed
    by date with sum of pnl_pts."""
    if len(df_trades) == 0:
        return pd.Series(dtype=float)
    return df_trades.groupby("ts_close_date")["pnl_pts"].sum().sort_index()


def correlation_pair(df_a: pd.DataFrame, df_b: pd.DataFrame) -> dict:
    """Compute daily-aggregated correlation (PRIMARY, outer-join on dates,
    zero-fill missing) and same-day overlap correlation (SECONDARY, subset
    to days where BOTH edges fired)."""
    daily_a = daily_pnl(df_a)
    daily_b = daily_pnl(df_b)

    # Primary: outer join, zero-fill. Trading-day union view.
    all_days = sorted(set(daily_a.index) | set(daily_b.index))
    a_aligned = daily_a.reindex(all_days, fill_value=0.0)
    b_aligned = daily_b.reindex(all_days, fill_value=0.0)
    if len(all_days) >= 2 and a_aligned.std() > 0 and b_aligned.std() > 0:
        corr_outer = float(a_aligned.corr(b_aligned))
    else:
        corr_outer = float("nan")
    n_days_outer = len(all_days)

    # Secondary: inner join. Days both fired.
    both_days = sorted(set(daily_a.index) & set(daily_b.index))
    n_days_inner = len(both_days)
    if n_days_inner >= 2:
        a_inner = daily_a.reindex(both_days)
        b_inner = daily_b.reindex(both_days)
        if a_inner.std() > 0 and b_inner.std() > 0:
            corr_inner = float(a_inner.corr(b_inner))
        else:
            corr_inner = float("nan")
    else:
        corr_inner = float("nan")

    return {
        "n_days_a": len(daily_a),
        "n_days_b": len(daily_b),
        "n_days_outer": n_days_outer,
        "n_days_inner": n_days_inner,
        "corr_daily_outer_zerofill": corr_outer,
        "corr_daily_inner_overlap": corr_inner,
    }


def joint_metrics(df_joint: pd.DataFrame) -> dict:
    """Compute per-trade and daily portfolio metrics on the concatenated
    trade list."""
    pnl = df_joint["pnl_pts"]
    daily = daily_pnl(df_joint)

    return {
        "n_trades": len(pnl),
        "n_days": len(daily),
        "sum_pnl_pts": float(pnl.sum()),
        # Per-trade (ordered by ts_close)
        "mean_per_trade": float(pnl.mean()) if len(pnl) else float("nan"),
        "std_per_trade": float(pnl.std()) if len(pnl) else float("nan"),
        "sharpe_per_trade": per_trade_sharpe(pnl),
        "max_dd_per_trade": max_drawdown_pts(pnl),
        # Daily aggregates
        "mean_per_day": float(daily.mean()) if len(daily) else float("nan"),
        "std_per_day": float(daily.std()) if len(daily) else float("nan"),
        "sharpe_per_day": per_trade_sharpe(daily),  # mean/std on daily series
        "max_dd_per_day": max_drawdown_pts(daily),
    }


def monthly_breakdown(df_a: pd.DataFrame, df_b: pd.DataFrame, gated: bool) -> pd.DataFrame:
    """Per-month rows for edge_a, edge_b, and joint. Returns long-format
    DataFrame with columns: month, scope, regime, n_trades, sum_pnl_pts,
    mean_pnl_pts, sharpe_per_trade, max_dd_pts."""
    if gated:
        a = df_a[df_a["vol_bar60"] >= GATE_VOL_BAR60].copy()
        b = df_b[df_b["vol_bar60"] >= GATE_VOL_BAR60].copy()
    else:
        a = df_a.copy()
        b = df_b.copy()
    regime_label = "gated" if gated else "ungated"

    rows = []
    months = sorted(set(a["ts_close_month"]) | set(b["ts_close_month"]))

    for month in months:
        for scope, df_scope in (("edge_a", a[a["ts_close_month"] == month]),
                                ("edge_b", b[b["ts_close_month"] == month])):
            pnl = df_scope["pnl_pts"]
            rows.append({
                "month": str(month),
                "scope": scope,
                "regime": regime_label,
                "n_trades": len(pnl),
                "sum_pnl_pts": float(pnl.sum()) if len(pnl) else 0.0,
                "mean_pnl_pts": float(pnl.mean()) if len(pnl) else float("nan"),
                "sharpe_per_trade": per_trade_sharpe(pnl),
                "max_dd_pts": max_drawdown_pts(pnl),
            })
        # Joint for this month: concat A+B in this month, sorted by ts_close
        joint_month = pd.concat(
            [a[a["ts_close_month"] == month], b[b["ts_close_month"] == month]],
            ignore_index=True
        ).sort_values("ts_close_dt").reset_index(drop=True)
        pnl = joint_month["pnl_pts"]
        rows.append({
            "month": str(month),
            "scope": "joint",
            "regime": regime_label,
            "n_trades": len(pnl),
            "sum_pnl_pts": float(pnl.sum()) if len(pnl) else 0.0,
            "mean_pnl_pts": float(pnl.mean()) if len(pnl) else float("nan"),
            "sharpe_per_trade": per_trade_sharpe(pnl),
            "max_dd_pts": max_drawdown_pts(pnl),
        })
    return pd.DataFrame(rows)


def plot_joint_equity(df_a: pd.DataFrame, df_b: pd.DataFrame,
                      joint_gated: dict, joint_ungated: dict,
                      out_path: str) -> None:
    """Joint per-trade cumulative pnl, gated vs ungated, calendar x-axis."""
    joint_g = build_portfolio(df_a, df_b, gated=True).copy()
    joint_u = build_portfolio(df_a, df_b, gated=False).copy()
    joint_g["cum"] = joint_g["pnl_pts"].cumsum()
    joint_u["cum"] = joint_u["pnl_pts"].cumsum()

    fig, ax = plt.subplots(figsize=(11, 6))
    ax.plot(joint_u["ts_close_dt"], joint_u["cum"],
            label=(f"Ungated  (n={joint_ungated['n_trades']}, "
                   f"sharpe_trade={joint_ungated['sharpe_per_trade']:.3f}, "
                   f"sharpe_day={joint_ungated['sharpe_per_day']:.3f}, "
                   f"sum={joint_ungated['sum_pnl_pts']:.0f}, "
                   f"maxDD={joint_ungated['max_dd_per_trade']:.0f})"),
            linewidth=1.4, alpha=0.85)
    ax.plot(joint_g["ts_close_dt"], joint_g["cum"],
            label=(f"Gated v60>={GATE_VOL_BAR60}  (n={joint_gated['n_trades']}, "
                   f"sharpe_trade={joint_gated['sharpe_per_trade']:.3f}, "
                   f"sharpe_day={joint_gated['sharpe_per_day']:.3f}, "
                   f"sum={joint_gated['sum_pnl_pts']:.0f}, "
                   f"maxDD={joint_gated['max_dd_per_trade']:.0f})"),
            linewidth=1.4, alpha=0.85)
    ax.axhline(0.0, color="black", linewidth=0.6, alpha=0.4)
    ax.set_title("Step 3c.2 — Joint portfolio equity (Edge A + Edge B, equal-weight per trade)")
    ax.set_xlabel("ts_close (UTC)")
    ax.set_ylabel("Cumulative pnl_pts")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=9)
    ax.xaxis.set_major_locator(mdates.AutoDateLocator())
    ax.xaxis.set_major_formatter(mdates.ConciseDateFormatter(ax.xaxis.get_major_locator()))
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)


def plot_monthly_pnl(monthly_df: pd.DataFrame, out_path: str) -> None:
    """Stacked bar: monthly sum_pnl_pts by edge, gated portfolio."""
    g = monthly_df[monthly_df["regime"] == "gated"].copy()
    a = g[g["scope"] == "edge_a"].set_index("month")["sum_pnl_pts"]
    b = g[g["scope"] == "edge_b"].set_index("month")["sum_pnl_pts"]
    months = sorted(set(a.index) | set(b.index))
    a = a.reindex(months, fill_value=0.0)
    b = b.reindex(months, fill_value=0.0)

    fig, ax = plt.subplots(figsize=(10, 5.5))
    x = np.arange(len(months))
    width = 0.6

    # Stacked: positive values stack up, negative stack down. Handle separately
    # to avoid pyplot's stacked-bar issues with mixed signs.
    a_pos = a.where(a >= 0, 0.0).values
    a_neg = a.where(a < 0,  0.0).values
    b_pos = b.where(b >= 0, 0.0).values
    b_neg = b.where(b < 0,  0.0).values

    ax.bar(x, a_pos, width, label="Edge A (SHORT 57226)", color="#1f77b4")
    ax.bar(x, b_pos, width, bottom=a_pos, label="Edge B (LONG 51274)", color="#ff7f0e")
    ax.bar(x, a_neg, width, color="#1f77b4", alpha=0.7)
    ax.bar(x, b_neg, width, bottom=a_neg, color="#ff7f0e", alpha=0.7)

    # Total markers (sum)
    totals = (a + b).values
    for xi, t in zip(x, totals):
        ax.annotate(f"{t:+.0f}", xy=(xi, t), xytext=(0, 5 if t >= 0 else -12),
                    textcoords="offset points", ha="center", fontsize=9)

    ax.axhline(0.0, color="black", linewidth=0.6)
    ax.set_xticks(x)
    ax.set_xticklabels(months, rotation=0)
    ax.set_xlabel("UTC month")
    ax.set_ylabel("Monthly sum pnl_pts (gated)")
    ax.set_title("Step 3c.2 — Monthly pnl by edge (gated portfolio)")
    ax.legend(loc="best", fontsize=9)
    ax.grid(True, alpha=0.3, axis="y")
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)


def write_markdown(per_edge_stats: dict,
                   joint_gated: dict, joint_ungated: dict,
                   corr_gated: dict, corr_ungated: dict,
                   monthly_df: pd.DataFrame,
                   out_path: str) -> None:
    lines = []
    lines.append("# C6 #1C Step 3c.2 — Deduped portfolio metrics\n")
    lines.append(f"Gate: `vol_bar60 >= {GATE_VOL_BAR60}` (Step 3a recommendation, decile-7-of-OOS, absolute threshold).\n")
    lines.append("Portfolio: equal-weight per trade (each trade contributes pnl_pts directly, no vol scaling, no risk parity).\n")
    lines.append("Sharpe: `mean / std`, NO annualization. Per-trade sharpe and per-day sharpe both reported.\n")
    lines.append("Drawdown: peak-to-trough on cumulative pnl_pts (points, no costs).\n")
    lines.append("Edge A is deduped to pid 57226 only (57230 is identical).\n")
    lines.append("\n---\n")

    # 1. Per-edge self-check (informational reprint of 3c.1 numbers)
    lines.append("## 1. Per-edge stats (replicated from Step 3c.1)\n")
    lines.append("| edge | regime | n | mean | sharpe | sum | maxDD |")
    lines.append("|---|---|---:|---:|---:|---:|---:|")
    for label in ("edge_a", "edge_b"):
        for regime in ("ungated", "gated"):
            s = per_edge_stats[label][regime]
            lines.append(f"| {label} | {regime} | {s['n']} | {s['mean']:.4f} | {s['sharpe']:.4f} | {s['sum']:.2f} | {s['max_dd']:.2f} |")
    lines.append("\n")

    # 2. Joint portfolio metrics
    lines.append("## 2. Joint portfolio metrics (Edge A + Edge B, equal-weight per trade)\n")
    lines.append("| metric | ungated | gated |")
    lines.append("|---|---:|---:|")
    lines.append(f"| n_trades | {joint_ungated['n_trades']} | {joint_gated['n_trades']} |")
    lines.append(f"| n_days   | {joint_ungated['n_days']} | {joint_gated['n_days']} |")
    lines.append(f"| sum pnl_pts | {joint_ungated['sum_pnl_pts']:.2f} | {joint_gated['sum_pnl_pts']:.2f} |")
    lines.append(f"| mean per trade | {joint_ungated['mean_per_trade']:.4f} | {joint_gated['mean_per_trade']:.4f} |")
    lines.append(f"| std per trade  | {joint_ungated['std_per_trade']:.4f} | {joint_gated['std_per_trade']:.4f} |")
    lines.append(f"| sharpe per trade | {joint_ungated['sharpe_per_trade']:.4f} | {joint_gated['sharpe_per_trade']:.4f} |")
    lines.append(f"| maxDD per trade  | {joint_ungated['max_dd_per_trade']:.2f} | {joint_gated['max_dd_per_trade']:.2f} |")
    lines.append(f"| mean per day  | {joint_ungated['mean_per_day']:.4f} | {joint_gated['mean_per_day']:.4f} |")
    lines.append(f"| std per day   | {joint_ungated['std_per_day']:.4f} | {joint_gated['std_per_day']:.4f} |")
    lines.append(f"| sharpe per day | {joint_ungated['sharpe_per_day']:.4f} | {joint_gated['sharpe_per_day']:.4f} |")
    lines.append(f"| maxDD per day  | {joint_ungated['max_dd_per_day']:.2f} | {joint_gated['max_dd_per_day']:.2f} |")
    lines.append("\n")

    # Sharpe deltas commentary
    dt = joint_gated['sharpe_per_trade'] - joint_ungated['sharpe_per_trade']
    dd = joint_gated['sharpe_per_day']   - joint_ungated['sharpe_per_day']
    ds = joint_gated['sum_pnl_pts']      - joint_ungated['sum_pnl_pts']
    dx = joint_gated['max_dd_per_trade'] - joint_ungated['max_dd_per_trade']
    lines.append(f"Effect of gate on portfolio: "
                 f"sharpe_trade Δ {dt:+.3f}, sharpe_day Δ {dd:+.3f}, "
                 f"sum_pnl Δ {ds:+.0f} pts, maxDD_trade Δ {dx:+.0f} pts (less negative = improvement).\n\n")

    # 3. Correlation
    lines.append("## 3. Edge A ↔ Edge B correlation\n")
    lines.append("| metric | ungated | gated |")
    lines.append("|---|---:|---:|")
    lines.append(f"| n_days A | {corr_ungated['n_days_a']} | {corr_gated['n_days_a']} |")
    lines.append(f"| n_days B | {corr_ungated['n_days_b']} | {corr_gated['n_days_b']} |")
    lines.append(f"| n_days union (outer) | {corr_ungated['n_days_outer']} | {corr_gated['n_days_outer']} |")
    lines.append(f"| n_days both fired (inner) | {corr_ungated['n_days_inner']} | {corr_gated['n_days_inner']} |")
    lines.append(f"| **corr daily, outer-join zero-fill (PRIMARY)** | {corr_ungated['corr_daily_outer_zerofill']:.4f} | {corr_gated['corr_daily_outer_zerofill']:.4f} |")
    lines.append(f"| corr daily, inner-overlap (SECONDARY) | {corr_ungated['corr_daily_inner_overlap']:.4f} | {corr_gated['corr_daily_inner_overlap']:.4f} |")
    lines.append("\n")
    lines.append("Interpretation: outer-join correlation is the diversification number for the portfolio. "
                 "Inner-overlap shows whether the edges co-move on days when both fire (smaller sample).\n\n")

    # 4. Monthly breakdown
    lines.append("## 4. Monthly breakdown\n")
    lines.append("Long-format CSV in `step3c2_monthly_breakdown.csv`. Gated rows summarized below "
                 "(joint portfolio per month):\n\n")
    g_joint = monthly_df[(monthly_df["regime"] == "gated") & (monthly_df["scope"] == "joint")].copy()
    if len(g_joint) > 0:
        lines.append("| month | n_trades | sum_pnl | mean | sharpe_per_trade | maxDD |")
        lines.append("|---|---:|---:|---:|---:|---:|")
        for _, r in g_joint.iterrows():
            sharpe = r['sharpe_per_trade']
            sharpe_str = f"{sharpe:.4f}" if not np.isnan(sharpe) else "nan"
            lines.append(f"| {r['month']} | {int(r['n_trades'])} | {r['sum_pnl_pts']:.2f} | "
                         f"{r['mean_pnl_pts']:.4f} | {sharpe_str} | {r['max_dd_pts']:.2f} |")
        lines.append("\n")

    lines.append("---\n")
    lines.append("## Sanity checks performed\n")
    lines.append(f"- Per-edge gated/ungated stats replicated against Step 3c.1's published numbers "
                 f"(tolerance {SELFCHECK_TOL}). If this script ran to completion, all checks passed.\n")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))


def main() -> int:
    args = parse_args()

    if not os.path.isfile(args.diagnostic_journal):
        print(f"ERROR: diagnostic journal not found: {args.diagnostic_journal}",
              file=sys.stderr)
        return 1
    os.makedirs(args.output_dir, exist_ok=True)

    print(f"[portfolio_metrics] reading diagnostic journal: {args.diagnostic_journal}")
    diag = pd.read_csv(args.diagnostic_journal)
    print(f"[portfolio_metrics]   shape: {diag.shape}")
    required_cols = {"ts_close", "pid", "side", "bracket_id", "sl_mode",
                     "pnl_pts", "vol_bar60"}
    missing = required_cols - set(diag.columns)
    if missing:
        print(f"ERROR: diagnostic journal missing columns: {sorted(missing)}",
              file=sys.stderr)
        return 1

    # 1. Per-edge slices and self-check
    print("\n[portfolio_metrics] slicing edges and running self-check...")
    df_a = slice_edge(diag, EDGE_A)
    df_b = slice_edge(diag, EDGE_B)
    per_edge_stats = {
        "edge_a": selfcheck_per_edge(df_a, EDGE_A),
        "edge_b": selfcheck_per_edge(df_b, EDGE_B),
    }
    print("[portfolio_metrics]   self-check OK against Step 3c.1 numbers.")

    # 2. Joint portfolio metrics
    print("\n[portfolio_metrics] computing joint portfolio metrics...")
    joint_g_df = build_portfolio(df_a, df_b, gated=True)
    joint_u_df = build_portfolio(df_a, df_b, gated=False)
    joint_gated = joint_metrics(joint_g_df)
    joint_ungated = joint_metrics(joint_u_df)
    print(f"[portfolio_metrics]   gated:   n_trades={joint_gated['n_trades']}, "
          f"n_days={joint_gated['n_days']}, sum={joint_gated['sum_pnl_pts']:.2f}, "
          f"sharpe_trade={joint_gated['sharpe_per_trade']:.4f}, "
          f"sharpe_day={joint_gated['sharpe_per_day']:.4f}, "
          f"maxDD_trade={joint_gated['max_dd_per_trade']:.2f}")
    print(f"[portfolio_metrics]   ungated: n_trades={joint_ungated['n_trades']}, "
          f"n_days={joint_ungated['n_days']}, sum={joint_ungated['sum_pnl_pts']:.2f}, "
          f"sharpe_trade={joint_ungated['sharpe_per_trade']:.4f}, "
          f"sharpe_day={joint_ungated['sharpe_per_day']:.4f}, "
          f"maxDD_trade={joint_ungated['max_dd_per_trade']:.2f}")

    # Self-check: joint sum_pnl == sum of per-edge sums for each regime
    expected_g_sum = per_edge_stats["edge_a"]["gated"]["sum"] + per_edge_stats["edge_b"]["gated"]["sum"]
    expected_u_sum = per_edge_stats["edge_a"]["ungated"]["sum"] + per_edge_stats["edge_b"]["ungated"]["sum"]
    if abs(joint_gated["sum_pnl_pts"] - expected_g_sum) > SELFCHECK_TOL:
        raise RuntimeError(f"Joint gated sum {joint_gated['sum_pnl_pts']:.4f} != A+B sum {expected_g_sum:.4f}")
    if abs(joint_ungated["sum_pnl_pts"] - expected_u_sum) > SELFCHECK_TOL:
        raise RuntimeError(f"Joint ungated sum {joint_ungated['sum_pnl_pts']:.4f} != A+B sum {expected_u_sum:.4f}")
    print("[portfolio_metrics]   joint-sum reconciliation OK (joint == A+B per regime).")

    # 3. Correlation
    print("\n[portfolio_metrics] computing daily correlations...")
    corr_gated = correlation_pair(
        df_a[df_a["vol_bar60"] >= GATE_VOL_BAR60],
        df_b[df_b["vol_bar60"] >= GATE_VOL_BAR60],
    )
    corr_ungated = correlation_pair(df_a, df_b)
    print(f"[portfolio_metrics]   gated:   "
          f"outer-zerofill corr={corr_gated['corr_daily_outer_zerofill']:.4f} "
          f"(n_days={corr_gated['n_days_outer']}), "
          f"inner-overlap corr={corr_gated['corr_daily_inner_overlap']:.4f} "
          f"(n_days={corr_gated['n_days_inner']})")
    print(f"[portfolio_metrics]   ungated: "
          f"outer-zerofill corr={corr_ungated['corr_daily_outer_zerofill']:.4f} "
          f"(n_days={corr_ungated['n_days_outer']}), "
          f"inner-overlap corr={corr_ungated['corr_daily_inner_overlap']:.4f} "
          f"(n_days={corr_ungated['n_days_inner']})")

    # 4. Monthly breakdown
    print("\n[portfolio_metrics] computing monthly breakdown...")
    monthly_g = monthly_breakdown(df_a, df_b, gated=True)
    monthly_u = monthly_breakdown(df_a, df_b, gated=False)
    monthly_all = pd.concat([monthly_u, monthly_g], ignore_index=True)
    print(f"[portfolio_metrics]   {monthly_all['month'].nunique()} months, "
          f"{len(monthly_all)} rows total")

    # 5. Write outputs
    print("\n[portfolio_metrics] writing outputs...")

    # Portfolio summary CSV
    summary_rows = []
    for regime, stats in (("ungated", joint_ungated), ("gated", joint_gated)):
        summary_rows.append({
            "regime": regime,
            **{k: stats[k] for k in (
                "n_trades", "n_days", "sum_pnl_pts",
                "mean_per_trade", "std_per_trade", "sharpe_per_trade", "max_dd_per_trade",
                "mean_per_day", "std_per_day", "sharpe_per_day", "max_dd_per_day",
            )},
        })
    summary_csv = os.path.join(args.output_dir, "step3c2_portfolio_summary.csv")
    pd.DataFrame(summary_rows).to_csv(summary_csv, index=False)
    print(f"[portfolio_metrics]   wrote {summary_csv}")

    # Correlation CSV
    corr_rows = []
    for regime, c in (("ungated", corr_ungated), ("gated", corr_gated)):
        corr_rows.append({"regime": regime, **c})
    corr_csv = os.path.join(args.output_dir, "step3c2_correlation.csv")
    pd.DataFrame(corr_rows).to_csv(corr_csv, index=False)
    print(f"[portfolio_metrics]   wrote {corr_csv}")

    # Monthly breakdown CSV
    monthly_csv = os.path.join(args.output_dir, "step3c2_monthly_breakdown.csv")
    monthly_all.to_csv(monthly_csv, index=False)
    print(f"[portfolio_metrics]   wrote {monthly_csv}")

    # Joint equity PNG
    joint_eq_png = os.path.join(args.output_dir, "step3c2_joint_equity.png")
    plot_joint_equity(df_a, df_b, joint_gated, joint_ungated, joint_eq_png)
    print(f"[portfolio_metrics]   wrote {joint_eq_png}")

    # Monthly PNG
    monthly_png = os.path.join(args.output_dir, "step3c2_monthly_pnl.png")
    plot_monthly_pnl(monthly_all, monthly_png)
    print(f"[portfolio_metrics]   wrote {monthly_png}")

    # Markdown writeup
    md_path = os.path.join(args.output_dir, "step3c2_portfolio_summary.md")
    write_markdown(per_edge_stats, joint_gated, joint_ungated,
                   corr_gated, corr_ungated, monthly_all, md_path)
    print(f"[portfolio_metrics]   wrote {md_path}")

    print("\n[portfolio_metrics] Step 3c.2 complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
