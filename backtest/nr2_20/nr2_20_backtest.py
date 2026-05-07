#!/usr/bin/env python3
"""
nr2_20_backtest.py
==================

Single-run backtest for the XAUUSD NR2-NR20 + Inside Bar compression-breakout
strategy described in HANDOFF_NR2_20_BACKTEST.md.

The bar loop is intentionally written in plain Python (not vectorized) so the
control flow mirrors the future C++ port `XauusdNr2_20Engine.hpp` line for line.
This makes the row-for-row verifier trivial later.

Usage
-----
  Run on real data:
    python nr2_20_backtest.py --bars XAUUSD_15min.csv --out nr2_20_trades.csv

  Run with non-default params:
    python nr2_20_backtest.py --bars XAUUSD_15min.csv --out trades.csv \\
        --min-nr 7 --rr 1.5 --trend-ma 20 --time-stop 32

  Self-test against synthetic fixture:
    python nr2_20_backtest.py --self-test

Input CSV schema (header required, UTC timestamps):
    ts,open,high,low,close,volume

Output CSV schema:
    trade_id,entry_ts,exit_ts,side,entry_px,exit_px,pnl_quote,pnl_R,bars_held,
    exit_reason,box_top,box_bottom,formation_ts,formation_bar,session,
    trend_ma_at_entry,nr_class_count,ib_count

Author: Session 2026-05-03
"""

import argparse
import csv
import math
import sys
from collections import deque
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from typing import List, Optional, Tuple

# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class Bar:
    """One OHLC bar."""
    ts: datetime
    open: float
    high: float
    low: float
    close: float
    volume: float
    # Derived (filled by engine):
    range: float = 0.0
    nr_class: int = 0       # largest n for which this bar is NR_n (0 if not NR_2)
    is_ib: bool = False
    is_qcb: bool = False
    ema: float = 0.0
    trend: int = 0           # +1 up, -1 down, 0 flat
    seq: int = 0             # 0-based sequential bar index


@dataclass
class Box:
    """An armed compression box, ready to be broken out of."""
    top: float
    bottom: float
    formation_bar: int          # seq index of the bar that completed the cluster
    formation_ts: datetime
    formation_session: str
    nr_class_count: int         # diagnostic: how many NR-classified QCBs in cluster
    ib_count: int               # diagnostic: how many IB QCBs in cluster

    def expired(self, current_seq: int, box_expiry: int) -> bool:
        return (current_seq - self.formation_bar) > box_expiry


@dataclass
class Position:
    """An open position."""
    side: str                   # "LONG" or "SHORT"
    entry_ts: datetime
    entry_px: float
    sl_px: float
    tp_px: float
    entry_seq: int              # for time-stop accounting
    box: Box                    # the box that triggered entry


@dataclass
class Trade:
    """A closed trade — what we write to CSV."""
    trade_id: int
    entry_ts: datetime
    exit_ts: datetime
    side: str
    entry_px: float
    exit_px: float
    pnl_quote: float
    pnl_R: float
    bars_held: int
    exit_reason: str
    box_top: float
    box_bottom: float
    formation_ts: datetime
    formation_bar: int
    session: str
    trend_ma_at_entry: float
    nr_class_count: int
    ib_count: int


# ---------------------------------------------------------------------------
# Parameters (defaults match HANDOFF_NR2_20_BACKTEST.md §8)
# ---------------------------------------------------------------------------

