"""
regrade_strong_v3.py — C6 #1C Step 3a: hybrid re-grade of v2 STRONG cohort
==========================================================================

Purpose
-------
Step 3 pre-flight (regime_diagnostic.py) found a structural divergence between
the linear regression result and the per-edge vol profiles. The aggregate
regression said calendar-driven (~80% post-coef retention after vol control),
but inspection of per-edge decile breakdowns showed several STRONG edges fire
in narrow vol bands rather than uniformly — meaning their post-tariff
appearance was a sampling coincidence, not a calendar effect.

This module re-grades the v2 STRONG cohort under a hybrid framework that
combines:

    1. SL-framework (atr_sl) economic gates  — recomputed cost_ok/sharpe_ok
                                                using atr_sl pnl, not the
                                                bracket-framework v1 means.

    2. Calendar gates                         — post_dominates (recomputed
                                                with atr_sl means) and
                                                sub_periods_consistent
                                                (passthrough from v2).

    3. Vol-decile coverage gates              — count of deciles with n>=5 AND
                                                mean_pnl>0; dominant_band
                                                classification (low/mid/high/
                                                mixed). Names true regime
                                                edges (REGIME_LOW_VOL or
                                                REGIME_HIGH_VOL) distinct
                                                from genuine TARIFF_DEPENDENT.

The output is a per-edge CSV plus a markdown report that classifies each
v2 STRONG entry into one of:

    STRONG              — regime-agnostic + economic
    STRONG_VOL_GATED    — economic but requires entry-vol gate
    REGIME_LOW_VOL      — fires only in low-vol deciles (1-3)
    REGIME_HIGH_VOL     — fires only in high-vol deciles (8-10)
    TARIFF_DEPENDENT    — post_dominates AND not vol-conditional
    MARGINAL            — economic gates fail
    CULL                — bleeds across deciles, fails everything

Both cost-floor scenarios (0.4 baseline + 0.8 stress) are graded and reported
side-by-side. Both vol measures (decile_bar + decile_wall) are graded; the
final verdict is the **most conservative** of the two when they disagree.

Read-only with respect to: simulator, v1, v2, panel binary, catalogue, OOS
sentinel, fwd_ret_journal.csv, drift_stability_v2_per_edge.csv, all C++ code.

Inputs
------
    backtest/edgefinder/output/paper_trade/regime_diagnostic_journal.csv
        Step 3 pre-flight output. atr_sl-only journal augmented with
        vol_bar60, vol_wall60, decile_bar, decile_wall.
    backtest/edgefinder/output/paper_trade/drift_stability_v2_per_edge.csv
        v2 verdicts; used to identify the STRONG cohort and pull through
        v2 columns for comparison.

Outputs
-------
    backtest/edgefinder/output/paper_trade/regrade_v3_per_edge.csv
        One row per (pid, side, bracket_id) in v2 STRONG. Columns include
        v2 fields + atr_sl economic stats + decile coverage metrics +
        v3 verdicts (per cost floor + per vol measure + final consolidated).
    backtest/edgefinder/output/paper_trade/regrade_v3.md
        Human-readable report.

Methodology
-----------
For each (pid, side, bracket_id) in the v2 STRONG cohort:

  1. Pull all atr_sl trades from regime_diagnostic_journal.csv matching
     (pid, side, bracket_id). These are the trades that survived to OOS.

  2. SL-framework economic stats:
        n_pre, n_post, mean_pre, mean_post, mean_oos
        sharpe_pre, sharpe_post, sharpe_oos
        sum_pre, sum_post, sum_oos
        cost_ok_04 = (mean_oos > 0.4)
        cost_ok_08 = (mean_oos > 0.8)
        sharpe_ok = (sharpe_oos > 0.10)

  3. Calendar gates:
        post_dominates_v3 = sign-aligned post AND (pre_flipped OR pre_collapsed)
            (matches v2 logic but uses atr_sl means)
        sub_periods_consistent = passthrough from v2

  4. Vol-decile coverage (separately for decile_bar and decile_wall):
        decile_coverage = count of deciles where n>=5 AND mean_pnl>0
        active_deciles  = list of deciles where n>=5 (regardless of sign)
        dominant_band   = 'low' if all active deciles in {1,2,3}
                          'mid' if all active in {4,5,6,7}
                          'high' if all active in {8,9,10}
                          'mixed' otherwise
        top2_concentration = sum of |sum_pnl| in top 2 deciles by |sum_pnl|,
                             divided by sum of |sum_pnl| across all deciles
        If active_deciles spans only {1,2,3}: REGIME_LOW_VOL candidate
        If active_deciles spans only {8,9,10}: REGIME_HIGH_VOL candidate

  5. Verdict ladder per (cost_floor, vol_measure) combination — 4 verdicts
     per edge. The 'final' verdict is the most conservative across all 4.

  6. For STRONG_VOL_GATED final verdicts, recommend an entry vol gate:
     the lowest decile cutoff (using decile_bar) at which the edge has
     positive cumulative pnl. The absolute vol_bar60 value at that cutoff
     is the lower bound of the recommended decile (i.e. the min of
     vol_bar60 in that decile across the full atr_sl population).

Verdict ladder (top-down, exclusive)
------------------------------------
    if not v1_stable:
        return CULL
    if active_deciles_subset_of([1,2,3]) and decile_coverage >= 1:
        return REGIME_LOW_VOL
    if active_deciles_subset_of([8,9,10]) and decile_coverage >= 1:
        return REGIME_HIGH_VOL
    if mean_oos <= 0 or decile_coverage == 0:
        return CULL
    if post_dominates:
        return TARIFF_DEPENDENT
    if not cost_ok or not sharpe_ok:
        return MARGINAL
    if decile_coverage >= 6 and dominant_band == 'mixed':
        return STRONG
    if decile_coverage >= 4:
        return STRONG_VOL_GATED
    return MARGINAL

Conservative consolidation order (most -> least conservative)
-------------------------------------------------------------
    CULL > REGIME_LOW_VOL > REGIME_HIGH_VOL > TARIFF_DEPENDENT
        > MARGINAL > STRONG_VOL_GATED > STRONG

When 4 verdicts disagree (cost_04+bar, cost_04+wall, cost_08+bar, cost_08+wall),
the final verdict = most conservative.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd


# -----------------------------------------------------------------------------
# Defaults
# -----------------------------------------------------------------------------
DEFAULT_OUT_DIR = 'backtest/edgefinder/output/paper_trade'
DEFAULT_DIAG_JOURNAL = f'{DEFAULT_OUT_DIR}/regime_diagnostic_journal.csv'
DEFAULT_V2_PER_EDGE = f'{DEFAULT_OUT_DIR}/drift_stability_v2_per_edge.csv'
DEFAULT_TARIFF_CUTOFF = '2026-04-02T00:00:00Z'

COST_FLOORS = [0.4, 0.8]
SHARPE_FLOOR = 0.10
N_FLOOR_SUBPERIOD = 20            # min n_pre AND n_post for sub-period stats
DECILE_MIN_N = 5                  # min trades in a decile to count for coverage
DECILE_COVERAGE_STRONG = 6        # min coverage for unconditional STRONG
DECILE_COVERAGE_VOL_GATED = 4     # min coverage for STRONG_VOL_GATED
LOW_BAND = {1, 2, 3}
MID_BAND = {4, 5, 6, 7}
HIGH_BAND = {8, 9, 10}

# Conservative ordering: lower index = more conservative.
VERDICT_RANK = {
    'CULL':              0,
    'REGIME_LOW_VOL':    1,
    'REGIME_HIGH_VOL':   2,
    'TARIFF_DEPENDENT':  3,
    'MARGINAL':          4,
    'STRONG_VOL_GATED':  5,
    'STRONG':            6,
}


def _log(msg: str) -> None:
    print(msg, flush=True)


def _most_conservative(verdicts: list[str]) -> str:
    """Return the most conservative verdict from a list (lowest VERDICT_RANK)."""
    if not verdicts:
        return 'CULL'
    return min(verdicts, key=lambda v: VERDICT_RANK.get(v, -1))


# -----------------------------------------------------------------------------
# IO
# -----------------------------------------------------------------------------
def load_diag_journal(path: Path) -> pd.DataFrame:
    """Load Step 3 pre-flight augmented journal."""
    _log(f"loading regime diagnostic journal: {path}")
    df = pd.read_csv(path)
    required = {'ts_close', 'pid', 'side', 'bracket_id', 'sl_mode', 'pnl_pts',
                'subperiod', 'vol_bar60', 'vol_wall60', 'decile_bar',
                'decile_wall'}
    missing = required - set(df.columns)
    if missing:
        raise RuntimeError(f"diag journal missing cols: {sorted(missing)}")
    if not (df['sl_mode'] == 'atr_sl').all():
        # Step 3 pre-flight wrote atr_sl only, but be defensive.
        df = df[df['sl_mode'] == 'atr_sl'].copy()
    df['ts_close'] = pd.to_datetime(df['ts_close'], utc=True)
    _log(f"  {len(df):,} atr_sl trades")
    return df


def load_v2_per_edge(path: Path) -> pd.DataFrame:
    """Load v2 verdicts CSV."""
    _log(f"loading v2 per-edge verdicts: {path}")
    df = pd.read_csv(path)
    if 'verdict_v2' not in df.columns:
        raise RuntimeError(f"v2 per-edge missing 'verdict_v2'")
    _log(f"  {len(df)} edges total; verdict_v2 counts: "
         f"{df['verdict_v2'].value_counts().to_dict()}")
    return df


# -----------------------------------------------------------------------------
# Per-edge stats
# -----------------------------------------------------------------------------
def _safe_sharpe(arr: np.ndarray) -> float:
    """Sharpe of a per-trade pnl_pts array. ddof=1, returns nan if n<2 or
    std=0."""
    if len(arr) < 2:
        return float('nan')
    s = float(arr.std(ddof=1))
    if not (s > 0):
        return float('nan')
    return float(arr.mean()) / s


def edge_economic_stats(trades: pd.DataFrame) -> dict:
    """Compute SL-framework economic stats for one edge (atr_sl-only).
    `trades` is the journal subset for one (pid, side, bracket_id)."""
    pnl = trades['pnl_pts'].values.astype(np.float64)
    pre = trades[trades['subperiod'] == 'pre']['pnl_pts'].values.astype(
        np.float64)
    post = trades[trades['subperiod'] == 'post']['pnl_pts'].values.astype(
        np.float64)

    return {
        'n_oos':       int(len(pnl)),
        'n_pre':       int(len(pre)),
        'n_post':      int(len(post)),
        'mean_oos':    float(pnl.mean()) if len(pnl) > 0 else float('nan'),
        'mean_pre':    float(pre.mean()) if len(pre) > 0 else float('nan'),
        'mean_post':   float(post.mean()) if len(post) > 0 else float('nan'),
        'sum_oos':     float(pnl.sum()),
        'sum_pre':     float(pre.sum()),
        'sum_post':    float(post.sum()),
        'sharpe_oos':  _safe_sharpe(pnl),
        'sharpe_pre':  _safe_sharpe(pre),
        'sharpe_post': _safe_sharpe(post),
    }


def post_dominates_atr(stats: dict) -> bool:
    """Recompute post_dominates using atr_sl (SL-framework) means.

    Definition (matching v2 spirit):
      post_dominates = sign-aligned post AND (pre_flipped OR pre_collapsed
                                              OR pre_unknown)

    Where:
      sign(x): +1 if x > 0, -1 if x < 0, 0 if abs(x) < eps or nan
      sign-aligned post: sign(mean_post) == sign(mean_oos) and != 0
      pre_flipped: sign(mean_pre) is opposite to sign(mean_post)
      pre_collapsed: |mean_pre| < 0.25 * |mean_post|
                     (matches v2's heuristic; pre is too small to confirm)
      pre_unknown: n_pre < N_FLOOR_SUBPERIOD
    """
    eps = 1e-9

    def _sign(x):
        if x is None or not np.isfinite(x) or abs(x) < eps:
            return 0
        return 1 if x > 0 else -1

    sign_oos = _sign(stats['mean_oos'])
    sign_pre = _sign(stats['mean_pre'])
    sign_post = _sign(stats['mean_post'])
    n_pre = stats['n_pre']
    n_post = stats['n_post']

    if sign_oos == 0 or sign_post == 0:
        return False
    if sign_post != sign_oos:
        return False
    if n_post < N_FLOOR_SUBPERIOD:
        return False

    pre_unknown = n_pre < N_FLOOR_SUBPERIOD
    pre_flipped = (sign_pre != 0 and sign_pre != sign_post)
    pre_collapsed = (
        np.isfinite(stats['mean_pre']) and np.isfinite(stats['mean_post'])
        and abs(stats['mean_pre']) < 0.25 * abs(stats['mean_post'])
    )

    return bool(pre_unknown or pre_flipped or pre_collapsed)


# -----------------------------------------------------------------------------
# Decile coverage
# -----------------------------------------------------------------------------
def _band_classify(active: set[int]) -> str:
    """Classify the active-decile set into low/mid/high/mixed."""
    if not active:
        return 'none'
    if active.issubset(LOW_BAND):
        return 'low'
    if active.issubset(MID_BAND):
        return 'mid'
    if active.issubset(HIGH_BAND):
        return 'high'
    return 'mixed'


def decile_coverage_stats(
    trades: pd.DataFrame,
    decile_col: str,
) -> dict:
    """Compute coverage stats for one edge under one vol measure.

    Coverage rules:
      decile_coverage   = count of deciles where n>=DECILE_MIN_N AND mean_pnl>0
      active_deciles    = set of deciles where n>=DECILE_MIN_N
      dominant_band     = low/mid/high/mixed based on active_deciles
      top2_concentration = top-2 |sum_pnl| / sum |sum_pnl| across active deciles
    """
    sub = trades.dropna(subset=[decile_col, 'pnl_pts']).copy()
    if sub.empty:
        return {
            'n_trades': 0,
            'decile_coverage': 0,
            'active_deciles': '',
            'profitable_deciles': '',
            'dominant_band': 'none',
            'top2_concentration': float('nan'),
            'cumulative_by_decile': '{}',
        }
    sub[decile_col] = sub[decile_col].astype(int)
    grp = sub.groupby(decile_col)['pnl_pts'].agg(['count', 'mean', 'sum'])
    grp = grp.reset_index().rename(columns={decile_col: 'decile'})

    active = set(int(r['decile']) for _, r in grp.iterrows()
                 if r['count'] >= DECILE_MIN_N)
    profitable = set(int(r['decile']) for _, r in grp.iterrows()
                     if r['count'] >= DECILE_MIN_N and r['mean'] > 0)
    band = _band_classify(active)

    if not grp.empty:
        abs_sums = np.abs(grp['sum'].values)
        total_abs = abs_sums.sum()
        if total_abs > 0:
            top2 = float(np.sort(abs_sums)[-2:].sum() / total_abs)
        else:
            top2 = float('nan')
    else:
        top2 = float('nan')

    cum_by_decile = {int(r['decile']): {
        'n': int(r['count']),
        'mean': float(r['mean']),
        'sum': float(r['sum']),
    } for _, r in grp.iterrows()}

    return {
        'n_trades': int(sub.shape[0]),
        'decile_coverage': len(profitable),
        'active_deciles': ','.join(str(d) for d in sorted(active)),
        'profitable_deciles': ','.join(str(d) for d in sorted(profitable)),
        'dominant_band': band,
        'top2_concentration': top2,
        'cumulative_by_decile': json.dumps(cum_by_decile),
    }


# -----------------------------------------------------------------------------
# Verdict ladder
# -----------------------------------------------------------------------------
def assign_verdict(
    v1_stable: bool,
    stats: dict,
    cost_floor: float,
    coverage: dict,
) -> str:
    """Apply the v3 verdict ladder. See module docstring."""
    if not v1_stable:
        return 'CULL'

    active = set(int(d) for d in coverage['active_deciles'].split(',')
                 if d != '')

    # REGIME_LOW_VOL: fires only in deciles 1-3 (with at least one positive).
    # decile_coverage > 0 prevents 'fires only in dead deciles' edge case.
    if active and active.issubset(LOW_BAND) and coverage['decile_coverage'] >= 1:
        return 'REGIME_LOW_VOL'
    if active and active.issubset(HIGH_BAND) and coverage['decile_coverage'] >= 1:
        return 'REGIME_HIGH_VOL'

    # Dead edges: negative mean OR zero profitable deciles.
    if not np.isfinite(stats['mean_oos']) or stats['mean_oos'] <= 0:
        return 'CULL'
    if coverage['decile_coverage'] == 0:
        return 'CULL'

    # Calendar dependence.
    if post_dominates_atr(stats):
        return 'TARIFF_DEPENDENT'

    # Economic gates.
    cost_ok = stats['mean_oos'] > cost_floor
    sharpe_ok = (np.isfinite(stats['sharpe_oos'])
                 and stats['sharpe_oos'] > SHARPE_FLOOR)
    if not (cost_ok and sharpe_ok):
        return 'MARGINAL'

    # Coverage gates.
    if (coverage['decile_coverage'] >= DECILE_COVERAGE_STRONG
            and coverage['dominant_band'] == 'mixed'):
        return 'STRONG'
    if coverage['decile_coverage'] >= DECILE_COVERAGE_VOL_GATED:
        return 'STRONG_VOL_GATED'
    return 'MARGINAL'


# -----------------------------------------------------------------------------
# Recommended vol gate for STRONG_VOL_GATED edges
# -----------------------------------------------------------------------------
def recommend_vol_gate(
    edge_trades: pd.DataFrame,
    full_population: pd.DataFrame,
) -> Optional[dict]:
    """For a STRONG_VOL_GATED edge, find the cutoff decile that maximises
    sharpe-after-gate, subject to the cutoff being a *meaningful* gate (i.e.
    the gate must materially improve on the no-gate case, otherwise return
    None to signal 'no gate needed').

    Returns None if no cutoff in [2..10] yields:
        (a) at least DECILE_MIN_N trades after the gate,
        (b) positive mean_after,
        (c) sharpe_after > 1.5 * sharpe_no_gate (or sharpe_no_gate <= 0),
        (d) the resulting recommendation is not just cutoff=1 (no filter).

    Picks the cutoff with the highest sharpe_after among qualifying cutoffs.
    Tie-broken by the lowest cutoff (most permissive). If no cutoff qualifies,
    returns None — the calling MD then writes 'no meaningful gate found'.

    `edge_trades` is the journal subset for this edge.
    `full_population` is the full atr_sl journal (used to derive the absolute
    vol_bar60 value at each decile boundary).
    """
    et = edge_trades.dropna(subset=['decile_bar', 'pnl_pts', 'vol_bar60'])
    if et.empty:
        return None

    # No-gate baseline (the entire edge population).
    pnl_full = et['pnl_pts'].values
    if len(pnl_full) < 2:
        return None
    mean_full = float(pnl_full.mean())
    sharpe_full = _safe_sharpe(pnl_full)

    # Decile boundaries from the full atr_sl population.
    fp = full_population.dropna(subset=['vol_bar60', 'decile_bar']).copy()
    fp['decile_bar'] = fp['decile_bar'].astype(int)
    decile_min_vol = fp.groupby('decile_bar')['vol_bar60'].min().to_dict()

    candidates = []
    # cutoff=1 is "no gate" — exclude it.
    # Minimum sample after gate: 50 trades (avoid overfit small-sample picks).
    GATE_MIN_AFTER = 50
    for cutoff in range(2, 11):
        sub = et[et['decile_bar'].astype(int) >= cutoff]
        if len(sub) < max(DECILE_MIN_N, GATE_MIN_AFTER):
            continue
        pnl = sub['pnl_pts'].values
        if len(pnl) < 2:
            continue
        mean_pnl = float(pnl.mean())
        sharpe = _safe_sharpe(pnl)
        if not (mean_pnl > 0):
            continue
        if not np.isfinite(sharpe):
            continue

        # Improvement requirement: sharpe after gate must materially exceed
        # the no-gate sharpe. If no-gate sharpe is non-positive, any positive
        # post-gate sharpe qualifies. Otherwise require >= 1.5x improvement.
        improves = (
            (not np.isfinite(sharpe_full) or sharpe_full <= 0)
            or (sharpe > 1.5 * sharpe_full)
        )
        if not improves:
            continue

        candidates.append({
            'cutoff_decile': cutoff,
            'cutoff_vol_bar60': float(decile_min_vol.get(cutoff,
                                                         float('nan'))),
            'n_after_gate': int(len(sub)),
            'mean_pnl_after_gate': mean_pnl,
            'sharpe_after_gate': sharpe,
            'sum_pnl_after_gate': float(pnl.sum()),
            'sharpe_no_gate': sharpe_full,
            'mean_no_gate': mean_full,
        })

    if not candidates:
        return None

    # Pick highest sharpe; tie-break to lowest cutoff (most permissive).
    candidates.sort(key=lambda r: (-r['sharpe_after_gate'], r['cutoff_decile']))
    return candidates[0]


# -----------------------------------------------------------------------------
# Main grading loop
# -----------------------------------------------------------------------------
def grade_strong_cohort(
    diag_journal: pd.DataFrame,
    v2_per_edge: pd.DataFrame,
) -> pd.DataFrame:
    """Re-grade every v2 STRONG edge under the hybrid v3 framework.

    Returns a DataFrame with one row per (pid, side, bracket_id) in v2 STRONG.
    """
    strong = v2_per_edge[v2_per_edge['verdict_v2'] == 'STRONG'].copy()
    _log(f"v2 STRONG cohort: {len(strong)} edges")

    rows = []
    for _, edge in strong.iterrows():
        pid = int(edge['pid'])
        side = str(edge['side'])
        bid = int(edge['bracket_id'])
        v1_stable = (edge.get('verdict_v1') == 'STABLE')

        et = diag_journal[
            (diag_journal['pid'] == pid)
            & (diag_journal['side'] == side)
            & (diag_journal['bracket_id'] == bid)
        ].copy()

        if et.empty:
            _log(f"  WARN: no journal rows for pid {pid} {side} bracket {bid}")
            rows.append({
                'pid': pid, 'side': side, 'bracket_id': bid,
                'v1_stable': v1_stable,
                'verdict_v2': 'STRONG',
                'verdict_v3_final': 'CULL',
                'note': 'no atr_sl trades in diagnostic journal',
            })
            continue

        stats = edge_economic_stats(et)
        cov_bar = decile_coverage_stats(et, 'decile_bar')
        cov_wall = decile_coverage_stats(et, 'decile_wall')

        # Grade under each (cost_floor, vol_measure) combination.
        verdicts = {}
        for cf in COST_FLOORS:
            for vm_name, cov in [('bar', cov_bar), ('wall', cov_wall)]:
                v = assign_verdict(v1_stable, stats, cf, cov)
                verdicts[f'verdict_v3_cost{cf}_{vm_name}'] = v

        v_list = list(verdicts.values())
        final = _most_conservative(v_list)

        # Recommended vol gate (only meaningful if final == STRONG_VOL_GATED).
        gate = None
        if final == 'STRONG_VOL_GATED':
            gate = recommend_vol_gate(et, diag_journal)

        row = {
            'pid': pid,
            'side': side,
            'bracket_id': bid,
            'rank': edge.get('rank'),
            'v1_stable': v1_stable,
            'verdict_v1': edge.get('verdict_v1'),
            'verdict_v2': 'STRONG',
            # economic stats (atr_sl)
            'n_oos':       stats['n_oos'],
            'n_pre':       stats['n_pre'],
            'n_post':      stats['n_post'],
            'mean_oos':    stats['mean_oos'],
            'mean_pre':    stats['mean_pre'],
            'mean_post':   stats['mean_post'],
            'sum_oos':     stats['sum_oos'],
            'sum_pre':     stats['sum_pre'],
            'sum_post':    stats['sum_post'],
            'sharpe_oos':  stats['sharpe_oos'],
            'sharpe_pre':  stats['sharpe_pre'],
            'sharpe_post': stats['sharpe_post'],
            # v2 calendar gates (passthrough for comparison)
            'v2_post_dominates': edge.get('post_dominates'),
            'v2_sub_periods_consistent': edge.get('sub_periods_consistent'),
            # v3 calendar gate (recomputed under atr_sl)
            'v3_post_dominates_atr': post_dominates_atr(stats),
            # cost floor outcomes
            'cost_ok_04': stats['mean_oos'] > 0.4,
            'cost_ok_08': stats['mean_oos'] > 0.8,
            'sharpe_ok_v3': (np.isfinite(stats['sharpe_oos'])
                             and stats['sharpe_oos'] > SHARPE_FLOOR),
            # coverage (bar)
            'decile_coverage_bar':    cov_bar['decile_coverage'],
            'active_deciles_bar':     cov_bar['active_deciles'],
            'profitable_deciles_bar': cov_bar['profitable_deciles'],
            'dominant_band_bar':      cov_bar['dominant_band'],
            'top2_concentration_bar': cov_bar['top2_concentration'],
            # coverage (wall)
            'decile_coverage_wall':    cov_wall['decile_coverage'],
            'active_deciles_wall':     cov_wall['active_deciles'],
            'profitable_deciles_wall': cov_wall['profitable_deciles'],
            'dominant_band_wall':      cov_wall['dominant_band'],
            'top2_concentration_wall': cov_wall['top2_concentration'],
            # verdicts (4 cells)
            **verdicts,
            'verdict_v3_final': final,
            # vol gate recommendation
            'gate_cutoff_decile':      gate['cutoff_decile'] if gate else None,
            'gate_cutoff_vol_bar60':   gate['cutoff_vol_bar60'] if gate else None,
            'gate_n_after':            gate['n_after_gate'] if gate else None,
            'gate_mean_pnl_after':     gate['mean_pnl_after_gate'] if gate else None,
            'gate_sharpe_after':       gate['sharpe_after_gate'] if gate else None,
            'gate_sum_pnl_after':      gate['sum_pnl_after_gate'] if gate else None,
            'gate_sharpe_no_gate':     gate['sharpe_no_gate'] if gate else None,
            'gate_mean_no_gate':       gate['mean_no_gate'] if gate else None,
            # decile detail
            'cumulative_by_decile_bar':  cov_bar['cumulative_by_decile'],
            'cumulative_by_decile_wall': cov_wall['cumulative_by_decile'],
        }
        rows.append(row)

    return pd.DataFrame(rows)


# -----------------------------------------------------------------------------
# MD report
# -----------------------------------------------------------------------------
def _fmt_float(x, w: int = 9, prec: int = 4) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'nan':>{w}}"
    return f"{float(x):>+{w}.{prec}f}"


def _fmt_int(x, w: int = 5) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'-':>{w}}"
    try:
        return f"{int(x):>{w}}"
    except (ValueError, TypeError):
        return f"{'-':>{w}}"


def _fmt_bool(x) -> str:
    if x is True:
        return 'Y'
    if x is False:
        return 'N'
    if isinstance(x, str):
        return 'Y' if x.lower() == 'true' else ('N' if x.lower() == 'false'
                                                 else x)
    return '-'


def write_md(out_path: Path, graded: pd.DataFrame) -> None:
    lines = []
    lines.append("# C6 #1C Step 3a — hybrid v3 re-grade of v2 STRONG cohort")
    lines.append("")
    lines.append("Combines SL-framework (atr_sl) economic gates, calendar "
                 "gates (post_dominates), and vol-decile coverage gates. "
                 "Both cost-floor scenarios (0.4 baseline, 0.8 stress) and "
                 "both vol measures (decile_bar, decile_wall) are graded. "
                 "**Final verdict is the most conservative of the four cells.**")
    lines.append("")
    lines.append("Verdict ladder (top-down, exclusive):")
    lines.append("")
    lines.append("```")
    lines.append("CULL                : v1 not STABLE, OR mean_oos <= 0,")
    lines.append("                      OR zero profitable deciles")
    lines.append("REGIME_LOW_VOL      : active deciles all in {1,2,3}")
    lines.append("REGIME_HIGH_VOL     : active deciles all in {8,9,10}")
    lines.append("TARIFF_DEPENDENT    : post_dominates AND not vol-conditional")
    lines.append("MARGINAL            : economic gates fail (cost or sharpe)")
    lines.append("STRONG              : decile_coverage>=6 AND band='mixed'")
    lines.append("STRONG_VOL_GATED    : decile_coverage>=4 (any band)")
    lines.append("```")
    lines.append("")
    lines.append("Conservative consolidation order (most -> least):")
    lines.append("`CULL > REGIME_LOW_VOL > REGIME_HIGH_VOL > TARIFF_DEPENDENT "
                 "> MARGINAL > STRONG_VOL_GATED > STRONG`")
    lines.append("")

    # ---- v2 -> v3 transition table ----
    lines.append("## v2 -> v3 final verdict transitions")
    lines.append("")
    counts = graded['verdict_v3_final'].value_counts()
    lines.append("| verdict_v3_final  | n |")
    lines.append("|-------------------|--:|")
    for v in ['STRONG', 'STRONG_VOL_GATED', 'TARIFF_DEPENDENT', 'MARGINAL',
              'REGIME_LOW_VOL', 'REGIME_HIGH_VOL', 'CULL']:
        n = int(counts.get(v, 0))
        if n > 0:
            lines.append(f"| {v:<17} | {n} |")
    lines.append("")

    # ---- 4-cell verdict table ----
    lines.append("## per-edge 4-cell verdict matrix (cost_floor x vol_measure)")
    lines.append("")
    lines.append("| pid | side | b | cost04+bar | cost04+wall | cost08+bar | cost08+wall | **final**            |")
    lines.append("|----:|:-----|--:|:-----------|:------------|:-----------|:------------|:---------------------|")
    for _, r in graded.sort_values(['pid', 'side', 'bracket_id']).iterrows():
        lines.append(
            f"| {int(r['pid'])} | {r['side']} | {int(r['bracket_id'])} | "
            f"{r.get('verdict_v3_cost0.4_bar', '')} | "
            f"{r.get('verdict_v3_cost0.4_wall', '')} | "
            f"{r.get('verdict_v3_cost0.8_bar', '')} | "
            f"{r.get('verdict_v3_cost0.8_wall', '')} | "
            f"**{r['verdict_v3_final']}** |"
        )
    lines.append("")

    # ---- Economic stats table ----
    lines.append("## per-edge SL-framework (atr_sl) economic stats")
    lines.append("")
    lines.append("| pid | side | b | n_oos | n_pre | n_post | mean_pre | mean_post | mean_oos | sharpe_oos | sum_oos | post_dom_v3 |")
    lines.append("|----:|:-----|--:|------:|------:|-------:|---------:|----------:|---------:|-----------:|--------:|:-----------:|")
    for _, r in graded.sort_values(['pid', 'side', 'bracket_id']).iterrows():
        lines.append(
            f"| {int(r['pid'])} | {r['side']} | {int(r['bracket_id'])} | "
            f"{_fmt_int(r['n_oos'])} | {_fmt_int(r['n_pre'])} | "
            f"{_fmt_int(r['n_post'])} | "
            f"{_fmt_float(r['mean_pre'], 8, 3)} | "
            f"{_fmt_float(r['mean_post'], 9, 3)} | "
            f"{_fmt_float(r['mean_oos'], 8, 3)} | "
            f"{_fmt_float(r['sharpe_oos'], 9, 4)} | "
            f"{_fmt_float(r['sum_oos'], 8, 1)} | "
            f"{_fmt_bool(r['v3_post_dominates_atr']):^11} |"
        )
    lines.append("")

    # ---- Coverage table ----
    lines.append("## per-edge decile coverage")
    lines.append("")
    lines.append("| pid | side | b | cov_bar | active_bar | profitable_bar | band_bar | top2_bar | cov_wall | active_wall | band_wall |")
    lines.append("|----:|:-----|--:|--------:|:-----------|:---------------|:---------|---------:|---------:|:------------|:----------|")
    for _, r in graded.sort_values(['pid', 'side', 'bracket_id']).iterrows():
        lines.append(
            f"| {int(r['pid'])} | {r['side']} | {int(r['bracket_id'])} | "
            f"{int(r['decile_coverage_bar'])} | "
            f"{r['active_deciles_bar']:<10s} | "
            f"{r['profitable_deciles_bar']:<14s} | "
            f"{r['dominant_band_bar']:<8s} | "
            f"{_fmt_float(r['top2_concentration_bar'], 7, 3)} | "
            f"{int(r['decile_coverage_wall'])} | "
            f"{r['active_deciles_wall']:<11s} | "
            f"{r['dominant_band_wall']:<9s} |"
        )
    lines.append("")

    # ---- Recommended vol gates ----
    gated = graded[graded['verdict_v3_final'] == 'STRONG_VOL_GATED'].copy()
    lines.append("## recommended vol gates (STRONG_VOL_GATED edges only)")
    lines.append("")
    if gated.empty:
        lines.append("(no STRONG_VOL_GATED edges)")
        lines.append("")
    else:
        lines.append("Gate rule: enter only when `vol_bar60 >= cutoff_vol_bar60`.")
        lines.append("Cutoff = decile [2..10] that maximises sharpe_after_gate, "
                     "subject to:")
        lines.append("")
        lines.append("* at least 50 trades after the gate (avoids overfit small-sample picks),")
        lines.append("* positive mean_after_gate,")
        lines.append("* sharpe_after_gate > 1.5x sharpe_no_gate (or sharpe_no_gate <= 0).")
        lines.append("")
        lines.append("If no cutoff qualifies, the edge survives as STRONG_VOL_GATED but "
                     "the recommendation is **'no meaningful gate found'** — the edge "
                     "is profitable in aggregate but vol concentration alone cannot "
                     "improve the sharpe sufficiently. Reconsider the verdict in 3c.")
        lines.append("")
        lines.append("| pid | side | b | cutoff_dec | cutoff_vol_bar60 | n_after | mean_after | sharpe_after | sharpe_no_gate | sum_after |")
        lines.append("|----:|:-----|--:|-----------:|-----------------:|--------:|-----------:|-------------:|---------------:|----------:|")
        for _, r in gated.sort_values(['pid', 'side', 'bracket_id']).iterrows():
            cd = r.get('gate_cutoff_decile')
            cv = r.get('gate_cutoff_vol_bar60')
            na = r.get('gate_n_after')
            ma = r.get('gate_mean_pnl_after')
            sa = r.get('gate_sharpe_after')
            sn = r.get('gate_sharpe_no_gate')
            sm = r.get('gate_sum_pnl_after')
            if cd is None or (isinstance(cd, float) and not np.isfinite(cd)):
                lines.append(
                    f"| {int(r['pid'])} | {r['side']} | "
                    f"{int(r['bracket_id'])} | (no meaningful gate found) "
                    f"| - | - | - | - | "
                    f"{_fmt_float(r['sharpe_oos'], 14, 4)} | - |"
                )
                continue
            lines.append(
                f"| {int(r['pid'])} | {r['side']} | "
                f"{int(r['bracket_id'])} | {int(cd):>10d} | "
                f"{_fmt_float(cv, 16, 8)} | "
                f"{_fmt_int(int(na) if na is not None else None)} | "
                f"{_fmt_float(ma, 10, 3)} | "
                f"{_fmt_float(sa, 12, 4)} | "
                f"{_fmt_float(sn, 14, 4)} | "
                f"{_fmt_float(sm, 9, 1)} |"
            )
        lines.append("")

    # ---- Per-edge detail blocks ----
    lines.append("## per-edge detail blocks")
    lines.append("")
    for _, r in graded.sort_values(['pid', 'side', 'bracket_id']).iterrows():
        lines.append(f"### pid {int(r['pid'])} {r['side']} "
                     f"bracket {int(r['bracket_id'])} -> "
                     f"**{r['verdict_v3_final']}**")
        lines.append("")
        lines.append(f"* v2: STRONG  -> v3 final: **{r['verdict_v3_final']}**")
        lines.append(f"* economic (atr_sl): n={int(r['n_oos'])}, "
                     f"mean_pre={_fmt_float(r['mean_pre'], 0, 3).strip()}, "
                     f"mean_post={_fmt_float(r['mean_post'], 0, 3).strip()}, "
                     f"mean_oos={_fmt_float(r['mean_oos'], 0, 3).strip()}, "
                     f"sharpe_oos={_fmt_float(r['sharpe_oos'], 0, 4).strip()}")
        lines.append(f"* cost gates: cost_ok@0.4={_fmt_bool(r['cost_ok_04'])}, "
                     f"cost_ok@0.8={_fmt_bool(r['cost_ok_08'])}, "
                     f"sharpe_ok={_fmt_bool(r['sharpe_ok_v3'])}")
        lines.append(f"* calendar: v3_post_dominates_atr="
                     f"{_fmt_bool(r['v3_post_dominates_atr'])} "
                     f"(v2 said {_fmt_bool(r['v2_post_dominates'])})")
        lines.append(f"* coverage_bar: {int(r['decile_coverage_bar'])} "
                     f"profitable / active={r['active_deciles_bar']}, "
                     f"band={r['dominant_band_bar']}, "
                     f"top2={_fmt_float(r['top2_concentration_bar'], 0, 3).strip()}")
        lines.append(f"* coverage_wall: {int(r['decile_coverage_wall'])} "
                     f"profitable / active={r['active_deciles_wall']}, "
                     f"band={r['dominant_band_wall']}, "
                     f"top2={_fmt_float(r['top2_concentration_wall'], 0, 3).strip()}")
        try:
            cum = json.loads(r['cumulative_by_decile_bar'])
            if cum:
                lines.append("")
                lines.append("decile_bar breakdown:")
                lines.append("")
                lines.append("| decile |   n | mean_pnl | sum_pnl |")
                lines.append("|-------:|----:|---------:|--------:|")
                for d in sorted(int(k) for k in cum.keys()):
                    c = cum[str(d)] if str(d) in cum else cum.get(d)
                    lines.append(
                        f"| {d:>6d} | {int(c['n']):>3d} | "
                        f"{_fmt_float(c['mean'], 8, 3)} | "
                        f"{_fmt_float(c['sum'], 8, 1)} |"
                    )
        except (json.JSONDecodeError, TypeError):
            pass
        lines.append("")

    # ---- Survivors summary ----
    survivors = graded[graded['verdict_v3_final'].isin(
        ['STRONG', 'STRONG_VOL_GATED'])]
    lines.append("## survivors summary")
    lines.append("")
    if survivors.empty:
        lines.append("**No survivors.** v2 STRONG cohort fully demoted under "
                     "v3 hybrid framework.")
    else:
        lines.append(f"**{len(survivors)} survivor(s):**")
        lines.append("")
        for _, r in survivors.iterrows():
            extra = ""
            if r['verdict_v3_final'] == 'STRONG_VOL_GATED':
                cd = r.get('gate_cutoff_decile')
                cv = r.get('gate_cutoff_vol_bar60')
                if cd is not None and not (isinstance(cd, float)
                                           and not np.isfinite(cd)):
                    extra = (f" — gate: vol_bar60 >= "
                             f"{cv:.8f} (decile {int(cd)})")
                else:
                    extra = (" — no meaningful vol gate found "
                             "(profitable in aggregate; reconsider in 3c)")
            lines.append(f"* pid {int(r['pid'])} {r['side']} "
                         f"bracket {int(r['bracket_id'])}: "
                         f"**{r['verdict_v3_final']}**{extra}")
    lines.append("")

    out_path.write_text("\n".join(lines))
    _log(f"wrote {out_path}")


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
def main() -> int:
    p = argparse.ArgumentParser(
        description="C6 #1C Step 3a hybrid re-grade of v2 STRONG cohort.",
    )
    p.add_argument('--diag-journal', default=DEFAULT_DIAG_JOURNAL,
                   help=f"path to regime_diagnostic_journal.csv "
                        f"(default {DEFAULT_DIAG_JOURNAL})")
    p.add_argument('--v2-per-edge', default=DEFAULT_V2_PER_EDGE,
                   help=f"path to drift_stability_v2_per_edge.csv "
                        f"(default {DEFAULT_V2_PER_EDGE})")
    p.add_argument('--out-dir', default=DEFAULT_OUT_DIR,
                   help=f"output dir (default {DEFAULT_OUT_DIR})")
    args = p.parse_args()

    diag_path = Path(args.diag_journal)
    v2_path = Path(args.v2_per_edge)
    out_dir = Path(args.out_dir)

    for f in [diag_path, v2_path]:
        if not f.is_file():
            print(f"ERROR: missing input: {f}", file=sys.stderr)
            return 2
    out_dir.mkdir(parents=True, exist_ok=True)

    diag = load_diag_journal(diag_path)
    v2 = load_v2_per_edge(v2_path)

    graded = grade_strong_cohort(diag, v2)
    _log(f"graded {len(graded)} edges")

    csv_path = out_dir / 'regrade_v3_per_edge.csv'
    graded.to_csv(csv_path, index=False)
    _log(f"wrote {csv_path}")

    md_path = out_dir / 'regrade_v3.md'
    write_md(md_path, graded)

    # Summary to stdout
    counts = graded['verdict_v3_final'].value_counts()
    _log("")
    _log("=== v3 hybrid re-grade summary ===")
    for v in ['STRONG', 'STRONG_VOL_GATED', 'TARIFF_DEPENDENT', 'MARGINAL',
              'REGIME_LOW_VOL', 'REGIME_HIGH_VOL', 'CULL']:
        n = int(counts.get(v, 0))
        if n > 0:
            _log(f"  {v:<17}: {n}")
    survivors = graded[graded['verdict_v3_final'].isin(
        ['STRONG', 'STRONG_VOL_GATED'])]
    _log(f"  -> survivors: {len(survivors)}")
    for _, r in survivors.iterrows():
        _log(f"     pid {int(r['pid'])} {r['side']} "
             f"bracket {int(r['bracket_id'])}: {r['verdict_v3_final']}")
    _log("==================================")
    return 0


if __name__ == '__main__':
    sys.exit(main())
