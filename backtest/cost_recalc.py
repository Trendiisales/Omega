#!/usr/bin/env python3
# ============================================================================
# cost_recalc.py  --  Option 3 from SESSION_HANDOFF_2026-04-29_pm.md
#
# Re-cost any *_duka_bt_trades.csv using the per-month median bid-ask spread
# from tick_q.csv (output of backtest/tick_quality.py).  Non-invasive: no
# engine code is touched, no rebuild needed.
#
# Why this exists
# ---------------
# The three BT harnesses (hbg_duka_bt, cfe_duka_bt, mce_duka_bt) all call
#     omega::apply_realistic_costs(tr, 3.0, 100.0)
# inside their on_close callbacks.  apply_realistic_costs (defined in
# include/OmegaTradeLedger.hpp) computes:
#
#     slippage_entry = (spreadAtEntry / 2) * tick_mult * size
#     slippage_exit  = (exitReason == "TP_HIT") ? 0.0
#                    : (spreadAtEntry / 2) * tick_mult * size
#     commission     = commission_per_side * 2 * size
#     net_pnl        = gross_pnl - slippage_entry - slippage_exit - commission
#
# but the value of `tr.spreadAtEntry` that each engine stamps is wildly
# inconsistent:
#
#   * HBG  -- captures pos.spread_at_entry from confirm_fill (~1.8pt median,
#             swinging to 11.85pt; way wider than real bid-ask).  Likely
#             reflects bracket-width or stale tick state, not bid-ask.
#   * CFE  -- effectively 0 for every trade (no spread cost at all).
#   * MCE  -- effectively 0 for every trade (no spread cost at all).
#
# Meanwhile real XAUUSD median spread per tick_q.csv is 0.34pt (2024-Q1) to
# 0.92pt (2026-02 peak).  None of the three engines is using that.
#
# This script normalises all three by re-costing every trade with the
# month-of-entry median spread, holding everything else fixed.  Output gives
# you per-engine and per-month "reported P&L was X; corrected for actual
# market spread is Y; gap is Z" so the size of the cost-mis-estimation is
# visible across the regime.
#
# Usage
# -----
#   python3 backtest/cost_recalc.py --tick-quality tick_q.csv \
#           hbg_duka_bt_trades.csv mce_duka_bt_trades.csv cfe_duka_bt_trades.csv
#
# Each input *_trades.csv yields a corresponding *_trades_recost.csv next to
# it, with the original columns plus:
#     ym                       -- YYYY-MM derived from entry_ts (UTC)
#     market_spread_pt         -- |tick_q.csv med_spread_pt| for that ym
#     implied_engine_spread_pt -- back-derived from gross/net delta minus commission
#     corrected_cost           -- entry_slip + exit_slip + commission at market spread
#     corrected_net_pnl        -- gross_pnl - corrected_cost
#     gap                      -- corrected_net_pnl - reported net_pnl
#
# Validation expectations from §"How to validate cost_recalc.py output" of
# the handoff:
#   1. Pre-2026 trades: corrected ~ reported within ~5% (real spread was
#      near 0.3pt, close to the engines' assumption -- though see asymmetry above).
#   2. 2026-Q1 trades: corrected substantially worse than reported for
#      engines that were under-charging (CFE/MCE -- 0pt).
#   3. HBG 2026-Q1: corrected P&L roughly -$6,500 to -$7,500 if the spread
#      assumption was 0.3pt.  In reality HBG was *over-charging* at ~1.8pt
#      median, so HBG's corrected 2026-Q1 may actually IMPROVE.  We will
#      print the magnitudes either way and flag what we find.
# ============================================================================

from __future__ import annotations

import argparse
import csv
import math
import os
import sys
from collections import defaultdict
from datetime import datetime, timezone
from typing import Dict, List, Optional, Tuple


# ----------------------------------------------------------------------------
# Tick-quality CSV  ->  ym -> abs(median spread, points)
# ----------------------------------------------------------------------------
def load_spread_table(path: str) -> Dict[str, float]:
    """
    Load tick_q.csv produced by backtest/tick_quality.py.

    The CSV that exists in the repo as of this commit was written by the
    PRE-FIX version of tick_quality.py (column-order bug: it treated
    askPrice as bid and bidPrice as ask, producing negative spreads).
    Magnitudes are correct, sign is wrong.  We use abs() to be safe with
    either pre-fix or post-fix outputs.

    Returns: { "2024-03": 0.34, ..., "2026-04": 0.71 }
    """
    table: Dict[str, float] = {}
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ym = row.get("ym", "").strip()
            raw = row.get("med_spread_pt", "").strip()
            if not ym or not raw:
                continue
            try:
                v = abs(float(raw))
            except ValueError:
                continue
            table[ym] = v
    if not table:
        raise RuntimeError(f"no rows loaded from {path}")
    return table


