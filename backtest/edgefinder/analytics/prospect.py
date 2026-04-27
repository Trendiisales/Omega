"""
edgefinder/analytics/prospect.py
================================

Per-predicate edge evaluation. Given:

  * A panel partition (TRAIN / VAL / OOS) with regime columns applied.
  * A predicate catalogue from regime.py.

Compute, for every predicate × bracket × side (LONG / SHORT):

    n            sample count
    hit_rate     fraction of shots with PnL > 0
    expectancy   mean PnL per shot in points
    pf           profit factor: sum(wins) / |sum(losses)|
    sharpe       expectancy / stdev(PnL) (per-shot sharpe, NOT annualised)
    t_stat       Welch t-stat of mean vs zero
    p_raw        two-sided p-value from t_stat (gaussian approximation,
                 valid here because n >= 1000)

LONG side uses fwd_bracket_pts_<i> directly. SHORT side negates them; this is
correct because brackets in PanelSchema are symmetric (a +20 TP / -10 SL long
becomes a +10 SL hit / -20 TP miss when shorted, which negation captures).

A min-N filter (default 1000, per design decision) drops predicates that
trigger too rarely to be statistically meaningful. Filtered rows are NOT
written; this keeps the catalogue lean for MTC.

Performance notes
-----------------
The catalogue contains ~90k predicates; the panel has ~760k rows. Naive
double-loop = 68B comparisons. We avoid that by:

  1. Pre-extracting the bracket-PnL columns and warmed/fwd_complete masks once.
  2. Iterating predicates and computing the mask only for the rows we need.
     Each mask is O(n_rows) but with cheap numpy ops; ~90k * 760k = 68B
     comparisons total, but as numpy vector ops this runs in tens of minutes,
     not days. We could do better with grouping but the bookkeeping costs
     more than it saves at this scale.
  3. Skipping a predicate as soon as its mask sum drops below min_n.
"""
from __future__ import annotations

from dataclasses import dataclass, asdict
from typing import Iterable

import numpy as np
import pandas as pd

from .regime import PredicateSpec, predicate_mask


N_BRACKETS = 6
SIDES = ('LONG', 'SHORT')
BRACKET_COLS = [f'fwd_bracket_pts_{i}' for i in range(N_BRACKETS)]


@dataclass
class ProspectRow:
    pid:           int
    feature:       str
    op:            str
    threshold:     float
    regime_session:str
    regime_vol:    str
    regime_trend:  str
    side:          str           # 'LONG' or 'SHORT'
    bracket_id:    int           # 0..N_BRACKETS-1
    n:             int
    hit_rate:      float
    expectancy:    float
    pf:            float
    sharpe:        float
    t_stat:        float
    p_raw:         float

    def to_dict(self) -> dict:
        return asdict(self)


def _summarise(pnl: np.ndarray) -> tuple[float, float, float, float, float, float]:
    """
    Compute (hit_rate, expectancy, pf, sharpe, t_stat, p_raw) over a 1-d
    pnl array. Caller guarantees pnl has at least min_n samples.

    Uses gaussian approx for p-value: valid for n >= 30 by CLT, very accurate
    by n=1000. Two-sided.
    """
    n = pnl.size
    mean = float(np.mean(pnl))
    std  = float(np.std(pnl, ddof=1))
    wins = pnl[pnl > 0]
    losses = pnl[pnl < 0]
    sum_w = float(np.sum(wins))
    sum_l = float(np.sum(losses))
    hit   = float(wins.size) / float(n) if n > 0 else 0.0
    pf    = (sum_w / abs(sum_l)) if sum_l < 0 else float('inf')

    if std <= 0.0:
        sharpe = 0.0
        t_stat = 0.0
        p_raw  = 1.0
    else:
        sharpe = mean / std
        t_stat = mean / (std / np.sqrt(n))
        # two-sided gaussian p
        from math import erfc, sqrt
        p_raw = float(erfc(abs(t_stat) / sqrt(2.0)))

    return hit, mean, pf, sharpe, t_stat, p_raw


