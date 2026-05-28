#!/usr/bin/env python3
"""
sunday_open_bracket_measure.py -- viability measurement for time-triggered
                                   straddle bracket at Sun 22:00 UTC open +
                                   daily 22:00 UTC reopen events on XAUUSD.

Built 2026-05-28 per operator directive after bracket_gold compression-mode
backtest verdict (PF=0.62 over 2yr, walk-forward failed). Hypothesis: a
time-triggered bracket fired at known vol-expansion events (weekend gap +
daily-reopen liquidity handoff) may have edge where compression-triggered
does not.

For each event (Sun open + daily 22:00 UTC) in the 2yr DukasCopy XAUUSD
tape, this measures:
  - mid + spread at T0
  - realized 5/15/30/60-min range from T0
  - per bracket-distance N in {3, 5, 8, 12, 20}, simulate:
      * arm LONG_STOP = mid + N + 0.5*spread
      * arm SHORT_STOP = mid - N - 0.5*spread
      * walk forward up to 60min; first stop hit = entry
      * symmetric 1:1 R:R = TP at entry+N, SL at entry-N (LONG)
      * record outcome (TP/SL/timeout) + pts P&L
      * subtract cost = full_spread + retail_multiplier * full_spread
        (assume retail spread is 4x DukasCopy interbank)

Output: per-event CSV + aggregate table by (N, session_type).

USAGE:
    python3 sunday_open_bracket_measure.py /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv

OUTPUT:
    /tmp/sunday_bracket_events.csv     -- one row per event * N
    stdout: aggregate summary
"""
import sys
import csv
from datetime import datetime, timezone, timedelta
from collections import defaultdict

RETAIL_SPREAD_MULTIPLIER = 4.0   # retail = 4x DukasCopy interbank, conservative
TRIGGER_HOUR_UTC = 22            # daily reopen + Sun open hour
HOLD_MINUTES_PRE_FILL = 60       # how long to wait for stop fill
HOLD_MINUTES_POST_FILL = 60      # how long to hold after fill before timeout exit
BRACKET_DISTANCES_PTS = [3, 5, 8, 12, 20]


def parse_row(line: str):
    """Parse DukasCopy YYYYMMDD,HH:MM:SS,bid,ask -> (ts_dt, bid, ask) or None."""
    try:
        d, t, b, a = line.strip().split(',')
        if len(d) != 8 or len(t) != 8:
            return None
        Y = int(d[:4]); M = int(d[4:6]); D = int(d[6:])
        h = int(t[:2]); m = int(t[3:5]); s = int(t[6:])
        bid = float(b); ask = float(a)
        if bid <= 0 or ask <= 0 or ask < bid:
            return None
        ts = datetime(Y, M, D, h, m, s, tzinfo=timezone.utc)
        return (ts, bid, ask)
    except Exception:
        return None


def is_event_trigger(prev_ts, this_ts):
    """
    Return event_type str or None.
    Sunday open: prev gap > 30h (typical weekend Fri 22:00 -> Sun 22:00 = 48h).
    Daily reopen: prev gap > 3min crossing the 22:00 UTC boundary.
    """
    if prev_ts is None:
        return None
    gap = (this_ts - prev_ts).total_seconds()
    if gap > 30 * 3600:                # weekend gap
        return "Sunday"
    if gap > 180 and this_ts.hour == TRIGGER_HOUR_UTC and prev_ts.hour != TRIGGER_HOUR_UTC:
        return "DailyReopen"
    return None


