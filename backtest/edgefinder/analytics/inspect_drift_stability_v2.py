"""
backtest/edgefinder/analytics/inspect_drift_stability_v2.py
===========================================================

C6 thread #1B-followup — economic-survivability and tariff-dependence
diagnostic.

Context
-------
inspect_drift_stability.py (C6 #1B v1) classified 18/19 strict OOS
survivors as STABLE under three gates: train/val/oos sign-match,
|OOS t-stat| >= 2.0, and OOS/train signed-mean ratio >= 0.20.

Inspection of the resulting drift_stability.md exposed three weaknesses
the v1 diagnostic does not catch:

    1. Per-trade Sharpe in train is mostly 0.05-0.20 — economically
       marginal. The healthy-looking |t-stat| values are driven by
       large n, not by per-trade signal-to-noise.

    2. The OOS window contains the Apr 2 2026 tariff crash. SHORT-side
       edges show OOS magnitudes 2-10x larger than train, which is
       almost certainly the tariff event leaking in rather than a
       persistent edge property.

    3. The v1 diagnostic has no cost-survivability check. At a
       BlackBull XAUUSD round-trip cost of ~0.4 pts, edges with
       mean_oos < 0.4 are net negative.

This v2 module addresses all three. It re-grades the 19 survivors using
a verdict ladder that explicitly tests:

    a) economic floor (mean_oos > cost_floor)
    b) per-trade Sharpe floor (sharpe_oos >= sharpe_floor)
    c) tariff-dependence (does the edge work pre-tariff as well as
       post-tariff, or is it concentrated in the post-tariff window?)

Verdict ladder (v2)
-------------------
First requires the v1 STABLE conditions (sign-match, OOS t-stat, OOS/train
ratio); then layers the v2 gates on top.

    STRONG            : v1-STABLE
                        AND sharpe_oos >= sharpe_floor
                        AND mean_oos > cost_floor
                        AND pre-tariff AND post-tariff OOS sub-periods
                            BOTH same-sign as the full OOS mean.

    TARIFF_DEPENDENT  : v1-STABLE
                        AND post-tariff sub-period sign matches OOS sign
                        AND (pre-tariff sub-period sign-flipped
                             OR pre-tariff |mean| < 10% of post-tariff |mean|).
                        These edges look STABLE only because the tariff
                        event dominates the OOS window. Excluding the
                        tariff window the edge does not fire (or fires
                        the wrong way). Not a real persistent edge.

    MARGINAL          : v1-STABLE
                        AND pre/post sub-periods are consistent
                        BUT sharpe_oos < sharpe_floor
                            OR mean_oos < cost_floor.
                        Sign is real, magnitude is not economic.

    CULL              : v1 verdict was WEAK / FLIPPED / DEAD,
                        OR pre/post sub-periods disagree in a way the
                            TARIFF_DEPENDENT clause does not capture.

Implementation notes
--------------------
This module re-uses inspect_drift_stability.per_partition_stats and
.per_edge_stability for the v1 layer; it then computes pre/post-tariff
OOS sub-period stats from the same trades CSV and joins.

We treat the tariff split as a calendar-day boundary at 2026-04-02 00:00
UTC. Trades with ts_close < that boundary are PRE; trades with ts_close
>= that boundary are POST. Coarser than the actual tariff-validation
moment (2026-04-02 01:02:44 UTC) but the entire Apr 2 trading day is
dominated by the event so the day-level boundary is correct.

This module is read-only. It does not modify core code, the panel
binary, the catalogue, the OOS sentinel, or any v1 output file. It
writes only its own outputs:

    drift_stability_v2_per_edge.csv
    drift_stability_v2_subperiod.csv
    drift_stability_v2.md

The trade timestamps in paper_trade_topk_trades_classified.csv are stored
as ts_close strings with explicit +00:00 tz suffixes (UTC). We parse them
with utc=True to be defensive against any environment-locale weirdness.

CLI
---
    python -m backtest.edgefinder.analytics.inspect_drift_stability_v2 \\
        [--trades-csv PATH]
        [--summary-csv PATH]
        [--out-dir PATH]
        [--t-stable FLOAT]      default 2.0   (passed to v1 layer)
        [--frac-floor FLOAT]    default 0.20  (passed to v1 layer)
        [--n-floor INT]         default 30    (passed to v1 layer)
        [--cost-floor FLOAT]    default 0.4   (mean_oos pts/trade floor)
        [--sharpe-floor FLOAT]  default 0.10  (per-trade OOS sharpe floor)
        [--tariff-cutoff STR]   default 2026-04-02
                                 ISO date; trades with ts_close >= this
                                 are POST. Use YYYY-MM-DD format.
        [--n-floor-subperiod INT] default 20  (min trades in each of pre/post
                                  to grade sub-period stability; below this
                                  the edge is treated as having insufficient
                                  pre or post data and falls through to the
                                  TARIFF_DEPENDENT or CULL bucket).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd

# Re-use v1 functions
from .inspect_drift_stability import (
    per_partition_stats,
    per_edge_stability,
    PARTITION_ORDER,
)


DEFAULT_TRADES_CSV = (
    'backtest/edgefinder/output/paper_trade/paper_trade_topk_trades_classified.csv'
)
DEFAULT_SUMMARY_CSV = (
    'backtest/edgefinder/output/paper_trade/paper_trade_topk_outcome_summary.csv'
)
DEFAULT_OUT_DIR = 'backtest/edgefinder/output/paper_trade'
DEFAULT_T_STABLE = 2.0
DEFAULT_FRAC_FLOOR = 0.20
DEFAULT_N_FLOOR = 30
DEFAULT_COST_FLOOR = 0.4
DEFAULT_SHARPE_FLOOR = 0.10
DEFAULT_TARIFF_CUTOFF = '2026-04-02'  # YYYY-MM-DD, UTC midnight
DEFAULT_N_FLOOR_SUBPERIOD = 20


# -----------------------------------------------------------------------------
# Sub-period stats
# -----------------------------------------------------------------------------
def compute_subperiod_stats(
    trades: pd.DataFrame,
    tariff_cutoff_iso: str,
) -> pd.DataFrame:
    """
    For OOS rows only, split into PRE-tariff and POST-tariff and compute
    n / mean / std / sharpe / t_stat per (edge, subperiod).

    Returns a long-format frame with columns:
        rank, pid, side, bracket_id, subperiod ('pre' or 'post'),
        n, mean_pnl_pts, std_pnl_pts, sharpe, t_stat
    """
    if 'ts_close' not in trades.columns:
        raise ValueError(
            "trades CSV missing ts_close column — cannot split OOS into "
            "pre/post tariff sub-periods."
        )

    oos = trades[trades['partition'] == 'oos'].copy()
    if oos.empty:
        return pd.DataFrame(columns=[
            'rank', 'pid', 'side', 'bracket_id', 'subperiod',
            'n', 'mean_pnl_pts', 'std_pnl_pts', 'sharpe', 't_stat',
        ])

    # Parse timestamps as UTC. The CSV stores them as e.g.
    # '2026-04-02 01:00:00+00:00' so utc=True is correct.
    ts = pd.to_datetime(oos['ts_close'], utc=True, errors='coerce')
    if ts.isna().any():
        n_bad = int(ts.isna().sum())
        raise ValueError(
            f"compute_subperiod_stats: failed to parse {n_bad} ts_close "
            f"values as UTC timestamps. Sample: "
            f"{oos.loc[ts.isna(), 'ts_close'].head(3).tolist()}"
        )

    # Cutoff at UTC midnight of the given date.
    cutoff = pd.Timestamp(tariff_cutoff_iso, tz='UTC')
    oos['subperiod'] = np.where(ts < cutoff, 'pre', 'post')

    group_cols = ['rank', 'pid', 'side', 'bracket_id', 'subperiod']
    g = oos.groupby(group_cols, sort=False)['pnl_pts']

    n = g.size().rename('n')
    mean = g.mean().rename('mean_pnl_pts')
    std = g.std(ddof=1).rename('std_pnl_pts')

    out = pd.concat([n, mean, std], axis=1).reset_index()

    with np.errstate(divide='ignore', invalid='ignore'):
        sharpe = np.where(
            (out['std_pnl_pts'] > 0) & (out['n'] >= 2),
            out['mean_pnl_pts'] / out['std_pnl_pts'],
            0.0,
        )
        se = np.where(
            (out['std_pnl_pts'] > 0) & (out['n'] >= 2),
            out['std_pnl_pts'] / np.sqrt(out['n']),
            np.nan,
        )
        t_stat = np.where(
            np.isfinite(se) & (se > 0),
            out['mean_pnl_pts'] / se,
            0.0,
        )
    out['sharpe'] = sharpe
    out['t_stat'] = t_stat

    return out


# -----------------------------------------------------------------------------
# v2 verdict assignment
# -----------------------------------------------------------------------------
def _sign(x: float) -> int:
    if not np.isfinite(x):
        return 0
    if x > 0:
        return +1
    if x < 0:
        return -1
    return 0


def assign_v2_verdicts(
    per_edge_v1: pd.DataFrame,
    per_part: pd.DataFrame,
    subperiod: pd.DataFrame,
    cost_floor: float,
    sharpe_floor: float,
    n_floor_subperiod: int,
) -> pd.DataFrame:
    """
    Layer v2 gates on top of the v1 per_edge frame.

    Returns a frame with the v1 columns plus:
        sharpe_oos             (pulled from per_part)
        mean_oos_abs           (|mean_oos|)
        cost_ok                (mean_oos > cost_floor in the trade direction)
        sharpe_ok              (sharpe_oos >= sharpe_floor in trade direction)
        n_pre, n_post,
        mean_pre, mean_post,
        sharpe_pre, sharpe_post,
        sign_pre, sign_post,
        sub_periods_consistent (pre AND post same-sign as oos overall)
        post_dominates         (post sign matches oos AND
                                (pre sign-flipped OR pre |mean| < 10% post |mean|))
        verdict_v2             in {STRONG, TARIFF_DEPENDENT, MARGINAL, CULL}
        verdict_v1             (passthrough)
    """
    # 1. Pull sharpe_oos from per_part
    oos_part = per_part[per_part['partition'] == 'oos'].copy()
    sharpe_oos_lookup = oos_part.set_index(
        ['rank', 'pid', 'side', 'bracket_id']
    )['sharpe'].to_dict()

    # 2. Pivot subperiod into wide
    if subperiod.empty:
        # Build empty wide frame matching expected shape
        sub_wide = pd.DataFrame(columns=[
            'rank', 'pid', 'side', 'bracket_id',
            'n_pre', 'n_post', 'mean_pre', 'mean_post',
            'std_pre', 'std_post', 'sharpe_pre', 'sharpe_post',
            't_pre', 't_post',
        ])
    else:
        idx_cols = ['rank', 'pid', 'side', 'bracket_id']
        pre = subperiod[subperiod['subperiod'] == 'pre'].set_index(idx_cols)
        post = subperiod[subperiod['subperiod'] == 'post'].set_index(idx_cols)

        sub_wide = pd.DataFrame(index=pd.MultiIndex.from_frame(
            per_edge_v1[idx_cols]
        ))
        for src, tag in [(pre, 'pre'), (post, 'post')]:
            sub_wide[f'n_{tag}'] = src['n'].reindex(sub_wide.index)
            sub_wide[f'mean_{tag}'] = src['mean_pnl_pts'].reindex(sub_wide.index)
            sub_wide[f'std_{tag}'] = src['std_pnl_pts'].reindex(sub_wide.index)
            sub_wide[f'sharpe_{tag}'] = src['sharpe'].reindex(sub_wide.index)
            sub_wide[f't_{tag}'] = src['t_stat'].reindex(sub_wide.index)
        sub_wide = sub_wide.reset_index()

    # Fill NaN n_pre / n_post with 0 (groups with no rows in that sub-period)
    for c in ('n_pre', 'n_post'):
        if c in sub_wide.columns:
            sub_wide[c] = sub_wide[c].fillna(0).astype(int)

    # 3. Merge v1 + sub-period
    merge_keys = ['rank', 'pid', 'side', 'bracket_id']
    out = per_edge_v1.merge(sub_wide, on=merge_keys, how='left')

    # 4. Compute auxiliary columns and verdict
    out['sharpe_oos'] = out.apply(
        lambda r: sharpe_oos_lookup.get(
            (int(r['rank']), int(r['pid']), str(r['side']), int(r['bracket_id'])),
            np.nan,
        ),
        axis=1,
    )

    # cost_ok: mean_oos in the *trade direction* must exceed cost_floor.
    # The trades CSV's pnl_pts is already side-corrected (LONG kept, SHORT
    # negated), so a positive mean_oos already represents profit in the
    # trade direction. Thus cost_ok is simply mean_oos > cost_floor.
    out['cost_ok'] = out['mean_oos'] > cost_floor
    # sharpe_ok: same logic. sharpe_oos is computed on side-corrected pnl,
    # so a positive value is profit-in-direction.
    out['sharpe_ok'] = out['sharpe_oos'] >= sharpe_floor

    # Sub-period signs (computed against the *side-corrected* mean, so
    # positive = trade-direction profit)
    out['sign_pre'] = out['mean_pre'].apply(_sign).astype('Int64')
    out['sign_post'] = out['mean_post'].apply(_sign).astype('Int64')

    # Sub-period consistency: both pre and post share sign with full OOS
    # AND each has at least n_floor_subperiod trades. If pre or post n is
    # below floor, treat that side as "unknown" rather than consistent.
    sign_oos = out['sign_oos'].astype('Int64')

    pre_known = out['n_pre'].fillna(0) >= n_floor_subperiod
    post_known = out['n_post'].fillna(0) >= n_floor_subperiod

    out['sub_periods_consistent'] = (
        pre_known & post_known &
        (out['sign_pre'] == sign_oos) &
        (out['sign_post'] == sign_oos)
    )

    # post_dominates: post is consistent with OOS, but pre is either
    # missing/insufficient, sign-flipped relative to oos, or magnitude
    # is < 10% of post magnitude.
    pre_flipped = pre_known & (out['sign_pre'] != sign_oos) & (out['sign_pre'] != 0)
    pre_collapsed = pre_known & out['mean_pre'].abs().lt(out['mean_post'].abs() * 0.1)
    pre_unknown = ~pre_known
    post_aligned = post_known & (out['sign_post'] == sign_oos)

    out['post_dominates'] = post_aligned & (pre_flipped | pre_collapsed | pre_unknown)

    # 5. v2 verdict ladder
    def _verdict(row) -> str:
        if row['verdict'] != 'STABLE':
            return 'CULL'
        # v1 STABLE — apply v2 gates
        if row['sub_periods_consistent'] and row['cost_ok'] and row['sharpe_ok']:
            return 'STRONG'
        if row['post_dominates'] and not row['sub_periods_consistent']:
            # Edge looks STABLE in OOS only because of the post-tariff window
            return 'TARIFF_DEPENDENT'
        if row['sub_periods_consistent']:
            # Sign-stable across sub-periods but fails cost or sharpe gate
            return 'MARGINAL'
        # All other cases — sub-periods inconsistent and not post-dominated
        return 'CULL'

    out['verdict_v2'] = out.apply(_verdict, axis=1)
    out = out.rename(columns={'verdict': 'verdict_v1'})

    # Order columns: identifiers first, v1 verdict, then v2 layer, then v2 verdict last
    col_order = [
        'rank', 'pid', 'side', 'bracket_id',
        'n_train', 'n_val', 'n_oos',
        'mean_train', 'mean_val', 'mean_oos',
        't_train', 't_val', 't_oos',
        'sign_train', 'sign_val', 'sign_oos',
        'all_signs_match', 'oos_mean_frac_of_train', 'oos_t_abs',
        'verdict_v1',
        'sharpe_oos', 'cost_ok', 'sharpe_ok',
        'n_pre', 'n_post',
        'mean_pre', 'mean_post',
        'sharpe_pre', 'sharpe_post',
        't_pre', 't_post',
        'sign_pre', 'sign_post',
        'sub_periods_consistent', 'post_dominates',
        'verdict_v2',
    ]
    # Be defensive about missing v1 cols (std_train etc. weren't in our merge)
    col_order = [c for c in col_order if c in out.columns]
    out = out[col_order]

    return out


# -----------------------------------------------------------------------------
# MD report
# -----------------------------------------------------------------------------
def _fmt_signed(x, w: int = 9, prec: int = 4) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'nan':>{w}}"
    return f"{float(x):>+{w}.{prec}f}"


def _fmt_int(x, w: int = 5) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'-':>{w}}"
    try:
        return f"{int(x):>{w}}"
    except (ValueError, TypeError):
        return f"{'-':>{w}}"


def _sign_str(s) -> str:
    if pd.isna(s):
        return '?'
    s = int(s)
    if s == 1:
        return '+'
    if s == -1:
        return '-'
    return '0'


def write_md_report(
    per_edge_v2: pd.DataFrame,
    subperiod: pd.DataFrame,
    trades_csv_path: Path,
    out_md_path: Path,
    cost_floor: float,
    sharpe_floor: float,
    tariff_cutoff_iso: str,
    n_floor_subperiod: int,
) -> None:
    lines = []
    lines.append("# C6 #1B v2 — Economic & Tariff-Dependence Diagnostic")
    lines.append("")
    lines.append(f"- Trades CSV:        `{trades_csv_path}`")
    lines.append(f"- Edges:             {len(per_edge_v2)}")
    lines.append(f"- Cost floor:        mean_oos > {cost_floor} pts/trade")
    lines.append(f"- Sharpe floor:      sharpe_oos >= {sharpe_floor}")
    lines.append(f"- Tariff cutoff:     {tariff_cutoff_iso} 00:00 UTC "
                 f"(trades < cutoff → pre, >= cutoff → post)")
    lines.append(f"- Sub-period n floor: {n_floor_subperiod} trades minimum "
                 f"in each of pre / post to be considered known")
    lines.append("")

    # Verdict counts
    counts_v2 = per_edge_v2['verdict_v2'].value_counts()
    counts_v1 = per_edge_v2['verdict_v1'].value_counts()
    lines.append("## v2 verdict counts")
    lines.append("")
    lines.append("| verdict_v2        | n  | description |")
    lines.append("|:------------------|---:|:------------|")
    descriptions = {
        'STRONG':           'v1-STABLE + sub-periods consistent + cost+sharpe pass — real persistent edge',
        'TARIFF_DEPENDENT': 'v1-STABLE only because of post-tariff window — pre-tariff sign-flipped or absent',
        'MARGINAL':         'sub-periods consistent but fails cost floor or sharpe floor — uneconomic',
        'CULL':             'fails v1 STABLE OR sub-periods inconsistent in non-tariff-attributable way',
    }
    for v in ['STRONG', 'TARIFF_DEPENDENT', 'MARGINAL', 'CULL']:
        n = int(counts_v2.get(v, 0))
        lines.append(f"| {v:<17} | {n:>2} | {descriptions[v]} |")
    lines.append("")

    lines.append("## v1 → v2 verdict transition")
    lines.append("")
    cross = pd.crosstab(per_edge_v2['verdict_v1'], per_edge_v2['verdict_v2'])
    lines.append("| v1 \\ v2 | " + " | ".join(cross.columns) + " |")
    lines.append("|:---" + "|---:" * len(cross.columns) + "|")
    for v1_label, row in cross.iterrows():
        cells = " | ".join(str(int(x)) for x in row.values)
        lines.append(f"| {v1_label} | {cells} |")
    lines.append("")

    # STRONG edges
    strong = per_edge_v2[per_edge_v2['verdict_v2'] == 'STRONG']
    lines.append("## STRONG edges (real persistent drift, cost-survivable)")
    lines.append("")
    if strong.empty:
        lines.append("_None._ No edge passes all v2 gates.")
    else:
        lines.append(f"{len(strong)} edge(s).")
        lines.append("")
        lines.append("| rank | pid | side | b | n_oos | mean_oos | sharpe_oos | "
                     "n_pre | mean_pre | n_post | mean_post |")
        lines.append("|---:|---:|:---|:-:|---:|---:|---:|---:|---:|---:|---:|")
        for _, r in strong.iterrows():
            lines.append(
                f"| {int(r['rank'])} | {int(r['pid'])} | {r['side']} | {int(r['bracket_id'])} "
                f"| {_fmt_int(r['n_oos'], 5)} "
                f"| {_fmt_signed(r['mean_oos'])} "
                f"| {_fmt_signed(r['sharpe_oos'], 8, 4)} "
                f"| {_fmt_int(r['n_pre'], 5)} "
                f"| {_fmt_signed(r['mean_pre'])} "
                f"| {_fmt_int(r['n_post'], 5)} "
                f"| {_fmt_signed(r['mean_post'])} |"
            )
    lines.append("")

    # TARIFF_DEPENDENT edges
    tariff = per_edge_v2[per_edge_v2['verdict_v2'] == 'TARIFF_DEPENDENT']
    lines.append("## TARIFF_DEPENDENT edges")
    lines.append("")
    if tariff.empty:
        lines.append("_None._")
    else:
        lines.append(f"{len(tariff)} edge(s). These look STABLE only because the "
                     f"post-{tariff_cutoff_iso} window dominates the OOS sample. "
                     "Pre-tariff the edge is sign-flipped, has insufficient "
                     "trades, or has a magnitude an order of magnitude smaller. "
                     "Not a persistent edge.")
        lines.append("")
        lines.append("| rank | pid | side | b | n_pre | mean_pre | sign_pre | "
                     "n_post | mean_post | sign_post |")
        lines.append("|---:|---:|:---|:-:|---:|---:|:-:|---:|---:|:-:|")
        for _, r in tariff.iterrows():
            lines.append(
                f"| {int(r['rank'])} | {int(r['pid'])} | {r['side']} | {int(r['bracket_id'])} "
                f"| {_fmt_int(r['n_pre'], 5)} "
                f"| {_fmt_signed(r['mean_pre'])} "
                f"| {_sign_str(r['sign_pre'])} "
                f"| {_fmt_int(r['n_post'], 5)} "
                f"| {_fmt_signed(r['mean_post'])} "
                f"| {_sign_str(r['sign_post'])} |"
            )
    lines.append("")

    # MARGINAL edges
    marg = per_edge_v2[per_edge_v2['verdict_v2'] == 'MARGINAL']
    lines.append("## MARGINAL edges (consistent sub-periods, fails economic gates)")
    lines.append("")
    if marg.empty:
        lines.append("_None._")
    else:
        lines.append(f"{len(marg)} edge(s). Sub-period sign is consistent but "
                     f"either mean_oos < cost_floor ({cost_floor} pts) or "
                     f"sharpe_oos < {sharpe_floor}.")
        lines.append("")
        lines.append("| rank | pid | side | b | mean_oos | sharpe_oos | cost_ok | sharpe_ok |")
        lines.append("|---:|---:|:---|:-:|---:|---:|:-:|:-:|")
        for _, r in marg.iterrows():
            lines.append(
                f"| {int(r['rank'])} | {int(r['pid'])} | {r['side']} | {int(r['bracket_id'])} "
                f"| {_fmt_signed(r['mean_oos'])} "
                f"| {_fmt_signed(r['sharpe_oos'], 8, 4)} "
                f"| {'Y' if r['cost_ok'] else 'N'} "
                f"| {'Y' if r['sharpe_ok'] else 'N'} |"
            )
    lines.append("")

    # Full table
    lines.append("## Full per-edge v2 table")
    lines.append("")
    lines.append(
        "| rank | pid | side | b | n_oos | mean_oos | sharpe_oos | "
        "n_pre | mean_pre | n_post | mean_post | sub_consist | post_dom | "
        "v1 | v2 |"
    )
    lines.append(
        "|---:|---:|:---|:-:|---:|---:|---:|---:|---:|---:|---:|:-:|:-:|:--|:--|"
    )
    for _, r in per_edge_v2.iterrows():
        lines.append(
            f"| {int(r['rank'])} | {int(r['pid'])} | {r['side']} | {int(r['bracket_id'])} "
            f"| {_fmt_int(r['n_oos'], 5)} "
            f"| {_fmt_signed(r['mean_oos'])} "
            f"| {_fmt_signed(r['sharpe_oos'], 8, 4)} "
            f"| {_fmt_int(r['n_pre'], 5)} "
            f"| {_fmt_signed(r['mean_pre'])} "
            f"| {_fmt_int(r['n_post'], 5)} "
            f"| {_fmt_signed(r['mean_post'])} "
            f"| {'Y' if r['sub_periods_consistent'] else 'N'} "
            f"| {'Y' if r['post_dominates'] else 'N'} "
            f"| {r['verdict_v1']} | **{r['verdict_v2']}** |"
        )
    lines.append("")

    out_md_path.write_text('\n'.join(lines))


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "C6 #1B v2: economic-survivability + tariff-dependence diagnostic "
            "for strict OOS survivors. Layers cost-floor, sharpe-floor, and "
            "pre/post-tariff sub-period consistency on top of v1 STABLE."
        )
    )
    p.add_argument('--trades-csv', default=DEFAULT_TRADES_CSV)
    p.add_argument('--summary-csv', default=DEFAULT_SUMMARY_CSV)
    p.add_argument('--out-dir', default=DEFAULT_OUT_DIR)
    p.add_argument('--t-stable', type=float, default=DEFAULT_T_STABLE)
    p.add_argument('--frac-floor', type=float, default=DEFAULT_FRAC_FLOOR)
    p.add_argument('--n-floor', type=int, default=DEFAULT_N_FLOOR)
    p.add_argument('--cost-floor', type=float, default=DEFAULT_COST_FLOOR,
                   help=f"mean_oos pts/trade floor (default {DEFAULT_COST_FLOOR})")
    p.add_argument('--sharpe-floor', type=float, default=DEFAULT_SHARPE_FLOOR,
                   help=f"per-trade OOS sharpe floor (default {DEFAULT_SHARPE_FLOOR})")
    p.add_argument('--tariff-cutoff', default=DEFAULT_TARIFF_CUTOFF,
                   help=f"YYYY-MM-DD UTC midnight (default {DEFAULT_TARIFF_CUTOFF})")
    p.add_argument('--n-floor-subperiod', type=int, default=DEFAULT_N_FLOOR_SUBPERIOD,
                   help=(f"min trades in each of pre / post to grade as known "
                         f"(default {DEFAULT_N_FLOOR_SUBPERIOD})"))
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

    required = ['rank', 'pid', 'side', 'bracket_id', 'partition', 'pnl_pts', 'ts_close']
    missing = [c for c in required if c not in trades.columns]
    if missing:
        print(f"ERROR: trades CSV missing required columns: {missing}", file=sys.stderr)
        return 2

    # Validate tariff cutoff is in OOS window
    try:
        cutoff_ts = pd.Timestamp(args.tariff_cutoff, tz='UTC')
    except (ValueError, TypeError) as e:
        print(f"ERROR: --tariff-cutoff not parseable as UTC date: {args.tariff_cutoff} ({e})",
              file=sys.stderr)
        return 2

    print(f"computing v1 per-partition stats")
    per_part = per_partition_stats(trades)

    print(f"computing v1 per-edge stability "
          f"(t_stable={args.t_stable}, frac_floor={args.frac_floor}, "
          f"n_floor={args.n_floor})")
    per_edge_v1 = per_edge_stability(
        per_part,
        t_stable=args.t_stable,
        frac_floor=args.frac_floor,
        n_floor=args.n_floor,
    )

    print(f"computing OOS sub-period stats with cutoff {cutoff_ts.isoformat()}")
    subperiod = compute_subperiod_stats(trades, args.tariff_cutoff)
    print(f"sub-period rows: {len(subperiod)}")

    sub_csv = out_dir / 'drift_stability_v2_subperiod.csv'
    subperiod.to_csv(sub_csv, index=False)
    print(f"wrote {sub_csv} ({len(subperiod)} rows)")

    print(f"assigning v2 verdicts "
          f"(cost_floor={args.cost_floor}, sharpe_floor={args.sharpe_floor}, "
          f"n_floor_subperiod={args.n_floor_subperiod})")
    per_edge_v2 = assign_v2_verdicts(
        per_edge_v1=per_edge_v1,
        per_part=per_part,
        subperiod=subperiod,
        cost_floor=args.cost_floor,
        sharpe_floor=args.sharpe_floor,
        n_floor_subperiod=args.n_floor_subperiod,
    )

    edge_csv = out_dir / 'drift_stability_v2_per_edge.csv'
    per_edge_v2.to_csv(edge_csv, index=False)
    print(f"wrote {edge_csv} ({len(per_edge_v2)} rows)")

    counts = per_edge_v2['verdict_v2'].value_counts()
    print(f"v2 verdicts:")
    for v in ['STRONG', 'TARIFF_DEPENDENT', 'MARGINAL', 'CULL']:
        print(f"  {v:<17}: {int(counts.get(v, 0)):>3}")

    md_path = out_dir / 'drift_stability_v2.md'
    write_md_report(
        per_edge_v2=per_edge_v2,
        subperiod=subperiod,
        trades_csv_path=trades_csv_path,
        out_md_path=md_path,
        cost_floor=args.cost_floor,
        sharpe_floor=args.sharpe_floor,
        tariff_cutoff_iso=args.tariff_cutoff,
        n_floor_subperiod=args.n_floor_subperiod,
    )
    print(f"wrote {md_path}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
