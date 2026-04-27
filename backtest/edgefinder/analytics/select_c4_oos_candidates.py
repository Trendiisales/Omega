"""
edgefinder/analytics/select_c4_oos_candidates.py
================================================

Final candidate selection from c4_clean for OOS probing. Strategy:

  Per side (LONG, SHORT):
    Sort c4_clean rows by |sharpe| descending.
    Walk the sorted list, applying a per-regime-tuple cap.
    Accept up to N_PER_SIDE rows.

The regime tuple is (regime_session, regime_vol, regime_trend). The cap
prevents one tuple (e.g. LONDON, VOL_HI, TREND_DOWN — 8.9% of clean) from
dominating the OOS family. Cap counters are INDEPENDENT per side, so a
tuple at cap on LONG can still take its first SHORT row.

Long-bias interrogation: top-30 by VAL |sharpe| was 25 LONG / 5 SHORT
during C4 audit, raising the question whether the drift baseline correctly
detrends the rally period or just under-corrects. Selecting 25 LONG +
25 SHORT lets OOS results test both directions on equal footing — if
LONG holds and SHORT collapses, drift correction is broken; if both hold
or both fail, the long-bias was a real (or noise) asymmetry, not a bug.

NO panel load. NO OOS access. NO sentinel touch. Reads c4_clean only.

Outputs (to <indir>):
    oos_candidates.{parquet,pkl}     — selected rows (~50)
    oos_candidates_summary.md        — regime-stratified human summary
    oos_candidates_manifest.json     — selection params + counts

Run:
    python3 -m backtest.edgefinder.analytics.select_c4_oos_candidates
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

DEFAULT_INDIR     = 'backtest/edgefinder/output/inspect_c4'
CLEAN_BASE        = 'c4_clean'
CANDIDATES_BASE   = 'oos_candidates'
SUMMARY_NAME      = 'oos_candidates_summary.md'
MANIFEST_NAME     = 'oos_candidates_manifest.json'

DEFAULT_N_PER_SIDE     = 25
DEFAULT_REGIME_CAP     = 3
DEFAULT_RANK_METRIC    = 'sharpe'   # |sharpe| descending
SIDES                  = ('LONG', 'SHORT')


def _ts() -> str:
    return time.strftime('%H:%M:%S')


# -----------------------------------------------------------------------------
# Persistence helpers (parquet preferred, pickle fallback) — mirrors cli.py
# -----------------------------------------------------------------------------
def _save_df(df: pd.DataFrame, base_path: Path) -> Path:
    parquet_path = base_path.with_suffix('.parquet')
    pickle_path  = base_path.with_suffix('.pkl')
    try:
        df.to_parquet(parquet_path, index=False)
        if pickle_path.exists():
            pickle_path.unlink()
        return parquet_path
    except (ImportError, ValueError) as e:
        print(f"  [persist] parquet unavailable ({e}); using pickle", flush=True)
        df.to_pickle(pickle_path)
        if parquet_path.exists():
            parquet_path.unlink()
        return pickle_path


def _load_df(base_path: Path) -> pd.DataFrame:
    parquet_path = base_path.with_suffix('.parquet')
    pickle_path  = base_path.with_suffix('.pkl')
    if parquet_path.exists():
        try:
            return pd.read_parquet(parquet_path)
        except (ImportError, ValueError) as e:
            print(f"  [persist] parquet read failed ({e}); trying pickle",
                  file=sys.stderr)
    if pickle_path.exists():
        return pd.read_pickle(pickle_path)
    raise FileNotFoundError(
        f"neither {parquet_path} nor {pickle_path} exists; "
        f"did you run inspect_c4 first?"
    )


# -----------------------------------------------------------------------------
# Selection: cap-and-refill per side
# -----------------------------------------------------------------------------
def _regime_key(row: pd.Series) -> tuple[str, str, str]:
    return (
        str(row['regime_session']),
        str(row['regime_vol']),
        str(row['regime_trend']),
    )


def select_side(
    clean: pd.DataFrame,
    side: str,
    n_target: int,
    regime_cap: int,
    rank_metric: str,
) -> tuple[pd.DataFrame, dict]:
    """
    Select up to n_target rows from clean[side==side], walking by
    |rank_metric| descending and applying a per-regime-tuple cap.

    Returns (selected_df, stats_dict). stats includes:
        n_selected, n_eligible, n_capped_dropped, exhausted (bool),
        regime_tuple_counts (final).
    """
    sub = clean[clean['side'] == side].copy()
    n_eligible = len(sub)
    if n_eligible == 0:
        return sub.iloc[0:0].copy(), {
            'n_selected': 0,
            'n_eligible': 0,
            'n_capped_dropped': 0,
            'exhausted': True,
            'regime_tuple_counts': {},
        }

    sub['_abs_score'] = sub[rank_metric].abs()
    sub = sub.sort_values('_abs_score', ascending=False, kind='mergesort')

    chosen_idx: list = []
    capped_dropped = 0
    counts: dict[tuple[str, str, str], int] = {}

    for orig_idx, row in sub.iterrows():
        if len(chosen_idx) >= n_target:
            break
        key = _regime_key(row)
        if counts.get(key, 0) >= regime_cap:
            capped_dropped += 1
            continue
        chosen_idx.append(orig_idx)
        counts[key] = counts.get(key, 0) + 1

    selected = sub.loc[chosen_idx].drop(columns=['_abs_score']).copy()
    exhausted = len(selected) < n_target
    stats = {
        'n_selected':            int(len(selected)),
        'n_eligible':            int(n_eligible),
        'n_capped_dropped':      int(capped_dropped),
        'exhausted':             bool(exhausted),
        'regime_tuple_counts':   {f"{k[0]}|{k[1]}|{k[2]}": int(v)
                                  for k, v in counts.items()},
    }
    return selected, stats


def select_candidates(
    clean: pd.DataFrame,
    n_per_side: int,
    regime_cap: int,
    rank_metric: str,
) -> tuple[pd.DataFrame, dict]:
    """Run select_side for each side, concatenate."""
    parts: list[pd.DataFrame] = []
    stats: dict = {}
    for side in SIDES:
        sel, side_stats = select_side(
            clean, side,
            n_target=n_per_side,
            regime_cap=regime_cap,
            rank_metric=rank_metric,
        )
        parts.append(sel)
        stats[side] = side_stats

    out = pd.concat(parts, axis=0, ignore_index=True) if parts else \
          clean.iloc[0:0].copy()
    return out, stats


# -----------------------------------------------------------------------------
# Regime-stratified summary
# -----------------------------------------------------------------------------
def _fmt_pct(n: int, total: int) -> str:
    if total <= 0:
        return ' (n/a)'
    return f' ({100.0 * n / total:.1f}%)'


def _md_table(df: pd.DataFrame, header: list[str]) -> str:
    """Render a small markdown table from a DataFrame."""
    lines = []
    lines.append('| ' + ' | '.join(header) + ' |')
    lines.append('|' + '|'.join(['---'] * len(header)) + '|')
    for _, row in df.iterrows():
        cells = [str(row[h]) for h in header]
        lines.append('| ' + ' | '.join(cells) + ' |')
    return '\n'.join(lines)


def _render_distribution_block(s: pd.Series, label: str) -> str:
    """Render percentile distribution as a fixed-width block."""
    arr = s.dropna().to_numpy(dtype=np.float64)
    if arr.size == 0:
        return f"{label}: (no data)"
    qs = (0.0, 0.10, 0.25, 0.50, 0.75, 0.90, 1.0)
    parts = [f"{label}:"]
    for q in qs:
        v = np.quantile(arr, q)
        parts.append(f"  q{int(q*100):>3}={v:>+10.4f}")
    return '\n'.join(parts)


def write_summary(
    candidates: pd.DataFrame,
    side_stats: dict,
    out_path: Path,
    n_per_side: int,
    regime_cap: int,
    rank_metric: str,
    indir: Path,
) -> None:
    n_total = len(candidates)
    long_n  = int((candidates['side'] == 'LONG').sum())
    short_n = int((candidates['side'] == 'SHORT').sum())

    lines: list[str] = []
    lines.append(f"# OOS Candidate Selection — Session C4")
    lines.append('')
    lines.append(f"Generated: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}")
    lines.append('')
    lines.append(f"**Source:** `{indir / CLEAN_BASE}.{{parquet,pkl}}`")
    lines.append(f"**Strategy:** top-{n_per_side} LONG + top-{n_per_side} SHORT "
                 f"by |VAL {rank_metric}|, regime-tuple cap = {regime_cap} "
                 f"(per side, independent counters).")
    lines.append('')
    lines.append(f"**OOS sentinel:** UNBURNED. This is a candidate list only.")
    lines.append('')
    lines.append('---')
    lines.append('')

    # ---- selection counts ----
    lines.append('## Selection counts')
    lines.append('')
    lines.append(f"- Total candidates: **{n_total}**")
    lines.append(f"- LONG:  {long_n} (target {n_per_side}, "
                 f"eligible {side_stats['LONG']['n_eligible']}, "
                 f"capped-dropped {side_stats['LONG']['n_capped_dropped']}, "
                 f"exhausted={side_stats['LONG']['exhausted']})")
    lines.append(f"- SHORT: {short_n} (target {n_per_side}, "
                 f"eligible {side_stats['SHORT']['n_eligible']}, "
                 f"capped-dropped {side_stats['SHORT']['n_capped_dropped']}, "
                 f"exhausted={side_stats['SHORT']['exhausted']})")
    if side_stats['LONG']['exhausted'] or side_stats['SHORT']['exhausted']:
        lines.append('')
        lines.append('> ⚠ Side(s) exhausted before reaching target. Cap-and-refill '
                     'could not produce the requested count because clean rows for '
                     'that side ran out (or all remaining rows were capped). '
                     'Inspect `oos_candidates.parquet` for what was selected.')
    lines.append('')

    # ---- bracket × side ----
    lines.append('## Bracket × side breakdown')
    lines.append('')
    pivot = candidates.pivot_table(
        index='bracket_id', columns='side',
        values='pid', aggfunc='count', fill_value=0,
    )
    for side in SIDES:
        if side not in pivot.columns:
            pivot[side] = 0
    pivot['TOTAL'] = pivot[list(SIDES)].sum(axis=1)
    pivot = pivot.reset_index()
    pivot['bracket_id'] = pivot['bracket_id'].astype(int)
    lines.append(_md_table(
        pivot, ['bracket_id'] + list(SIDES) + ['TOTAL']
    ))
    lines.append('')

    # ---- VAL metric distributions ----
    lines.append('## VAL metric distributions on selected candidates')
    lines.append('')
    lines.append('```')
    for col in ['sharpe', 'excess_expectancy', 'expectancy', 'pf', 'hit_rate', 'n']:
        if col in candidates.columns:
            lines.append(_render_distribution_block(candidates[col], col))
            lines.append('')
    lines.append('```')
    lines.append('')

    # ---- regime concentration ----
    lines.append('## Regime concentration on selected candidates')
    lines.append('')
    for col in ['regime_session', 'regime_vol', 'regime_trend']:
        if col not in candidates.columns:
            continue
        c = candidates[col].value_counts()
        lines.append(f'### {col}')
        lines.append('')
        df_c = c.reset_index()
        df_c.columns = [col, 'count']
        df_c['pct'] = (df_c['count'] / n_total * 100.0).round(1).astype(str) + '%'
        lines.append(_md_table(df_c, [col, 'count', 'pct']))
        lines.append('')

    # ---- regime tuple stratification per side ----
    lines.append('## Regime tuples per side (post-cap)')
    lines.append('')
    for side in SIDES:
        sub = candidates[candidates['side'] == side]
        if len(sub) == 0:
            lines.append(f'### {side} — (none selected)')
            lines.append('')
            continue
        tup = sub.groupby(['regime_session', 'regime_vol', 'regime_trend']) \
                 .size().reset_index(name='count') \
                 .sort_values('count', ascending=False)
        lines.append(f'### {side} ({len(sub)} candidates)')
        lines.append('')
        lines.append(_md_table(tup, ['regime_session', 'regime_vol',
                                     'regime_trend', 'count']))
        lines.append('')

    # ---- feature concentration per side ----
    lines.append('## Feature concentration per side')
    lines.append('')
    for side in SIDES:
        sub = candidates[candidates['side'] == side]
        if len(sub) == 0:
            continue
        feat = sub['feature'].value_counts().reset_index()
        feat.columns = ['feature', 'count']
        lines.append(f'### {side}')
        lines.append('')
        lines.append(_md_table(feat.head(15),
                               ['feature', 'count']))
        lines.append('')

    # ---- top-10 by sharpe per side ----
    lines.append('## Top-10 by |VAL sharpe| per side')
    lines.append('')
    for side in SIDES:
        sub = candidates[candidates['side'] == side].copy()
        if len(sub) == 0:
            continue
        sub['_abs'] = sub[rank_metric].abs()
        top = sub.sort_values('_abs', ascending=False, kind='mergesort').head(10)
        cols = ['pid', 'feature', 'op', 'threshold',
                'regime_session', 'regime_vol', 'regime_trend',
                'bracket_id', 'n', 'sharpe', 'excess_expectancy', 'pf']
        avail = [c for c in cols if c in top.columns]
        # Format floats for display
        disp = top[avail].copy()
        for c in disp.columns:
            if pd.api.types.is_float_dtype(disp[c]):
                disp[c] = disp[c].map(lambda x: f"{x:+.4f}" if pd.notna(x) else 'nan')
        lines.append(f'### {side}')
        lines.append('')
        lines.append(_md_table(disp, avail))
        lines.append('')

    out_path.write_text('\n'.join(lines))


# -----------------------------------------------------------------------------
# Driver
# -----------------------------------------------------------------------------
@dataclass
class SelectArgs:
    indir: str
    n_per_side: int
    regime_cap: int
    rank_metric: str


def run(args: SelectArgs) -> int:
    indir = Path(args.indir)
    print(f"[{_ts()}] select_c4_oos_candidates starting")
    print(f"[{_ts()}]   indir       = {indir}")
    print(f"[{_ts()}]   n_per_side  = {args.n_per_side}")
    print(f"[{_ts()}]   regime_cap  = {args.regime_cap} (per side, independent)")
    print(f"[{_ts()}]   rank_metric = |{args.rank_metric}|")

    clean = _load_df(indir / CLEAN_BASE)
    print(f"[{_ts()}] loaded c4_clean: {len(clean)} rows")
    if args.rank_metric not in clean.columns:
        print(f"ERROR: rank_metric={args.rank_metric!r} not in clean columns: "
              f"{list(clean.columns)}", file=sys.stderr)
        return 2

    required = {
        'pid', 'feature', 'op', 'threshold',
        'regime_session', 'regime_vol', 'regime_trend',
        'side', 'bracket_id', 'n', 'sharpe',
    }
    missing = required - set(clean.columns)
    if missing:
        print(f"ERROR: c4_clean missing columns: {missing}", file=sys.stderr)
        return 2

    candidates, side_stats = select_candidates(
        clean,
        n_per_side=args.n_per_side,
        regime_cap=args.regime_cap,
        rank_metric=args.rank_metric,
    )
    print(f"[{_ts()}] selected: {len(candidates)} candidates "
          f"(LONG={side_stats['LONG']['n_selected']}, "
          f"SHORT={side_stats['SHORT']['n_selected']})")

    for side in SIDES:
        st = side_stats[side]
        if st['exhausted']:
            print(f"[{_ts()}] WARNING: {side} side exhausted "
                  f"(eligible={st['n_eligible']}, "
                  f"capped-dropped={st['n_capped_dropped']}, "
                  f"selected={st['n_selected']}/{args.n_per_side})")

    # ---- write outputs ----
    cand_path = _save_df(candidates, indir / CANDIDATES_BASE)
    print(f"[{_ts()}] wrote {cand_path}")

    summary_path = indir / SUMMARY_NAME
    write_summary(
        candidates=candidates,
        side_stats=side_stats,
        out_path=summary_path,
        n_per_side=args.n_per_side,
        regime_cap=args.regime_cap,
        rank_metric=args.rank_metric,
        indir=indir,
    )
    print(f"[{_ts()}] wrote {summary_path}")

    manifest = {
        'when_iso':       time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'indir':          str(indir),
        'n_per_side':     int(args.n_per_side),
        'regime_cap':     int(args.regime_cap),
        'rank_metric':    str(args.rank_metric),
        'n_total':        int(len(candidates)),
        'n_long':         int(side_stats['LONG']['n_selected']),
        'n_short':        int(side_stats['SHORT']['n_selected']),
        'long_exhausted':  bool(side_stats['LONG']['exhausted']),
        'short_exhausted': bool(side_stats['SHORT']['exhausted']),
        'long_capped_dropped':  int(side_stats['LONG']['n_capped_dropped']),
        'short_capped_dropped': int(side_stats['SHORT']['n_capped_dropped']),
        'long_eligible':  int(side_stats['LONG']['n_eligible']),
        'short_eligible': int(side_stats['SHORT']['n_eligible']),
        'long_regime_counts':  side_stats['LONG']['regime_tuple_counts'],
        'short_regime_counts': side_stats['SHORT']['regime_tuple_counts'],
        'oos_sentinel':   'UNBURNED',
    }
    manifest_path = indir / MANIFEST_NAME
    manifest_path.write_text(json.dumps(manifest, indent=2))
    print(f"[{_ts()}] wrote {manifest_path}")

    # ---- final summary block ----
    print()
    print(f"[{_ts()}] === SUMMARY ===")
    print(f"  candidates total : {len(candidates)}")
    print(f"  LONG / SHORT     : {side_stats['LONG']['n_selected']} / "
          f"{side_stats['SHORT']['n_selected']}")
    bracket_counts = candidates['bracket_id'].value_counts().sort_index()
    print(f"  by bracket_id    : "
          + ", ".join(f"b{int(b)}={int(c)}" for b, c in bracket_counts.items()))
    if 'sharpe' in candidates.columns:
        sh = candidates['sharpe'].abs()
        print(f"  |sharpe| range   : {sh.min():.4f} .. {sh.max():.4f}")
        print(f"  |sharpe| median  : {sh.median():.4f}")
    print(f"  unique features  : {candidates['feature'].nunique()}")
    print(f"  unique regime tuples (long)  : "
          f"{len(side_stats['LONG']['regime_tuple_counts'])}")
    print(f"  unique regime tuples (short) : "
          f"{len(side_stats['SHORT']['regime_tuple_counts'])}")
    print()
    print(f"  OOS sentinel     : UNBURNED")
    print(f"  next step        : review oos_candidates_summary.md, "
          f"then probe-oos when ready")

    return 0


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def _parse_args(argv: Optional[list[str]] = None) -> SelectArgs:
    p = argparse.ArgumentParser(
        prog='select_c4_oos_candidates',
        description='Select OOS candidates from c4_clean: top-N per side '
                    'with regime-tuple cap and refill.',
    )
    p.add_argument('--indir', default=DEFAULT_INDIR)
    p.add_argument('--n-per-side', type=int, default=DEFAULT_N_PER_SIDE)
    p.add_argument('--regime-cap', type=int, default=DEFAULT_REGIME_CAP)
    p.add_argument('--rank-metric', default=DEFAULT_RANK_METRIC,
                   choices=('sharpe', 'excess_expectancy', 'expectancy'))
    a = p.parse_args(argv)
    return SelectArgs(
        indir=a.indir,
        n_per_side=a.n_per_side,
        regime_cap=a.regime_cap,
        rank_metric=a.rank_metric,
    )


def main(argv: Optional[list[str]] = None) -> int:
    return run(_parse_args(argv))


if __name__ == '__main__':
    sys.exit(main())
