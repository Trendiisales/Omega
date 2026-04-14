#!/usr/bin/env python3
"""
CFE Backtest — CandleFlowEngine full faithful reimplementation in Python.
Runs against 2yr_XAUUSD_tick.csv (111M rows).

Usage:
    python3 cfe_backtest.py --data ~/Tick/2yr_XAUUSD_tick.csv
    python3 cfe_backtest.py --data ~/Tick/2yr_XAUUSD_tick.csv --start 2024-01-01 --end 2024-06-01
    python3 cfe_backtest.py --data ~/Tick/2yr_XAUUSD_tick.csv --no-gates   # baseline without filters

Output:
    cfe_backtest_trades.csv   — every trade
    cfe_backtest_report.txt   — full stats report
    cfe_backtest_equity.csv   — equity curve (daily)
"""

import csv
import sys
import math
import datetime
import argparse
import os
from collections import deque
from dataclasses import dataclass, field
from typing import Optional

# ══════════════════════════════════════════════════════════════════════════════
# Constants — exact mirror of CandleFlowEngine.hpp
# ══════════════════════════════════════════════════════════════════════════════

CFE_BODY_RATIO_MIN          = 0.60
CFE_COST_SLIPPAGE           = 0.10
CFE_COMMISSION_PTS          = 0.10
CFE_COST_MULT               = 2.0
CFE_STAGNATION_MS           = 90_000     # Asia default; London/NY = 180_000
CFE_STAGNATION_MULT         = 1.0
CFE_RISK_DOLLARS            = 30.0
CFE_MIN_LOT                 = 0.01
CFE_MAX_LOT                 = 0.20
CFE_RSI_PERIOD              = 30
CFE_RSI_EMA_N               = 10
CFE_RSI_THRESH              = 6.0
CFE_DFE_RSI_LEVEL_LONG_MIN  = 35.0
CFE_DFE_RSI_LEVEL_SHORT_MAX = 65.0
CFE_DFE_DRIFT_PERSIST_TICKS = 2
CFE_DFE_DRIFT_SUSTAINED_THRESH = 0.8
CFE_DFE_DRIFT_SUSTAINED_MS  = 45_000
CFE_BAR_TREND_BLOCK_DRIFT   = 0.5
CFE_BAR_TREND_BLOCK_MS      = 45_000
CFE_DFE_PRICE_CONFIRM_TICKS = 3
CFE_DFE_PRICE_CONFIRM_MIN   = 0.05
CFE_IMB_EXIT_THRESH         = 0.08
CFE_IMB_EXIT_TICKS          = 2
CFE_IMB_MIN_HOLD_MS         = 20_000
CFE_DFE_DRIFT_THRESH        = 1.5
CFE_DFE_DRIFT_ACCEL         = 0.2
CFE_DFE_RSI_THRESH          = 3.0
CFE_DFE_RSI_TREND_MAX       = 12.0
CFE_DFE_SL_MULT             = 0.7
CFE_MAX_ATR_ENTRY           = 6.0
CFE_DFE_COOLDOWN_MS         = 120_000
CFE_DFE_MIN_SPREAD_MULT     = 1.5
CFE_OPPOSITE_DIR_COOLDOWN_MS = 60_000
CFE_WINNER_COOLDOWN_MS      = 30_000

# ATR: 20-bar EMA of (high-low) on M1 bars
ATR_PERIOD                  = 20

# EWM drift: span=300 ticks (~30s at London tick rate)
EWM_DRIFT_SPAN              = 300
EWM_DRIFT_ALPHA             = 2.0 / (EWM_DRIFT_SPAN + 1)

# M1 bar aggregation
M1_MS                       = 60_000

# VWAP: daily reset cumulative
# chop gate
CHOP_VOL_RATIO_THRESH       = 1.2
CHOP_DRIFT_ABS_THRESH       = 1.0

# ══════════════════════════════════════════════════════════════════════════════
# Data structures
# ══════════════════════════════════════════════════════════════════════════════

@dataclass
class Bar:
    ts_ms:  int   = 0
    open:   float = 0.0
    high:   float = 0.0
    low:    float = 0.0
    close:  float = 0.0
    valid:  bool  = False

@dataclass
class OpenPos:
    active:       bool  = False
    is_long:      bool  = False
    entry:        float = 0.0
    sl:           float = 0.0
    trail_sl:     float = 0.0
    size:         float = 0.0
    full_size:    float = 0.0
    cost_pts:     float = 0.0
    entry_ts_ms:  int   = 0
    mfe:          float = 0.0
    trail_active: bool  = False
    partial_done: bool  = False
    atr_pts:      float = 0.0

@dataclass
class Trade:
    id:          int
    side:        str
    entry:       float
    exit_px:     float
    sl:          float
    size:        float
    pnl_pts:     float
    pnl_usd:     float
    entry_ts_ms: int
    exit_ts_ms:  int
    exit_reason: str
    hold_ms:     int
    mfe:         float
    entry_type:  str   # 'BAR' or 'DFE' or 'SUS'

# ══════════════════════════════════════════════════════════════════════════════
# Indicator helpers
# ══════════════════════════════════════════════════════════════════════════════

