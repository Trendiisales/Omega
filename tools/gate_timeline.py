#!/usr/bin/env python3
"""
gate_timeline.py  --  Per-engine gate-rejection timeline and reason breakdown

For every bracketed tag matching a rejection/block/gate pattern, count
occurrences and collect a sample of reason details.  Optional window
restriction by HH:MM:SS UTC.

Purpose: on Apr 21 the daily engine-distribution showed ~5000 gate
rejections across 30+ tag types while zero trades were taken.  This tool
surfaces which gates dominated during a specific window (e.g. the 124pt
down-move 13:47-19:44) and what those rejections said, so the overfit
layer can be identified without guessing.

Usage on VPS:
    python tools\\gate_timeline.py --log C:\\Omega\\logs\\tmp_21\\omega_2026-04-21.log
    python tools\\gate_timeline.py --log C:\\Omega\\logs\\tmp_21\\omega_2026-04-21.log --window-start 13:47:09 --window-end 19:44:30

Outputs (written next to --log):
    gate_timeline_summary.txt   human-readable ranked summary
    gate_timeline_details.csv   every matched line with ts, tag, reason-tail
"""

import argparse
import csv
import os
import re
import sys
from collections import Counter, defaultdict


# Tag pattern: match bracketed tokens that look like engine gate tags.
# Uppercase, digits, hyphens, underscores.  Must be >= 3 chars, have at
# least one hyphen, and contain an engine prefix or gate suffix keyword.
RE_LINE = re.compile(
    r'^(?P<ts>\d{2}:\d{2}:\d{2}) \[(?P<tag>[A-Z][A-Z0-9_\-]+)\](?P<tail>.*)$'
)

# Only consider tags that look like gate/block/reject/rejection markers.
# We keep a permissive list and filter by substring tokens.
GATE_TOKENS = (
    'BLOCK', 'GATE', 'REJECT', 'NOSIG', 'OVERRIDE', 'KILL', 'LOCK',
    'CONFLICT', 'STOP', 'FAIL', 'BAN', 'FREEZE', 'SUPPRESS',
)

# Also include known-useful engine-state tags even without the above tokens,
# so we can see e.g. CBE-STATE or DPE-SEED context.
STATE_TAGS_ALLOW = (
    'CBE-STATE', 'DPE-SEED', 'DPE-BUILDING', 'CBE-BE', 'CBE-TRAIL-ARM',
    'CFE-RSI-LEVEL', 'CFE-SUS-PEAK', 'CFE-ATR-CAP', 'OMEGA-DIAG',
)


def is_gate_tag(tag):
    for t in GATE_TOKENS:
        if t in tag:
            return True
    if tag in STATE_TAGS_ALLOW:
        return True
    return False


def ts_to_sec(ts):
    h, m, s = ts.split(':')
    return int(h) * 3600 + int(m) * 60 + int(s)


def run(log_path, w_start, w_end, top_n):
    if not os.path.isfile(log_path):
        print("ERROR: log not found: {}".format(log_path), file=sys.stderr)
        return 1

    start_sec = ts_to_sec(w_start) if w_start else None
    end_sec = ts_to_sec(w_end) if w_end else None

    out_dir = os.path.dirname(os.path.abspath(log_path))
    summary_path = os.path.join(out_dir, 'gate_timeline_summary.txt')
    details_path = os.path.join(out_dir, 'gate_timeline_details.csv')

    tag_count = Counter()
    tag_samples = defaultdict(list)  # tag -> list of (ts, tail) up to N
    tag_first_ts = {}
    tag_last_ts = {}

    total_lines = 0
    total_matched = 0
    total_gated = 0

    details_rows = []

    with open(log_path, 'r', encoding='utf-8', errors='replace') as fh:
        for raw in fh:
            total_lines += 1
            line = raw.rstrip('\n')
            m = RE_LINE.match(line)
            if not m:
                continue
            total_matched += 1
            tag = m.group('tag')
            ts = m.group('ts')
            tail = m.group('tail').strip()
            ts_sec = ts_to_sec(ts)

            if start_sec is not None and ts_sec < start_sec:
                continue
            if end_sec is not None and ts_sec > end_sec:
                continue
            if not is_gate_tag(tag):
                continue

            total_gated += 1
            tag_count[tag] += 1
            if tag not in tag_first_ts:
                tag_first_ts[tag] = ts
            tag_last_ts[tag] = ts
            if len(tag_samples[tag]) < 5:
                tag_samples[tag].append((ts, tail))

            details_rows.append({
                'ts': ts,
                'tag': tag,
                'tail': tail,
            })

    # Build summary text
    lines = []
    lines.append("=" * 74)
    lines.append("GATE TIMELINE SUMMARY")
    lines.append("=" * 74)
    lines.append("log:           {}".format(log_path))
    if w_start or w_end:
        lines.append("window:        {} -> {}".format(
            w_start or '(start)', w_end or '(end)'))
    lines.append("lines scanned: {}".format(total_lines))
    lines.append("tagged lines:  {}".format(total_matched))
    lines.append("gate lines:    {}".format(total_gated))
    lines.append("")
    lines.append("Ranked gate activity:")
    lines.append("-" * 74)
    lines.append("  {:<30} {:>6}  {}".format("tag", "count", "first -> last"))
    for tag, cnt in tag_count.most_common(top_n):
        span = "{} -> {}".format(tag_first_ts.get(tag, '?'),
                                 tag_last_ts.get(tag, '?'))
        lines.append("  {:<30} {:>6}  {}".format(tag, cnt, span))
    lines.append("")
    lines.append("Sample reason tails per tag (up to 5):")
    lines.append("-" * 74)
    for tag, cnt in tag_count.most_common(top_n):
        lines.append("[{}]  (n={})".format(tag, cnt))
        for ts, tail in tag_samples[tag]:
            show = tail if len(tail) <= 160 else tail[:157] + '...'
            lines.append("  {}  {}".format(ts, show))
        lines.append("")

    with open(summary_path, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines))

    with open(details_path, 'w', encoding='utf-8', newline='') as fh:
        w = csv.DictWriter(fh, fieldnames=['ts', 'tag', 'tail'])
        w.writeheader()
        for r in details_rows:
            w.writerow(r)

    print("Wrote: {}".format(summary_path))
    print("Wrote: {}  ({} rows)".format(details_path, len(details_rows)))
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Per-engine gate-rejection timeline for Omega logs")
    ap.add_argument('--log', required=True)
    ap.add_argument('--window-start', default=None)
    ap.add_argument('--window-end', default=None)
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()
    sys.exit(run(args.log, args.window_start, args.window_end, args.top))


if __name__ == '__main__':
    main()
