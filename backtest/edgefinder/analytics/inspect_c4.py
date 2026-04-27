"""
edgefinder/analytics/inspect_c4.py
==================================

Session C4 — final pre-OOS hygiene pass on VAL survivors.

Pipeline:

    val_survivors  ─┐
                    ├─►  (1) flag absolute-price features
    catalogue ──────┘    (2) recompute per-row diagnostics on TRAIN bars:
                              spread_clean_share  (fraction of bars with
                                                   spread_max_pts >= 0.498)
                              time_cluster_max    (max fraction of bars in
                                                   any single UTC calendar day)
                         (3) dedup by
                              (regime_session, regime_vol, regime_trend,
                               side, bracket_id, threshold-bucket-on-feature)
                         (4) emit two artefacts:
                              c4_full_flagged.{parquet,pkl}   — all 4,024 rows
                                                                annotated
                              c4_clean.{parquet,pkl}          — clean subset
                                                                ready for OOS

The clean subset filter:
    is_absolute_price == False
    spread_clean_share  >= 0.10        (>= 10% of bars have non-anomalous spread)
    time_cluster_max    <= 0.30        (<= 30% of bars in any single UTC day)
    deduped within (regime, side, bracket, threshold-bucket-on-feature)
    keep best by VAL sharpe within each dedup group

Output target: 30 - 80 distinct hypotheses ready for OOS.

OOS sentinel is NEVER touched by this script. It only reads TRAIN bars (via
the panel) and the val_survivors artefact. It writes to a NEW directory
(inspect_c4) so existing artefacts in work/ are untouched.

Resume context (Session B2 → C3 → C4):
    HEAD                 1a653fef
    VAL survivors        4,024
    OOS sentinel         UNBURNED
    Bracket-5 dominance  3,112 / 4,024 of survivors
    Known issue          absolute-price features leak through as
                         quantile-binned predicates ("hardcoded ranges")
"""
from __future__ import annotations

import argparse
import json
import pickle
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

import numpy as np
import pandas as pd

# Re-use the existing modules; do NOT redefine schema or partitioning here.
from .load import load_panel
from .regime import (
    apply_regimes, CatalogueArtifact, PredicateSpec, predicate_mask,
)
from .walkforward import PartitionBounds, train_partition


# -----------------------------------------------------------------------------
# Constants
# -----------------------------------------------------------------------------

# Absolute-price features: raw quoted price levels (not relative distances or
# z-scores). Quantile-binned predicates on these are equivalent to
# "price was in some hardcoded historical range", which is curve-fit to the
# training period and does not generalise. Relative cousins (e.g.
# session_open_dist_pts, vwap_dist_pts, vwap_z, dist_to_pdh_pts,
# range_20bar_position, bb_position) are NOT in this list — they encode
# distance, not absolute level.
#
# This list is the C3 finding made explicit. Match the existing
# regime.EXCLUDED_EXACT vocabulary; keep this list synchronised by name.
ABSOLUTE_PRICE_FEATURES = frozenset({
    'ema_9',
    'ema_21',
    'ema_50',
    'ema_200',
    'bb_upper',
    'bb_lower',
    'range_20bar_hi',
    'range_20bar_lo',
    'session_open_price',
    'session_high',
    'session_low',
    'vwap_session',
    'pdh',
    'pdl',
    'asian_hi',
    'asian_lo',
})

# Spread anomaly threshold from C2 audit. Bars with spread_max_pts <= 0.498
# were the artefactual rows clustered in Jan 22-23, 2024 rally; clean bars
# have spread_max_pts > 0.498.
SPREAD_ANOMALY_THRESHOLD = 0.498

# Defaults — overridable via CLI.
DEFAULT_PANEL                = 'backtest/edgefinder/output/bars_xauusd_full.bin'
DEFAULT_WORKDIR              = 'backtest/edgefinder/output/work'
DEFAULT_OUTDIR               = 'backtest/edgefinder/output/inspect_c4'

