#!/usr/bin/env python3
"""
apr17_replay.py  --  Apr 17 GoldFlow drift replay simulator (v2)

v2 changes from v1:
    - Primary drift source is GOLD-BRK-DIAG (emits every 10s unconditionally
      regardless of position state), not GFE-NOSIG (which stops emitting
      while a stack position is open).
    - Freeze detection runs on the regular 10s cadence of GOLD-BRK-DIAG,
      so we can actually see drift held constant during position spans.
    - GOLD-BRK-DIAG carries its own stack_open= field, so position state
      is read directly from the diag line rather than derived from
      ENTRY/EXIT markers.
    - GFE-NOSIG is kept as a secondary source: cross-checked where both
      emit, to confirm both read the same RegimeGovernor drift.

Purpose:
    Reconstruct what g_gold_stack.ewm_drift() WOULD have produced if Fix A
    had been live on Apr 17, and compare to the drift actually logged.
    Answers three questions:
      1. Was logged drift actually frozen during stack position spans?
         (The specific mechanism Fix A targets.)
      2. Was logged drift biased at session start? (Would indicate a
         second bug -- state carried across UTC day rollover.)
      3. Would the LONG:SHORT attempt distribution have been balanced
         under Fix A's replay model?

Replay model:
    ewm_fast  alpha = 0.05
    ewm_slow  alpha = 0.005
    drift     = ewm_fast - ewm_slow
    Seeded from the first XAUUSD tick of the day (clean-boot assumption).

Inputs:
    --log   path to omega_2026-04-17.log  (required)

Outputs (written next to --log):
    apr17_replay_ticks.csv       every XAUUSD tick: ts, mid, sim_drift
    apr17_replay_drift.csv       every GOLD-BRK-DIAG row: ts, logged_drift,
                                 sim_drift, delta, stack_open, slot, phase
    apr17_replay_freeze.csv      detected freeze windows:
                                 start_ts, end_ts, duration_s, drift_held,
                                 samples, stack_open_span
    apr17_replay_xcheck.csv      GFE-NOSIG vs GOLD-BRK-DIAG agreement
                                 where timestamps align (within 2s)
    apr17_replay_summary.txt     human-readable summary + LONG:SHORT
                                 attempt distribution per slot

Usage on VPS (PowerShell):
    python C:\\Omega\\tools\\apr17_replay.py --log C:\\Omega\\logs\\tmp_17\\omega_2026-04-17.log

Exit codes:
    0  success
    1  log not found or unreadable
    2  parse produced zero ticks (format mismatch)
    3  parse produced zero GOLD-BRK-DIAG rows (format mismatch)
"""

import argparse
import csv
import os
import re
import sys
from collections import defaultdict


# -------------------------------------------------------------------------
# Regex patterns (validated against Apr 17 samples)
# -------------------------------------------------------------------------

RE_TICK_XAU = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[TICK\] XAUUSD (?P<bid>[\d.]+)/(?P<ask>[\d.]+)'
)

# GOLD-BRK-DIAG emits every 10s. Fields used:
#   phase=IDLE|ARMED|PENDING|LIVE|COOLDOWN
#   stack_open=0|1
#   drift=<signed float, 2 dp>
#   session_slot=<int>
RE_BRK_DIAG = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[GOLD-BRK-DIAG\] '
    r'phase=(?P<phase>\S+) '
    r'.*?stack_open=(?P<stack_open>\d) '
    r'.*?drift=(?P<drift>-?[\d.]+) '
    r'.*?session_slot=(?P<slot>\d+)'
)

# GFE-NOSIG kept for cross-check only.
RE_GFE_NOSIG = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[GFE-NOSIG\] drift=(?P<drift>-?[\d.]+) '
)

RE_CVD_GF = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[CVD-GF\] GoldFlow (?P<dir>LONG|SHORT) '
    r'entry blocked'
)


# -------------------------------------------------------------------------
# Engine constants
# -------------------------------------------------------------------------

ALPHA_FAST = 0.05
ALPHA_SLOW = 0.005


# -------------------------------------------------------------------------
# Helpers
# -------------------------------------------------------------------------

def ts_to_seconds(ts_str):
    h, m, s = ts_str.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s)


def seconds_to_ts(secs):
    h = secs // 3600
    m = (secs % 3600) // 60
    s = secs % 60
    return f"{h:02d}:{m:02d}:{s:02d}"


