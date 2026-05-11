#!/usr/bin/env python3
# =============================================================================
# multiday_replay_2026-05-09.py
# =============================================================================
# Runs backtest/replay_2026-05-09 over every captured XAUUSD daily tape (data/
# and logs/), parses the per-day PRE-S22 (now-reverted production) and
# S22 OPTION B (deprecated) sections, and prints:
#   1. Per-day comparison table
#   2. Aggregate totals across all days
#   3. Realistic net at multiple cost models and lot sizes
#
# Single-day results are very noisy because gold has wildly varying volatility
# regimes. Multi-day aggregate is the real signal for whether the strategy
# clears costs across a typical 2-3 week window.
#
# USAGE:
#   python3 backtest/multiday_replay_2026-05-09.py
#     -- runs against every l2_ticks_XAUUSD_*.csv it finds in data/ + logs/
#
#   python3 backtest/multiday_replay_2026-05-09.py path/glob/*.csv
#     -- runs against a custom glob
#
# AUTHORISATION TRAIL: produced for user request 2026-05-09 in chat
# ("ok lets run this over the april and may data and see how it performs").
# =============================================================================

import glob
import os
import re
import subprocess
import sys
from collections import defaultdict

REPLAY_BINARY = "./backtest/replay_2026-05-09"
DEFAULT_GLOBS = [
    "logs/l2_ticks_XAUUSD_*.csv",     # preferred: most recent captures
    "data/l2_ticks_XAUUSD_*.csv",
]

# Regexes match the C++ harness's printf format.
RE_HEADER_TRADES   = re.compile(r"trades\s+:\s+(\d+)")
RE_HEADER_WINS     = re.compile(r"wins\s+:\s+(\d+)\s+\(([\d.]+)%\s+WR\)")
RE_AVG_PT          = re.compile(r"avg pt/trade\s+:\s+([+\-]?[\d.]+)\s+pt")
RE_AVG_HOLD        = re.compile(r"avg hold\s+:\s+([\d.]+)\s+s")
RE_GROSS_001       = re.compile(r"@ 0\.01 lot\s+:\s+([+\-]?[\d.]+)")
RE_GROSS_030       = re.compile(r"@ 0\.30 lot\s+:\s+([+\-]?[\d.]+)")
RE_TICKS           = re.compile(r"loaded (\d+) ticks")
RE_SPAN            = re.compile(r"span\s+=\s+([\d.]+) hours")
RE_INSESS_PCT      = re.compile(r"in 06-22 UTC session gate:.*?\(([\d.]+)%\)")


def parse_replay_output(text):
    """Parse the C++ binary's stdout/stderr blob into structured per-section data."""
    lines = text.split("\n")
    # Tape pre-flight
    ticks       = first_match(lines, RE_TICKS)
    span_h      = first_match(lines, RE_SPAN, fallback=0.0, cast=float)
    insess_pct  = first_match(lines, RE_INSESS_PCT, fallback=0.0, cast=float)

    # Locate the two engine section headers
    pre_start = None
    optb_start = None
    for i, ln in enumerate(lines):
        if "PRE-S22" in ln and pre_start is None:
            pre_start = i
        elif "S22 OPTION B" in ln and optb_start is None:
            optb_start = i

    pre  = parse_section(lines, pre_start, optb_start)
    optb = parse_section(lines, optb_start, len(lines))

    return {
        "ticks":      ticks,
        "span_h":     span_h,
        "insess_pct": insess_pct,
        "pre_s22":    pre,
        "option_b":   optb,
    }


def parse_section(lines, start, end):
    if start is None:
        return None
    section = "\n".join(lines[start:end])
    return {
        "trades":   parse_int(RE_HEADER_TRADES,   section),
        "wins":     parse_int(RE_HEADER_WINS,     section),
        "wr":       parse_float(RE_HEADER_WINS,   section, group=2),
        "avg_pt":   parse_float(RE_AVG_PT,        section),
        "avg_hold": parse_float(RE_AVG_HOLD,      section),
        "gross_001": parse_float(RE_GROSS_001,    section),
        "gross_030": parse_float(RE_GROSS_030,    section),
    }


def first_match(lines, regex, fallback=0, cast=int):
    for ln in lines:
        m = regex.search(ln)
        if m:
            try:
                return cast(m.group(1))
            except (ValueError, IndexError):
                pass
    return fallback


def parse_int(regex, text, group=1):
    m = regex.search(text)
    return int(m.group(group)) if m else 0


