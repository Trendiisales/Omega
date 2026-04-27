"""
edgefinder/analytics/mtc.py
===========================

Multiple-testing correction over a set of prospect rows. Two methods,
side-by-side:

  * Benjamini-Hochberg (BH) — controls the False Discovery Rate (FDR).
  * Bonferroni — controls the Family-Wise Error Rate (FWER).

Correction is applied SEPARATELY PER BRACKET.

Economic pre-filter
-------------------
Before MTC, we drop rows whose effect size is statistically detectable but
economically meaningless. With n=1000+ and per-shot stdev of 5-15 points,
a sub-spread excess_expectancy of 0.1 points produces large t-stats.
That's not an edge -- it's a high-precision measurement of nothing.

The pre-filter requires BOTH:
    |excess_expectancy| >= min_excess_pts   (default 1.0)
    sharpe              >= min_sharpe       (default 0.05)

Rows that fail get q_bh = q_bonf = 1.0, survives_bh = survives_bonf = False.
They are NOT dropped from the output (the report needs them for diagnosis)
but they don't consume MTC family budget -- this is critical, because
including them in the family makes the BH cutoff harsher and rejects
genuine borderline edges.

Outputs added to the input DataFrame:
    q_bh           — BH adjusted p-value (1.0 if economic-filtered)
    q_bonf         — Bonferroni adjusted p-value (1.0 if economic-filtered)
    economic_pass  — True if the row meets the economic threshold
    survives_bh    — economic_pass AND q_bh   <= alpha (default 0.05)
    survives_bonf  — economic_pass AND q_bonf <= alpha (default 0.05)
"""
from __future__ import annotations

import numpy as np
import pandas as pd


def _bh_qvalues(p: np.ndarray) -> np.ndarray:
    """Benjamini-Hochberg adjusted p-values."""
    n = p.size
    if n == 0:
        return p.copy()
    order = np.argsort(p, kind='mergesort')
    p_sorted = p[order]
    ranks = np.arange(1, n + 1, dtype=np.float64)
    raw = p_sorted * (n / ranks)
    q_sorted = np.minimum.accumulate(raw[::-1])[::-1]
    q_sorted = np.clip(q_sorted, 0.0, 1.0)
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
    min_excess_pts: float = 1.0,
    min_sharpe: float = 0.05,
) -> pd.DataFrame:
    """
    Apply economic pre-filter, then BH and Bonferroni corrections to
    `prospects`. Returns a copy with q_bh, q_bonf, economic_pass,
    survives_bh, survives_bonf columns added.

    Parameters
    ----------
    prospects : DataFrame
        Output of prospect.score_predicates(). Must contain
        p_raw, excess_expectancy, sharpe.
    alpha : float
        Significance threshold for the survives_* flags.
    per : {'bracket', 'bracket_side', 'all'}
        Family grouping. Default 'bracket'.
    min_excess_pts : float
        Minimum |excess_expectancy| (in points) for economic pass.
    min_sharpe : float
        Minimum per-shot sharpe for economic pass.
    """
    needed = {'p_raw', 'excess_expectancy', 'sharpe'}
    missing = needed - set(prospects.columns)
    if missing:
        raise ValueError(f"prospects missing required columns: {missing}")
    df = prospects.copy()

    # Economic pre-filter
    df['economic_pass'] = (
        (df['excess_expectancy'].abs() >= min_excess_pts) &
        (df['sharpe'].abs()             >= min_sharpe)
    )

    # Pre-fill q-values for everyone, then overwrite for economic-pass rows.
    df['q_bh']   = 1.0
    df['q_bonf'] = 1.0

    eligible = df[df['economic_pass']].copy()
    if eligible.empty:
        df['survives_bh']   = False
        df['survives_bonf'] = False
        return df

    # Group eligible rows by family and apply MTC within each family.
    if per == 'bracket':
        groupby_cols = ['bracket_id']
    elif per == 'bracket_side':
        groupby_cols = ['bracket_id', 'side']
    elif per == 'all':
        groupby_cols = None
    else:
        raise ValueError(f"unknown per: {per}")

    if groupby_cols is None:
        groups = [(None, eligible)]
    else:
        groups = list(eligible.groupby(groupby_cols, sort=False))

    for _, sub in groups:
        idx = sub.index.to_numpy()
        p = sub['p_raw'].to_numpy(dtype=np.float64)
        p = np.where(np.isfinite(p), p, 1.0)
        df.loc[idx, 'q_bh']   = _bh_qvalues(p)
        df.loc[idx, 'q_bonf'] = _bonf_qvalues(p)

    df['survives_bh']   = df['economic_pass'] & (df['q_bh']   <= alpha)
    df['survives_bonf'] = df['economic_pass'] & (df['q_bonf'] <= alpha)
    return df
