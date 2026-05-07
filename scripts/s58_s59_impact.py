#!/usr/bin/env python3
"""
S58 + S59 hypothetical-impact analyzer for omega_trade_closes_*.csv files.

Reads production trade-close CSVs and reports what S58 (cold-start guard) and
S59 (fill-spread reject) WOULD have prevented if they had been active.

S59 is computed PRECISELY from spread_at_entry, which is recorded per trade.
S58 is APPROXIMATED by detecting service starts via inter-trade time gaps
(>2h gap implies a restart) and counting trades within the cold-start window.
Approximate because the CSV doesn't record m_range_history.size() at fire.

USAGE:
    # On VPS where the CSVs live (Python 3 required):
    python3 s58_s59_impact.py C:\\Omega\\logs\\trades

    # Or against specific files:
    python3 s58_s59_impact.py omega_trade_closes_2026-05-07.csv [more...]

    # On Mac, after pulling CSVs:
    python3 scripts/s58_s59_impact.py ~/omega_csvs/

THRESHOLDS (mirror the engine constants):
    MAX_FILL_SPREAD per symbol (= 2x MAX_SPREAD = 4 pips):
      EURUSD/GBPUSD/AUDUSD/NZDUSD : 0.00040  price units (4 pips)
      USDJPY                       : 0.04000  price units (4 pips of JPY)
      XAUUSD (gold)                : 0.40000  price units (40 cents = 4 ticks)
                                     [GoldMidScalper uses MAX_SPREAD=0.20]
    Cold-start window:
      15 minutes after each inferred service start.
"""

import csv
import glob
import os
import sys
from collections import defaultdict


# -- Thresholds (mirror engine constants) ------------------------------------
MAX_FILL_SPREAD = {
    "EURUSD": 0.00040,
    "GBPUSD": 0.00040,
    "AUDUSD": 0.00040,
    "NZDUSD": 0.00040,
    "USDJPY": 0.04000,
    "XAUUSD": 0.40000,  # 40 cents (gold MAX_SPREAD is 0.20, S59 cap = 2x)
}

COLD_START_WINDOW_SEC = 15 * 60   # 15 min after inferred service start
SERVICE_RESTART_GAP_HOURS = 2.0   # >2h with no trades implies restart


# -- Helpers -----------------------------------------------------------------
def safe_float(v, default=0.0):
    try:
        return float(v) if v not in (None, "") else default
    except (ValueError, TypeError):
        return default


def safe_int(v, default=0):
    try:
        return int(v) if v not in (None, "") else default
    except (ValueError, TypeError):
        return default


def load_trades(paths):
    trades = []
    for path in paths:
        try:
            with open(path, newline="") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    trades.append(row)
        except (OSError, csv.Error) as e:
            print(f"WARNING: failed to read {path}: {e}", file=sys.stderr)
    return trades


def s59_would_reject(row):
    """True if this trade's spread_at_entry exceeded MAX_FILL_SPREAD."""
    sym = row.get("symbol", "")
    threshold = MAX_FILL_SPREAD.get(sym)
    if threshold is None:
        return False
    return safe_float(row.get("spread_at_entry")) > threshold


def detect_inferred_restarts(trades):
    """Return list of unix timestamps that look like service starts.
    A service start = the first trade after a >SERVICE_RESTART_GAP_HOURS gap.
    """
    sorted_t = sorted(trades, key=lambda t: safe_int(t.get("entry_ts_unix")))
    if not sorted_t:
        return []
    starts = [safe_int(sorted_t[0].get("entry_ts_unix"))]
    for i in range(1, len(sorted_t)):
        prev = safe_int(sorted_t[i - 1].get("entry_ts_unix"))
        cur = safe_int(sorted_t[i].get("entry_ts_unix"))
        if cur - prev > SERVICE_RESTART_GAP_HOURS * 3600:
            starts.append(cur)
    return starts


def trade_in_cold_start(row, starts):
    """True if this trade fired within COLD_START_WINDOW_SEC of any inferred
    service start."""
    ts = safe_int(row.get("entry_ts_unix"))
    for s in starts:
        if 0 <= ts - s < COLD_START_WINDOW_SEC:
            return True
    return False


# -- Reporting ---------------------------------------------------------------
def fmt_money(x):
    sign = "+" if x >= 0 else "-"
    return f"{sign}${abs(x):.2f}"


