"""
backtest/edgefinder/analytics/inspect_drift_stability.py
========================================================

C6 thread #1B — drift-sign stability diagnostic for MtM-dominated survivors.

Context
-------
C6 #1A established that all 19 strict OOS survivors are MtM-dominated:
    TP : 0 / 78,000 trades (0.000%)
    SL : 66 / 78,000 trades (0.085%)
    MtM: 77,934 / 78,000 trades (99.915%)

The bracket TP/SL framework essentially never fires for these edges. Their
realised per-trade PnL is therefore the directional drift over the bracket
horizon (~60 min for b3/b4/b5), not a bracketed outcome. The "strict
survivor" rank is effectively a ranking by mean(fwd_ret_h_pts × side_sign
| predicate fires).

That doesn't necessarily mean the edges are useless — it means they are
fwd_ret directional drift strategies, mislabelled as bracket strategies.
The question this diagnostic answers is:

    Does the drift sign survive across the train / val / oos split?

If yes for some edges → those are real directional drift edges. They have
no protective stop in their current form, but the underlying signal is
real and worth converting into an explicit fwd_ret strategy with an
external SL (e.g. -1.5×ATR or fixed -30pts). That is C6 thread #1C.

If no → those edges are train-fit noise, no signal in OOS, and the C5
strict-survivor gate has a structural bug (it ranks on a metric that does
not generalize).

What this module does
---------------------
Read-only over:
    paper_trade_topk_trades_classified.csv   (per-trade, with outcome col)
    paper_trade_topk_outcome_summary.csv     (per (edge, partition) summary)

For each edge it computes, per partition:
    n, mean_pnl_pts, std_pnl_pts, sharpe (per-trade, 0-mean baseline), t_stat
And per edge it adds cross-partition stability metrics:
    sign_train, sign_val, sign_oos
    all_signs_match            (True if all three signs equal and non-zero)
    oos_mean_frac_of_train     (OOS mean / train mean, NaN if train mean ≈ 0)
    oos_t_abs                  (|t-stat| of OOS leg, used as significance proxy)
    verdict                    in {'STABLE', 'WEAK', 'FLIPPED', 'DEAD'}

Verdict logic
-------------
Each verdict is a conjunction of conditions on the OOS leg specifically,
because OOS is the only leg the strict-survivor gate did not see at
selection time. (Train and val were both used for ranking and gating.)

    STABLE   : all three signs match
               AND |OOS t-stat| > T_STABLE      (default 2.0, ~5% two-sided)
               AND oos_mean_frac_of_train > FRAC_FLOOR  (default 0.20)
    WEAK     : all three signs match
               BUT either |OOS t-stat| < T_STABLE OR magnitude collapsed
    FLIPPED  : OOS sign disagrees with train sign (regardless of val)
    DEAD     : OOS n < N_FLOOR (default 30) — too few trades to judge

These thresholds are conservative and configurable via CLI. Edges in
STABLE are candidates for promotion to fwd_ret strategies with explicit
external stops. Edges in WEAK / FLIPPED / DEAD should be culled or
investigated for what predicate-feature changed between train and OOS.

t-statistic
-----------
Standard one-sample t against zero on per-trade PnL:
    t = mean / (std / sqrt(n))
    std uses ddof=1 (sample std).
This is *not* a Sharpe ratio — Sharpe annualises and divides by per-period
std. For a per-trade significance test the one-sample t is the right tool.
The Sharpe is reported separately as a magnitude-vs-noise descriptor; it
is not used in the verdict.

What this module does NOT do
----------------------------
- Modify any core code, the panel binary, the catalogue, or the OOS
  sentinel.
- Recompute outcomes — those come from paper_trade_topk_trades_classified.csv
  produced by inspect_bracket_outcomes.py.
- Look at intra-partition time stability (e.g. is OOS positive for the
  first half but negative for the second half). That is a separate
  diagnostic and would need explicit time-bucketing.
- Make any claim about whether these edges work in production. STABLE
  here means "drift sign + magnitude survived the train→val→oos split"
  not "edge is profitable net of slippage and costs".

CLI
---
    python -m backtest.edgefinder.analytics.inspect_drift_stability \\
        [--trades-csv PATH]    default backtest/edgefinder/output/paper_trade/paper_trade_topk_trades_classified.csv
        [--summary-csv PATH]   default backtest/edgefinder/output/paper_trade/paper_trade_topk_outcome_summary.csv
        [--out-dir PATH]       default backtest/edgefinder/output/paper_trade
        [--t-stable FLOAT]     default 2.0   (|OOS t-stat| threshold for STABLE)
        [--frac-floor FLOAT]   default 0.20  (OOS / train mean magnitude floor)
        [--n-floor INT]        default 30    (min OOS trades to grade)

Outputs (under --out-dir):
    drift_stability_per_partition.csv
        rank, pid, side, bracket_id, partition, n, mean_pnl_pts, std_pnl_pts,
        sharpe, t_stat, sum_pnl_pts
    drift_stability_per_edge.csv
        rank, pid, side, bracket_id,
        n_train, n_val, n_oos,
        mean_train, mean_val, mean_oos,
        std_train, std_val, std_oos,
        t_train, t_val, t_oos,
        sign_train, sign_val, sign_oos,
        all_signs_match,
        oos_mean_frac_of_train,
        oos_t_abs,
        verdict
    drift_stability.md
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd


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

PARTITION_ORDER = ['train', 'val', 'oos']


# -----------------------------------------------------------------------------
# Per-partition stats
# -----------------------------------------------------------------------------
def per_partition_stats(trades: pd.DataFrame) -> pd.DataFrame:
    """
    Compute n, mean, std (ddof=1), Sharpe (mean/std), t-stat (mean / SE),
    and sum_pnl_pts per (edge, partition).

    Sharpe here is per-trade (not annualised). t-stat is one-sample
    against zero.

    Groups with n < 2 get std = NaN (ddof=1 undefined). Sharpe and t-stat
    fall back to 0.0 for n < 2 to keep the table clean.
    """
    group_cols = ['rank', 'pid', 'side', 'bracket_id', 'partition']
    g = trades.groupby(group_cols, sort=False)['pnl_pts']

    n = g.size().rename('n')
    mean = g.mean().rename('mean_pnl_pts')
    # ddof=1 sample std. Pandas uses ddof=1 by default for Series.std() but the
    # equivalent groupby method's default also uses ddof=1; we make it explicit
    # to avoid numpy/pandas version drift surprises.
    std = g.std(ddof=1).rename('std_pnl_pts')
    total = g.sum().rename('sum_pnl_pts')

    out = pd.concat([n, mean, std, total], axis=1).reset_index()

    # Sharpe (per-trade)
    with np.errstate(divide='ignore', invalid='ignore'):
        sharpe = np.where(
            (out['std_pnl_pts'] > 0) & (out['n'] >= 2),
            out['mean_pnl_pts'] / out['std_pnl_pts'],
            0.0,
        )
    out['sharpe'] = sharpe

    # One-sample t-stat: mean / (std / sqrt(n))
    with np.errstate(divide='ignore', invalid='ignore'):
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
    out['t_stat'] = t_stat

    # Order columns
    out = out[[
        'rank', 'pid', 'side', 'bracket_id', 'partition',
        'n', 'mean_pnl_pts', 'std_pnl_pts', 'sharpe', 't_stat',
        'sum_pnl_pts',
    ]]

    # Sort by rank then partition
    part_sort = {p: i for i, p in enumerate(PARTITION_ORDER)}
    out['_p'] = out['partition'].map(part_sort).fillna(99).astype(int)
    out = out.sort_values(['rank', '_p'], kind='mergesort').drop(columns='_p')
    out = out.reset_index(drop=True)
    return out


# -----------------------------------------------------------------------------
# Per-edge cross-partition stability
# -----------------------------------------------------------------------------
def _sign(x: float) -> int:
    if not np.isfinite(x):
        return 0
    if x > 0:
        return +1
    if x < 0:
        return -1
    return 0


def per_edge_stability(
    per_part: pd.DataFrame,
    t_stable: float,
    frac_floor: float,
    n_floor: int,
) -> pd.DataFrame:
    """
    Pivot per-partition stats into one row per edge with cross-partition
    stability columns and a verdict.
    """
    edge_cols = ['rank', 'pid', 'side', 'bracket_id']
    edges = per_part[edge_cols].drop_duplicates().sort_values('rank').reset_index(drop=True)

    # Build a lookup: (rank, pid, side, bracket_id, partition) -> row dict
    keyed = per_part.set_index(edge_cols + ['partition'])

    rows = []
    for _, e in edges.iterrows():
        key = (int(e['rank']), int(e['pid']), str(e['side']), int(e['bracket_id']))

        def pull(part: str, field: str, default=np.nan):
            try:
                v = keyed.loc[key + (part,), field]
            except KeyError:
                return default
            # If duplicate index keys exist (shouldn't, but defensively), take first
            if isinstance(v, pd.Series):
                return float(v.iloc[0]) if len(v) else default
            return float(v) if pd.notna(v) else default

        n_train = int(pull('train', 'n', 0)) if pull('train', 'n', 0) == pull('train', 'n', 0) else 0
        n_val = int(pull('val', 'n', 0)) if pull('val', 'n', 0) == pull('val', 'n', 0) else 0
        n_oos = int(pull('oos', 'n', 0)) if pull('oos', 'n', 0) == pull('oos', 'n', 0) else 0

        mean_train = pull('train', 'mean_pnl_pts', np.nan)
        mean_val = pull('val', 'mean_pnl_pts', np.nan)
        mean_oos = pull('oos', 'mean_pnl_pts', np.nan)

        std_train = pull('train', 'std_pnl_pts', np.nan)
        std_val = pull('val', 'std_pnl_pts', np.nan)
        std_oos = pull('oos', 'std_pnl_pts', np.nan)

        t_train = pull('train', 't_stat', np.nan)
        t_val = pull('val', 't_stat', np.nan)
        t_oos = pull('oos', 't_stat', np.nan)

        s_train = _sign(mean_train)
        s_val = _sign(mean_val)
        s_oos = _sign(mean_oos)

        all_match = (s_train != 0) and (s_train == s_val == s_oos)

        # Magnitude ratio: oos mean / train mean (signed). If train mean is
        # ~0 the ratio is ill-defined; we mark NaN. We use abs(train) as the
        # denominator so the ratio retains the sign of OOS mean — but for
        # the verdict we look at the absolute magnitude floor below.
        if np.isfinite(mean_train) and abs(mean_train) > 1e-12:
            oos_frac = mean_oos / mean_train  # signed
        else:
            oos_frac = np.nan

        oos_t_abs = abs(t_oos) if np.isfinite(t_oos) else np.nan

        # Verdict
        if n_oos < n_floor:
            verdict = 'DEAD'
        elif s_train != 0 and s_oos != 0 and s_train != s_oos:
            verdict = 'FLIPPED'
        elif all_match:
            mag_ok = np.isfinite(oos_frac) and oos_frac >= frac_floor
            sig_ok = np.isfinite(oos_t_abs) and oos_t_abs >= t_stable
            if mag_ok and sig_ok:
                verdict = 'STABLE'
            else:
                verdict = 'WEAK'
        else:
            # Signs not all matching but OOS sign equals train sign (val
            # sign disagrees) — treat as WEAK rather than FLIPPED, because
            # val is small (28k bars vs 81k OOS) and can be regime-skewed.
            if s_train != 0 and s_oos != 0 and s_train == s_oos:
                verdict = 'WEAK'
            else:
                # OOS mean is exactly zero or non-finite; treat as DEAD
                verdict = 'DEAD'

        rows.append({
            'rank': int(e['rank']),
            'pid': int(e['pid']),
            'side': str(e['side']),
            'bracket_id': int(e['bracket_id']),
            'n_train': n_train, 'n_val': n_val, 'n_oos': n_oos,
            'mean_train': mean_train, 'mean_val': mean_val, 'mean_oos': mean_oos,
            'std_train': std_train, 'std_val': std_val, 'std_oos': std_oos,
            't_train': t_train, 't_val': t_val, 't_oos': t_oos,
            'sign_train': s_train, 'sign_val': s_val, 'sign_oos': s_oos,
            'all_signs_match': all_match,
            'oos_mean_frac_of_train': oos_frac,
            'oos_t_abs': oos_t_abs,
            'verdict': verdict,
        })

    return pd.DataFrame(rows)


# -----------------------------------------------------------------------------
# MD report
# -----------------------------------------------------------------------------
def _fmt_signed(x: float, w: int = 9, prec: int = 4) -> str:
    if not np.isfinite(x):
        return f"{'nan':>{w}}"
    return f"{x:>+{w}.{prec}f}"


def _fmt_int(x, w: int = 5) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'-':>{w}}"
    return f"{int(x):>{w}}"


def write_md_report(
    per_part: pd.DataFrame,
    per_edge: pd.DataFrame,
    trades_csv_path: Path,
    summary_csv_path: Path,
    out_md_path: Path,
    t_stable: float,
    frac_floor: float,
    n_floor: int,
) -> None:
    lines = []
    lines.append("# C6 #1B — Drift-Sign Stability Diagnostic")
    lines.append("")
    lines.append(f"- Trades CSV:  `{trades_csv_path}`")
    lines.append(f"- Summary CSV: `{summary_csv_path}`")
    lines.append(f"- Edges:       {len(per_edge)}")
    lines.append(f"- Thresholds:  |OOS t-stat| ≥ {t_stable}, "
                 f"OOS/train mag ≥ {frac_floor}, OOS n ≥ {n_floor}")
    lines.append("")

    # Verdict counts
    counts = per_edge['verdict'].value_counts()
    lines.append("## Verdict counts")
    lines.append("")
    lines.append("| verdict  | n  | description |")
    lines.append("|:---------|---:|:------------|")
    descriptions = {
        'STABLE':  'all signs match, OOS significant, OOS mag retains ≥ floor of train',
        'WEAK':    'all signs match but OOS lacks significance or magnitude collapsed',
        'FLIPPED': 'OOS sign disagrees with train sign — overfit or regime change',
        'DEAD':    'OOS too small to judge or OOS mean is zero',
    }
    for v in ['STABLE', 'WEAK', 'FLIPPED', 'DEAD']:
        n = int(counts.get(v, 0))
        lines.append(f"| {v:<8} | {n:>2} | {descriptions[v]} |")
    lines.append("")

    # Per-edge table
    lines.append("## Per-edge stability table")
    lines.append("")
    lines.append("Means below are per-trade points (negated for SHORT — already side-corrected).")
    lines.append("")
    lines.append(
        "| rank | pid   | side  | b | n_tr | n_v  | n_oos | mean_tr   | mean_v    | mean_oos  | "
        "t_tr   | t_v    | t_oos  | sign_tr | sign_v | sign_oos | oos/tr  | verdict  |"
    )
    lines.append(
        "|-----:|------:|:------|:-:|-----:|-----:|------:|----------:|----------:|----------:|"
        "-------:|-------:|-------:|--------:|-------:|---------:|--------:|:---------|"
    )
    for _, r in per_edge.iterrows():
        sign_str = lambda s: '+' if s == 1 else ('-' if s == -1 else '0')
        oos_frac = r['oos_mean_frac_of_train']
        oos_frac_s = f"{oos_frac:>+7.3f}" if np.isfinite(oos_frac) else f"{'nan':>7}"
        lines.append(
            f"| {int(r['rank']):>4} "
            f"| {int(r['pid']):>5} "
            f"| {r['side']:<5} "
            f"| {int(r['bracket_id'])} "
            f"| {_fmt_int(r['n_train'], 4)} "
            f"| {_fmt_int(r['n_val'], 4)} "
            f"| {_fmt_int(r['n_oos'], 5)} "
            f"| {_fmt_signed(r['mean_train'])} "
            f"| {_fmt_signed(r['mean_val'])} "
            f"| {_fmt_signed(r['mean_oos'])} "
            f"| {_fmt_signed(r['t_train'], 6, 2)} "
            f"| {_fmt_signed(r['t_val'], 6, 2)} "
            f"| {_fmt_signed(r['t_oos'], 6, 2)} "
            f"|       {sign_str(r['sign_train'])} "
            f"|      {sign_str(r['sign_val'])} "
            f"|        {sign_str(r['sign_oos'])} "
            f"| {oos_frac_s} "
            f"| **{r['verdict']:<7}** |"
        )
    lines.append("")

    # Highlight STABLE edges
    stable = per_edge[per_edge['verdict'] == 'STABLE']
    lines.append("## STABLE edges (candidates for fwd_ret promotion)")
    lines.append("")
    if stable.empty:
        lines.append("_None._ No edge passes all three OOS gates "
                     f"(sign-match across train/val/oos, |t-stat| ≥ {t_stable}, "
                     f"mag ≥ {frac_floor} of train).")
    else:
        lines.append(f"{len(stable)} edge(s) survived all three OOS gates.")
        lines.append("")
        lines.append("| rank | pid   | side  | b | n_oos | mean_oos  | t_oos  | oos/train |")
        lines.append("|-----:|------:|:------|:-:|------:|----------:|-------:|----------:|")
        for _, r in stable.iterrows():
            oos_frac = r['oos_mean_frac_of_train']
            oos_frac_s = f"{oos_frac:>+7.3f}" if np.isfinite(oos_frac) else f"{'nan':>7}"
            lines.append(
                f"| {int(r['rank']):>4} "
                f"| {int(r['pid']):>5} "
                f"| {r['side']:<5} "
                f"| {int(r['bracket_id'])} "
                f"| {_fmt_int(r['n_oos'], 5)} "
                f"| {_fmt_signed(r['mean_oos'])} "
                f"| {_fmt_signed(r['t_oos'], 6, 2)} "
                f"| {oos_frac_s} |"
            )
    lines.append("")

    # Highlight FLIPPED edges
    flipped = per_edge[per_edge['verdict'] == 'FLIPPED']
    lines.append("## FLIPPED edges (overfit or regime change)")
    lines.append("")
    if flipped.empty:
        lines.append("_None._")
    else:
        lines.append(f"{len(flipped)} edge(s) have OOS sign disagreeing with train sign. "
                     "Cull or investigate the regime change.")
        lines.append("")
        lines.append("| rank | pid   | side  | b | mean_train | mean_oos  |")
        lines.append("|-----:|------:|:------|:-:|-----------:|----------:|")
        for _, r in flipped.iterrows():
            lines.append(
                f"| {int(r['rank']):>4} "
                f"| {int(r['pid']):>5} "
                f"| {r['side']:<5} "
                f"| {int(r['bracket_id'])} "
                f"| {_fmt_signed(r['mean_train'], 10)} "
                f"| {_fmt_signed(r['mean_oos'])} |"
            )
    lines.append("")

    # Per-partition stats appendix
    lines.append("## Appendix — per-partition stats (full)")
    lines.append("")
    lines.append("| rank | pid   | side  | b | partition | n     | mean       | std       | sharpe   | t_stat |")
    lines.append("|-----:|------:|:------|:-:|:----------|------:|-----------:|----------:|---------:|-------:|")
    for _, r in per_part.iterrows():
        lines.append(
            f"| {int(r['rank']):>4} "
            f"| {int(r['pid']):>5} "
            f"| {r['side']:<5} "
            f"| {int(r['bracket_id'])} "
            f"| {r['partition']:<9} "
            f"| {_fmt_int(r['n'], 5)} "
            f"| {_fmt_signed(r['mean_pnl_pts'], 10)} "
            f"| {_fmt_signed(r['std_pnl_pts'], 9)} "
            f"| {_fmt_signed(r['sharpe'], 8)} "
            f"| {_fmt_signed(r['t_stat'], 6, 2)} |"
        )
    lines.append("")

    out_md_path.write_text('\n'.join(lines))


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="C6 #1B: Drift-sign stability diagnostic for MtM-dominated edges."
    )
    p.add_argument(
        '--trades-csv',
        default=DEFAULT_TRADES_CSV,
        help=f"Path to trades_classified CSV (default: {DEFAULT_TRADES_CSV})",
    )
    p.add_argument(
        '--summary-csv',
        default=DEFAULT_SUMMARY_CSV,
        help=f"Path to outcome_summary CSV (default: {DEFAULT_SUMMARY_CSV})",
    )
    p.add_argument(
        '--out-dir',
        default=DEFAULT_OUT_DIR,
        help=f"Output directory (default: {DEFAULT_OUT_DIR})",
    )
    p.add_argument(
        '--t-stable',
        type=float,
        default=DEFAULT_T_STABLE,
        help=f"|OOS t-stat| threshold for STABLE verdict (default: {DEFAULT_T_STABLE})",
    )
    p.add_argument(
        '--frac-floor',
        type=float,
        default=DEFAULT_FRAC_FLOOR,
        help=f"OOS / train signed mean ratio floor for STABLE (default: {DEFAULT_FRAC_FLOOR})",
    )
    p.add_argument(
        '--n-floor',
        type=int,
        default=DEFAULT_N_FLOOR,
        help=f"Min OOS trades to grade (default: {DEFAULT_N_FLOOR})",
    )
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)

    trades_csv_path = Path(args.trades_csv)
    summary_csv_path = Path(args.summary_csv)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not trades_csv_path.is_file():
        print(f"ERROR: trades CSV not found: {trades_csv_path}", file=sys.stderr)
        return 1
    if not summary_csv_path.is_file():
        print(f"ERROR: summary CSV not found: {summary_csv_path}", file=sys.stderr)
        return 1

    print(f"loading trades: {trades_csv_path}")
    trades = pd.read_csv(trades_csv_path)
    print(f"trades rows: {len(trades)}")

    required = ['rank', 'pid', 'side', 'bracket_id', 'partition', 'pnl_pts']
    missing = [c for c in required if c not in trades.columns]
    if missing:
        print(
            f"ERROR: trades CSV missing required columns: {missing}. "
            f"Got: {sorted(trades.columns)}",
            file=sys.stderr,
        )
        return 2

    print(f"computing per-partition stats")
    per_part = per_partition_stats(trades)
    print(f"per-partition rows: {len(per_part)}")

    per_part_csv = out_dir / 'drift_stability_per_partition.csv'
    per_part.to_csv(per_part_csv, index=False)
    print(f"wrote {per_part_csv} ({len(per_part)} rows)")

    print(f"computing per-edge stability "
          f"(t_stable={args.t_stable}, frac_floor={args.frac_floor}, "
          f"n_floor={args.n_floor})")
    per_edge = per_edge_stability(
        per_part,
        t_stable=args.t_stable,
        frac_floor=args.frac_floor,
        n_floor=args.n_floor,
    )

    per_edge_csv = out_dir / 'drift_stability_per_edge.csv'
    per_edge.to_csv(per_edge_csv, index=False)
    print(f"wrote {per_edge_csv} ({len(per_edge)} rows)")

    counts = per_edge['verdict'].value_counts()
    print(f"verdicts:")
    for v in ['STABLE', 'WEAK', 'FLIPPED', 'DEAD']:
        print(f"  {v:<8}: {int(counts.get(v, 0)):>3}")

    md_path = out_dir / 'drift_stability.md'
    write_md_report(
        per_part=per_part,
        per_edge=per_edge,
        trades_csv_path=trades_csv_path,
        summary_csv_path=summary_csv_path,
        out_md_path=md_path,
        t_stable=args.t_stable,
        frac_floor=args.frac_floor,
        n_floor=args.n_floor,
    )
    print(f"wrote {md_path}")

    return 0


if __name__ == '__main__':
    sys.exit(main())
