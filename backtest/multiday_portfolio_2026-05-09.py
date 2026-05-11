#!/usr/bin/env python3
# =============================================================================
# multiday_portfolio_2026-05-09.py
# =============================================================================
# Runs backtest/regime_portfolio_2026-05-09 over every captured XAUUSD daily
# tape and parses the four sections it prints:
#   (A) CHOP-ONLY        -- rk12 alone, no regime gate (= current production)
#   (B) TREND-ONLY       -- Donchian breakout alone, no regime gate
#   (C) PORTFOLIO chop   -- chop component of the regime-gated portfolio
#   (C) PORTFOLIO trend  -- trend component of the regime-gated portfolio
# Plus the combined total row.
#
# Output:
#   1. Per-day comparison: CHOP-ONLY vs PORTFOLIO combined
#   2. Aggregate totals across all days
#   3. Per-day CHOP / TREND component breakdown
#   4. Realistic net at multiple cost models
#
# USAGE:
#   python3 backtest/multiday_portfolio_2026-05-09.py
# =============================================================================

import glob
import os
import re
import subprocess
import sys

REPLAY_BINARY = "./backtest/regime_portfolio_2026-05-09"
DEFAULT_GLOBS = [
    "logs/l2_ticks_XAUUSD_*.csv",
    "data/l2_ticks_XAUUSD_*.csv",
]

# Section markers
SEC_CHOP_ONLY = "(A) CHOP-ONLY"
SEC_TREND_ONLY = "(B) TREND-ONLY"
SEC_PORT_CHOP  = "(C) PORTFOLIO -- CHOP component"
SEC_PORT_TREND = "(C) PORTFOLIO -- TREND component"
SEC_PORT_TOTAL = "PORTFOLIO COMBINED TOTAL"

RE_TRADES   = re.compile(r"trades\s*:\s*(\d+)")
RE_WINS     = re.compile(r"wins\s*:\s*(\d+)\s*\(([\d.]+)%\s*WR\)")
RE_AVG_PT   = re.compile(r"avg pt/trade\s*:\s*([+\-]?[\d.]+)")
RE_AVG_HOLD = re.compile(r"avg hold\s*:\s*([\d.]+)")
RE_RAW_PT   = re.compile(r"raw pt total\s*:\s*([+\-]?[\d.]+)")
RE_GROSS_001 = re.compile(r"GROSS USD @ 0\.01\s*:\s*([+\-]?[\d.]+)")
RE_GROSS_030 = re.compile(r"GROSS USD @ 0\.30\s*:\s*([+\-]?[\d.]+)")
RE_PORT_TRADES = re.compile(r"trades\s*:\s*(\d+)\s*\(chop=(\d+)\s*\+\s*trend=(\d+)\)")
RE_PORT_RAW    = re.compile(r"raw pt total\s*:\s*([+\-]?[\d.]+)\s*\(chop=([+\-]?[\d.]+)\s*\+\s*trend=([+\-]?[\d.]+)\)")
RE_REGIME_STATS = re.compile(r"Regime tape stats:\s*trend ticks=(\d+)\s*chop ticks=(\d+)")


def split_sections(text):
    """Split the binary's combined output into named sections."""
    sections = {}
    cur_name = None
    cur_lines = []
    for line in text.split("\n"):
        for marker in [SEC_CHOP_ONLY, SEC_TREND_ONLY,
                       SEC_PORT_CHOP, SEC_PORT_TREND,
                       SEC_PORT_TOTAL]:
            if marker in line:
                if cur_name:
                    sections[cur_name] = "\n".join(cur_lines)
                cur_name = marker
                cur_lines = [line]
                break
        else:
            if cur_name:
                cur_lines.append(line)
    if cur_name:
        sections[cur_name] = "\n".join(cur_lines)
    return sections


