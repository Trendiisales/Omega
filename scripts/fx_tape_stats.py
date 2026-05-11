#!/usr/bin/env python3
# =============================================================================
# fx_tape_stats.py -- Characterize HistData ASCII FX tick data so SymbolSpec
# values in backtest/microscalper_crtp_sweep.cpp can be anchored to the
# actual distribution rather than gold-ratio guesses.
#
# 2026-05-08 S20 Phase 0 FX (Path B): written after the EURUSD smoke test
# revealed Z-grid boundary saturation + 97% MAX_HOLD timeouts, mirroring the
# indices Phase 0 failure that was iterated through on guesses. This script
# measures the tape characteristics needed to set:
#   max_spread_pts        from spread p99
#   min_sd_pts            from 20-tick mid-stdev p10
#   base_tp_dist_pts      from 20-tick mid-range p50 / max-hold mid-range
#   base_be_trigger_pts   from typical favourable-excursion within max_hold
#   max_hold_sec          from how long the engine NEEDS to capture moves
#   session windows       from per-hour tick density
#   base_entry_z          NOT measured here; from sweep boundary inspection
#
# USAGE
#   python3 scripts/fx_tape_stats.py ~/Tick/EURUSD/HISTDATA_COM_ASCII_EURUSD_T*/DAT_ASCII_EURUSD_T_*.csv
#   python3 scripts/fx_tape_stats.py ~/Tick/EURUSD/HISTDATA_COM_ASCII_EURUSD_T202503/DAT_ASCII_EURUSD_T_202503.csv  # single month
#
# Reads HistData ASCII tick rows: YYYYMMDD HHMMSSmmm,bid,ask,vol
# Output: compact stats dump to stdout. Paste the output back to Claude.
# =============================================================================

import sys
import math
import statistics
from collections import defaultdict
from datetime import datetime, timezone


def parse_ts(s: str) -> int:
    """Parse a timestamp column to unix ms (UTC). Auto-detects three formats:

    1. HistData ASCII:    'YYYYMMDD HHMMSSmmm'  (18 chars)
    2. L2 / Dukascopy:    numeric epoch ms      (13 digits, e.g. 1714000000000)
    3. Numeric epoch s:   numeric epoch seconds (10 digits) -- auto-scaled to ms

    Raises ValueError on parse failure (caller catches and skips line).
    """
    s = s.strip()
    if not s:
        raise ValueError("empty ts")
    # All-digit numeric epoch (L2 captures, Dukascopy headerless, etc.)
    if s.isdigit():
        v = int(s)
        if v > 10**12:
            return v             # already in ms
        elif v > 10**9:
            return v * 1000      # in seconds; rescale
        else:
            return v             # tiny / invalid — let caller filter
    # HistData ASCII layout: digits at positions 0..7 + space/T at 8 + digits 9..17
    if len(s) >= 17 and s[8] in (' ', 'T'):
        yyyy = int(s[0:4]); mm = int(s[4:6]); dd = int(s[6:8])
        hh = int(s[9:11]); mn = int(s[11:13]); sc = int(s[13:15]); ms = int(s[15:18])
        epoch_s = int(datetime(yyyy, mm, dd, hh, mn, sc, tzinfo=timezone.utc).timestamp())
        return epoch_s * 1000 + ms
    raise ValueError(f"unrecognised timestamp shape: {s!r}")


def percentiles(xs, ps):
    """Return percentile values (linear interp) for a sorted list."""
    if not xs:
        return [0.0] * len(ps)
    n = len(xs)
    out = []
    for p in ps:
        if n == 1:
            out.append(xs[0])
            continue
        idx = (p / 100.0) * (n - 1)
        lo = int(math.floor(idx))
        hi = int(math.ceil(idx))
        if lo == hi:
            out.append(xs[lo])
        else:
            frac = idx - lo
            out.append(xs[lo] * (1.0 - frac) + xs[hi] * frac)
    return out


