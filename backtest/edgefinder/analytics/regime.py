"""
edgefinder/analytics/regime.py
==============================

Two responsibilities:

1. REGIME TAGGING. Given a panel DataFrame, derive coarse buckets (session,
   volatility tercile, trend state) that downstream code conditions on.
   These are added as new columns; the panel itself is unchanged on disk.

2. PREDICATE GENERATION. Produce a deterministic, leakage-free catalogue of
   boolean predicates over the panel for the scanner to evaluate. Quantile
   cut points for numeric features are FIT ON THE TRAIN PARTITION ONLY and
   then applied to VAL/OOS as frozen constants. This is the single most
   common source of subtle look-ahead in this kind of work; doing it once
   centrally prevents it everywhere.

The predicate catalogue is purely metadata (no boolean masks materialised
here) so it is cheap to construct and cache. The scanner in prospect.py
turns each catalogue entry into a mask on demand.

A "predicate" here is a tuple of:
    (predicate_id, feature, op, threshold, regime_session, regime_vol,
     regime_trend, expected_n_hint)

where:
    op ∈ {'le', 'gt', 'eq'}
    threshold is a float for numeric features, 1 for boolean features.
    regime_* are coarse bucket labels or 'ALL'.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, asdict, field
from pathlib import Path
from typing import Iterable, Optional

import numpy as np
import pandas as pd


# -----------------------------------------------------------------------------
# Column classification
# -----------------------------------------------------------------------------
# Columns that are NOT to be turned into predicates: padding, ids, raw
# timestamps, label columns (forward returns / brackets), and quality flags.
EXCLUDED_PREFIXES = (
    '_pad', 'fwd_ret_', 'fwd_bracket_', 'first_touch_',
)
EXCLUDED_EXACT = {
    'ts_close_ms', 'ts_close',
    'utc_hour', 'utc_minute_of_day', 'dow', 'dom', 'yday',
    'session', 'session_name',
    'open', 'high', 'low', 'close',          # raw price levels: not predictive on their own
    'warmed_up', 'fwd_complete',
    'regime_vol', 'regime_trend', 'regime_session',  # the regime cols themselves
}

# Boolean / transition features: predicate is just (feature == 1).
# Detected automatically as columns whose dtype is uint8 and whose values are
# in {0, 1}. Listed explicitly here as a sanity-check list.
KNOWN_BOOLEAN_FEATURES = [
    'above_pdh', 'below_pdl',
    'asian_built', 'above_asian_hi', 'below_asian_lo',
    'nr4', 'nr7', 'inside_bar', 'outside_bar',
    'cross_above_pdh', 'cross_below_pdl',
    'cross_above_asian_hi', 'cross_below_asian_lo',
    'cross_above_vwap', 'cross_below_vwap',
    'ema_9_50_bull_cross', 'ema_9_50_bear_cross',
    'enter_bb_upper', 'enter_bb_lower',
]

# Default quantile grid: deciles. 9 cut points → 18 predicates per feature
# (≤ at each cut, > at each cut). Override via fit_quantile_cuts(qs=...).
DEFAULT_QUANTILES = (0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90)

SESSION_BUCKETS = ['ASIAN', 'LONDON', 'OVERLAP', 'NY_AM', 'NY_PM', 'ALL']
VOL_BUCKETS     = ['VOL_LO', 'VOL_MID', 'VOL_HI', 'ALL']
TREND_BUCKETS   = ['TREND_DOWN', 'TREND_FLAT', 'TREND_UP', 'ALL']


# -----------------------------------------------------------------------------
# Regime tagging
# -----------------------------------------------------------------------------
@dataclass
class RegimeCuts:
    """Frozen cut-points fit on TRAIN, applied to all partitions."""
    vol_lo: float
    vol_hi: float
    trend_down: float   # ema_9_minus_50 lower threshold
    trend_up:   float   # ema_9_minus_50 upper threshold

    def to_dict(self) -> dict:
        return asdict(self)

    @staticmethod
    def from_dict(d: dict) -> 'RegimeCuts':
        return RegimeCuts(**d)


def fit_regime_cuts(train_df: pd.DataFrame) -> RegimeCuts:
    """
    Fit volatility and trend regime cuts on TRAIN data only.

    Vol regime: terciles of vol_60bar_stddev (warmed bars only).
    Trend regime: terciles of ema_9_minus_50 (warmed bars only).
    """
    warm = train_df[train_df['warmed_up'] == 1]
    vol = warm['vol_60bar_stddev'].dropna()
    trend = warm['ema_9_minus_50'].dropna()

    if len(vol) < 1000 or len(trend) < 1000:
        raise ValueError(
            f"too few warmed bars to fit regime cuts: vol={len(vol)} trend={len(trend)}"
        )

    vol_lo, vol_hi = np.quantile(vol.to_numpy(), [1.0 / 3.0, 2.0 / 3.0])
    tr_lo, tr_hi  = np.quantile(trend.to_numpy(), [1.0 / 3.0, 2.0 / 3.0])

    return RegimeCuts(
        vol_lo=float(vol_lo),
        vol_hi=float(vol_hi),
        trend_down=float(tr_lo),
        trend_up=float(tr_hi),
    )


def apply_regimes(df: pd.DataFrame, cuts: RegimeCuts) -> pd.DataFrame:
    """
    Add regime_session / regime_vol / regime_trend columns.

    Operates on a COPY; original df is not mutated. Unwarmed bars get
    regime_vol='UNK' and regime_trend='UNK' so they can be filtered out
    downstream without ambiguity.
    """
    out = df.copy()

    # Session: already categorical from load.py, just rename for clarity.
    out['regime_session'] = out['session_name'].astype(str)

    # Volatility tercile.
    vol = out['vol_60bar_stddev']
    rv = np.where(
        vol.isna(), 'UNK',
        np.where(vol <= cuts.vol_lo, 'VOL_LO',
                 np.where(vol <= cuts.vol_hi, 'VOL_MID', 'VOL_HI'))
    )
    out['regime_vol'] = rv

    # Trend tercile (ema_9_minus_50 sign / magnitude).
    tr = out['ema_9_minus_50']
    rt = np.where(
        tr.isna(), 'UNK',
        np.where(tr <= cuts.trend_down, 'TREND_DOWN',
                 np.where(tr <= cuts.trend_up, 'TREND_FLAT', 'TREND_UP'))
    )
    out['regime_trend'] = rt

    return out


# -----------------------------------------------------------------------------
# Feature classification
# -----------------------------------------------------------------------------
def _is_boolean_feature(s: pd.Series) -> bool:
    if s.dtype == np.uint8:
        u = s.dropna().unique()
        if len(u) <= 2 and set(int(x) for x in u).issubset({0, 1}):
            return True
    return False


def classify_features(df: pd.DataFrame) -> tuple[list[str], list[str]]:
    """
    Inspect the panel and split feature columns into (numeric, boolean).

    Returns lists of column names. Excluded columns (ids, padding, labels)
    are dropped entirely.
    """
    numeric: list[str] = []
    boolean: list[str] = []
    for col in df.columns:
        if col in EXCLUDED_EXACT:
            continue
        if any(col.startswith(p) for p in EXCLUDED_PREFIXES):
            continue
        s = df[col]
        if not pd.api.types.is_numeric_dtype(s):
            continue
        if _is_boolean_feature(s):
            boolean.append(col)
        else:
            # Skip degenerate columns (all-NaN or near-constant).
            if s.dropna().nunique() < 5:
                continue
            numeric.append(col)
    return numeric, boolean


# -----------------------------------------------------------------------------
# Quantile cuts (fit on TRAIN, frozen)
# -----------------------------------------------------------------------------
def fit_quantile_cuts(
    train_df: pd.DataFrame,
    numeric_features: Iterable[str],
    qs: tuple = DEFAULT_QUANTILES,
) -> dict[str, list[float]]:
    """
    For each numeric feature, compute the quantile cut points on warmed-up
    TRAIN rows. Returns dict feature -> list of cut values (one per q in qs).

    Skip a feature if it has fewer than 1000 warmed non-NaN values.
    """
    warm = train_df[train_df['warmed_up'] == 1]
    out: dict[str, list[float]] = {}
    for feat in numeric_features:
        if feat not in warm.columns:
            continue
        s = warm[feat].dropna().to_numpy()
        if s.size < 1000:
            continue
        cuts = np.quantile(s, qs).tolist()
        # Keep only strictly-increasing cuts; ties collapse predicates
        # (a duplicate cut produces an empty 'between' bucket).
        deduped: list[float] = []
        for c in cuts:
            if not deduped or c > deduped[-1]:
                deduped.append(float(c))
        if len(deduped) >= 2:
            out[feat] = deduped
    return out


# -----------------------------------------------------------------------------
# Predicate catalogue
# -----------------------------------------------------------------------------
@dataclass
class PredicateSpec:
    pid: int                       # stable id
    feature: str
    op: str                        # 'le' | 'gt' | 'eq'
    threshold: float
    regime_session: str            # SESSION_BUCKETS
    regime_vol: str                # VOL_BUCKETS
    regime_trend: str              # TREND_BUCKETS

    def describe(self) -> str:
        if self.op == 'eq':
            base = f"{self.feature}=={int(self.threshold)}"
        else:
            sym = '<=' if self.op == 'le' else '>'
            base = f"{self.feature}{sym}{self.threshold:.6g}"
        regime = []
        if self.regime_session != 'ALL': regime.append(f"sess={self.regime_session}")
        if self.regime_vol     != 'ALL': regime.append(f"vol={self.regime_vol}")
        if self.regime_trend   != 'ALL': regime.append(f"trd={self.regime_trend}")
        if regime:
            base = f"{base} | " + ",".join(regime)
        return base

    def to_dict(self) -> dict:
        return asdict(self)


def build_predicate_catalogue(
    numeric_features: Iterable[str],
    boolean_features: Iterable[str],
    quantile_cuts: dict[str, list[float]],
    sessions: Iterable[str] = SESSION_BUCKETS,
    vols:     Iterable[str] = VOL_BUCKETS,
    trends:   Iterable[str] = TREND_BUCKETS,
) -> list[PredicateSpec]:
    """
    Cartesian product:
        (feature predicate) x (session bucket) x (vol bucket) x (trend bucket)

    Numeric features contribute 2*len(cuts) predicates (le/gt at each cut).
    Boolean features contribute 1 predicate ('==1').

    The catalogue is deterministic for a given (cuts, feature lists,
    bucket lists). pid is assigned in iteration order.
    """
    sessions = list(sessions)
    vols = list(vols)
    trends = list(trends)
    out: list[PredicateSpec] = []
    pid = 0

    # Numeric predicates
    for feat in numeric_features:
        cuts = quantile_cuts.get(feat)
        if not cuts:
            continue
        for thr in cuts:
            for op in ('le', 'gt'):
                for s in sessions:
                    for v in vols:
                        for t in trends:
                            out.append(PredicateSpec(
                                pid=pid, feature=feat, op=op, threshold=float(thr),
                                regime_session=s, regime_vol=v, regime_trend=t,
                            ))
                            pid += 1

    # Boolean predicates
    for feat in boolean_features:
        for s in sessions:
            for v in vols:
                for t in trends:
                    out.append(PredicateSpec(
                        pid=pid, feature=feat, op='eq', threshold=1.0,
                        regime_session=s, regime_vol=v, regime_trend=t,
                    ))
                    pid += 1

    return out


# -----------------------------------------------------------------------------
# Persistence
# -----------------------------------------------------------------------------
@dataclass
class CatalogueArtifact:
    """Everything needed to re-derive the same catalogue on another partition."""
    regime_cuts: RegimeCuts
    quantile_cuts: dict[str, list[float]]
    numeric_features: list[str]
    boolean_features: list[str]
    n_predicates: int
    train_to_iso: str
    val_to_iso: str
    oos_to_iso: str

    def save(self, path: str | Path) -> None:
        Path(path).write_text(json.dumps({
            'regime_cuts': self.regime_cuts.to_dict(),
            'quantile_cuts': self.quantile_cuts,
            'numeric_features': self.numeric_features,
            'boolean_features': self.boolean_features,
            'n_predicates': self.n_predicates,
            'train_to_iso': self.train_to_iso,
            'val_to_iso': self.val_to_iso,
            'oos_to_iso': self.oos_to_iso,
        }, indent=2))

    @staticmethod
    def load(path: str | Path) -> 'CatalogueArtifact':
        d = json.loads(Path(path).read_text())
        return CatalogueArtifact(
            regime_cuts=RegimeCuts.from_dict(d['regime_cuts']),
            quantile_cuts={k: list(v) for k, v in d['quantile_cuts'].items()},
            numeric_features=list(d['numeric_features']),
            boolean_features=list(d['boolean_features']),
            n_predicates=int(d['n_predicates']),
            train_to_iso=d['train_to_iso'],
            val_to_iso=d['val_to_iso'],
            oos_to_iso=d['oos_to_iso'],
        )


# -----------------------------------------------------------------------------
# Mask materialisation (used by prospect.py)
# -----------------------------------------------------------------------------
def predicate_mask(df: pd.DataFrame, spec: PredicateSpec) -> np.ndarray:
    """
    Compute the boolean mask for a single predicate over df. df must already
    have regime_session/regime_vol/regime_trend columns (apply_regimes).

    NaNs in the feature column produce False (NaN is never a predicate hit).
    """
    feat = df[spec.feature]
    if spec.op == 'le':
        m = (feat <= spec.threshold).to_numpy(copy=True)
    elif spec.op == 'gt':
        m = (feat >  spec.threshold).to_numpy(copy=True)
    elif spec.op == 'eq':
        m = (feat == spec.threshold).to_numpy(copy=True)
    else:
        raise ValueError(f"unknown op: {spec.op}")
    # NaNs in feat compare to False already, but make it explicit.
    # to_numpy(copy=True) above guarantees the mask is writable for &= below.
    m &= ~feat.isna().to_numpy()

    if spec.regime_session != 'ALL':
        m &= (df['regime_session'].to_numpy() == spec.regime_session)
    if spec.regime_vol != 'ALL':
        m &= (df['regime_vol'].to_numpy() == spec.regime_vol)
    if spec.regime_trend != 'ALL':
        m &= (df['regime_trend'].to_numpy() == spec.regime_trend)

    # Always require warmed_up and fwd_complete; otherwise we'd score on
    # half-formed features or unfilled labels.
    m &= (df['warmed_up'].to_numpy() == 1)
    m &= (df['fwd_complete'].to_numpy() == 1)
    return m
