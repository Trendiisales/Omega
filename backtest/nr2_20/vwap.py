#!/usr/bin/env python3
"""
vwap.py
=======

Shared VWAP and HTF-trend utilities used by both nr2_20_backtest.py
(as an optional trend filter) and vwap_continuation_backtest.py
(as the core indicator).

Convention used here:
  - Session VWAP, daily reset at 00:00 UTC. This is the institutional default
    for spot FX/metals. (If you want a per-trading-session reset — Asia /
    London / NY — pass session_reset=True to VwapCalc.)
  - Falls back to bar-tick-count if a bar's volume is 0 (HISTDATA "volume" is
    really tick count for OTC instruments anyway, so this is fine and avoids
    div-by-zero).
  - Typical price = (high + low + close) / 3 (industry standard for VWAP).

Author: Session 2026-05-03
"""

from collections import deque
from datetime import datetime
from typing import List, Optional


# ---------------------------------------------------------------------------
# VWAP calculator (stateful, bar-by-bar)
# ---------------------------------------------------------------------------

class VwapCalc:
    """Cumulative-since-reset VWAP. Updates each bar; resets at the configured
    boundary."""

    def __init__(self, reset: str = "daily"):
        """
        reset: "daily"   -> reset at 00:00 UTC (default)
               "session" -> reset at each Omega session boundary
                            (07:00 LONDON, 13:00 NY, 22:00 ASIA UTC)
               "never"   -> single anchored VWAP from the first bar onward
        """
        if reset not in ("daily", "session", "never"):
            raise ValueError(f"reset must be 'daily'|'session'|'never', got {reset}")
        self._reset = reset
        self._cum_pv: float = 0.0
        self._cum_v: float = 0.0
        self._last_ts: Optional[datetime] = None
        self._vwap_history: deque = deque(maxlen=64)  # for slope calc

    def _is_reset_boundary(self, ts: datetime) -> bool:
        if self._last_ts is None:
            return True   # first bar always anchors
        if self._reset == "never":
            return False
        if self._reset == "daily":
            return ts.date() != self._last_ts.date()
        if self._reset == "session":
            # Reset on transitions across 07/13/22 UTC hour boundaries
            prev_h = self._last_ts.hour
            cur_h = ts.hour
            # Find which session each falls in
            def sess(h: int) -> str:
                if 7 <= h < 13: return "LONDON"
                if 13 <= h < 22: return "NY"
                return "ASIA"
            return sess(prev_h) != sess(cur_h)
        return False

    def update(self, ts: datetime, high: float, low: float, close: float,
               volume: float) -> float:
        """Push one bar, return current VWAP. Volume can be 0 — falls back to 1
        per bar so we still get a series."""
        if self._is_reset_boundary(ts):
            self._cum_pv = 0.0
            self._cum_v = 0.0
        v = volume if volume > 0 else 1.0
        typical = (high + low + close) / 3.0
        self._cum_pv += typical * v
        self._cum_v += v
        vwap = self._cum_pv / self._cum_v if self._cum_v > 0 else close
        self._last_ts = ts
        self._vwap_history.append(vwap)
        return vwap

    def current(self) -> Optional[float]:
        if not self._vwap_history:
            return None
        return self._vwap_history[-1]

    def slope(self, lookback_bars: int = 3) -> int:
        """+1 if VWAP rose over last `lookback_bars` bars, -1 if fell, 0 flat or
        not enough history."""
        if len(self._vwap_history) <= lookback_bars:
            return 0
        prior = self._vwap_history[-(lookback_bars + 1)]
        cur = self._vwap_history[-1]
        if cur > prior:
            return 1
        if cur < prior:
            return -1
        return 0


# ---------------------------------------------------------------------------
# HTF (higher-timeframe) trend helper
# ---------------------------------------------------------------------------

class HtfEma:
    """Aggregates 15m bars into HTF bars by close-of-period sampling, then
    computes an EMA on those HTF closes for trend determination.

    Default htf_minutes=60 → 1H. For 4H pass htf_minutes=240.
    """

    def __init__(self, htf_minutes: int = 60, ema_period: int = 50,
                 slope_lookback: int = 3, base_minutes: int = 15):
        if htf_minutes % base_minutes != 0:
            raise ValueError(
                f"htf_minutes ({htf_minutes}) must be a multiple of "
                f"base_minutes ({base_minutes})"
            )
        self._bars_per_htf = htf_minutes // base_minutes
        self._ema_period = ema_period
        self._slope_lookback = slope_lookback
        self._bar_counter = 0
        self._ema: Optional[float] = None
        self._ema_history: deque = deque(maxlen=slope_lookback + 1)

    def update(self, close: float) -> Optional[float]:
        """Push one base-TF bar's close. Returns current HTF EMA (may be None
        until first HTF bar completes)."""
        self._bar_counter += 1
        if self._bar_counter % self._bars_per_htf == 0:
            # New HTF bar just completed — update EMA on this HTF close
            k = 2.0 / (self._ema_period + 1.0)
            if self._ema is None:
                self._ema = close
            else:
                self._ema = (close - self._ema) * k + self._ema
            self._ema_history.append(self._ema)
        return self._ema

    def trend(self) -> int:
        if len(self._ema_history) <= self._slope_lookback:
            return 0
        prior = self._ema_history[0]
        cur = self._ema_history[-1]
        if cur > prior:
            return 1
        if cur < prior:
            return -1
        return 0


# ---------------------------------------------------------------------------
# Convenience: combined VWAP-side + slope check for trend determination
# ---------------------------------------------------------------------------

def vwap_trend(close: float, vwap: Optional[float], slope: int) -> int:
    """Return +1 / -1 / 0.
      +1: close > vwap AND vwap slope up
      -1: close < vwap AND vwap slope down
       0: otherwise (price on wrong side, or slope flat / disagreeing)
    """
    if vwap is None:
        return 0
    if close > vwap and slope == 1:
        return 1
    if close < vwap and slope == -1:
        return -1
    return 0
