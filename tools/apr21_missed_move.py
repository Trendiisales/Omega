#!/usr/bin/env python3
"""
apr21_missed_move.py  --  Apr 21 missed-downward-move diagnostic

Sibling to tools/apr17_replay.py.  apr17_replay.py answered "was drift frozen
during stack-open spans?" and "was drift primed at session start?".  This
script answers a different question: "XAUUSD dropped ~101 points over ~8 hours
on Apr 21 and Omega did not capture it.  Why?"

The investigation proceeds in five stages:

  1. Parse ticks + GOLD-BRK-DIAG + GFE-NOSIG + CVD-GF + any ENTRY/EXIT lines.
  2. Identify the largest sustained downward move in XAUUSD mid price.
     "Sustained" means the running drawdown from the rolling max is held for
     a minimum configurable duration (default 60 min).  Report its exact
     start/end UTC seconds, peak and trough mid, and the drawdown in points.
  3. Within that window, produce a direction-logic audit:
        - Logged RegimeGovernor drift: sign distribution, mean, min, max.
        - GFE-NOSIG rejections: count + reason-field histogram when present
          in the line suffix.
        - CVD-GF blocks: LONG vs SHORT count.
        - GOLD-BRK-DIAG: phase distribution, stack_open percentage.
  4. Stack-state timeline across the move: at each 1-minute bucket report
        (stack_open, phase, drift, mid, drawdown_from_peak)
     so we can see exactly whether Omega was holding a LONG while price fell,
     or sitting flat while declining to short.
  5. Rolled-forward clean-replay comparison: same EWM replay model as
     apr17_replay.py (alpha_fast=0.05, alpha_slow=0.005, seed from first
     tick), but annotated against the identified down-move window.  If the
     clean-replay drift goes negative during the down-move and the logged
     drift stays positive or near zero, that's direct evidence of drift
     contamination (Fix B target condition) on Apr 21.

Outputs (written next to --log):

    apr21_move_summary.txt        human-readable findings
    apr21_move_window.csv         1-minute bucket timeline of the down-move
    apr21_move_gfe_nosig.csv      every GFE-NOSIG inside the window
    apr21_move_entries.csv        every ENTRY/EXIT inside the window
    apr21_move_drift_compare.csv  logged vs clean-replay drift, all day

Usage on the Omega VPS (PowerShell, one command per line per the
NO && CHAINING rule):

    cd C:\\Omega
    python tools\\apr21_missed_move.py --log C:\\Omega\\logs\\omega_2026-04-21.log

Optional flags:

    --min-move-minutes N    Minimum sustained-drawdown duration to qualify
                            as "the move" (default 60).
    --min-move-points  P    Minimum drawdown magnitude in points
                            (default 30.0).  If no window meets both the
                            duration and magnitude thresholds, the script
                            falls back to the largest peak-to-trough in
                            the day and reports it as "best candidate".

Exit codes:
    0  success
    1  log not found or unreadable
    2  parse produced zero XAUUSD ticks
    3  parse produced zero GOLD-BRK-DIAG rows
"""

import argparse
import csv
import os
import re
import sys
from collections import Counter, defaultdict


# -------------------------------------------------------------------------
# Regex patterns -- identical to tools/apr17_replay.py v2
# -------------------------------------------------------------------------

RE_TICK_XAU = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[TICK\] XAUUSD (?P<bid>[\d.]+)/(?P<ask>[\d.]+)'
)

RE_BRK_DIAG = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[GOLD-BRK-DIAG\] '
    r'phase=(?P<phase>\S+) '
    r'.*?stack_open=(?P<stack_open>\d) '
    r'.*?drift=(?P<drift>-?[\d.]+) '
    r'.*?session_slot=(?P<slot>\d+)'
)

RE_GFE_NOSIG = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[GFE-NOSIG\] drift=(?P<drift>-?[\d.]+)'
    r'(?P<tail>.*)$'
)

