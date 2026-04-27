"""
edgefinder/analytics/inspect_c4_audit.py
========================================

Read-only audit of c4_clean and c4_full_flagged. Prints the diagnostic
distributions needed to choose how to tighten down to OOS-ready (~30-80
candidates) without burning OOS.

What it does:
  1. Sharpe / expectancy / pf percentile distributions on c4_clean.
  2. Bracket × side breakdown of c4_clean (where does the mass sit?).
  3. Feature concentration on c4_clean (how lumpy is the top-feature tail?).
  4. Regime concentration: which (regime_session, regime_vol, regime_trend)
     tuples are over-represented?
  5. Cross-tab of bracket_id × feature top-12 (is one bracket dominated by
     a small feature set?).
  6. Hypothetical top-K previews: at K=50 and K=30, show what the
     bracket/side/feature mix would look like if we just took top-K by
     |sharpe|.
  7. Sweep preview: at increasing |sharpe| / |excess| thresholds, how does
     n_clean shrink? Picks a few spots that look like ~30-80 targets.

NO mutations. NO panel load. NO OOS access. Reads only:
    backtest/edgefinder/output/inspect_c4/c4_clean.{parquet,pkl}
    backtest/edgefinder/output/inspect_c4/c4_full_flagged.{parquet,pkl}

Run:
    python3 -m backtest.edgefinder.analytics.inspect_c4_audit
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

DEFAULT_INDIR = 'backtest/edgefinder/output/inspect_c4'
CLEAN_BASE    = 'c4_clean'
FULL_BASE     = 'c4_full_flagged'


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


def _hr(title: str) -> None:
    bar = '=' * 78
    print()
    print(bar)
    print(f"  {title}")
    print(bar)


def _percentiles(s: pd.Series, qs=(0.0, 0.10, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99, 1.0)) -> str:
    s = s.dropna().to_numpy(dtype=np.float64)
    if s.size == 0:
        return "  (no data)"
    out = []
    for q in qs:
        v = np.quantile(s, q)
        out.append(f"  q{int(q*100):>3}={v:>+10.4f}")
    return "\n".join(out)


def section_distributions(clean: pd.DataFrame) -> None:
    _hr("1. VAL metric distributions on c4_clean (n = {})".format(len(clean)))
    for col in ['sharpe', 'excess_expectancy', 'expectancy', 'pf', 'n', 'hit_rate']:
        if col not in clean.columns:
            continue
        print(f"\n  {col}:")
        print(_percentiles(clean[col]))


def section_bracket_side(clean: pd.DataFrame) -> None:
    _hr("2. Bracket × side breakdown on c4_clean")
    if 'bracket_id' not in clean.columns or 'side' not in clean.columns:
        print("  (missing columns)")
        return
    pivot = clean.pivot_table(
        index='bracket_id', columns='side',
        values='pid', aggfunc='count', fill_value=0,
    )
    pivot['TOTAL'] = pivot.sum(axis=1)
    pivot.loc['TOTAL'] = pivot.sum(axis=0)
    print()
    print(pivot.to_string())

    # median sharpe per bracket
    print()
    print("  median |sharpe| per bracket (clean):")
    s = clean.assign(abs_sharpe=clean['sharpe'].abs())
    by_b = s.groupby('bracket_id')['abs_sharpe'].agg(['median', 'mean', 'max', 'count'])
    print(by_b.to_string())


def section_feature_concentration(clean: pd.DataFrame) -> None:
    _hr("3. Feature concentration on c4_clean")
    counts = clean['feature'].value_counts()
    cum = counts.cumsum()
    pct = (cum / counts.sum() * 100.0).round(1)
    head = pd.DataFrame({
        'count': counts,
        'cum':   cum,
        'cum_pct': pct,
    }).head(20)
    print()
    print(head.to_string())
    print(f"\n  total unique features: {counts.shape[0]}")
    print(f"  top 5 features cover : {pct.iloc[4]:.1f}% of clean rows" if len(pct) >= 5 else "")
    print(f"  top 10 features cover: {pct.iloc[9]:.1f}% of clean rows" if len(pct) >= 10 else "")


def section_regime_concentration(clean: pd.DataFrame) -> None:
    _hr("4. Regime concentration on c4_clean")
    for col in ['regime_session', 'regime_vol', 'regime_trend']:
        if col not in clean.columns:
            continue
        c = clean[col].value_counts()
        print(f"\n  {col}:")
        for k, v in c.items():
            print(f"    {str(k):16s} {int(v):>5}  ({100.0*v/len(clean):.1f}%)")

    print("\n  most over-represented (session, vol, trend) tuples:")
    tup = clean.groupby(['regime_session', 'regime_vol', 'regime_trend']).size()
    tup = tup.sort_values(ascending=False).head(15)
    for (s, v, t), n in tup.items():
        print(f"    {str(s):10s} {str(v):8s} {str(t):12s} {int(n):>5}  "
              f"({100.0*n/len(clean):.1f}%)")


def section_bracket_feature_crosstab(clean: pd.DataFrame, top_features: int = 12) -> None:
    _hr(f"5. Bracket × feature crosstab (top-{top_features} features)")
    top_feats = clean['feature'].value_counts().head(top_features).index.tolist()
    sub = clean[clean['feature'].isin(top_feats)]
    pivot = sub.pivot_table(
        index='feature', columns='bracket_id',
        values='pid', aggfunc='count', fill_value=0,
    )
    pivot = pivot.loc[top_feats]
    pivot['TOTAL'] = pivot.sum(axis=1)
    print()
    print(pivot.to_string())


def section_topk_preview(clean: pd.DataFrame, ks=(30, 50, 80, 150)) -> None:
    _hr("6. Top-K by |sharpe| previews (no commitment)")
    if 'sharpe' not in clean.columns:
        print("  (no sharpe column)")
        return
    ranked = clean.assign(abs_sharpe=clean['sharpe'].abs()) \
                  .sort_values('abs_sharpe', ascending=False, kind='mergesort')
    for k in ks:
        if k > len(ranked):
            print(f"\n  --- top-{k}: SKIPPED (only {len(ranked)} clean rows) ---")
            continue
        sub = ranked.head(k)
        print(f"\n  --- top-{k} ---")
        b = sub['bracket_id'].value_counts().sort_index()
        s = sub['side'].value_counts()
        f = sub['feature'].value_counts().head(8)
        sharpe_min = sub['abs_sharpe'].min()
        sharpe_max = sub['abs_sharpe'].max()
        print(f"    |sharpe| range: {sharpe_min:.4f} .. {sharpe_max:.4f}")
        print(f"    by bracket: " + ", ".join(f"b{int(bk)}={int(c)}" for bk, c in b.items()))
        print(f"    by side:    " + ", ".join(f"{si}={int(c)}" for si, c in s.items()))
        print(f"    top features (count):")
        for ft, ct in f.items():
            print(f"      {str(ft):28s} {int(ct)}")


def section_sharpe_sweep(clean: pd.DataFrame) -> None:
    _hr("7. Sweep: how clean count shrinks vs |sharpe| threshold")
    if 'sharpe' not in clean.columns:
        print("  (no sharpe column)")
        return
    abs_sh = clean['sharpe'].abs()
    print(f"\n  total clean: {len(clean)}")
    print(f"\n  {'|sharpe| >=':>14s}  {'n':>6s}  {'b3':>5s} {'b4':>5s} {'b5':>5s}  "
          f"{'LONG':>5s} {'SHORT':>5s}  unique_features")
    for thr in [0.05, 0.075, 0.10, 0.125, 0.15, 0.20, 0.25, 0.30, 0.40, 0.50]:
        sub = clean[abs_sh >= thr]
        b = sub['bracket_id'].value_counts()
        s = sub['side'].value_counts()
        unique_feats = sub['feature'].nunique()
        print(f"  {thr:>14.3f}  {len(sub):>6d}  "
              f"{int(b.get(3,0)):>5d} {int(b.get(4,0)):>5d} {int(b.get(5,0)):>5d}  "
              f"{int(s.get('LONG',0)):>5d} {int(s.get('SHORT',0)):>5d}  {unique_feats}")

    # Also do excess_expectancy sweep
    print(f"\n  {'|excess| >=':>14s}  {'n':>6s}  {'b3':>5s} {'b4':>5s} {'b5':>5s}  "
          f"{'LONG':>5s} {'SHORT':>5s}  unique_features")
    abs_ex = clean['excess_expectancy'].abs()
    for thr in [1.0, 2.0, 3.0, 5.0, 7.5, 10.0, 15.0, 20.0, 30.0, 50.0]:
        sub = clean[abs_ex >= thr]
        b = sub['bracket_id'].value_counts()
        s = sub['side'].value_counts()
        unique_feats = sub['feature'].nunique()
        print(f"  {thr:>14.3f}  {len(sub):>6d}  "
              f"{int(b.get(3,0)):>5d} {int(b.get(4,0)):>5d} {int(b.get(5,0)):>5d}  "
              f"{int(s.get('LONG',0)):>5d} {int(s.get('SHORT',0)):>5d}  {unique_feats}")

    # Combined: |sharpe| AND |excess|
    print(f"\n  combined |sharpe| AND |excess|:")
    print(f"  {'|sh|':>6s} {'|ex|':>6s}  {'n':>6s}  {'b3':>5s} {'b4':>5s} {'b5':>5s}  "
          f"unique_features")
    for sh_thr, ex_thr in [
        (0.10, 5.0), (0.10, 10.0), (0.15, 5.0), (0.15, 10.0),
        (0.20, 5.0), (0.20, 10.0), (0.25, 10.0), (0.30, 15.0),
    ]:
        sub = clean[(abs_sh >= sh_thr) & (abs_ex >= ex_thr)]
        b = sub['bracket_id'].value_counts()
        unique_feats = sub['feature'].nunique()
        print(f"  {sh_thr:>6.3f} {ex_thr:>6.2f}  {len(sub):>6d}  "
              f"{int(b.get(3,0)):>5d} {int(b.get(4,0)):>5d} {int(b.get(5,0)):>5d}  "
              f"{unique_feats}")


def section_full_vs_clean(full: pd.DataFrame, clean: pd.DataFrame) -> None:
    _hr("8. Full-flagged vs clean (what got dropped, why)")
    print(f"\n  full_flagged: {len(full)}")
    print(f"  clean:        {len(clean)}")
    dropped = len(full) - len(clean)
    print(f"  dropped:      {dropped}")

    if 'is_absolute_price' in full.columns:
        n_abs = int(full['is_absolute_price'].sum())
        print(f"\n  abs-price rows in full: {n_abs}")
        # how many of the dropped were abs-price vs failing other filters?
        # full has: abs-price OR failed-spread OR failed-time-cluster
        f_dropped_abs = int(full[full['is_absolute_price']].shape[0])
        print(f"  of dropped {dropped}, abs-price contribution: {f_dropped_abs}")
        other_drops = dropped - f_dropped_abs
        print(f"  spread/time-cluster drops: ~{other_drops}")

    if 'spread_clean_share' in full.columns:
        sc = full['spread_clean_share'].dropna()
        print(f"\n  spread_clean_share distribution (full):")
        print(_percentiles(sc))

    if 'time_cluster_max' in full.columns:
        tc = full['time_cluster_max'].dropna()
        print(f"\n  time_cluster_max distribution (full):")
        print(_percentiles(tc))


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog='inspect_c4_audit',
        description='Read-only audit of inspect_c4 outputs.',
    )
    p.add_argument('--indir', default=DEFAULT_INDIR,
                   help='dir containing c4_clean and c4_full_flagged')
    args = p.parse_args(argv)

    indir = Path(args.indir)
    print(f"Reading from {indir}")
    clean = _load_df(indir / CLEAN_BASE)
    full  = _load_df(indir / FULL_BASE)
    print(f"  c4_clean:        {len(clean)} rows, {len(clean.columns)} cols")
    print(f"  c4_full_flagged: {len(full)} rows, {len(full.columns)} cols")

    section_distributions(clean)
    section_bracket_side(clean)
    section_feature_concentration(clean)
    section_regime_concentration(clean)
    section_bracket_feature_crosstab(clean)
    section_topk_preview(clean)
    section_sharpe_sweep(clean)
    section_full_vs_clean(full, clean)

    print()
    print("=" * 78)
    print("  done. no files written. no OOS touched.")
    print("=" * 78)
    return 0


if __name__ == '__main__':
    sys.exit(main())
