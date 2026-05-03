#!/usr/bin/env python3
"""
usdjpy_xauusd_fvg_signal_test.py
==================================
Phase 0 viability test for ranked Fair Value Gap (FVG) zones across symbols.

CORE QUESTION: do FVGs (and ranked FVGs in particular) predict price reactions
better than random horizontal levels in the same data?

This is a SNIFF TEST. It does not simulate trades. It measures whether the
zones themselves have predictive power as support/resistance, which is the
prerequisite for any FVG-based engine to work in the first place. If FVGs
don't even beat random levels at predicting bounces, no amount of clever
engine logic on top will save them.

Originally written for XAUUSD; now generalized to any symbol whose tick
data is available in either Dukascopy or HistData ASCII format.

INPUT FORMATS (auto-detected from the first non-empty line of the CSV):
    Dukascopy (header `timestamp,askPrice,bidPrice`, ms-epoch rows):
        timestamp,askPrice,bidPrice
        1709258400133,2044.562,2044.265
    HistData ASCII tick (no header, `YYYYMMDD HHMMSSfff,bid,ask,vol`):
        20240301 000000003,109.234,109.236,0

USAGE:
    python3 scripts/usdjpy_xauusd_fvg_signal_test.py \\
        --symbol   USDJPY \\
        --tick-csv ~/Tick/USDJPY/USDJPY_2024-03_2026-04_combined.csv \\
        --out-dir  ~/fvg_phase0/USDJPY_15min \\
        --start    2025-09-01 \\
        --end      2026-03-01 \\
        --tf       15min

OUTPUTS:
    fvgs.csv               every detected FVG with score, session, ATR, outcome
    random.csv             random horizontal levels (control group)
    summary.txt            bounce-rate stats with FVG vs random, score quartile,
                           ATR quartile, session, direction
    fvg_distribution.png   score distribution + reaction-rate by quartile/session

DESIGN CHOICES (read before complaining about realism):

1. FVG detection: standard 3-bar definition.
       Bullish: bar[i-2].high < bar[i].low (gap is [bar[i-2].high, bar[i].low])
       Bearish: bar[i-2].low > bar[i].high (gap is [bar[i].high, bar[i-2].low])
   The gap zone is the unfilled imbalance.

2. Scoring: composite of the video's stated criteria. We use what's
   actually computable from tick data without an external "real volume"
   feed. Each component is normalized to 0..1 then weighted.
       gap_size_atr     : (gap_height / ATR) clipped to [0, 3]
       displacement     : middle bar body / range, weighted by range/ATR
       trend_alignment  : 1.0 if EMA-aligned with FVG direction, 0.0 against
       tick_volume      : middle bar tick count / rolling mean tick count
       age_decay        : exp(-age_bars / decay_const) - older = lower
   Final score = weighted mean of components. Range [0, 1].

3. Reaction definition: FIRST-TOUCH classification.
   We wait until price first re-enters the FVG zone (mitigation entry),
   then walk each bar forward and ask: did this bar's range cross the
   favorable threshold FIRST or the adverse threshold FIRST?
       BOUNCE  : favorable threshold (>= reaction_atr_mult * ATR in
                 expected direction from zone midpoint) hit on a bar
                 where adverse threshold was NOT yet hit
       BREAK   : adverse threshold hit first
       CHOP    : neither hit within reaction_lookforward bars
   When both thresholds are crossed within the SAME bar (intrabar
   ambiguity that we cannot resolve from OHLC alone), we conservatively
   classify as BREAK.

   Rationale: an earlier version of this script used max-of-window
   favorable vs max-of-window adverse over the FULL lookforward window
   and labeled `bounce` whenever the favorable max was the larger of the
   two. That overcounted bounces in cases where adverse moves preceded
   favorable ones (the trade would have stopped you out before recovery).
   First-touch is strictly more conservative and is the apples-to-apples
   way to ask "does this zone work as support/resistance?".

4. Random baseline: for every detected FVG we generate a control "random
   level" - a horizontal price band of the same height drawn at a
   uniform-random midpoint within +-`random_band_atr_range` ATR of the
   FVG midpoint. Random midpoints within `gap_height` of the actual FVG
   midpoint are REJECTED AND RE-SAMPLED (up to
   `random_overlap_max_attempts` tries) so the control group cannot be
   contaminated by levels that essentially ARE the FVG. Reactions are
   measured identically to the FVG case.

5. Ranking buckets: FVGs are split into 4 score quartiles AND 4 ATR
   quartiles (ATR at entry time). News-day FVGs concentrate in the top
   ATR quartile; if ranking adds value primarily in news-light regimes,
   the ATR-quartile breakdown will show it. Score Q4 - Q1 is the
   acceptance gate for "does the ranking carry information?".

6. Session split: each FVG is tagged with the trading session of its
   formation bar (Asian 00-07 UTC, London 07-12, NY 12-21, Off 21-24).
   Bounce stats are reported per session because gold's Asian profile is
   essentially the inverse of USDJPY's, and aggregating across sessions
   hides regime variation that matters for engine design.

LIMITATIONS:
    * No order-flow data. The video's "% buying volume inside FVG" relies
      on assumptions about candle structure; we use tick count as a proxy
      and call it that. This is the same compromise TradingView makes for
      gold/forex.
    * Mid-price OHLC. Same as the EMA scalp test.
    * "Mitigation" defined as price first touching the zone interior.
      Some traders define it as fully filling the zone. Both are
      reported separately (`fully_mitigated` column).
    * HistData timestamps are assumed UTC. HistData publishes both UTC
      and EST variants of their tick files; if you used the EST variant
      you will need to shift the data, or this script will treat EST
      hours as UTC and mis-bucket sessions.
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


# =============================================================================
# Config
# =============================================================================

@dataclass
class FvgConfig:
    # Timeframe for FVG detection (e.g. "15min", "1h", "4h")
    timeframe: str = "15min"

    # Indicator periods on the chosen timeframe
    atr_period: int = 14
    ema_fast: int = 20
    ema_slow: int = 50
    tick_volume_window: int = 20

    # FVG sizing filters (kill obviously-trivial gaps and news spikes)
    min_gap_atr: float = 0.10
    max_gap_atr: float = 5.0

    # Score weights (weighted mean; sum need not equal 1)
    w_gap_size: float = 1.0
    w_displacement: float = 1.0
    w_trend_align: float = 1.0
    w_tick_volume: float = 1.0
    w_age_decay: float = 0.5
    age_decay_bars: float = 100.0

    # Reaction measurement
    reaction_lookforward: int = 30
    reaction_atr_mult: float = 1.0
    max_age_for_test: int = 500

    # Random control settings
    random_levels_per_fvg: int = 1
    random_band_atr_range: float = 3.0
    random_overlap_max_attempts: int = 50


# =============================================================================
# Tick format detection
# =============================================================================

def _sniff_first_data_line(tick_csv: Path, max_scan_bytes: int = 1_048_576) -> str:
    """Read the first non-empty, non-comment line of `tick_csv`.

    Used by `_detect_tick_format` to figure out how to parse the file.
    Stops scanning after `max_scan_bytes` so a malformed file doesn't hang.
    """
    bytes_read = 0
    with open(tick_csv, "r", encoding="utf-8", errors="replace") as f:
        while bytes_read < max_scan_bytes:
            line = f.readline()
            if not line:
                break
            bytes_read += len(line)
            stripped = line.strip()
            if stripped and not stripped.startswith("#"):
                return stripped
    raise RuntimeError(
        f"could not find a data line in first {max_scan_bytes} bytes of {tick_csv}"
    )


def _detect_tick_format(tick_csv: Path) -> str:
    """Return one of 'dukascopy', 'dukascopy_no_header', 'histdata'.

    Detection rules (in order):
      - line begins with the literal "timestamp" -> dukascopy (with header)
      - first comma-separated field looks like "YYYYMMDD HHMMSSfff" -> histdata
      - first comma-separated field is an integer >= 10**11 (epoch ms) -> dukascopy_no_header
      - otherwise raise
    """
    line = _sniff_first_data_line(tick_csv)
    lower = line.lower()
    if lower.startswith("timestamp") and "," in lower:
        return "dukascopy"

    parts = line.split(",")
    if len(parts) >= 1:
        first = parts[0].strip()
        # HistData tick: "YYYYMMDD HHMMSSfff" -> 8-digit date, space, all-digit time
        if (
            len(first) >= 15
            and first[:8].isdigit()
            and " " in first
        ):
            tail = first.split(" ", 1)[1]
            if tail.isdigit():
                return "histdata"
        # Dukascopy without a header (raw integer epoch ms in the first column)
        try:
            v = int(first)
            if v > 10**11:
                return "dukascopy_no_header"
        except ValueError:
            pass

    raise RuntimeError(
        f"unrecognized tick format. First data line was: {line!r}\n"
        "Expected either:\n"
        "  Dukascopy: 'timestamp,askPrice,bidPrice' header followed by ms-epoch rows\n"
        "  HistData : 'YYYYMMDD HHMMSSfff,bid,ask,vol' rows (no header)"
    )


# =============================================================================
# Tick -> bars
# =============================================================================

def _load_dukascopy_chunked(
    tick_csv: Path,
    start_ms: int,
    end_ms: int,
    has_header: bool,
    chunksize: int,
) -> pd.DataFrame:
    parts: List[pd.DataFrame] = []
    rows_seen = 0
    rows_kept = 0
    t0 = time.time()
    if has_header:
        reader = pd.read_csv(
            tick_csv,
            chunksize=chunksize,
            dtype={"timestamp": np.int64, "askPrice": np.float64, "bidPrice": np.float64},
        )
    else:
        reader = pd.read_csv(
            tick_csv,
            chunksize=chunksize,
            header=None,
            names=["timestamp", "askPrice", "bidPrice"],
            dtype={"timestamp": np.int64, "askPrice": np.float64, "bidPrice": np.float64},
        )
    for i, chunk in enumerate(reader):
        rows_seen += len(chunk)
        chunk = chunk[(chunk["timestamp"] >= start_ms) & (chunk["timestamp"] < end_ms)]
        if len(chunk) == 0:
            continue
        rows_kept += len(chunk)
        parts.append(chunk)
        if i % 5 == 0:
            print(
                f"[load]   chunk {i}: seen={rows_seen:,} kept={rows_kept:,}  "
                f"elapsed={time.time()-t0:.1f}s"
            )
    if not parts:
        raise RuntimeError("no ticks fell within the requested date range")
    df = pd.concat(parts, ignore_index=True)
    print(f"[load] dukascopy done: {len(df):,} ticks in {time.time()-t0:.1f}s")
    return df


def _load_histdata_chunked(
    tick_csv: Path,
    start_ms: int,
    end_ms: int,
    chunksize: int,
) -> pd.DataFrame:
    """Parse HistData ASCII tick format: `YYYYMMDD HHMMSSfff,bid,ask,vol`.

    Timestamps are interpreted as UTC. If your HistData download is in EST
    you will need to shift it before running this script (or session
    bucketing will be wrong). HistData's "T_LAST" / "T_BID_ASK" tick CSVs
    are typically EST, but depend on the download settings - the docstring
    of the file you have should tell you.
    """
    parts: List[pd.DataFrame] = []
    rows_seen = 0
    rows_kept = 0
    t0 = time.time()
    reader = pd.read_csv(
        tick_csv,
        chunksize=chunksize,
        header=None,
        names=["ts_str", "bid", "ask", "vol"],
        dtype={"ts_str": str, "bid": np.float64, "ask": np.float64, "vol": np.float64},
    )
    for i, chunk in enumerate(reader):
        rows_seen += len(chunk)
        dt = pd.to_datetime(
            chunk["ts_str"], format="%Y%m%d %H%M%S%f", utc=True, errors="coerce"
        )
        valid = dt.notna()
        if not valid.any():
            continue
        chunk = chunk[valid].reset_index(drop=True)
        dt = dt[valid].reset_index(drop=True)
        ts_ms = (dt.astype("int64") // 10**6).astype(np.int64)
        mask = (ts_ms >= start_ms) & (ts_ms < end_ms)
        if not mask.any():
            continue
        out = pd.DataFrame({
            "timestamp": ts_ms[mask].values.astype(np.int64),
            "askPrice": chunk["ask"][mask].values.astype(np.float64),
            "bidPrice": chunk["bid"][mask].values.astype(np.float64),
        })
        rows_kept += len(out)
        parts.append(out)
        if i % 5 == 0:
            print(
                f"[load]   chunk {i}: seen={rows_seen:,} kept={rows_kept:,}  "
                f"elapsed={time.time()-t0:.1f}s"
            )
    if not parts:
        raise RuntimeError("no ticks fell within the requested date range")
    df = pd.concat(parts, ignore_index=True)
    print(f"[load] histdata done: {len(df):,} ticks in {time.time()-t0:.1f}s")
    return df


def load_ticks_chunked(
    tick_csv: Path,
    start: pd.Timestamp,
    end: pd.Timestamp,
    chunksize: int = 5_000_000,
) -> pd.DataFrame:
    """Format-aware tick loader. Returns a DataFrame with columns
    `timestamp` (int64 ms epoch), `askPrice`, `bidPrice`."""
    print(f"[load] streaming {tick_csv} chunksize={chunksize:,}")
    fmt = _detect_tick_format(tick_csv)
    print(f"[load] detected format: {fmt}")
    start_ms = int(start.timestamp() * 1000)
    end_ms = int(end.timestamp() * 1000)
    if fmt == "dukascopy":
        return _load_dukascopy_chunked(
            tick_csv, start_ms, end_ms, has_header=True, chunksize=chunksize
        )
    if fmt == "dukascopy_no_header":
        return _load_dukascopy_chunked(
            tick_csv, start_ms, end_ms, has_header=False, chunksize=chunksize
        )
    if fmt == "histdata":
        return _load_histdata_chunked(
            tick_csv, start_ms, end_ms, chunksize=chunksize
        )
    raise RuntimeError(f"unhandled format: {fmt}")


def ticks_to_bars(ticks: pd.DataFrame, timeframe: str) -> pd.DataFrame:
    print(f"[bars] reconstructing {timeframe} OHLC from {len(ticks):,} ticks")
    t0 = time.time()
    ticks = ticks.copy()
    ticks["mid"] = (ticks["askPrice"] + ticks["bidPrice"]) / 2.0
    ticks["spread"] = ticks["askPrice"] - ticks["bidPrice"]
    ticks["dt"] = pd.to_datetime(ticks["timestamp"], unit="ms", utc=True)
    ticks = ticks.set_index("dt")
    grp = ticks.resample(timeframe, label="left", closed="left")
    bars = pd.DataFrame({
        "open":         grp["mid"].first(),
        "high":         grp["mid"].max(),
        "low":          grp["mid"].min(),
        "close":        grp["mid"].last(),
        "spread_mean":  grp["spread"].mean(),
        "tick_count":   grp["mid"].count(),
    }).dropna(subset=["open"])
    bars = bars[bars["tick_count"] > 0]
    print(f"[bars] {len(bars):,} {timeframe} bars in {time.time()-t0:.1f}s")
    return bars


# =============================================================================
# Indicators
# =============================================================================

def ema(series: pd.Series, period: int) -> pd.Series:
    return series.ewm(span=period, adjust=False).mean()


def atr(bars: pd.DataFrame, period: int) -> pd.Series:
    high, low, close = bars["high"], bars["low"], bars["close"]
    prev_close = close.shift(1)
    tr = pd.concat([
        (high - low).abs(),
        (high - prev_close).abs(),
        (low - prev_close).abs(),
    ], axis=1).max(axis=1)
    return tr.ewm(alpha=1.0 / period, adjust=False).mean()


def add_indicators(bars: pd.DataFrame, cfg: FvgConfig) -> pd.DataFrame:
    print("[ind] computing ATR, EMAs, tick-volume baseline")
    bars = bars.copy()
    bars["atr"] = atr(bars, cfg.atr_period)
    bars["ema_fast"] = ema(bars["close"], cfg.ema_fast)
    bars["ema_slow"] = ema(bars["close"], cfg.ema_slow)
    bars["tv_mean"] = bars["tick_count"].rolling(cfg.tick_volume_window).mean()
    return bars


# =============================================================================
# Session classification
# =============================================================================

def classify_session(ts: pd.Timestamp) -> str:
    """UTC-hour-based session split (non-overlapping partition).

    Asian  00-07 UTC
    London 07-12 UTC
    NY     12-21 UTC
    Off    21-24 UTC
    """
    h = ts.hour
    if 0 <= h < 7:
        return "asian"
    if 7 <= h < 12:
        return "london"
    if 12 <= h < 21:
        return "ny"
    return "off"


# =============================================================================
# FVG detection + scoring
# =============================================================================

@dataclass
class Fvg:
    formed_idx: int
    formed_time: pd.Timestamp
    direction: str            # "bull" or "bear"
    zone_low: float
    zone_high: float
    gap_height: float
    # Context filled at formation
    session: str = "unknown"
    atr_at_formation: float = 0.0
    # Score components (0..1 each)
    s_gap_size: float = 0.0
    s_displacement: float = 0.0
    s_trend_align: float = 0.0
    s_tick_volume: float = 0.0
    s_age_decay: float = 1.0
    score_at_formation: float = 0.0
    # Reaction (filled in measure_reactions)
    entry_idx: Optional[int] = None
    entry_time: Optional[pd.Timestamp] = None
    bars_until_entry: Optional[int] = None
    reaction: Optional[str] = None         # "bounce" / "break" / "chop" / "no_entry"
    reaction_magnitude_atr: Optional[float] = None
    fully_mitigated: Optional[bool] = None
    score_at_entry: Optional[float] = None  # includes age decay
    atr_at_entry: Optional[float] = None    # for ATR-quartile bucketing


def detect_fvgs(bars: pd.DataFrame, cfg: FvgConfig) -> List[Fvg]:
    """Walk the bar series and detect every 3-bar FVG. Score each at formation.
    Reaction is measured later in `measure_reactions`."""
    print(f"[fvg] detecting 3-bar FVGs across {len(bars):,} bars")
    o = bars["open"].values
    h = bars["high"].values
    l = bars["low"].values
    c = bars["close"].values
    a = bars["atr"].values
    ef = bars["ema_fast"].values
    es = bars["ema_slow"].values
    tc = bars["tick_count"].values
    tv_mean = bars["tv_mean"].values
    times = bars.index

    fvgs: List[Fvg] = []
    n = len(bars)
    skipped_too_small = 0
    skipped_too_big = 0

    for i in range(2, n):
        atr_i = a[i]
        if not np.isfinite(atr_i) or atr_i <= 0:
            continue

        # Bullish FVG
        if h[i - 2] < l[i]:
            gap = l[i] - h[i - 2]
            ratio = gap / atr_i
            if ratio < cfg.min_gap_atr:
                skipped_too_small += 1
                continue
            if ratio > cfg.max_gap_atr:
                skipped_too_big += 1
                continue
            zone_low, zone_high = h[i - 2], l[i]
            direction = "bull"
        # Bearish FVG
        elif l[i - 2] > h[i]:
            gap = l[i - 2] - h[i]
            ratio = gap / atr_i
            if ratio < cfg.min_gap_atr:
                skipped_too_small += 1
                continue
            if ratio > cfg.max_gap_atr:
                skipped_too_big += 1
                continue
            zone_low, zone_high = h[i], l[i - 2]
            direction = "bear"
        else:
            continue

        # Score components, all clipped to [0, 1]
        # 1. Gap size: 0.1 ATR -> low score, 2 ATR -> max score
        s_gap = float(np.clip((ratio - cfg.min_gap_atr) / (2.0 - cfg.min_gap_atr), 0.0, 1.0))

        # 2. Displacement: middle bar (i-1) body / range, weighted by range/ATR
        mid_body = abs(c[i - 1] - o[i - 1])
        mid_range = h[i - 1] - l[i - 1]
        if mid_range > 0 and atr_i > 0:
            body_frac = mid_body / mid_range
            range_atr = mid_range / atr_i
            s_disp = float(np.clip(body_frac * np.clip(range_atr / 2.0, 0.0, 1.0), 0.0, 1.0))
        else:
            s_disp = 0.0

        # 3. Trend alignment: EMA fast vs slow at bar i
        if np.isfinite(ef[i]) and np.isfinite(es[i]):
            ema_bull = ef[i] > es[i]
            if (direction == "bull" and ema_bull) or (direction == "bear" and not ema_bull):
                s_trend = 1.0
            else:
                s_trend = 0.0
        else:
            s_trend = 0.5

        # 4. Tick volume on middle bar vs rolling mean
        if np.isfinite(tv_mean[i - 1]) and tv_mean[i - 1] > 0:
            tv_ratio = tc[i - 1] / tv_mean[i - 1]
            s_tv = float(np.clip((tv_ratio - 1.0) / 2.0, 0.0, 1.0))  # 1x = 0, 3x = 1
        else:
            s_tv = 0.0

        weights = [cfg.w_gap_size, cfg.w_displacement, cfg.w_trend_align, cfg.w_tick_volume]
        components = [s_gap, s_disp, s_trend, s_tv]
        score_form = float(np.average(components, weights=weights))

        fvgs.append(Fvg(
            formed_idx=i,
            formed_time=times[i],
            direction=direction,
            zone_low=float(zone_low),
            zone_high=float(zone_high),
            gap_height=float(gap),
            session=classify_session(times[i]),
            atr_at_formation=float(atr_i),
            s_gap_size=s_gap,
            s_displacement=s_disp,
            s_trend_align=s_trend,
            s_tick_volume=s_tv,
            s_age_decay=1.0,
            score_at_formation=score_form,
        ))

    print(f"[fvg] found {len(fvgs):,} FVGs  "
          f"(skipped: {skipped_too_small:,} too small, {skipped_too_big:,} too big)")
    return fvgs


# =============================================================================
# Reaction measurement (FIRST-TOUCH)
# =============================================================================

def measure_reactions(fvgs: List[Fvg], bars: pd.DataFrame, cfg: FvgConfig) -> None:
    """For each FVG, find the first bar where price re-enters the zone, then
    walk forward and classify on the FIRST threshold cross (favorable or
    adverse). Same-bar both-cross resolves to BREAK (conservative). Tracks
    `fully_mitigated` independently across the full lookforward window.

    Modifies Fvg objects in place.
    """
    print(f"[react] measuring reactions on {len(fvgs):,} FVGs (first-touch)")
    h = bars["high"].values
    l = bars["low"].values
    a = bars["atr"].values
    times = bars.index
    n = len(bars)
    weights = [cfg.w_gap_size, cfg.w_displacement, cfg.w_trend_align,
               cfg.w_tick_volume, cfg.w_age_decay]

    for fv in fvgs:
        # Search forward from formed_idx + 1 for first entry into the zone.
        max_search = min(n, fv.formed_idx + 1 + cfg.max_age_for_test)
        entry_idx: Optional[int] = None
        for j in range(fv.formed_idx + 1, max_search):
            if h[j] >= fv.zone_low and l[j] <= fv.zone_high:
                entry_idx = j
                break
        if entry_idx is None:
            fv.reaction = "no_entry"
            continue

        fv.entry_idx = entry_idx
        fv.entry_time = times[entry_idx]
        fv.bars_until_entry = entry_idx - fv.formed_idx

        # ATR at entry (used for thresholds and ATR-quartile bucketing)
        atr_at_entry = a[entry_idx]
        fv.atr_at_entry = float(atr_at_entry) if np.isfinite(atr_at_entry) else None

        # Age-decayed score at entry
        age = fv.bars_until_entry
        s_age = float(np.exp(-age / cfg.age_decay_bars))
        fv.s_age_decay = s_age
        components = [fv.s_gap_size, fv.s_displacement, fv.s_trend_align,
                      fv.s_tick_volume, s_age]
        fv.score_at_entry = float(np.average(components, weights=weights))

        if not np.isfinite(atr_at_entry) or atr_at_entry <= 0:
            fv.reaction = "chop"
            fv.reaction_magnitude_atr = 0.0
            fv.fully_mitigated = False
            continue
        threshold = cfg.reaction_atr_mult * atr_at_entry

        zone_mid = (fv.zone_low + fv.zone_high) / 2.0
        end = min(n, entry_idx + 1 + cfg.reaction_lookforward)

        # Walk bars: resolve first-touch eagerly, but keep walking to track
        # fully_mitigated and max-move diagnostics across the full window.
        first_touch_reaction: Optional[str] = None
        first_touch_magnitude_atr: Optional[float] = None
        max_fav_atr = 0.0
        max_adv_atr = 0.0
        fully_mit = False

        for k in range(entry_idx, end):
            up_move = h[k] - zone_mid
            down_move = zone_mid - l[k]

            if fv.direction == "bull":
                fav_move, adv_move = up_move, down_move
            else:
                fav_move, adv_move = down_move, up_move

            fav_atr = fav_move / atr_at_entry
            adv_atr = adv_move / atr_at_entry
            if fav_atr > max_fav_atr:
                max_fav_atr = fav_atr
            if adv_atr > max_adv_atr:
                max_adv_atr = adv_atr

            if first_touch_reaction is None:
                fav_hit = fav_move >= threshold
                adv_hit = adv_move >= threshold
                if fav_hit and adv_hit:
                    # Same-bar ambiguity: conservative BREAK.
                    first_touch_reaction = "break"
                    first_touch_magnitude_atr = -float(adv_atr)
                elif fav_hit:
                    first_touch_reaction = "bounce"
                    first_touch_magnitude_atr = float(fav_atr)
                elif adv_hit:
                    first_touch_reaction = "break"
                    first_touch_magnitude_atr = -float(adv_atr)

            # fully_mitigated tracked across the full window regardless of
            # first-touch outcome - "did the zone get fully traded through?"
            if fv.direction == "bull" and l[k] < fv.zone_low:
                fully_mit = True
            if fv.direction == "bear" and h[k] > fv.zone_high:
                fully_mit = True

        fv.fully_mitigated = fully_mit

        if first_touch_reaction is not None:
            fv.reaction = first_touch_reaction
            fv.reaction_magnitude_atr = first_touch_magnitude_atr
        else:
            fv.reaction = "chop"
            fv.reaction_magnitude_atr = float(max(max_fav_atr, max_adv_atr))


# =============================================================================
# Random-level control with overlap rejection
# =============================================================================

def generate_random_levels(
    fvgs: List[Fvg],
    bars: pd.DataFrame,
    cfg: FvgConfig,
    rng: np.random.Generator,
) -> List[Fvg]:
    """For each FVG, generate a matched random horizontal level with the same
    height and direction. The random midpoint is uniform within
    +-`random_band_atr_range` ATR of the formation close, but rejected and
    re-sampled if it falls within `gap_height` of the actual FVG midpoint.
    This prevents control levels that essentially ARE the FVG zone from
    contaminating the baseline.
    """
    print("[rand] generating random control levels (overlap-rejected)")
    c = bars["close"].values
    a = bars["atr"].values
    randoms: List[Fvg] = []
    skipped_no_valid = 0

    for fv in fvgs:
        atr_i = a[fv.formed_idx]
        if not np.isfinite(atr_i) or atr_i <= 0:
            continue
        mid_price = c[fv.formed_idx]
        fvg_mid = (fv.zone_low + fv.zone_high) / 2.0

        chosen_offset: Optional[float] = None
        for _ in range(cfg.random_overlap_max_attempts):
            offset = rng.uniform(
                -cfg.random_band_atr_range, cfg.random_band_atr_range
            ) * atr_i
            cand_mid = mid_price + offset
            if abs(cand_mid - fvg_mid) >= fv.gap_height:
                chosen_offset = offset
                break
        if chosen_offset is None:
            skipped_no_valid += 1
            continue

        rand_mid = mid_price + chosen_offset
        rand_low = rand_mid - fv.gap_height / 2.0
        rand_high = rand_mid + fv.gap_height / 2.0
        rand_fv = Fvg(
            formed_idx=fv.formed_idx,
            formed_time=fv.formed_time,
            direction=fv.direction,
            zone_low=rand_low,
            zone_high=rand_high,
            gap_height=fv.gap_height,
            session=fv.session,                       # mirror for fair comparison
            atr_at_formation=fv.atr_at_formation,
        )
        randoms.append(rand_fv)
    if skipped_no_valid:
        print(
            f"[rand] skipped {skipped_no_valid:,} FVGs: no non-overlapping random "
            f"midpoint after {cfg.random_overlap_max_attempts} attempts"
        )
    return randoms


# =============================================================================
# Reporting
# =============================================================================

def fvgs_to_df(fvgs: List[Fvg]) -> pd.DataFrame:
    if not fvgs:
        return pd.DataFrame()
    return pd.DataFrame([f.__dict__ for f in fvgs])


def reaction_stats(df: pd.DataFrame, label: str) -> dict:
    if df.empty:
        return {"label": label, "n_total": 0, "n_entered": 0}
    entered = df[df["reaction"] != "no_entry"]
    n = len(entered)
    if n == 0:
        return {"label": label, "n_total": len(df), "n_entered": 0}
    bounces = (entered["reaction"] == "bounce").sum()
    breaks = (entered["reaction"] == "break").sum()
    chops = (entered["reaction"] == "chop").sum()
    return {
        "label": label,
        "n_total": len(df),
        "n_entered": n,
        "no_entry_pct": (len(df) - n) / len(df) * 100,
        "bounce_rate": bounces / n,
        "break_rate": breaks / n,
        "chop_rate": chops / n,
        "bounces": int(bounces),
        "breaks": int(breaks),
        "chops": int(chops),
    }


def write_summary(
    fvgs: List[Fvg],
    randoms: List[Fvg],
    out_path: Path,
    cfg: FvgConfig,
    start: pd.Timestamp,
    end: pd.Timestamp,
    symbol: str,
) -> str:
    lines = [
        "=" * 78,
        f"Phase 0 FVG Signal Test - {symbol} - {cfg.timeframe}",
        "=" * 78,
        f"Window:  {start.date()} -> {end.date()}",
        f"Reaction: lookforward={cfg.reaction_lookforward} bars, "
        f"threshold={cfg.reaction_atr_mult}xATR, classification=FIRST-TOUCH",
        f"Score weights: gap={cfg.w_gap_size} disp={cfg.w_displacement} "
        f"trend={cfg.w_trend_align} tv={cfg.w_tick_volume} age={cfg.w_age_decay}",
        "",
    ]

    fvg_df = fvgs_to_df(fvgs)
    rand_df = fvgs_to_df(randoms)

    fvg_stats = reaction_stats(fvg_df, "ALL FVGs")
    rand_stats = reaction_stats(rand_df, "Random control")

    if fvg_stats["n_total"] == 0:
        lines.append("No FVGs detected.")
        text = "\n".join(lines) + "\n"
        out_path.write_text(text)
        return text

    lines += [
        "OVERALL FVG vs RANDOM",
        f"  FVGs detected:        {fvg_stats['n_total']:,}",
        f"  FVGs ever entered:    {fvg_stats.get('n_entered', 0):,} "
        f"({100 - fvg_stats.get('no_entry_pct', 0):.1f}%)",
        f"  Random levels:        {rand_stats['n_total']:,}",
        f"  Random ever entered:  {rand_stats.get('n_entered', 0):,} "
        f"({100 - rand_stats.get('no_entry_pct', 0):.1f}%)",
        "",
        "BOUNCE RATE (favorable threshold hit FIRST within "
        f"{cfg.reaction_lookforward} bars; same-bar ties -> BREAK)",
    ]
    if fvg_stats.get("n_entered", 0) > 0:
        lines.append(f"  All FVGs:        {fvg_stats['bounce_rate']:.1%}  "
                     f"(n={fvg_stats['n_entered']})")
    if rand_stats.get("n_entered", 0) > 0:
        lines.append(f"  Random control:  {rand_stats['bounce_rate']:.1%}  "
                     f"(n={rand_stats['n_entered']})")
    if (fvg_stats.get("n_entered", 0) > 0 and rand_stats.get("n_entered", 0) > 0):
        edge = fvg_stats["bounce_rate"] - rand_stats["bounce_rate"]
        lines.append(f"  EDGE vs random:  {edge*100:+.1f} pp")

    entered = fvg_df[fvg_df["reaction"] != "no_entry"].copy()

    # Score-quartile bucketing
    score_top_minus_bottom: Optional[float] = None
    if len(entered) >= 20:
        try:
            entered["q_score"] = pd.qcut(
                entered["score_at_entry"], 4,
                labels=["Q1_low", "Q2", "Q3", "Q4_high"],
                duplicates="drop",
            )
            lines += ["", "BOUNCE RATE BY SCORE QUARTILE (score_at_entry, includes age decay)"]
            for q, sub in entered.groupby("q_score", observed=True):
                br = (sub["reaction"] == "bounce").mean()
                avg_score = sub["score_at_entry"].mean()
                lines.append(
                    f"  {q:9s}: bounce={br:.1%}  n={len(sub):,}  "
                    f"avg_score={avg_score:.3f}"
                )
            top = entered[entered["q_score"] == "Q4_high"]
            bot = entered[entered["q_score"] == "Q1_low"]
            if len(top) > 0 and len(bot) > 0:
                top_br = (top["reaction"] == "bounce").mean()
                bot_br = (bot["reaction"] == "bounce").mean()
                score_top_minus_bottom = top_br - bot_br
                lines.append(
                    f"  Top quartile - bottom quartile: {(top_br - bot_br)*100:+.1f} pp"
                )
                if rand_stats.get("n_entered", 0) > 0:
                    lines.append(
                        f"  Top quartile - random:          "
                        f"{(top_br - rand_stats['bounce_rate'])*100:+.1f} pp"
                    )
        except ValueError:
            lines.append("  (not enough score variation to bucket into quartiles)")

    # ATR-quartile bucketing (news / volatility regime)
    if len(entered) >= 20 and "atr_at_entry" in entered.columns:
        valid_atr = entered.dropna(subset=["atr_at_entry"]).copy()
        if len(valid_atr) >= 20:
            try:
                valid_atr["q_atr"] = pd.qcut(
                    valid_atr["atr_at_entry"], 4,
                    labels=["A1_low", "A2", "A3", "A4_high"],
                    duplicates="drop",
                )
                lines += [
                    "",
                    "BOUNCE RATE BY ATR QUARTILE "
                    "(atr_at_entry; A4 = high-vol / news regime)",
                ]
                for q, sub in valid_atr.groupby("q_atr", observed=True):
                    br = (sub["reaction"] == "bounce").mean()
                    avg_atr = sub["atr_at_entry"].mean()
                    lines.append(
                        f"  {q:8s}: bounce={br:.1%}  n={len(sub):,}  "
                        f"avg_atr={avg_atr:.6f}"
                    )
                top_a = valid_atr[valid_atr["q_atr"] == "A4_high"]
                bot_a = valid_atr[valid_atr["q_atr"] == "A1_low"]
                if len(top_a) > 0 and len(bot_a) > 0:
                    top_br = (top_a["reaction"] == "bounce").mean()
                    bot_br = (bot_a["reaction"] == "bounce").mean()
                    lines.append(
                        f"  Low-ATR quartile - High-ATR: "
                        f"{(bot_br - top_br)*100:+.1f} pp  "
                        f"(positive = high-vol regime fares worse)"
                    )
            except ValueError:
                lines.append("  (not enough ATR variation to bucket into quartiles)")

    # Direction split
    if not entered.empty:
        lines += ["", "BY DIRECTION"]
        for d, sub in entered.groupby("direction"):
            br = (sub["reaction"] == "bounce").mean()
            lines.append(f"  {d:5s}: bounce={br:.1%}  n={len(sub):,}")

    # Session split
    if not entered.empty and "session" in entered.columns:
        lines += [
            "",
            "BY SESSION (UTC: asian 00-07, london 07-12, ny 12-21, off 21-24)",
        ]
        for s, sub in entered.groupby("session"):
            br = (sub["reaction"] == "bounce").mean()
            lines.append(f"  {s:7s}: bounce={br:.1%}  n={len(sub):,}")

    # Component-level: which score components actually predict reaction?
    if len(entered) >= 50:
        lines += ["", "COMPONENT vs OUTCOME (correlation with bounce=1, break=-1, chop=0)"]
        outcome_num = entered["reaction"].map({"bounce": 1, "chop": 0, "break": -1})
        for comp in [
            "s_gap_size", "s_displacement", "s_trend_align",
            "s_tick_volume", "s_age_decay", "gap_height",
            "bars_until_entry", "atr_at_entry",
        ]:
            if comp in entered.columns:
                vals = pd.to_numeric(entered[comp], errors="coerce")
                mask = vals.notna() & outcome_num.notna()
                if mask.sum() > 10:
                    corr = float(np.corrcoef(vals[mask], outcome_num[mask])[0, 1])
                    lines.append(f"  {comp:20s}: corr={corr:+.3f}")

    # Acceptance gates
    lines += ["", "ACCEPTANCE GATES (Phase 0 -> Phase 1)"]
    n_for_stats = fvg_stats.get("n_entered", 0)
    lines.append(
        f"  >= 200 entered FVGs               : "
        f"{'PASS' if n_for_stats >= 200 else 'FAIL'}  ({n_for_stats})"
    )

    if (fvg_stats.get("n_entered", 0) > 0 and rand_stats.get("n_entered", 0) > 0):
        edge = fvg_stats["bounce_rate"] - rand_stats["bounce_rate"]
        lines.append(
            f"  FVG bounce rate - random >= 5pp   : "
            f"{'PASS' if edge >= 0.05 else 'FAIL'}  ({edge*100:+.1f} pp)"
        )

    if score_top_minus_bottom is not None:
        lines.append(
            f"  Top score quartile - bottom>= 10pp: "
            f"{'PASS' if score_top_minus_bottom >= 0.10 else 'FAIL'}  "
            f"({score_top_minus_bottom*100:+.1f} pp)"
        )

    lines.append("=" * 78)
    text = "\n".join(lines) + "\n"
    out_path.write_text(text)
    return text


def write_distribution_chart(fvgs: List[Fvg], out_path: Path) -> None:
    df = fvgs_to_df(fvgs)
    if df.empty:
        return
    entered = df[df["reaction"] != "no_entry"].copy()
    if len(entered) < 20:
        return

    fig, ax = plt.subplots(2, 2, figsize=(13, 9))

    # Score histogram
    ax[0, 0].hist(entered["score_at_entry"].dropna(), bins=30,
                  color="steelblue", alpha=0.7)
    ax[0, 0].set_xlabel("score_at_entry (incl. age decay)")
    ax[0, 0].set_ylabel("count")
    ax[0, 0].set_title("FVG score distribution")
    ax[0, 0].grid(alpha=0.3)

    # Reaction breakdown overall
    rx_counts = entered["reaction"].value_counts()
    rx_color_map = {"bounce": "green", "break": "red", "chop": "gray"}
    rx_colors = [rx_color_map.get(r, "steelblue") for r in rx_counts.index]
    ax[0, 1].bar(rx_counts.index, rx_counts.values, color=rx_colors)
    ax[0, 1].set_title("Reaction outcome (all FVGs)")
    ax[0, 1].set_ylabel("count")
    ax[0, 1].grid(alpha=0.3, axis="y")

    # Bounce rate by score quartile
    try:
        entered["q_score"] = pd.qcut(
            entered["score_at_entry"], 4,
            labels=["Q1", "Q2", "Q3", "Q4"],
            duplicates="drop",
        )
        br_q = entered.groupby("q_score", observed=True).apply(
            lambda s: (s["reaction"] == "bounce").mean(),
            include_groups=False,
        )
        ax[1, 0].bar(range(len(br_q)), br_q.values, color="seagreen")
        ax[1, 0].set_xticks(range(len(br_q)))
        ax[1, 0].set_xticklabels([str(q) for q in br_q.index])
        ax[1, 0].set_ylabel("bounce rate")
        ax[1, 0].set_title("Bounce rate by score quartile")
        ax[1, 0].grid(alpha=0.3, axis="y")
    except ValueError:
        ax[1, 0].text(0.5, 0.5, "insufficient variation",
                      ha="center", va="center")

    # Bounce rate by session (or fall back to bars-until-entry)
    if "session" in entered.columns and entered["session"].nunique() > 1:
        sess_br = entered.groupby("session").apply(
            lambda s: (s["reaction"] == "bounce").mean(),
            include_groups=False,
        )
        ax[1, 1].bar(range(len(sess_br)), sess_br.values, color="darkorange")
        ax[1, 1].set_xticks(range(len(sess_br)))
        ax[1, 1].set_xticklabels([str(s) for s in sess_br.index])
        ax[1, 1].set_ylabel("bounce rate")
        ax[1, 1].set_title("Bounce rate by session")
        ax[1, 1].grid(alpha=0.3, axis="y")
    else:
        ax[1, 1].hist(entered["bars_until_entry"].dropna(), bins=40,
                      color="darkorange", alpha=0.7)
        ax[1, 1].set_xlabel("bars between formation and entry")
        ax[1, 1].set_ylabel("count")
        ax[1, 1].set_title("How quickly do FVGs get tested?")
        ax[1, 1].grid(alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


# =============================================================================
# Symbol auto-detection (for cache filename / report header)
# =============================================================================

KNOWN_SYMBOLS = (
    "XAUUSD", "XAGUSD", "EURUSD", "GBPUSD", "USDJPY", "AUDUSD", "USDCAD",
    "USDCHF", "NZDUSD", "EURJPY", "GBPJPY", "EURGBP", "AUDJPY", "EURAUD",
    "CHFJPY", "CADJPY", "AUDNZD",
)


def _derive_symbol_from_path(p: Path) -> Optional[str]:
    stem = p.stem.upper()
    for sym in KNOWN_SYMBOLS:
        if sym in stem:
            return sym
    return None


# =============================================================================
# Main
# =============================================================================

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--symbol", type=str, default=None,
        help="Symbol name (e.g. XAUUSD, USDJPY). If omitted, derived from "
             "--tick-csv filename. Used in cache file naming and report header."
    )
    ap.add_argument("--tick-csv", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--start", required=True, type=str)
    ap.add_argument("--end", required=True, type=str)
    ap.add_argument(
        "--tf", default="15min", type=str,
        help="Bar timeframe for FVG detection (e.g. 15min, 1h, 4h)"
    )
    ap.add_argument("--use-bar-cache", action="store_true")
    args = ap.parse_args()

    cfg = FvgConfig()
    cfg.timeframe = args.tf
    args.out_dir.mkdir(parents=True, exist_ok=True)
    start = pd.Timestamp(args.start, tz="UTC")
    end = pd.Timestamp(args.end, tz="UTC")

    # Resolve symbol
    if args.symbol is None:
        derived = _derive_symbol_from_path(args.tick_csv)
        if derived is None:
            print(
                f"[warn] could not derive symbol from {args.tick_csv.name}; "
                f"using 'UNKNOWN'. Pass --symbol to override."
            )
            args.symbol = "UNKNOWN"
        else:
            args.symbol = derived
            print(f"[symbol] auto-derived: {args.symbol}")
    else:
        args.symbol = args.symbol.upper()

    bar_cache = (
        args.out_dir
        / f"bars_{args.symbol}_{args.tf}_{args.start}_{args.end}.pkl"
    )

    if args.use_bar_cache and bar_cache.exists():
        print(f"[cache] loading {bar_cache}")
        bars = pd.read_pickle(bar_cache)
    else:
        ticks = load_ticks_chunked(args.tick_csv, start, end)
        bars = ticks_to_bars(ticks, cfg.timeframe)
        bars.to_pickle(bar_cache)
        print(f"[cache] wrote {bar_cache.name}")
        del ticks

    bars = add_indicators(bars, cfg)
    fvgs = detect_fvgs(bars, cfg)
    measure_reactions(fvgs, bars, cfg)

    rng = np.random.default_rng(seed=42)
    randoms = generate_random_levels(fvgs, bars, cfg, rng)
    measure_reactions(randoms, bars, cfg)

    fvg_df = fvgs_to_df(fvgs)
    rand_df = fvgs_to_df(randoms)
    if not fvg_df.empty:
        fvg_df.to_csv(args.out_dir / "fvgs.csv", index=False)
    if not rand_df.empty:
        rand_df.to_csv(args.out_dir / "random.csv", index=False)

    summary = write_summary(
        fvgs, randoms, args.out_dir / "summary.txt",
        cfg, start, end, args.symbol,
    )
    write_distribution_chart(fvgs, args.out_dir / "fvg_distribution.png")
    print()
    print(summary)
    print(f"Outputs written to: {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
