#!/usr/bin/env python3
"""
apr17_replay.py  --  Apr 17 GoldFlow drift replay simulator

Purpose:
    Reconstruct what RegimeGovernor::detect() drift WOULD have produced if
    Fix A had been live on Apr 17, and compare to the drift actually logged.
    Output lets Jo see:
      1. Whether logged drift was frozen during GoldStack position windows
         (the bug Fix A targets).
      2. Whether drift was ALREADY biased before any position opened
         (which would indicate a second bug -- state carrying across UTC
         day rollover or bad initialization).
      3. Per-session-slot LONG:SHORT attempt distribution, logged vs
         projected-post-fix.

What this script does NOT do:
    - Re-implement engine decision logic (CVD gates, window thresholds, etc.)
    - Speculate about Apr 17 profitability
    - Modify any Omega code

Replay model:
    ewm_fast  alpha = 0.05
    ewm_slow  alpha = 0.005
    drift     = ewm_fast - ewm_slow
    Seeded from the FIRST mid of the day (clean-boot assumption). If logged
    drift at that first tick differs materially from 0, the discrepancy is
    reported as "init_bias".

Inputs:
    --log   path to omega_2026-04-17.log  (required)

Outputs (written next to --log):
    apr17_replay_ticks.csv       every XAUUSD tick: ts, mid, sim_drift
    apr17_replay_drift.csv       every [GFE-NOSIG]: ts, logged_drift,
                                 sim_drift, delta, pos_open, slot
    apr17_replay_freeze.csv      detected freeze windows:
                                 start_ts, end_ts, duration_s, drift_held
    apr17_replay_summary.txt     human-readable summary + LONG:SHORT
                                 attempt distribution per slot

Usage on VPS (PowerShell):
    python C:\\Omega\\apr17_replay.py --log C:\\Omega\\logs\\tmp_17\\omega_2026-04-17.log

Exit codes:
    0  success
    1  log not found or unreadable
    2  parse produced zero ticks (format mismatch)
"""

import argparse
import csv
import os
import re
import sys
from collections import defaultdict


# -------------------------------------------------------------------------
# Regex patterns (anchored, tested against samples from the Apr 17 log)
# -------------------------------------------------------------------------

RE_TICK_XAU = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[TICK\] XAUUSD (?P<bid>[\d.]+)/(?P<ask>[\d.]+)'
)

RE_GFE_NOSIG = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[GFE-NOSIG\] drift=(?P<drift>-?[\d.]+) '
    r'atr=(?P<atr>-?[\d.]+) eff_thresh=(?P<thr>-?[\d.]+) '
    r'fast_long=(?P<fl>\d+)\s*fast_short=(?P<fs>\d+) '
    r'need=(?P<need>\d+) window=(?P<win_now>\d+)/(?P<win_max>\d+) '
    r'slot=(?P<slot>\d+) l2_live=(?P<l2l>\d+) l2_imb=(?P<l2i>[\d.]+)'
)

RE_STACK_ENTRY = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[GOLD-STACK-ENTRY\] (?P<dir>LONG|SHORT) '
    r'entry=(?P<entry>[\d.]+)'
)

RE_STACK_EXIT = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[GOLD-STACK-(?:TP|SL|TIMEOUT)\] '
    r'(?P<eng>\S+)\s+fill=(?P<fill>[\d.]+) pnl=(?P<pnl>-?[\d.]+)'
)

RE_CVD_GF = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[CVD-GF\] GoldFlow (?P<dir>LONG|SHORT) '
    r'entry blocked'
)


# -------------------------------------------------------------------------
# Engine constants (must match GoldFlowEngine.hpp / GoldEngineStack.hpp)
# -------------------------------------------------------------------------

ALPHA_FAST = 0.05
ALPHA_SLOW = 0.005


# -------------------------------------------------------------------------
# Helpers
# -------------------------------------------------------------------------

def ts_to_seconds(ts_str):
    """Convert HH:MM:SS to seconds from 00:00:00."""
    h, m, s = ts_str.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s)


def seconds_to_ts(secs):
    h = secs // 3600
    m = (secs % 3600) // 60
    s = secs % 60
    return f"{h:02d}:{m:02d}:{s:02d}"


# -------------------------------------------------------------------------
# Main parse-and-replay pass
# -------------------------------------------------------------------------