DEFAULT_SPREAD_CLEAN_SHARE_MIN = 0.10
DEFAULT_TIME_CLUSTER_MAX       = 0.30
DEFAULT_TIME_BUCKET            = 'day'   # 'day' | 'hour' | 'week'

# Threshold-bucketing for dedup. Two predicates with thresholds 1230.4 and
# 1230.7 on the same feature/regime/side/bracket are not distinct hypotheses.
# Bucket the threshold quantile-rank to N decimal places.
DEFAULT_THRESHOLD_BUCKET_DECIMALS = 1   # i.e. ranks rounded to 0.1

# Catalogue / artefact filenames (mirror cli.py constants).
CATALOGUE_JSON   = 'catalogue.json'
CATALOGUE_PICKLE = 'catalogue.pkl'
VAL_SURVIVORS    = 'val_survivors'   # extension auto-detected


# -----------------------------------------------------------------------------
# Persistence helpers (mirror cli.py: parquet preferred, pickle fallback)
# -----------------------------------------------------------------------------
def _ts() -> str:
    return time.strftime('%H:%M:%S')


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
            print(f"  [persist] parquet read failed ({e}); trying pickle", flush=True)
    if pickle_path.exists():
        return pd.read_pickle(pickle_path)
    raise FileNotFoundError(
        f"neither {parquet_path} nor {pickle_path} exists "
        f"(did you run `gate-val` to produce val_survivors?)"
    )


# -----------------------------------------------------------------------------
# Step 1 — flag absolute-price features
# -----------------------------------------------------------------------------
def flag_absolute_price(survivors: pd.DataFrame) -> pd.DataFrame:
    """
    Add boolean column `is_absolute_price`. Pure annotation; no rows dropped.
    """
    out = survivors.copy()
    out['is_absolute_price'] = out['feature'].isin(ABSOLUTE_PRICE_FEATURES)
    return out


# -----------------------------------------------------------------------------
# Step 2 — per-row TRAIN diagnostics (spread cleanliness + time-cluster)
# -----------------------------------------------------------------------------
def _time_bucket_key(idx: pd.DatetimeIndex, bucket: str) -> np.ndarray:
    """
    Return a per-row integer/string key labelling each timestamp's calendar
    bucket. Used to compute time_cluster_max = max share-of-bars-in-any-bucket.
    """
    if bucket == 'day':
        # YYYY-MM-DD
        return idx.strftime('%Y-%m-%d').to_numpy()
    if bucket == 'hour':
        return idx.strftime('%Y-%m-%d-%H').to_numpy()
    if bucket == 'week':
        # ISO week label
        iso = idx.isocalendar()
        return (iso['year'].astype(str) + '-W' + iso['week'].astype(str)).to_numpy()
    raise ValueError(f"unknown time bucket: {bucket!r}")


