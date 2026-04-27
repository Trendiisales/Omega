"""
regime_diagnostic.py — C6 #1C Step 3 pre-flight: regime vs calendar diagnostic
==============================================================================

Purpose
-------
Step 2's paper-trade simulator (backtest/edgefinder/analytics/fwd_ret_strategy_runner.py)
revealed that the v2 STRONG list contains edges with large post/pre tariff
asymmetry (mean ratio 3.3x for atr_sl, 8.6x for fixed_sl) — much larger than v2's
bracket-framework gates flagged. Two competing hypotheses:

    (H1) calendar-driven: the post-tariff window simply behaves differently and
         the asymmetry is a pure date-of-trade effect. If true, the bleeders
         (pid 81481, pid 64201) are genuinely dead and pid 70823 is genuinely
         TARIFF_DEPENDENT.

    (H2) regime-driven: the post-tariff window is a high-volatility regime, and
         the asymmetry is just trades performing better in high vol. If true,
         the same edges may work pre-tariff if you condition entries on a vol
         threshold — bleeders may be salvageable, 70823 may not need a tariff
         label.

This module runs that diagnostic. It is read-only: it does not modify the
simulator, v1, v2, panel binary, catalogue, OOS sentinel, or any C++ code.

Inputs (read-only)
------------------
    backtest/edgefinder/output/paper_trade/cache/bars_1m.parquet
        (built by fwd_ret_strategy_runner; mtime-keyed to tick CSV)
    backtest/edgefinder/output/paper_trade/fwd_ret_journal.csv
        (Step 2 output: 6936 trades, 17 cols)
    backtest/edgefinder/output/paper_trade/drift_stability_v2_per_edge.csv
        (v2 output: per-edge verdicts, used to identify the STRONG cohort)

Outputs
-------
    backtest/edgefinder/output/paper_trade/regime_diagnostic.md
    backtest/edgefinder/output/paper_trade/regime_diagnostic_table.csv
        vol_decile x subperiod mean pnl_pts table (long format, both vol modes)
    backtest/edgefinder/output/paper_trade/regime_diagnostic_per_edge.csv
        per-(edge_label, vol_decile) breakdown for the v2 STRONG cohort
    backtest/edgefinder/output/paper_trade/regime_diagnostic_journal.csv
        the journal augmented with vol_wall60, vol_bar60, decile_wall, decile_bar
        (re-runnable cache; read by future Step 3a if needed)

Methodology
-----------
For each row in fwd_ret_journal.csv with sl_mode='atr_sl' (3468 rows):
  1. Look up the 1-min bar with ts_close == journal.ts_close (the entry bar).
  2. Compute vol_wall60: std of 1-min log returns (close-to-close) for bars
     whose ts_close is in (entry_ts - 60min, entry_ts]. Require >= 30 bars.
  3. Compute vol_bar60: std of 1-min log returns for the last 60 1-min bars
     in the bar series strictly preceding the entry bar (no calendar window).
  4. Bin into 10 deciles independently for each vol measure across the full
     atr_sl population (NaNs excluded from binning).
  5. Run two pairs of regressions on pnl_pts:
        Model A: pnl_pts ~ subperiod
        Model B_wall: pnl_pts ~ decile_wall + subperiod + decile_wall:subperiod
        Model B_bar : pnl_pts ~ decile_bar  + subperiod + decile_bar:subperiod
  6. Report coefficients, t-stats, R^2, AIC.
  7. Build a 10x2 (decile x subperiod) mean-pnl table for each vol measure.
  8. Per-edge breakdown for the v2 STRONG cohort: for each (pid, side, bracket_id)
     in STRONG, compute mean pnl_pts by decile_bar (ignoring subperiod), so the
     human can see whether the "bleeders" (pid 81481, pid 64201) bleed across
     all vol regimes or only in low vol.

Decision rules (interpreted by the human, not enforced here)
-----------------------------------------------------------
  * If subperiod coefficient retains magnitude/significance after the vol
    decile is added (Model B), the asymmetry is calendar-driven (H1).
  * If subperiod coefficient shrinks substantially (e.g. >50% magnitude
    reduction or p-value moves above 0.10), the asymmetry is regime-driven (H2).
  * If partial, both effects coexist; report and let human decide.

Implementation notes
--------------------
  * Uses pandas + numpy + polars + statsmodels. statsmodels is the only soft
    dep; if missing, the module falls back to a hand-rolled OLS using numpy
    (gives the same coefficients and t-stats; AIC is computed by hand).
  * Vol windowing uses a binary-search on the bar ts_close array — O(log N)
    per trade, total ~100ms for 3468 trades on a typical Mac.
  * The bar series is sorted ascending by ts_close. The simulator already
    enforces this when writing the parquet cache.
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
import polars as pl


# -----------------------------------------------------------------------------
# Defaults — match Step 2 simulator paths exactly
# -----------------------------------------------------------------------------
DEFAULT_OUT_DIR = 'backtest/edgefinder/output/paper_trade'
DEFAULT_JOURNAL = f'{DEFAULT_OUT_DIR}/fwd_ret_journal.csv'
DEFAULT_V2_PER_EDGE = f'{DEFAULT_OUT_DIR}/drift_stability_v2_per_edge.csv'
DEFAULT_BARS_1M = f'{DEFAULT_OUT_DIR}/cache/bars_1m.parquet'
DEFAULT_TARIFF_CUTOFF = '2026-04-02T00:00:00Z'

VOL_WINDOW_MIN = 60          # wall-clock window for vol_wall60
VOL_WINDOW_BARS = 60         # bar-count window for vol_bar60
MIN_BARS_WALL = 30           # min bars in wall-clock window else NaN
N_DECILES = 10


def _log(msg: str) -> None:
    print(msg, flush=True)


# -----------------------------------------------------------------------------
# IO
# -----------------------------------------------------------------------------
def load_bars_1m(path: Path) -> pd.DataFrame:
    """Load 1-min OHLC bars built by fwd_ret_strategy_runner.

    Schema (from build_minute_bars):
        ts_open, ts_close (datetime[ms, UTC]), open, high, low, close (Float64),
        n_ticks (UInt32)

    Returns a pandas DataFrame sorted by ts_close ascending, indexed 0..N-1.
    """
    _log(f"loading 1m bars: {path}")
    df = pl.read_parquet(path).to_pandas()
    if 'ts_close' not in df.columns or 'close' not in df.columns:
        raise RuntimeError(
            f"bars_1m parquet missing required cols; got {list(df.columns)}"
        )
    # Make ts_close tz-aware UTC pandas Timestamps (polars -> pandas can be
    # tz-aware already, but we normalise defensively).
    ts = pd.to_datetime(df['ts_close'], utc=True)
    df = df.assign(ts_close=ts).sort_values('ts_close').reset_index(drop=True)
    _log(f"  {len(df):,} bars, {df['ts_close'].iloc[0]} ... "
         f"{df['ts_close'].iloc[-1]}")
    return df


def load_journal(path: Path) -> pd.DataFrame:
    """Load Step 2 journal. Schema (from _journal_row in fwd_ret_strategy_runner):
        ts_close, rank, pid, side, bracket_id, edge_label, sl_mode, sl_pts,
        entry_price, atr_at_entry, exit_idx, ts_exit, exit_price, exit_reason,
        holding_bars, pnl_pts, subperiod
    """
    _log(f"loading journal: {path}")
    df = pd.read_csv(path)
    required = {'ts_close', 'pid', 'side', 'bracket_id', 'sl_mode',
                'pnl_pts', 'subperiod', 'exit_reason'}
    missing = required - set(df.columns)
    if missing:
        raise RuntimeError(f"journal missing cols: {sorted(missing)}")
    df['ts_close'] = pd.to_datetime(df['ts_close'], utc=True)
    _log(f"  {len(df):,} rows; sl_mode counts: "
         f"{df['sl_mode'].value_counts().to_dict()}")
    return df


def load_v2_strong(path: Path) -> pd.DataFrame:
    """Load v2 per-edge verdicts and return the STRONG cohort.

    Schema (from inspect_drift_stability_v2.assign_v2_verdicts col_order):
        rank, pid, side, bracket_id, ..., verdict_v1, sharpe_oos, cost_ok,
        sharpe_ok, n_pre, n_post, mean_pre, mean_post, sharpe_pre, sharpe_post,
        t_pre, t_post, sign_pre, sign_post, sub_periods_consistent,
        post_dominates, verdict_v2
    """
    _log(f"loading v2 per-edge verdicts: {path}")
    df = pd.read_csv(path)
    if 'verdict_v2' not in df.columns:
        raise RuntimeError(f"v2 per-edge missing 'verdict_v2'; got "
                           f"{list(df.columns)}")
    strong = df[df['verdict_v2'] == 'STRONG'].copy()
    _log(f"  {len(df)} edges total, {len(strong)} STRONG")
    return strong


# -----------------------------------------------------------------------------
# Vol computation
# -----------------------------------------------------------------------------
def _compute_log_returns(close: np.ndarray) -> np.ndarray:
    """1-min close-to-close log returns. Length = len(close); first = NaN."""
    out = np.full(len(close), np.nan, dtype=np.float64)
    if len(close) >= 2:
        out[1:] = np.log(close[1:] / close[:-1])
    return out


def compute_vols_for_journal(
    journal: pd.DataFrame,
    bars: pd.DataFrame,
) -> pd.DataFrame:
    """Add vol_wall60 and vol_bar60 columns to journal.

    Returns the journal with two new float cols. Trades whose entry ts_close
    cannot be found in the bar series (should not happen for atr_sl trades
    that the simulator emitted) get NaN. Trades with insufficient prior bars
    get NaN.

    The bar 'close' here is mid-price close (per build_minute_bars).
    """
    _log("computing vol_wall60 and vol_bar60")
    bars_ts = bars['ts_close'].values  # numpy datetime64[ns, UTC] from pandas
    bars_close = bars['close'].values.astype(np.float64)
    log_rets = _compute_log_returns(bars_close)

    # Sanity: bars_ts should be sorted ascending.
    if len(bars_ts) >= 2 and not (np.diff(bars_ts.astype('datetime64[ns]')
                                           ).astype(np.int64) >= 0).all():
        raise RuntimeError("bars_1m not sorted ascending by ts_close")

    j_ts = journal['ts_close'].values  # already tz-aware UTC

    # We need to map each journal ts_close to its index in bars_ts.
    # bars_ts and j_ts may have different tz handling at the numpy level —
    # convert both to int64 ns since epoch for searchsorted.
    bars_ts_ns = pd.to_datetime(bars['ts_close'], utc=True).astype(
        'datetime64[ns, UTC]').astype('int64').values
    j_ts_ns = pd.to_datetime(journal['ts_close'], utc=True).astype(
        'datetime64[ns, UTC]').astype('int64').values

    # For each journal entry, find the bar index with ts_close == entry ts.
    # searchsorted with side='left' gives the leftmost match.
    idx = np.searchsorted(bars_ts_ns, j_ts_ns, side='left')

    n_bars = len(bars_ts_ns)
    vol_wall = np.full(len(journal), np.nan, dtype=np.float64)
    vol_bar = np.full(len(journal), np.nan, dtype=np.float64)

    window_ns_60 = int(VOL_WINDOW_MIN) * 60 * 1_000_000_000
    n_matched = 0
    n_unmatched = 0

    for i in range(len(journal)):
        k = int(idx[i])
        if k >= n_bars or bars_ts_ns[k] != j_ts_ns[i]:
            # Entry bar not in the bar series — should be rare. Skip with NaN.
            n_unmatched += 1
            continue
        n_matched += 1

        # vol_bar60: last VOL_WINDOW_BARS log returns ending at bar k.
        # log_rets[k] is the return INTO bar k (from k-1 to k). To get the
        # vol of returns OBSERVED BEFORE the entry, we use returns at indices
        # (k - VOL_WINDOW_BARS, ..., k-1] — i.e. the last 60 returns up to and
        # including the return that produced the entry bar's close. This is
        # the strictest "observed before entry" interpretation; using k itself
        # would include the entry bar's own close-to-close move.
        lo_b = k - VOL_WINDOW_BARS
        hi_b = k  # exclusive
        if lo_b >= 1 and hi_b - lo_b == VOL_WINDOW_BARS:
            seg = log_rets[lo_b:hi_b]
            seg = seg[~np.isnan(seg)]
            if len(seg) >= 2:
                vol_bar[i] = float(np.std(seg, ddof=1))

        # vol_wall60: returns whose bar index has ts_close in
        # (entry_ts - 60min, entry_ts). Strictly less than entry_ts to keep
        # the entry bar itself out of the window. Allow last 30+ bars.
        lo_ts = j_ts_ns[i] - window_ns_60
        # Find leftmost bar with ts_close > lo_ts (i.e. ts_close in the window
        # is > lo_ts), and rightmost bar with ts_close < entry_ts.
        lo_w = np.searchsorted(bars_ts_ns, lo_ts, side='right')
        hi_w = k  # exclusive — bar k is the entry bar
        if hi_w - lo_w >= MIN_BARS_WALL:
            # Returns observed in this window: log_rets[lo_w : hi_w].
            # log_rets[lo_w] is the return INTO bar lo_w from bar lo_w - 1.
            # That return covers a price move that ended at ts_close[lo_w],
            # which is > lo_ts by construction. Acceptable.
            seg = log_rets[lo_w:hi_w]
            seg = seg[~np.isnan(seg)]
            if len(seg) >= 2:
                vol_wall[i] = float(np.std(seg, ddof=1))

    _log(f"  matched {n_matched}/{len(journal)} entries to bars "
         f"({n_unmatched} unmatched -> NaN vol)")
    nan_wall = int(np.isnan(vol_wall).sum())
    nan_bar = int(np.isnan(vol_bar).sum())
    _log(f"  vol_wall60: {len(journal) - nan_wall} valid, {nan_wall} NaN")
    _log(f"  vol_bar60 : {len(journal) - nan_bar} valid, {nan_bar} NaN")

    out = journal.copy()
    out['vol_wall60'] = vol_wall
    out['vol_bar60'] = vol_bar
    return out


def assign_deciles(values: np.ndarray) -> np.ndarray:
    """Assign 1..N_DECILES decile ranks. NaN -> NaN. Ties broken by rank()."""
    out = np.full(len(values), np.nan, dtype=np.float64)
    mask = ~np.isnan(values)
    if mask.sum() == 0:
        return out
    s = pd.Series(values[mask])
    # qcut returns a Categorical; convert to int 1..10.
    try:
        cats = pd.qcut(s.rank(method='first'), q=N_DECILES, labels=False)
    except ValueError:
        # Fallback: not enough unique values for 10 deciles.
        cats = pd.qcut(s.rank(method='first'), q=N_DECILES, labels=False,
                       duplicates='drop')
    out[mask] = cats.values.astype(np.float64) + 1.0  # 1..10
    return out


# -----------------------------------------------------------------------------
# Regression — statsmodels with numpy fallback
# -----------------------------------------------------------------------------
def _try_import_statsmodels():
    try:
        import statsmodels.api as sm  # noqa
        return sm
    except Exception:
        return None


def _ols_numpy(y: np.ndarray, X: np.ndarray, names: list[str]) -> dict:
    """Hand-rolled OLS with HC0 robust SE. Returns coefs, SE, t, p, R2, AIC.
    No statsmodels dependency.
    """
    n, k = X.shape
    XtX = X.T @ X
    XtX_inv = np.linalg.pinv(XtX)
    beta = XtX_inv @ X.T @ y
    yhat = X @ beta
    resid = y - yhat
    rss = float(resid @ resid)
    tss = float(((y - y.mean()) ** 2).sum())
    r2 = 1.0 - rss / tss if tss > 0 else float('nan')

    # HC0 (White) robust covariance
    u2 = resid ** 2
    meat = (X * u2[:, None]).T @ X
    cov_hc0 = XtX_inv @ meat @ XtX_inv
    se = np.sqrt(np.maximum(np.diag(cov_hc0), 0.0))
    t = beta / np.where(se > 0, se, np.nan)

    # Two-sided p-values from normal approx (robust SE doesn't have nice df)
    # erfc-based approximation
    from math import erfc, sqrt as _sqrt
    p = np.array([erfc(abs(ti) / _sqrt(2.0)) if np.isfinite(ti) else np.nan
                  for ti in t])

    # AIC: assume Gaussian; sigma2_hat = rss/n
    sigma2 = rss / n if n > 0 else float('nan')
    if sigma2 > 0:
        ll = -0.5 * n * (math.log(2 * math.pi * sigma2) + 1.0)
    else:
        ll = float('nan')
    aic = 2 * k - 2 * ll if math.isfinite(ll) else float('nan')

    return {
        'names': names,
        'coef': beta.tolist(),
        'se': se.tolist(),
        't': t.tolist(),
        'p': p.tolist(),
        'r2': float(r2),
        'aic': float(aic),
        'n': int(n),
        'k': int(k),
    }


def fit_model_a(df: pd.DataFrame, sm) -> dict:
    """pnl_pts ~ subperiod (post=1, pre=0). Constant + post dummy."""
    sub = df.dropna(subset=['pnl_pts', 'subperiod']).copy()
    y = sub['pnl_pts'].values.astype(np.float64)
    post = (sub['subperiod'] == 'post').astype(np.float64).values
    X = np.column_stack([np.ones_like(post), post])
    names = ['const', 'post']
    if sm is not None:
        m = sm.OLS(y, X).fit(cov_type='HC0')
        return {
            'names': names,
            'coef': m.params.tolist(),
            'se': m.bse.tolist(),
            't': m.tvalues.tolist(),
            'p': m.pvalues.tolist(),
            'r2': float(m.rsquared),
            'aic': float(m.aic),
            'n': int(m.nobs),
            'k': int(m.df_model + 1),
        }
    return _ols_numpy(y, X, names)


def fit_model_b(df: pd.DataFrame, decile_col: str, sm) -> dict:
    """pnl_pts ~ decile + post + decile:post. Decile in [1,10] used as numeric.
    """
    sub = df.dropna(subset=['pnl_pts', 'subperiod', decile_col]).copy()
    y = sub['pnl_pts'].values.astype(np.float64)
    dec = sub[decile_col].values.astype(np.float64)
    post = (sub['subperiod'] == 'post').astype(np.float64).values
    inter = dec * post
    X = np.column_stack([np.ones_like(post), dec, post, inter])
    names = ['const', decile_col, 'post', f'{decile_col}:post']
    if sm is not None:
        m = sm.OLS(y, X).fit(cov_type='HC0')
        return {
            'names': names,
            'coef': m.params.tolist(),
            'se': m.bse.tolist(),
            't': m.tvalues.tolist(),
            'p': m.pvalues.tolist(),
            'r2': float(m.rsquared),
            'aic': float(m.aic),
            'n': int(m.nobs),
            'k': int(m.df_model + 1),
        }
    return _ols_numpy(y, X, names)


# -----------------------------------------------------------------------------
# Tabulation
# -----------------------------------------------------------------------------
def vol_decile_subperiod_table(
    df: pd.DataFrame,
    decile_col: str,
) -> pd.DataFrame:
    """Mean pnl_pts and count by (decile, subperiod). Long format with one
    row per (decile, subperiod) cell.
    """
    sub = df.dropna(subset=['pnl_pts', 'subperiod', decile_col]).copy()
    sub[decile_col] = sub[decile_col].astype(int)
    grp = sub.groupby([decile_col, 'subperiod'])['pnl_pts'].agg(
        ['mean', 'count', 'sum']).reset_index()
    grp = grp.rename(columns={'mean': 'mean_pnl', 'count': 'n', 'sum': 'sum_pnl',
                              decile_col: 'decile'})
    grp['vol_measure'] = decile_col
    return grp[['vol_measure', 'decile', 'subperiod', 'n', 'mean_pnl',
                'sum_pnl']]


def per_edge_decile_breakdown(
    df: pd.DataFrame,
    strong: pd.DataFrame,
    decile_col: str,
) -> pd.DataFrame:
    """For each STRONG edge (pid, side, bracket_id), mean pnl_pts by decile,
    pooling pre and post. Output is long format.
    """
    keys = strong[['pid', 'side', 'bracket_id']].drop_duplicates()
    sub = df.merge(keys, on=['pid', 'side', 'bracket_id'], how='inner')
    sub = sub.dropna(subset=['pnl_pts', decile_col]).copy()
    sub[decile_col] = sub[decile_col].astype(int)
    grp = sub.groupby(['pid', 'side', 'bracket_id', decile_col])[
        'pnl_pts'].agg(['mean', 'count', 'sum']).reset_index()
    grp = grp.rename(columns={'mean': 'mean_pnl', 'count': 'n',
                              'sum': 'sum_pnl', decile_col: 'decile'})
    grp['vol_measure'] = decile_col
    return grp[['vol_measure', 'pid', 'side', 'bracket_id', 'decile', 'n',
                'mean_pnl', 'sum_pnl']]


# -----------------------------------------------------------------------------
# MD report
# -----------------------------------------------------------------------------
def _fmt_float(x: float, w: int = 8, prec: int = 4) -> str:
    if x is None or (isinstance(x, float) and not np.isfinite(x)):
        return f"{'nan':>{w}}"
    return f"{x:>+{w}.{prec}f}"


def _fmt_pval(p: float) -> str:
    if p is None or not np.isfinite(p):
        return '   nan'
    if p < 1e-4:
        return '<1e-4'
    return f"{p:.4f}"


def _fmt_model(label: str, m: dict) -> list[str]:
    out = [f"### {label}", "",
           f"n={m['n']}, k={m['k']}, R^2={m['r2']:.6f}, AIC={m['aic']:.2f}",
           "",
           "| term            |     coef |       SE |       t |       p |",
           "|-----------------|---------:|---------:|--------:|--------:|"]
    for name, c, s, t, p in zip(m['names'], m['coef'], m['se'], m['t'],
                                m['p']):
        out.append(
            f"| {name:<15} | {_fmt_float(c)} | {_fmt_float(s)} | "
            f"{_fmt_float(t, 7, 3)} | {_fmt_pval(p):>7} |"
        )
    out.append("")
    return out


def _fmt_decile_table(table: pd.DataFrame, vol_measure: str) -> list[str]:
    sub = table[table['vol_measure'] == vol_measure].copy()
    if sub.empty:
        return [f"### {vol_measure} — no data", ""]
    pre = sub[sub['subperiod'] == 'pre'].set_index('decile')
    post = sub[sub['subperiod'] == 'post'].set_index('decile')
    deciles = sorted(set(pre.index) | set(post.index))
    lines = [f"### {vol_measure} — mean pnl_pts by decile x subperiod", "",
             "| decile | n_pre | mean_pre  | n_post | mean_post | post-pre  |",
             "|-------:|------:|----------:|-------:|----------:|----------:|"]
    for d in deciles:
        n_pre = int(pre.loc[d, 'n']) if d in pre.index else 0
        m_pre = float(pre.loc[d, 'mean_pnl']) if d in pre.index else float(
            'nan')
        n_post = int(post.loc[d, 'n']) if d in post.index else 0
        m_post = float(post.loc[d, 'mean_pnl']) if d in post.index else float(
            'nan')
        delta = (m_post - m_pre) if (np.isfinite(m_pre) and np.isfinite(m_post)
                                     ) else float('nan')
        lines.append(
            f"| {d:>6d} | {n_pre:>5d} | {_fmt_float(m_pre, 9, 3)} | "
            f"{n_post:>6d} | {_fmt_float(m_post, 9, 3)} | "
            f"{_fmt_float(delta, 9, 3)} |"
        )
    lines.append("")
    return lines


def _fmt_per_edge(per_edge: pd.DataFrame, vol_measure: str,
                  strong: pd.DataFrame) -> list[str]:
    sub = per_edge[per_edge['vol_measure'] == vol_measure].copy()
    if sub.empty:
        return [f"### per-edge ({vol_measure}) — no data", ""]
    lines = [f"### per-edge mean pnl_pts by decile ({vol_measure})", ""]
    keys = strong[['pid', 'side', 'bracket_id']].drop_duplicates(
    ).sort_values(['pid', 'side', 'bracket_id'])
    for _, k in keys.iterrows():
        edge_rows = sub[(sub['pid'] == k['pid']) &
                        (sub['side'] == k['side']) &
                        (sub['bracket_id'] == k['bracket_id'])]
        lines.append(f"**pid {int(k['pid'])} {k['side']} "
                     f"bracket {int(k['bracket_id'])}**")
        lines.append("")
        if edge_rows.empty:
            lines.append("(no decile data — all NaN)")
            lines.append("")
            continue
        lines.append("| decile |   n | mean_pnl | sum_pnl |")
        lines.append("|-------:|----:|---------:|--------:|")
        for _, r in edge_rows.sort_values('decile').iterrows():
            lines.append(
                f"| {int(r['decile']):>6d} | {int(r['n']):>3d} | "
                f"{_fmt_float(r['mean_pnl'], 8, 3)} | "
                f"{_fmt_float(r['sum_pnl'], 8, 2)} |"
            )
        lines.append("")
    return lines


def _interpretation_block(model_a: dict, model_b_wall: dict,
                          model_b_bar: dict) -> list[str]:
    """Auto-summarise the regression result. Pure description; no decisions."""
    def _post_coef(m: dict) -> tuple[float, float, float]:
        if 'post' in m['names']:
            i = m['names'].index('post')
            return m['coef'][i], m['t'][i], m['p'][i]
        return float('nan'), float('nan'), float('nan')

    a_c, a_t, a_p = _post_coef(model_a)
    bw_c, bw_t, bw_p = _post_coef(model_b_wall)
    bb_c, bb_t, bb_p = _post_coef(model_b_bar)

    def _attenuation(a: float, b: float) -> str:
        if not (np.isfinite(a) and np.isfinite(b)) or a == 0:
            return 'n/a'
        ratio = b / a
        return f"{ratio*100:.1f}% retained ({(1-ratio)*100:+.1f}% change)"

    lines = [
        "## interpretation summary",
        "",
        f"**Model A (calendar only)** post coefficient: {a_c:+.4f} "
        f"(t={a_t:+.3f}, p={_fmt_pval(a_p).strip()})",
        "",
        f"**Model B with vol_wall60** post coefficient: {bw_c:+.4f} "
        f"(t={bw_t:+.3f}, p={_fmt_pval(bw_p).strip()}); "
        f"attenuation vs A: {_attenuation(a_c, bw_c)}",
        "",
        f"**Model B with vol_bar60** post coefficient: {bb_c:+.4f} "
        f"(t={bb_t:+.3f}, p={_fmt_pval(bb_p).strip()}); "
        f"attenuation vs A: {_attenuation(a_c, bb_c)}",
        "",
        "Decision rules (interpreted by human):",
        "",
        "* If post coefficient retains >70% magnitude AND remains significant "
        "(p<0.05) in Model B: **calendar-driven** (H1) — proceed Step 3a as "
        "scoped, bleeders stay CULL, pid 70823 stays TARIFF_DEPENDENT.",
        "* If post coefficient drops to <30% magnitude OR loses significance "
        "(p>0.10): **regime-driven** (H2) — Step 3a should add a "
        "vol-conditional gate as primary framework; bleeders may survive with "
        "vol threshold; pid 70823 may not need TARIFF_DEPENDENT label.",
        "* Mixed (30-70% retention): both effects coexist; recommend "
        "calendar gate as conservative default, with vol gate as overlay.",
        "",
    ]
    return lines


def write_md(
    out_path: Path,
    model_a: dict,
    model_b_wall: dict,
    model_b_bar: dict,
    table: pd.DataFrame,
    per_edge: pd.DataFrame,
    strong: pd.DataFrame,
    n_journal: int,
    n_atr_sl: int,
    n_atr_sl_with_vol_wall: int,
    n_atr_sl_with_vol_bar: int,
    sm_available: bool,
) -> None:
    lines = []
    lines.append("# C6 #1C Step 3 pre-flight — regime vs calendar diagnostic")
    lines.append("")
    lines.append("Tests whether the post-tariff outperformance observed by "
                 "Step 2's paper-trade simulator is **calendar-driven** "
                 "(H1) or **regime-driven** (H2), by adding entry-time "
                 "realized volatility as a control.")
    lines.append("")
    lines.append("**Two vol measures reported side-by-side:**")
    lines.append("")
    lines.append("* `vol_wall60`: std of 1-min log returns over last 60 "
                 "wall-clock minutes ending strictly before entry "
                 f"(min {MIN_BARS_WALL} bars else NaN).")
    lines.append("* `vol_bar60`: std of 1-min log returns over last "
                 f"{VOL_WINDOW_BARS} traded 1-min bars ending strictly before "
                 "entry (no calendar gap handling).")
    lines.append("")
    lines.append(f"Regression engine: "
                 f"{'statsmodels OLS, HC0 robust SE' if sm_available else 'numpy OLS, HC0 robust SE (statsmodels unavailable)'}.")
    lines.append("")
    lines.append("## inputs")
    lines.append("")
    lines.append(f"* journal: {n_journal} rows total, {n_atr_sl} atr_sl rows.")
    lines.append(f"* atr_sl trades with vol_wall60: {n_atr_sl_with_vol_wall}.")
    lines.append(f"* atr_sl trades with vol_bar60 : {n_atr_sl_with_vol_bar}.")
    lines.append(f"* v2 STRONG cohort: {len(strong)} edges.")
    lines.append("")

    lines.append("## regressions")
    lines.append("")
    lines.extend(_fmt_model("Model A — pnl_pts ~ post", model_a))
    lines.extend(_fmt_model("Model B (wall) — pnl_pts ~ decile_wall + post + decile_wall:post",
                            model_b_wall))
    lines.extend(_fmt_model("Model B (bar) — pnl_pts ~ decile_bar + post + decile_bar:post",
                            model_b_bar))

    lines.extend(_interpretation_block(model_a, model_b_wall, model_b_bar))

    lines.append("## decile x subperiod tables")
    lines.append("")
    lines.extend(_fmt_decile_table(table, 'decile_wall'))
    lines.extend(_fmt_decile_table(table, 'decile_bar'))

    lines.append("## per-edge breakdown (v2 STRONG cohort)")
    lines.append("")
    lines.append("Mean pnl_pts pooled across pre and post, broken by decile. "
                 "If a 'bleeder' edge has positive mean in high deciles, the "
                 "edge may be salvageable via a vol-threshold entry filter.")
    lines.append("")
    lines.extend(_fmt_per_edge(per_edge, 'decile_bar', strong))

    out_path.write_text("\n".join(lines))
    _log(f"wrote {out_path}")


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
def main() -> int:
    p = argparse.ArgumentParser(
        description="C6 #1C Step 3 pre-flight regime diagnostic.",
    )
    p.add_argument('--journal', default=DEFAULT_JOURNAL,
                   help=f"path to fwd_ret_journal.csv (default {DEFAULT_JOURNAL})")
    p.add_argument('--v2-per-edge', default=DEFAULT_V2_PER_EDGE,
                   help=f"path to drift_stability_v2_per_edge.csv "
                        f"(default {DEFAULT_V2_PER_EDGE})")
    p.add_argument('--bars-1m', default=DEFAULT_BARS_1M,
                   help=f"path to bars_1m.parquet (default {DEFAULT_BARS_1M})")
    p.add_argument('--out-dir', default=DEFAULT_OUT_DIR,
                   help=f"output dir (default {DEFAULT_OUT_DIR})")
    p.add_argument('--tariff-cutoff', default=DEFAULT_TARIFF_CUTOFF,
                   help="ISO timestamp; trades with ts_close >= cutoff are "
                        "tagged 'post'. Should match what was used for "
                        "fwd_ret_journal.csv. Used here only for sanity check.")
    args = p.parse_args()

    journal_path = Path(args.journal)
    v2_path = Path(args.v2_per_edge)
    bars_path = Path(args.bars_1m)
    out_dir = Path(args.out_dir)

    for f in [journal_path, v2_path, bars_path]:
        if not f.is_file():
            print(f"ERROR: missing input: {f}", file=sys.stderr)
            return 2
    out_dir.mkdir(parents=True, exist_ok=True)

    # 1. Load
    bars = load_bars_1m(bars_path)
    journal = load_journal(journal_path)
    strong = load_v2_strong(v2_path)

    # 2. Restrict to atr_sl (primary framework per Step 2)
    atr_sl = journal[journal['sl_mode'] == 'atr_sl'].reset_index(drop=True)
    _log(f"atr_sl subset: {len(atr_sl)} rows")

    # 3. Compute vols
    aug = compute_vols_for_journal(atr_sl, bars)

    # 4. Assign deciles
    aug['decile_wall'] = assign_deciles(aug['vol_wall60'].values)
    aug['decile_bar'] = assign_deciles(aug['vol_bar60'].values)
    n_with_wall = int(aug['decile_wall'].notna().sum())
    n_with_bar = int(aug['decile_bar'].notna().sum())
    _log(f"deciles assigned: wall={n_with_wall}, bar={n_with_bar}")

    # 5. Persist augmented journal (cache for any follow-up Step 3a work)
    aug_path = out_dir / 'regime_diagnostic_journal.csv'
    aug.to_csv(aug_path, index=False)
    _log(f"wrote {aug_path}")

    # 6. Regressions
    sm = _try_import_statsmodels()
    sm_available = sm is not None
    _log(f"regression backend: {'statsmodels' if sm_available else 'numpy'}")
    model_a = fit_model_a(aug, sm)
    model_b_wall = fit_model_b(aug, 'decile_wall', sm)
    model_b_bar = fit_model_b(aug, 'decile_bar', sm)

    # 7. Tables
    tab_wall = vol_decile_subperiod_table(aug, 'decile_wall')
    tab_bar = vol_decile_subperiod_table(aug, 'decile_bar')
    table = pd.concat([tab_wall, tab_bar], ignore_index=True)
    table_path = out_dir / 'regime_diagnostic_table.csv'
    table.to_csv(table_path, index=False)
    _log(f"wrote {table_path}")

    # 8. Per-edge breakdown (using bar deciles as primary)
    per_edge_wall = per_edge_decile_breakdown(aug, strong, 'decile_wall')
    per_edge_bar = per_edge_decile_breakdown(aug, strong, 'decile_bar')
    per_edge = pd.concat([per_edge_wall, per_edge_bar], ignore_index=True)
    per_edge_path = out_dir / 'regime_diagnostic_per_edge.csv'
    per_edge.to_csv(per_edge_path, index=False)
    _log(f"wrote {per_edge_path}")

    # 9. MD report
    md_path = out_dir / 'regime_diagnostic.md'
    write_md(
        out_path=md_path,
        model_a=model_a,
        model_b_wall=model_b_wall,
        model_b_bar=model_b_bar,
        table=table,
        per_edge=per_edge,
        strong=strong,
        n_journal=len(journal),
        n_atr_sl=len(aug),
        n_atr_sl_with_vol_wall=n_with_wall,
        n_atr_sl_with_vol_bar=n_with_bar,
        sm_available=sm_available,
    )

    # Summary to stdout
    def _post_coef(m: dict) -> tuple[float, float]:
        if 'post' in m['names']:
            i = m['names'].index('post')
            return m['coef'][i], m['p'][i]
        return float('nan'), float('nan')
    a_c, a_p = _post_coef(model_a)
    bw_c, bw_p = _post_coef(model_b_wall)
    bb_c, bb_p = _post_coef(model_b_bar)
    _log("")
    _log("=== regime diagnostic summary ===")
    _log(f"Model A     post: coef={a_c:+.4f}  p={a_p:.4f}")
    _log(f"Model B/wall post: coef={bw_c:+.4f}  p={bw_p:.4f}  "
         f"({(bw_c/a_c*100 if a_c else float('nan')):.1f}% of A)")
    _log(f"Model B/bar  post: coef={bb_c:+.4f}  p={bb_p:.4f}  "
         f"({(bb_c/a_c*100 if a_c else float('nan')):.1f}% of A)")
    _log("=================================")
    return 0


if __name__ == '__main__':
    sys.exit(main())