def run(log_path):
    if not os.path.isfile(log_path):
        print(f"ERROR: log not found: {log_path}", file=sys.stderr)
        return 1

    out_dir = os.path.dirname(os.path.abspath(log_path))
    ticks_path   = os.path.join(out_dir, "apr17_replay_ticks.csv")
    drift_path   = os.path.join(out_dir, "apr17_replay_drift.csv")
    freeze_path  = os.path.join(out_dir, "apr17_replay_freeze.csv")
    summary_path = os.path.join(out_dir, "apr17_replay_summary.txt")

    # ---- Pass 1: collect ticks, GFE-NOSIG rows, stack windows, cvd-gf ----
    ticks        = []   # list of (ts_sec, mid)
    nosig_rows   = []   # list of (ts_sec, logged_drift, slot)
    stack_spans  = []   # list of (entry_ts_sec, exit_ts_sec, direction)
    cvd_blocks   = []   # list of (ts_sec, direction, slot_best_guess)

    open_entry_ts = None
    open_dir = None

    line_count = 0
    with open(log_path, 'r', encoding='utf-8', errors='replace') as fh:
        for raw in fh:
            line_count += 1
            line = raw.rstrip('\n')

            m = RE_TICK_XAU.match(line)
            if m:
                bid = float(m.group('bid'))
                ask = float(m.group('ask'))
                mid = (bid + ask) / 2.0
                ticks.append((ts_to_seconds(m.group('ts')), mid))
                continue

            m = RE_GFE_NOSIG.match(line)
            if m:
                nosig_rows.append((
                    ts_to_seconds(m.group('ts')),
                    float(m.group('drift')),
                    int(m.group('slot')),
                ))
                continue

            m = RE_STACK_ENTRY.match(line)
            if m:
                # A second ENTRY line for the same trade (the pts/size line)
                # would re-match; protect by only opening if we have no
                # open position.
                if open_entry_ts is None:
                    open_entry_ts = ts_to_seconds(m.group('ts'))
                    open_dir = m.group('dir')
                continue

            m = RE_STACK_EXIT.match(line)
            if m:
                if open_entry_ts is not None:
                    stack_spans.append((
                        open_entry_ts,
                        ts_to_seconds(m.group('ts')),
                        open_dir,
                    ))
                    open_entry_ts = None
                    open_dir = None
                continue

            m = RE_CVD_GF.match(line)
            if m:
                cvd_blocks.append((
                    ts_to_seconds(m.group('ts')),
                    m.group('dir'),
                ))
                continue

    if not ticks:
        print(f"ERROR: parsed zero XAUUSD ticks from {log_path}", file=sys.stderr)
        print(f"       lines scanned: {line_count}", file=sys.stderr)
        return 2

    # If a position was still open at EOF (unlikely but possible), close it
    # at the last tick so its window is represented.
    if open_entry_ts is not None:
        stack_spans.append((open_entry_ts, ticks[-1][0], open_dir))

    # ---- Pass 2: replay EWMs across the full tick stream ----
    # Seed from the first mid (clean-boot assumption).
    first_mid = ticks[0][1]
    ewm_fast = first_mid
    ewm_slow = first_mid

    # sim_drift keyed by ts_sec -> drift at LAST tick of that second.
    sim_drift_by_sec = {}
    tick_rows = []

    for ts_sec, mid in ticks:
        ewm_fast = ALPHA_FAST * mid + (1.0 - ALPHA_FAST) * ewm_fast
        ewm_slow = ALPHA_SLOW * mid + (1.0 - ALPHA_SLOW) * ewm_slow
        drift = ewm_fast - ewm_slow
        sim_drift_by_sec[ts_sec] = drift
        tick_rows.append((seconds_to_ts(ts_sec), f"{mid:.3f}", f"{drift:.4f}"))

    # ---- Pass 3: position-open flag for each GFE-NOSIG row ----
    def pos_open_at(ts_sec):
        for a, b, _d in stack_spans:
            if a <= ts_sec <= b:
                return True
        return False

    # ---- Pass 4: freeze-window detection on LOGGED drift ----
    # A "freeze" = consecutive GFE-NOSIG rows where drift did not change by
    # more than 1e-6 AND a position was open for the whole span.
    freeze_rows = []
    cur_start = None
    cur_val = None
    cur_end = None
    for ts_sec, logged_drift, _slot in nosig_rows:
        if not pos_open_at(ts_sec):
            # window break
            if cur_start is not None and cur_end is not None and cur_end > cur_start:
                freeze_rows.append((cur_start, cur_end, cur_val))
            cur_start = None
            cur_val = None
            cur_end = None
            continue

        if cur_start is None:
            cur_start = ts_sec
            cur_val = logged_drift
            cur_end = ts_sec
        elif abs(logged_drift - cur_val) < 1e-6:
            cur_end = ts_sec
        else:
            if cur_end > cur_start:
                freeze_rows.append((cur_start, cur_end, cur_val))
            cur_start = ts_sec
            cur_val = logged_drift
            cur_end = ts_sec

    if cur_start is not None and cur_end is not None and cur_end > cur_start:
        freeze_rows.append((cur_start, cur_end, cur_val))

    # ---- Pass 5: per-slot LONG:SHORT attempt counts (from CVD-GF blocks) ----
    # CVD-GF lines are directional attempt records. Slot isn't on the line,
    # so bucket by UTC time-of-day. Matches Omega's session_slot convention:
    #   slot 6 = Asia            (00:00-06:59)
    #   slot 7 = London open     (07:00-11:59)
    #   slot 8 = NY overlap      (12:00-16:59)
    #   slot 9 = NY afternoon    (17:00-21:59)
    #   slot 10 = late/rollover  (22:00-23:59)
    # (If Omega uses different slot numbers internally, the BUCKETING by
    # time-of-day is still correct; only the slot labels shift.)
    def slot_for(ts_sec):
        h = ts_sec // 3600
        if h < 7:   return 6
        if h < 12:  return 7
        if h < 17:  return 8
        if h < 22:  return 9
        return 10

    logged_attempts = defaultdict(lambda: {'LONG': 0, 'SHORT': 0})
    projected_attempts = defaultdict(lambda: {'LONG': 0, 'SHORT': 0})

    for ts_sec, direction in cvd_blocks:
        s = slot_for(ts_sec)
        logged_attempts[s][direction] += 1

        # Projected direction under Fix A: sign of SIMULATED drift at the
        # nearest prior tick. If sim drift sign matches logged direction,
        # the attempt stays; if it flips, it becomes the opposite direction.
        # This is the most conservative projection: it assumes CVD-GF only
        # fires when upstream direction generator chose that direction, and
        # the direction generator is the sign of drift.
        sim = sim_drift_by_sec.get(ts_sec)
        if sim is None:
            # Find nearest prior tick
            prior = [t for t in sim_drift_by_sec if t <= ts_sec]
            if prior:
                sim = sim_drift_by_sec[max(prior)]
            else:
                sim = 0.0
        projected_dir = 'LONG' if sim > 0 else ('SHORT' if sim < 0 else direction)
        projected_attempts[s][projected_dir] += 1

    # ---- Init-bias check ----
    # If first GFE-NOSIG's logged drift is far from what a fresh seed would
    # produce on the first few ticks, we have a second bug: state carried
    # across day rollover.
    init_bias_report = ""
    if nosig_rows:
        first_nosig_ts, first_logged, _ = nosig_rows[0]
        first_sim = sim_drift_by_sec.get(first_nosig_ts)
        if first_sim is None:
            prior = [t for t in sim_drift_by_sec if t <= first_nosig_ts]
            first_sim = sim_drift_by_sec[max(prior)] if prior else 0.0
        init_bias_report = (
            f"First GFE-NOSIG at {seconds_to_ts(first_nosig_ts)}:\n"
            f"  logged drift     = {first_logged:+.4f}\n"
            f"  simulated drift  = {first_sim:+.4f}\n"
            f"  delta            = {first_logged - first_sim:+.4f}\n"
        )
        if abs(first_logged - first_sim) > 0.5:
            init_bias_report += (
                "  *** INIT BIAS DETECTED ***\n"
                "  Logged drift at start of day is materially different from\n"
                "  what a clean-seeded EWM on today's ticks produces. This\n"
                "  suggests ewm_fast_/ewm_slow_ are NOT being reset on UTC\n"
                "  day rollover -- a second bug, independent of Fix A.\n"
            )
        else:
            init_bias_report += (
                "  Logged drift ~= simulated drift at session start.\n"
                "  Day-rollover reset appears healthy.\n"
            )

    # ---- Write outputs ----
    with open(ticks_path, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['ts', 'mid', 'sim_drift'])
        w.writerows(tick_rows)

    with open(drift_path, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['ts', 'logged_drift', 'sim_drift', 'delta', 'pos_open', 'slot'])
        for ts_sec, logged_drift, slot in nosig_rows:
            sim = sim_drift_by_sec.get(ts_sec)
            if sim is None:
                prior = [t for t in sim_drift_by_sec if t <= ts_sec]
                sim = sim_drift_by_sec[max(prior)] if prior else 0.0
            w.writerow([
                seconds_to_ts(ts_sec),
                f"{logged_drift:+.4f}",
                f"{sim:+.4f}",
                f"{logged_drift - sim:+.4f}",
                int(pos_open_at(ts_sec)),
                slot,
            ])

    with open(freeze_path, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['start_ts', 'end_ts', 'duration_s', 'drift_held'])
        for a, b, v in freeze_rows:
            w.writerow([seconds_to_ts(a), seconds_to_ts(b), b - a, f"{v:+.4f}"])

    # ---- Summary ----
    total_cvd = len(cvd_blocks)
    total_long = sum(1 for _, d in cvd_blocks if d == 'LONG')
    total_short = total_cvd - total_long

    proj_long = sum(x['LONG'] for x in projected_attempts.values())
    proj_short = sum(x['SHORT'] for x in projected_attempts.values())

    with open(summary_path, 'w', encoding='utf-8') as fh:
        fh.write("Apr 17 2026 -- GoldFlow drift replay summary\n")
        fh.write("=" * 60 + "\n\n")
        fh.write(f"Log file            : {log_path}\n")
        fh.write(f"Lines scanned       : {line_count:,}\n")
        fh.write(f"XAUUSD ticks parsed : {len(ticks):,}\n")
        fh.write(f"GFE-NOSIG rows      : {len(nosig_rows):,}\n")
        fh.write(f"GoldStack spans     : {len(stack_spans)}\n")
        fh.write(f"CVD-GF blocks       : {total_cvd}\n")
        fh.write("\n")
        fh.write(f"First tick mid      : {first_mid:.3f} at {seconds_to_ts(ticks[0][0])}\n")
        fh.write(f"Last tick mid       : {ticks[-1][1]:.3f} at {seconds_to_ts(ticks[-1][0])}\n")
        fh.write(f"Net move            : {ticks[-1][1] - first_mid:+.3f} pts\n")
        fh.write("\n")
        fh.write("INIT BIAS CHECK\n")
        fh.write("-" * 60 + "\n")
        fh.write(init_bias_report + "\n")
        fh.write("FREEZE WINDOWS (logged drift held constant while pos open)\n")
        fh.write("-" * 60 + "\n")
        fh.write(f"Count                 : {len(freeze_rows)}\n")
        if freeze_rows:
            longest = max(freeze_rows, key=lambda x: x[1] - x[0])
            total_frozen_s = sum(b - a for a, b, _ in freeze_rows)
            fh.write(f"Longest freeze        : {longest[1]-longest[0]}s at "
                     f"{seconds_to_ts(longest[0])}, drift held = {longest[2]:+.4f}\n")
            fh.write(f"Total time frozen     : {total_frozen_s}s\n")
        fh.write("\n")
        fh.write("LONG:SHORT ATTEMPT DISTRIBUTION (from [CVD-GF] blocks)\n")
        fh.write("-" * 60 + "\n")
        fh.write(f"{'slot':<6} {'logged_L':>10} {'logged_S':>10} "
                 f"{'proj_L':>10} {'proj_S':>10}\n")
        all_slots = sorted(set(logged_attempts.keys()) | set(projected_attempts.keys()))
        for s in all_slots:
            lg = logged_attempts[s]
            pr = projected_attempts[s]
            fh.write(f"{s:<6} {lg['LONG']:>10} {lg['SHORT']:>10} "
                     f"{pr['LONG']:>10} {pr['SHORT']:>10}\n")
        fh.write(f"{'TOTAL':<6} {total_long:>10} {total_short:>10} "
                 f"{proj_long:>10} {proj_short:>10}\n")
        fh.write("\n")
        fh.write("NOTES\n")
        fh.write("-" * 60 + "\n")
        fh.write(
            "1. Replay uses alpha_fast=0.05, alpha_slow=0.005 seeded from\n"
            "   first tick of day. If these alphas change in\n"
            "   GoldEngineStack.hpp, update the script constants.\n"
            "2. Projected direction = sign of simulated drift at each\n"
            "   CVD-GF timestamp. This assumes Fix A restores drift to the\n"
            "   same values a clean-seeded replay produces.\n"
            "3. 'Projected' attempts are what upstream direction generator\n"
            "   WOULD have requested. They may still be blocked by CVD-GF\n"
            "   or other gates in the live engine; this script does not\n"
            "   replay gate logic.\n"
            "4. If init bias is detected, Fix A alone is insufficient --\n"
            "   a second fix is needed to reset EWM state on UTC rollover.\n"
        )

    # Console echo
    print(f"Parsed : {len(ticks):,} ticks, {len(nosig_rows):,} GFE-NOSIG, "
          f"{len(stack_spans)} stack spans, {total_cvd} CVD-GF blocks")
    print(f"Freeze windows detected : {len(freeze_rows)}")
    print(f"Logged   L:S = {total_long}:{total_short}")
    print(f"Projected L:S = {proj_long}:{proj_short}")
    print()
    print(f"Wrote: {ticks_path}")
    print(f"Wrote: {drift_path}")
    print(f"Wrote: {freeze_path}")
    print(f"Wrote: {summary_path}")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Apr 17 GoldFlow drift replay")
    ap.add_argument("--log", required=True, help="path to omega_2026-04-17.log")
    args = ap.parse_args()
    sys.exit(run(args.log))