# ----------------------------------------------------------------------------
# Trade-row helpers
# ----------------------------------------------------------------------------
def ym_of_entry(entry_ts_str: str) -> Optional[str]:
    """
    entry_ts in the *_duka_bt_trades.csv files is unix-seconds (UTC).
    Convert to "YYYY-MM" using UTC, matching the convention used inside
    each harness's by_month bucket (std::gmtime in hbg_duka_bt.cpp).
    """
    try:
        v = int(float(entry_ts_str))
    except (ValueError, TypeError):
        return None
    if v <= 0:
        return None
    dt = datetime.fromtimestamp(v, tz=timezone.utc)
    return f"{dt.year:04d}-{dt.month:02d}"


def back_derive_engine_spread(
    gross_pnl: float,
    net_pnl: float,
    size: float,
    exit_reason: str,
    tick_mult: float,
    commission_per_side: float,
) -> float:
    """
    Solve apply_realistic_costs() for the value of spreadAtEntry that the
    engine actually used, given gross_pnl and the recorded net_pnl in the
    trade CSV.

    With size>0, tick_mult>0:
        delta      = gross - net
        commission = commission_per_side * 2 * size
        rest       = delta - commission
        rest = (size * tick_mult) * spread        for non-TP exits
        rest = (size * tick_mult / 2) * spread    for TP_HIT exits
    """
    if size <= 0 or tick_mult <= 0:
        return 0.0
    delta = gross_pnl - net_pnl
    commission = commission_per_side * 2.0 * size
    rest = delta - commission
    if exit_reason == "TP_HIT":
        denom = 0.5 * tick_mult * size
    else:
        denom = tick_mult * size
    if denom == 0.0:
        return 0.0
    return rest / denom


def corrected_cost(
    spread_pt: float,
    size: float,
    exit_reason: str,
    tick_mult: float,
    commission_per_side: float,
) -> float:
    """
    Apply the same apply_realistic_costs() formula, but with `spread_pt`
    plugged in for spreadAtEntry.  Returns total cost in USD (entry slip
    + exit slip + commission), all positive.
    """
    half = (spread_pt / 2.0) * tick_mult * size
    slip_entry = half
    slip_exit = 0.0 if exit_reason == "TP_HIT" else half
    commission = commission_per_side * 2.0 * size
    return slip_entry + slip_exit + commission


# ----------------------------------------------------------------------------
# Per-trade-CSV processor
# ----------------------------------------------------------------------------
class EngineSummary:
    __slots__ = ("name", "n", "reported_net", "corrected_net",
                 "by_month", "missing_months", "implied_spreads")

    def __init__(self, name: str) -> None:
        self.name = name
        self.n = 0
        self.reported_net = 0.0
        self.corrected_net = 0.0
        # ym -> [n, reported_net_sum, corrected_net_sum]
        self.by_month: Dict[str, List[float]] = defaultdict(
            lambda: [0.0, 0.0, 0.0]
        )
        self.missing_months: Dict[str, int] = defaultdict(int)
        self.implied_spreads: List[float] = []