@dataclass
class Params:
    max_nr: int = 20
    min_nr: int = 4
    min_qcb: int = 2
    cluster_lookback: int = 6
    box_expiry: int = 8
    trend_ma_period: int = 50
    trend_slope_lookback: int = 3
    rr: float = 1.0
    time_stop_bars: int = 64
    same_bar_resolution: str = "sl_first"   # sl_first | tp_first | reject
    spread_pips: float = 0.5                # cost model — XAUUSD 1 pip = 0.10
    slippage_pips: float = 0.3
    pip_size: float = 0.10                  # XAUUSD: 1 pip = 0.10 USD/oz
    # Trend filter selection: ema (default), vwap, or both (must agree)
    trend_filter: str = "ema"               # ema | vwap | both
    vwap_reset: str = "daily"               # daily | session | never
    vwap_slope_lookback: int = 3


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def session_for_ts(ts: datetime) -> str:
    """Map a UTC timestamp to ASIA / LONDON / NY per Omega convention."""
    h = ts.hour
    if 7 <= h < 13:
        return "LONDON"
    if 13 <= h < 22:
        return "NY"
    return "ASIA"   # 22:00–07:00 UTC


def parse_iso_utc(s: str) -> datetime:
    """Parse a few common timestamp formats, return tz-aware UTC datetime.
    Accepted forms:
      - ISO 8601:        "2024-01-01T00:00:00Z"  /  "2024-01-01 00:00:00"
      - HISTDATA native: "20240101 000000"
      - Unix epoch sec:  "1756684800"  (10-digit int as string)
      - Unix epoch ms:   "1756684800000"  (13-digit int as string)
    """
    s = s.strip()
    # Unix epoch first (cheap to detect — all digits)
    if s.isdigit():
        v = int(s)
        # Heuristic: 10-digit = seconds, 13-digit = milliseconds
        if v > 10_000_000_000:
            v = v // 1000
        return datetime.fromtimestamp(v, tz=timezone.utc)
    # Common ISO with Z
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    try:
        dt = datetime.fromisoformat(s)
    except ValueError:
        # HISTDATA-style: "20240101 000000"
        try:
            dt = datetime.strptime(s, "%Y%m%d %H%M%S")
        except ValueError:
            # Fallback: "2024-01-01 00:00:00"
            dt = datetime.strptime(s, "%Y-%m-%d %H:%M:%S")
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def load_bars_csv(path: str) -> List[Bar]:
    """Load bars from CSV. Required columns: ts, open, high, low, close.
    Volume optional (defaults to 0)."""
    bars: List[Bar] = []
    with open(path, "r", newline="") as f:
        reader = csv.DictReader(f)
        # Be tolerant of column name casing
        cols_lower = {c.lower(): c for c in reader.fieldnames or []}
        # Timestamp column: accept any of these names
        ts_c = None
        for cand in ("ts", "ts_unix", "timestamp", "datetime", "time"):
            if cand in cols_lower:
                ts_c = cols_lower[cand]
                break
        if ts_c is None:
            raise ValueError(
                f"CSV missing timestamp column "
                f"(tried: ts, ts_unix, timestamp, datetime, time). "
                f"Got: {reader.fieldnames}"
            )
        # OHLC columns must be present
        for r in ("open", "high", "low", "close"):
            if r not in cols_lower:
                raise ValueError(
                    f"CSV missing required column '{r}'. "
                    f"Got: {reader.fieldnames}"
                )
        o_c = cols_lower["open"]
        h_c = cols_lower["high"]
        l_c = cols_lower["low"]
        c_c = cols_lower["close"]
        # Volume column: accept several names (HISTDATA-OTC uses tick_count)
        v_c = None
        for cand in ("volume", "tick_count", "ticks", "vol"):
            if cand in cols_lower:
                v_c = cols_lower[cand]
                break

        for i, row in enumerate(reader):
            try:
                bar = Bar(
                    ts=parse_iso_utc(row[ts_c]),
                    open=float(row[o_c]),
                    high=float(row[h_c]),
                    low=float(row[l_c]),
                    close=float(row[c_c]),
                    volume=float(row[v_c]) if v_c and row.get(v_c) else 0.0,
                )
            except (ValueError, KeyError) as e:
                raise ValueError(f"Row {i+2} parse error: {e}")
            bar.range = bar.high - bar.low
            bar.seq = i
            bars.append(bar)
    return bars


# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------

class Nr2_20Engine:
    """Stateful, bar-by-bar engine. One open position at a time."""

    def __init__(self, params: Params):
        self.p = params
        # Rolling window of recent bars for NR_n classification (max_nr deep)
        self._bar_window: deque = deque(maxlen=params.max_nr)
        # EMA state
        self._ema: Optional[float] = None
        self._ema_history: deque = deque(maxlen=params.trend_slope_lookback + 1)
        # VWAP state (lazy import so vwap.py is only required if filter uses it)
        self._vwap_calc = None
        self._vwap_now: Optional[float] = None
        if params.trend_filter in ("vwap", "both"):
            from vwap import VwapCalc
            self._vwap_calc = VwapCalc(reset=params.vwap_reset)
        # Active box (at most one armed at a time; if multiple clusters fire,
        # the most recent box replaces the older one)
        self._box: Optional[Box] = None
        # Position state
        self._pos: Optional[Position] = None
        # Output
        self.trades: List[Trade] = []
        self._trade_counter: int = 0
        # Diagnostics
        self.boxes_formed: int = 0
        self.boxes_expired_unused: int = 0
        self.entries_rejected_trend: int = 0
        self.entries_rejected_conflict: int = 0
        self.bars_seen: int = 0

    # ------ classification ------

    def _classify_nr(self, bar: Bar) -> int:
        """Return the largest n in [2, max_nr] for which `bar` is NR_n.
        Returns 0 if bar is not even NR_2 (or window too short)."""
        n_window = len(self._bar_window)
        if n_window < 1:   # need at least the previous bar
            return 0
        ranges = [b.range for b in self._bar_window] + [bar.range]
        # ranges is the last (n_window + 1) bar ranges including current
        largest_n = 0
        for n in range(2, self.p.max_nr + 1):
            if len(ranges) < n:
                break
            window = ranges[-n:]
            if bar.range == min(window):
                largest_n = n
        return largest_n

    def _classify_ib(self, bar: Bar) -> bool:
        if not self._bar_window:
            return False
        prev = self._bar_window[-1]
        return bar.high < prev.high and bar.low > prev.low

    # ------ EMA / trend ------

    def _update_ema(self, close: float) -> None:
        k = 2.0 / (self.p.trend_ma_period + 1.0)
        if self._ema is None:
            self._ema = close
        else:
            self._ema = (close - self._ema) * k + self._ema
        self._ema_history.append(self._ema)

    def _ema_trend(self) -> int:
        if len(self._ema_history) <= self.p.trend_slope_lookback:
            return 0
        prior = self._ema_history[0]
        current = self._ema_history[-1]
        if current > prior:
            return 1
        if current < prior:
            return -1
        return 0

    def _vwap_trend(self, close: float) -> int:
        """+1 / -1 / 0 based on close-vs-VWAP and VWAP slope."""
        if self._vwap_calc is None or self._vwap_now is None:
            return 0
        slope = self._vwap_calc.slope(self.p.vwap_slope_lookback)
        if close > self._vwap_now and slope == 1:
            return 1
        if close < self._vwap_now and slope == -1:
            return -1
        return 0

    def _trend(self, bar: Bar) -> int:
        """Combine filter modes per Params.trend_filter."""
        ema_t = self._ema_trend()
        if self.p.trend_filter == "ema":
            return ema_t
        vwap_t = self._vwap_trend(bar.close)
        if self.p.trend_filter == "vwap":
            return vwap_t
        # both — must agree
        if ema_t == vwap_t and ema_t != 0:
            return ema_t
        return 0

    # ------ cluster / box ------

    def _try_form_box(self, bar: Bar) -> None:
        """Scan the last `cluster_lookback` bars (including current). If at least
        `min_qcb` are QCBs, arm a box."""
        # Build the lookback list: most recent (cluster_lookback - 1) bars from
        # window plus the current bar.
        lookback = list(self._bar_window)[-(self.p.cluster_lookback - 1):] + [bar]
        qcbs = [b for b in lookback if b.is_qcb]
        if len(qcbs) < self.p.min_qcb:
            return
        # Box edges are computed from the qualifying compression bars only.
        # We deliberately do NOT include arbitrary non-QCB bars in the span,
        # because a wide-range bar (e.g. the breakout candle itself) sitting
        # at the end of the cluster would otherwise pollute box_top/box_bottom
        # and prevent the breakout-vs-box-edge check from ever firing.
        first_qcb_seq = qcbs[0].seq  # kept for clarity / diagnostics
        span_bars = qcbs
        box_top = max(b.high for b in span_bars)
        box_bottom = min(b.low for b in span_bars)
        nr_count = sum(1 for b in qcbs if b.nr_class >= self.p.min_nr)
        ib_count = sum(1 for b in qcbs if b.is_ib)
        self._box = Box(
            top=box_top,
            bottom=box_bottom,
            formation_bar=bar.seq,
            formation_ts=bar.ts,
            formation_session=session_for_ts(bar.ts),
            nr_class_count=nr_count,
            ib_count=ib_count,
        )
        self.boxes_formed += 1

    # ------ entry ------

    def _try_entry(self, bar: Bar) -> None:
        if self._pos is not None or self._box is None:
            return
        # Box must still be valid (not expired) at the bar evaluating entry.
        # Note: a box formed THIS bar is still age 0 (current_seq - formation == 0).
        if self._box.expired(bar.seq, self.p.box_expiry):
            self.boxes_expired_unused += 1
            self._box = None
            return
        long_signal = bar.close > self._box.top
        short_signal = bar.close < self._box.bottom
        if long_signal and short_signal:
            self.entries_rejected_conflict += 1
            return
        if long_signal:
            if bar.trend != 1:
                self.entries_rejected_trend += 1
                self._box = None  # box consumed (wrong-trend breakout)
                return
            self._open_position(bar, side="LONG")
        elif short_signal:
            if bar.trend != -1:
                self.entries_rejected_trend += 1
                self._box = None
                return
            self._open_position(bar, side="SHORT")

    def _open_position(self, bar: Bar, side: str) -> None:
        spread_cost = self.p.spread_pips * self.p.pip_size
        slip_cost = self.p.slippage_pips * self.p.pip_size
        if side == "LONG":
            entry_px = bar.close + spread_cost / 2 + slip_cost   # buy at ask + slip
            sl_px = self._box.bottom
            risk = entry_px - sl_px
            if risk <= 0:
                # Pathological: box bottom above entry — skip
                return
            tp_px = entry_px + risk * self.p.rr
        else:  # SHORT
            entry_px = bar.close - spread_cost / 2 - slip_cost
            sl_px = self._box.top
            risk = sl_px - entry_px
            if risk <= 0:
                return
            tp_px = entry_px - risk * self.p.rr

        self._pos = Position(
            side=side,
            entry_ts=bar.ts,
            entry_px=entry_px,
            sl_px=sl_px,
            tp_px=tp_px,
            entry_seq=bar.seq,
            box=self._box,
        )
        # Box is now consumed; engine returns to box-hunting once position closes
        self._box = None

    # ------ exit ------

    def _try_exit(self, bar: Bar) -> None:
        if self._pos is None:
            return
        # Don't exit on the entry bar itself (exit eligibility starts next bar)
        if bar.seq == self._pos.entry_seq:
            return

        bars_held = bar.seq - self._pos.entry_seq

        sl_hit = False
        tp_hit = False
        if self._pos.side == "LONG":
            if bar.low <= self._pos.sl_px:
                sl_hit = True
            if bar.high >= self._pos.tp_px:
                tp_hit = True
        else:  # SHORT
            if bar.high >= self._pos.sl_px:
                sl_hit = True
            if bar.low <= self._pos.tp_px:
                tp_hit = True

        if sl_hit and tp_hit:
            if self.p.same_bar_resolution == "sl_first":
                self._close(bar, self._pos.sl_px, "SL", bars_held)
                return
            elif self.p.same_bar_resolution == "tp_first":
                self._close(bar, self._pos.tp_px, "TP", bars_held)
                return
            else:  # reject
                self._close(bar, bar.close, "AMBIGUOUS_REJECT", bars_held)
                return

        if sl_hit:
            self._close(bar, self._pos.sl_px, "SL", bars_held)
            return
        if tp_hit:
            self._close(bar, self._pos.tp_px, "TP", bars_held)
            return

        if bars_held >= self.p.time_stop_bars:
            self._close(bar, bar.close, "TIME_STOP", bars_held)
            return

    def _close(self, bar: Bar, exit_px: float, reason: str, bars_held: int) -> None:
        spread_cost = self.p.spread_pips * self.p.pip_size
        slip_cost = self.p.slippage_pips * self.p.pip_size
        # Apply exit-side cost (subtract on long, add on short = worse fill)
        if self._pos.side == "LONG":
            adj_exit = exit_px - spread_cost / 2 - slip_cost
            pnl_quote = adj_exit - self._pos.entry_px
        else:
            adj_exit = exit_px + spread_cost / 2 + slip_cost
            pnl_quote = self._pos.entry_px - adj_exit

        risk_per_unit = abs(self._pos.entry_px - self._pos.sl_px)
        pnl_R = pnl_quote / risk_per_unit if risk_per_unit > 0 else 0.0

        self._trade_counter += 1
        trade = Trade(
            trade_id=self._trade_counter,
            entry_ts=self._pos.entry_ts,
            exit_ts=bar.ts,
            side=self._pos.side,
            entry_px=self._pos.entry_px,
            exit_px=adj_exit,
            pnl_quote=pnl_quote,
            pnl_R=pnl_R,
            bars_held=bars_held,
            exit_reason=reason,
            box_top=self._pos.box.top,
            box_bottom=self._pos.box.bottom,
            formation_ts=self._pos.box.formation_ts,
            formation_bar=self._pos.box.formation_bar,
            session=self._pos.box.formation_session,
            trend_ma_at_entry=self._ema or 0.0,
            nr_class_count=self._pos.box.nr_class_count,
            ib_count=self._pos.box.ib_count,
        )
        self.trades.append(trade)
        self._pos = None

    # ------ box housekeeping ------

    def _check_box_expiry(self, bar: Bar) -> None:
        if self._box is not None and self._box.expired(bar.seq, self.p.box_expiry):
            self.boxes_expired_unused += 1
            self._box = None

    # ------ main step ------

    def step(self, bar: Bar) -> None:
        """Process one bar. Order matters and mirrors the C++ engine's per-bar loop."""
        self.bars_seen += 1

        # 1a. Update EMA (always — it's the default trend filter)
        self._update_ema(bar.close)
        # 1b. Update VWAP if a VWAP-using filter mode is selected
        if self._vwap_calc is not None:
            self._vwap_now = self._vwap_calc.update(
                bar.ts, bar.high, bar.low, bar.close, bar.volume
            )
        # 1c. Resolve trend per the configured filter mode
        bar.trend = self._trend(bar)

        # 2. Classify this bar (NR_n, IB) — uses prior bars in the window
        bar.nr_class = self._classify_nr(bar)
        bar.is_ib = self._classify_ib(bar)
        bar.is_qcb = (bar.nr_class >= self.p.min_nr) or bar.is_ib

        # 3. Try to exit existing position (uses bar's H/L/close)
        self._try_exit(bar)

        # 4. If still no position, expire any stale box, then try to form a new
        #    one from the cluster including this bar
        if self._pos is None:
            self._check_box_expiry(bar)
            self._try_form_box(bar)

            # 5. Try entry on the box (close-of-bar evaluation)
            self._try_entry(bar)

        # 6. Push this bar into the rolling window for the next step
        self._bar_window.append(bar)