def report_section_per_engine(title, trades, classifier, classifier_name):
    """Print per-engine impact for a given classifier (function row -> bool)."""
    print("=" * 80)
    print(title)
    print("=" * 80)

    by_engine = defaultdict(list)
    for t in trades:
        by_engine[t.get("engine", "unknown")].append(t)

    overall_n = 0
    overall_n_rej = 0
    overall_pnl = 0.0
    overall_rej_pnl = 0.0
    overall_rej_loss_pnl = 0.0
    overall_rej_win_pnl = 0.0

    for engine in sorted(by_engine.keys()):
        ets = by_engine[engine]
        n = len(ets)
        total_pnl = sum(safe_float(t.get("net_pnl")) for t in ets)
        rejected = [t for t in ets if classifier(t)]
        n_rej = len(rejected)
        rej_pnl = sum(safe_float(t.get("net_pnl")) for t in rejected)
        rej_losses = [t for t in rejected if safe_float(t.get("net_pnl")) < 0]
        rej_wins = [t for t in rejected if safe_float(t.get("net_pnl")) > 0]
        sum_rej_losses = sum(safe_float(t.get("net_pnl")) for t in rej_losses)
        sum_rej_wins = sum(safe_float(t.get("net_pnl")) for t in rej_wins)

        pct = (100.0 * n_rej / n) if n else 0.0
        improvement = -rej_pnl
        new_total = total_pnl - rej_pnl

        print(f"\n  --- {engine} ---")
        print(f"    Total trades                  : {n}")
        print(f"    Total net_pnl (actual)        : {fmt_money(total_pnl)}")
        print(f"    {classifier_name} would reject  : {n_rej} ({pct:.1f}%)")
        print(f"      rejected losses             : {len(rej_losses)} / {fmt_money(sum_rej_losses)}")
        print(f"      rejected wins               : {len(rej_wins)} / {fmt_money(sum_rej_wins)}")
        print(f"    Net change if guard active    : {fmt_money(improvement)}")
        print(f"    Counterfactual total          : {fmt_money(new_total)}")

        overall_n += n
        overall_n_rej += n_rej
        overall_pnl += total_pnl
        overall_rej_pnl += rej_pnl
        overall_rej_loss_pnl += sum_rej_losses
        overall_rej_win_pnl += sum_rej_wins

    pct = (100.0 * overall_n_rej / overall_n) if overall_n else 0.0
    print(f"\n  --- OVERALL ---")
    print(f"    Total trades                  : {overall_n}")
    print(f"    Total net_pnl (actual)        : {fmt_money(overall_pnl)}")
    print(f"    {classifier_name} would reject  : {overall_n_rej} ({pct:.1f}%)")
    print(f"      losses removed              : {fmt_money(overall_rej_loss_pnl)}")
    print(f"      wins removed                : {fmt_money(overall_rej_win_pnl)}")
    print(f"    Net change if guard active    : {fmt_money(-overall_rej_pnl)}")
    print(f"    Counterfactual total          : {fmt_money(overall_pnl - overall_rej_pnl)}")
    print()

    return overall_pnl, overall_rej_pnl, overall_n, overall_n_rej


def report_combined(trades, s58_classifier, s59_classifier):
    print("=" * 80)
    print("COMBINED S58 OR S59 IMPACT (deduplicated overlap)")
    print("=" * 80)

    by_engine = defaultdict(list)
    for t in trades:
        by_engine[t.get("engine", "unknown")].append(t)

    overall_n = 0
    overall_n_rej = 0
    overall_pnl = 0.0
    overall_rej_pnl = 0.0

    for engine in sorted(by_engine.keys()):
        ets = by_engine[engine]
        n = len(ets)
        total_pnl = sum(safe_float(t.get("net_pnl")) for t in ets)
        rejected = [t for t in ets if s58_classifier(t) or s59_classifier(t)]
        n_rej = len(rejected)
        rej_pnl = sum(safe_float(t.get("net_pnl")) for t in rejected)

        pct = (100.0 * n_rej / n) if n else 0.0
        improvement = -rej_pnl
        new_total = total_pnl - rej_pnl

        print(f"\n  --- {engine} ---")
        print(f"    Trades blocked by S58 OR S59  : {n_rej} of {n} ({pct:.1f}%)")
        print(f"    Net change                    : {fmt_money(improvement)}")
        print(f"    Counterfactual total          : {fmt_money(new_total)}")

        overall_n += n
        overall_n_rej += n_rej
        overall_pnl += total_pnl
        overall_rej_pnl += rej_pnl

    pct = (100.0 * overall_n_rej / overall_n) if overall_n else 0.0
    print(f"\n  --- OVERALL ---")
    print(f"    Total trades                  : {overall_n}")
    print(f"    Blocked by S58 OR S59         : {overall_n_rej} ({pct:.1f}%)")
    print(f"    Net change                    : {fmt_money(-overall_rej_pnl)}")
    print(f"    Counterfactual total          : {fmt_money(overall_pnl - overall_rej_pnl)}")
    print()


