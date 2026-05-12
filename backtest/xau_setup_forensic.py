#!/usr/bin/env python3
"""
XAU Forensic Setup Analysis.

Faithful re-implementation of the GoldMidScalperEngine entry-trigger logic
(see /Users/jo/omega_repo/include/GoldMidScalperEngine.hpp). We replay
~15 days of L2 ticks and FIRE a setup every time the engine would have
fired one IGNORING all cadence-gating: cooldown, same-level blocks,
SpreadRegimeGate, flow_live gating, and `can_enter` from the supervisor.
We also bypass the DOM both-walls block (because L2 walls data shape
isn't directly comparable in tape) -- this gives the upper bound of
setup frequency.

Entry trigger conditions kept faithful:
  - Warmup: m_ticks_received >= MIN_ENTRY_TICKS (30)
  - Window: len(m_window) >= STRUCTURE_LOOKBACK (300)
  - Session gate: 06:00 <= UTC hour < 22:00
  - Spread <= MAX_SPREAD (2.5) at IDLE->ARMED moment
  - IDLE -> ARMED: range in [MIN_RANGE, MAX_RANGE] = [8, 20]
  - ARMED extends bracket_hi/lo until out of [MIN_RANGE, MAX_RANGE] or
    mid leaves the bracket (then -> IDLE)
  - mid stays inside bracket for MIN_BREAK_TICKS (5) consecutive ticks
  - cost gate: tp_dist = sl_dist*RR >= spread*2 + 0.12
  - ATR-expansion gate: m_range_history median*1.10 < new range
  - Cold-start: history >= EXPANSION_MIN_HISTORY (5)

After all gates pass we mark a "FIRE event" at this point. The actual
LIVE engine then enters PENDING and waits for the first true breakout
fill (ask>=hi LONG, bid<=lo SHORT). For this forensic we use the same
PENDING->FILL semantics: first tick where ask>=bracket_high or
bid<=bracket_low after FIRE (with PENDING_TIMEOUT_S=120) gives us our
entry direction + entry_price + entry_ts. This matches the live trade
exactly (it was a SHORT fill with TRAIL_HIT in 17s).

After fill we record price at +5s/+17s/+30s/+60s, then RESET to IDLE
immediately (we are NOT actually managing the trade -- no TP/SL/trail,
no cooldown, no same-level block).
"""
import csv
import glob
import os
import sys
from collections import deque
from datetime import datetime, timezone
import bisect

DATA_GLOB = "/sessions/kind-bold-hypatia/mnt/omega_repo/data/l2_ticks_XAUUSD_*.csv"
LEDGER_OUT = "/sessions/kind-bold-hypatia/mnt/omega_repo/backtest/xau_setup_forensic_ledger.csv"

STRUCTURE_LOOKBACK = 300
MIN_ENTRY_TICKS    = 30
MIN_BREAK_TICKS    = 5
MIN_RANGE          = 8.0
MAX_RANGE          = 20.0
SL_FRAC            = 0.6
SL_BUFFER          = 1.0
TP_RR              = 4.0
MAX_SPREAD         = 2.5
PENDING_TIMEOUT_S  = 120
EXPANSION_HISTORY_LEN = 20
EXPANSION_MIN_HISTORY = 5
EXPANSION_MULT     = 1.10
SESSION_START_HOUR_UTC = 6
SESSION_END_HOUR_UTC   = 22
HORIZONS_S = (5, 17, 30, 60)
WIN_THRESHOLD_PTS = 2.0

def in_session(ts_s):
    h = datetime.fromtimestamp(ts_s, tz=timezone.utc).hour
    return SESSION_START_HOUR_UTC <= h < SESSION_END_HOUR_UTC