# ---------------------------------------------------------------------------
# Output / metrics
# ---------------------------------------------------------------------------

def write_trades_csv(trades: List[Trade], out_path: str) -> None:
    cols = [
        "trade_id", "entry_ts", "exit_ts", "side", "entry_px", "exit_px",
        "pnl_quote", "pnl_R", "bars_held", "exit_reason", "box_top",
        "box_bottom", "formation_ts", "formation_bar", "session",
        "trend_ma_at_entry", "nr_class_count", "ib_count",
    ]
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for t in trades:
            w.writerow([
                t.trade_id,
                t.entry_ts.isoformat(),
                t.exit_ts.isoformat(),
                t.side,
                f"{t.entry_px:.5f}",
                f"{t.exit_px:.5f}",
                f"{t.pnl_quote:.5f}",
                f"{t.pnl_R:.4f}",
                t.bars_held,
                t.exit_reason,
                f"{t.box_top:.5f}",
                f"{t.box_bottom:.5f}",
                t.formation_ts.isoformat(),
                t.formation_bar,
                t.session,
                f"{t.trend_ma_at_entry:.5f}",
                t.nr_class_count,
                t.ib_count,
            ])


def summarize(trades: List[Trade], engine: Nr2_20Engine) -> dict:
    n = len(trades)
    if n == 0:
        return {
            "trades": 0, "wins": 0, "losses": 0, "win_rate": 0.0,
            "profit_factor": 0.0, "avg_R": 0.0, "total_R": 0.0,
            "max_dd_R": 0.0, "sharpe_trade": 0.0,
            "boxes_formed": engine.boxes_formed,
            "boxes_expired_unused": engine.boxes_expired_unused,
            "entries_rejected_trend": engine.entries_rejected_trend,
            "bars_seen": engine.bars_seen,
        }
    pnls = [t.pnl_R for t in trades]
    wins = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p < 0]
    gross_win = sum(wins)
    gross_loss = abs(sum(losses))
    pf = gross_win / gross_loss if gross_loss > 0 else float("inf")
    avg_R = sum(pnls) / n
    total_R = sum(pnls)
    # Equity curve in R-space, max drawdown
    eq = 0.0
    peak = 0.0
    max_dd = 0.0
    for p in pnls:
        eq += p
        if eq > peak:
            peak = eq
        dd = peak - eq
        if dd > max_dd:
            max_dd = dd
    # Trade-level Sharpe (per-trade), annualized assuming ~250 trading days *
    # ~3 trades/day = 750 trades/yr — caller can re-annualize if needed
    if n > 1:
        mean = avg_R
        var = sum((p - mean) ** 2 for p in pnls) / (n - 1)
        sd = math.sqrt(var)
        sharpe = (mean / sd) * math.sqrt(750) if sd > 0 else 0.0
    else:
        sharpe = 0.0
    return {
        "trades": n,
        "wins": len(wins),
        "losses": len(losses),
        "win_rate": len(wins) / n,
        "profit_factor": pf,
        "avg_R": avg_R,
        "total_R": total_R,
        "max_dd_R": max_dd,
        "sharpe_trade": sharpe,
        "boxes_formed": engine.boxes_formed,
        "boxes_expired_unused": engine.boxes_expired_unused,
        "entries_rejected_trend": engine.entries_rejected_trend,
        "bars_seen": engine.bars_seen,
    }