def report_overview(trades, restarts):
    print("=" * 80)
    print("DATA OVERVIEW")
    print("=" * 80)
    print(f"  Trades loaded                 : {len(trades)}")
    print(f"  Inferred service restarts     : {len(restarts)}")
    if trades:
        sorted_t = sorted(trades, key=lambda t: safe_int(t.get("entry_ts_unix")))
        first = sorted_t[0].get("entry_ts_utc", "?")
        last = sorted_t[-1].get("exit_ts_utc", "?")
        print(f"  Date range                    : {first}  ->  {last}")
    syms = sorted({t.get("symbol", "") for t in trades})
    print(f"  Symbols                       : {', '.join(syms)}")
    engines = sorted({t.get("engine", "") for t in trades})
    print(f"  Engines                       : {', '.join(engines)}")
    print()
    print("Thresholds in use:")
    for s in sorted(MAX_FILL_SPREAD.keys()):
        print(f"  MAX_FILL_SPREAD[{s}]      = {MAX_FILL_SPREAD[s]}")
    print(f"  COLD_START_WINDOW_SEC     = {COLD_START_WINDOW_SEC}")
    print(f"  SERVICE_RESTART_GAP_HOURS = {SERVICE_RESTART_GAP_HOURS}")
    print()


# -- Main --------------------------------------------------------------------
def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    # Resolve all input paths to a list of CSV files
    paths = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            paths.extend(sorted(glob.glob(
                os.path.join(arg, "omega_trade_closes_*.csv"))))
        elif os.path.isfile(arg):
            paths.append(arg)
        else:
            matches = sorted(glob.glob(arg))
            if matches:
                paths.extend(matches)
            else:
                print(f"WARNING: no match for {arg}", file=sys.stderr)

    if not paths:
        print("ERROR: no CSV files found.", file=sys.stderr)
        sys.exit(1)

    print(f"Loading {len(paths)} CSV files:")
    for p in paths:
        print(f"  {p}")
    print()

    trades = load_trades(paths)
    if not trades:
        print("ERROR: no trades loaded.", file=sys.stderr)
        sys.exit(1)

    restarts = detect_inferred_restarts(trades)

    report_overview(trades, restarts)

    s59_cls = s59_would_reject
    s58_cls = lambda row: trade_in_cold_start(row, restarts)

    report_section_per_engine(
        "S59 FILL_SPREAD_REJECT IMPACT (PRECISE from spread_at_entry)",
        trades, s59_cls, "S59")

    report_section_per_engine(
        "S58 COLD_START_BLOCK IMPACT (APPROXIMATE: 15-min window after inferred restarts)",
        trades, s58_cls, "S58")

    report_combined(trades, s58_cls, s59_cls)

    print("=" * 80)
    print("CAVEATS")
    print("=" * 80)
    print("  S59 numbers are PRECISE: spread_at_entry is recorded per trade and")
    print("    the engine guard would have rejected before fill triggered.")
    print()
    print("  S58 numbers are APPROXIMATE: cold-start = 'within 15 min of an")
    print("    inferred service start'. The guard's actual logic is")
    print("    'm_range_history.size() < EXPANSION_MIN_HISTORY=5' which is")
    print("    correlated with but not identical to clock time. Real S58 will")
    print("    reject SOME trades the approximation flags AND vice versa.")
    print()
    print("  Counterfactual P&L assumes the rejected trades simply don't happen.")
    print("    In reality, the same compression structure may re-form and fire")
    print("    later in the same session, producing a DIFFERENT trade. So actual")
    print("    counterfactual P&L will differ -- usually the rejected losses")
    print("    are NOT fully recovered by replacement trades.")
    print()


if __name__ == "__main__":
    main()
