"""
backtest/edgefinder/analytics/paper_trade_topk.py
==================================================

C6 thread #1 — paper-trade simulator for strict OOS survivors.

Purpose
-------
The C5 step produced 19 strict OOS survivors and a top-K ranked list. The
scoring code (prospect.py) averages bracket PnL across all bars where the
predicate fires; it does NOT expose:

    - the chronological trade list,
    - equity curve shape,
    - peak-trough drawdown,
    - win/loss streaks,
    - trade-clustering (when do edges fire together?),
    - cross-edge correlation (do they move as one book or independently?).

This module is a read-only "trade journal" replay. For each selected
survivor it walks the panel chronologically, records every bar where the
predicate fires AND warmed_up=1 AND fwd_complete=1 AND regime matches, and
treats that bar as one paper trade with realised PnL = fwd_bracket_pts_<b>
(negated for SHORT). It then aggregates per-edge metrics, builds an
equal-weight portfolio across the selected edges, and writes a per-trade
CSV, a per-edge metrics parquet, and a human-readable MD report.

It is read-only on:
    - the panel binary (load.py)
    - catalogue.json / catalogue.pkl (workdir)
    - c5_topk_survivors.csv (or any compatible candidate CSV)

It does NOT touch the OOS sentinel. Slicing OOS is done with the same
boundary expression as oos_partition() but WITHOUT calling oos_partition()
(which stamps the sentinel on every read). The OOS sentinel is already
burned at 2026-04-27T08:52:53Z; this tool is replay, not a re-burn.

It does NOT modify any core code.

Inputs
------
Default candidate CSV (matches inspect_c5_topk.py output):
    backtest/edgefinder/output/inspect_c4/c5_topk_survivors.csv

Required columns:
    pid, side, bracket_id, feature, op, threshold,
    regime_session, regime_vol, regime_trend, rank

Outputs (all under backtest/edgefinder/output/paper_trade/):
    paper_trade_topk_trades.csv      per-trade records
    paper_trade_topk_summary.parquet per-edge metrics
    paper_trade_topk.md              human-readable report

CLI
---
    python -m backtest.edgefinder.analytics.paper_trade_topk \\
        [--candidates-file PATH] \\
        [--top-k N]                    default 3
        [--partitions train,val,oos]   default all three
        [--panel PATH]                 default DEFAULT_PANEL
        [--workdir PATH]               default DEFAULT_WORKDIR
        [--out-dir PATH]               default backtest/edgefinder/output/paper_trade
        [--point-value-usd FLOAT]      default 1.0 (1 USD per point per lot,
                                       used for equity-curve dollar conversion;
                                       set to your real point value to read in $)

Notes on equity curve interpretation
------------------------------------
Per-trade PnL is in points (fwd_bracket_pts). The equity curve is the
running sum of those points. Drawdown is computed as the largest negative
deviation from the running maximum. Convert to dollars by multiplying by
--point-value-usd × position size; left to the user. The portfolio curve
is the equal-weighted sum of per-edge equity curves (overlapping trades
are simply additive in points — there is no margin model here).

Trade overlap analysis
----------------------
We report the maximum number of concurrently-open trades across all edges
based on bracket horizon. Bracket horizon comes from BRACKET_SPECS in
load.py (5 / 15 / 60 / 240 minutes depending on bracket_id). A trade
"opens" at its bar's ts_close and "closes" at ts_close + horizon. We do
NOT attempt to use first_touch_*m to refine the close time; the bracket
horizon is the worst-case holding period and is the right ceiling for
overlap accounting.
"""
from __future__ import annotations

import argparse
import pickle
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

from .load import load_panel, BRACKET_SPECS
from .regime import (
    CatalogueArtifact,
    PredicateSpec,
    apply_regimes,
    predicate_mask,
)
from .walkforward import PartitionBounds


# -----------------------------------------------------------------------------
# Defaults — mirror cli.py so behaviour is consistent
# -----------------------------------------------------------------------------
DEFAULT_PANEL = 'backtest/edgefinder/output/bars_xauusd_full.bin'
DEFAULT_WORKDIR = 'backtest/edgefinder/output/work'
DEFAULT_OUT_DIR = 'backtest/edgefinder/output/paper_trade'
DEFAULT_CANDIDATES = 'backtest/edgefinder/output/inspect_c4/c5_topk_survivors.csv'