def process_trade_csv(
    in_path: str,
    out_path: str,
    spread_table: Dict[str, float],
    tick_mult: float,
    commission_per_side: float,
    fallback_ym_spread: float,
) -> EngineSummary:
    """
    Read in_path; write per-trade out_path with extra columns; return
    aggregated EngineSummary.
    """
    summary = EngineSummary(name=os.path.basename(in_path))

    extra_cols = [
        "ym",
        "market_spread_pt",
        "implied_engine_spread_pt",
        "corrected_cost",
        "corrected_net_pnl",
        "gap",
    ]

    with open(in_path, newline="") as f_in, open(out_path, "w", newline="") as f_out:
        reader = csv.DictReader(f_in)
        if reader.fieldnames is None:
            raise RuntimeError(f"no header in {in_path}")
        out_fields = list(reader.fieldnames) + extra_cols
        writer = csv.DictWriter(f_out, fieldnames=out_fields)
        writer.writeheader()

        for row in reader:
            try:
                size = float(row["size"])
                gross = float(row["gross_pnl"])
                net = float(row["net_pnl"])
                exit_reason = row.get("exit_reason", "")
            except (KeyError, ValueError):
                continue

            ym = ym_of_entry(row.get("entry_ts", ""))
            if ym is None:
                continue

            if ym in spread_table:
                spread_pt = spread_table[ym]
            else:
                summary.missing_months[ym] += 1
                spread_pt = fallback_ym_spread

            implied = back_derive_engine_spread(
                gross_pnl=gross,
                net_pnl=net,
                size=size,
                exit_reason=exit_reason,
                tick_mult=tick_mult,
                commission_per_side=commission_per_side,
            )
            cost = corrected_cost(
                spread_pt=spread_pt,
                size=size,
                exit_reason=exit_reason,
                tick_mult=tick_mult,
                commission_per_side=commission_per_side,
            )
            corrected_net = gross - cost
            gap = corrected_net - net

            row["ym"] = ym
            row["market_spread_pt"] = f"{spread_pt:.4f}"
            row["implied_engine_spread_pt"] = f"{implied:.4f}"
            row["corrected_cost"] = f"{cost:.4f}"
            row["corrected_net_pnl"] = f"{corrected_net:.4f}"
            row["gap"] = f"{gap:.4f}"
            writer.writerow(row)

            summary.n += 1
            summary.reported_net += net
            summary.corrected_net += corrected_net
            bucket = summary.by_month[ym]
            bucket[0] += 1
            bucket[1] += net
            bucket[2] += corrected_net
            summary.implied_spreads.append(implied)

    return summary