def compute_train_diagnostics(
    survivors: pd.DataFrame,
    df_train_r: pd.DataFrame,
    catalogue_by_pid: dict[int, PredicateSpec],
    time_bucket: str = DEFAULT_TIME_BUCKET,
) -> pd.DataFrame:
    """
    For every (pid, side, bracket_id) row in `survivors`, materialise the
    predicate mask on TRAIN, and compute:

        n_train_bars       — number of TRAIN bars where predicate hits
        spread_clean_share — fraction of those bars where
                             spread_max_pts > SPREAD_ANOMALY_THRESHOLD
        time_cluster_max   — max share of bars falling in any single
                             calendar bucket (UTC day by default)
        time_cluster_bucket— which bucket the max came from (for inspection)

    Same predicate (same pid) yields identical mask regardless of side or
    bracket_id, so we cache by pid.
    """
    # Pre-extract panel arrays we'll touch repeatedly.
    spread = df_train_r['spread_max_pts'].to_numpy(dtype=np.float64)
    time_keys = _time_bucket_key(df_train_r.index, time_bucket)

    n_total = len(survivors)
    cache: dict[int, dict[str, float]] = {}

    diag_rows: list[dict] = []
    for i, (_, row) in enumerate(survivors.iterrows()):
        pid = int(row['pid'])
        if pid not in cache:
            spec = catalogue_by_pid.get(pid)
            if spec is None:
                cache[pid] = {
                    'n_train_bars': 0,
                    'spread_clean_share': float('nan'),
                    'time_cluster_max': float('nan'),
                    'time_cluster_bucket': '',
                }
            else:
                m = predicate_mask(df_train_r, spec)
                n_bars = int(m.sum())
                if n_bars == 0:
                    cache[pid] = {
                        'n_train_bars': 0,
                        'spread_clean_share': float('nan'),
                        'time_cluster_max': float('nan'),
                        'time_cluster_bucket': '',
                    }
                else:
                    sub_spread = spread[m]
                    finite = np.isfinite(sub_spread)
                    if int(finite.sum()) == 0:
                        clean_share = float('nan')
                    else:
                        clean_share = float(
                            np.mean(sub_spread[finite] > SPREAD_ANOMALY_THRESHOLD)
                        )

                    sub_keys = time_keys[m]
                    # value_counts via numpy: fastest path that doesn't
                    # require building a Series for every pid.
                    uniq, counts = np.unique(sub_keys, return_counts=True)
                    if counts.size == 0:
                        tc_max = float('nan')
                        tc_bucket = ''
                    else:
                        argmax = int(np.argmax(counts))
                        tc_max = float(counts[argmax]) / float(n_bars)
                        tc_bucket = str(uniq[argmax])

                    cache[pid] = {
                        'n_train_bars': n_bars,
                        'spread_clean_share': clean_share,
                        'time_cluster_max': tc_max,
                        'time_cluster_bucket': tc_bucket,
                    }

        diag_rows.append({**cache[pid]})

        if (i + 1) % 500 == 0 or (i + 1) == n_total:
            print(
                f"  [{_ts()}] diagnostics: {i+1}/{n_total} "
                f"(unique pids cached: {len(cache)})",
                flush=True,
            )

    diag = pd.DataFrame(diag_rows, index=survivors.index)
    out = pd.concat([survivors, diag], axis=1)
    return out


# -----------------------------------------------------------------------------
# Step 3 — dedup by (regime, side, bracket, threshold-bucket)
# -----------------------------------------------------------------------------
def _threshold_rank_within_feature(
    survivors: pd.DataFrame,
    decimals: int,
) -> np.ndarray:
    """
    Within each `feature`, compute the empirical-CDF rank of `threshold` and
    round to `decimals` places. This produces a stable bucket key that is
    invariant to the exact float threshold value but distinguishes broadly
    different threshold regions (low / mid / high quantile).

    Boolean predicates have op='eq' and threshold=1.0; their rank is always
    a single value within the feature group and they collapse to a single
    bucket per feature.

    Returns a 1-d numpy array of length len(survivors), in the row order of
    `survivors` (positional, NOT keyed by survivors.index). Caller is
    responsible for assigning back to a column with the same row order.
    """
    n = len(survivors)
    rank_buckets = np.zeros(n, dtype=np.float64)

    # Build a positional index 0..n-1 alongside the data so we can write
    # into rank_buckets without touching the (potentially non-sequential)
    # frame index.
    feat_arr = survivors['feature'].to_numpy()
    thr_arr  = survivors['threshold'].to_numpy(dtype=np.float64)
    pos      = np.arange(n, dtype=np.int64)

    # Group positions by feature.
    df_pos = pd.DataFrame({'feature': feat_arr, 'thr': thr_arr, 'pos': pos})
    for _, sub in df_pos.groupby('feature', sort=False):
        sub_thr = sub['thr'].to_numpy(dtype=np.float64)
        sub_pos = sub['pos'].to_numpy(dtype=np.int64)
        m = len(sub_thr)
        if m == 1:
            rank_buckets[sub_pos[0]] = 0.5
            continue

        order = np.argsort(sub_thr, kind='mergesort')
        sorted_thr = sub_thr[order]
        ranks_sorted = np.empty(m, dtype=np.float64)
        i = 0
        while i < m:
            j = i
            while j + 1 < m and sorted_thr[j + 1] == sorted_thr[i]:
                j += 1
            avg = (i + j) / 2.0
            ranks_sorted[i:j + 1] = avg
            i = j + 1
        ranks_sorted = ranks_sorted / max(m - 1, 1)

        # Scatter ranks back to the positions in original order.
        ranks_in_order = np.empty(m, dtype=np.float64)
        ranks_in_order[order] = ranks_sorted
        for k in range(m):
            rank_buckets[sub_pos[k]] = ranks_in_order[k]

    factor = 10 ** decimals
    return np.round(rank_buckets * factor) / factor