class RSICalc:
    """Tick-level RSI + slope EMA — mirrors CandleFlowEngine RSI logic."""
    def __init__(self):
        self.gains    = deque()
        self.losses   = deque()
        self.prev_mid = 0.0
        self.cur      = 50.0
        self.prev     = 50.0
        self.trend    = 0.0   # slope EMA
        self.warmed   = False
        self.alpha    = 2.0 / (CFE_RSI_EMA_N + 1)

    def update(self, mid: float):
        if self.prev_mid == 0.0:
            self.prev_mid = mid
            return
        chg = mid - self.prev_mid
        self.prev_mid = mid
        self.gains.append(chg if chg > 0 else 0.0)
        self.losses.append(-chg if chg < 0 else 0.0)
        if len(self.gains) > CFE_RSI_PERIOD:
            self.gains.popleft()
            self.losses.popleft()
        if len(self.gains) < CFE_RSI_PERIOD:
            return
        ag = sum(self.gains)  / CFE_RSI_PERIOD
        al = sum(self.losses) / CFE_RSI_PERIOD
        self.prev = self.cur
        self.cur  = 100.0 if al == 0.0 else 100.0 - 100.0 / (1.0 + ag / al)
        slope = self.cur - self.prev
        if not self.warmed:
            self.trend = slope
            self.warmed = True
        else:
            self.trend = self.alpha * slope + (1.0 - self.alpha) * self.trend

    def direction(self) -> int:
        if not self.warmed:
            return 0
        if self.trend > CFE_RSI_THRESH:
            return 1
        if self.trend < -CFE_RSI_THRESH:
            return -1
        return 0


