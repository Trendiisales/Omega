"""
edgefinder/analytics/prospect.py
================================

Per-predicate edge evaluation. Given:

  * A panel partition (TRAIN / VAL / OOS) with regime columns applied.
  * A predicate catalogue from regime.py.
  * A drift baseline (per side, per bracket) representing the null
    hypothesis "trading this side of this bracket on EVERY warmed bar
    of the partition". The t-test is performed against this drift, not
    against zero. This prevents the partition's overall trend (e.g.,
    XAUUSD's $2k -> $5.5k rally) from making every long-biased
    predicate look "significant".

Compute, for every predicate × bracket × side (LONG / SHORT):

    n              sample count
    hit_rate       fraction of shots with PnL > 0
    expectancy     mean PnL per shot in points
    drift_baseline partition mean PnL for this (side, bracket)
    excess_expectancy = expectancy - drift_baseline
    pf             profit factor: sum(wins) / |sum(losses)|
    sharpe         expectancy / stdev(PnL) (per-shot sharpe; raw, not excess)
    t_stat         Welch t-stat of (mean - drift) vs zero
    p_raw          two-sided p-value from t_stat (gaussian approximation,
                   valid here because n >= 1000)

LONG side uses fwd_bracket_pts_<i> directly. SHORT side negates them.
"""
from __future__ import annotations

from dataclasses import dataclass, asdict
from typing import Iterable, Optional

import numpy as np
import pandas as pd

from .regime import PredicateSpec, predicate_mask


N_BRACKETS = 6
SIDES = ('LONG', 'SHORT')
BRACKET_COLS = [f'fwd_bracket_pts_{i}' for i in range(N_BRACKETS)]


@dataclass
class ProspectRow:
    pid:               int
    feature:           str
    op:                str
    threshold:         float
    regime_session:    str
    regime_vol:        str
    regime_trend:      str
    side:              str
    bracket_id:        int
    n:                 int
    hit_rate:          float
    expectancy:        float
    drift_baseline:    float
    excess_expectancy: float
    pf:                float
    sharpe:            float
    t_stat:            float
    p_raw:             float

    def to_dict(self) -> dict:
        return asdict(self)


def compute_drift_baseline(df: pd.DataFrame) -> dict[tuple[str, int], float]:
    """
    Compute the per-(side, bracket) mean PnL on warmed + fwd_complete bars
    of the partition. This is the null hypothesis baseline: "what would I
    earn if I just traded this side of this bracket on every bar?"

    Returns dict keyed by (side, bracket_id).
    """
    mask = (df['warmed_up'].to_numpy() == 1) & (df['fwd_complete'].to_numpy() == 1)
    if not mask.any():
        return {(side, b): 0.0 for side in SIDES for b in range(N_BRACKETS)}

    out: dict[tuple[str, int], float] = {}
    for b in range(N_BRACKETS):
        col = df[BRACKET_COLS[b]].to_numpy(dtype=np.float64)
        sub = col[mask]
        sub = sub[np.isfinite(sub)]
        if sub.size == 0:
            mean = 0.0
        else:
            mean = float(np.mean(sub))
        out[('LONG',  b)] = mean
        out[('SHORT', b)] = -mean   # short side is negation of long
    return out


def _summarise(
    pnl: np.ndarray,
    drift: float,
) -> tuple[float, float, float, float, float, float, float]:
    """
    Compute (hit_rate, expectancy, excess, pf, sharpe, t_stat, p_raw) over a
    1-d pnl array, testing the mean against `drift` (not against zero).

    Caller guarantees pnl has at least min_n samples.
    """
    n = pnl.size
    mean = float(np.mean(pnl))
    std  = float(np.std(pnl, ddof=1))
    excess = mean - drift
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
        # Test (mean - drift) against zero
        t_stat = excess / (std / np.sqrt(n))
        from math import erfc, sqrt
        p_raw = float(erfc(abs(t_stat) / sqrt(2.0)))

    return hit, mean, excess, pf, sharpe, t_stat, p_raw


