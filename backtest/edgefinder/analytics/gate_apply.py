"""
C6 #1C Step 3c.1 — Apply vol_bar60 gate and recompute equity curves.

Reads the regime_diagnostic_journal.csv (per-trade atr_sl rows with vol_bar60
attached, produced by Step 3 pre-flight regime_diagnostic.py).

For the two deployable survivor edges:
    Edge A: pid 57226 SHORT XAUUSD b5 (deduped from 57226/57230)
    Edge B: pid 51274 LONG  XAUUSD b5

Applies the gate vol_bar60 >= 0.000375 (Step 3a recommendation, decile-7-of-OOS)
and produces:
    - per-trade cumulative equity curves (gated vs ungated), one PNG per edge
    - numeric summary table (CSV) with n, mean, std, sharpe, sum_pnl, max_dd
    - markdown writeup

Convention notes:
    - Sharpe = mean(pnl_pts) / std(pnl_pts), per-trade, no annualization
      (matches Step 3a's gate_sharpe_after / gate_sharpe_no_gate definition).
    - Drawdown = peak-to-trough on cumulative pnl_pts in points (no costs,
      no normalization).
    - Equity curves are calendar-time on the x-axis (ts_close), per-trade
      cumulative on y-axis.
    - 57226 and 57230 have identical journal rows in the v2 STRONG cohort
      (rank 4 and 5 respectively). They are treated as ONE logical edge
      (Edge A) and only 57226 is processed.

Inputs:
    --diagnostic-journal  Path to regime_diagnostic_journal.csv (per-trade
                          atr_sl rows with vol_bar60). Required.
    --regrade-per-edge    Path to regrade_v3_per_edge.csv. Used to verify the
                          gate cutoff and replicate the no-gate / gated
                          numbers from Step 3a as a self-consistency check.
                          Required.
    --output-dir          Directory for outputs. Default:
                          backtest/edgefinder/output/paper_trade/

Outputs:
    step3c_equity_curves_edge_a.png
    step3c_equity_curves_edge_b.png
    step3c_gate_summary.csv
    step3c_gate_summary.md
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates


GATE_VOL_BAR60 = 0.000375  # Step 3a recommendation; absolute threshold,
# decile-7-of-OOS-population. Hardcoded constant — re-derivation against
# this dataset would only re-confirm it.

# Deployable edges. Edge A is deduped from 57226/57230 (identical rows).
EDGE_A = {
    "label": "edge_a",
    "title": "Edge A — pid 57226 SHORT XAUUSD b5 (deduped from 57226/57230)",
    "pid": 57226,
    "side": "SHORT",
    "bracket_id": 5,
}
EDGE_B = {
    "label": "edge_b",
    "title": "Edge B — pid 51274 LONG XAUUSD b5",
    "pid": 51274,
    "side": "LONG",
    "bracket_id": 5,
}

# Tolerance for the self-consistency check against Step 3a's per-edge file.
# Anything >= this in absolute difference will fail loudly. Step 3a's numbers
# are all <= 4 dp in the CSV; 1e-3 is comfortably loose.
RECONCILE_TOL = 1e-3


@dataclass
class EdgeStats:
    label: str
    title: str
    pid: int
    side: str
    bracket_id: int
    n_total: int
    mean_total: float
    std_total: float
    sharpe_total: float
    sum_total: float
    max_dd_total: float
    n_gated: int
    mean_gated: float
    std_gated: float
    sharpe_gated: float
    sum_gated: float
    max_dd_gated: float
    n_excluded: int  # n_total - n_gated; trades the gate would skip


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--diagnostic-journal", required=True,
                   help="Path to regime_diagnostic_journal.csv")
    p.add_argument("--regrade-per-edge", required=True,
                   help="Path to regrade_v3_per_edge.csv")
    p.add_argument("--output-dir", required=True,
                   help="Directory for outputs (created if missing)")
    return p.parse_args()


def per_trade_sharpe(pnl: pd.Series) -> float:
    """Sharpe = mean / std, per-trade, no annualization. Matches Step 3a."""
    if len(pnl) == 0:
        return float("nan")
    s = pnl.std()
    if s == 0 or np.isnan(s):
        return float("nan")
    return float(pnl.mean() / s)


def max_drawdown_pts(pnl: pd.Series) -> float:
    """Peak-to-trough on cumulative pnl_pts. Returned as a non-positive
    number (most negative excursion below running peak). 0.0 if no drawdown
    or if the series is empty."""
    if len(pnl) == 0:
        return 0.0
    cum = pnl.cumsum()
    running_peak = cum.cummax()
    dd = cum - running_peak  # <= 0 everywhere
    return float(dd.min())


def slice_edge(df: pd.DataFrame, edge: dict) -> pd.DataFrame:
    """Return atr_sl rows for the given edge, sorted by ts_close ascending."""
    sub = df[(df["pid"] == edge["pid"]) &
             (df["side"] == edge["side"]) &
             (df["bracket_id"] == edge["bracket_id"])].copy()
    if len(sub) == 0:
        raise RuntimeError(f"No rows found for edge {edge}")
    if "sl_mode" in sub.columns:
        # diagnostic_journal is already atr_sl-only, but defensive filter
        # in case caller swaps in a different file.
        sub = sub[sub["sl_mode"] == "atr_sl"].copy()
    sub["ts_close_dt"] = pd.to_datetime(sub["ts_close"], utc=True)
    sub = sub.sort_values("ts_close_dt").reset_index(drop=True)
    return sub


def compute_edge_stats(df_edge: pd.DataFrame, edge: dict) -> EdgeStats:
    pnl_total = df_edge["pnl_pts"]
    gated_mask = df_edge["vol_bar60"] >= GATE_VOL_BAR60
    pnl_gated = df_edge.loc[gated_mask, "pnl_pts"]

    return EdgeStats(
        label=edge["label"],
        title=edge["title"],
        pid=edge["pid"],
        side=edge["side"],
        bracket_id=edge["bracket_id"],
        n_total=len(pnl_total),
        mean_total=float(pnl_total.mean()) if len(pnl_total) else float("nan"),
        std_total=float(pnl_total.std()) if len(pnl_total) else float("nan"),
        sharpe_total=per_trade_sharpe(pnl_total),
        sum_total=float(pnl_total.sum()),
        max_dd_total=max_drawdown_pts(pnl_total),
        n_gated=int(gated_mask.sum()),
        mean_gated=float(pnl_gated.mean()) if len(pnl_gated) else float("nan"),
        std_gated=float(pnl_gated.std()) if len(pnl_gated) else float("nan"),
        sharpe_gated=per_trade_sharpe(pnl_gated),
        sum_gated=float(pnl_gated.sum()),
        max_dd_gated=max_drawdown_pts(pnl_gated),
        n_excluded=len(pnl_total) - int(gated_mask.sum()),
    )


def reconcile_with_3a(stats: EdgeStats, regrade: pd.DataFrame, edge: dict) -> None:
    """Self-consistency check: confirm we replicate Step 3a's numbers.

    For Edge A we use pid 57226 (rank 4 in regrade); 57230 (rank 5) is
    its dup and would yield the same numbers. For Edge B we use 51274.
    """
    row = regrade[(regrade["pid"] == edge["pid"]) &
                  (regrade["side"] == edge["side"]) &
                  (regrade["bracket_id"] == edge["bracket_id"])]
    if len(row) == 0:
        raise RuntimeError(f"Edge {edge} not found in regrade_v3_per_edge.csv")
    if len(row) > 1:
        raise RuntimeError(f"Edge {edge} has multiple rows in regrade_v3_per_edge.csv")
    r = row.iloc[0]

    # Step 3a fields:
    #   n_oos                     -> stats.n_total
    #   gate_mean_no_gate         -> stats.mean_total
    #   gate_sharpe_no_gate       -> stats.sharpe_total
    #   gate_n_after              -> stats.n_gated
    #   gate_mean_pnl_after       -> stats.mean_gated
    #   gate_sharpe_after         -> stats.sharpe_gated
    #   gate_sum_pnl_after        -> stats.sum_gated
    checks = [
        ("n_oos",                    int(r["n_oos"]),                    stats.n_total),
        ("gate_mean_no_gate",        float(r["gate_mean_no_gate"]),      stats.mean_total),
        ("gate_sharpe_no_gate",      float(r["gate_sharpe_no_gate"]),    stats.sharpe_total),
        ("gate_n_after",             int(r["gate_n_after"]),             stats.n_gated),
        ("gate_mean_pnl_after",      float(r["gate_mean_pnl_after"]),    stats.mean_gated),
        ("gate_sharpe_after",        float(r["gate_sharpe_after"]),      stats.sharpe_gated),
        ("gate_sum_pnl_after",       float(r["gate_sum_pnl_after"]),     stats.sum_gated),
    ]
    fails = []
    for name, expected, got in checks:
        if expected is None or (isinstance(expected, float) and np.isnan(expected)):
            continue
        if isinstance(expected, int):
            if expected != got:
                fails.append(f"  {name}: 3a says {expected}, computed {got}")
        else:
            if abs(expected - got) > RECONCILE_TOL:
                fails.append(f"  {name}: 3a says {expected:.6f}, computed {got:.6f}, diff={abs(expected-got):.2e}")
    if fails:
        raise RuntimeError(
            f"Reconciliation failed for edge {edge['label']} pid={edge['pid']}:\n"
            + "\n".join(fails)
        )


def plot_equity_curves(df_edge: pd.DataFrame, stats: EdgeStats, out_path: str) -> None:
    """Per-trade cumulative equity curves, gated vs ungated, calendar x-axis."""
    df = df_edge.copy()
    df["cum_pnl_total"] = df["pnl_pts"].cumsum()

    gated_mask = df["vol_bar60"] >= GATE_VOL_BAR60
    # Gated cumulative: only count gated trades, but plot against the same
    # calendar timeline so the visual comparison is honest about gaps.
    df["pnl_gated"] = np.where(gated_mask, df["pnl_pts"], 0.0)
    df["cum_pnl_gated"] = df["pnl_gated"].cumsum()

    fig, ax = plt.subplots(figsize=(11, 6))
    ax.plot(df["ts_close_dt"], df["cum_pnl_total"],
            label=f"Ungated  (n={stats.n_total}, "
                  f"sharpe={stats.sharpe_total:.3f}, "
                  f"sum={stats.sum_total:.0f}, "
                  f"maxDD={stats.max_dd_total:.0f})",
            linewidth=1.4, alpha=0.85)
    ax.plot(df["ts_close_dt"], df["cum_pnl_gated"],
            label=f"Gated v60>={GATE_VOL_BAR60}  (n={stats.n_gated}, "
                  f"sharpe={stats.sharpe_gated:.3f}, "
                  f"sum={stats.sum_gated:.0f}, "
                  f"maxDD={stats.max_dd_gated:.0f})",
            linewidth=1.4, alpha=0.85)
    ax.axhline(0.0, color="black", linewidth=0.6, alpha=0.4)
    ax.set_title(stats.title)
    ax.set_xlabel("ts_close (UTC)")
    ax.set_ylabel("Cumulative pnl_pts")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=9)
    ax.xaxis.set_major_locator(mdates.AutoDateLocator())
    ax.xaxis.set_major_formatter(mdates.ConciseDateFormatter(ax.xaxis.get_major_locator()))
    fig.tight_layout()
    fig.savefig(out_path, dpi=130)
    plt.close(fig)


def stats_to_summary_rows(stats: EdgeStats) -> list:
    """Return two CSV rows per edge: ungated and gated."""
    return [
        {
            "edge_label": stats.label,
            "pid": stats.pid,
            "side": stats.side,
            "bracket_id": stats.bracket_id,
            "regime": "ungated",
            "n": stats.n_total,
            "mean_pnl_pts": stats.mean_total,
            "std_pnl_pts": stats.std_total,
            "sharpe": stats.sharpe_total,
            "sum_pnl_pts": stats.sum_total,
            "max_dd_pts": stats.max_dd_total,
            "n_excluded_by_gate": 0,
        },
        {
            "edge_label": stats.label,
            "pid": stats.pid,
            "side": stats.side,
            "bracket_id": stats.bracket_id,
            "regime": f"gated_v60>={GATE_VOL_BAR60}",
            "n": stats.n_gated,
            "mean_pnl_pts": stats.mean_gated,
            "std_pnl_pts": stats.std_gated,
            "sharpe": stats.sharpe_gated,
            "sum_pnl_pts": stats.sum_gated,
            "max_dd_pts": stats.max_dd_gated,
            "n_excluded_by_gate": stats.n_excluded,
        },
    ]


def write_markdown(stats_list: list, out_path: str) -> None:
    lines = []
    lines.append("# C6 #1C Step 3c.1 — Gate-applied equity curves\n")
    lines.append(f"Gate: `vol_bar60 >= {GATE_VOL_BAR60}` (Step 3a recommendation, "
                 f"decile-7-of-OOS population, absolute threshold).\n")
    lines.append("Sharpe convention: per-trade, `mean / std`, no annualization. "
                 "Matches Step 3a `gate_sharpe_*` fields.\n")
    lines.append("Drawdown: peak-to-trough on cumulative `pnl_pts` (points, no costs).\n")
    lines.append("Edge A is deduped from pid 57226 / 57230 (identical journal rows in v2 STRONG cohort).\n")
    lines.append("\n---\n")

    for s in stats_list:
        lines.append(f"## {s.title}\n")
        lines.append("| metric | ungated | gated |")
        lines.append("|---|---:|---:|")
        lines.append(f"| n | {s.n_total} | {s.n_gated} |")
        lines.append(f"| mean pnl_pts | {s.mean_total:.4f} | {s.mean_gated:.4f} |")
        lines.append(f"| std pnl_pts  | {s.std_total:.4f} | {s.std_gated:.4f} |")
        lines.append(f"| sharpe       | {s.sharpe_total:.4f} | {s.sharpe_gated:.4f} |")
        lines.append(f"| sum pnl_pts  | {s.sum_total:.2f} | {s.sum_gated:.2f} |")
        lines.append(f"| max DD pts   | {s.max_dd_total:.2f} | {s.max_dd_gated:.2f} |")
        lines.append(f"\nGate excludes {s.n_excluded} of {s.n_total} trades "
                     f"({100.0 * s.n_excluded / s.n_total:.1f}%).\n")

        # Effect summary
        if s.n_gated > 0 and s.n_total > 0:
            sharpe_lift = s.sharpe_gated - s.sharpe_total
            mean_lift = s.mean_gated - s.mean_total
            sum_change = s.sum_gated - s.sum_total
            dd_change = s.max_dd_gated - s.max_dd_total  # less negative is better
            lines.append(f"Effect: sharpe {s.sharpe_total:.3f} -> {s.sharpe_gated:.3f} "
                         f"(Δ {sharpe_lift:+.3f}); "
                         f"mean {s.mean_total:.2f} -> {s.mean_gated:.2f} "
                         f"(Δ {mean_lift:+.2f} pts/trade); "
                         f"sum {s.sum_total:.0f} -> {s.sum_gated:.0f} "
                         f"(Δ {sum_change:+.0f} pts); "
                         f"maxDD {s.max_dd_total:.0f} -> {s.max_dd_gated:.0f} "
                         f"(Δ {dd_change:+.0f} pts; less negative = improvement).\n")
        lines.append("\n")

    lines.append("---\n")
    lines.append("## Sanity checks performed\n")
    lines.append("- Replicated Step 3a per-edge numbers from `regrade_v3_per_edge.csv` "
                 "to within 1e-3 absolute tolerance for: `n_oos`, `gate_mean_no_gate`, "
                 "`gate_sharpe_no_gate`, `gate_n_after`, `gate_mean_pnl_after`, "
                 "`gate_sharpe_after`, `gate_sum_pnl_after`. If this script ran "
                 "to completion, all checks passed.\n")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))


def main() -> int:
    args = parse_args()

    if not os.path.isfile(args.diagnostic_journal):
        print(f"ERROR: diagnostic journal not found: {args.diagnostic_journal}",
              file=sys.stderr)
        return 1
    if not os.path.isfile(args.regrade_per_edge):
        print(f"ERROR: regrade per-edge not found: {args.regrade_per_edge}",
              file=sys.stderr)
        return 1
    os.makedirs(args.output_dir, exist_ok=True)

    print(f"[gate_apply] reading diagnostic journal: {args.diagnostic_journal}")
    diag = pd.read_csv(args.diagnostic_journal)
    print(f"[gate_apply]   shape: {diag.shape}")
    required_cols = {"ts_close", "pid", "side", "bracket_id", "sl_mode",
                     "pnl_pts", "vol_bar60"}
    missing = required_cols - set(diag.columns)
    if missing:
        print(f"ERROR: diagnostic journal missing columns: {sorted(missing)}",
              file=sys.stderr)
        return 1

    print(f"[gate_apply] reading regrade per-edge: {args.regrade_per_edge}")
    regrade = pd.read_csv(args.regrade_per_edge)
    print(f"[gate_apply]   shape: {regrade.shape}")

    summary_rows = []
    stats_list = []

    for edge in (EDGE_A, EDGE_B):
        print(f"\n[gate_apply] === {edge['label']} :: {edge['title']} ===")
        df_edge = slice_edge(diag, edge)
        stats = compute_edge_stats(df_edge, edge)
        print(f"[gate_apply]   ungated: n={stats.n_total}, "
              f"mean={stats.mean_total:.4f}, sharpe={stats.sharpe_total:.4f}, "
              f"sum={stats.sum_total:.2f}, maxDD={stats.max_dd_total:.2f}")
        print(f"[gate_apply]   gated:   n={stats.n_gated}, "
              f"mean={stats.mean_gated:.4f}, sharpe={stats.sharpe_gated:.4f}, "
              f"sum={stats.sum_gated:.2f}, maxDD={stats.max_dd_gated:.2f}")

        reconcile_with_3a(stats, regrade, edge)
        print(f"[gate_apply]   reconciliation OK against Step 3a per-edge file.")

        png_path = os.path.join(args.output_dir,
                                f"step3c_equity_curves_{edge['label']}.png")
        plot_equity_curves(df_edge, stats, png_path)
        print(f"[gate_apply]   wrote {png_path}")

        summary_rows.extend(stats_to_summary_rows(stats))
        stats_list.append(stats)

    summary_csv = os.path.join(args.output_dir, "step3c_gate_summary.csv")
    pd.DataFrame(summary_rows).to_csv(summary_csv, index=False)
    print(f"\n[gate_apply] wrote {summary_csv}")

    summary_md = os.path.join(args.output_dir, "step3c_gate_summary.md")
    write_markdown(stats_list, summary_md)
    print(f"[gate_apply] wrote {summary_md}")

    print("\n[gate_apply] Step 3c.1 complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