# ---------------------------------------------------------------------------
# Self-test (synthetic fixture)
# ---------------------------------------------------------------------------

def _synthetic_bars() -> List[Bar]:
    """Build a hand-crafted sequence with a clear LONG setup:
       - Strong uptrend bars to push EMA up
       - Then a tight compression (4 NR/IB bars at price ~2050)
       - Then a strong bullish breakout candle that closes above the box top
       - Followed by continuation (TP hit)
    """
    bars: List[Bar] = []
    base_ts = datetime(2024, 1, 1, 0, 0, 0, tzinfo=timezone.utc)
    seq = 0

    def add(o, h, l, c, minutes_offset):
        nonlocal seq
        ts = datetime(2024, 1, 1, 0, 0, 0, tzinfo=timezone.utc)
        # Increment timestamp by 15min per bar
        from datetime import timedelta
        ts = base_ts + timedelta(minutes=15 * seq)
        b = Bar(ts=ts, open=o, high=h, low=l, close=c, volume=100.0)
        b.range = h - l
        b.seq = seq
        bars.append(b)
        seq += 1

    # 60 strong uptrend bars (push EMA decisively up)
    px = 2000.0
    for i in range(60):
        o = px
        c = px + 1.0
        h = c + 0.3
        l = o - 0.3
        add(o, h, l, c, 0)
        px = c

    # 6 tight compression bars at ~2060 (price has risen from 2000 → 2060)
    # Each ~0.3 wide → forces them to be NR_2/NR_4 etc. relative to prior bars
    box_center = px
    for i in range(6):
        o = box_center
        c = box_center + 0.05
        h = box_center + 0.10
        l = box_center - 0.10
        add(o, h, l, c, 0)

    # Breakout bar: closes well above the box top (~box_center + 0.10)
    o = box_center
    c = box_center + 2.0
    h = c + 0.2
    l = o - 0.1
    add(o, h, l, c, 0)

    # 5 follow-through bars (let TP hit)
    px = c
    for i in range(5):
        o = px
        c = px + 1.5
        h = c + 0.2
        l = o - 0.1
        add(o, h, l, c, 0)
        px = c

    return bars