class BarBuilder:
    """Aggregates ticks into M1 bars."""
    def __init__(self):
        self.bar_start_ms = 0
        self.o = self.h = self.l = self.c = 0.0
        self.has_bar = False
        self.completed: Optional[Bar] = None

    def update(self, mid: float, ts_ms: int) -> Optional[Bar]:
        """Returns completed bar when a new bar period starts, else None."""
        bar_ts = (ts_ms // M1_MS) * M1_MS
        self.completed = None
        if not self.has_bar:
            self.bar_start_ms = bar_ts
            self.o = self.h = self.l = self.c = mid
            self.has_bar = True
            return None
        if bar_ts != self.bar_start_ms:
            # Close current bar
            b = Bar(ts_ms=self.bar_start_ms,
                    open=self.o, high=self.h, low=self.l, close=self.c,
                    valid=True)
            self.completed = b
            # Start new
            self.bar_start_ms = bar_ts
            self.o = self.h = self.l = self.c = mid
            return b
        self.h = max(self.h, mid)
        self.l = min(self.l, mid)
        self.c = mid
        return None


class ATRCalc:
    """ATR from M1 bar high-low ranges, EMA smoothed."""
    def __init__(self):
        self.vals  = deque()
        self.atr   = 0.0
        self.alpha = 2.0 / (ATR_PERIOD + 1)

    def update(self, bar: Bar):
        rng = bar.high - bar.low
        self.vals.append(rng)
        if len(self.vals) < ATR_PERIOD:
            self.atr = sum(self.vals) / len(self.vals)
        else:
            self.atr = self.alpha * rng + (1.0 - self.alpha) * self.atr


class VWAPCalc:
    """Daily-reset cumulative VWAP."""
    def __init__(self):
        self.cum_pv  = 0.0
        self.cum_vol = 0.0
        self.last_day = -1

    def update(self, mid: float, ts_ms: int) -> float:
        day = ts_ms // 86_400_000
        if day != self.last_day:
            self.cum_pv  = 0.0
            self.cum_vol = 0.0
            self.last_day = day
        self.cum_pv  += mid
        self.cum_vol += 1.0
        return self.cum_pv / self.cum_vol if self.cum_vol > 0 else 0.0


class VolRatioCalc:
    """Vol ratio: recent tick rate vs longer baseline (proxy for range expansion)."""
    def __init__(self, short_n=50, long_n=300):
        self.short_buf = deque(maxlen=short_n)
        self.long_buf  = deque(maxlen=long_n)

    def update(self, mid: float) -> float:
        self.short_buf.append(mid)
        self.long_buf.append(mid)
        if len(self.short_buf) < 10 or len(self.long_buf) < 50:
            return 1.0
        short_std = _std(self.short_buf)
        long_std  = _std(self.long_buf)
        if long_std == 0.0:
            return 1.0
        return short_std / long_std


def _std(buf) -> float:
    n = len(buf)
    if n < 2:
        return 0.0
    m = sum(buf) / n
    return math.sqrt(sum((x - m) ** 2 for x in buf) / n)

# ══════════════════════════════════════════════════════════════════════════════
# CFE Engine
# ══════════════════════════════════════════════════════════════════════════════

class CFEEngine:
    def __init__(self, gates_enabled=True):
        self.gates_enabled = gates_enabled
        self.rsi        = RSICalc()
        self.bar_bld    = BarBuilder()
        self.atr_calc   = ATRCalc()
        self.vwap_calc  = VWAPCalc()
        self.vol_ratio  = VolRatioCalc()

        self.pos        = OpenPos()
        self.phase      = 'IDLE'   # IDLE / LIVE / COOLDOWN
        self.trades     = []

        # EWM drift
        self.ewm_mid    = 0.0
        self.ewm_drift  = 0.0
        self.prev_ewm   = 0.0

        # Cooldown state
        self.cooldown_start_ms  = 0
        self.cooldown_ms        = 15_000
        self.dfe_cooldown_until = 0
        self.last_closed_dir    = 0
        self.last_closed_ms     = 0

        # DFE state
        self.dfe_warmed         = False
        self.prev_ewm_drift     = 0.0
        self.dfe_eff_thresh     = CFE_DFE_DRIFT_THRESH
        self.dfe_persist_ticks  = 0
        self.dfe_persist_dir    = 0

        # Sustained drift state
        self.drift_sus_start_ms = 0
        self.drift_sus_dir      = 0

        # Recent mid for DFE price confirm
        self.recent_mid         = deque(maxlen=CFE_DFE_PRICE_CONFIRM_TICKS + 2)

        # IMB exit state (no real L2 in backtest -- skip IMB_EXIT)
        self.imb_against_ticks  = 0

        # Adverse excursion filter
        self.last_loss_exit_px  = 0.0
        self.last_loss_dir      = 0
        self.last_loss_atr      = 0.0
        self.adverse_block      = False

        self.trade_id           = 0
        self.last_tick_ms       = 0   # for gap detection

    # ── Public entry point ──────────────────────────────────────────────────

    def on_tick(self, bid: float, ask: float, ts_ms: int):
        mid    = (bid + ask) * 0.5
        spread = ask - bid

        # ── Gap detection: reset time-sensitive state on gaps > 1 hour ───────
        # Weekend/session gaps mean sustained drift, DFE persist, and EWM drift
        # state are stale. A 60h weekend would make drift_sus_start_ms make
        # Monday's first ticks look like they have days of sustained drift.
        # Also reset bar builder so a partial bar from Friday doesn't bleed in.
        if self.last_tick_ms > 0:
            gap_ms = ts_ms - self.last_tick_ms
            if gap_ms > 3_600_000:  # > 1 hour
                self.drift_sus_start_ms = 0
                self.drift_sus_dir      = 0
                self.dfe_persist_ticks  = 0
                self.dfe_persist_dir    = 0
                self.ewm_mid            = mid   # reset EWM to current price
                self.ewm_drift          = 0.0
                self.prev_ewm_drift     = 0.0
                self.dfe_warmed         = False
                self.bar_bld            = BarBuilder()  # discard partial bar
                self.recent_mid.clear()
                # If position was open over the gap (shouldn't happen but defensive)
                if self.phase == 'LIVE':
                    p = self.pos
                    exit_px = bid if p.is_long else ask
                    pnl = ((exit_px - p.entry) if p.is_long else (p.entry - exit_px)) * p.size
                    self._close(exit_px, 'FORCE_CLOSE', ts_ms, pnl, ts_ms - p.entry_ts_ms)
        self.last_tick_ms = ts_ms

        # Always update indicators
        self.rsi.update(mid)
        self.recent_mid.append(mid)
        vwap = self.vwap_calc.update(mid, ts_ms)
        vol_ratio = self.vol_ratio.update(mid)

        # EWM drift
        if self.ewm_mid == 0.0:
            self.ewm_mid = mid
        self.ewm_mid   = EWM_DRIFT_ALPHA * mid + (1.0 - EWM_DRIFT_ALPHA) * self.ewm_mid
        self.ewm_drift = mid - self.ewm_mid

        # M1 bar
        completed_bar = self.bar_bld.update(mid, ts_ms)
        if completed_bar:
            self.atr_calc.update(completed_bar)

        atr = self.atr_calc.atr if self.atr_calc.atr > 0 else spread * 3.0

        # Session
        utc_hour = (ts_ms // 1000 % 86400) // 3600
        in_asia  = (utc_hour >= 22 or utc_hour < 7)
        post_ny  = (19 <= utc_hour < 22)

        # Sustained drift tracking
        self._update_sustained(ts_ms)
        drift_sustained_ms = (ts_ms - self.drift_sus_start_ms) \
            if (self.drift_sus_dir != 0 and self.drift_sus_start_ms > 0) else 0

        # Manage open position
        if self.phase == 'LIVE':
            self._manage(bid, ask, mid, ts_ms, atr)
            return

        # Cooldown
        if self.phase == 'COOLDOWN':
            if ts_ms - self.cooldown_start_ms >= self.cooldown_ms:
                self.phase = 'IDLE'
            else:
                return

        # ── IDLE: check entry ───────────────────────────────────────────────

        # Adverse excursion block
        if self.gates_enabled and self.adverse_block and self.last_loss_dir != 0:
            dist = (self.last_loss_exit_px - mid) if self.last_loss_dir == +1 \
                   else (mid - self.last_loss_exit_px)
            thresh = self.last_loss_atr * 0.5
            same_dir = (self.ewm_drift > 0 and self.last_loss_dir == +1) or \
                       (self.ewm_drift < 0 and self.last_loss_dir == -1)
            if same_dir and dist > thresh:
                return
            if not same_dir or dist <= thresh:
                self.adverse_block = False

        # Opposite direction cooldown
        if self.last_closed_ms > 0 and self.last_closed_dir != 0:
            since = ts_ms - self.last_closed_ms
            if since < CFE_OPPOSITE_DIR_COOLDOWN_MS:
                intended = 1 if self.ewm_drift > 0 else -1
                if intended != self.last_closed_dir:
                    return

        # Update DFE effective threshold
        self.dfe_eff_thresh = max(4.0, atr * 0.40) if in_asia \
                              else max(CFE_DFE_DRIFT_THRESH, atr * 0.30)

        # DFE drift persistence
        if abs(self.ewm_drift) >= self.dfe_eff_thresh:
            d = 1 if self.ewm_drift > 0 else -1
            if d == self.dfe_persist_dir:
                self.dfe_persist_ticks += 1
            else:
                self.dfe_persist_ticks = 1
                self.dfe_persist_dir   = d
        else:
            self.dfe_persist_ticks = 0
            self.dfe_persist_dir   = 0

        # ── DFE path ────────────────────────────────────────────────────────
        if self.rsi.warmed and abs(self.ewm_drift) >= self.dfe_eff_thresh:
            drift_delta = self.ewm_drift - self.prev_ewm_drift
            drift_accel = self.dfe_warmed and (
                (self.ewm_drift > 0 and drift_delta >= CFE_DFE_DRIFT_ACCEL) or
                (self.ewm_drift < 0 and drift_delta <= -CFE_DFE_DRIFT_ACCEL))
            self.prev_ewm_drift = self.ewm_drift
            self.dfe_warmed = True
            dfe_long = self.ewm_drift > 0

            rsi_ok = (self.rsi.trend > CFE_DFE_RSI_THRESH and
                      self.rsi.trend < CFE_DFE_RSI_TREND_MAX) if dfe_long \
                     else (self.rsi.trend < -CFE_DFE_RSI_THRESH and
                           self.rsi.trend > -CFE_DFE_RSI_TREND_MAX)
            rsi_level_ok = (self.rsi.cur >= CFE_DFE_RSI_LEVEL_LONG_MIN) if dfe_long \
                           else (self.rsi.cur <= CFE_DFE_RSI_LEVEL_SHORT_MAX)
            persist_ok   = (self.dfe_persist_ticks >= CFE_DFE_DRIFT_PERSIST_TICKS)

            # Price confirm
            price_confirms = True
            if len(self.recent_mid) >= CFE_DFE_PRICE_CONFIRM_TICKS:
                oldest = list(self.recent_mid)[0]
                net    = mid - oldest
                if dfe_long:
                    price_confirms = net >= CFE_DFE_PRICE_CONFIRM_MIN
                else:
                    price_confirms = net <= -CFE_DFE_PRICE_CONFIRM_MIN

            spread_ok = spread < (CFE_COST_SLIPPAGE * 2 + CFE_COMMISSION_PTS * 2) * CFE_DFE_MIN_SPREAD_MULT
            cooldown_ok = ts_ms >= self.dfe_cooldown_until
            atr_ok = atr <= CFE_MAX_ATR_ENTRY

            if (drift_accel and rsi_ok and rsi_level_ok and persist_ok and
                    price_confirms and spread_ok and cooldown_ok and atr_ok):
                sl_pts = atr * CFE_DFE_SL_MULT
                self._enter(dfe_long, bid, ask, spread, atr, ts_ms, 'DFE')
                return

        # ── Sustained-drift path ─────────────────────────────────────────────
        if not in_asia and drift_sustained_ms >= CFE_DFE_DRIFT_SUSTAINED_MS:
            sus_long = self.drift_sus_dir == 1
            rsi_ok2  = (self.rsi.trend > 0) if sus_long else (self.rsi.trend < 0)
            rsi_level_ok2 = (self.rsi.cur >= 40) if sus_long else (self.rsi.cur <= 60)
            spread_ok2    = spread < (CFE_COST_SLIPPAGE * 2 + CFE_COMMISSION_PTS * 2) * CFE_DFE_MIN_SPREAD_MULT
            atr_ok2       = atr <= CFE_MAX_ATR_ENTRY
            cooldown_ok2  = ts_ms >= self.dfe_cooldown_until
            if rsi_ok2 and rsi_level_ok2 and spread_ok2 and atr_ok2 and cooldown_ok2:
                self.drift_sus_start_ms = ts_ms  # reset to avoid re-firing
                self._enter(sus_long, bid, ask, spread, atr, ts_ms, 'SUS')
                return

        # ── Bar-close path ───────────────────────────────────────────────────
        if completed_bar is None:
            return
        if not self.rsi.warmed:
            return

        bar = completed_bar

        # Post-NY gate
        if self.gates_enabled and post_ny:
            return

        # Drift minimum gate
        if self.gates_enabled and abs(self.ewm_drift) < 0.3:
            return

        # Trend context gate
        if drift_sustained_ms >= CFE_BAR_TREND_BLOCK_MS:
            if self.drift_sus_dir == -1 and bar.close > bar.open:
                return
            if self.drift_sus_dir == +1 and bar.close < bar.open:
                return

        # RSI direction
        rsi_dir = self.rsi.direction()
        if rsi_dir == 0:
            return

        # RSI/drift agreement gate
        if self.gates_enabled:
            if rsi_dir == +1 and self.ewm_drift < 0.0:
                return
            if rsi_dir == -1 and self.ewm_drift > 0.0:
                return

        # Candle direction agrees with RSI
        body = bar.close - bar.open
        bar_range = bar.high - bar.low
        if bar_range == 0:
            return
        body_ratio = abs(body) / bar_range
        bullish = body > 0 and body_ratio >= CFE_BODY_RATIO_MIN
        bearish = body < 0 and body_ratio >= CFE_BODY_RATIO_MIN
        if rsi_dir == +1 and not bullish:
            return
        if rsi_dir == -1 and not bearish:
            return

        # Cost coverage
        cost_pts = spread + CFE_COST_SLIPPAGE * 2 + CFE_COMMISSION_PTS * 2
        if bar_range < CFE_COST_MULT * cost_pts:
            return

        # ATR cap
        if atr > CFE_MAX_ATR_ENTRY:
            return

        # VWAP filter
        if self.gates_enabled and vwap > 0 and abs(self.ewm_drift) < 1.0:
            is_long_intent = self.ewm_drift > 0
            if is_long_intent and mid > vwap:
                return
            if not is_long_intent and mid < vwap:
                return

        # Chop gate
        if self.gates_enabled and vol_ratio > CHOP_VOL_RATIO_THRESH and abs(self.ewm_drift) < CHOP_DRIFT_ABS_THRESH:
            return

        is_long = (rsi_dir == +1)
        self._enter(is_long, bid, ask, spread, atr, ts_ms, 'BAR')

    # ── Enter ────────────────────────────────────────────────────────────────

    def _enter(self, is_long: bool, bid: float, ask: float,
               spread: float, atr: float, ts_ms: int, entry_type: str):
        entry_px = ask if is_long else bid
        sl_pts   = atr if atr > 0 else spread * 5.0
        sl_px    = (entry_px - sl_pts) if is_long else (entry_px + sl_pts)
        size     = CFE_RISK_DOLLARS / (sl_pts * 100.0)
        size     = math.floor(size / 0.001) * 0.001
        size     = max(CFE_MIN_LOT, min(CFE_MAX_LOT, size))
        cost_pts = spread + CFE_COST_SLIPPAGE * 2 + CFE_COMMISSION_PTS * 2

        self.pos = OpenPos(
            active=True, is_long=is_long,
            entry=entry_px, sl=sl_px, trail_sl=sl_px,
            size=size, full_size=size, cost_pts=cost_pts,
            entry_ts_ms=ts_ms, mfe=0.0,
            trail_active=False, partial_done=False, atr_pts=atr
        )
        self.pos.entry_type = entry_type
        self.trade_id += 1
        self.phase = 'LIVE'
        self.imb_against_ticks = 0

    # ── Manage open position ─────────────────────────────────────────────────

    def _manage(self, bid: float, ask: float, mid: float, ts_ms: int, atr: float):
        p = self.pos
        move = (mid - p.entry) if p.is_long else (p.entry - mid)
        if move > p.mfe:
            p.mfe = move
        hold_ms = ts_ms - p.entry_ts_ms

        # Partial TP at MFE >= 2x cost
        if not p.partial_done and p.mfe >= p.cost_pts * 2.0:
            px = bid if p.is_long else ask
            pnl = ((px - p.entry) if p.is_long else (p.entry - px)) * p.full_size * 0.5
            self._record_trade(px, 'PARTIAL_TP', ts_ms, p.full_size * 0.5, pnl, hold_ms)
            p.partial_done = True
            p.size = p.full_size * 0.5

        # Trail SL: engage at MFE >= 2x ATR
        if p.mfe >= p.atr_pts * 2.0:
            trail_dist = p.atr_pts * 0.5
            new_trail  = (mid - trail_dist) if p.is_long else (mid + trail_dist)
            if not p.trail_active:
                if (p.is_long and new_trail > p.sl) or (not p.is_long and new_trail < p.sl):
                    p.trail_sl     = new_trail
                    p.trail_active = True
            else:
                if (p.is_long and new_trail > p.trail_sl) or \
                   (not p.is_long and new_trail < p.trail_sl):
                    p.trail_sl = new_trail

        # SL hit
        eff_sl = p.trail_sl if p.trail_active else p.sl
        if (p.is_long and bid <= eff_sl) or (not p.is_long and ask >= eff_sl):
            px     = bid if p.is_long else ask
            reason = 'TRAIL_SL' if p.trail_active else 'SL_HIT'
            pnl    = ((px - p.entry) if p.is_long else (p.entry - px)) * p.size
            self._close(px, reason, ts_ms, pnl, hold_ms)
            return

        # Stagnation
        utc_hour = (ts_ms // 1000 % 86400) // 3600
        in_asia  = (utc_hour >= 22 or utc_hour < 7)
        stag_ms  = CFE_STAGNATION_MS if in_asia else 180_000

        if hold_ms >= stag_ms:
            if p.mfe < p.cost_pts * CFE_STAGNATION_MULT:
                px  = bid if p.is_long else ask
                pnl = ((px - p.entry) if p.is_long else (p.entry - px)) * p.size
                self._close(px, 'STAGNATION', ts_ms, pnl, hold_ms)
                return

    # ── Close ────────────────────────────────────────────────────────────────

    def _record_trade(self, exit_px, reason, ts_ms, size, pnl, hold_ms):
        """Record a trade (partial or full)."""
        p = self.pos
        t = Trade(
            id          = self.trade_id,
            side        = 'LONG' if p.is_long else 'SHORT',
            entry       = p.entry,
            exit_px     = exit_px,
            sl          = p.sl,
            size        = size,
            pnl_pts     = (exit_px - p.entry) if p.is_long else (p.entry - exit_px),
            pnl_usd     = pnl * 100.0,
            entry_ts_ms = p.entry_ts_ms,
            exit_ts_ms  = ts_ms,
            exit_reason = reason,
            hold_ms     = hold_ms,
            mfe         = p.mfe,
            entry_type  = getattr(p, 'entry_type', 'BAR'),
        )
        self.trades.append(t)

    def _close(self, exit_px, reason, ts_ms, pnl, hold_ms):
        p = self.pos
        self._record_trade(exit_px, reason, ts_ms, p.size, pnl, hold_ms)

        # Adverse excursion tracking
        adverse = (p.entry - exit_px) if p.is_long else (exit_px - p.entry)
        if adverse > 0 and p.atr_pts > 0 and adverse >= p.atr_pts * 0.5:
            self.last_loss_exit_px = exit_px
            self.last_loss_dir     = +1 if p.is_long else -1
            self.last_loss_atr     = p.atr_pts
            self.adverse_block     = True

        self.last_closed_dir = +1 if p.is_long else -1
        self.last_closed_ms  = ts_ms

        # Cooldown
        is_winner = reason in ('PARTIAL_TP', 'TRAIL_SL', 'IMB_EXIT')
        if reason == 'STAGNATION':
            self.cooldown_ms = 60_000
        elif reason == 'FORCE_CLOSE':
            self.cooldown_ms = 300_000
        elif reason == 'IMB_EXIT':
            self.cooldown_ms = 30_000
        elif is_winner:
            self.cooldown_ms = 30_000
        else:
            self.cooldown_ms = 15_000

        if reason == 'FORCE_CLOSE':
            self.dfe_cooldown_until = ts_ms + 300_000
        elif reason in ('SL_HIT', 'TRAIL_SL'):
            self.dfe_cooldown_until = ts_ms + CFE_DFE_COOLDOWN_MS
        elif is_winner:
            self.dfe_cooldown_until = ts_ms + CFE_WINNER_COOLDOWN_MS

        self.pos   = OpenPos()
        self.phase = 'COOLDOWN'
        self.cooldown_start_ms = ts_ms

    # ── Sustained drift update ────────────────────────────────────────────────

    def _update_sustained(self, ts_ms: int):
        d = self.ewm_drift
        new_dir = 1 if d >= CFE_DFE_DRIFT_SUSTAINED_THRESH else \
                  (-1 if d <= -CFE_DFE_DRIFT_SUSTAINED_THRESH else 0)
        if new_dir != 0 and new_dir == self.drift_sus_dir:
            pass  # continuing
        elif new_dir != 0:
            self.drift_sus_dir      = new_dir
            self.drift_sus_start_ms = ts_ms
        else:
            self.drift_sus_dir      = 0
            self.drift_sus_start_ms = 0

# ══════════════════════════════════════════════════════════════════════════════
# Statistics
# ══════════════════════════════════════════════════════════════════════════════

def compute_stats(trades):
    if not trades:
        return {}

    # Exclude partial TPs from standalone stats (they pair with full closes)
    full_closes = [t for t in trades if t.exit_reason != 'PARTIAL_TP']
    all_trades  = trades

    pnls   = [t.pnl_usd for t in full_closes]
    wins   = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p < 0]

    total_pnl = sum(t.pnl_usd for t in all_trades)
    wr = len(wins) / len(pnls) * 100 if pnls else 0
    avg_win  = sum(wins)   / len(wins)   if wins   else 0
    avg_loss = sum(losses) / len(losses) if losses else 0
    rr = abs(avg_win / avg_loss) if avg_loss != 0 else 0
    expectancy = (wr/100 * avg_win) + ((1 - wr/100) * avg_loss)

    # Max drawdown on cumulative equity
    equity = 0.0
    peak   = 0.0
    max_dd = 0.0
    for t in sorted(all_trades, key=lambda x: x.exit_ts_ms):
        equity += t.pnl_usd
        if equity > peak:
            peak = equity
        dd = peak - equity
        if dd > max_dd:
            max_dd = dd

    # Profit factor
    gross_profit = sum(wins)
    gross_loss   = abs(sum(losses))
    pf = gross_profit / gross_loss if gross_loss > 0 else float('inf')

    # By exit reason
    by_reason = {}
    for t in all_trades:
        r = t.exit_reason
        if r not in by_reason:
            by_reason[r] = {'count': 0, 'pnl': 0.0, 'wins': 0}
        by_reason[r]['count'] += 1
        by_reason[r]['pnl']   += t.pnl_usd
        if t.pnl_usd > 0:
            by_reason[r]['wins'] += 1

    # By entry type
    by_type = {}
    for t in full_closes:
        k = t.entry_type
        if k not in by_type:
            by_type[k] = {'count': 0, 'pnl': 0.0, 'wins': 0}
        by_type[k]['count'] += 1
        by_type[k]['pnl']   += t.pnl_usd
        if t.pnl_usd > 0:
            by_type[k]['wins'] += 1

    # By UTC hour
    by_hour = {}
    for t in full_closes:
        h = (t.entry_ts_ms // 1000 % 86400) // 3600
        if h not in by_hour:
            by_hour[h] = {'count': 0, 'pnl': 0.0, 'wins': 0}
        by_hour[h]['count'] += 1
        by_hour[h]['pnl']   += t.pnl_usd
        if t.pnl_usd > 0:
            by_hour[h]['wins'] += 1

    # Monthly PnL
    monthly = {}
    for t in all_trades:
        dt = datetime.datetime.utcfromtimestamp(t.exit_ts_ms / 1000)
        k  = f'{dt.year}-{dt.month:02d}'
        monthly[k] = monthly.get(k, 0.0) + t.pnl_usd

    # Sharpe (monthly)
    m_vals = list(monthly.values())
    sharpe = 0.0
    if len(m_vals) > 1:
        m_mean = sum(m_vals) / len(m_vals)
        m_std  = math.sqrt(sum((x - m_mean)**2 for x in m_vals) / len(m_vals))
        sharpe = (m_mean / m_std * math.sqrt(12)) if m_std > 0 else 0.0

    return dict(
        total_trades=len(full_closes), all_records=len(all_trades),
        total_pnl=total_pnl, win_rate=wr,
        avg_win=avg_win, avg_loss=avg_loss, rr=rr,
        expectancy=expectancy, max_dd=max_dd,
        profit_factor=pf, sharpe=sharpe,
        by_reason=by_reason, by_type=by_type,
        by_hour=by_hour, monthly=monthly,
        gross_profit=gross_profit, gross_loss=gross_loss,
    )


def print_report(stats, gates_enabled, out_file=None):
    lines = []
    a = lines.append
    a("=" * 70)
    a(f"CFE BACKTEST REPORT  |  Gates: {'ON' if gates_enabled else 'OFF (BASELINE)'}")
    a("=" * 70)
    a(f"Total trades (full closes): {stats['total_trades']:,}")
    a(f"Total records (incl partials): {stats['all_records']:,}")
    a(f"")
    a(f"Net P&L:        ${stats['total_pnl']:,.2f}")
    a(f"Gross profit:   ${stats['gross_profit']:,.2f}")
    a(f"Gross loss:     ${stats['gross_loss']:,.2f}")
    a(f"Profit factor:  {stats['profit_factor']:.2f}")
    a(f"Win rate:       {stats['win_rate']:.1f}%")
    a(f"Avg win:        ${stats['avg_win']:.2f}")
    a(f"Avg loss:       ${stats['avg_loss']:.2f}")
    a(f"R:R ratio:      {stats['rr']:.2f}")
    a(f"Expectancy:     ${stats['expectancy']:.2f} per trade")
    a(f"Max drawdown:   ${stats['max_dd']:.2f}")
    a(f"Sharpe (monthly annualised): {stats['sharpe']:.2f}")
    a(f"")
    a("── BY EXIT REASON ─────────────────────────────────────────────────")
    for r, v in sorted(stats['by_reason'].items()):
        wr = v['wins'] / v['count'] * 100 if v['count'] > 0 else 0
        a(f"  {r:20s}  n={v['count']:5d}  pnl=${v['pnl']:+8.2f}  wr={wr:.0f}%")
    a(f"")
    a("── BY ENTRY TYPE ──────────────────────────────────────────────────")
    for r, v in sorted(stats['by_type'].items()):
        wr = v['wins'] / v['count'] * 100 if v['count'] > 0 else 0
        a(f"  {r:6s}  n={v['count']:5d}  pnl=${v['pnl']:+8.2f}  wr={wr:.0f}%")
    a(f"")
    a("── BY UTC HOUR (full closes) ───────────────────────────────────────")
    for h in range(24):
        v = stats['by_hour'].get(h, {'count': 0, 'pnl': 0.0, 'wins': 0})
        if v['count'] == 0:
            continue
        wr = v['wins'] / v['count'] * 100
        bar = '█' * int(v['pnl'] / 20) if v['pnl'] > 0 else '░' * int(abs(v['pnl']) / 20)
        a(f"  {h:02d}:00  n={v['count']:4d}  pnl=${v['pnl']:+7.2f}  wr={wr:.0f}%  {bar[:40]}")
    a(f"")
    a("── MONTHLY P&L ─────────────────────────────────────────────────────")
    cum = 0.0
    for m, pnl in sorted(stats['monthly'].items()):
        cum += pnl
        bar = '█' * int(pnl / 20) if pnl > 0 else '░' * int(abs(pnl) / 20)
        a(f"  {m}  {pnl:+8.2f}  cum={cum:+8.2f}  {bar[:40]}")
    a("=" * 70)

    report = "\n".join(lines)
    print(report)
    if out_file:
        with open(out_file, 'w') as f:
            f.write(report + "\n")


# ══════════════════════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description='CFE backtest on 2yr XAUUSD tick data')
    parser.add_argument('--data',    required=True, help='Path to 2yr_XAUUSD_tick.csv')
    parser.add_argument('--start',   default=None,  help='Start date YYYY-MM-DD (optional)')
    parser.add_argument('--end',     default=None,  help='End date YYYY-MM-DD (optional)')
    parser.add_argument('--no-gates', action='store_true', help='Disable all gates (baseline)')
    parser.add_argument('--out-dir', default='.',   help='Output directory')
    args = parser.parse_args()

    gates = not args.no_gates

    start_ms = None
    end_ms   = None
    if args.start:
        dt = datetime.datetime.strptime(args.start, '%Y-%m-%d').replace(
            tzinfo=datetime.timezone.utc)
        start_ms = int(dt.timestamp() * 1000)
    if args.end:
        dt = datetime.datetime.strptime(args.end, '%Y-%m-%d').replace(
            tzinfo=datetime.timezone.utc)
        end_ms = int(dt.timestamp() * 1000)

    engine = CFEEngine(gates_enabled=gates)

    print(f"Loading {args.data} ...", flush=True)
    total_rows  = 0
    skipped     = 0
    tick_count  = 0
    report_every = 5_000_000

    with open(args.data, 'r') as f:
        for raw in f:
            total_rows += 1
            if total_rows % report_every == 0:
                print(f"  {total_rows:,} rows  |  {tick_count:,} ticks processed  |"
                      f"  {len(engine.trades):,} trades", flush=True)

            raw = raw.strip()
            if not raw:
                continue
            parts = raw.split(',')
            if len(parts) < 4:
                skipped += 1
                continue

            try:
                date_str = parts[0]   # YYYYMMDD
                time_str = parts[1]   # HH:MM:SS
                bid      = float(parts[2])
                ask      = float(parts[3])
            except (ValueError, IndexError):
                skipped += 1
                continue

            # Parse timestamp
            try:
                dt = datetime.datetime.strptime(
                    f'{date_str} {time_str}', '%Y%m%d %H:%M:%S'
                ).replace(tzinfo=datetime.timezone.utc)
                ts_ms = int(dt.timestamp() * 1000)
            except ValueError:
                skipped += 1
                continue

            if start_ms and ts_ms < start_ms:
                continue
            if end_ms and ts_ms >= end_ms:
                break

            engine.on_tick(bid, ask, ts_ms)
            tick_count += 1

    # Force-close any open position at end
    if engine.phase == 'LIVE':
        p = engine.pos
        mid = p.entry  # no current price, use entry as proxy
        pnl = 0.0
        engine._close(mid, 'FORCE_CLOSE', ts_ms, pnl, ts_ms - p.entry_ts_ms)

    print(f"\nDone. {total_rows:,} rows, {tick_count:,} ticks, "
          f"{skipped:,} skipped, {len(engine.trades):,} trade records", flush=True)

    if not engine.trades:
        print("No trades generated.")
        return

    # Write trades CSV
    trades_path = os.path.join(args.out_dir, 'cfe_backtest_trades.csv')
    with open(trades_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['id','side','entry_type','entry','exit_px','sl','size',
                    'pnl_pts','pnl_usd','entry_ts_ms','exit_ts_ms',
                    'hold_ms','mfe','exit_reason'])
        for t in engine.trades:
            w.writerow([t.id, t.side, t.entry_type,
                        f'{t.entry:.3f}', f'{t.exit_px:.3f}', f'{t.sl:.3f}',
                        f'{t.size:.3f}', f'{t.pnl_pts:.4f}', f'{t.pnl_usd:.2f}',
                        t.entry_ts_ms, t.exit_ts_ms,
                        t.hold_ms, f'{t.mfe:.4f}', t.exit_reason])
    print(f"Trades: {trades_path}")

    # Write equity CSV (daily)
    equity_path = os.path.join(args.out_dir, 'cfe_backtest_equity.csv')
    daily = {}
    for t in engine.trades:
        d = datetime.datetime.utcfromtimestamp(t.exit_ts_ms / 1000).strftime('%Y-%m-%d')
        daily[d] = daily.get(d, 0.0) + t.pnl_usd
    with open(equity_path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['date', 'daily_pnl', 'cumulative_pnl'])
        cum = 0.0
        for d in sorted(daily):
            cum += daily[d]
            w.writerow([d, f'{daily[d]:.2f}', f'{cum:.2f}'])
    print(f"Equity:  {equity_path}")

    # Report
    stats = compute_stats(engine.trades)
    report_path = os.path.join(args.out_dir, 'cfe_backtest_report.txt')
    print_report(stats, gates, report_path)
    print(f"Report:  {report_path}")


if __name__ == '__main__':
    main()