def score_predicates(
    df: pd.DataFrame,
    catalogue: Iterable[PredicateSpec],
    min_n: int = 1000,
    progress_every: int = 5000,
) -> pd.DataFrame:
    """
    Score every (predicate × side × bracket) combination on `df` and return
    a DataFrame of ProspectRow rows. Predicates whose mask has fewer than
    min_n hits across ALL six brackets are skipped entirely (mask is the
    same for all brackets; only the PnL column changes).

    `df` must have regime columns applied. `catalogue` is the output of
    regime.build_predicate_catalogue().
    """
    # Pre-extract PnL columns as numpy arrays (one-time copy, much faster
    # than repeatedly going through pandas .loc).
    bracket_pnl = np.column_stack([
        df[col].to_numpy(dtype=np.float64) for col in BRACKET_COLS
    ])  # shape (n_rows, N_BRACKETS)

    out_rows: list[dict] = []
    n_seen = 0
    n_kept = 0
    for spec in catalogue:
        n_seen += 1
        if progress_every and n_seen % progress_every == 0:
            print(f"  scored {n_seen} predicates, kept {n_kept} rows so far")

        m = predicate_mask(df, spec)
        n_hit = int(m.sum())
        if n_hit < min_n:
            continue

        # Same mask, evaluate each bracket × side.
        sub_pnl = bracket_pnl[m]   # (n_hit, N_BRACKETS)
        # Drop any rows where ANY bracket is NaN (would corrupt the mean).
        # This should be rare since we already require fwd_complete, but
        # belt-and-braces.
        finite = np.isfinite(sub_pnl).all(axis=1)
        if int(finite.sum()) < min_n:
            continue
        sub_pnl = sub_pnl[finite]
        n_eff = sub_pnl.shape[0]

        for b in range(N_BRACKETS):
            long_pnl  =  sub_pnl[:, b]
            short_pnl = -sub_pnl[:, b]

            hit, mean, pf, sh, t, p = _summarise(long_pnl)
            out_rows.append(dict(
                pid=spec.pid, feature=spec.feature, op=spec.op,
                threshold=spec.threshold,
                regime_session=spec.regime_session,
                regime_vol=spec.regime_vol,
                regime_trend=spec.regime_trend,
                side='LONG', bracket_id=b, n=n_eff,
                hit_rate=hit, expectancy=mean, pf=pf, sharpe=sh,
                t_stat=t, p_raw=p,
            ))
            n_kept += 1

            hit, mean, pf, sh, t, p = _summarise(short_pnl)
            out_rows.append(dict(
                pid=spec.pid, feature=spec.feature, op=spec.op,
                threshold=spec.threshold,
                regime_session=spec.regime_session,
                regime_vol=spec.regime_vol,
                regime_trend=spec.regime_trend,
                side='SHORT', bracket_id=b, n=n_eff,
                hit_rate=hit, expectancy=mean, pf=pf, sharpe=sh,
                t_stat=t, p_raw=p,
            ))
            n_kept += 1

    print(f"  finished: scored {n_seen} predicates, produced {n_kept} prospect rows")
    if not out_rows:
        return pd.DataFrame(columns=[f.name for f in ProspectRow.__dataclass_fields__.values()])
    return pd.DataFrame(out_rows)


def reapply_to_partition(
    df_partition: pd.DataFrame,
    train_prospects: pd.DataFrame,
    catalogue_by_pid: dict[int, PredicateSpec],
) -> pd.DataFrame:
    """
    Re-evaluate a TRAIN-survivor set on another partition (VAL or OOS).

    `train_prospects` is a DataFrame produced by score_predicates() on TRAIN,
    typically already filtered (e.g., MTC-survivors only). For each row we
    look up the predicate spec by pid, recompute the mask on `df_partition`,
    and produce the same statistics. min_n is NOT enforced here — VAL/OOS
    samples can be smaller than 1000; the caller decides what to do.
    """
    if 'pid' not in train_prospects.columns:
        raise ValueError("train_prospects must contain 'pid' column")

    bracket_pnl = np.column_stack([
        df_partition[col].to_numpy(dtype=np.float64) for col in BRACKET_COLS
    ])

    # Group rows by pid so we evaluate each predicate's mask only once.
    out_rows: list[dict] = []
    for pid, sub in train_prospects.groupby('pid', sort=False):
        spec = catalogue_by_pid.get(int(pid))
        if spec is None:
            # Catalogue/predicates desynced; report 0 rather than crash.
            for _, r in sub.iterrows():
                out_rows.append(_empty_row(r))
            continue

        m = predicate_mask(df_partition, spec)
        if not m.any():
            for _, r in sub.iterrows():
                out_rows.append(_empty_row(r))
            continue

        sub_pnl = bracket_pnl[m]
        finite = np.isfinite(sub_pnl).all(axis=1)
        sub_pnl = sub_pnl[finite]
        if sub_pnl.shape[0] == 0:
            for _, r in sub.iterrows():
                out_rows.append(_empty_row(r))
            continue
        n_eff = sub_pnl.shape[0]

        # We only need to evaluate the (bracket_id, side) combos present in
        # `sub` — typically we re-evaluate all 12, but if the caller
        # pre-filtered (e.g., to one bracket) we still respect that.
        for _, r in sub.iterrows():
            b = int(r['bracket_id'])
            side = str(r['side'])
            pnl = sub_pnl[:, b]
            if side == 'SHORT':
                pnl = -pnl
            hit, mean, pf, sh, t, p = _summarise(pnl)
            out_rows.append(dict(
                pid=int(pid), feature=spec.feature, op=spec.op,
                threshold=spec.threshold,
                regime_session=spec.regime_session,
                regime_vol=spec.regime_vol,
                regime_trend=spec.regime_trend,
                side=side, bracket_id=b, n=n_eff,
                hit_rate=hit, expectancy=mean, pf=pf, sharpe=sh,
                t_stat=t, p_raw=p,
            ))

    return pd.DataFrame(out_rows)


def _empty_row(r: pd.Series) -> dict:
    """Synthesise a zero-sample prospect row (for partitions where the
    predicate never triggered)."""
    return dict(
        pid=int(r['pid']), feature=str(r['feature']), op=str(r['op']),
        threshold=float(r['threshold']),
        regime_session=str(r['regime_session']),
        regime_vol=str(r['regime_vol']),
        regime_trend=str(r['regime_trend']),
        side=str(r['side']), bracket_id=int(r['bracket_id']),
        n=0, hit_rate=0.0, expectancy=0.0, pf=0.0, sharpe=0.0,
        t_stat=0.0, p_raw=1.0,
    )
