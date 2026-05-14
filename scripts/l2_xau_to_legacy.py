#!/usr/bin/env python3
"""
l2_xau_to_legacy.py -- Convert XAU L2 capture files to the legacy
htf_bt_minimal tick format.

Created 2026-05-14 part-AA-pre, supporting the MinimalH4Gold L2-month backtest
follow-up to S67. Mirrors the scripts/duka_to_legacy.py converter pattern but
for the L2-capture schema.

L2 file header (two flavours observed in the omega_repo data/ directory):
  Flavour A (XAUUSD-prefixed, 2026-04-22+): includes 'ts_ms,mid,bid,ask,...'
  Flavour B (unprefixed,    2026-04-09 .. 2026-04-22): includes 'ts_ms,bid,ask,...'
Both have 'ts_ms' as col 1 (Unix epoch milliseconds, UTC).
We parse the header line to find ts_ms / bid / ask column indices, robust to
both flavours.

Output (matches scripts/duka_to_legacy.py output, which htf_bt_minimal parses):
  YYYYMMDD,HH:MM:SS,bid,ask

Lines are emitted in input-file order; caller is responsible for passing the
input files in chronological order. Each input file is internally already
chronologically sorted by ts_ms.

Usage:
  python3 scripts/l2_xau_to_legacy.py \
      --out /Users/jo/Tick/legacy/XAUUSD_L2_2026-04-09_2026-05-08_legacy.csv \
      data/l2_ticks_2026-04-09.csv \
      data/l2_ticks_2026-04-10.csv \
      ...
"""
import sys
import datetime
import argparse


def convert(in_paths, out_path):
    written = 0
    skipped_bad = 0
    skipped_files = 0
    with open(out_path, 'w') as out:
        for in_path in in_paths:
            try:
                fh = open(in_path, 'r')
            except OSError as e:
                print(f"  SKIP {in_path}: {e}", file=sys.stderr)
                skipped_files += 1
                continue
            with fh:
                hdr_line = fh.readline().rstrip('\n')
                hdr = hdr_line.split(',')
                try:
                    i_ts = hdr.index('ts_ms')
                    i_bid = hdr.index('bid')
                    i_ask = hdr.index('ask')
                except ValueError:
                    print(f"  SKIP {in_path}: missing ts_ms/bid/ask in header: {hdr_line[:80]}",
                          file=sys.stderr)
                    skipped_files += 1
                    continue
                file_count = 0
                for line in fh:
                    parts = line.rstrip('\n').split(',')
                    if len(parts) <= max(i_ts, i_bid, i_ask):
                        skipped_bad += 1
                        continue
                    try:
                        ts_ms = int(parts[i_ts])
                        bid = float(parts[i_bid])
                        ask = float(parts[i_ask])
                    except ValueError:
                        skipped_bad += 1
                        continue
                    if bid <= 0 or ask <= 0:
                        skipped_bad += 1
                        continue
                    dt = datetime.datetime.fromtimestamp(
                        ts_ms / 1000.0, tz=datetime.timezone.utc)
                    out.write(f"{dt.strftime('%Y%m%d')},{dt.strftime('%H:%M:%S')},"
                              f"{bid:.5f},{ask:.5f}\n")
                    written += 1
                    file_count += 1
            print(f"  done: {in_path}  ({file_count} ticks)", file=sys.stderr)
    print(f"\nTotal written: {written} ticks", file=sys.stderr)
    print(f"Skipped bad lines: {skipped_bad}", file=sys.stderr)
    print(f"Skipped files: {skipped_files}", file=sys.stderr)
    print(f"Output: {out_path}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', required=True, help='Output legacy CSV path')
    ap.add_argument('inputs', nargs='+', help='Input L2 CSV files (in chronological order)')
    args = ap.parse_args()
    convert(args.inputs, args.out)


if __name__ == '__main__':
    main()