def dedup_by_hypothesis_key(
    survivors: pd.DataFrame,
    threshold_bucket_decimals: int = DEFAULT_THRESHOLD_BUCKET_DECIMALS,
    rank_metric: str = 'sharpe',
) -> pd.DataFrame:
    """
    Group rows by:
        (feature, op, regime_session, regime_vol, regime_trend,
         side, bracket_id, threshold_bucket)
    and keep the row with the highest |VAL rank_metric| in each group.

    `rank_metric` defaults to 'sharpe' (the per-shot VAL sharpe). Could be
    swapped to 'excess_expectancy' if you prefer to keep the best in points
    rather than risk-adjusted.
    """
    if rank_metric not in survivors.columns:
        raise ValueError(
            f"rank_metric={rank_metric!r} not in survivor columns: "
            f"{list(survivors.columns)}"
        )

    df = survivors.reset_index(drop=True).copy()
    df['threshold_bucket'] = _threshold_rank_within_feature(
        df, threshold_bucket_decimals,
    )

    df['_rank_score'] = df[rank_metric].abs()
    grp_cols = [
        'feature', 'op',
        'regime_session', 'regime_vol', 'regime_trend',
        'side', 'bracket_id',
        'threshold_bucket',
    ]
    df = df.sort_values('_rank_score', ascending=False, kind='mergesort')
    deduped = df.drop_duplicates(subset=grp_cols, keep='first')
    deduped = deduped.drop(columns=['_rank_score']).reset_index(drop=True)
    return deduped


# -----------------------------------------------------------------------------
# Step 4 — emit
# -----------------------------------------------------------------------------
def select_clean(
    flagged_dedup: pd.DataFrame,
    spread_clean_share_min: float,
    time_cluster_max: float,
) -> pd.DataFrame:
    """
    Apply the clean filter:

        is_absolute_price  == False
        spread_clean_share >= spread_clean_share_min
        time_cluster_max   <= time_cluster_max

    Rows with NaN in spread_clean_share or time_cluster_max are dropped (a
    NaN means the predicate did not produce any TRAIN bars, which means it
    cannot be assessed and therefore should not be promoted to OOS).
    """
    df = flagged_dedup.copy()
    cond = (
        (~df['is_absolute_price']) &
        (df['spread_clean_share'].notna()) &
        (df['time_cluster_max'].notna()) &
        (df['spread_clean_share'] >= spread_clean_share_min) &
        (df['time_cluster_max']   <= time_cluster_max)
    )
    return df[cond].copy()


# -----------------------------------------------------------------------------
# Driver
# -----------------------------------------------------------------------------
@dataclass
class InspectC4Args:
    panel: str
    workdir: str
    outdir: str
    spread_clean_share_min: float
    time_cluster_max: float
    time_bucket: str
    threshold_bucket_decimals: int
    rank_metric: str


