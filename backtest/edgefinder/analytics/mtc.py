"""
edgefinder/analytics/mtc.py
===========================

Multiple-testing correction over a set of prospect rows. We expose two
methods, side-by-side, per the design decision:

  * Benjamini-Hochberg (BH) — controls the False Discovery Rate (FDR).
    Less conservative; produces more candidates.
  * Bonferroni — controls the Family-Wise Error Rate (FWER).
    Most conservative; produces fewer false positives.

Correction is applied SEPARATELY PER BRACKET. The reason: the six brackets
have different risk/reward profiles and different effective sample sizes,
so the family of tested hypotheses is naturally per-bracket. Pooling all
brackets into one family would be defensible too but it would double-count
the "is there any edge here?" question across correlated outcomes.

Outputs added to the input DataFrame:
    q_bh    — Benjamini-Hochberg adjusted p-value
    q_bonf  — Bonferroni adjusted p-value
    survives_bh   — q_bh   <= alpha (default 0.05)
    survives_bonf — q_bonf <= alpha (default 0.05)
"""
from __future__ import annotations

import numpy as np
import pandas as pd


def _bh_qvalues(p: np.ndarray) -> np.ndarray:
    """
    Benjamini-Hochberg adjusted p-values.

    For sorted p-values p_(1) <= p_(2) <= ... <= p_(m):
        q_(i) = min over k>=i of (m / k) * p_(k)

    Returned in the original (unsorted) order of `p`.
    """
    n = p.size
    if n == 0:
        return p.copy()
    order = np.argsort(p, kind='mergesort')
    p_sorted = p[order]
    ranks = np.arange(1, n + 1, dtype=np.float64)
    raw = p_sorted * (n / ranks)
    # Enforce monotonicity from the right.
    q_sorted = np.minimum.accumulate(raw[::-1])[::-1]
    q_sorted = np.clip(q_sorted, 0.0, 1.0)
    # Restore original order.
    q = np.empty_like(q_sorted)
    q[order] = q_sorted
    return q


def _bonf_qvalues(p: np.ndarray) -> np.ndarray:
    """Bonferroni: q_i = min(1, m * p_i)."""
    n = p.size
    if n == 0:
        return p.copy()
    return np.clip(p * n, 0.0, 1.0)


def apply_mtc(
    prospects: pd.DataFrame,
    alpha: float = 0.05,
    per: str = 'bracket',
) -> pd.DataFrame:
    """
    Apply BH and Bonferroni corrections to `prospects` and return a copy
    with q_bh, q_bonf, survives_bh, survives_bonf columns added.

    Parameters
    ----------
    prospects : DataFrame
        Output of prospect.score_predicates(). Must contain p_raw.
    alpha : float
        Significance threshold for the survives_* flags.
    per : {'bracket', 'bracket_side', 'all'}
        Family grouping. Default 'bracket' = correct within each bracket
        (LONG and SHORT pooled). 'bracket_side' = correct within
        (bracket, side). 'all' = single family.
    """
    if 'p_raw' not in prospects.columns:
        raise ValueError("prospects must contain 'p_raw' column")
    df = prospects.copy()
    df['q_bh'] = np.nan
    df['q_bonf'] = np.nan

    if per == 'bracket':
        groups = df.groupby('bracket_id', sort=False)
    elif per == 'bracket_side':
        groups = df.groupby(['bracket_id', 'side'], sort=False)
    elif per == 'all':
        groups = [(None, df)]
    else:
        raise ValueError(f"unknown per: {per}")

    for _, sub in groups:
        idx = sub.index.to_numpy()
        p = sub['p_raw'].to_numpy(dtype=np.float64)
        # Replace NaNs with 1.0 (never significant).
        p = np.where(np.isfinite(p), p, 1.0)
        df.loc[idx, 'q_bh']   = _bh_qvalues(p)
        df.loc[idx, 'q_bonf'] = _bonf_qvalues(p)

    df['survives_bh']   = df['q_bh']   <= alpha
    df['survives_bonf'] = df['q_bonf'] <= alpha
    return df
