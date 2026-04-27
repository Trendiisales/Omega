"""
backtest/edgefinder/analytics/inspect_c5.py
===========================================

C5 — read-only diagnostic on OOS results.

Inputs:
    backtest/edgefinder/output/work/oos_survivors.parquet     (50 rows)
    backtest/edgefinder/output/inspect_c4/oos_candidates.parquet  (50 rows, the input)

Compares VAL→OOS for each candidate. Categorises survivors. Reports:
  - Per-row OOS expectancy / sharpe / pf / n / t_stat
  - Sign match vs VAL
  - Effective n (if mask population on OOS was thin, flag it)
  - Breakdowns: by side, bracket, regime tuple, feature
  - Top survivors by OOS sharpe (sign-matched only)
  - Failures: sign-flipped, pf<1, t_stat sub-threshold

Pure inspection — writes nothing back to disk except an MD summary
sibling to the OOS file. No mutation of work/ or output/.
"""
from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pandas as pd


REPO_ROOT = Path(__file__).resolve().parents[3]
OOS_PATH  = REPO_ROOT / 'backtest/edgefinder/output/work/oos_survivors.parquet'
CAND_PATH = REPO_ROOT / 'backtest/edgefinder/output/inspect_c4/oos_candidates.parquet'
OUT_PATH  = REPO_ROOT / 'backtest/edgefinder/output/inspect_c4/c5_oos_results.md'


def _fmt_sharpe(x: float) -> str:
    if pd.isna(x):
        return '   nan'
    return f'{x:+6.3f}'


def _fmt_pct(x: float) -> str:
    if pd.isna(x):
        return '  nan'
    return f'{100.0*x:5.1f}'