def median_of(d):
    s = sorted(d)
    n = len(s)
    if n == 0: return 0.0
    if n & 1: return s[n // 2]
    return 0.5 * (s[n // 2 - 1] + s[n // 2])

def process_file(path, ledger_rows, hourly_counts):
    # We need: a window of last 300 mids, plus a fast lookup of price
    # at +k seconds after entry_ts. We hold per-file the entire (ts_s, mid, bid, ask)
    # arrays so we can do binary search for horizons.
    ts_ms_arr  = []
    bid_arr    = []
    ask_arr    = []
    mid_arr    = []
    imb_arr    = []
    with open(path, "r", newline="") as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            try:
                ts_ms = int(row["ts_ms"])
                bid = float(row["bid"]); ask = float(row["ask"])
            except (KeyError, ValueError):
                continue
            if bid <= 0 or ask <= 0: continue
            mid = (bid + ask) * 0.5
            ts_ms_arr.append(ts_ms)
            bid_arr.append(bid); ask_arr.append(ask); mid_arr.append(mid)
            try: imb_arr.append(float(row.get("l2_imb", "0") or 0))
            except ValueError: imb_arr.append(0.0)

    n = len(ts_ms_arr)
    if n == 0: return

    ts_s_arr = [t // 1000 for t in ts_ms_arr]

    # State
    phase = "IDLE"   # IDLE, ARMED, PENDING
    # Engine uses m_window with max len = STRUCTURE_LOOKBACK*2, but takes
    # max/min over the WHOLE deque (not just last 300). We mirror that.
    WIN_CAP = STRUCTURE_LOOKBACK * 2
    window_vals = deque()  # actual values in arrival order, capped to WIN_CAP
    # Monotonic deques storing indices into a global counter `gidx` so we can
    # pop stale entries that fall outside the current window of size WIN_CAP.
    mono_max = deque()  # decreasing -> front is max
    mono_min = deque()  # increasing -> front is min
    gidx = 0  # absolute tick index since file start
    win_left = 0  # absolute index of oldest entry still in window_vals
    ticks_received = 0
    bracket_hi = 0.0
    bracket_lo = 0.0
    inside_ticks = 0
    armed_ts = 0
    range_history = deque(maxlen=EXPANSION_HISTORY_LEN)
    pending_since_idx = -1
    pending_hi = 0.0
    pending_lo = 0.0

    for i in range(n):
        bid = bid_arr[i]; ask = ask_arr[i]; mid = mid_arr[i]
        ts_ms = ts_ms_arr[i]; ts_s = ts_s_arr[i]
        spread = ask - bid

        ticks_received += 1
        # Maintain window_vals + monotonic deques for O(1) max/min
        window_vals.append(mid)
        # Pop from mono tails any values <= mid (for max) / >= mid (for min)
        while mono_max and mono_max[-1][1] <= mid:
            mono_max.pop()
        mono_max.append((gidx, mid))
        while mono_min and mono_min[-1][1] >= mid:
            mono_min.pop()
        mono_min.append((gidx, mid))
        # Evict stale entries beyond WIN_CAP
        if len(window_vals) > WIN_CAP:
            window_vals.popleft()
            win_left += 1
            if mono_max and mono_max[0][0] < win_left:
                mono_max.popleft()
            if mono_min and mono_min[0][0] < win_left:
                mono_min.popleft()
        gidx += 1

        # PENDING (waiting for fill)
        if phase == "PENDING":
            would_fill_long  = ask >= pending_hi
            would_fill_short = bid <= pending_lo
            if would_fill_long or would_fill_short:
                direction = "LONG" if would_fill_long else "SHORT"
                entry_px  = pending_hi if would_fill_long else pending_lo
                entry_ts_s = ts_s
                # Find horizon prices
                horizon_prices = {}
                for hsec in HORIZONS_S:
                    target_ts_ms = ts_ms + hsec * 1000
                    j = bisect.bisect_left(ts_ms_arr, target_ts_ms, lo=i)
                    if j >= n:
                        horizon_prices[hsec] = None
                    else:
                        # For LONG favorable = ask at horizon - entry
                        # For SHORT favorable = entry - bid at horizon
                        if direction == "LONG":
                            horizon_prices[hsec] = ask_arr[j]
                        else:
                            horizon_prices[hsec] = bid_arr[j]
                row = {
                    "ts_ms": ts_ms,
                    "ts_iso": datetime.fromtimestamp(ts_s, tz=timezone.utc).isoformat(),
                    "direction": direction,
                    "entry_price": entry_px,
                    "bracket_hi": pending_hi,
                    "bracket_lo": pending_lo,
                    "range_at_fire": pending_hi - pending_lo,
                    "spread_at_entry": spread,
                    "l2_imb_at_entry": imb_arr[i],
                }
                for hsec in HORIZONS_S:
                    p = horizon_prices[hsec]
                    if p is None:
                        row[f"px_{hsec}s"] = ""
                        row[f"fav_{hsec}s"] = ""
                    else:
                        fav = (p - entry_px) if direction == "LONG" else (entry_px - p)
                        row[f"px_{hsec}s"] = f"{p:.3f}"
                        row[f"fav_{hsec}s"] = f"{fav:.3f}"
                ledger_rows.append(row)
                # Hourly bucket
                hr_key = datetime.fromtimestamp(ts_s, tz=timezone.utc).strftime("%Y-%m-%d %H")
                hourly_counts[hr_key] = hourly_counts.get(hr_key, 0) + 1
                # Reset (ignore cooldown / live management for forensic)
                phase = "IDLE"
                bracket_hi = bracket_lo = 0.0
                inside_ticks = 0
                continue
            # timeout
            if ts_s - armed_ts > PENDING_TIMEOUT_S:
                phase = "IDLE"
                bracket_hi = bracket_lo = 0.0
                inside_ticks = 0
            continue

        # Warmup / window-ready gates
        if ticks_received < MIN_ENTRY_TICKS: continue
        if len(window_vals) < STRUCTURE_LOOKBACK: continue
        # Session gate
        if not in_session(ts_s): continue
        # Spread gate
        if spread > MAX_SPREAD: continue

        # Compute window range over the latest STRUCTURE_LOOKBACK mids
        # (window has up to 2x lookback; engine uses entire m_window for
        # min/max, so we mimic that.)
        # Using max/min over deque -- O(n), n<=600, fast enough at 3.9M ticks
        # we still want to keep it acceptable; use precomputed via two deques
        # would be O(1) but complexity not worth it here.
        w_hi = mono_max[0][1]; w_lo = mono_min[0][1]
        rng = w_hi - w_lo

        if phase == "IDLE":
            if MIN_RANGE <= rng <= MAX_RANGE:
                phase = "ARMED"
                bracket_hi = w_hi; bracket_lo = w_lo
                inside_ticks = 0
                armed_ts = ts_s
            continue

        if phase == "ARMED":
            if w_hi > bracket_hi: bracket_hi = w_hi
            if w_lo < bracket_lo: bracket_lo = w_lo
            rng = bracket_hi - bracket_lo
            if rng > MAX_RANGE or rng < MIN_RANGE:
                phase = "IDLE"; bracket_hi=bracket_lo=0.0; inside_ticks=0
                continue
            # mid inside bracket?
            if bracket_lo <= mid <= bracket_hi:
                inside_ticks += 1
            else:
                phase = "IDLE"; bracket_hi=bracket_lo=0.0; inside_ticks=0
                continue
            if inside_ticks < MIN_BREAK_TICKS:
                continue

            # Cost gate
            sl_dist = rng * SL_FRAC + SL_BUFFER
            tp_dist = sl_dist * TP_RR
            min_tp  = spread * 2.0 + 0.12
            if tp_dist < min_tp:
                phase = "IDLE"; bracket_hi=bracket_lo=0.0; inside_ticks=0
                continue

            # ATR-expansion gate: append, then test
            range_history.append(rng)
            if len(range_history) < EXPANSION_MIN_HISTORY:
                # cold-start block
                phase = "IDLE"; bracket_hi=bracket_lo=0.0; inside_ticks=0
                continue
            med = median_of(range_history)
            if rng < med * EXPANSION_MULT:
                phase = "IDLE"; bracket_hi=bracket_lo=0.0; inside_ticks=0
                continue

            # FIRE -> PENDING
            phase = "PENDING"
            pending_hi = bracket_hi
            pending_lo = bracket_lo
            armed_ts = ts_s
            continue

    print(f"  {os.path.basename(path)}: {n} ticks processed")


def main():
    files = sorted(glob.glob(DATA_GLOB))
    print(f"Found {len(files)} tape files")
    ledger = []
    hourly = {}
    for p in files:
        process_file(p, ledger, hourly)

    print(f"\nTotal fires: {len(ledger)}")

    # Write ledger
    if ledger:
        fieldnames = list(ledger[0].keys())
        with open(LEDGER_OUT, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fieldnames)
            w.writeheader()
            for r in ledger: w.writerow(r)
        print(f"Wrote ledger -> {LEDGER_OUT}")

    # Per-horizon aggregates
    summary = {}
    for h in HORIZONS_S:
        favs = []
        for r in ledger:
            v = r[f"fav_{h}s"]
            if v == "" or v is None: continue
            favs.append(float(v))
        favs.sort()
        if not favs:
            summary[h] = None; continue
        n = len(favs)
        mean = sum(favs)/n
        med  = favs[n//2] if n & 1 else 0.5*(favs[n//2-1]+favs[n//2])
        q1   = favs[n//4]
        q3   = favs[(3*n)//4]
        wins = sum(1 for x in favs if x >= WIN_THRESHOLD_PTS)
        summary[h] = {"n": n, "mean": mean, "median": med, "q1": q1, "q3": q3,
                      "winrate": wins/n, "wins": wins}
    # session-hours per day
    days = sorted({datetime.fromtimestamp(int(r["ts_ms"])//1000, tz=timezone.utc)
                   .strftime("%Y-%m-%d") for r in ledger})
    n_days = len(days)
    n_session_hours = n_days * (SESSION_END_HOUR_UTC - SESSION_START_HOUR_UTC)
    setups_per_session_hour = (len(ledger)/n_session_hours) if n_session_hours else 0.0
    setups_per_day = (len(ledger)/n_days) if n_days else 0.0

    # Print summary
    print("\n--- Per-horizon ---")
    print(f"{'h':>4} {'n':>6} {'mean':>8} {'median':>8} {'Q1':>8} {'Q3':>8} {'winrate':>9}")
    for h in HORIZONS_S:
        s = summary[h]
        if s is None: continue
        print(f"{h:>4} {s['n']:>6} {s['mean']:>8.3f} {s['median']:>8.3f} {s['q1']:>8.3f} {s['q3']:>8.3f} {s['winrate']*100:>8.2f}%")

    print(f"\nDays covered: {n_days}")
    print(f"Setups/day:   {setups_per_day:.2f}")
    print(f"Setups/hr (sessions only): {setups_per_session_hour:.2f}")

    # Determine ranking of the live trade (17s, SHORT, +2.65 gross / lot 0.01
    # corresponds to favorable move of ~2.65 pts).
    live_fav = 2.65
    s17 = summary[17]
    if s17 is not None:
        below = sum(1 for r in ledger
                    if r["fav_17s"] not in ("", None)
                    and float(r["fav_17s"]) <= live_fav)
        pctile = below / s17["n"] * 100.0
        print(f"\nLive trade fav@17s={live_fav}pt -> at percentile {pctile:.1f}")

    # Stash for markdown step
    import json
    out = {
        "n_files": len(files),
        "n_fires": len(ledger),
        "n_days": n_days,
        "days": days,
        "setups_per_day": setups_per_day,
        "setups_per_session_hour": setups_per_session_hour,
        "summary": summary,
        "live_fav_17s": live_fav,
        "live_pctile_17s": pctile if s17 else None,
        "hourly_counts": hourly,
    }
    with open("/tmp/xau_forensic_summary.json", "w") as f:
        json.dump(out, f, default=float, indent=2)
    print("Wrote summary -> /tmp/xau_forensic_summary.json")

if __name__ == "__main__":
    main()