def slot_from_hour(ts_sec):
    """Session-slot bucketing by UTC hour.
       Matches Omega's convention (may shift numerically but time-bucketing
       is what matters for the attempt distribution report)."""
    h = ts_sec // 3600
    if h < 7:   return 6
    if h < 12:  return 7
    if h < 17:  return 8
    if h < 22:  return 9
    return 10


# -------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------

def run(log_path):
    if not os.path.isfile(log_path):
        print(f"ERROR: log not found: {log_path}", file=sys.stderr)
        return 1

    out_dir = os.path.dirname(os.path.abspath(log_path))
    ticks_path   = os.path.join(out_dir, "apr17_replay_ticks.csv")
    drift_path   = os.path.join(out_dir, "apr17_replay_drift.csv")
    freeze_path  = os.path.join(out_dir, "apr17_replay_freeze.csv")
    xcheck_path  = os.path.join(out_dir, "apr17_replay_xcheck.csv")
    summary_path = os.path.join(out_dir, "apr17_replay_summary.txt")

    # ---- Pass 1: single-pass parse ----
    ticks      = []   # (ts_sec, mid)
    brk_rows   = []   # (ts_sec, logged_drift, stack_open, slot, phase)
    nosig_rows = []   # (ts_sec, logged_drift)
    cvd_blocks = []   # (ts_sec, direction)

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

            m = RE_BRK_DIAG.match(line)
            if m:
                brk_rows.append((
                    ts_to_seconds(m.group('ts')),
                    float(m.group('drift')),
                    int(m.group('stack_open')),
                    int(m.group('slot')),
                    m.group('phase'),
                ))
                continue

            m = RE_GFE_NOSIG.match(line)
            if m:
                nosig_rows.append((
                    ts_to_seconds(m.group('ts')),
                    float(m.group('drift')),
                ))
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

    if not brk_rows:
        print(f"ERROR: parsed zero GOLD-BRK-DIAG rows from {log_path}", file=sys.stderr)
        print(f"       regex may be out of date -- check emission format", file=sys.stderr)
        return 3

    # ---- Pass 2: replay EWMs across the tick stream ----
    first_mid = ticks[0][1]
    ewm_fast = first_mid
    ewm_slow = first_mid

    sim_drift_by_sec = {}
    tick_rows_out = []

    for ts_sec, mid in ticks:
        ewm_fast = ALPHA_FAST * mid + (1.0 - ALPHA_FAST) * ewm_fast
        ewm_slow = ALPHA_SLOW * mid + (1.0 - ALPHA_SLOW) * ewm_slow
        drift = ewm_fast - ewm_slow
        sim_drift_by_sec[ts_sec] = drift
        tick_rows_out.append((seconds_to_ts(ts_sec), f"{mid:.3f}", f"{drift:.4f}"))

    def sim_drift_at(ts_sec):
        """Return simulated drift at or just before ts_sec."""
        if ts_sec in sim_drift_by_sec:
            return sim_drift_by_sec[ts_sec]
        prior = [t for t in sim_drift_by_sec if t <= ts_sec]
        if prior:
            return sim_drift_by_sec[max(prior)]
        return 0.0

    # ---- Pass 3: freeze detection on GOLD-BRK-DIAG ----
    # A freeze = 3+ consecutive GOLD-BRK-DIAG rows (30+ seconds at 10s cadence)
    # with drift unchanged at the logged precision (2 decimal places, so
    # tolerance 0.005 = half the last displayed digit) AND stack_open=1
    # throughout.
    FREEZE_TOL = 0.005
    MIN_SAMPLES = 3

    freeze_rows = []
    run_start = None
    run_val = None
    run_end = None
    run_samples = 0
    run_stack_span = True

    def close_run():
        nonlocal run_start, run_val, run_end, run_samples, run_stack_span
        if run_start is not None and run_samples >= MIN_SAMPLES and run_stack_span:
            freeze_rows.append((run_start, run_end, run_val, run_samples, 1))
        run_start = None
        run_val = None
        run_end = None
        run_samples = 0
        run_stack_span = True

    for ts_sec, logged_drift, stack_open, _slot, _phase in brk_rows:
        if run_start is None:
            run_start = ts_sec
            run_val = logged_drift
            run_end = ts_sec
            run_samples = 1
            run_stack_span = (stack_open == 1)
            continue

        if abs(logged_drift - run_val) <= FREEZE_TOL:
            run_end = ts_sec
            run_samples += 1
            if stack_open != 1:
                run_stack_span = False
        else:
            close_run()
            run_start = ts_sec
            run_val = logged_drift
            run_end = ts_sec
            run_samples = 1
            run_stack_span = (stack_open == 1)

    close_run()

    # Also flag any flat runs while stack_open=0 that are long enough to be
    # suspicious (>=5 samples = 50+s of zero drift change with no position
    # to "excuse" a freeze).
    freeze_rows_no_pos = []
    run_start = None
    run_val = None
    run_end = None
    run_samples = 0
    run_stack_span = True

    for ts_sec, logged_drift, stack_open, _slot, _phase in brk_rows:
        if run_start is None:
            run_start = ts_sec
            run_val = logged_drift
            run_end = ts_sec
            run_samples = 1
            run_stack_span = (stack_open == 0)
            continue
        if abs(logged_drift - run_val) <= FREEZE_TOL:
            run_end = ts_sec
            run_samples += 1
            if stack_open != 0:
                run_stack_span = False
        else:
            if run_samples >= 5 and run_stack_span and (run_end - run_start) >= 40:
                freeze_rows_no_pos.append((run_start, run_end, run_val, run_samples))
            run_start = ts_sec
            run_val = logged_drift
            run_end = ts_sec
            run_samples = 1
            run_stack_span = (stack_open == 0)

    if run_start is not None and run_samples >= 5 and run_stack_span and (run_end - run_start) >= 40:
        freeze_rows_no_pos.append((run_start, run_end, run_val, run_samples))

    # ---- Pass 4: init-bias check ----
    init_bias_report = ""
    if brk_rows:
        first_ts, first_logged, _, _, _ = brk_rows[0]
        first_sim = sim_drift_at(first_ts)
        delta = first_logged - first_sim
        init_bias_report = (
            f"First GOLD-BRK-DIAG at {seconds_to_ts(first_ts)}:\n"
            f"  logged drift     = {first_logged:+.4f}\n"
            f"  simulated drift  = {first_sim:+.4f}\n"
            f"  delta            = {delta:+.4f}\n"
        )
        if abs(delta) > 0.5:
            init_bias_report += (
                "  *** INIT BIAS DETECTED ***\n"
                "  Logged drift at start of day differs materially from a\n"
                "  clean-seeded EWM replay on today's ticks. This indicates\n"
                "  ewm_fast_/ewm_slow_ are NOT being reset on UTC day\n"
                "  rollover -- a second bug independent of Fix A.\n"
            )
        else:
            init_bias_report += (
                "  Logged ~= simulated at start. Day-rollover reset healthy.\n"
            )

    # ---- Pass 5: GFE-NOSIG vs GOLD-BRK-DIAG cross-check ----
    brk_ts_index = {r[0]: r[1] for r in brk_rows}
    brk_ts_sorted = sorted(brk_ts_index.keys())

    xcheck_rows_out = []
    xcheck_agreements = 0
    xcheck_disagreements = 0

    for ts_sec, nosig_drift in nosig_rows:
        nearest = None
        best_dt = 999
        for bts in brk_ts_sorted:
            dt = abs(bts - ts_sec)
            if dt < best_dt:
                best_dt = dt
                nearest = bts
            if bts > ts_sec + 10:
                break
        if nearest is not None and best_dt <= 2:
            brk_drift = brk_ts_index[nearest]
            delta = nosig_drift - brk_drift
            agree = abs(delta) <= 0.02
            if agree:
                xcheck_agreements += 1
            else:
                xcheck_disagreements += 1
            xcheck_rows_out.append((
                seconds_to_ts(ts_sec),
                seconds_to_ts(nearest),
                f"{nosig_drift:+.4f}",
                f"{brk_drift:+.4f}",
                f"{delta:+.4f}",
                "AGREE" if agree else "DIVERGE",
            ))

    # ---- Pass 6: per-slot LONG:SHORT attempt distribution ----
    logged_attempts = defaultdict(lambda: {'LONG': 0, 'SHORT': 0})
    projected_attempts = defaultdict(lambda: {'LONG': 0, 'SHORT': 0})

    for ts_sec, direction in cvd_blocks:
        s = slot_from_hour(ts_sec)
        logged_attempts[s][direction] += 1
        sim = sim_drift_at(ts_sec)
        projected_dir = 'LONG' if sim > 0 else ('SHORT' if sim < 0 else direction)
        projected_attempts[s][projected_dir] += 1

    # ---- Write outputs ----
    with open(ticks_path, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['ts', 'mid', 'sim_drift'])
        w.writerows(tick_rows_out)

    with open(drift_path, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['ts', 'logged_drift', 'sim_drift', 'delta',
                    'stack_open', 'slot', 'phase'])
        for ts_sec, logged, stack_open, slot, phase in brk_rows:
            sim = sim_drift_at(ts_sec)
            w.writerow([
                seconds_to_ts(ts_sec),
                f"{logged:+.4f}",
                f"{sim:+.4f}",
                f"{logged - sim:+.4f}",
                stack_open,
                slot,
                phase,
            ])

    with open(freeze_path, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['start_ts', 'end_ts', 'duration_s',
                    'drift_held', 'samples', 'stack_open'])
        for a, b, v, n, so in freeze_rows:
            w.writerow([seconds_to_ts(a), seconds_to_ts(b), b - a,
                        f"{v:+.4f}", n, so])
        for a, b, v, n in freeze_rows_no_pos:
            w.writerow([seconds_to_ts(a), seconds_to_ts(b), b - a,
                        f"{v:+.4f}", n, 0])

    with open(xcheck_path, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['nosig_ts', 'brk_ts', 'nosig_drift', 'brk_drift',
                    'delta', 'verdict'])
        w.writerows(xcheck_rows_out)

    # ---- Summary ----
    total_cvd = len(cvd_blocks)
    total_long = sum(1 for _, d in cvd_blocks if d == 'LONG')
    total_short = total_cvd - total_long
    proj_long = sum(x['LONG'] for x in projected_attempts.values())
    proj_short = sum(x['SHORT'] for x in projected_attempts.values())

    total_frozen_s = sum(b - a for a, b, _, _, _ in freeze_rows)
    stack_open_samples = sum(1 for r in brk_rows if r[2] == 1)
    stack_open_span_s = stack_open_samples * 10

    with open(summary_path, 'w', encoding='utf-8') as fh:
        fh.write("Apr 17 2026 -- GoldFlow drift replay summary (v2 -- GOLD-BRK-DIAG source)\n")
        fh.write("=" * 72 + "\n\n")
        fh.write(f"Log file              : {log_path}\n")
        fh.write(f"Lines scanned         : {line_count:,}\n")
        fh.write(f"XAUUSD ticks parsed   : {len(ticks):,}\n")
        fh.write(f"GOLD-BRK-DIAG rows    : {len(brk_rows):,}\n")
        fh.write(f"GFE-NOSIG rows        : {len(nosig_rows):,}\n")
        fh.write(f"CVD-GF blocks         : {total_cvd}\n")
        fh.write("\n")
        fh.write(f"First tick mid        : {first_mid:.3f} at {seconds_to_ts(ticks[0][0])}\n")
        fh.write(f"Last tick mid         : {ticks[-1][1]:.3f} at {seconds_to_ts(ticks[-1][0])}\n")
        fh.write(f"Net move              : {ticks[-1][1] - first_mid:+.3f} pts\n")
        fh.write(f"Stack-open samples    : {stack_open_samples} ({stack_open_span_s}s approx)\n")
        fh.write("\n")

        fh.write("INIT BIAS CHECK\n")
        fh.write("-" * 72 + "\n")
        fh.write(init_bias_report + "\n")

        fh.write("FREEZE WINDOWS DURING STACK-OPEN (the Fix A signature)\n")
        fh.write("-" * 72 + "\n")
        fh.write(f"Detection threshold   : drift delta <= {FREEZE_TOL}, "
                 f">= {MIN_SAMPLES} consecutive samples, stack_open=1 throughout\n")
        fh.write(f"Freeze windows found  : {len(freeze_rows)}\n")
        if freeze_rows:
            longest = max(freeze_rows, key=lambda x: x[1] - x[0])
            pct = 100.0 * total_frozen_s / max(1, stack_open_span_s)
            fh.write(f"Longest freeze        : {longest[1]-longest[0]}s at "
                     f"{seconds_to_ts(longest[0])}, drift held = {longest[2]:+.4f}, "
                     f"{longest[3]} samples\n")
            fh.write(f"Total time frozen     : {total_frozen_s}s "
                     f"({pct:.1f}% of stack-open time)\n")
            fh.write("\n")
            fh.write("Top 10 freeze windows (longest first):\n")
            for a, b, v, n, _ in sorted(freeze_rows, key=lambda x: -(x[1]-x[0]))[:10]:
                fh.write(f"  {seconds_to_ts(a)} -> {seconds_to_ts(b)}  "
                         f"({b-a:4d}s, {n:3d} samples)  drift={v:+.4f}\n")
            fh.write("\n")
            fh.write("*** FIX A MECHANISM CONFIRMED if freezes cover a large\n"
                    "    fraction of stack-open time. Shadow-validate the\n"
                    "    post-fix binary to confirm freezes disappear. ***\n")
        else:
            fh.write("\n")
            fh.write("*** NO FREEZE WINDOWS DETECTED ***\n")
            fh.write("Drift was NOT held constant during stack-open spans.\n"
                    "This means the early-return guard in RegimeGovernor::detect()\n"
                    "was NOT the dominant source of the LONG:SHORT skew. The skew\n"
                    "has a different cause. Fix A's code change is still correct\n"
                    "(an early-return that prevents buffer updates is a latent\n"
                    "bug) but it does not explain the 169:9 Apr 17 distribution.\n"
                    "Requires further investigation: read the full direction-\n"
                    "decision path in GoldFlowEngine and upstream.\n")
        fh.write("\n")

        if freeze_rows_no_pos:
            fh.write("SUSPICIOUS FLAT RUNS WHILE STACK CLOSED (separate issue)\n")
            fh.write("-" * 72 + "\n")
            fh.write(f"Found {len(freeze_rows_no_pos)} runs of 50+s where drift\n"
                    f"was flat with no position open. Not Fix A related but\n"
                    f"worth investigating -- drift should move continuously\n"
                    f"as ticks arrive.\n")
            for a, b, v, n in sorted(freeze_rows_no_pos, key=lambda x: -(x[1]-x[0]))[:5]:
                fh.write(f"  {seconds_to_ts(a)} -> {seconds_to_ts(b)}  "
                        f"({b-a:4d}s, {n:3d} samples)  drift={v:+.4f}\n")
            fh.write("\n")

        fh.write("GFE-NOSIG vs GOLD-BRK-DIAG CROSS-CHECK\n")
        fh.write("-" * 72 + "\n")
        fh.write(f"Paired samples (within 2s): {xcheck_agreements + xcheck_disagreements}\n")
        fh.write(f"  Agree (|delta| <= 0.02) : {xcheck_agreements}\n")
        fh.write(f"  Diverge                 : {xcheck_disagreements}\n")
        if xcheck_disagreements > 0:
            fh.write("  *** If diverge count is non-trivial, GFE-NOSIG and\n"
                    "      GOLD-BRK-DIAG may NOT read the same drift source.\n"
                    "      Check GoldFlowEngine for its own EWM state. ***\n")
        fh.write("\n")

        fh.write("LONG:SHORT ATTEMPT DISTRIBUTION (from [CVD-GF] blocks)\n")
        fh.write("-" * 72 + "\n")
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
        fh.write("-" * 72 + "\n")
        fh.write(
            "1. Replay uses alpha_fast=0.05, alpha_slow=0.005 seeded from\n"
            "   first XAUUSD tick of day. Update if engine alphas change.\n"
            "2. GOLD-BRK-DIAG is the authoritative source for logged drift.\n"
            "   Source: tick_gold.hpp line ~1490, reads g_gold_stack.ewm_drift()\n"
            "   which returns ewm_fast_ - ewm_slow_ from RegimeGovernor state.\n"
            "3. Freeze detection requires stack_open=1 across the whole run\n"
            "   because that is the exact condition Fix A addresses.\n"
            "4. Projected direction = sign of simulated drift at the CVD-GF\n"
            "   timestamp. Assumes Fix A restores drift to clean-seeded values.\n"
        )

    print(f"Parsed : {len(ticks):,} ticks, {len(brk_rows):,} GOLD-BRK-DIAG, "
          f"{len(nosig_rows):,} GFE-NOSIG, {total_cvd} CVD-GF blocks")
    print(f"Stack-open samples   : {stack_open_samples} (~{stack_open_span_s}s)")
    print(f"Freeze windows       : {len(freeze_rows)} (total {total_frozen_s}s frozen)")
    print(f"Xcheck agree/diverge : {xcheck_agreements}/{xcheck_disagreements}")
    print(f"Logged   L:S = {total_long}:{total_short}")
    print(f"Projected L:S = {proj_long}:{proj_short}")
    print()
    print(f"Wrote: {ticks_path}")
    print(f"Wrote: {drift_path}")
    print(f"Wrote: {freeze_path}")
    print(f"Wrote: {xcheck_path}")
    print(f"Wrote: {summary_path}")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Apr 17 GoldFlow drift replay (v2)")
    ap.add_argument("--log", required=True, help="path to omega_2026-04-17.log")
    args = ap.parse_args()
    sys.exit(run(args.log))