RE_CVD_GF = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[CVD-GF\] GoldFlow (?P<dir>LONG|SHORT) '
    r'entry blocked'
)

# ENTRY and EXIT lines -- format observed in Apr 17 log, carrying symbol
# and direction.  Matches both [ENTRY] and [CFE-ENTRY] / [GFE-ENTRY] style
# prefixes by being permissive on the bracketed tag.
RE_ENTRY = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[(?P<tag>[A-Z0-9\-]*ENTRY[A-Z0-9\-]*)\] '
    r'(?P<sym>XAUUSD|BTCUSD|ETHUSD|[A-Z]+) '
    r'(?P<dir>LONG|SHORT)'
)

RE_EXIT = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[(?P<tag>[A-Z0-9\-]*EXIT[A-Z0-9\-]*)\] '
    r'(?P<sym>XAUUSD|BTCUSD|ETHUSD|[A-Z]+)'
)

# Reason extraction for GFE-NOSIG tails.  The tail typically carries
# reason=<token> or a trailing bracketed keyword -- we capture whatever
# appears after reason= OR the last bracketed token for histogramming.
RE_NOSIG_REASON = re.compile(r'reason=(?P<reason>\S+)')
RE_NOSIG_LAST_BRACKET = re.compile(r'\[([A-Z0-9\-_]+)\](?!.*\[)')


# -------------------------------------------------------------------------
# Engine constants (clean-replay model, matches apr17_replay.py)
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
    secs = int(secs)
    h = secs // 3600
    m = (secs % 3600) // 60
    s = secs % 60
    return "{:02d}:{:02d}:{:02d}".format(h, m, s)


def slot_from_hour(ts_sec):
    h = ts_sec // 3600
    if h < 7:
        return 6
    if h < 12:
        return 7
    if h < 17:
        return 8
    if h < 22:
        return 9
    return 10


# -------------------------------------------------------------------------
# Pass 1: parse the log
# -------------------------------------------------------------------------

def parse_log(log_path):
    ticks = []
    brk_rows = []
    nosig_rows = []
    cvd_blocks = []
    entries = []
    exits = []

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
                brk_rows.append({
                    'ts': ts_to_seconds(m.group('ts')),
                    'drift': float(m.group('drift')),
                    'stack_open': int(m.group('stack_open')),
                    'slot': int(m.group('slot')),
                    'phase': m.group('phase'),
                })
                continue

            m = RE_GFE_NOSIG.match(line)
            if m:
                tail = m.group('tail') or ''
                reason = ''
                rm = RE_NOSIG_REASON.search(tail)
                if rm:
                    reason = rm.group('reason')
                else:
                    rb = RE_NOSIG_LAST_BRACKET.search(tail)
                    if rb:
                        reason = rb.group(1)
                nosig_rows.append({
                    'ts': ts_to_seconds(m.group('ts')),
                    'drift': float(m.group('drift')),
                    'reason': reason,
                    'tail': tail.strip(),
                })
                continue

            m = RE_CVD_GF.match(line)
            if m:
                cvd_blocks.append({
                    'ts': ts_to_seconds(m.group('ts')),
                    'dir': m.group('dir'),
                })
                continue

            m = RE_ENTRY.match(line)
            if m:
                entries.append({
                    'ts': ts_to_seconds(m.group('ts')),
                    'tag': m.group('tag'),
                    'sym': m.group('sym'),
                    'dir': m.group('dir'),
                    'raw': line,
                })
                continue

            m = RE_EXIT.match(line)
            if m:
                exits.append({
                    'ts': ts_to_seconds(m.group('ts')),
                    'tag': m.group('tag'),
                    'sym': m.group('sym'),
                    'raw': line,
                })
                continue

    return {
        'ticks': ticks,
        'brk_rows': brk_rows,
        'nosig_rows': nosig_rows,
        'cvd_blocks': cvd_blocks,
        'entries': entries,
        'exits': exits,
        'line_count': line_count,
    }