def score_predicates(
    df: pd.DataFrame,
    catalogue: Iterable[PredicateSpec],
    min_n: int = 1000,
    drift_baseline: Optional[dict[tuple[str, int], float]] = None,
    progress_every: int = 5000,
) -> pd.DataFrame:
    """
    Score every (predicate × side × bracket) combination on `df` against the
    drift baseline and return a DataFrame.

    If `drift_baseline` is None, it's computed from `df` itself.
    """
    if drift_baseline is None:
        drift_baseline = compute_drift_baseline(df)

    bracket_pnl = np.column_stack([
        df[col].to_numpy(dtype=np.float64) for col in BRACKET_COLS
    ])

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

        sub_pnl = bracket_pnl[m]
        finite = np.isfinite(sub_pnl).all(axis=1)
        if int(finite.sum()) < min_n:
            continue
        sub_pnl = sub_pnl[finite]
        n_eff = sub_pnl.shape[0]

        for b in range(N_BRACKETS):
            long_pnl  =  sub_pnl[:, b]
            short_pnl = -sub_pnl[:, b]
            drift_long  = float(drift_baseline.get(('LONG',  b), 0.0))
            drift_short = float(drift_baseline.get(('SHORT', b), 0.0))

            hit, mean, exc, pf, sh, t, p = _summarise(long_pnl, drift_long)
            out_rows.append(dict(
                pid=spec.pid, feature=spec.feature, op=spec.op,
                threshold=spec.threshold,
                regime_session=spec.regime_session,
                regime_vol=spec.regime_vol,
                regime_trend=spec.regime_trend,
                side='LONG', bracket_id=b, n=n_eff,
                hit_rate=hit, expectancy=mean,
                drift_baseline=drift_long,
                excess_expectancy=exc,
                pf=pf, sharpe=sh,
                t_stat=t, p_raw=p,
            ))
            n_kept += 1

            hit, mean, exc, pf, sh, t, p = _summarise(short_pnl, drift_short)
            out_rows.append(dict(
                pid=spec.pid, feature=spec.feature, op=spec.op,
                threshold=spec.threshold,
                regime_session=spec.regime_session,
                regime_vol=spec.regime_vol,
                regime_trend=spec.regime_trend,
                side='SHORT', bracket_id=b, n=n_eff,
                hit_rate=hit, expectancy=mean,
                drift_baseline=drift_short,
                excess_expectancy=exc,
                pf=pf, sharpe=sh,
                t_stat=t, p_raw=p,
            ))
            n_kept += 1

    print(f"  finished: scored {n_seen} predicates, produced {n_kept} prospect rows")
    if not out_rows:
        return pd.DataFrame(columns=[
            'pid','feature','op','threshold',
            'regime_session','regime_vol','regime_trend',
            'side','bracket_id','n',
            'hit_rate','expectancy','drift_baseline','excess_expectancy',
            'pf','sharpe','t_stat','p_raw',
        ])
    return pd.DataFrame(out_rows)


def reapply_to_partition(
    df_partition: pd.DataFrame,
    train_prospects: pd.DataFrame,
    catalogue_by_pid: dict[int, PredicateSpec],
    drift_baseline: Optional[dict[tuple[str, int], float]] = None,
) -> pd.DataFrame:
    """
    Re-evaluate a TRAIN-survivor set on another partition (VAL or OOS).

    Drift baseline defaults to the partition's own drift.
    """
    if 'pid' not in train_prospects.columns:
        raise ValueError("train_prospects must contain 'pid' column")
    if drift_baseline is None:
        drift_baseline = compute_drift_baseline(df_partition)

    bracket_pnl = np.column_stack([
        df_partition[col].to_numpy(dtype=np.float64) for col in BRACKET_COLS
    ])

    out_rows: list[dict] = []
    for pid, sub in train_prospects.groupby('pid', sort=False):
        spec = catalogue_by_pid.get(int(pid))
        if spec is None:
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

        for _, r in sub.iterrows():
            b = int(r['bracket_id'])
            side = str(r['side'])
            pnl = sub_pnl[:, b]
            if side == 'SHORT':
                pnl = -pnl
            drift = float(drift_baseline.get((side, b), 0.0))
            hit, mean, exc, pf, sh, t, p = _summarise(pnl, drift)
            out_rows.append(dict(
                pid=int(pid), feature=spec.feature, op=spec.op,
                threshold=spec.threshold,
                regime_session=spec.regime_session,
                regime_vol=spec.regime_vol,
                regime_trend=spec.regime_trend,
                side=side, bracket_id=b, n=n_eff,
                hit_rate=hit, expectancy=mean,
                drift_baseline=drift,
                excess_expectancy=exc,
                pf=pf, sharpe=sh,
                t_stat=t, p_raw=p,
            ))

    return pd.DataFrame(out_rows)


def _empty_row(r: pd.Series) -> dict:
    """Synthesise a zero-sample prospect row."""
    return dict(
        pid=int(r['pid']), feature=str(r['feature']), op=str(r['op']),
        threshold=float(r['threshold']),
        regime_session=str(r['regime_session']),
        regime_vol=str(r['regime_vol']),
        regime_trend=str(r['regime_trend']),
        side=str(r['side']), bracket_id=int(r['bracket_id']),
        n=0, hit_rate=0.0, expectancy=0.0,
        drift_baseline=0.0, excess_expectancy=0.0,
        pf=0.0, sharpe=0.0, t_stat=0.0, p_raw=1.0,
    )
