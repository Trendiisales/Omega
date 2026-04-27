"""
backtest/edgefinder/analytics/inspect_bracket_outcomes.py
=========================================================

C6 thread #1A — TP/SL/MtM outcome classifier.

Purpose
-------
The C5 ranking ranks 19 strict OOS survivors by averaged bracket PnL across
TRAIN+VAL. C6 #1 uncovered that rank-1 (pid 70823 LONG b4) and rank-2
(pid 70823 LONG b3) are not two edges — every one of their 3716 trades
resolves as MtM with zero TP/SL hits, and because both brackets are
60-minute horizon, fwd_bracket_pts_3 == fwd_bracket_pts_4 bar-for-bar on
that predicate. That is, the price never traverses ±50 pts within 60 min on
those NY_PM/VOL_MID/asian_range>47.22 bars, so the bracket framework
collapses to "settle at horizon" — a directional fwd_ret drift dressed in
bracket clothing.

That raises the question: how many of the other 17 strict survivors are
also MtM-dominated? A bracket strategy whose TPs and SLs essentially never
fire is not a bracket edge at all — it's just selecting bars where the
60-min directional drift is positive. That kind of edge is much more
fragile than a real TP/SL bracket: it relies on the post-bar drift staying
positive, with no protective stop ever invoked. If that drift sign flips
in OOS (or in production), the edge dies silently, with no SL to bound the
damage.

This module is a read-only diagnostic. It loads
    paper_trade_topk_trades.csv
classifies every row as TP / SL / MtM, aggregates per
    (pid, side, bracket_id, partition)
and flags any (pid, bracket) that is >90% MtM as a "fwd_ret edge in
bracket clothing".

Classification logic
--------------------
The trades CSV stores `pnl_pts = fwd_bracket_pts_<b> * side_sign`. After
the side-sign multiplication the bracket simulator's TP/SL outcomes are
uniform across LONG and SHORT:

    pnl_pts ≈ +bracket_tp_pts  → TP    (price moved enough in trade direction)
    pnl_pts ≈ -bracket_sl_pts  → SL    (price moved enough against trade direction)
    otherwise                   → MtM   (settled at horizon at intermediate price)

We compare with abs-tolerance 1e-6 to absorb any bracket-simulator FP
roundoff (the simulator clamps to discrete TP/SL prices, but downstream
arithmetic can still introduce ULP-level wobble).

We do NOT make assumptions about which bar within the horizon the TP/SL
fired — only the final outcome matters for this diagnostic. If you want
first-touch timing analysis, that requires the panel's first_touch_*m
columns and is out of scope here.

Output
------
1) per-trade classification appended to a new CSV:
    paper_trade_topk_trades_classified.csv
   adds one column: outcome ∈ {TP, SL, MtM}

2) per (pid, side, bracket_id, partition) summary CSV:
    paper_trade_topk_outcome_summary.csv
   columns: pid, side, bracket_id, rank, partition,
            n, tp_n, sl_n, mtm_n, tp_pct, sl_pct, mtm_pct,
            mean_pnl_pts, mean_pnl_pts_tp, mean_pnl_pts_sl, mean_pnl_pts_mtm,
            flag_mtm_dominated   (True if mtm_pct > 0.90)

3) MD report:
    paper_trade_topk_outcome_summary.md
   ranked table + flagged "fwd_ret edges in bracket clothing"

CLI
---
    python -m backtest.edgefinder.analytics.inspect_bracket_outcomes \\
        [--trades-csv PATH]   default backtest/edgefinder/output/paper_trade/paper_trade_topk_trades.csv
        [--out-dir PATH]      default backtest/edgefinder/output/paper_trade
        [--mtm-flag-threshold FLOAT]  default 0.90
        [--tol FLOAT]         default 1e-6  (abs tolerance for TP/SL match)

This module does NOT modify any core code, the panel binary, the
catalogue, or the OOS sentinel. It is a pure post-hoc CSV diagnostic.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd


# Default paths — relative to repo root, matching paper_trade_topk.py conventions.
DEFAULT_TRADES_CSV = (
    'backtest/edgefinder/output/paper_trade/paper_trade_topk_trades.csv'
)
DEFAULT_OUT_DIR = 'backtest/edgefinder/output/paper_trade'
DEFAULT_MTM_FLAG_THRESHOLD = 0.90
DEFAULT_TOL = 1e-6


# -----------------------------------------------------------------------------
# Classification
# -----------------------------------------------------------------------------
def classify_trades(trades: pd.DataFrame, tol: float) -> pd.Series:
    """
    Classify each trade row as TP / SL / MtM.

    Inputs:
        trades: DataFrame with columns pnl_pts, bracket_tp_pts, bracket_sl_pts.
        tol:    absolute tolerance for TP/SL match (in points).

    Returns:
        Series of dtype object with values in {'TP', 'SL', 'MtM'}.
    """
    required = {'pnl_pts', 'bracket_tp_pts', 'bracket_sl_pts'}
    missing = required - set(trades.columns)
    if missing:
        raise ValueError(
            f"trades CSV missing required columns: {sorted(missing)}. "
            f"Got: {sorted(trades.columns)}"
        )

    pnl = trades['pnl_pts'].to_numpy(dtype=np.float64)
    tp = trades['bracket_tp_pts'].to_numpy(dtype=np.float64)
    sl = trades['bracket_sl_pts'].to_numpy(dtype=np.float64)

    # After side-sign correction, TP outcome is +tp_pts and SL outcome is -sl_pts
    # for both LONG and SHORT trades.
    is_tp = np.abs(pnl - tp) <= tol
    is_sl = np.abs(pnl - (-sl)) <= tol

    # Sanity: a single trade should not satisfy both (would imply tp_pts == -sl_pts,
    # i.e. tp_pts == 0, which is nonsensical). If it ever happens, prefer TP and warn.
    overlap = is_tp & is_sl
    if overlap.any():
        n_overlap = int(overlap.sum())
        print(
            f"WARNING: {n_overlap} trade(s) match both TP and SL within tol={tol}. "
            f"Classifying as TP. This usually means tol is too loose or "
            f"bracket_tp_pts == -bracket_sl_pts in the data.",
            file=sys.stderr,
        )
        is_sl = is_sl & ~is_tp

    outcome = np.where(is_tp, 'TP', np.where(is_sl, 'SL', 'MtM'))
    return pd.Series(outcome, index=trades.index, name='outcome')


# -----------------------------------------------------------------------------
# Aggregation
# -----------------------------------------------------------------------------
def summarise(
    trades: pd.DataFrame,
    mtm_flag_threshold: float,
) -> pd.DataFrame:
    """
    Aggregate per (pid, side, bracket_id, rank, partition).
    `trades` must already have an 'outcome' column.
    """
    if 'outcome' not in trades.columns:
        raise ValueError("trades must have 'outcome' column from classify_trades()")

    group_cols = ['pid', 'side', 'bracket_id', 'rank', 'partition']

    # n per group
    n_total = trades.groupby(group_cols, sort=False).size().rename('n')

    # outcome counts
    counts = (
        trades
        .groupby(group_cols + ['outcome'], sort=False)
        .size()
        .unstack(fill_value=0)
    )
    for col in ('TP', 'SL', 'MtM'):
        if col not in counts.columns:
            counts[col] = 0
    counts = counts[['TP', 'SL', 'MtM']].rename(
        columns={'TP': 'tp_n', 'SL': 'sl_n', 'MtM': 'mtm_n'}
    )

    # mean PnL overall and per outcome.
    #
    # IMPORTANT: every per-outcome mean Series MUST be reindexed onto the same
    # MultiIndex as n_total (with NaN for groups where the outcome did not
    # fire). Otherwise pd.concat(axis=1) below silently drops the MultiIndex
    # names when one of the inputs has a default RangeIndex (which happens
    # when zero rows have that outcome anywhere — e.g. zero TP hits in the
    # entire CSV when every edge is MtM-dominated). When the names are
    # dropped, reset_index() does not hoist pid/side/bracket_id/rank/partition
    # back into columns and the col_order selection raises KeyError.
    mean_pnl = (
        trades.groupby(group_cols, sort=False)['pnl_pts']
        .mean()
        .rename('mean_pnl_pts')
    )

    def _mean_for(outcome_label: str) -> pd.Series:
        col_name = f'mean_pnl_pts_{outcome_label.lower()}'
        sub = trades[trades['outcome'] == outcome_label]
        if sub.empty:
            # Build an all-NaN Series indexed by the canonical MultiIndex of
            # n_total so the downstream concat aligns correctly.
            return pd.Series(np.nan, index=n_total.index, name=col_name, dtype=np.float64)
        grouped = (
            sub.groupby(group_cols, sort=False)['pnl_pts']
            .mean()
            .rename(col_name)
        )
        # Reindex onto the canonical MultiIndex so groups with zero of this
        # outcome get NaN rather than being dropped.
        return grouped.reindex(n_total.index)

    mean_tp = _mean_for('TP')
    mean_sl = _mean_for('SL')
    mean_mtm = _mean_for('MtM')

    summary = pd.concat([n_total, counts, mean_pnl, mean_tp, mean_sl, mean_mtm], axis=1)
    summary = summary.reset_index()

    # Defensive: confirm group-by columns made it back as columns. If concat
    # somehow still dropped the MultiIndex (e.g. all groups truly empty), fail
    # loudly rather than crashing on the col_order selection below.
    missing_idx_cols = [c for c in group_cols if c not in summary.columns]
    if missing_idx_cols:
        raise RuntimeError(
            f"summarise(): pd.concat dropped MultiIndex names "
            f"{missing_idx_cols}. This indicates an unexpected mismatch "
            f"between n_total.index and the per-outcome mean indexes."
        )

    # Fill NaN means (groups with zero of that outcome) with 0.0 for cleanliness.
    for c in ('mean_pnl_pts_tp', 'mean_pnl_pts_sl', 'mean_pnl_pts_mtm'):
        if c not in summary.columns:
            summary[c] = 0.0
        summary[c] = summary[c].fillna(0.0)

    # Percentages
    n_safe = summary['n'].replace(0, np.nan)
    summary['tp_pct'] = (summary['tp_n'] / n_safe).fillna(0.0)
    summary['sl_pct'] = (summary['sl_n'] / n_safe).fillna(0.0)
    summary['mtm_pct'] = (summary['mtm_n'] / n_safe).fillna(0.0)

    # Flag
    summary['flag_mtm_dominated'] = summary['mtm_pct'] > mtm_flag_threshold

    # Order columns
    col_order = [
        'pid', 'side', 'bracket_id', 'rank', 'partition',
        'n', 'tp_n', 'sl_n', 'mtm_n',
        'tp_pct', 'sl_pct', 'mtm_pct',
        'mean_pnl_pts',
        'mean_pnl_pts_tp', 'mean_pnl_pts_sl', 'mean_pnl_pts_mtm',
        'flag_mtm_dominated',
    ]
    summary = summary[col_order]

    # Sort: rank ascending, then partition in fixed order if present
    partition_order = {'train': 0, 'val': 1, 'oos': 2, 'all': 3}
    summary['_part_sort'] = summary['partition'].map(partition_order).fillna(99)
    summary = summary.sort_values(['rank', '_part_sort'], kind='mergesort')
    summary = summary.drop(columns='_part_sort').reset_index(drop=True)

    return summary


# -----------------------------------------------------------------------------
# MD report
# -----------------------------------------------------------------------------
def write_md_report(
    summary: pd.DataFrame,
    trades_csv_path: Path,
    out_md_path: Path,
    mtm_flag_threshold: float,
    tol: float,
) -> None:
    lines = []
    lines.append("# C6 #1A — TP/SL/MtM Outcome Classification")
    lines.append("")
    lines.append(f"- Source CSV: `{trades_csv_path}`")
    lines.append(f"- Total trade rows: {len(summary['pid']) and int(summary['n'].sum())}")
    lines.append(f"- Unique edges: {summary[['pid','side','bracket_id']].drop_duplicates().shape[0]}")
    lines.append(f"- Partitions: {sorted(summary['partition'].unique().tolist())}")
    lines.append(f"- MtM-dominated flag threshold: mtm_pct > {mtm_flag_threshold:.2f}")
    lines.append(f"- TP/SL match tolerance: abs(pnl - target) <= {tol:g}")
    lines.append("")

    # ----- Flagged edges (per partition) -----
    flagged = summary[summary['flag_mtm_dominated']].copy()
    lines.append("## Edges flagged as fwd_ret-in-bracket-clothing")
    lines.append("")
    if flagged.empty:
        lines.append("_None — every edge has at least "
                     f"{(1.0 - mtm_flag_threshold) * 100:.0f}% TP/SL hits "
                     "in every partition._")
    else:
        lines.append(f"{len(flagged)} (edge, partition) cells flagged. "
                     "These edges' bracket TPs and SLs essentially never fire — "
                     "the realised PnL is the directional drift over the bracket "
                     "horizon, not a bracketed outcome. Such edges have no protective "
                     "stop-loss in practice and are fragile to drift-sign flips.")
        lines.append("")
        lines.append("| rank | pid   | side  | b | partition | n     | mtm_pct | tp_pct | sl_pct | mean_pnl_pts |")
        lines.append("|-----:|------:|:------|:-:|:----------|------:|--------:|-------:|-------:|-------------:|")
        for _, r in flagged.iterrows():
            lines.append(
                f"| {int(r['rank']):>4} "
                f"| {int(r['pid']):>5} "
                f"| {r['side']:<5} "
                f"| {int(r['bracket_id'])} "
                f"| {r['partition']:<9} "
                f"| {int(r['n']):>5} "
                f"| {r['mtm_pct']*100:>6.2f}% "
                f"| {r['tp_pct']*100:>5.2f}% "
                f"| {r['sl_pct']*100:>5.2f}% "
                f"| {r['mean_pnl_pts']:>+12.4f} |"
            )
    lines.append("")

    # ----- Full table -----
    lines.append("## Full per-edge × partition outcome table")
    lines.append("")
    lines.append("| rank | pid   | side  | b | partition | n     | tp_n | sl_n | mtm_n | tp_pct | sl_pct | mtm_pct | mean_pnl_pts | flag |")
    lines.append("|-----:|------:|:------|:-:|:----------|------:|-----:|-----:|------:|-------:|-------:|--------:|-------------:|:-----|")
    for _, r in summary.iterrows():
        flag = "**MtM**" if r['flag_mtm_dominated'] else ""
        lines.append(
            f"| {int(r['rank']):>4} "
            f"| {int(r['pid']):>5} "
            f"| {r['side']:<5} "
            f"| {int(r['bracket_id'])} "
            f"| {r['partition']:<9} "
            f"| {int(r['n']):>5} "
            f"| {int(r['tp_n']):>4} "
            f"| {int(r['sl_n']):>4} "
            f"| {int(r['mtm_n']):>5} "
            f"| {r['tp_pct']*100:>5.2f}% "
            f"| {r['sl_pct']*100:>5.2f}% "
            f"| {r['mtm_pct']*100:>6.2f}% "
            f"| {r['mean_pnl_pts']:>+12.4f} "
            f"| {flag} |"
        )
    lines.append("")

    # ----- Aggregate counts per partition -----
    lines.append("## Edge counts per partition")
    lines.append("")
    lines.append("| partition | total_edges | mtm_dominated | tp_dominated (>50%) | sl_dominated (>50%) |")
    lines.append("|:----------|------------:|--------------:|--------------------:|--------------------:|")
    for part in sorted(summary['partition'].unique()):
        sub = summary[summary['partition'] == part]
        total_edges = len(sub)
        mtm_dom = int((sub['mtm_pct'] > mtm_flag_threshold).sum())
        tp_dom = int((sub['tp_pct'] > 0.50).sum())
        sl_dom = int((sub['sl_pct'] > 0.50).sum())
        lines.append(
            f"| {part:<9} | {total_edges:>11} | {mtm_dom:>13} | {tp_dom:>19} | {sl_dom:>19} |"
        )
    lines.append("")

    # ----- Interpretive guide -----
    lines.append("## Interpretive guide")
    lines.append("")
    lines.append(
        "- **MtM-dominated edges (mtm_pct > threshold):** the bracket TP/SL "
        "rarely fires; realised PnL is essentially the post-bar directional "
        "drift over the bracket's horizon. Equivalent to a directional "
        "fwd_ret strategy with no protective stop. Flagged because:"
    )
    lines.append(
        "    1. They survived strict OOS gates by luck-of-drift, not by a "
        "robust TP/SL distribution."
    )
    lines.append(
        "    2. Two MtM-dominated edges with the same predicate but different "
        "bracket_ids are NOT two independent edges (e.g. 60-min b3 and 60-min "
        "b4 produce identical PnL when neither hits TP/SL — confirmed for "
        "pid 70823 in C6 #1)."
    )
    lines.append(
        "    3. Out-of-sample they fail silently if the drift sign flips."
    )
    lines.append(
        "- **TP-dominated edges (tp_pct > 50%):** the predicate catches bars "
        "where the trade direction reaches its TP within the horizon "
        "frequently. These are real bracket edges."
    )
    lines.append(
        "- **SL-dominated edges (sl_pct > 50%):** the predicate is anti-correlated "
        "with the trade direction at the bracket scale; the strategy is bleeding "
        "to SL more often than not. Likely a sign-flipped or overfit predicate."
    )
    lines.append("")

    out_md_path.write_text('\n'.join(lines))


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="C6 #1A: TP/SL/MtM outcome classifier for paper-trade survivors."
    )
    p.add_argument(
        '--trades-csv',
        default=DEFAULT_TRADES_CSV,
        help=f"Path to paper_trade_topk_trades.csv (default: {DEFAULT_TRADES_CSV})",
    )
    p.add_argument(
        '--out-dir',
        default=DEFAULT_OUT_DIR,
        help=f"Output directory (default: {DEFAULT_OUT_DIR})",
    )
    p.add_argument(
        '--mtm-flag-threshold',
        type=float,
        default=DEFAULT_MTM_FLAG_THRESHOLD,
        help=(
            f"Flag edges with mtm_pct > this value as fwd_ret-in-bracket-clothing "
            f"(default: {DEFAULT_MTM_FLAG_THRESHOLD})"
        ),
    )
    p.add_argument(
        '--tol',
        type=float,
        default=DEFAULT_TOL,
        help=(
            f"Absolute tolerance (in points) for matching pnl_pts to "
            f"+bracket_tp_pts or -bracket_sl_pts (default: {DEFAULT_TOL:g})"
        ),
    )
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)

    trades_csv_path = Path(args.trades_csv)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not trades_csv_path.is_file():
        print(f"ERROR: trades CSV not found: {trades_csv_path}", file=sys.stderr)
        return 1

    print(f"loading trades: {trades_csv_path}")
    trades = pd.read_csv(trades_csv_path)
    print(f"trades rows: {len(trades)}")

    # Sanity: enforce required columns up-front
    required = [
        'pid', 'side', 'bracket_id', 'rank', 'partition',
        'pnl_pts', 'bracket_tp_pts', 'bracket_sl_pts',
    ]
    missing = [c for c in required if c not in trades.columns]
    if missing:
        print(
            f"ERROR: trades CSV missing required columns: {missing}. "
            f"Got: {sorted(trades.columns)}",
            file=sys.stderr,
        )
        return 2

    # Sanity: bracket_tp_pts and bracket_sl_pts should be strictly positive in the
    # paper-trade CSV — they are bracket distances in points.
    if (trades['bracket_tp_pts'] <= 0).any() or (trades['bracket_sl_pts'] <= 0).any():
        n_bad_tp = int((trades['bracket_tp_pts'] <= 0).sum())
        n_bad_sl = int((trades['bracket_sl_pts'] <= 0).sum())
        print(
            f"WARNING: {n_bad_tp} rows with bracket_tp_pts <= 0 and {n_bad_sl} "
            f"rows with bracket_sl_pts <= 0. Classification may be unreliable "
            f"for those rows.",
            file=sys.stderr,
        )

    print(f"classifying with tol={args.tol:g}")
    trades['outcome'] = classify_trades(trades, tol=args.tol)
    counts = trades['outcome'].value_counts()
    print(f"  TP : {int(counts.get('TP', 0)):>7}")
    print(f"  SL : {int(counts.get('SL', 0)):>7}")
    print(f"  MtM: {int(counts.get('MtM', 0)):>7}")

    classified_csv = out_dir / 'paper_trade_topk_trades_classified.csv'
    trades.to_csv(classified_csv, index=False)
    print(f"wrote {classified_csv} ({len(trades)} rows)")

    print(f"summarising per (pid, side, bracket_id, rank, partition)")
    summary = summarise(trades, mtm_flag_threshold=args.mtm_flag_threshold)

    summary_csv = out_dir / 'paper_trade_topk_outcome_summary.csv'
    summary.to_csv(summary_csv, index=False)
    print(f"wrote {summary_csv} ({len(summary)} rows)")

    n_flagged = int(summary['flag_mtm_dominated'].sum())
    n_total = len(summary)
    print(
        f"flagged (mtm_pct > {args.mtm_flag_threshold:.2f}): "
        f"{n_flagged} / {n_total} (edge, partition) cells"
    )

    md_path = out_dir / 'paper_trade_topk_outcome_summary.md'
    write_md_report(
        summary=summary,
        trades_csv_path=trades_csv_path,
        out_md_path=md_path,
        mtm_flag_threshold=args.mtm_flag_threshold,
        tol=args.tol,
    )
    print(f"wrote {md_path}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
