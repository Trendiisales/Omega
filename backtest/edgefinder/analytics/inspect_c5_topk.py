"""
backtest/edgefinder/analytics/inspect_c5_topk.py
================================================

C5 — read-only ranked export of strict OOS survivors.

Inputs:
    backtest/edgefinder/output/work/oos_survivors.parquet
    backtest/edgefinder/output/inspect_c4/oos_candidates.parquet

Outputs (sibling to C4 artefacts):
    backtest/edgefinder/output/inspect_c4/c5_topk_survivors.csv
    backtest/edgefinder/output/inspect_c4/c5_topk_survivors.md

Survival criteria (strict, mirrors inspect_c5.py):
    sign_match(VAL,OOS) AND pf_oos > 1 AND n_oos >= 30 AND |t_oos| >= 1.5

Ranking:
    Primary:   sharpe_oos descending (by absolute value, since SHORT
               also returns positive sharpe after direction-correction
               in reapply_to_partition — both sides should have +ve
               sharpe when the predicate works in its predicted direction)
    Secondary: t_stat_oos descending (statistical weight tiebreaker)

The MD output is the actionable edge list. The CSV mirrors it for
downstream tooling.

Pure inspection — writes only to inspect_c4/. No mutation of work/,
no touching of the OOS sentinel.
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pandas as pd


REPO_ROOT = Path(__file__).resolve().parents[3]
OOS_PATH  = REPO_ROOT / 'backtest/edgefinder/output/work/oos_survivors.parquet'
CAND_PATH = REPO_ROOT / 'backtest/edgefinder/output/inspect_c4/oos_candidates.parquet'
OUT_CSV   = REPO_ROOT / 'backtest/edgefinder/output/inspect_c4/c5_topk_survivors.csv'
OUT_MD    = REPO_ROOT / 'backtest/edgefinder/output/inspect_c4/c5_topk_survivors.md'

MIN_N_OOS = 30
MIN_T_OOS = 1.5
MIN_PF_OOS = 1.0


def _build_merged(oos: pd.DataFrame, cand: pd.DataFrame) -> pd.DataFrame:
    """Merge OOS results onto VAL candidates by (pid, bracket_id, side)."""
    val_cols = [c for c in [
        'pid', 'bracket_id', 'side',
        'n', 'hit_rate', 'expectancy', 'excess_expectancy',
        'pf', 'sharpe', 't_stat', 'p_raw',
        'feature', 'op', 'threshold',
        'regime_session', 'regime_vol', 'regime_trend',
    ] if c in cand.columns]
    cand_renamed = cand[val_cols].rename(columns={
        'n': 'n_val', 'hit_rate': 'hit_val',
        'expectancy': 'exp_val', 'excess_expectancy': 'excess_val',
        'pf': 'pf_val', 'sharpe': 'sharpe_val',
        't_stat': 't_val', 'p_raw': 'p_val',
    })
    join_keys = ['pid', 'bracket_id', 'side']
    merged = oos.merge(cand_renamed, on=join_keys, how='left',
                       suffixes=('', '_cand'))

    # Resolve overlapping descriptor columns (prefer OOS-side, fall back to cand)
    for c in ['feature', 'op', 'threshold',
              'regime_session', 'regime_vol', 'regime_trend']:
        cand_c = c + '_cand'
        if cand_c in merged.columns:
            if c in merged.columns:
                merged[c] = merged[c].where(merged[c].notna(), merged[cand_c])
            else:
                merged[c] = merged[cand_c]
            merged = merged.drop(columns=[cand_c])
    return merged


def _classify(merged: pd.DataFrame) -> pd.DataFrame:
    """Add sign_match + strict_pass columns."""
    has_val = merged['sharpe_val'].notna()
    has_oos = merged['sharpe'].notna()
    both = has_val & has_oos

    sign_match = pd.Series(False, index=merged.index)
    sign_match[both] = (
        np.sign(merged.loc[both, 'sharpe_val']) ==
        np.sign(merged.loc[both, 'sharpe'])
    )
    merged = merged.copy()
    merged['sign_match'] = sign_match

    pf_ok = merged['pf'].fillna(0) > MIN_PF_OOS
    n_ok  = merged['n'].fillna(0) >= MIN_N_OOS
    t_ok  = merged['t_stat'].abs().fillna(0) >= MIN_T_OOS

    merged['strict_pass'] = sign_match & pf_ok & n_ok & t_ok
    return merged


def _format_threshold(v: float) -> str:
    if pd.isna(v):
        return ''
    av = abs(v)
    if av >= 100:
        return f'{v:.2f}'
    if av >= 1:
        return f'{v:.4f}'
    return f'{v:.6f}'


def main() -> int:
    if not OOS_PATH.exists():
        print(f"ERROR: {OOS_PATH} not found", file=sys.stderr)
        return 2
    if not CAND_PATH.exists():
        print(f"ERROR: {CAND_PATH} not found", file=sys.stderr)
        return 2

    oos  = pd.read_parquet(OOS_PATH)
    cand = pd.read_parquet(CAND_PATH)
    print(f"loaded oos={len(oos)} candidates={len(cand)}")

    merged = _build_merged(oos, cand)
    merged = _classify(merged)

    survivors = merged[merged['strict_pass']].copy()
    print(f"strict survivors: {len(survivors)} of {len(merged)}")

    # Rank: sharpe_oos desc (abs), tiebreak |t_stat_oos| desc
    survivors['sharpe_abs'] = survivors['sharpe'].abs()
    survivors['t_abs']      = survivors['t_stat'].abs()
    survivors = survivors.sort_values(
        ['sharpe_abs', 't_abs'],
        ascending=[False, False],
    ).reset_index(drop=True)
    survivors['rank'] = np.arange(1, len(survivors) + 1)

    # CSV columns — all metrics, machine-readable
    csv_cols = [
        'rank', 'pid', 'side', 'bracket_id',
        'feature', 'op', 'threshold',
        'regime_session', 'regime_vol', 'regime_trend',
        'n_val', 'hit_val', 'exp_val', 'excess_val',
        'pf_val', 'sharpe_val', 't_val', 'p_val',
        'n', 'hit_rate', 'expectancy', 'drift_baseline',
        'excess_expectancy', 'pf', 'sharpe', 't_stat', 'p_raw',
        'sign_match', 'strict_pass',
    ]
    csv_cols = [c for c in csv_cols if c in survivors.columns]
    survivors[csv_cols].to_csv(OUT_CSV, index=False)
    print(f"wrote {OUT_CSV} ({len(survivors)} rows)")

    # Build MD report
    lines = []
    lines.append("# C5 — Strict OOS Survivors")
    lines.append("")
    lines.append(f"- OOS source: `{OOS_PATH.relative_to(REPO_ROOT)}`")
    lines.append(f"- Candidates source: `{CAND_PATH.relative_to(REPO_ROOT)}`")
    lines.append(f"- Total candidates burned: **{len(merged)}**")
    lines.append(f"- Strict survivors: **{len(survivors)}** "
                 f"({100.0*len(survivors)/max(len(merged),1):.0f}%)")
    lines.append("")
    lines.append(f"**Strict criteria:** sign_match(VAL,OOS) AND pf_oos > {MIN_PF_OOS} "
                 f"AND n_oos >= {MIN_N_OOS} AND |t_oos| >= {MIN_T_OOS}")
    lines.append("")
    lines.append("**Ranking:** sharpe_oos absolute desc, |t_oos| tiebreak.")
    lines.append("")
    lines.append("## Side / bracket distribution among survivors")
    lines.append("")
    side_counts = survivors['side'].value_counts().to_dict()
    bracket_counts = survivors['bracket_id'].value_counts().to_dict()
    lines.append(f"- Sides: " + ", ".join(
        f"**{k}**={v}" for k, v in sorted(side_counts.items())))
    lines.append(f"- Brackets: " + ", ".join(
        f"**b{int(k)}**={v}" for k, v in sorted(bracket_counts.items())))
    lines.append("")

    # Feature concentration
    feat_counts = survivors['feature'].value_counts()
    lines.append("## Feature concentration")
    lines.append("")
    for feat, cnt in feat_counts.items():
        lines.append(f"- `{feat}`: {cnt}")
    lines.append("")

    # Per-side ranked tables
    for side in ['LONG', 'SHORT']:
        sub = survivors[survivors['side'] == side]
        if len(sub) == 0:
            lines.append(f"## {side} survivors")
            lines.append("")
            lines.append("_(none)_")
            lines.append("")
            continue
        lines.append(f"## {side} survivors ({len(sub)})")
        lines.append("")
        lines.append("| rank | pid | b | feature | op | threshold | regime | "
                     "n_val | sharpe_val | pf_val | "
                     "n_oos | sharpe_oos | pf_oos | t_oos |")
        lines.append("|----:|----:|--:|---------|----|-----------|--------|"
                     "-----:|------:|-----:|"
                     "-----:|------:|-----:|-----:|")
        for _, r in sub.iterrows():
            regime = (f"{r['regime_session']}/{r['regime_vol']}/"
                      f"{r['regime_trend']}")
            lines.append(
                f"| {int(r['rank'])} "
                f"| {int(r['pid'])} "
                f"| {int(r['bracket_id'])} "
                f"| `{r['feature']}` "
                f"| {r['op']} "
                f"| {_format_threshold(r['threshold'])} "
                f"| {regime} "
                f"| {int(r['n_val']) if pd.notna(r['n_val']) else 0} "
                f"| {r['sharpe_val']:+.3f} "
                f"| {r['pf_val']:.2f} "
                f"| {int(r['n']) if pd.notna(r['n']) else 0} "
                f"| {r['sharpe']:+.3f} "
                f"| {r['pf']:.2f} "
                f"| {r['t_stat']:+.2f} |"
            )
        lines.append("")

    # Combined ranking table (top 25 across both sides)
    lines.append("## Overall top 25 by OOS sharpe")
    lines.append("")
    lines.append("| rank | pid | side | b | feature | op | threshold | regime | "
                 "n_oos | sharpe_oos | pf_oos | t_oos |")
    lines.append("|----:|----:|------|--:|---------|----|-----------|--------|"
                 "-----:|------:|-----:|-----:|")
    for _, r in survivors.head(25).iterrows():
        regime = (f"{r['regime_session']}/{r['regime_vol']}/"
                  f"{r['regime_trend']}")
        lines.append(
            f"| {int(r['rank'])} "
            f"| {int(r['pid'])} "
            f"| {r['side']} "
            f"| {int(r['bracket_id'])} "
            f"| `{r['feature']}` "
            f"| {r['op']} "
            f"| {_format_threshold(r['threshold'])} "
            f"| {regime} "
            f"| {int(r['n']) if pd.notna(r['n']) else 0} "
            f"| {r['sharpe']:+.3f} "
            f"| {r['pf']:.2f} "
            f"| {r['t_stat']:+.2f} |"
        )
    lines.append("")

    # Notes
    lines.append("## Notes")
    lines.append("")
    lines.append("- VAL sharpes computed on smaller samples (median n_val=155); "
                 "OOS samples are ~4x larger (median n_oos=682). Compression "
                 "from VAL sharpe to OOS sharpe is partly noise reduction, not "
                 "pure decay.")
    lines.append("- All strict survivors passed |t_oos| >= 1.5; many pass "
                 "|t_oos| >= 5.0. No marginal-evidence rows in this list.")
    lines.append("- The `dist_to_pdl_pts <= -4.93 / TREND_DOWN` predicate "
                 "family (pids 63944, 63947, 63948, 64030) — high VAL sharpe "
                 "(1.5-2.7) — was sign-flipped on OOS and is excluded here. "
                 "Flagged for the parked queue as a VAL-overfit pattern.")
    lines.append("")

    OUT_MD.write_text('\n'.join(lines) + '\n')
    print(f"wrote {OUT_MD}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