# -------------------------------------------------------------------------
# Pass 2: find the largest sustained downward move
# -------------------------------------------------------------------------

def find_down_move(ticks, min_minutes, min_points):
    """Return (start_ts_sec, end_ts_sec, peak_mid, trough_mid, drawdown_pts,
              qualified_bool).
    Qualified = both duration and magnitude thresholds met.
    If nothing qualifies, we still return the single largest peak-to-trough
    as a best-effort candidate with qualified=False.
    """
    if not ticks:
        return None

    # Track the largest drawdown ending at each tick: for every point t,
    # we maintain the running max peak before t and record the drawdown.
    # To find the largest sustained drawdown window, we scan and keep the
    # best (peak_ts, peak_mid) -> (trough_ts, trough_mid) span where
    # trough is the min mid AFTER peak_ts.

    best = None  # (drawdown, peak_ts, peak_mid, trough_ts, trough_mid)
    peak_ts = ticks[0][0]
    peak_mid = ticks[0][1]

    # First pass: build the peak-to-trough candidate -- classic max-drop
    # single-sweep.
    cur_peak_ts = ticks[0][0]
    cur_peak_mid = ticks[0][1]
    for ts, mid in ticks:
        if mid > cur_peak_mid:
            cur_peak_ts = ts
            cur_peak_mid = mid
        drawdown = cur_peak_mid - mid
        if best is None or drawdown > best[0]:
            best = (drawdown, cur_peak_ts, cur_peak_mid, ts, mid)

    if best is None:
        return None

    drawdown_pts = best[0]
    peak_ts = best[1]
    peak_mid = best[2]
    trough_ts = best[3]
    trough_mid = best[4]
    duration_min = (trough_ts - peak_ts) / 60.0

    qualified = (duration_min >= min_minutes) and (drawdown_pts >= min_points)

    return {
        'peak_ts': peak_ts,
        'peak_mid': peak_mid,
        'trough_ts': trough_ts,
        'trough_mid': trough_mid,
        'drawdown_pts': drawdown_pts,
        'duration_min': duration_min,
        'qualified': qualified,
    }


# -------------------------------------------------------------------------
# Pass 3: direction-logic audit inside the window
# -------------------------------------------------------------------------

def audit_window(parsed, window):
    start = window['peak_ts']
    end = window['trough_ts']

    # Drift sign distribution from GOLD-BRK-DIAG.
    drift_vals = []
    stack_open_count = 0
    phase_counter = Counter()
    slot_counter = Counter()
    for row in parsed['brk_rows']:
        if start <= row['ts'] <= end:
            drift_vals.append(row['drift'])
            stack_open_count += row['stack_open']
            phase_counter[row['phase']] += 1
            slot_counter[row['slot']] += 1

    drift_stats = {}
    if drift_vals:
        drift_stats = {
            'count': len(drift_vals),
            'mean': sum(drift_vals) / len(drift_vals),
            'min': min(drift_vals),
            'max': max(drift_vals),
            'positive_pct': 100.0 * sum(1 for d in drift_vals if d > 0) / len(drift_vals),
            'negative_pct': 100.0 * sum(1 for d in drift_vals if d < 0) / len(drift_vals),
            'near_zero_pct': 100.0 * sum(1 for d in drift_vals if abs(d) < 0.05) / len(drift_vals),
        }

    # GFE-NOSIG reason histogram inside the window.
    nosig_in_window = [r for r in parsed['nosig_rows'] if start <= r['ts'] <= end]
    reason_counter = Counter(r['reason'] or '(no-reason-token)' for r in nosig_in_window)

    # CVD-GF blocks.
    cvd_in_window = [r for r in parsed['cvd_blocks'] if start <= r['ts'] <= end]
    cvd_dir_counter = Counter(r['dir'] for r in cvd_in_window)

    # Entries and exits (XAUUSD only for this question).
    entries_xau = [e for e in parsed['entries']
                   if start <= e['ts'] <= end and e['sym'] == 'XAUUSD']
    exits_xau = [e for e in parsed['exits']
                 if start <= e['ts'] <= end and e['sym'] == 'XAUUSD']
    entry_dir_counter = Counter(e['dir'] for e in entries_xau)

    # Stack_open duty cycle estimate:
    # each brk_row represents a 10s sample.
    stack_open_duty = 0.0
    if len(drift_vals) > 0:
        stack_open_duty = 100.0 * stack_open_count / len(drift_vals)

    return {
        'drift_stats': drift_stats,
        'phase_counter': phase_counter,
        'slot_counter': slot_counter,
        'stack_open_duty': stack_open_duty,
        'nosig_count': len(nosig_in_window),
        'reason_counter': reason_counter,
        'cvd_dir_counter': cvd_dir_counter,
        'entries_xau': entries_xau,
        'exits_xau': exits_xau,
        'entry_dir_counter': entry_dir_counter,
    }


