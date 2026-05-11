#!/usr/bin/env python3
# =============================================================================
# multiday_option_a_2026-05-09.py
# =============================================================================
# Runs backtest/option_a_chop_gated_2026-05-09 over every captured XAUUSD daily
# tape and parses the four sections it prints (no-gate baseline + 3 gated
# variants). Produces:
#
#   1. Per-day comparison: gross @ 0.30 lot for all 4 variants side-by-side
#   2. Aggregate totals for each variant
#   3. Realistic net at multiple cost models -- "which threshold WINS overall?"
#
# USAGE:
#   python3 backtest/multiday_option_a_2026-05-09.py
# =============================================================================

import glob
import os
import re
import subprocess
import sys

REPLAY_BINARY = "./backtest/option_a_chop_gated_2026-05-09"
DEFAULT_GLOBS = [
    "logs/l2_ticks_XAUUSD_*.csv",
    "data/l2_ticks_XAUUSD_*.csv",
]

# Section name fragments
VARIANT_NAMES = [
    ("CHOP-ONLY",         "(A) CHOP-ONLY"),
    ("ER<0.18",           "(B) CHOP-GATED  ER<0.18"),
    ("ER<0.25",           "(C) CHOP-GATED  ER<0.25"),
    ("ER<0.32",           "(D) CHOP-GATED  ER<0.32"),
]

RE_TRADES   = re.compile(r"trades\s*:\s*(\d+)")
RE_WINS     = re.compile(r"wins\s*:\s*(\d+)\s*\(([\d.]+)%\s*WR\)")
RE_AVG_PT   = re.compile(r"avg pt/trade\s*:\s*([+\-]?[\d.]+)")
RE_GROSS_001 = re.compile(r"GROSS USD @ 0\.01\s*:\s*([+\-]?[\d.]+)")
RE_GROSS_030 = re.compile(r"GROSS USD @ 0\.30\s*:\s*([+\-]?[\d.]+)")
RE_FILTERED = re.compile(r"filtered by regime\s*:\s*(\d+)")
RE_TREND_PCT = re.compile(r"regime tape stats\s*:\s*(\d+)\s*trend ticks\s*\(([\d.]+)%\)")


def split_sections(text):
    sections = {}
    cur_label = None
    cur_lines = []
    for line in text.split("\n"):
        for label, fragment in VARIANT_NAMES:
            if fragment in line:
                if cur_label:
                    sections[cur_label] = "\n".join(cur_lines)
                cur_label = label
                cur_lines = [line]
                break
        else:
            if cur_label:
                cur_lines.append(line)
    if cur_label:
        sections[cur_label] = "\n".join(cur_lines)
    return sections


def parse_section(text):
    if not text:
        return None
    out = {
        "trades":    0,
        "wins":      0,
        "wr":        0.0,
        "avg_pt":    0.0,
        "gross_001": 0.0,
        "gross_030": 0.0,
        "filtered":  0,
        "trend_pct": 0.0,
    }
    m = RE_TRADES.search(text)
    if m: out["trades"] = int(m.group(1))
    m = RE_WINS.search(text)
    if m:
        out["wins"] = int(m.group(1))
        out["wr"]   = float(m.group(2))
    m = RE_AVG_PT.search(text)
    if m: out["avg_pt"] = float(m.group(1))
    m = RE_GROSS_001.search(text)
    if m: out["gross_001"] = float(m.group(1))
    m = RE_GROSS_030.search(text)
    if m: out["gross_030"] = float(m.group(1))
    m = RE_FILTERED.search(text)
    if m: out["filtered"] = int(m.group(1))
    m = RE_TREND_PCT.search(text)
    if m: out["trend_pct"] = float(m.group(2))
    return out


def date_from_filename(path):
    m = re.search(r"(\d{4}-\d{2}-\d{2})", os.path.basename(path))
    return m.group(1) if m else os.path.basename(path)


def collect_tapes():
    seen = {}
    for pattern in DEFAULT_GLOBS:
        for path in glob.glob(pattern):
            d = date_from_filename(path)
            if d not in seen:
                seen[d] = path
            else:
                existing = seen[d]
                if "logs/" in path and "logs/" not in existing:
                    seen[d] = path
                elif os.path.getsize(path) > os.path.getsize(existing):
                    seen[d] = path
    return [seen[d] for d in sorted(seen.keys())]


