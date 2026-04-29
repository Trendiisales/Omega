#!/usr/bin/env python3
# ============================================================================
# post_regime_baseline.py -- 2026-04-29 LATE
# ----------------------------------------------------------------------------
# Goal
#   Produce a per-engine "before" scoresheet on post-Apr-2025 trades.  The
#   2025-04 cut is the empirical microstructure regime change confirmed by
#   tick_quality.py: median XAUUSD spread stepped from 0.55pt to 0.68pt
#   between 2025-03 and 2025-04 and stayed in 0.55-0.92pt range through
#   2026-04.  Anything before 2025-04 calibrates to a regime that no longer
#   exists; anything after 2025-04 reflects the live cost environment.
#
#   This tool reads every trade CSV in the repo, identifies which engine
#   produced each trade, filters to entry_ts >= 2025-04-01 UTC, and emits
#   a scoresheet (one row per engine) with:
#     - n_trades, n_wins, win_rate
#     - gross_pnl, net_pnl, expectancy_per_trade
#     - max_drawdown (running net_pnl peak-to-trough)
#     - sharpe (per-trade-return mean / stddev * sqrt(n))
#     - exit reason histogram (top 4 reasons by frequency)
#
#   No engine code is touched; this is purely a measurement script.  Use it
#   as the "before" snapshot for any retune, then re-run after the retune
#   to compare apples-to-apples.
#
# Inputs (auto-discovered, read-only)
#   * /Users/jo/omega_repo/hbg_duka_bt_trades.csv      -- HBG (legacy schema)
#   * /Users/jo/omega_repo/cfe_duka_bt_trades.csv      -- CFE (legacy schema)
#   * /Users/jo/omega_repo/mce_duka_bt_trades.csv      -- MCE (legacy schema)
#   * /Users/jo/omega_repo/backtest/bt_*_trades.csv    -- multi-engine schema
#                                                         (column 'engine')
#
#   Schema A (legacy per-engine):
#     id,symbol,side,exit_reason,entry,exit,sl,tp,size,
#     gross_pnl,net_pnl,mfe,mae,entry_ts,exit_ts
#     -> all rows attributed to the engine implied by the filename
#
#   Schema B (multi-engine, column 'engine'):
#     entryTs,symbol,side,engine,entryPrice,exitPrice,pnl,mfe,mae,
#     hold_sec,exitReason,spreadAtEntry,atrAtEntry,latencyMs,regime
#     -> rows grouped by 'engine' column.  net P&L not present; we use
#        the 'pnl' column as both gross and net (best signal we have).
#
# Usage
#   python3 backtest/post_regime_baseline.py
#   python3 backtest/post_regime_baseline.py --since 2025-04-01
#   python3 backtest/post_regime_baseline.py --out RETUNE_BASELINE.txt
#
# Output
#   stdout: human-readable per-engine table
#   --out:  same content written to a file (also keeps stdout)
# ============================================================================

import argparse
import csv
import math
import os
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------------------
# Per-engine CSVs in the repo root.  Each entry maps (filename, schema_A_engine_name).
# Schema-A files are attributed entirely to one engine because the CSV does
# not carry an engine column.
# ---------------------------------------------------------------------------
SCHEMA_A_FILES = [
    ("hbg_duka_bt_trades.csv", "HybridBracketGold"),
    ("cfe_duka_bt_trades.csv", "CandleFlowEngine"),
    ("mce_duka_bt_trades.csv", "MacroCrash"),
]

# Schema-B files in /backtest -- multi-engine, attribute by 'engine' column.
SCHEMA_B_FILES = [
    "backtest/bt_trades.csv",
    "backtest/bt_26m_clean_trades.csv",
    "backtest/bt_26m_v2_trades.csv",
    "backtest/bt_26m_s45fix1_trades.csv",
    "backtest/bt_26m_trades.csv",
    "backtest/bt_april_trades.csv",
]


