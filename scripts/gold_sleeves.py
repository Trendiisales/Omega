"""Signal sleeves. Each returns a per-bar target position in {-1, 0, +1}.
Sizing handled by portfolio layer."""
from __future__ import annotations
import numpy as np
import pandas as pd


def _ema(x: pd.Series, span: int) -> pd.Series:
    return x.ewm(span=span, adjust=False, min_periods=span).mean()


def _rsi(x: pd.Series, period: int = 14) -> pd.Series:
    d = x.diff()
    up = d.clip(lower=0).ewm(alpha=1 / period, adjust=False).mean()
    dn = (-d.clip(upper=0)).ewm(alpha=1 / period, adjust=False).mean()
    rs = up / dn.replace(0, np.nan)
    return 100 - 100 / (1 + rs)


def _zscore(x: pd.Series, window: int) -> pd.Series:
    m = x.rolling(window, min_periods=window // 2).mean()
    s = x.rolling(window, min_periods=window // 2).std()
    return (x - m) / s.replace(0, np.nan)


# ---------- Sleeve A: multi-horizon trend (5m bars) ----------
def sleeve_trend(bars: pd.DataFrame, fast: int = 20, slow: int = 96,
                 slope_z_min: float = 0.5, vol_q_max: float = 0.90,
                 entry_cooldown: int = 8) -> pd.Series:
    """Multi-horizon trend. Long when fast>slow AND slow rising AND momentum confirms.
    Cooldown reduces churn. Vol-managed (top-10% RV blocked)."""
    c = bars["close"]
    ef = _ema(c, fast)
    es = _ema(c, slow)
    slope = es.diff(5) / bars["atr"].replace(0, np.nan)
    slope_z = _zscore(slope, 200)
    mom = (c - c.shift(20)) / bars["atr"].replace(0, np.nan)
    rv_cap = bars["rv"].rolling(200, min_periods=50).quantile(vol_q_max)
    long_ok = (ef > es) & (slope_z > slope_z_min) & (mom > 0.5) & (bars["rv"] <= rv_cap)
    short_ok = (ef < es) & (slope_z < -slope_z_min) & (mom < -0.5) & (bars["rv"] <= rv_cap)
    pos = pd.Series(0, index=bars.index, dtype="int8")
    pos[long_ok] = 1
    pos[short_ok] = -1
    return pos.fillna(0).astype("int8")


# ---------- Sleeve B: session pullback continuation ----------
def sleeve_pullback(bars: pd.DataFrame, ema_trend: int = 30, ema_pull: int = 8,
                    rsi_pull_long: float = 48.0, rsi_pull_short: float = 52.0) -> pd.Series:
    """Trade in higher-TF direction during London/NY only.
    Long if close>EMA_trend AND pullback (RSI dips <48 then recovers >50) near EMA_pull.
    Short mirror.  Excludes Asia + NY late."""
    c = bars["close"]
    et = _ema(c, ema_trend)
    ep = _ema(c, ema_pull)
    rsi = _rsi(c, 14)
    rsi_min3 = rsi.rolling(3).min()
    rsi_max3 = rsi.rolling(3).max()
    in_sess = bars["session"].isin(["london", "ny_overlap"])
    near_ep = (c - ep).abs() < bars["atr"] * 0.5
    long_ok = (c > et) & (rsi_min3.shift(1) < rsi_pull_long) & (rsi > 50) & near_ep & in_sess
    short_ok = (c < et) & (rsi_max3.shift(1) > rsi_pull_short) & (rsi < 50) & near_ep & in_sess
    pos = pd.Series(0, index=bars.index, dtype="int8")
    pos[long_ok] = 1
    pos[short_ok] = -1
    return pos.astype("int8")


# ---------- Sleeve C: VWAP mean-reversion, regime-gated ----------
def sleeve_meanrev(bars: pd.DataFrame, z_enter: float = 2.2, lookback: int = 48) -> pd.Series:
    """Fade z-score deviation from rolling VWAP. Only when NOT in a strong trend.
    Filter: |ewm_drift|<1.0 (drift weak) AND regime==0 (calm)."""
    vwap = (bars["close"] * bars["tick_count"]).rolling(lookback, min_periods=10).sum() / \
           bars["tick_count"].rolling(lookback, min_periods=10).sum()
    dev = (bars["close"] - vwap) / bars["close"].rolling(lookback, min_periods=10).std()
    calm = bars["regime"] == 0
    weak_drift = bars["ewm_drift"].abs() < 1.0
    # also block fading when EMA20 strongly slanted (don't fight trends)
    ema_slope = _ema(bars["close"], 20).diff(5) / bars["atr"].replace(0, np.nan)
    no_trend = ema_slope.abs() < 1.0
    long_ok = (dev < -z_enter) & calm & weak_drift & no_trend
    short_ok = (dev > z_enter) & calm & weak_drift & no_trend
    pos = pd.Series(0, index=bars.index, dtype="int8")
    pos[long_ok] = 1
    pos[short_ok] = -1
    return pos.astype("int8")


# ---------- Sleeve D: microstructure (pre-engineered features) ----------
def sleeve_micro(bars: pd.DataFrame, imb_long: float = 0.58, imb_short: float = 0.42,
                 drift_thresh: float = 0.3, vpin_max: float = 0.15) -> pd.Series:
    """Use bar-mean l2_imb + ewm_drift, filtered by vpin (skip toxic).
    Long: imb high AND drift positive AND vpin low. Short mirror."""
    imb = bars["l2_imb"]
    drift = bars["ewm_drift"]
    vpin = bars["vpin"]
    in_sess = bars["session"].isin(["london", "ny_overlap", "ny_late"])
    long_ok = (imb > imb_long) & (drift > drift_thresh) & (vpin < vpin_max) & in_sess
    short_ok = (imb < imb_short) & (drift < -drift_thresh) & (vpin < vpin_max) & in_sess
    pos = pd.Series(0, index=bars.index, dtype="int8")
    pos[long_ok] = 1
    pos[short_ok] = -1
    return pos.astype("int8")


SLEEVE_FNS = {
    "trend": sleeve_trend,
    "pullback": sleeve_pullback,
    "meanrev": sleeve_meanrev,
    "micro": sleeve_micro,
}