def parse_float(regex, text, group=1):
    m = regex.search(text)
    return float(m.group(group)) if m else 0.0


def date_from_filename(path):
    """Extract YYYY-MM-DD from a filename like l2_ticks_XAUUSD_2026-05-08.csv."""
    m = re.search(r"(\d{4}-\d{2}-\d{2})", os.path.basename(path))
    return m.group(1) if m else os.path.basename(path)


def collect_tapes():
    seen = {}  # date -> path (logs/ takes priority over data/)
    for pattern in DEFAULT_GLOBS:
        for path in glob.glob(pattern):
            d = date_from_filename(path)
            if d not in seen:
                seen[d] = path
            else:
                # Prefer logs/ over data/ (logs is fresher)
                existing = seen[d]
                if "logs/" in path and "logs/" not in existing:
                    seen[d] = path
                # Otherwise prefer the larger file
                elif os.path.getsize(path) > os.path.getsize(existing):
                    seen[d] = path
    return [seen[d] for d in sorted(seen.keys())]


def run_replay(path):
    try:
        result = subprocess.run(
            [REPLAY_BINARY, path],
            capture_output=True, text=True, timeout=120
        )
        return result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return ""
    except FileNotFoundError:
        print(f"[err] {REPLAY_BINARY} not found. Build it first:", file=sys.stderr)
        print(f"      clang++ -std=c++17 -O3 -DNDEBUG \\", file=sys.stderr)
        print(f"          backtest/replay_microscalper_2026-05-09.cpp \\", file=sys.stderr)
        print(f"          -o backtest/replay_2026-05-09", file=sys.stderr)
        sys.exit(1)


def fmt_money(v):
    sign = "+" if v >= 0 else ""
    return f"{sign}{v:>11,.2f}"