def epoch_seconds_for_iso(date_str):
    """Convert a date string like '2025-04-01' to UTC epoch seconds."""
    dt = datetime.strptime(date_str, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    return int(dt.timestamp())


def safe_float(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def safe_int(v, default=0):
    try:
        # Some entry_ts fields are floats (with .0 suffix); pass through float first
        return int(float(v))
    except (TypeError, ValueError):
        return default


# ---------------------------------------------------------------------------
# Aggregator -- accumulates per-engine stats from rows
# ---------------------------------------------------------------------------
class EngineStats:
    __slots__ = ("name", "trades", "wins", "gross", "net",
                 "exit_counts", "running_peak", "max_dd", "running_net",
                 "pnl_series", "first_ts", "last_ts")

    def __init__(self, name):
        self.name = name
        self.trades = 0
        self.wins = 0
        self.gross = 0.0
        self.net = 0.0
        self.exit_counts = Counter()
        self.running_peak = 0.0
        self.max_dd = 0.0
        self.running_net = 0.0
        self.pnl_series = []
        self.first_ts = None
        self.last_ts = None

    def add(self, gross_pnl, net_pnl, exit_reason, entry_ts):
        self.trades += 1
        if net_pnl > 0:
            self.wins += 1
        self.gross += gross_pnl
        self.net += net_pnl
        self.exit_counts[exit_reason or "?"] += 1
        self.running_net += net_pnl
        if self.running_net > self.running_peak:
            self.running_peak = self.running_net
        dd = self.running_peak - self.running_net
        if dd > self.max_dd:
            self.max_dd = dd
        self.pnl_series.append(net_pnl)
        if self.first_ts is None or entry_ts < self.first_ts:
            self.first_ts = entry_ts
        if self.last_ts is None or entry_ts > self.last_ts:
            self.last_ts = entry_ts

    def expectancy(self):
        return self.net / self.trades if self.trades else 0.0

    def win_rate(self):
        return 100.0 * self.wins / self.trades if self.trades else 0.0

    def sharpe(self):
        n = len(self.pnl_series)
        if n < 2:
            return 0.0
        mean = sum(self.pnl_series) / n
        var = sum((p - mean) ** 2 for p in self.pnl_series) / (n - 1)
        sd = math.sqrt(var) if var > 0 else 0.0
        if sd <= 0.0:
            return 0.0
        return (mean / sd) * math.sqrt(n)

    def top_exits(self, k=4):
        return self.exit_counts.most_common(k)

    def date_range(self):
        if self.first_ts is None:
            return ("-", "-")
        a = datetime.fromtimestamp(self.first_ts, tz=timezone.utc).strftime("%Y-%m-%d")
        b = datetime.fromtimestamp(self.last_ts,  tz=timezone.utc).strftime("%Y-%m-%d")
        return (a, b)


# ---------------------------------------------------------------------------
# Loaders -- read each schema and feed the aggregator
# ---------------------------------------------------------------------------
def load_schema_a(path, engine_name, since_ts, agg):
    """Legacy per-engine schema -- HBG/CFE/MCE."""
    if not os.path.exists(path):
        return 0, 0
    raw = 0
    kept = 0
    with open(path, newline="") as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            raw += 1
            ts = safe_int(row.get("entry_ts"))
            if ts < since_ts:
                continue
            gross = safe_float(row.get("gross_pnl"))
            net   = safe_float(row.get("net_pnl"))
            exit_reason = (row.get("exit_reason") or "").strip()
            agg[engine_name].add(gross, net, exit_reason, ts)
            kept += 1
    return raw, kept


def load_schema_b(path, since_ts, agg):
    """Multi-engine schema with 'engine' column."""
    if not os.path.exists(path):
        return 0, 0
    raw = 0
    kept = 0
    with open(path, newline="") as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            raw += 1
            ts = safe_int(row.get("entryTs"))
            if ts < since_ts:
                continue
            engine = (row.get("engine") or "?").strip() or "?"
            pnl = safe_float(row.get("pnl"))
            exit_reason = (row.get("exitReason") or "").strip()
            agg[engine].add(pnl, pnl, exit_reason, ts)
            kept += 1
    return raw, kept


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def fmt_money(x):
    sign = "-" if x < 0 else " "
    return f"{sign}${abs(x):>10.2f}"


def render(agg, since_str, file_summary, out_stream):
    p = lambda s="": print(s, file=out_stream)

    p("=" * 96)
    p(f"POST-REGIME BASELINE   (since {since_str} UTC)")
    p(f"Generated 2026-04-29   --   /Users/jo/omega_repo/backtest/post_regime_baseline.py")
    p("=" * 96)
    p()
    p("Files scanned:")
    for f, raw, kept, src in file_summary:
        p(f"  {f:<48s} schema={src} rows={raw:>7d} post-cut={kept:>6d}")
    p()

    if not agg:
        p("(no rows post-cut, nothing to report)")
        return

    # Sort engines by absolute trade count desc, so the busy engines appear first
    engines = sorted(agg.values(), key=lambda e: -e.trades)

    # Header
    p(f"{'engine':<24s} {'trades':>6s} {'WR%':>6s} {'gross':>12s} {'net':>12s}"
      f" {'expectancy':>11s} {'maxDD':>10s} {'sharpe':>7s}  {'date_range':<25s}")
    p("-" * 134)

    grand_n   = 0
    grand_g   = 0.0
    grand_n_p = 0.0
    for e in engines:
        a, b = e.date_range()
        p(f"{e.name:<24s} {e.trades:>6d} {e.win_rate():>5.1f}% "
          f"{fmt_money(e.gross):>12s} {fmt_money(e.net):>12s} "
          f"{fmt_money(e.expectancy()):>11s} {fmt_money(-e.max_dd):>10s} "
          f"{e.sharpe():>7.2f}  {a}..{b}")
        grand_n   += e.trades
        grand_g   += e.gross
        grand_n_p += e.net

    p("-" * 134)
    p(f"{'TOTAL':<24s} {grand_n:>6d} {'':>6s} "
      f"{fmt_money(grand_g):>12s} {fmt_money(grand_n_p):>12s}")
    p()

    # Exit reason histograms
    p("Exit reason histograms (top 4 per engine):")
    for e in engines:
        if e.trades == 0:
            continue
        parts = []
        for reason, count in e.top_exits(4):
            pct = 100.0 * count / e.trades
            parts.append(f"{reason}={count}({pct:.0f}%)")
        p(f"  {e.name:<24s} " + "  ".join(parts))
    p()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", default="2025-04-01",
                    help="Start of the calibration window (YYYY-MM-DD UTC). "
                         "Default 2025-04-01, the empirical regime cut.")
    ap.add_argument("--out", default=None,
                    help="If given, also write the report to this path.")
    args = ap.parse_args()

    since_ts = epoch_seconds_for_iso(args.since)
    agg = defaultdict(lambda: EngineStats(""))
    # The defaultdict trick won't work for the name; populate explicitly.
    def get_engine(name):
        if name not in agg or not agg[name].name:
            agg[name] = EngineStats(name)
        return agg[name]

    file_summary = []

    # Schema-A loaders
    for fname, engine_name in SCHEMA_A_FILES:
        get_engine(engine_name)  # ensure exists
        raw, kept = load_schema_a(os.path.join(REPO_ROOT, fname),
                                  engine_name, since_ts, agg)
        file_summary.append((fname, raw, kept, "A"))

    # Schema-B loaders -- engine names auto-discovered from rows
    for path in SCHEMA_B_FILES:
        full = os.path.join(REPO_ROOT, path)
        # We need to ensure rows from this file create EngineStats with name set;
        # patch the agg accessor before loading.
        before = set(agg.keys())
        raw, kept = load_schema_b(full, since_ts, agg)
        new_keys = set(agg.keys()) - before
        for k in new_keys:
            if not agg[k].name:
                agg[k] = EngineStats(k)
        # Also fix any pre-existing key whose .name was blanked by defaultdict
        for k in agg:
            if not agg[k].name:
                agg[k].name = k
        file_summary.append((path, raw, kept, "B"))

    render(agg, args.since, file_summary, sys.stdout)
    if args.out:
        with open(args.out, "w") as f:
            render(agg, args.since, file_summary, f)
        print(f"\n[also written to: {args.out}]")


if __name__ == "__main__":
    main()