def main() -> int:
    if not OOS_PATH.exists():
        print(f"ERROR: {OOS_PATH} not found", file=sys.stderr)
        return 2
    if not CAND_PATH.exists():
        print(f"ERROR: {CAND_PATH} not found", file=sys.stderr)
        return 2

    oos = pd.read_parquet(OOS_PATH)
    cand = pd.read_parquet(CAND_PATH)

    print(f"OOS results: {len(oos)} rows, columns: {list(oos.columns)}")
    print(f"Candidates:  {len(cand)} rows, columns: {list(cand.columns)}")
    print()

    # ------------------------------------------------------------------
    # Join VAL stats from candidates onto OOS results.
    # Candidates have VAL-side metrics; rename to *_val for clarity.
    # ------------------------------------------------------------------
    val_cols_to_keep = [c for c in [
        'pid', 'bracket_id', 'side',
        'n', 'hit_rate', 'expectancy', 'excess_expectancy',
        'pf', 'sharpe', 't_stat', 'p_raw',
        'feature', 'op', 'threshold',
        'regime_session', 'regime_vol', 'regime_trend',
    ] if c in cand.columns]
    cand_renamed = cand[val_cols_to_keep].copy()
    rename_map = {
        'n': 'n_val', 'hit_rate': 'hit_val',
        'expectancy': 'exp_val', 'excess_expectancy': 'excess_val',
        'pf': 'pf_val', 'sharpe': 'sharpe_val',
        't_stat': 't_val', 'p_raw': 'p_val',
    }
    cand_renamed = cand_renamed.rename(columns=rename_map)

    join_keys = ['pid', 'bracket_id', 'side']
    merged = oos.merge(cand_renamed, on=join_keys, how='left',
                       suffixes=('', '_cand'))
    if len(merged) != len(oos):
        print(f"WARN: merge changed row count {len(oos)} → {len(merged)}",
              file=sys.stderr)

    # Resolve overlapping descriptor columns (feature/op/threshold/regime_*)
    # to a single canonical column. Prefer OOS-side if present, else candidate.
    for c in ['feature', 'op', 'threshold',
              'regime_session', 'regime_vol', 'regime_trend']:
        cand_c = c + '_cand'
        if cand_c in merged.columns:
            if c in merged.columns:
                merged[c] = merged[c].where(merged[c].notna(), merged[cand_c])
            else:
                merged[c] = merged[cand_c]
            merged = merged.drop(columns=[cand_c])

    # ------------------------------------------------------------------
    # Survival categorisation
    # ------------------------------------------------------------------
    # Sign-match: sharpe sign on OOS matches sharpe sign on VAL.
    # (Both are direction-corrected: SHORT side already negates pnl
    #  inside reapply_to_partition, so a positive sharpe == edge in the
    #  predicted direction on both partitions.)
    has_val_sharpe = merged['sharpe_val'].notna()
    has_oos_sharpe = merged['sharpe'].notna()
    both_finite = has_val_sharpe & has_oos_sharpe

    # NaN sharpe on OOS means n_oos was zero or pnl all non-finite.
    n_zero_oos = int((merged['n'].fillna(0) == 0).sum())
    print(f"OOS rows with n=0 (predicate matched zero OOS bars): {n_zero_oos}")

    sign_match = pd.Series(False, index=merged.index)
    sign_match[both_finite] = (
        np.sign(merged.loc[both_finite, 'sharpe_val']) ==
        np.sign(merged.loc[both_finite, 'sharpe'])
    )
    merged['sign_match'] = sign_match

    # Tiered survival
    pf_ok      = merged['pf'].fillna(0) > 1.0
    sharpe_pos = merged['sharpe'].fillna(0) > 0
    n_ok       = merged['n'].fillna(0) >= 30   # min OOS sample
    t_ok       = merged['t_stat'].abs().fillna(0) >= 1.0  # weak filter

    merged['oos_pass_strict'] = sign_match & pf_ok & n_ok & (merged['t_stat'].abs().fillna(0) >= 1.5)
    merged['oos_pass_loose']  = sign_match & pf_ok & n_ok

    n_strict = int(merged['oos_pass_strict'].sum())
    n_loose  = int(merged['oos_pass_loose'].sum())
    n_sign   = int(sign_match.sum())

    print()
    print("=" * 72)
    print("SURVIVAL SUMMARY")
    print("=" * 72)
    print(f"  Total candidates burned on OOS:  {len(merged)}")
    print(f"  Sign-matched (vs VAL):           {n_sign} ({100*n_sign/len(merged):.0f}%)")
    print(f"  Loose pass (sign + pf>1 + n>=30): {n_loose} ({100*n_loose/len(merged):.0f}%)")
    print(f"  Strict pass (loose + |t|>=1.5):  {n_strict} ({100*n_strict/len(merged):.0f}%)")
    print(f"  n=0 OOS (no mask hits):          {n_zero_oos}")
    print()

    # ------------------------------------------------------------------
    # Per-side breakdown
    # ------------------------------------------------------------------
    print("=" * 72)
    print("BY SIDE")
    print("=" * 72)
    for side in ['LONG', 'SHORT']:
        sub = merged[merged['side'] == side]
        if len(sub) == 0:
            continue
        n_total  = len(sub)
        n_sm     = int(sub['sign_match'].sum())
        n_loose_ = int(sub['oos_pass_loose'].sum())
        n_strict_= int(sub['oos_pass_strict'].sum())
        med_sh_v = sub['sharpe_val'].median()
        med_sh_o = sub['sharpe'].median()
        print(f"  {side:5s}: total={n_total:2d}  sign_match={n_sm:2d}  "
              f"loose={n_loose_:2d}  strict={n_strict_:2d}  "
              f"median_sharpe VAL={_fmt_sharpe(med_sh_v)} OOS={_fmt_sharpe(med_sh_o)}")
    print()

    # ------------------------------------------------------------------
    # Per-bracket breakdown
    # ------------------------------------------------------------------
    print("=" * 72)
    print("BY BRACKET")
    print("=" * 72)
    for b in sorted(merged['bracket_id'].unique()):
        sub = merged[merged['bracket_id'] == b]
        n_total = len(sub)
        n_sm    = int(sub['sign_match'].sum())
        n_loose_= int(sub['oos_pass_loose'].sum())
        med_o   = sub['sharpe'].median()
        print(f"  b{int(b)}: total={n_total:2d}  sign_match={n_sm:2d}  "
              f"loose={n_loose_:2d}  median_OOS_sharpe={_fmt_sharpe(med_o)}")
    print()

    # ------------------------------------------------------------------
    # Top survivors by OOS sharpe (sign-matched, n>=30)
    # ------------------------------------------------------------------
    print("=" * 72)
    print("TOP 20 SURVIVORS BY OOS SHARPE (sign-matched, n_oos >= 30)")
    print("=" * 72)
    show_cols = ['pid', 'side', 'bracket_id', 'feature', 'op', 'threshold',
                 'regime_session', 'regime_vol', 'regime_trend',
                 'n_val', 'sharpe_val', 'pf_val',
                 'n', 'sharpe', 'pf', 't_stat', 'sign_match']
    show_cols = [c for c in show_cols if c in merged.columns]
    survivors = merged[merged['sign_match'] & (merged['n'].fillna(0) >= 30)].copy()
    survivors = survivors.sort_values('sharpe', ascending=False, key=lambda s: s.abs())
    print(survivors[show_cols].head(20).to_string(index=False))
    print()

    # ------------------------------------------------------------------
    # Failures (sign flip OR pf<=1)
    # ------------------------------------------------------------------
    print("=" * 72)
    print("FAILURES — sign-flipped or pf<=1 (top 10 worst by |sharpe_val|)")
    print("=" * 72)
    failed = merged[(~merged['sign_match']) | (merged['pf'].fillna(0) <= 1.0)].copy()
    failed = failed.sort_values('sharpe_val', ascending=False, key=lambda s: s.abs())
    print(failed[show_cols].head(10).to_string(index=False))
    print(f"\n  Total failures: {len(failed)} of {len(merged)} "
          f"({100*len(failed)/len(merged):.0f}%)")
    print()

    # ------------------------------------------------------------------
    # Feature & regime concentration of survivors
    # ------------------------------------------------------------------
    print("=" * 72)
    print("FEATURE CONCENTRATION (loose survivors only)")
    print("=" * 72)
    loose = merged[merged['oos_pass_loose']]
    if len(loose) > 0:
        feat_counts = loose['feature'].value_counts()
        print(feat_counts.to_string())
        print()
        print(f"  unique features in survivors: {len(feat_counts)} of "
              f"{merged['feature'].nunique()} candidates")
    else:
        print("  (no loose survivors)")
    print()

    print("=" * 72)
    print("REGIME TUPLE CONCENTRATION (loose survivors only)")
    print("=" * 72)
    if len(loose) > 0:
        rcols = [c for c in ['regime_session', 'regime_vol', 'regime_trend']
                 if c in loose.columns]
        if rcols:
            tup = loose[rcols].apply(
                lambda r: '|'.join(str(v) for v in r.values), axis=1)
            print(tup.value_counts().to_string())
    else:
        print("  (no loose survivors)")
    print()

    # ------------------------------------------------------------------
    # n_oos sanity — were any candidates starved?
    # ------------------------------------------------------------------
    print("=" * 72)
    print("OOS SAMPLE SIZE DISTRIBUTION")
    print("=" * 72)
    n_series = merged['n'].fillna(0)
    print(f"  n=0:        {int((n_series == 0).sum())}")
    print(f"  1<=n<30:    {int(((n_series >= 1) & (n_series < 30)).sum())}")
    print(f"  30<=n<100:  {int(((n_series >= 30) & (n_series < 100)).sum())}")
    print(f"  100<=n<500: {int(((n_series >= 100) & (n_series < 500)).sum())}")
    print(f"  n>=500:     {int((n_series >= 500).sum())}")
    print(f"  median n_oos: {int(n_series.median())}")
    print(f"  median n_val: {int(merged['n_val'].fillna(0).median())}")
    print()

    # ------------------------------------------------------------------
    # Write MD summary
    # ------------------------------------------------------------------
    lines = []
    lines.append("# C5 — OOS Burn Results")
    lines.append("")
    lines.append(f"- Source: `{OOS_PATH.relative_to(REPO_ROOT)}`")
    lines.append(f"- Candidates: `{CAND_PATH.relative_to(REPO_ROOT)}`")
    lines.append("")
    lines.append("## Survival summary")
    lines.append("")
    lines.append(f"- Total burned: **{len(merged)}**")
    lines.append(f"- Sign-matched (vs VAL): **{n_sign}** "
                 f"({100*n_sign/len(merged):.0f}%)")
    lines.append(f"- Loose pass (sign + pf>1 + n_oos>=30): **{n_loose}** "
                 f"({100*n_loose/len(merged):.0f}%)")
    lines.append(f"- Strict pass (loose + |t|>=1.5): **{n_strict}** "
                 f"({100*n_strict/len(merged):.0f}%)")
    lines.append(f"- n=0 OOS: **{n_zero_oos}**")
    lines.append("")
    lines.append("## By side")
    lines.append("")
    lines.append("| side | total | sign_match | loose | strict | "
                 "median sharpe VAL | median sharpe OOS |")
    lines.append("|------|------:|-----------:|------:|-------:|"
                 "------------------:|------------------:|")
    for side in ['LONG', 'SHORT']:
        sub = merged[merged['side'] == side]
        if len(sub) == 0:
            continue
        lines.append(f"| {side} | {len(sub)} | "
                     f"{int(sub['sign_match'].sum())} | "
                     f"{int(sub['oos_pass_loose'].sum())} | "
                     f"{int(sub['oos_pass_strict'].sum())} | "
                     f"{sub['sharpe_val'].median():+.3f} | "
                     f"{sub['sharpe'].median():+.3f} |")
    lines.append("")
    lines.append("## By bracket")
    lines.append("")
    lines.append("| bracket | total | sign_match | loose | median OOS sharpe |")
    lines.append("|--------:|------:|-----------:|------:|------------------:|")
    for b in sorted(merged['bracket_id'].unique()):
        sub = merged[merged['bracket_id'] == b]
        lines.append(f"| {int(b)} | {len(sub)} | "
                     f"{int(sub['sign_match'].sum())} | "
                     f"{int(sub['oos_pass_loose'].sum())} | "
                     f"{sub['sharpe'].median():+.3f} |")
    lines.append("")

    OUT_PATH.write_text('\n'.join(lines) + '\n')
    print(f"Wrote summary: {OUT_PATH}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
