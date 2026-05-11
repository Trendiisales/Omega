#!/usr/bin/env python3
# =============================================================================
# multiday_asia_compare_2026-05-09.py
# =============================================================================
# Tests whether opening the session window past 06-22 UTC into the Asia hours
# (22-06 UTC, currently excluded) adds or detracts from total P&L.
#
# Runs option_a_chop_gated_2026-05-09 twice per tape:
#   1. Default (06-22 UTC session gate)
#   2. --no-session (entries permitted any UTC hour)
#
# Reports per-day delta and aggregate. The "ASIA CONTRIBUTION" column shows
# what extending the session into 22-06 UTC adds (could be positive, negative,
# or zero -- depends on the regime quality of Asia hours on this data).
#
# Uses the WINNING gate from Option A (ER<0.18) for both runs so we get the
# cleanest comparison.
#
# USAGE:
#   python3 backtest/multiday_asia_compare_2026-05-09.py
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

# Compare the WINNING ER<0.18 variant under both session modes
TARGET_VARIANT = "(B) CHOP-GATED  ER<0.18"

RE_TRADES   = re.compile(r"trades\s*:\s*(\d+)")
RE_WINS     = re.compile(r"wins\s*:\s*(\d+)\s*\(([\d.]+)%\s*WR\)")
RE_GROSS_001 = re.compile(r"GROSS USD @ 0\.01\s*:\s*([+\-]?[\d.]+)")
RE_GROSS_030 = re.compile(r"GROSS USD @ 0\.30\s*:\s*([+\-]?[\d.]+)")


def split_target_section(text):
    """Extract just the target variant's section from binary output."""
    lines = text.split("\n")
    start = None
    for i, ln in enumerate(lines):
        if TARGET_VARIANT in ln:
            start = i
            break
    if start is None:
        return ""
    end = len(lines)
    for j in range(start + 1, len(lines)):
        if lines[j].startswith("(") and "CHOP-" in lines[j]:
            end = j
            break
    return "\n".join(lines[start:end])


def parse_section(text):
    if not text:
        return None
    out = {"trades": 0, "wins": 0, "wr": 0.0, "gross_001": 0.0, "gross_030": 0.0}
    m = RE_TRADES.search(text)
    if m: out["trades"] = int(m.group(1))
    m = RE_WINS.search(text)
    if m:
        out["wins"] = int(m.group(1))
        out["wr"]   = float(m.group(2))
    m = RE_GROSS_001.search(text)
    if m: out["gross_001"] = float(m.group(1))
    m = RE_GROSS_030.search(text)
    if m: out["gross_030"] = float(m.group(1))
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


def run_replay(path, no_session):
    args = [REPLAY_BINARY]
    if no_session:
        args.append("--no-session")
    args.append(path)
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=180)
        return result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return ""
    except FileNotFoundError:
        print(f"[err] {REPLAY_BINARY} not found.", file=sys.stderr)
        sys.exit(1)


def fmt_money(v):
    sign = "+" if v >= 0 else ""
    return f"{sign}{v:>11,.2f}"