def run(args: InspectC4Args) -> int:
    workdir = Path(args.workdir)
    outdir  = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    print(f"[{_ts()}] inspect_c4 starting")
    print(f"[{_ts()}]   panel    = {args.panel}")
    print(f"[{_ts()}]   workdir  = {workdir}")
    print(f"[{_ts()}]   outdir   = {outdir}")
    print(f"[{_ts()}]   filters  = spread_clean_share>={args.spread_clean_share_min}, "
          f"time_cluster_max<={args.time_cluster_max} ({args.time_bucket})")
    print(f"[{_ts()}]   dedup    = threshold_bucket_decimals={args.threshold_bucket_decimals}, "
          f"rank_metric={args.rank_metric}")

    # --- load val survivors ---
    survivors = _load_df(workdir / VAL_SURVIVORS)
    n0 = len(survivors)
    print(f"[{_ts()}] loaded VAL survivors: {n0} rows")
    if n0 == 0:
        print(f"[{_ts()}] no survivors; nothing to do.")
        return 0
    required = {
        'pid', 'feature', 'op', 'threshold',
        'regime_session', 'regime_vol', 'regime_trend',
        'side', 'bracket_id', 'n', 'expectancy', 'pf', 'sharpe',
        'expectancy_train', 'gate_passed',
    }
    missing = required - set(survivors.columns)
    if missing:
        print(
            f"ERROR: VAL survivors missing required columns: {missing}",
            file=sys.stderr,
        )
        return 2

    # --- load catalogue ---
    art = CatalogueArtifact.load(workdir / CATALOGUE_JSON)
    with open(workdir / CATALOGUE_PICKLE, 'rb') as f:
        catalogue: list[PredicateSpec] = pickle.load(f)
    catalogue_by_pid = {s.pid: s for s in catalogue}
    print(f"[{_ts()}] loaded catalogue: {len(catalogue)} predicates "
          f"(train_to={art.train_to_iso})")

    # --- load + tag panel TRAIN ---
    print(f"[{_ts()}] loading panel: {args.panel}")
    df = load_panel(args.panel)
    bounds = PartitionBounds.from_iso(
        art.train_to_iso, art.val_to_iso, art.oos_to_iso,
    )
    df_train = train_partition(df, bounds)
    df_train_r = apply_regimes(df_train, art.regime_cuts)
    print(f"[{_ts()}] TRAIN bars after apply_regimes: {len(df_train_r)}")

    # --- step 1: flag absolute-price ---
    flagged = flag_absolute_price(survivors)
    n_abs = int(flagged['is_absolute_price'].sum())
    print(f"[{_ts()}] flagged absolute-price rows: {n_abs} of {n0} "
          f"({100.0*n_abs/n0:.1f}%)")

    # --- step 2: per-row TRAIN diagnostics ---
    print(f"[{_ts()}] computing TRAIN diagnostics "
          f"(spread cleanliness, time clustering by {args.time_bucket})...")
    flagged_diag = compute_train_diagnostics(
        flagged, df_train_r, catalogue_by_pid,
        time_bucket=args.time_bucket,
    )

    # --- step 3: dedup ---
    print(f"[{_ts()}] deduplicating by "
          f"(feature, op, regime, side, bracket, threshold_bucket)...")
    flagged_dedup = dedup_by_hypothesis_key(
        flagged_diag,
        threshold_bucket_decimals=args.threshold_bucket_decimals,
        rank_metric=args.rank_metric,
    )
    print(f"[{_ts()}] dedup: {len(flagged_diag)} -> {len(flagged_dedup)} rows")

    # --- step 4: write full flagged ---
    full_path = _save_df(flagged_dedup, outdir / 'c4_full_flagged')
    print(f"[{_ts()}] wrote {full_path} ({len(flagged_dedup)} rows)")

    # --- step 4: clean filter ---
    clean = select_clean(
        flagged_dedup,
        spread_clean_share_min=args.spread_clean_share_min,
        time_cluster_max=args.time_cluster_max,
    )
    clean_path = _save_df(clean, outdir / 'c4_clean')
    print(f"[{_ts()}] wrote {clean_path} ({len(clean)} rows)")

    # --- summary ---
    print()
    print(f"[{_ts()}] === SUMMARY ===")
    print(f"  VAL survivors in       : {n0}")
    print(f"  absolute-price flagged : {n_abs}")
    print(f"  after dedup            : {len(flagged_dedup)}")
    print(f"  CLEAN (OOS-ready)      : {len(clean)}")
    if len(clean) > 0:
        bracket_counts = clean['bracket_id'].value_counts().sort_index()
        print(f"  clean by bracket_id    : "
              + ", ".join(f"b{int(b)}={int(c)}" for b, c in bracket_counts.items()))
        side_counts = clean['side'].value_counts()
        print(f"  clean by side          : "
              + ", ".join(f"{s}={int(c)}" for s, c in side_counts.items()))
        feat_counts = clean['feature'].value_counts().head(10)
        print(f"  top features in clean  :")
        for f, c in feat_counts.items():
            print(f"      {f:30s} {int(c)}")

    # --- write a short JSON manifest for resume context ---
    manifest = {
        'when_iso':                  time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'panel':                     str(args.panel),
        'workdir':                   str(workdir),
        'outdir':                    str(outdir),
        'val_survivors_in':          int(n0),
        'absolute_price_flagged':    int(n_abs),
        'after_dedup':               int(len(flagged_dedup)),
        'clean_count':               int(len(clean)),
        'spread_clean_share_min':    args.spread_clean_share_min,
        'time_cluster_max':          args.time_cluster_max,
        'time_bucket':               args.time_bucket,
        'threshold_bucket_decimals': args.threshold_bucket_decimals,
        'rank_metric':               args.rank_metric,
        'spread_anomaly_threshold':  SPREAD_ANOMALY_THRESHOLD,
        'absolute_price_features':   sorted(ABSOLUTE_PRICE_FEATURES),
    }
    (outdir / 'c4_manifest.json').write_text(json.dumps(manifest, indent=2))
    print(f"[{_ts()}] wrote {outdir / 'c4_manifest.json'}")

    return 0


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def _parse_args(argv: Optional[list[str]] = None) -> InspectC4Args:
    p = argparse.ArgumentParser(
        prog='inspect_c4',
        description='Session C4 hygiene pass on VAL survivors '
                    '(absolute-price flag + spread + time-cluster + dedup).',
    )
    p.add_argument('--panel',   default=DEFAULT_PANEL)
    p.add_argument('--workdir', default=DEFAULT_WORKDIR,
                   help='dir containing val_survivors and catalogue')
    p.add_argument('--outdir',  default=DEFAULT_OUTDIR,
                   help='where to write c4_full_flagged + c4_clean')
    p.add_argument('--spread-clean-share-min', type=float,
                   default=DEFAULT_SPREAD_CLEAN_SHARE_MIN,
                   help='min fraction of TRAIN bars with spread_max_pts > '
                        f'{SPREAD_ANOMALY_THRESHOLD}')
    p.add_argument('--time-cluster-max', type=float,
                   default=DEFAULT_TIME_CLUSTER_MAX,
                   help='max share of TRAIN bars in any single time bucket')
    p.add_argument('--time-bucket', choices=('day', 'hour', 'week'),
                   default=DEFAULT_TIME_BUCKET)
    p.add_argument('--threshold-bucket-decimals', type=int,
                   default=DEFAULT_THRESHOLD_BUCKET_DECIMALS,
                   help='dedup granularity on per-feature threshold rank '
                        '(higher = more distinct hypotheses kept)')
    p.add_argument('--rank-metric', default='sharpe',
                   choices=('sharpe', 'excess_expectancy', 'expectancy', 'pf'),
                   help='which VAL metric to keep best of within a dedup group')
    a = p.parse_args(argv)
    return InspectC4Args(
        panel=a.panel,
        workdir=a.workdir,
        outdir=a.outdir,
        spread_clean_share_min=a.spread_clean_share_min,
        time_cluster_max=a.time_cluster_max,
        time_bucket=a.time_bucket,
        threshold_bucket_decimals=a.threshold_bucket_decimals,
        rank_metric=a.rank_metric,
    )


def main(argv: Optional[list[str]] = None) -> int:
    args = _parse_args(argv)
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