def run_self_test() -> int:
    print("=" * 60)
    print("SELF-TEST: synthetic LONG setup")
    print("=" * 60)
    params = Params(
        trend_ma_period=20,         # shorter so EMA reacts inside our short fixture
        trend_slope_lookback=3,
        min_nr=2,                   # loosen for synthetic (small range diffs)
        min_qcb=2,
        cluster_lookback=6,
        box_expiry=8,
        rr=1.0,
        time_stop_bars=64,
    )
    bars = _synthetic_bars()
    eng = Nr2_20Engine(params)
    for b in bars:
        eng.step(b)

    summary = summarize(eng.trades, eng)
    print(f"Bars processed:    {summary['bars_seen']}")
    print(f"Boxes formed:      {summary['boxes_formed']}")
    print(f"Boxes expired:     {summary['boxes_expired_unused']}")
    print(f"Trades:            {summary['trades']}")
    print(f"Wins / Losses:     {summary['wins']} / {summary['losses']}")
    print(f"Total R:           {summary['total_R']:.3f}")
    if eng.trades:
        t = eng.trades[0]
        print()
        print("First trade detail:")
        print(f"  side:           {t.side}")
        print(f"  entry_px:       {t.entry_px:.4f}")
        print(f"  exit_px:        {t.exit_px:.4f}")
        print(f"  exit_reason:    {t.exit_reason}")
        print(f"  bars_held:      {t.bars_held}")
        print(f"  pnl_R:          {t.pnl_R:.3f}")
        print(f"  box [bot, top]: [{t.box_bottom:.4f}, {t.box_top:.4f}]")

    # Assertions
    failures = []
    if summary["boxes_formed"] < 1:
        failures.append("Expected at least 1 box to form during compression")
    if summary["trades"] < 1:
        failures.append("Expected at least 1 trade (LONG breakout)")
    elif eng.trades[0].side != "LONG":
        failures.append(f"Expected LONG trade, got {eng.trades[0].side}")
    elif eng.trades[0].exit_reason not in ("TP", "TIME_STOP"):
        failures.append(
            f"Expected TP exit on bullish continuation, got {eng.trades[0].exit_reason}"
        )

    print()
    if failures:
        print("FAIL:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS: synthetic LONG setup produced expected trade")
    return 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="NR2-NR20 + Inside Bar compression-breakout backtest "
                    "(XAUUSD 15m baseline)."
    )
    p.add_argument("--bars", help="Path to OHLC CSV. Required unless --self-test.")
    p.add_argument("--out", default="nr2_20_trades.csv",
                   help="Output trades CSV path. Default: nr2_20_trades.csv")
    p.add_argument("--self-test", action="store_true",
                   help="Run synthetic-fixture self-test and exit.")

    # Strategy params
    p.add_argument("--max-nr", type=int, default=20)
    p.add_argument("--min-nr", type=int, default=4)
    p.add_argument("--min-qcb", type=int, default=2)
    p.add_argument("--cluster-lookback", type=int, default=6)
    p.add_argument("--box-expiry", type=int, default=8)
    p.add_argument("--trend-ma", type=int, default=50, dest="trend_ma_period")
    p.add_argument("--trend-slope-lookback", type=int, default=3)
    p.add_argument("--rr", type=float, default=1.0)
    p.add_argument("--time-stop", type=int, default=64, dest="time_stop_bars")
    p.add_argument("--same-bar-resolution", default="sl_first",
                   choices=["sl_first", "tp_first", "reject"])
    p.add_argument("--spread-pips", type=float, default=0.5)
    p.add_argument("--slippage-pips", type=float, default=0.3)
    p.add_argument("--pip-size", type=float, default=0.10)
    # Trend filter mode (this is what the VWAP-comparison work added)
    p.add_argument("--trend-filter", default="ema",
                   choices=["ema", "vwap", "both"],
                   help="Which trend filter to apply for entry gating. "
                        "ema = original EMA(period) slope. "
                        "vwap = close-vs-VWAP plus VWAP slope. "
                        "both = require both filters to agree.")
    p.add_argument("--vwap-reset", default="daily",
                   choices=["daily", "session", "never"],
                   help="When to reset the VWAP cumulative anchor.")
    p.add_argument("--vwap-slope-lookback", type=int, default=3,
                   help="Bars over which to compute the VWAP slope direction.")
    return p