CATALOGUE_JSON = 'catalogue.json'
CATALOGUE_PICKLE = 'catalogue.pkl'

REQUIRED_CANDIDATE_COLUMNS = (
    'pid', 'side', 'bracket_id',
    'feature', 'op', 'threshold',
    'regime_session', 'regime_vol', 'regime_trend',
)


def _ts() -> str:
    return time.strftime('%H:%M:%S')


# -----------------------------------------------------------------------------
# Edge identity / labels
# -----------------------------------------------------------------------------
@dataclass(frozen=True)
class EdgeKey:
    """Unique identifier for one selected survivor edge."""
    rank: int
    pid: int
    side: str
    bracket_id: int

    @property
    def label(self) -> str:
        return f"r{self.rank:02d}_pid{self.pid}_{self.side}_b{self.bracket_id}"


def _row_to_spec(row: pd.Series) -> PredicateSpec:
    """Reconstruct a PredicateSpec from a candidate-CSV row."""
    return PredicateSpec(
        pid=int(row['pid']),
        feature=str(row['feature']),
        op=str(row['op']),
        threshold=float(row['threshold']),
        regime_session=str(row['regime_session']),
        regime_vol=str(row['regime_vol']),
        regime_trend=str(row['regime_trend']),
    )


def _row_to_edgekey(row: pd.Series) -> EdgeKey:
    return EdgeKey(
        rank=int(row['rank']) if 'rank' in row.index and pd.notna(row['rank']) else 0,
        pid=int(row['pid']),
        side=str(row['side']),
        bracket_id=int(row['bracket_id']),
    )


# -----------------------------------------------------------------------------
# Trade extraction
# -----------------------------------------------------------------------------
def _extract_trades_for_edge(
    df_part: pd.DataFrame,
    spec: PredicateSpec,
    edge: EdgeKey,
    partition_label: str,
    catalogue_pid: int,
) -> pd.DataFrame:
    """
    Return a DataFrame, one row per bar where the predicate fires AND
    bracket PnL is finite. Columns:

        ts_close, partition, edge_label, rank, pid, side, bracket_id,
        feature, op, threshold,
        regime_session, regime_vol, regime_trend,
        feature_value, pnl_pts,
        bracket_horizon_min, bracket_sl_pts, bracket_tp_pts,
        ts_close_estimated_exit
    """
    if catalogue_pid != edge.pid:
        # _row_to_spec was passed a row whose pid disagrees with the
        # catalogue_pid we resolved through catalogue_by_pid; refuse silently
        # and emit zero trades. The CSV-vs-catalogue audit happens upstream.
        return _empty_trades_df()

    m = predicate_mask(df_part, spec)
    if not m.any():
        return _empty_trades_df()

    bracket_col = f'fwd_bracket_pts_{edge.bracket_id}'
    if bracket_col not in df_part.columns:
        raise ValueError(
            f"panel missing bracket column {bracket_col} for edge {edge.label}"
        )
    pnl_long = df_part[bracket_col].to_numpy(dtype=np.float64)
    finite = np.isfinite(pnl_long)
    use = m & finite
    if not use.any():
        return _empty_trades_df()

    side_sign = +1.0 if edge.side == 'LONG' else -1.0
    pnl_pts = pnl_long[use] * side_sign

    ts = df_part.index[use]
    feat_vals = df_part[spec.feature].to_numpy(dtype=np.float64)[use]

    bspec = BRACKET_SPECS[edge.bracket_id]
    horizon_min = int(bspec['horizon_min'])
    sl_pts = float(bspec['sl_pts'])
    tp_pts = float(bspec['tp_pts'])

    estimated_exit = ts + pd.Timedelta(minutes=horizon_min)

    out = pd.DataFrame({
        'ts_close': ts,
        'partition': partition_label,
        'edge_label': edge.label,
        'rank': edge.rank,
        'pid': edge.pid,
        'side': edge.side,
        'bracket_id': edge.bracket_id,
        'feature': spec.feature,
        'op': spec.op,
        'threshold': spec.threshold,
        'regime_session': spec.regime_session,
        'regime_vol': spec.regime_vol,
        'regime_trend': spec.regime_trend,
        'feature_value': feat_vals,
        'pnl_pts': pnl_pts,
        'bracket_horizon_min': horizon_min,
        'bracket_sl_pts': sl_pts,
        'bracket_tp_pts': tp_pts,
        'ts_close_estimated_exit': estimated_exit,
    })
    return out