def simulate_bracket(t0, mid0, spread0, future_ticks, N):
    """
    Walk future_ticks (already filtered to <= 60min post T0).
    LONG_STOP  = mid0 + N + 0.5 * spread0  (place above mid by N + half-spread)
    SHORT_STOP = mid0 - N - 0.5 * spread0  (place below mid by N + half-spread)
    First tick where ask >= LONG_STOP -> LONG entry at LONG_STOP (no extra slip
                                          beyond half-spread already in trigger).
    First tick where bid <= SHORT_STOP -> SHORT entry at SHORT_STOP.
    After fill: TP at entry + N (LONG) or entry - N (SHORT).
                SL at entry - N (LONG) or entry + N (SHORT).
                Timeout after HOLD_MINUTES_POST_FILL minutes -> exit at mid.
    Return dict with side, outcome, gross_pts, hold_sec.
    """
    long_stop  = mid0 + N + 0.5 * spread0
    short_stop = mid0 - N - 0.5 * spread0
    fill = None
    fill_idx = None
    for i, (ts, b, a) in enumerate(future_ticks):
        if a >= long_stop:
            fill = ("LONG", long_stop, ts)
            fill_idx = i
            break
        if b <= short_stop:
            fill = ("SHORT", short_stop, ts)
            fill_idx = i
            break

    if fill is None:
        return dict(side="NONE", outcome="NO_FILL",
                    gross_pts=0.0, hold_sec=0,
                    arm_spread=spread0)

    side, entry, fill_ts = fill
    if side == "LONG":
        tp = entry + N
        sl = entry - N
    else:
        tp = entry - N
        sl = entry + N

    # Walk from fill onward
    post_ticks = future_ticks[fill_idx + 1:]
    fill_deadline = fill_ts + timedelta(minutes=HOLD_MINUTES_POST_FILL)
    outcome = None
    exit_px = None
    exit_ts = None
    for ts, b, a in post_ticks:
        if ts > fill_deadline:
            break
        # intra-tick TP/SL check, conservative ordering: SL before TP same tick
        if side == "LONG":
            if b <= sl:
                outcome = "SL"; exit_px = sl; exit_ts = ts; break
            if a >= tp:
                outcome = "TP"; exit_px = tp; exit_ts = ts; break
        else:
            if a >= sl:
                outcome = "SL"; exit_px = sl; exit_ts = ts; break
            if b <= tp:
                outcome = "TP"; exit_px = tp; exit_ts = ts; break

    if outcome is None:
        # timeout exit at last seen mid
        last = post_ticks[-1] if post_ticks else (fill_ts, entry, entry)
        last_ts, last_b, last_a = last
        exit_px = (last_b + last_a) * 0.5
        exit_ts = last_ts
        outcome = "TIMEOUT"

    if side == "LONG":
        gross_pts = exit_px - entry
    else:
        gross_pts = entry - exit_px

    hold_sec = int((exit_ts - fill_ts).total_seconds())
    return dict(side=side, outcome=outcome,
                gross_pts=gross_pts, hold_sec=hold_sec,
                arm_spread=spread0)