def main(argv):
    tapes = collect_tapes()
    if not tapes:
        print("[err] no tapes found.", file=sys.stderr)
        return 1

    print(f"[ok] running Asia-session comparison across {len(tapes)} tapes...")
    print(f"     using ER<0.18 gate (winner from Option A) under both modes\n")

    rows = []
    for path in tapes:
        d = date_from_filename(path)
        out_default = run_replay(path, no_session=False)
        out_full24  = run_replay(path, no_session=True)
        sess_on  = parse_section(split_target_section(out_default)) or {}
        sess_off = parse_section(split_target_section(out_full24))  or {}
        rows.append({"date": d, "path": path,
                     "sess_on": sess_on, "sess_off": sess_off})
        on_t  = sess_on.get("trades", 0)
        off_t = sess_off.get("trades", 0)
        print(f"  ran {d}: sess_on_trades={on_t}  full24_trades={off_t}  "
              f"asia_extra={off_t - on_t}")

    # Per-day comparison
    print()
    print("=" * 130)
    print("PER-DAY: 06-22 UTC session (current)  vs  full 24h (Asia included)")
    print("=" * 130)
    print(f"{'DAY':<12} | "
          f"{'SESSION 06-22 UTC':^36} | "
          f"{'FULL 24H':^36} | "
          f"{'ASIA CONTRIBUTION':^28}")
    print(f"{'':<12} | "
          f"{'trades':>7} {'WR%':>6} {'gross@0.30':>16} | "
          f"{'trades':>7} {'WR%':>6} {'gross@0.30':>16} | "
          f"{'asia_trades':>11} {'asia_$@0.30':>14}")
    print("-" * 130)

    tot_on  = {"trades": 0, "wins": 0, "g001": 0.0, "g030": 0.0}
    tot_off = {"trades": 0, "wins": 0, "g001": 0.0, "g030": 0.0}

    for r in rows:
        on  = r["sess_on"]
        off = r["sess_off"]
        asia_trades = off.get("trades", 0) - on.get("trades", 0)
        asia_g030   = off.get("gross_030", 0.0) - on.get("gross_030", 0.0)
        print(f"{r['date']:<12} | "
              f"{on.get('trades', 0):>7d} {on.get('wr', 0):>5.2f}% "
              f"{fmt_money(on.get('gross_030', 0.0)):>16} | "
              f"{off.get('trades', 0):>7d} {off.get('wr', 0):>5.2f}% "
              f"{fmt_money(off.get('gross_030', 0.0)):>16} | "
              f"{asia_trades:>11d} {fmt_money(asia_g030):>14}")
        for src, dst in [(on, tot_on), (off, tot_off)]:
            dst["trades"] += src.get("trades", 0)
            dst["wins"]   += src.get("wins",   0)
            dst["g001"]   += src.get("gross_001", 0.0)
            dst["g030"]   += src.get("gross_030", 0.0)

    print("-" * 130)
    on_wr  = (100.0 * tot_on["wins"]  / tot_on["trades"])  if tot_on["trades"]  else 0.0
    off_wr = (100.0 * tot_off["wins"] / tot_off["trades"]) if tot_off["trades"] else 0.0
    asia_total_trades = tot_off["trades"] - tot_on["trades"]
    asia_total_g030   = tot_off["g030"]   - tot_on["g030"]
    asia_total_g001   = tot_off["g001"]   - tot_on["g001"]
    print(f"{'TOTAL':<12} | "
          f"{tot_on['trades']:>7d} {on_wr:>5.2f}% "
          f"{fmt_money(tot_on['g030']):>16} | "
          f"{tot_off['trades']:>7d} {off_wr:>5.2f}% "
          f"{fmt_money(tot_off['g030']):>16} | "
          f"{asia_total_trades:>11d} {fmt_money(asia_total_g030):>14}")
    print()

    # Asia trade quality
    asia_avg_pt = (asia_total_g001 / max(asia_total_trades, 1)) if asia_total_trades else 0.0
    print("=" * 130)
    print("ASIA SESSION ECONOMICS (the 22-06 UTC trades the current production excludes)")
    print("=" * 130)
    print(f"  Asia-only trade count    : {asia_total_trades}")
    print(f"  Asia-only gross @ 0.01   : {fmt_money(asia_total_g001)}")
    print(f"  Asia-only gross @ 0.30   : {fmt_money(asia_total_g030)}")
    if asia_total_trades > 0:
        print(f"  Asia avg pt/trade        : {asia_avg_pt:+.4f} pt")
    print()

    # Net at multiple cost models
    print("=" * 130)
    print("REALISTIC NET: does extending into Asia improve overall returns?")
    print("=" * 130)
    for cost_pt in [0.20, 0.25, 0.30, 0.35]:
        on_cost  = tot_on["trades"]  * cost_pt * 100.0 * 0.30
        off_cost = tot_off["trades"] * cost_pt * 100.0 * 0.30
        on_net   = tot_on["g030"]  - on_cost
        off_net  = tot_off["g030"] - off_cost
        delta    = off_net - on_net
        verdict  = "ASIA HELPS" if delta > 0 else "ASIA HURTS"
        print(f"  @ {cost_pt:.2f} pt cost @ 0.30 lot:")
        print(f"    Session 06-22 net : {fmt_money(on_net)}")
        print(f"    Full 24h net      : {fmt_money(off_net)}")
        print(f"    Delta             : {fmt_money(delta)}  -> {verdict}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