def _empty_trades_df() -> pd.DataFrame:
    return pd.DataFrame(columns=[
        'ts_close', 'partition', 'edge_label', 'rank', 'pid', 'side',
        'bracket_id', 'feature', 'op', 'threshold',
        'regime_session', 'regime_vol', 'regime_trend',
        'feature_value', 'pnl_pts',
        'bracket_horizon_min', 'bracket_sl_pts', 'bracket_tp_pts',
        'ts_close_estimated_exit',
    ])


# -----------------------------------------------------------------------------
# Per-edge metrics
# -----------------------------------------------------------------------------
def _summarise_edge(trades: pd.DataFrame) -> dict:
    """
    Compute aggregate metrics for a single edge across all the trades
    captured for it. `trades` may span multiple partitions — the caller is
    responsible for filtering if a partition-specific summary is wanted.
    """
    n = len(trades)
    if n == 0:
        return dict(
            n=0, hit_rate=0.0, expectancy=0.0, std=0.0, sharpe=0.0,
            sum_pnl_pts=0.0, pf=0.0, max_dd_pts=0.0, max_dd_frac=0.0,
            longest_win_streak=0, longest_loss_streak=0,
            best_trade_pts=0.0, worst_trade_pts=0.0,
            first_ts=None, last_ts=None,
        )

    pnl = trades['pnl_pts'].to_numpy(dtype=np.float64)
    n_int = int(n)
    wins = pnl[pnl > 0]
    losses = pnl[pnl < 0]
    sum_w = float(np.sum(wins)) if wins.size else 0.0
    sum_l = float(np.sum(losses)) if losses.size else 0.0
    sum_pnl = float(np.sum(pnl))
    mean = float(np.mean(pnl))
    std = float(np.std(pnl, ddof=1)) if n_int > 1 else 0.0
    sharpe = (mean / std) if std > 0 else 0.0
    pf = (sum_w / abs(sum_l)) if sum_l < 0 else float('inf')
    hit_rate = float(wins.size) / float(n_int)

    # Equity curve in chronological order — sort by ts_close defensively.
    trades_sorted = trades.sort_values('ts_close')
    pnl_sorted = trades_sorted['pnl_pts'].to_numpy(dtype=np.float64)
    equity = np.cumsum(pnl_sorted)
    running_max = np.maximum.accumulate(equity)
    drawdown = equity - running_max  # zero or negative
    max_dd = float(drawdown.min()) if drawdown.size else 0.0
    peak = float(running_max.max()) if running_max.size and running_max.max() > 0 else 0.0
    max_dd_frac = (abs(max_dd) / peak) if peak > 0 else 0.0

    # Streaks
    sign = np.sign(pnl_sorted)
    longest_w = 0
    longest_l = 0
    cur_w = 0
    cur_l = 0
    for s in sign:
        if s > 0:
            cur_w += 1
            cur_l = 0
            if cur_w > longest_w:
                longest_w = cur_w
        elif s < 0:
            cur_l += 1
            cur_w = 0
            if cur_l > longest_l:
                longest_l = cur_l
        else:
            cur_w = 0
            cur_l = 0

    return dict(
        n=n_int, hit_rate=hit_rate, expectancy=mean, std=std, sharpe=sharpe,
        sum_pnl_pts=sum_pnl, pf=pf,
        max_dd_pts=max_dd, max_dd_frac=max_dd_frac,
        longest_win_streak=longest_w, longest_loss_streak=longest_l,
        best_trade_pts=float(pnl.max()), worst_trade_pts=float(pnl.min()),
        first_ts=trades_sorted['ts_close'].iloc[0],
        last_ts=trades_sorted['ts_close'].iloc[-1],
    )


# -----------------------------------------------------------------------------
# Portfolio overlap accounting
# -----------------------------------------------------------------------------
def _max_concurrent_open(trades: pd.DataFrame) -> tuple[int, pd.Timestamp | None]:
    """
    Sweep-line over (open, close) intervals to find max concurrent positions.
    Returns (max_count, ts_at_max).
    """
    if trades.empty:
        return 0, None
    events = []
    for _, r in trades[['ts_close', 'ts_close_estimated_exit']].iterrows():
        events.append((r['ts_close'], +1))
        events.append((r['ts_close_estimated_exit'], -1))
    # Process closes before opens at identical ts to avoid spurious overlap.
    events.sort(key=lambda e: (e[0], -e[1]))
    cur = 0
    best = 0
    best_ts = events[0][0]
    for ts, delta in events:
        cur += delta
        if cur > best:
            best = cur
            best_ts = ts
    return best, best_ts