def main(argv=None) -> int:
    args = build_argparser().parse_args(argv)

    if args.self_test:
        return run_self_test()

    if not args.bars:
        print("ERROR: --bars is required (or use --self-test).", file=sys.stderr)
        return 2

    params = Params(
        max_nr=args.max_nr,
        min_nr=args.min_nr,
        min_qcb=args.min_qcb,
        cluster_lookback=args.cluster_lookback,
        box_expiry=args.box_expiry,
        trend_ma_period=args.trend_ma_period,
        trend_slope_lookback=args.trend_slope_lookback,
        rr=args.rr,
        time_stop_bars=args.time_stop_bars,
        same_bar_resolution=args.same_bar_resolution,
        spread_pips=args.spread_pips,
        slippage_pips=args.slippage_pips,
        pip_size=args.pip_size,
        trend_filter=args.trend_filter,
        vwap_reset=args.vwap_reset,
        vwap_slope_lookback=args.vwap_slope_lookback,
    )

    print(f"Loading bars from {args.bars} ...", file=sys.stderr)
    bars = load_bars_csv(args.bars)
    print(f"  loaded {len(bars)} bars "
          f"({bars[0].ts.isoformat()} → {bars[-1].ts.isoformat()})", file=sys.stderr)

    eng = Nr2_20Engine(params)
    for b in bars:
        eng.step(b)

    write_trades_csv(eng.trades, args.out)
    summary = summarize(eng.trades, eng)

    print(f"\nWrote {len(eng.trades)} trades to {args.out}")
    print("Summary:")
    for k, v in summary.items():
        if isinstance(v, float):
            print(f"  {k:25s} {v:.4f}")
        else:
            print(f"  {k:25s} {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