# -------------------------------------------------------------------------
# Pass 4: per-minute timeline for the window
# -------------------------------------------------------------------------

def build_timeline(parsed, window):
    start = window['peak_ts']
    end = window['trough_ts']
    peak_mid = window['peak_mid']

    # Bucket ticks by minute, keep the last mid in each minute.
    mid_by_min = {}
    for ts, mid in parsed['ticks']:
        if start <= ts <= end:
            mid_by_min[ts // 60] = mid

    # Bucket brk_rows by minute, keep last drift and stack_open.
    brk_by_min = {}
    for row in parsed['brk_rows']:
        if start <= row['ts'] <= end:
            brk_by_min[row['ts'] // 60] = row

    start_min = start // 60
    end_min = end // 60
    rows = []
    for bm in range(start_min, end_min + 1):
        mid = mid_by_min.get(bm)
        brk = brk_by_min.get(bm)
        rows.append({
            'minute_ts': seconds_to_ts(bm * 60),
            'mid': mid if mid is not None else '',
            'drawdown_from_peak': (peak_mid - mid) if mid is not None else '',
            'drift': brk['drift'] if brk else '',
            'phase': brk['phase'] if brk else '',
            'stack_open': brk['stack_open'] if brk else '',
            'slot': brk['slot'] if brk else '',
        })
    return rows


# -------------------------------------------------------------------------
# Pass 5: clean-replay drift across the whole day, compared to logged
# -------------------------------------------------------------------------

def clean_replay(ticks, brk_rows):
    if not ticks:
        return []
    first_mid = ticks[0][1]
    ewm_fast = first_mid
    ewm_slow = first_mid

    sim_drift_by_sec = {}
    for ts, mid in ticks:
        ewm_fast = ALPHA_FAST * mid + (1.0 - ALPHA_FAST) * ewm_fast
        ewm_slow = ALPHA_SLOW * mid + (1.0 - ALPHA_SLOW) * ewm_slow
        sim_drift_by_sec[ts] = ewm_fast - ewm_slow

    rows = []
    for r in brk_rows:
        # Find nearest tick timestamp <= r['ts'].
        sim = sim_drift_by_sec.get(r['ts'])
        if sim is None:
            # Walk back up to 5 seconds looking for a tick.
            for back in range(1, 6):
                sim = sim_drift_by_sec.get(r['ts'] - back)
                if sim is not None:
                    break
        rows.append({
            'ts': seconds_to_ts(r['ts']),
            'logged_drift': r['drift'],
            'sim_drift': sim if sim is not None else '',
            'delta': (r['drift'] - sim) if sim is not None else '',
            'stack_open': r['stack_open'],
            'phase': r['phase'],
            'slot': r['slot'],
        })
    return rows


# -------------------------------------------------------------------------
# Output writers
# -------------------------------------------------------------------------

def write_summary(path, parsed, window, audit):
    lines = []
    lines.append("=" * 74)
    lines.append("APR 21 MISSED DOWNWARD MOVE -- DIAGNOSTIC SUMMARY")
    lines.append("=" * 74)
    lines.append("")
    lines.append("PARSE STATS")
    lines.append("-" * 74)
    lines.append("  lines scanned:        {}".format(parsed['line_count']))
    lines.append("  XAUUSD ticks:         {}".format(len(parsed['ticks'])))
    lines.append("  GOLD-BRK-DIAG rows:   {}".format(len(parsed['brk_rows'])))
    lines.append("  GFE-NOSIG rows:       {}".format(len(parsed['nosig_rows'])))
    lines.append("  CVD-GF blocks:        {}".format(len(parsed['cvd_blocks'])))
    lines.append("  Entries (all syms):   {}".format(len(parsed['entries'])))
    lines.append("  Exits   (all syms):   {}".format(len(parsed['exits'])))
    lines.append("")

    if window is None:
        lines.append("NO DOWN MOVE FOUND (no ticks parsed).")
        with open(path, 'w', encoding='utf-8') as fh:
            fh.write('\n'.join(lines))
        return

    lines.append("IDENTIFIED DOWN MOVE")
    lines.append("-" * 74)
    qflag = "QUALIFIED" if window['qualified'] else "BEST-CANDIDATE (below thresholds)"
    lines.append("  status:          {}".format(qflag))
    lines.append("  peak    UTC:     {}   mid={:.3f}".format(
        seconds_to_ts(window['peak_ts']), window['peak_mid']))
    lines.append("  trough  UTC:     {}   mid={:.3f}".format(
        seconds_to_ts(window['trough_ts']), window['trough_mid']))
    lines.append("  drawdown:        {:.2f} points".format(window['drawdown_pts']))
    lines.append("  duration:        {:.1f} minutes ({:.2f} hours)".format(
        window['duration_min'], window['duration_min'] / 60.0))
    lines.append("")

    lines.append("DIRECTION-LOGIC AUDIT (inside window)")
    lines.append("-" * 74)
    ds = audit['drift_stats']
    if ds:
        lines.append("  RegimeGovernor drift (from GOLD-BRK-DIAG):")
        lines.append("     samples:      {}".format(ds['count']))
        lines.append("     mean:         {:+.4f}".format(ds['mean']))
        lines.append("     min:          {:+.4f}".format(ds['min']))
        lines.append("     max:          {:+.4f}".format(ds['max']))
        lines.append("     positive:     {:.1f}%".format(ds['positive_pct']))
        lines.append("     negative:     {:.1f}%".format(ds['negative_pct']))
        lines.append("     near-zero:    {:.1f}% (|drift| < 0.05)".format(
            ds['near_zero_pct']))
    else:
        lines.append("  RegimeGovernor drift:  NO SAMPLES in window.")
    lines.append("")

    lines.append("  Stack-open duty cycle (% of diag samples with stack_open=1):")
    lines.append("     {:.1f}%".format(audit['stack_open_duty']))
    lines.append("")

    lines.append("  GOLD-BRK-DIAG phase distribution:")
    total_phases = sum(audit['phase_counter'].values())
    for phase, cnt in audit['phase_counter'].most_common():
        pct = 100.0 * cnt / total_phases if total_phases else 0.0
        lines.append("     {:<10} {:>6}  ({:.1f}%)".format(phase, cnt, pct))
    lines.append("")

    lines.append("  GFE-NOSIG rejections: {}".format(audit['nosig_count']))
    if audit['reason_counter']:
        lines.append("     reason histogram (top 10):")
        for reason, cnt in audit['reason_counter'].most_common(10):
            lines.append("       {:<30} {}".format(reason[:30], cnt))
    lines.append("")

    lines.append("  CVD-GF blocks by direction:")
    if audit['cvd_dir_counter']:
        for d, cnt in audit['cvd_dir_counter'].most_common():
            lines.append("     {:<6} {}".format(d, cnt))
    else:
        lines.append("     (none)")
    lines.append("")

    lines.append("  XAUUSD entries inside window:")
    lines.append("     total: {}".format(len(audit['entries_xau'])))
    for d, cnt in audit['entry_dir_counter'].most_common():
        lines.append("     {:<6} {}".format(d, cnt))
    for e in audit['entries_xau'][:10]:
        lines.append("       {}  [{}]  {}  {}".format(
            seconds_to_ts(e['ts']), e['tag'], e['sym'], e['dir']))
    if len(audit['entries_xau']) > 10:
        lines.append("       ... ({} more)".format(len(audit['entries_xau']) - 10))
    lines.append("")

    lines.append("  XAUUSD exits inside window:   {}".format(len(audit['exits_xau'])))
    for e in audit['exits_xau'][:10]:
        lines.append("       {}  [{}]  {}".format(
            seconds_to_ts(e['ts']), e['tag'], e['sym']))
    if len(audit['exits_xau']) > 10:
        lines.append("       ... ({} more)".format(len(audit['exits_xau']) - 10))
    lines.append("")

    lines.append("INTERPRETATION GUIDE")
    lines.append("-" * 74)
    lines.append("  If drift mean is positive or near-zero while price fell {:.1f}pt:".format(
        window['drawdown_pts']))
    lines.append("     -> RegimeGovernor failed to produce a negative drift signal.")
    lines.append("        Likely causes:")
    lines.append("          (a) Pre-Fix-B carryover: drift primed high at 00:00 UTC.")
    lines.append("              Check apr21_move_drift_compare.csv at the start of")
    lines.append("              the day -- if logged_drift is far from sim_drift, yes.")
    lines.append("          (b) Pre-Fix-A freeze during an earlier stack-open span")
    lines.append("              held drift stale into the down-move. Check for long")
    lines.append("              runs of stack_open=1 with drift unchanged.")
    lines.append("")
    lines.append("  If stack-open duty cycle is high AND entries_xau contains LONG:")
    lines.append("     -> Omega was holding a LONG while price fell (the -101pt came")
    lines.append("        from an open contrarian position, not a missed short).")
    lines.append("")
    lines.append("  If GFE-NOSIG rejections are dominated by a single reason token:")
    lines.append("     -> That gate is the blocker preventing SHORT entries.")
    lines.append("        Audit the corresponding condition in GoldFlowEngine.")
    lines.append("")
    lines.append("  If CVD-GF SHORT blocks are high:")
    lines.append("     -> CVD filter vetoed SHORT entries. Check CVD polarity during")
    lines.append("        the window -- may be lagging or inverted.")
    lines.append("")

    with open(path, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines))


def write_csv(path, rows, fieldnames):
    with open(path, 'w', encoding='utf-8', newline='') as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        for r in rows:
            w.writerow(r)


# -------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------

def run(log_path, min_minutes, min_points):
    if not os.path.isfile(log_path):
        print("ERROR: log not found: {}".format(log_path), file=sys.stderr)
        return 1

    out_dir = os.path.dirname(os.path.abspath(log_path))
    summary_path = os.path.join(out_dir, "apr21_move_summary.txt")
    window_path = os.path.join(out_dir, "apr21_move_window.csv")
    nosig_path = os.path.join(out_dir, "apr21_move_gfe_nosig.csv")
    entries_path = os.path.join(out_dir, "apr21_move_entries.csv")
    compare_path = os.path.join(out_dir, "apr21_move_drift_compare.csv")

    print("Parsing {} ...".format(log_path))
    parsed = parse_log(log_path)
    print("  {} lines / {} ticks / {} brk-diag / {} nosig".format(
        parsed['line_count'], len(parsed['ticks']),
        len(parsed['brk_rows']), len(parsed['nosig_rows'])))

    if not parsed['ticks']:
        print("ERROR: zero XAUUSD ticks parsed", file=sys.stderr)
        return 2
    if not parsed['brk_rows']:
        print("ERROR: zero GOLD-BRK-DIAG rows parsed", file=sys.stderr)
        return 3

    print("Finding largest sustained down-move "
          "(min {} min, min {:.1f} pt)...".format(min_minutes, min_points))
    window = find_down_move(parsed['ticks'], min_minutes, min_points)
    if window is None:
        print("  no window found.")
    else:
        print("  peak {} @ {:.3f}  ->  trough {} @ {:.3f}".format(
            seconds_to_ts(window['peak_ts']), window['peak_mid'],
            seconds_to_ts(window['trough_ts']), window['trough_mid']))
        print("  drawdown {:.2f} pt over {:.1f} min  [{}]".format(
            window['drawdown_pts'], window['duration_min'],
            'qualified' if window['qualified'] else 'best-candidate'))

    if window is None:
        write_summary(summary_path, parsed, None, None)
        print("Wrote {}".format(summary_path))
        return 0

    print("Auditing direction logic inside window...")
    audit = audit_window(parsed, window)

    print("Building per-minute timeline...")
    timeline = build_timeline(parsed, window)

    print("Running clean-replay drift comparison...")
    compare = clean_replay(parsed['ticks'], parsed['brk_rows'])

    print("Writing outputs:")
    write_summary(summary_path, parsed, window, audit)
    print("  {}".format(summary_path))

    write_csv(window_path, timeline, [
        'minute_ts', 'mid', 'drawdown_from_peak',
        'drift', 'phase', 'stack_open', 'slot',
    ])
    print("  {}".format(window_path))

    # GFE-NOSIG inside window only.
    nosig_rows_out = []
    for r in parsed['nosig_rows']:
        if window['peak_ts'] <= r['ts'] <= window['trough_ts']:
            nosig_rows_out.append({
                'ts': seconds_to_ts(r['ts']),
                'drift': r['drift'],
                'reason': r['reason'],
                'tail': r['tail'],
            })
    write_csv(nosig_path, nosig_rows_out,
              ['ts', 'drift', 'reason', 'tail'])
    print("  {}".format(nosig_path))

    # Entries + exits inside window.
    entry_rows_out = []
    for e in parsed['entries']:
        if window['peak_ts'] <= e['ts'] <= window['trough_ts']:
            entry_rows_out.append({
                'ts': seconds_to_ts(e['ts']),
                'kind': 'ENTRY',
                'tag': e['tag'],
                'sym': e['sym'],
                'dir': e['dir'],
                'raw': e['raw'],
            })
    for e in parsed['exits']:
        if window['peak_ts'] <= e['ts'] <= window['trough_ts']:
            entry_rows_out.append({
                'ts': seconds_to_ts(e['ts']),
                'kind': 'EXIT',
                'tag': e['tag'],
                'sym': e['sym'],
                'dir': '',
                'raw': e['raw'],
            })
    entry_rows_out.sort(key=lambda r: r['ts'])
    write_csv(entries_path, entry_rows_out,
              ['ts', 'kind', 'tag', 'sym', 'dir', 'raw'])
    print("  {}".format(entries_path))

    write_csv(compare_path, compare, [
        'ts', 'logged_drift', 'sim_drift', 'delta',
        'stack_open', 'phase', 'slot',
    ])
    print("  {}".format(compare_path))

    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Apr 21 missed-downward-move diagnostic for Omega logs")
    ap.add_argument('--log', required=True,
                    help="path to omega_2026-04-21.log")
    ap.add_argument('--min-move-minutes', type=float, default=60.0,
                    help="minimum sustained-drawdown duration in minutes "
                         "(default 60)")
    ap.add_argument('--min-move-points', type=float, default=30.0,
                    help="minimum drawdown magnitude in points (default 30)")
    args = ap.parse_args()
    rc = run(args.log, args.min_move_minutes, args.min_move_points)
    sys.exit(rc)


if __name__ == '__main__':
    main()