# -----------------------------------------------------------------------------
# Cross-edge correlation
# -----------------------------------------------------------------------------
def _edge_correlation_matrix(
    trades_all: pd.DataFrame,
    edges: list[EdgeKey],
    bin_freq: str = '1D',
) -> pd.DataFrame:
    """
    Bin per-edge PnL into time buckets (default daily) and compute the
    Pearson correlation matrix of those daily PnL series. Edges that never
    fire on the same day still get a correlation of 0 (not NaN), which is
    the conservative interpretation.
    """
    if not edges or trades_all.empty:
        return pd.DataFrame()
    series_by_label: dict[str, pd.Series] = {}
    for e in edges:
        sub = trades_all[trades_all['edge_label'] == e.label]
        if sub.empty:
            continue
        s = (sub.set_index('ts_close')['pnl_pts']
                .resample(bin_freq).sum())
        series_by_label[e.label] = s
    if not series_by_label:
        return pd.DataFrame()
    full = pd.concat(series_by_label, axis=1).fillna(0.0)
    return full.corr().fillna(0.0)


# -----------------------------------------------------------------------------
# Reporting
# -----------------------------------------------------------------------------
def _format_pts(v: float) -> str:
    if pd.isna(v):
        return ''
    return f"{v:+.2f}"


def _write_md(
    md_path: Path,
    candidates_path: Path,
    top_k: int,
    partitions: list[str],
    edges: list[EdgeKey],
    per_edge_overall: pd.DataFrame,
    per_edge_by_partition: pd.DataFrame,
    portfolio_overall: dict,
    portfolio_by_partition: pd.DataFrame,
    max_concurrent: int,
    max_concurrent_ts: pd.Timestamp | None,
    corr: pd.DataFrame,
    point_value_usd: float,
) -> None:
    lines: list[str] = []
    lines.append("# C6 #1 — Paper-Trade Replay of Top-K OOS Survivors")
    lines.append("")
    lines.append(f"- Candidates source: `{candidates_path}`")
    lines.append(f"- Selected top-K: **{top_k}**")
    lines.append(f"- Partitions replayed: **{', '.join(partitions)}**")
    lines.append(f"- Point value (USD/pt): **{point_value_usd:.4f}**")
    lines.append("")
    lines.append("Read-only replay. The OOS sentinel was NOT touched by this run.")
    lines.append("PnL is in points; multiply by `point_value_usd × lots` for USD.")
    lines.append("")

    # Edge selection
    lines.append("## Edges replayed")
    lines.append("")
    lines.append("| rank | pid | side | b | feature | op | threshold | regime |")
    lines.append("|----:|----:|------|--:|---------|----|-----------|--------|")
    for e in edges:
        sub = per_edge_overall[per_edge_overall['edge_label'] == e.label]
        if sub.empty:
            continue
        r = sub.iloc[0]
        regime = f"{r['regime_session']}/{r['regime_vol']}/{r['regime_trend']}"
        lines.append(
            f"| {e.rank} | {e.pid} | {e.side} | {e.bracket_id} "
            f"| `{r['feature']}` | {r['op']} | {r['threshold']:.4g} "
            f"| {regime} |"
        )
    lines.append("")

    # Per-edge overall metrics
    lines.append("## Per-edge metrics (all replayed partitions)")
    lines.append("")
    lines.append("| edge | n | hit | E[pnl] pts | sharpe | pf | sum pts | "
                 "max DD pts | win streak | loss streak | first | last |")
    lines.append("|------|--:|----:|-----------:|-------:|---:|--------:|"
                 "----------:|----------:|-----------:|-------|------|")
    for _, r in per_edge_overall.iterrows():
        first = (str(r['first_ts'])[:10] if pd.notna(r['first_ts']) else '')
        last  = (str(r['last_ts'])[:10]  if pd.notna(r['last_ts'])  else '')
        lines.append(
            f"| `{r['edge_label']}` "
            f"| {int(r['n'])} "
            f"| {r['hit_rate']*100:.1f}% "
            f"| {_format_pts(r['expectancy'])} "
            f"| {r['sharpe']:+.3f} "
            f"| {r['pf']:.2f} "
            f"| {_format_pts(r['sum_pnl_pts'])} "
            f"| {_format_pts(r['max_dd_pts'])} "
            f"| {int(r['longest_win_streak'])} "
            f"| {int(r['longest_loss_streak'])} "
            f"| {first} | {last} |"
        )
    lines.append("")

    # Per-edge by partition
    lines.append("## Per-edge metrics broken down by partition")
    lines.append("")
    lines.append("| edge | partition | n | hit | E[pnl] pts | sharpe | pf | sum pts | max DD pts |")
    lines.append("|------|-----------|--:|----:|-----------:|-------:|---:|--------:|----------:|")
    for _, r in per_edge_by_partition.iterrows():
        lines.append(
            f"| `{r['edge_label']}` "
            f"| {r['partition']} "
            f"| {int(r['n'])} "
            f"| {r['hit_rate']*100:.1f}% "
            f"| {_format_pts(r['expectancy'])} "
            f"| {r['sharpe']:+.3f} "
            f"| {r['pf']:.2f} "
            f"| {_format_pts(r['sum_pnl_pts'])} "
            f"| {_format_pts(r['max_dd_pts'])} |"
        )
    lines.append("")

    # Portfolio
    lines.append("## Equal-weight portfolio across all selected edges")
    lines.append("")
    lines.append("Trades are concatenated chronologically and PnL is summed in points. "
                 "No margin model, no position sizing — a +1 unit per trade for every edge.")
    lines.append("")
    lines.append("### Overall")
    lines.append("")
    p = portfolio_overall
    lines.append(f"- Total trades:       **{p['n']}**")
    lines.append(f"- Hit rate:           **{p['hit_rate']*100:.1f}%**")
    lines.append(f"- Expectancy/trade:   **{_format_pts(p['expectancy'])} pts**")
    lines.append(f"- Sharpe (per-trade): **{p['sharpe']:+.3f}**")
    lines.append(f"- Profit factor:      **{p['pf']:.2f}**")
    lines.append(f"- Sum PnL:            **{_format_pts(p['sum_pnl_pts'])} pts** "
                 f"(${p['sum_pnl_pts']*point_value_usd:+.2f} at given pt value)")
    lines.append(f"- Max drawdown:       **{_format_pts(p['max_dd_pts'])} pts** "
                 f"(**{p['max_dd_frac']*100:.1f}%** of peak equity)")
    lines.append(f"- Longest win streak: **{p['longest_win_streak']}**")
    lines.append(f"- Longest loss streak:**{p['longest_loss_streak']}**")
    lines.append(f"- Best single trade:  **{_format_pts(p['best_trade_pts'])} pts**")
    lines.append(f"- Worst single trade: **{_format_pts(p['worst_trade_pts'])} pts**")
    if max_concurrent_ts is not None:
        lines.append(f"- Max concurrent open positions: **{max_concurrent}** "
                     f"(at {max_concurrent_ts})")
    else:
        lines.append(f"- Max concurrent open positions: **0**")
    lines.append("")

    lines.append("### Portfolio by partition")
    lines.append("")
    lines.append("| partition | n | hit | E[pnl] pts | sharpe | pf | sum pts | max DD pts |")
    lines.append("|-----------|--:|----:|-----------:|-------:|---:|--------:|----------:|")
    for _, r in portfolio_by_partition.iterrows():
        lines.append(
            f"| {r['partition']} "
            f"| {int(r['n'])} "
            f"| {r['hit_rate']*100:.1f}% "
            f"| {_format_pts(r['expectancy'])} "
            f"| {r['sharpe']:+.3f} "
            f"| {r['pf']:.2f} "
            f"| {_format_pts(r['sum_pnl_pts'])} "
            f"| {_format_pts(r['max_dd_pts'])} |"
        )
    lines.append("")

    # Correlation
    if not corr.empty:
        lines.append("## Daily PnL correlation matrix between edges")
        lines.append("")
        lines.append("Daily-bucketed PnL series, Pearson correlation. Days where an edge "
                     "did not fire are filled with 0.0 (conservative; treats no-fire as "
                     "no-PnL rather than missing-data).")
        lines.append("")
        cols = list(corr.columns)
        header = "| edge | " + " | ".join(f"`{c}`" for c in cols) + " |"
        sep    = "|------|" + "|".join(["---:"] * len(cols)) + "|"
        lines.append(header)
        lines.append(sep)
        for idx in corr.index:
            row_vals = " | ".join(f"{corr.loc[idx, c]:+.3f}" for c in cols)
            lines.append(f"| `{idx}` | {row_vals} |")
        lines.append("")

    # Notes
    lines.append("## Interpretation notes")
    lines.append("")
    lines.append("- Replay matches the C5 OOS scoring exactly on the OOS partition — "
                 "any divergence between this report's OOS row and the C5 OOS metrics "
                 "is a bug, not an edge property. Reconcile if observed.")
    lines.append("- TRAIN and VAL numbers here are *replay*, not the original "
                 "scan_train / gate_val numbers. They use OOS-style aggregation "
                 "(no MTC, no sign-flip gating) and so will differ from the "
                 "original train_prospects / val_survivors metrics. They are "
                 "included to show the full equity arc of these edges, not as a "
                 "re-validation.")
    lines.append("- Drawdown is in points and is computed against the running peak of "
                 "the strategy equity curve (not against initial capital).")
    lines.append("- Bracket horizons are upper bounds on holding time. Realised holding "
                 "is shorter when SL/TP/first_touch resolves earlier; the panel does "
                 "not expose realised holding time per bar.")
    lines.append("")

    md_path.write_text('\n'.join(lines) + '\n')


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
def _slice_partition(
    df: pd.DataFrame,
    bounds: PartitionBounds,
    label: str,
) -> pd.DataFrame:
    """
    Read-only partition slicer. Mirrors the index expressions in
    walkforward.py but does NOT call oos_partition() (which stamps the
    sentinel on every read). The OOS sentinel is already burned and is
    not this tool's concern to maintain.
    """
    if label == 'train':
        return df[df.index <= bounds.train_to]
    if label == 'val':
        return df[(df.index > bounds.train_to) & (df.index <= bounds.val_to)]
    if label == 'oos':
        return df[(df.index > bounds.val_to) & (df.index <= bounds.oos_to)]
    raise ValueError(f"unknown partition label: {label}")


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog='paper_trade_topk',
        description='Read-only paper-trade replay of strict OOS survivors.',
    )
    p.add_argument('--candidates-file', default=DEFAULT_CANDIDATES,
                   help=f'CSV of candidates (default: {DEFAULT_CANDIDATES})')
    p.add_argument('--top-k', type=int, default=3,
                   help='replay only the top-K rows by `rank` column (default 3); '
                        'set 0 or negative to replay all rows')
    p.add_argument('--partitions', default='train,val,oos',
                   help='comma-separated subset of {train,val,oos} (default all three)')
    p.add_argument('--panel', default=DEFAULT_PANEL)
    p.add_argument('--workdir', default=DEFAULT_WORKDIR)
    p.add_argument('--out-dir', default=DEFAULT_OUT_DIR)
    p.add_argument('--point-value-usd', type=float, default=1.0,
                   help='USD per point per lot for dollar conversions in MD report '
                        '(default 1.0; replace with your real point value to read $)')
    args = p.parse_args(argv)

    cand_path = Path(args.candidates_file)
    if not cand_path.exists():
        print(f"ERROR: candidates file not found: {cand_path}", file=sys.stderr)
        return 2

    parts_req = [s.strip().lower() for s in args.partitions.split(',') if s.strip()]
    for x in parts_req:
        if x not in ('train', 'val', 'oos'):
            print(f"ERROR: unknown partition '{x}' "
                  f"(must be subset of train,val,oos)", file=sys.stderr)
            return 2

    workdir = Path(args.workdir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # ------------------------------------------------------------------
    # Load candidates and sanity-check schema
    # ------------------------------------------------------------------
    print(f"[{_ts()}] loading candidates: {cand_path}")
    cand = pd.read_csv(cand_path)
    missing = [c for c in REQUIRED_CANDIDATE_COLUMNS if c not in cand.columns]
    if missing:
        print(f"ERROR: candidates file missing required columns: {missing}",
              file=sys.stderr)
        return 2

    if 'rank' not in cand.columns:
        # Tolerate rank-less inputs by assigning row order as rank.
        cand = cand.copy()
        cand['rank'] = np.arange(1, len(cand) + 1)

    cand = cand.sort_values('rank').reset_index(drop=True)
    if args.top_k and args.top_k > 0:
        cand = cand.head(args.top_k).copy()
    print(f"[{_ts()}] candidates selected: {len(cand)} rows "
          f"(top-K = {args.top_k if args.top_k > 0 else 'all'})")

    # ------------------------------------------------------------------
    # Load catalogue + verify pids
    # ------------------------------------------------------------------
    cat_json = workdir / CATALOGUE_JSON
    cat_pkl  = workdir / CATALOGUE_PICKLE
    if not cat_json.exists() or not cat_pkl.exists():
        print(f"ERROR: catalogue artefacts not found in {workdir} "
              f"(need both {CATALOGUE_JSON} and {CATALOGUE_PICKLE}).",
              file=sys.stderr)
        return 2
    art = CatalogueArtifact.load(cat_json)
    with open(cat_pkl, 'rb') as f:
        catalogue: list[PredicateSpec] = pickle.load(f)
    catalogue_by_pid: dict[int, PredicateSpec] = {s.pid: s for s in catalogue}

    cand_pids = set(int(p) for p in cand['pid'].unique())
    orphan = cand_pids - set(catalogue_by_pid.keys())
    if orphan:
        print(f"ERROR: {len(orphan)} candidate pids absent from catalogue. "
              f"Refusing to replay against the wrong catalogue.",
              file=sys.stderr)
        print(f"           sample orphan pids: {sorted(orphan)[:10]}",
              file=sys.stderr)
        return 2

    # ------------------------------------------------------------------
    # Cross-check: candidate-CSV (feature, op, threshold, regime_*) must
    # match the catalogue spec for the same pid. If they don't, the CSV
    # came from a different catalogue and replay results will be wrong.
    # ------------------------------------------------------------------
    spec_mismatches = []
    for _, r in cand.iterrows():
        spec = catalogue_by_pid[int(r['pid'])]
        if (spec.feature        != str(r['feature'])        or
            spec.op             != str(r['op'])             or
            abs(float(spec.threshold) - float(r['threshold'])) > 1e-9 or
            spec.regime_session != str(r['regime_session']) or
            spec.regime_vol     != str(r['regime_vol'])     or
            spec.regime_trend   != str(r['regime_trend'])):
            spec_mismatches.append(int(r['pid']))
    if spec_mismatches:
        print(f"ERROR: {len(spec_mismatches)} candidate row(s) disagree with "
              f"the catalogue on (feature/op/threshold/regime). The CSV is "
              f"stale or from a different catalogue. Refusing to replay.",
              file=sys.stderr)
        print(f"           sample pids: {spec_mismatches[:10]}",
              file=sys.stderr)
        return 2

    # ------------------------------------------------------------------
    # Load panel + apply regimes
    # ------------------------------------------------------------------
    print(f"[{_ts()}] loading panel: {args.panel}")
    df = load_panel(args.panel)
    print(f"[{_ts()}] panel rows={len(df)} cols={len(df.columns)}")
    df = apply_regimes(df, art.regime_cuts)

    bounds = PartitionBounds.from_iso(
        art.train_to_iso, art.val_to_iso, art.oos_to_iso,
    )

    # ------------------------------------------------------------------
    # Replay each (edge, partition)
    # ------------------------------------------------------------------
    edges: list[EdgeKey] = [_row_to_edgekey(r) for _, r in cand.iterrows()]
    edges_by_label: dict[str, EdgeKey] = {e.label: e for e in edges}
    spec_by_label: dict[str, PredicateSpec] = {
        e.label: _row_to_spec(cand.iloc[i])
        for i, e in enumerate(edges)
    }

    all_trade_frames: list[pd.DataFrame] = []
    for part_label in parts_req:
        df_part = _slice_partition(df, bounds, part_label)
        print(f"[{_ts()}] partition {part_label}: rows={len(df_part)} "
              f"({df_part.index.min() if len(df_part) else 'n/a'} .. "
              f"{df_part.index.max() if len(df_part) else 'n/a'})")
        if len(df_part) == 0:
            continue
        for e in edges:
            spec = spec_by_label[e.label]
            tdf = _extract_trades_for_edge(
                df_part, spec, e,
                partition_label=part_label,
                catalogue_pid=catalogue_by_pid[e.pid].pid,
            )
            all_trade_frames.append(tdf)

    if all_trade_frames:
        trades_all = pd.concat(all_trade_frames, ignore_index=True)
    else:
        trades_all = _empty_trades_df()

    print(f"[{_ts()}] total trades across all (edge, partition): {len(trades_all)}")

    # ------------------------------------------------------------------
    # Per-edge metrics — overall and by partition
    # ------------------------------------------------------------------
    per_edge_overall_rows = []
    per_edge_by_partition_rows = []
    for e in edges:
        sub = trades_all[trades_all['edge_label'] == e.label]
        m = _summarise_edge(sub)
        m['edge_label'] = e.label
        m['rank']       = e.rank
        m['pid']        = e.pid
        m['side']       = e.side
        m['bracket_id'] = e.bracket_id
        # carry predicate descriptors from the spec for the report
        spec = spec_by_label[e.label]
        m['feature']        = spec.feature
        m['op']             = spec.op
        m['threshold']      = spec.threshold
        m['regime_session'] = spec.regime_session
        m['regime_vol']     = spec.regime_vol
        m['regime_trend']   = spec.regime_trend
        per_edge_overall_rows.append(m)

        for part_label in parts_req:
            ssub = sub[sub['partition'] == part_label]
            mp = _summarise_edge(ssub)
            mp['edge_label'] = e.label
            mp['rank']       = e.rank
            mp['pid']        = e.pid
            mp['side']       = e.side
            mp['bracket_id'] = e.bracket_id
            mp['partition']  = part_label
            per_edge_by_partition_rows.append(mp)

    per_edge_overall = pd.DataFrame(per_edge_overall_rows)
    per_edge_by_partition = pd.DataFrame(per_edge_by_partition_rows)

    # ------------------------------------------------------------------
    # Portfolio metrics
    # ------------------------------------------------------------------
    portfolio_overall = _summarise_edge(trades_all)
    port_rows = []
    for part_label in parts_req:
        psub = trades_all[trades_all['partition'] == part_label]
        m = _summarise_edge(psub)
        m['partition'] = part_label
        port_rows.append(m)
    portfolio_by_partition = pd.DataFrame(port_rows)

    max_conc, max_conc_ts = _max_concurrent_open(trades_all)

    # ------------------------------------------------------------------
    # Cross-edge correlation (daily PnL)
    # ------------------------------------------------------------------
    corr = _edge_correlation_matrix(trades_all, edges, bin_freq='1D')

    # ------------------------------------------------------------------
    # Outputs
    # ------------------------------------------------------------------
    out_csv = out_dir / 'paper_trade_topk_trades.csv'
    out_parq = out_dir / 'paper_trade_topk_summary.parquet'
    out_md  = out_dir / 'paper_trade_topk.md'

    trades_all.to_csv(out_csv, index=False)
    print(f"[{_ts()}] wrote {out_csv} ({len(trades_all)} rows)")

    # The summary parquet bundles overall + by-partition rows in one frame.
    overall_for_parq = per_edge_overall.copy()
    overall_for_parq['partition'] = 'ALL'
    summary_combined = pd.concat(
        [overall_for_parq, per_edge_by_partition],
        ignore_index=True,
    )
    # Drop Timestamp columns that aren't parquet-friendly across pyarrow versions.
    for c in ('first_ts', 'last_ts'):
        if c in summary_combined.columns:
            summary_combined[c] = summary_combined[c].astype('string')
    try:
        summary_combined.to_parquet(out_parq, index=False)
        print(f"[{_ts()}] wrote {out_parq} ({len(summary_combined)} rows)")
    except (ImportError, ValueError) as e:
        # Fallback: pickle alongside, like cli.py does.
        out_pkl = out_parq.with_suffix('.pkl')
        summary_combined.to_pickle(out_pkl)
        print(f"[{_ts()}] parquet unavailable ({e}); wrote {out_pkl} instead",
              flush=True)

    _write_md(
        md_path=out_md,
        candidates_path=cand_path,
        top_k=args.top_k,
        partitions=parts_req,
        edges=edges,
        per_edge_overall=per_edge_overall,
        per_edge_by_partition=per_edge_by_partition,
        portfolio_overall=portfolio_overall,
        portfolio_by_partition=portfolio_by_partition,
        max_concurrent=max_conc,
        max_concurrent_ts=max_conc_ts,
        corr=corr,
        point_value_usd=float(args.point_value_usd),
    )
    print(f"[{_ts()}] wrote {out_md}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
