"""
edgefinder/analytics/report.py
==============================

Final reporting layer.

Produces:
    edges_ranked.csv   — one row per (pid, side, bracket); sorted by
                         sharpe_train descending within each bracket.
                         Columns include drift_baseline, excess_expectancy,
                         economic_pass alongside the usual stats.

    edges_summary.md   — top 20 edges per bracket, BH and Bonferroni
                         survivors, with VAL/OOS replication shown.

This module makes NO statistical decisions. All gating is done upstream;
the report just renders.
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

from .regime import PredicateSpec


def _merge_partitions(
    train_prospects: pd.DataFrame,
    val_prospects:   Optional[pd.DataFrame],
    oos_prospects:   Optional[pd.DataFrame],
) -> pd.DataFrame:
    """Left-join VAL and OOS stats onto TRAIN by (pid, side, bracket)."""
    keys = ['pid', 'side', 'bracket_id']

    def _suffix(d: pd.DataFrame, suf: str) -> pd.DataFrame:
        keep_cols = ['n', 'hit_rate', 'expectancy', 'pf', 'sharpe',
                     'drift_baseline', 'excess_expectancy']
        keep = [c for c in keep_cols if c in d.columns]
        sub = d[keys + keep].copy()
        for k in keep:
            sub.rename(columns={k: f"{k}_{suf}"}, inplace=True)
        return sub

    tr = train_prospects.copy()
    rename_train = {
        'n': 'n_train', 'hit_rate': 'hit_rate_train',
        'expectancy': 'expectancy_train', 'pf': 'pf_train',
        'sharpe': 'sharpe_train',
        'drift_baseline': 'drift_baseline_train',
        'excess_expectancy': 'excess_expectancy_train',
    }
    rename_train = {k: v for k, v in rename_train.items() if k in tr.columns}
    tr.rename(columns=rename_train, inplace=True)

    if val_prospects is not None and not val_prospects.empty:
        tr = tr.merge(_suffix(val_prospects, 'val'), on=keys, how='left')
    else:
        for k in ['n_val', 'hit_rate_val', 'expectancy_val', 'pf_val',
                  'sharpe_val', 'drift_baseline_val', 'excess_expectancy_val']:
            tr[k] = np.nan

    if oos_prospects is not None and not oos_prospects.empty:
        tr = tr.merge(_suffix(oos_prospects, 'oos'), on=keys, how='left')
    else:
        for k in ['n_oos', 'hit_rate_oos', 'expectancy_oos', 'pf_oos',
                  'sharpe_oos', 'drift_baseline_oos', 'excess_expectancy_oos']:
            tr[k] = np.nan

    return tr


def write_edges_ranked(
    train_prospects: pd.DataFrame,
    val_prospects:   Optional[pd.DataFrame],
    oos_prospects:   Optional[pd.DataFrame],
    catalogue_by_pid: dict[int, PredicateSpec],
    out_path: str | Path,
) -> pd.DataFrame:
    """Write edges_ranked.csv and return the final DataFrame."""
    merged = _merge_partitions(train_prospects, val_prospects, oos_prospects)

    descs = []
    for pid in merged['pid']:
        spec = catalogue_by_pid.get(int(pid))
        descs.append(spec.describe() if spec else f"pid={pid}")
    merged['predicate'] = descs

    cols = [
        'pid', 'predicate', 'feature', 'op', 'threshold',
        'regime_session', 'regime_vol', 'regime_trend',
        'side', 'bracket_id',
        'n_train', 'hit_rate_train',
        'expectancy_train', 'drift_baseline_train', 'excess_expectancy_train',
        'pf_train', 'sharpe_train',
        't_stat', 'p_raw', 'q_bh', 'q_bonf',
        'economic_pass', 'survives_bh', 'survives_bonf',
        'n_val', 'hit_rate_val',
        'expectancy_val', 'drift_baseline_val', 'excess_expectancy_val',
        'pf_val', 'sharpe_val',
        'n_oos', 'hit_rate_oos',
        'expectancy_oos', 'drift_baseline_oos', 'excess_expectancy_oos',
        'pf_oos', 'sharpe_oos',
    ]
    cols = [c for c in cols if c in merged.columns]
    merged = merged[cols]
    merged = merged.sort_values(
        ['bracket_id', 'sharpe_train'],
        ascending=[True, False],
    ).reset_index(drop=True)

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    merged.to_csv(out, index=False)
    return merged


def write_edges_summary(
    ranked: pd.DataFrame,
    out_path: str | Path,
    top_k: int = 20,
    note_no_oos: bool = False,
) -> None:
    """Write a human-readable markdown summary."""
    lines: list[str] = []
    lines.append("# OmegaEdgeFinder — Edges Summary")
    lines.append("")
    lines.append(f"Total ranked rows: **{len(ranked)}**")
    if 'survives_bh' in ranked.columns:
        lines.append(f"BH survivors: **{int(ranked['survives_bh'].sum())}** | "
                     f"Bonferroni survivors: **{int(ranked['survives_bonf'].sum())}**")
    if 'economic_pass' in ranked.columns:
        lines.append(f"Economic-filter pass: **{int(ranked['economic_pass'].sum())}**")
    if note_no_oos:
        lines.append("")
        lines.append("> **OOS partition has not yet been consumed.** "
                     "The OOS columns are empty in this report.")
    lines.append("")

    n_brackets = int(ranked['bracket_id'].max()) + 1 if len(ranked) else 0
    for b in range(n_brackets):
        sub = ranked[ranked['bracket_id'] == b]
        if sub.empty:
            continue
        lines.append(f"## Bracket {b}")
        lines.append("")

        bh   = sub[sub.get('survives_bh',   False) == True].head(top_k)
        bonf = sub[sub.get('survives_bonf', False) == True].head(top_k)

        lines.append(f"### BH-survivors top {top_k} (FDR-corrected, drift-relative)")
        lines.append("")
        lines.append(_md_table(bh))
        lines.append("")

        lines.append(f"### Bonferroni-survivors top {top_k} (FWER-corrected, drift-relative)")
        lines.append("")
        lines.append(_md_table(bonf))
        lines.append("")

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines))


def _md_table(df: pd.DataFrame) -> str:
    if df.empty:
        return "_(none)_"
    cols = [
        'predicate', 'side', 'n_train',
        'expectancy_train', 'drift_baseline_train', 'excess_expectancy_train',
        'sharpe_train', 'pf_train', 'q_bh', 'q_bonf',
        'n_val', 'expectancy_val', 'excess_expectancy_val', 'pf_val',
        'n_oos', 'expectancy_oos', 'excess_expectancy_oos', 'pf_oos',
    ]
    cols = [c for c in cols if c in df.columns]
    sub = df[cols].copy()

    for c in sub.columns:
        if c in ('predicate', 'side'):
            continue
        if pd.api.types.is_float_dtype(sub[c]):
            sub[c] = sub[c].map(lambda x: '' if pd.isna(x) else f"{x:.4g}")
        elif pd.api.types.is_integer_dtype(sub[c]):
            sub[c] = sub[c].map(lambda x: '' if pd.isna(x) else f"{int(x)}")

    header = "| " + " | ".join(cols) + " |"
    sep    = "| " + " | ".join(['---'] * len(cols)) + " |"
    rows   = []
    for _, r in sub.iterrows():
        rows.append("| " + " | ".join(str(r[c]) for c in cols) + " |")
    return "\n".join([header, sep] + rows)