def run_replay(path):
    try:
        result = subprocess.run(
            [REPLAY_BINARY, path],
            capture_output=True, text=True, timeout=180
        )
        return result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return ""
    except FileNotFoundError:
        print(f"[err] {REPLAY_BINARY} not found. Build it first:", file=sys.stderr)
        print(f"      clang++ -std=c++17 -O3 -DNDEBUG \\", file=sys.stderr)
        print(f"          backtest/option_a_chop_gated_2026-05-09.cpp \\", file=sys.stderr)
        print(f"          -o backtest/option_a_chop_gated_2026-05-09", file=sys.stderr)
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
        print("[err] no tapes found.", file=sys.stderr)
        return 1

    print(f"[ok] running Option A multi-day replay across {len(tapes)} tapes...\n")

    rows = []
    for path in tapes:
        out = run_replay(path)
        sections = split_sections(out)
        parsed = {}
        for label, _ in VARIANT_NAMES:
            parsed[label] = parse_section(sections.get(label)) or {}
        rows.append({"path": path, "date": date_from_filename(path), "v": parsed})
        co_t = parsed["CHOP-ONLY"].get("trades", 0)
        g18  = parsed["ER<0.18"].get("trades", 0)
        g25  = parsed["ER<0.25"].get("trades", 0)
        g32  = parsed["ER<0.32"].get("trades", 0)
        tp   = parsed["CHOP-ONLY"].get("trend_pct", 0.0)
        print(f"  ran {path}: trend_pct={tp:.1f}%  "
              f"trades(no_gate={co_t} 0.18={g18} 0.25={g25} 0.32={g32})")

    if not rows:
        return 1

    # Per-day comparison: gross @ 0.30 for all 4 variants
    print()
    print("=" * 120)
    print("PER-DAY COMPARISON: gross USD @ 0.30 lot for each variant")
    print("=" * 120)
    print(f"{'DAY':<12} {'TREND%':>7} | "
          f"{'NO-GATE':^15} | {'ER<0.18':^15} | {'ER<0.25':^15} | {'ER<0.32':^15}")
    print(f"{'':<12} {'':<7} | "
          f"{'gross@0.30':^15} | {'gross@0.30':^15} | {'gross@0.30':^15} | {'gross@0.30':^15}")
    print("-" * 120)
    totals = {label: {"trades": 0, "wins": 0, "g001": 0.0, "g030": 0.0, "filtered": 0}
              for label, _ in VARIANT_NAMES}
    for r in rows:
        v = r["v"]
        tp = v["CHOP-ONLY"].get("trend_pct", 0.0)
        line = f"{r['date']:<12} {tp:>6.1f}% |"
        for label, _ in VARIANT_NAMES:
            g030 = v[label].get("gross_030", 0.0)
            line += f" {fmt_money(g030):^15} |"
            totals[label]["trades"]   += v[label].get("trades", 0)
            totals[label]["wins"]     += v[label].get("wins", 0)
            totals[label]["g001"]     += v[label].get("gross_001", 0.0)
            totals[label]["g030"]     += v[label].get("gross_030", 0.0)
            totals[label]["filtered"] += v[label].get("filtered", 0)
        print(line)
    print("-" * 120)
    line = f"{'TOTAL':<12} {'':<7} |"
    for label, _ in VARIANT_NAMES:
        line += f" {fmt_money(totals[label]['g030']):^15} |"
    print(line)
    print()

    # Trade count and filter aggregate
    print("=" * 120)
    print("AGGREGATE TRADE COUNT + FILTER STATS")
    print("=" * 120)
    print(f"{'VARIANT':<14} {'TRADES':>8} {'WR':>7} {'GROSS@0.01':>14} "
          f"{'GROSS@0.30':>14} {'FILTERED':>10}")
    print("-" * 120)
    for label, _ in VARIANT_NAMES:
        t = totals[label]
        wr = (100.0 * t["wins"] / t["trades"]) if t["trades"] else 0.0
        print(f"{label:<14} {t['trades']:>8d} {wr:>6.2f}% "
              f"{fmt_money(t['g001']):>14} {fmt_money(t['g030']):>14} "
              f"{t['filtered']:>10}")
    print()

    # Realistic net per variant
    print("=" * 120)
    print("REALISTIC NET (real-cost overlay): which variant WINS overall?")
    print("=" * 120)
    print(f"  Replay already eats spread on market entries/exits. Cost overlay")
    print(f"  is on-top deduction for TP exits not eating spread, slip, etc.\n")
    for cost_pt in [0.20, 0.25, 0.30, 0.35]:
        print(f"  @ {cost_pt:.2f} pt/trade extra cost:")
        best_label = None
        best_net030 = float("-inf")
        for label, _ in VARIANT_NAMES:
            t = totals[label]
            n = t["trades"]
            cost001 = n * cost_pt * 100.0 * 0.01
            cost030 = n * cost_pt * 100.0 * 0.30
            net001 = t["g001"] - cost001
            net030 = t["g030"] - cost030
            print(f"    {label:<14}  net @ 0.01 lot: {fmt_money(net001)}  "
                  f"@ 0.30 lot: {fmt_money(net030)}")
            if net030 > best_net030:
                best_net030 = net030
                best_label = label
        print(f"    -> WINNER: {best_label} ({fmt_money(best_net030)} @ 0.30 lot)\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