def main():
    if len(sys.argv) < 2:
        print("Usage: sunday_open_bracket_measure.py <xau_tick_csv>", file=sys.stderr)
        sys.exit(1)
    path = sys.argv[1]
    out_path = "/tmp/sunday_bracket_events.csv"

    # Stream the file, detect events, capture next 60min of ticks per event.
    events = []          # list of (event_type, T0_ts, mid0, spread0)
    pending_windows = [] # list of (event_idx, end_ts, ticks_list)
    prev_ts = None
    line_count = 0
    last_progress = 0

    print(f"[MEASURE] scanning {path} ...", file=sys.stderr)
    with open(path, 'r') as f:
        for line in f:
            line_count += 1
            row = parse_row(line)
            if row is None:
                continue
            ts, bid, ask = row

            # Detect event
            evt = is_event_trigger(prev_ts, ts)
            if evt is not None:
                mid0 = (bid + ask) * 0.5
                spread0 = ask - bid
                idx = len(events)
                events.append({
                    "idx": idx, "type": evt, "T0": ts,
                    "mid0": mid0, "spread0": spread0,
                    "ticks": [],
                })
                pending_windows.append((idx, ts + timedelta(minutes=HOLD_MINUTES_PRE_FILL + HOLD_MINUTES_POST_FILL)))

            # Append tick to all open windows
            for ev_idx, end_ts in pending_windows:
                if ts <= end_ts:
                    events[ev_idx]["ticks"].append((ts, bid, ask))
            # Drop expired windows
            pending_windows = [(i, e) for (i, e) in pending_windows if ts <= e]

            prev_ts = ts

            if line_count - last_progress >= 5_000_000:
                last_progress = line_count
                print(f"\r[MEASURE] {line_count // 1_000_000}M lines, {len(events)} events, "
                      f"{len(pending_windows)} open windows  ", end='', file=sys.stderr)
    print(f"\n[MEASURE] done: {line_count} lines, {len(events)} events", file=sys.stderr)

    # Simulate brackets for each event * N
    rows = []
    for ev in events:
        for N in BRACKET_DISTANCES_PTS:
            sim = simulate_bracket(ev["T0"], ev["mid0"], ev["spread0"], ev["ticks"], N)
            rows.append({
                "event_idx": ev["idx"],
                "event_type": ev["type"],
                "T0": ev["T0"].isoformat(),
                "weekday": ev["T0"].strftime("%a"),
                "mid0": round(ev["mid0"], 3),
                "spread0_pts": round(ev["spread0"], 4),
                "N_pts": N,
                "side": sim["side"],
                "outcome": sim["outcome"],
                "gross_pts": round(sim["gross_pts"], 4),
                "hold_sec": sim["hold_sec"],
                # Realised cost in pts (per round-trip):
                #   half-spread on entry + full DukasCopy spread on exit (TP fills
                #   at limit, SL/TIMEOUT cost a half-spread; pessimistic =
                #   full-spread to cover broker widening).
                # Retail multiplier applied to dukascopy spread.
                "cost_pts": round(ev["spread0"] * RETAIL_SPREAD_MULTIPLIER * 1.5, 4),
                "net_pts": round(sim["gross_pts"] - ev["spread0"] * RETAIL_SPREAD_MULTIPLIER * 1.5, 4),
            })

    # Write per-event CSV
    print(f"[MEASURE] writing {out_path} ({len(rows)} rows)", file=sys.stderr)
    with open(out_path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    # Aggregate
    agg = defaultdict(lambda: dict(n=0, wins=0, losses=0, no_fill=0,
                                   gross=0.0, net=0.0, gw=0.0, gl=0.0))
    for r in rows:
        key = (r["event_type"], r["N_pts"])
        a = agg[key]
        a["n"] += 1
        if r["outcome"] == "NO_FILL":
            a["no_fill"] += 1
            continue
        a["gross"] += r["gross_pts"]
        a["net"]   += r["net_pts"]
        if r["net_pts"] > 0:
            a["wins"] += 1; a["gw"] += r["net_pts"]
        else:
            a["losses"] += 1; a["gl"] += -r["net_pts"]

    print()
    print(f"  Retail spread multiplier: {RETAIL_SPREAD_MULTIPLIER}x DukasCopy")
    print(f"  Cost per trade: 1.5 * retail_spread (entry half + exit full)")
    print(f"  Hold pre-fill: {HOLD_MINUTES_PRE_FILL}min | post-fill: {HOLD_MINUTES_POST_FILL}min")
    print()
    print(f"  {'event_type':<12} {'N':>3} {'n':>4} {'no_fill':>7} {'filled':>6} {'WR':>6} "
          f"{'avg_gross':>9} {'avg_net':>8} {'PF':>6}  {'tot_net':>10}")
    print("  " + "-" * 100)
    for key in sorted(agg.keys()):
        et, N = key
        a = agg[key]
        filled = a["n"] - a["no_fill"]
        wr = (100.0 * a["wins"] / filled) if filled else 0.0
        pf = (a["gw"] / a["gl"]) if a["gl"] > 0 else 0.0
        avg_gross = (a["gross"] / filled) if filled else 0.0
        avg_net   = (a["net"]   / filled) if filled else 0.0
        print(f"  {et:<12} {N:>3} {a['n']:>4} {a['no_fill']:>7} {filled:>6} "
              f"{wr:>5.1f}% {avg_gross:>9.3f} {avg_net:>8.3f} {pf:>6.2f}  {a['net']:>+10.2f}")


if __name__ == "__main__":
    main()