# ----------------------------------------------------------------------------
# Reporting
# ----------------------------------------------------------------------------
def median(xs: List[float]) -> float:
    if not xs:
        return float("nan")
    s = sorted(xs)
    n = len(s)
    if n % 2 == 1:
        return s[n // 2]
    return 0.5 * (s[n // 2 - 1] + s[n // 2])


def percentile(xs: List[float], p: float) -> float:
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = (len(s) - 1) * p
    lo = int(math.floor(k))
    hi = int(math.ceil(k))
    if lo == hi:
        return s[lo]
    return s[lo] * (hi - k) + s[hi] * (k - lo)


def fmt_money(v: float) -> str:
    if math.isnan(v):
        return "    nan"
    return f"${v:>12,.2f}"


def fmt_pct(v: float) -> str:
    if math.isnan(v):
        return "  nan%"
    return f"{v:>+6.1f}%"


def print_engine_report(s: EngineSummary) -> None:
    print()
    print("=" * 100)
    print(f"  {s.name}    n={s.n}")
    print("=" * 100)

    # Implied spread distribution (sanity vs handoff §"New finding")
    if s.implied_spreads:
        spreads = s.implied_spreads
        print(
            f"  engine implied spread (pt):  "
            f"min={min(spreads):.4f}  "
            f"p25={percentile(spreads, 0.25):.4f}  "
            f"med={median(spreads):.4f}  "
            f"p75={percentile(spreads, 0.75):.4f}  "
            f"max={max(spreads):.4f}"
        )

    # Headline
    rep, cor = s.reported_net, s.corrected_net
    gap = cor - rep
    pct = (gap / abs(rep) * 100.0) if rep != 0.0 else float("nan")
    print(f"  TOTAL    reported = {fmt_money(rep)}    "
          f"corrected = {fmt_money(cor)}    "
          f"gap = {fmt_money(gap)}    ({fmt_pct(pct)})")

    # Per-month breakdown
    print()
    print(f"  {'ym':<10} {'n':>5} {'reported':>16} {'corrected':>16} "
          f"{'gap':>16} {'gap %':>8}  {'mkt spread':>10}")
    print(f"  {'-'*10} {'-'*5} {'-'*16} {'-'*16} {'-'*16} {'-'*8}  {'-'*10}")
    for ym in sorted(s.by_month):
        n, r, c = s.by_month[ym]
        g = c - r
        p = (g / abs(r) * 100.0) if r != 0.0 else float("nan")
        # spread for display: re-pull from any single trade in that month
        # is awkward; instead pull from the script's spread_table via the
        # per-trade output -- handled in the master report block.
        print(
            f"  {ym:<10} {int(n):>5} {fmt_money(r):>16} {fmt_money(c):>16} "
            f"{fmt_money(g):>16} {fmt_pct(p):>8}"
        )

    # 2026-Q1 isolated subtotal -- the months the handoff cares about most
    q1_ms = ("2026-01", "2026-02", "2026-03")
    q1 = [s.by_month[m] for m in q1_ms if m in s.by_month]
    if q1:
        n_sum = sum(int(b[0]) for b in q1)
        r_sum = sum(b[1] for b in q1)
        c_sum = sum(b[2] for b in q1)
        g_sum = c_sum - r_sum
        p_sum = (g_sum / abs(r_sum) * 100.0) if r_sum != 0.0 else float("nan")
        print(f"  {'  Q1-2026':<10} {n_sum:>5} {fmt_money(r_sum):>16} "
              f"{fmt_money(c_sum):>16} {fmt_money(g_sum):>16} "
              f"{fmt_pct(p_sum):>8}")

    if s.missing_months:
        print()
        print("  WARNING: trades with month not in tick_q.csv "
              "(used fallback spread):")
        for ym in sorted(s.missing_months):
            print(f"    {ym}: {s.missing_months[ym]} trades")


def print_cross_engine_summary(summaries: List[EngineSummary]) -> None:
    if len(summaries) < 2:
        return
    print()
    print("=" * 100)
    print("  CROSS-ENGINE SUMMARY")
    print("=" * 100)
    header = f"  {'engine':<32} {'n':>5} {'reported':>16} {'corrected':>16} {'gap':>16}"
    print(header)
    print(f"  {'-'*32} {'-'*5} {'-'*16} {'-'*16} {'-'*16}")
    for s in summaries:
        gap = s.corrected_net - s.reported_net
        print(f"  {s.name:<32} {s.n:>5} {fmt_money(s.reported_net):>16} "
              f"{fmt_money(s.corrected_net):>16} {fmt_money(gap):>16}")


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(
        description="Re-cost BT trade CSVs using per-month market spread "
                    "from tick_q.csv (Option 3 of handoff doc)."
    )
    p.add_argument(
        "trade_csvs",
        nargs="+",
        help="One or more *_duka_bt_trades.csv files",
    )
    p.add_argument(
        "--tick-quality",
        default="tick_q.csv",
        help="tick_q.csv produced by backtest/tick_quality.py "
             "(default: tick_q.csv in cwd)",
    )
    p.add_argument(
        "--tick-mult",
        type=float,
        default=100.0,
        help="USD per price-point per lot (XAUUSD=100; see tick_value_multiplier "
             "comment in OmegaTradeLedger.hpp). default=100",
    )
    p.add_argument(
        "--commission-per-side",
        type=float,
        default=3.0,
        help="USD commission per side per lot.  Matches "
             "apply_realistic_costs(tr, 3.0, 100.0) in the harnesses.  default=3.0",
    )
    p.add_argument(
        "--fallback-spread-pt",
        type=float,
        default=0.50,
        help="Spread (points) used when a trade's month is not in "
             "tick_q.csv.  default=0.50 (rough corpus median)",
    )
    p.add_argument(
        "--out-dir",
        default=None,
        help="Where to write *_recost.csv files.  Default: same directory "
             "as each input.",
    )
    args = p.parse_args(argv)

    # Load spread table
    if not os.path.exists(args.tick_quality):
        print(f"ERROR: --tick-quality file not found: {args.tick_quality}",
              file=sys.stderr)
        return 2
    spread_table = load_spread_table(args.tick_quality)

    print(f"Loaded {len(spread_table)} months of spread baseline from "
          f"{args.tick_quality}")
    if spread_table:
        # show first/last for sanity
        keys = sorted(spread_table)
        print(f"  range: {keys[0]} = {spread_table[keys[0]]:.4f}pt   "
              f"...   {keys[-1]} = {spread_table[keys[-1]]:.4f}pt")
        # show 2026-Q1 specifically (the months that matter)
        for ym in ("2026-01", "2026-02", "2026-03", "2026-04"):
            if ym in spread_table:
                print(f"  {ym}: {spread_table[ym]:.4f}pt")
    print()

    summaries: List[EngineSummary] = []
    for in_path in args.trade_csvs:
        if not os.path.exists(in_path):
            print(f"  skipping (not found): {in_path}", file=sys.stderr)
            continue
        if args.out_dir:
            os.makedirs(args.out_dir, exist_ok=True)
            base = os.path.basename(in_path)
            stem, _ = os.path.splitext(base)
            out_path = os.path.join(args.out_dir, f"{stem}_recost.csv")
        else:
            stem, ext = os.path.splitext(in_path)
            out_path = f"{stem}_recost{ext}"
        print(f"Processing {in_path}  ->  {out_path}")
        summary = process_trade_csv(
            in_path=in_path,
            out_path=out_path,
            spread_table=spread_table,
            tick_mult=args.tick_mult,
            commission_per_side=args.commission_per_side,
            fallback_ym_spread=args.fallback_spread_pt,
        )
        summaries.append(summary)
        print_engine_report(summary)

    print_cross_engine_summary(summaries)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
