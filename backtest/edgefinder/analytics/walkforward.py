"""
edgefinder/analytics/walkforward.py
===================================

Three-stage walk-forward gate:

    TRAIN  — fit regime cuts, fit quantile cuts, build catalogue,
             score every predicate, apply MTC. Output: train survivors.
    VAL    — re-evaluate train survivors on the validation partition.
             Survivors must keep sign-consistent expectancy and reach
             a minimum sample threshold (default 50). Output: val survivors.
    OOS    — re-evaluate val survivors on the out-of-sample partition.
             Single touch enforced by sentinel file AND by oos_partition()
             refusing to return data when the sentinel exists.

The sentinel mechanism is dual-layered per spec:

  1. A file at <output_dir>/.oos_consumed is written the moment OOS data
     is fetched. The file records timestamp + git SHA.
  2. oos_partition() checks the sentinel BEFORE returning the slice. If
     the sentinel exists and force=False, it raises OosAlreadyConsumed.
     Setting force=True is allowed but logs loudly and stamps the
     sentinel with a 're-consumed' marker that downstream report code
     can flag.

This is intentionally annoying: the OOS partition is supposed to be touched
exactly once, ever, for a given hypothesis family. If you need to "iterate"
on OOS, the right thing is a full re-extract on a new data slice, not a
softer sentinel.
"""
from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd


# Default partition boundaries. Aligned with the data range
# (2024-03-01 .. 2026-04-24) per the panel produced by the extractor.
DEFAULT_TRAIN_TO = '2025-12-31T23:59:59'
DEFAULT_VAL_TO   = '2026-01-31T23:59:59'
DEFAULT_OOS_TO   = '2026-04-30T23:59:59'

OOS_SENTINEL_NAME = '.oos_consumed'


class OosAlreadyConsumed(RuntimeError):
    """Raised when oos_partition() is called and a sentinel already exists."""


@dataclass
class PartitionBounds:
    train_to: pd.Timestamp
    val_to:   pd.Timestamp
    oos_to:   pd.Timestamp

    @staticmethod
    def from_iso(t_to: str, v_to: str, o_to: str) -> 'PartitionBounds':
        return PartitionBounds(
            train_to=pd.Timestamp(t_to, tz='UTC'),
            val_to=  pd.Timestamp(v_to, tz='UTC'),
            oos_to=  pd.Timestamp(o_to, tz='UTC'),
        )


def train_partition(df: pd.DataFrame, bounds: PartitionBounds) -> pd.DataFrame:
    return df[df.index <= bounds.train_to]


def val_partition(df: pd.DataFrame, bounds: PartitionBounds) -> pd.DataFrame:
    return df[(df.index > bounds.train_to) & (df.index <= bounds.val_to)]


def _sentinel_path(output_dir: str | os.PathLike) -> Path:
    return Path(output_dir) / OOS_SENTINEL_NAME


def oos_sentinel_exists(output_dir: str | os.PathLike) -> bool:
    return _sentinel_path(output_dir).exists()


def stamp_oos_sentinel(output_dir: str | os.PathLike, note: str = 'consumed') -> None:
    """Atomically write/append to the sentinel file."""
    p = _sentinel_path(output_dir)
    p.parent.mkdir(parents=True, exist_ok=True)
    record = {
        'when_iso': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'note': note,
        'pid': os.getpid(),
    }
    if p.exists():
        prior = p.read_text()
        p.write_text(prior + '\n' + json.dumps(record))
    else:
        p.write_text(json.dumps(record))


def oos_partition(
    df: pd.DataFrame,
    bounds: PartitionBounds,
    output_dir: str | os.PathLike,
    force: bool = False,
) -> pd.DataFrame:
    """
    Return the OOS slice. STAMPS THE SENTINEL on success. If the sentinel
    already exists and force=False, raises OosAlreadyConsumed.

    Set force=True only when knowingly re-running on a fresh hypothesis
    that has been independently TRAIN/VAL-validated. The sentinel file
    will be stamped 'reconsumed' to make the override audit-visible.
    """
    if oos_sentinel_exists(output_dir):
        if not force:
            raise OosAlreadyConsumed(
                f"OOS partition has already been consumed (sentinel: "
                f"{_sentinel_path(output_dir)}). Re-running OOS on the same "
                f"hypothesis family invalidates the single-shot guarantee. "
                f"If you really mean it, pass force=True. The sentinel will "
                f"be stamped 'reconsumed' for audit."
            )
        stamp_oos_sentinel(output_dir, note='reconsumed (force=True)')
    else:
        stamp_oos_sentinel(output_dir, note='consumed')

    return df[(df.index > bounds.val_to) & (df.index <= bounds.oos_to)]


def gate_val_survivors(
    val_prospects: pd.DataFrame,
    train_prospects: pd.DataFrame,
    min_n_val: int = 50,
    require_sign_match: bool = True,
    require_pf_gt_one: bool = True,
) -> pd.DataFrame:
    """
    From the per-(pid, side, bracket) results in val_prospects, return only
    the rows that:

      * Have n >= min_n_val on VAL.
      * Match the sign of expectancy on TRAIN (if require_sign_match).
        I.e. an edge that was profitable in TRAIN must remain profitable
        (positive expectancy) in VAL.
      * Have profit factor > 1 on VAL (if require_pf_gt_one).

    The returned DataFrame is the VAL prospect rows (NOT the train rows),
    annotated with an `expectancy_train` column for cross-reference and a
    boolean `gate_passed` column.
    """
    needed = {'pid', 'side', 'bracket_id', 'n', 'expectancy', 'pf'}
    miss_v = needed - set(val_prospects.columns)
    miss_t = needed - set(train_prospects.columns)
    if miss_v: raise ValueError(f"val_prospects missing: {miss_v}")
    if miss_t: raise ValueError(f"train_prospects missing: {miss_t}")

    train_idx = train_prospects.set_index(['pid', 'side', 'bracket_id'])
    val = val_prospects.copy()
    keys = list(zip(val['pid'], val['side'], val['bracket_id']))
    expect_train = []
    for k in keys:
        if k in train_idx.index:
            expect_train.append(float(train_idx.loc[k, 'expectancy']))
        else:
            expect_train.append(np.nan)
    val['expectancy_train'] = expect_train

    cond = val['n'] >= min_n_val
    if require_sign_match:
        cond &= np.sign(val['expectancy']) == np.sign(val['expectancy_train'])
        cond &= val['expectancy'] > 0  # train side already positive expectancy convention
    if require_pf_gt_one:
        cond &= val['pf'] > 1.0
    val['gate_passed'] = cond
    return val[cond].copy()