def parse_section(text, is_total=False):
    if not text:
        return None
    out = {
        "trades":    0,
        "wins":      0,
        "wr":        0.0,
        "avg_pt":    0.0,
        "avg_hold":  0.0,
        "raw_pt":    0.0,
        "gross_001": 0.0,
        "gross_030": 0.0,
    }
    if is_total:
        m = RE_PORT_TRADES.search(text)
        if m:
            out["trades"]      = int(m.group(1))
            out["chop_trades"] = int(m.group(2))
            out["trend_trades"]= int(m.group(3))
        m = RE_PORT_RAW.search(text)
        if m:
            out["raw_pt"]    = float(m.group(1))
            out["chop_raw"]  = float(m.group(2))
            out["trend_raw"] = float(m.group(3))
    else:
        m = RE_TRADES.search(text)
        if m: out["trades"] = int(m.group(1))
        m = RE_AVG_PT.search(text)
        if m: out["avg_pt"] = float(m.group(1))
        m = RE_AVG_HOLD.search(text)
        if m: out["avg_hold"] = float(m.group(1))
        m = RE_RAW_PT.search(text)
        if m: out["raw_pt"] = float(m.group(1))
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
        print(f"          backtest/regime_portfolio_2026-05-09.cpp \\", file=sys.stderr)
        print(f"          -o backtest/regime_portfolio_2026-05-09", file=sys.stderr)
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

    print(f"[ok] running multi-day Tier 3 portfolio replay across "
          f"{len(tapes)} tapes...\n")

    rows = []
    for path in tapes:
        out = run_replay(path)
        sections = split_sections(out)
        chop_only = parse_section(sections.get(SEC_CHOP_ONLY))
        trend_only = parse_section(sections.get(SEC_TREND_ONLY))
        port_chop  = parse_section(sections.get(SEC_PORT_CHOP))
        port_trend = parse_section(sections.get(SEC_PORT_TREND))
        port_total = parse_section(sections.get(SEC_PORT_TOTAL), is_total=True)
        if chop_only is None:
            continue
        rows.append({
            "path":       path,
            "date":       date_from_filename(path),
            "chop_only":  chop_only,
            "trend_only": trend_only or {},
            "port_chop":  port_chop or {},
            "port_trend": port_trend or {},
            "port_total": port_total or {},
        })
        co_t = chop_only["trades"]
        po_c = port_chop["trades"] if port_chop else 0
        po_t = port_trend["trades"] if port_trend else 0
        print(f"  ran {path}: chop_only={co_t}  port=({po_c}+{po_t})")

    if not rows:
        return 1

    # Per-day comparison: CHOP-ONLY vs PORTFOLIO COMBINED
    print()
    print("=" * 130)
    print("PER-DAY COMPARISON: CHOP-ONLY (= current production) vs PORTFOLIO (Tier 3)")
    print("=" * 130)
    fmt_hdr = (f"{'DAY':<12} | {'CHOP-ONLY':^46} | {'PORTFOLIO COMBINED':^54} | {'DELTA':^16}")
    print(fmt_hdr)
    print(f"{'':<12} | {'trades':>6} {'WR%':>6} {'avg_pt':>8} {'GROSS@0.30':>14} | "
          f"{'trades':>6} {'WR%':>6} {'raw_pt':>8} {'GROSS@0.30':>14} {'(c+t split)':>10} | "
          f"{'GROSS@0.30':>16}")
    print("-" * 130)

    totals_chop_only = {"trades": 0, "wins": 0, "g001": 0.0, "g030": 0.0}
    totals_port      = {"trades": 0, "wins": 0, "g001": 0.0, "g030": 0.0,
                        "chop_trades": 0, "trend_trades": 0,
                        "chop_raw": 0.0, "trend_raw": 0.0}

    for r in rows:
        co = r["chop_only"]
        pt = r["port_total"]
        port_g030 = (pt.get("raw_pt", 0.0)) * 100.0 * 0.30
        port_g001 = (pt.get("raw_pt", 0.0)) * 100.0 * 0.01
        ct = pt.get("chop_trades", 0)
        tt = pt.get("trend_trades", 0)
        delta_g030 = port_g030 - co["gross_030"]
        split_str = "({}+{})".format(ct, tt)
        port_avg_pt = (pt.get("raw_pt", 0) / max(pt.get("trades", 1), 1))
        print(f"{r['date']:<12} | "
              f"{co['trades']:>6d} {co['wr']:>5.2f}% {co['avg_pt']:>+8.4f} "
              f"{fmt_money(co['gross_030']):>14} | "
              f"{pt.get('trades',0):>6d} {pt.get('wr',0.0):>5.2f}% "
              f"{port_avg_pt:>+8.4f} "
              f"{fmt_money(port_g030):>14} "
              f"{split_str:>10} | "
              f"{fmt_money(delta_g030):>16}")
        totals_chop_only["trades"] += co["trades"]
        totals_chop_only["wins"]   += co["wins"]
        totals_chop_only["g001"]   += co["gross_001"]
        totals_chop_only["g030"]   += co["gross_030"]
        totals_port["trades"]      += pt.get("trades", 0)
        totals_port["wins"]        += pt.get("wins", 0)
        totals_port["g001"]        += port_g001
        totals_port["g030"]        += port_g030
        totals_port["chop_trades"] += ct
        totals_port["trend_trades"]+= tt
        totals_port["chop_raw"]    += pt.get("chop_raw", 0.0)
        totals_port["trend_raw"]   += pt.get("trend_raw", 0.0)

    print("-" * 130)
    co_wr = (100.0 * totals_chop_only["wins"] / totals_chop_only["trades"]) \
            if totals_chop_only["trades"] else 0.0
    po_wr = (100.0 * totals_port["wins"] / totals_port["trades"]) \
            if totals_port["trades"] else 0.0
    delta_total = totals_port["g030"] - totals_chop_only["g030"]
    split_total = "({}+{})".format(totals_port["chop_trades"], totals_port["trend_trades"])
    print(f"{'TOTAL':<12} | "
          f"{totals_chop_only['trades']:>6d} {co_wr:>5.2f}% "
          f"{(totals_chop_only['g001']/max(totals_chop_only['trades'],1)):>+8.4f} "
          f"{fmt_money(totals_chop_only['g030']):>14} | "
          f"{totals_port['trades']:>6d} {po_wr:>5.2f}% "
          f"{(totals_port['g001']/max(totals_port['trades'],1)):>+8.4f} "
          f"{fmt_money(totals_port['g030']):>14} "
          f"{split_total:>10} | "
          f"{fmt_money(delta_total):>16}")
    print()

    # Per-day breakdown of CHOP vs TREND components in the portfolio
    print("=" * 130)
    print("PORTFOLIO PER-DAY BREAKDOWN: chop component vs trend component")
    print("=" * 130)
    print(f"{'DAY':<12} | {'CHOP component':^36} | {'TREND component':^36} | "
          f"{'COMBINED':^28}")
    print(f"{'':<12} | {'trades':>6} {'WR%':>6} {'GROSS@0.30':>14} | "
          f"{'trades':>6} {'WR%':>6} {'GROSS@0.30':>14} | "
          f"{'GROSS@0.30':>14}")
    print("-" * 130)
    for r in rows:
        pc = r["port_chop"]
        ptr = r["port_trend"]
        ptot = r["port_total"]
        pc_g030 = pc.get("gross_030", 0.0)
        pt_g030 = ptr.get("gross_030", 0.0)
        comb = (ptot.get("raw_pt", 0.0)) * 100.0 * 0.30
        print(f"{r['date']:<12} | "
              f"{pc.get('trades',0):>6d} {pc.get('wr',0.0):>5.2f}% "
              f"{fmt_money(pc_g030):>14} | "
              f"{ptr.get('trades',0):>6d} {ptr.get('wr',0.0):>5.2f}% "
              f"{fmt_money(pt_g030):>14} | "
              f"{fmt_money(comb):>14}")
    print()

    # Realistic net cost overlay -- aggregate
    print("=" * 130)
    print("REALISTIC NET (aggregate, real-cost on top of replay gross)")
    print("=" * 130)
    print(f"  Replay already eats spread on market entries/exits. Cost overlay")
    print(f"  is on-top deduction for TP exits not eating spread, slip, etc.\n")
    n_co = totals_chop_only["trades"]
    n_po = totals_port["trades"]
    for cost_pt in [0.20, 0.25, 0.30, 0.35]:
        co_cost001 = n_co * cost_pt * 100.0 * 0.01
        co_cost030 = n_co * cost_pt * 100.0 * 0.30
        po_cost001 = n_po * cost_pt * 100.0 * 0.01
        po_cost030 = n_po * cost_pt * 100.0 * 0.30
        co_net001 = totals_chop_only["g001"] - co_cost001
        co_net030 = totals_chop_only["g030"] - co_cost030
        po_net001 = totals_port["g001"]      - po_cost001
        po_net030 = totals_port["g030"]      - po_cost030
        print(f"  @ {cost_pt:.2f} pt/trade:")
        print(f"    CHOP-ONLY  @ 0.01 lot: {fmt_money(co_net001)}  "
              f"@ 0.30 lot: {fmt_money(co_net030)}")
        print(f"    PORTFOLIO  @ 0.01 lot: {fmt_money(po_net001)}  "
              f"@ 0.30 lot: {fmt_money(po_net030)}")
        diff_001 = po_net001 - co_net001
        diff_030 = po_net030 - co_net030
        verdict = "PORTFOLIO WINS" if diff_030 > 0 else "CHOP-ONLY WINS"
        print(f"    Delta      @ 0.01 lot: {fmt_money(diff_001)}  "
              f"@ 0.30 lot: {fmt_money(diff_030)}  -> {verdict}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