def main(argv):
    if len(argv) > 1:
        tapes = sorted(set(sum([glob.glob(p) for p in argv[1:]], [])),
                       key=date_from_filename)
    else:
        tapes = collect_tapes()

    if not tapes:
        print("[err] no tapes found. Specify a glob or place files in "
              "data/ or logs/", file=sys.stderr)
        return 1

    print(f"[ok] running multi-day replay across {len(tapes)} tapes...\n")

    rows = []
    for path in tapes:
        out = run_replay(path)
        parsed = parse_replay_output(out)
        if parsed["pre_s22"] is None:
            continue
        rows.append((path, parsed))
        print(f"  ran {path}: "
              f"PRE={parsed['pre_s22']['trades']} trades  "
              f"OptB={parsed['option_b']['trades'] if parsed['option_b'] else 0}")

    if not rows:
        print("[err] no parseable replay outputs", file=sys.stderr)
        return 1

    # Per-day table -- PRE-S22 (now-reverted production geometry)
    print()
    print("=" * 110)
    print("PER-DAY: PRE-S22 / rk12 / now-reverted PRODUCTION geometry")
    print("=" * 110)
    fmt_hdr = (f"{'DAY':<12} {'SPAN_H':>7} {'IN-SESS%':>9} {'TRADES':>7} "
               f"{'WR%':>6} {'AVG_PT':>8} {'HOLD':>6} "
               f"{'GROSS_0.01':>12} {'GROSS_0.30':>14}")
    print(fmt_hdr)
    print("-" * 110)
    tot_trades, tot_wins, tot_g001, tot_g030 = 0, 0, 0.0, 0.0
    for path, p in rows:
        d   = date_from_filename(path)
        pre = p["pre_s22"]
        print(f"{d:<12} {p['span_h']:>7.2f} {p['insess_pct']:>8.1f}% "
              f"{pre['trades']:>7d} {pre['wr']:>5.2f}% "
              f"{pre['avg_pt']:>+8.4f} {pre['avg_hold']:>5.1f}s "
              f"{fmt_money(pre['gross_001']):>12} "
              f"{fmt_money(pre['gross_030']):>14}")
        tot_trades += pre["trades"]
        tot_wins   += pre["wins"]
        tot_g001   += pre["gross_001"]
        tot_g030   += pre["gross_030"]
    print("-" * 110)
    avg_wr = (100.0 * tot_wins / tot_trades) if tot_trades else 0.0
    avg_pt = (tot_g001 / tot_trades) if tot_trades else 0.0  # @0.01 lot, $1=1pt
    print(f"{'TOTAL':<12} {'':>7} {'':>9} {tot_trades:>7d} {avg_wr:>5.2f}% "
          f"{avg_pt:>+8.4f} {'':>6} "
          f"{fmt_money(tot_g001):>12} "
          f"{fmt_money(tot_g030):>14}")
    print()

    # Per-day table -- S22 OPTION B (deprecated)
    print("=" * 110)
    print("PER-DAY: S22 OPTION B (DEPRECATED, kept for comparison)")
    print("=" * 110)
    print(fmt_hdr)
    print("-" * 110)
    obt_trades, obt_wins, obt_g001, obt_g030 = 0, 0, 0.0, 0.0
    for path, p in rows:
        d  = date_from_filename(path)
        ob = p["option_b"]
        if ob is None:
            continue
        print(f"{d:<12} {p['span_h']:>7.2f} {p['insess_pct']:>8.1f}% "
              f"{ob['trades']:>7d} {ob['wr']:>5.2f}% "
              f"{ob['avg_pt']:>+8.4f} {ob['avg_hold']:>5.1f}s "
              f"{fmt_money(ob['gross_001']):>12} "
              f"{fmt_money(ob['gross_030']):>14}")
        obt_trades += ob["trades"]
        obt_wins   += ob["wins"]
        obt_g001   += ob["gross_001"]
        obt_g030   += ob["gross_030"]
    print("-" * 110)
    avg_wr_ob = (100.0 * obt_wins / obt_trades) if obt_trades else 0.0
    avg_pt_ob = (obt_g001 / obt_trades) if obt_trades else 0.0
    print(f"{'TOTAL':<12} {'':>7} {'':>9} {obt_trades:>7d} {avg_wr_ob:>5.2f}% "
          f"{avg_pt_ob:>+8.4f} {'':>6} "
          f"{fmt_money(obt_g001):>12} "
          f"{fmt_money(obt_g030):>14}")
    print()

    # Realistic net at multiple cost models -- aggregate
    print("=" * 110)
    print("REALISTIC NET (aggregate across all days, real-cost on top of replay gross)")
    print("=" * 110)
    print(f"  Replay gross already eats spread on market entries/exits. The cost")
    print(f"  overlay below is the on-top deduction for: TP exits not eating")
    print(f"  spread, slippage on market orders, commission ($7/lot RT), and")
    print(f"  adverse selection. Range 0.20-0.35 pt/trade.\n")
    for cost_pt in [0.20, 0.25, 0.30, 0.35]:
        c001 = tot_trades * cost_pt * 100.0 * 0.01
        c030 = tot_trades * cost_pt * 100.0 * 0.30
        net001 = tot_g001 - c001
        net030 = tot_g030 - c030
        avg_day_001 = net001 / len(rows) if rows else 0.0
        avg_day_030 = net030 / len(rows) if rows else 0.0
        print(f"  @ {cost_pt:.2f} pt/trade extra cost:")
        print(f"    @ 0.01 lot total : {fmt_money(net001)}   "
              f"avg/day {fmt_money(avg_day_001)}")
        print(f"    @ 0.30 lot total : {fmt_money(net030)}   "
              f"avg/day {fmt_money(avg_day_030)}")
    print()

    # Day-type breakdown (high/mid/low vol) by trades-per-day
    print("=" * 110)
    print("VARIANCE: PRE-S22 per-day distribution (trade frequency = volatility proxy)")
    print("=" * 110)
    sorted_by_trades = sorted(
        [(date_from_filename(p), r["pre_s22"]) for p, r in rows],
        key=lambda x: x[1]["trades"], reverse=True
    )
    top_3   = ", ".join("{}={}".format(d, r["trades"]) for d, r in sorted_by_trades[:3])
    bot_3   = ", ".join("{}={}".format(d, r["trades"]) for d, r in sorted_by_trades[-3:])
    print(f"  Highest-vol days (most trades): {top_3}")
    print(f"  Lowest-vol days (fewest trades): {bot_3}")
    if rows:
        sorted_by_pt = sorted(
            [(date_from_filename(p), r["pre_s22"]) for p, r in rows],
            key=lambda x: x[1]["avg_pt"], reverse=True
        )
        best_3  = ", ".join("{}={:+.3f}pt".format(d, r["avg_pt"]) for d, r in sorted_by_pt[:3])
        worst_3 = ", ".join("{}={:+.3f}pt".format(d, r["avg_pt"]) for d, r in sorted_by_pt[-3:])
        print(f"  Best per-trade days: {best_3}")
        print(f"  Worst per-trade days: {worst_3}")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