def stdev_window(window):
    """Population stdev of a 20-element window."""
    n = len(window)
    if n < 2:
        return 0.0
    m = sum(window) / n
    var = sum((x - m) ** 2 for x in window) / n
    return math.sqrt(var)


def scan_file(path: str, stats: dict):
    """Pass through one CSV, accumulating into the shared stats dict."""
    print(f"  scanning {path}", flush=True)
    spreads = stats['spreads']
    mids_for_sd = stats['mids_for_sd']
    sd20_samples = stats['sd20_samples']
    range20_samples = stats['range20_samples']
    inter_tick_ms = stats['inter_tick_ms']
    range_60s_samples = stats['range_60s_samples']
    range_180s_samples = stats['range_180s_samples']
    range_600s_samples = stats['range_600s_samples']
    hour_counts = stats['hour_counts']

    window = []
    horizon_60 = []   # (ts_ms, mid)
    horizon_180 = []
    horizon_600 = []
    prev_ts = None
    n = 0

    with open(path, 'r') as f:
        for line in f:
            line = line.rstrip('\r\n')
            if not line:
                continue
            try:
                comma = line.index(',')
                ts = parse_ts(line[:comma])
                rest = line[comma + 1:]
                p1, p2 = rest.split(',')[:2]
                bid_s, ask_s = p1, p2
                bid = float(bid_s); ask = float(ask_s)
                if bid <= 0 or ask <= 0:
                    continue
                if bid > ask:
                    bid, ask = ask, bid
            except (ValueError, IndexError):
                continue

            spread = ask - bid
            mid = (ask + bid) * 0.5
            n += 1

            # Tick density per UTC hour
            hour = (ts // 3600_000) % 24
            hour_counts[hour] += 1

            # Inter-tick interval (sample 1 in 100 to avoid OOM on tens of millions)
            if prev_ts is not None and n % 100 == 0:
                inter_tick_ms.append(ts - prev_ts)
            prev_ts = ts

            # Spread sampling (1 in 50)
            if n % 50 == 0:
                spreads.append(spread)

            # 20-tick window stats: sample every 20 ticks
            window.append(mid)
            if len(window) > 20:
                window.pop(0)
            if len(window) == 20 and n % 20 == 0:
                sd = stdev_window(window)
                rng = max(window) - min(window)
                sd20_samples.append(sd)
                range20_samples.append(rng)

            # Time-horizon ranges (60/180/600s) -- sample every 5s ish
            horizon_60.append((ts, mid))
            horizon_180.append((ts, mid))
            horizon_600.append((ts, mid))
            # Prune older
            while horizon_60 and ts - horizon_60[0][0] > 60_000:
                horizon_60.pop(0)
            while horizon_180 and ts - horizon_180[0][0] > 180_000:
                horizon_180.pop(0)
            while horizon_600 and ts - horizon_600[0][0] > 600_000:
                horizon_600.pop(0)
            if n % 500 == 0 and horizon_60:
                mids = [m for _, m in horizon_60]
                range_60s_samples.append(max(mids) - min(mids))
            if n % 500 == 0 and horizon_180:
                mids = [m for _, m in horizon_180]
                range_180s_samples.append(max(mids) - min(mids))
            if n % 1000 == 0 and horizon_600:
                mids = [m for _, m in horizon_600]
                range_600s_samples.append(max(mids) - min(mids))

    return n


def fmt(v: float, sig: int = 5) -> str:
    if v == 0:
        return "0"
    return f"{v:.{sig}g}"


def main():
    if len(sys.argv) < 2:
        print("usage: fx_tape_stats.py <csv> [<csv>...]", file=sys.stderr)
        return 2

    paths = sys.argv[1:]
    stats = {
        'spreads': [],
        'mids_for_sd': [],
        'sd20_samples': [],
        'range20_samples': [],
        'inter_tick_ms': [],
        'range_60s_samples': [],
        'range_180s_samples': [],
        'range_600s_samples': [],
        'hour_counts': defaultdict(int),
    }

    total = 0
    for p in paths:
        total += scan_file(p, stats)

    # Sort for percentiles
    for k in ('spreads', 'sd20_samples', 'range20_samples', 'inter_tick_ms',
              'range_60s_samples', 'range_180s_samples', 'range_600s_samples'):
        stats[k].sort()

    sp = percentiles(stats['spreads'], [50, 90, 95, 99])
    sd = percentiles(stats['sd20_samples'], [10, 50, 90, 99])
    r20 = percentiles(stats['range20_samples'], [50, 90, 95, 99])
    it = percentiles(stats['inter_tick_ms'], [50, 90, 99])
    r60 = percentiles(stats['range_60s_samples'], [50, 90, 99])
    r180 = percentiles(stats['range_180s_samples'], [50, 90, 99])
    r600 = percentiles(stats['range_600s_samples'], [50, 90, 99])

    print()
    print("=" * 60)
    print(f"FX TAPE STATS")
    print("=" * 60)
    print(f"total ticks parsed: {total:,}")
    print()
    print(f"SPREAD (sampled 1/50):  p50={fmt(sp[0])}  p90={fmt(sp[1])}  p95={fmt(sp[2])}  p99={fmt(sp[3])}")
    print(f"INTER-TICK MS (1/100):  p50={fmt(it[0])}  p90={fmt(it[1])}  p99={fmt(it[2])}")
    print(f"20-TICK STDEV    (1/20): p10={fmt(sd[0])}  p50={fmt(sd[1])}  p90={fmt(sd[2])}  p99={fmt(sd[3])}")
    print(f"20-TICK RANGE    (1/20): p50={fmt(r20[0])}  p90={fmt(r20[1])}  p95={fmt(r20[2])}  p99={fmt(r20[3])}")
    print(f"60s   MID RANGE  (1/500): p50={fmt(r60[0])}  p90={fmt(r60[1])}  p99={fmt(r60[2])}")
    print(f"180s  MID RANGE  (1/500): p50={fmt(r180[0])}  p90={fmt(r180[1])}  p99={fmt(r180[2])}")
    print(f"600s  MID RANGE  (1/1000):p50={fmt(r600[0])}  p90={fmt(r600[1])}  p99={fmt(r600[2])}")
    print()
    print("TICKS PER UTC HOUR (raw counts):")
    total_hours = sum(stats['hour_counts'].values())
    for h in range(24):
        c = stats['hour_counts'].get(h, 0)
        pct = 100.0 * c / max(total_hours, 1)
        bar = '#' * int(pct * 2)
        print(f"  {h:02d}:00  {c:>10,}  {pct:5.1f}%  {bar}")
    print()
    print("RECOMMENDED SymbolSpec ANCHORS (paste back to Claude):")
    print(f"  max_spread_pts    ~= 1.5 * spread_p99    = {fmt(sp[3] * 1.5)}")
    print(f"  min_sd_pts        ~= 0.5 * stdev_p10     = {fmt(sd[0] * 0.5)}")
    print(f"  base_tp_dist_pts  ~= range20_p90         = {fmt(r20[1])}")
    print(f"                       (or 60s_range_p50  = {fmt(r60[0])})")
    print(f"  base_sl_dist_pts  ~= 4 * tp              = {fmt(r20[1] * 4)}")
    print(f"  base_be_trigger   ~= 0.6 * tp            = {fmt(r20[1] * 0.6)}")
    print(f"  base_trail_dist   ~= 0.6 * tp            = {fmt(r20[1] * 0.6)}")
    print(f"  max_hold_sec       -- need: range exceeds tp in <max_hold")
    print(f"                       60s_p50={fmt(r60[0])}  vs tp_anchor={fmt(r20[1])}")
    print(f"                       180s_p50={fmt(r180[0])}  vs sl_anchor={fmt(r20[1] * 4)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
