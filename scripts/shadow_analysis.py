#!/usr/bin/env python3
"""
shadow_analysis.py
==================
Analyses Omega shadow CSV to produce per-engine, per-symbol, and combined
performance statistics: win rate, expectancy, Sharpe ratio, profit factor,
max drawdown, and trade count.

Shadow CSV columns (written by write_shadow_csv in main.cpp):
  entryTs, symbol, side, engine, entryPrice, exitPrice,
  pnl, mfe, mae, hold_sec, exitReason, spreadAtEntry, latencyMs, regime

Usage:
  python shadow_analysis.py                         # auto-finds logs/shadow/omega_shadow.csv
  python shadow_analysis.py path/to/omega_shadow.csv
  python shadow_analysis.py --days 14               # last N calendar days only
  python shadow_analysis.py --min-trades 5          # suppress rows with < N trades
  python shadow_analysis.py --csv                   # also write shadow_report.csv
  python shadow_analysis.py --plot                  # show equity curves (requires matplotlib)
"""

import sys
import os
import csv
import math
import argparse
import datetime
from collections import defaultdict
from typing import List, Dict, Tuple, Optional

# ─────────────────────────────────────────────────────────────────────────────
# Data model
# ─────────────────────────────────────────────────────────────────────────────

COLUMNS = [
    "entryTs", "symbol", "side", "engine", "entryPrice", "exitPrice",
    "pnl", "mfe", "mae", "hold_sec", "exitReason", "spreadAtEntry",
    "latencyMs", "regime"
]
# Column 6 ("pnl") is net_pnl (after slippage+commission) as of audit-2 fix 10.
# The column name in the CSV header is "pnl" for backward compatibility,
# but the value written is tr.net_pnl. All stats here are net.

class Trade:
    __slots__ = COLUMNS

    def __init__(self, row: dict):
        self.entryTs      = int(row.get("entryTs", 0) or 0)
        self.symbol       = row.get("symbol", "UNKNOWN").strip()
        self.side         = row.get("side", "").strip().upper()
        self.engine       = row.get("engine", "UNKNOWN").strip()
        self.entryPrice   = float(row.get("entryPrice", 0) or 0)
        self.exitPrice    = float(row.get("exitPrice", 0) or 0)
        self.pnl          = float(row.get("pnl", 0) or 0)  # net after costs (was gross pre-audit2)
        self.mfe          = float(row.get("mfe", 0) or 0)
        self.mae          = float(row.get("mae", 0) or 0)
        self.hold_sec     = float(row.get("hold_sec", 0) or 0)
        self.exitReason   = row.get("exitReason", "").strip()
        self.spreadAtEntry= float(row.get("spreadAtEntry", 0) or 0)
        self.latencyMs    = float(row.get("latencyMs", 0) or 0)
        self.regime       = row.get("regime", "").strip()


# ─────────────────────────────────────────────────────────────────────────────
# Statistics
# ─────────────────────────────────────────────────────────────────────────────

def stats(trades: List[Trade]) -> dict:
    if not trades:
        return {}

    pnls = [t.pnl for t in trades]
    n    = len(pnls)
    wins = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p <= 0]

    wr         = len(wins) / n
    avg_win    = sum(wins)   / len(wins)   if wins   else 0.0
    avg_loss   = abs(sum(losses) / len(losses)) if losses else 0.0
    expectancy = wr * avg_win - (1 - wr) * avg_loss

    pf = (sum(wins) / abs(sum(losses))) if losses and sum(losses) != 0 else float('inf')

    # Sharpe — annualised using average hold time
    mean_pnl = sum(pnls) / n
    var      = sum((p - mean_pnl) ** 2 for p in pnls) / n if n > 1 else 0.0
    stdev    = math.sqrt(var)
    avg_hold = sum(t.hold_sec for t in trades) / n
    avg_hold = max(avg_hold, 1.0)
    trades_per_year = (250 * 8 * 3600) / avg_hold
    sharpe   = (mean_pnl / stdev * math.sqrt(trades_per_year)) if stdev > 1e-9 else 0.0

    # Drawdown on cumulative equity curve
    cum = 0.0
    peak = 0.0
    max_dd = 0.0
    for p in pnls:
        cum += p
        if cum > peak:
            peak = cum
        dd = peak - cum
        if dd > max_dd:
            max_dd = dd

    total_pnl = sum(pnls)
    avg_hold_str = _fmt_hold(avg_hold)

    return {
        "n":           n,
        "wr":          wr,
        "total_pnl":   total_pnl,
        "avg_win":     avg_win,
        "avg_loss":    avg_loss,
        "expectancy":  expectancy,
        "profit_factor": pf,
        "sharpe":      sharpe,
        "max_dd":      max_dd,
        "avg_hold":    avg_hold_str,
        "n_wins":      len(wins),
        "n_losses":    len(losses),
    }


def _fmt_hold(sec: float) -> str:
    sec = int(sec)
    if sec < 60:
        return f"{sec}s"
    if sec < 3600:
        return f"{sec // 60}m{sec % 60:02d}s"
    return f"{sec // 3600}h{(sec % 3600) // 60:02d}m"


# ─────────────────────────────────────────────────────────────────────────────
# Report rendering
# ─────────────────────────────────────────────────────────────────────────────

HEADER_FMT = (
    f"{'GROUP':<28} {'N':>5} {'WR':>6} {'EXP':>8} {'SHARPE':>7} "
    f"{'PF':>6} {'TOTAL$':>9} {'MAX_DD':>8} {'AVG_HOLD':>10}"
)
SEP = "-" * len(HEADER_FMT)

def _row(label: str, s: dict, indent: int = 0) -> str:
    if not s:
        return ""
    pf = s['profit_factor']
    pf_str = f"{pf:6.2f}" if pf != float('inf') else "   inf"
    pad = "  " * indent
    wr_color = _color_wr(s['wr'])
    exp_color = _color_val(s['expectancy'])
    sh_color  = _color_val(s['sharpe'])
    return (
        f"{pad}{label:<{28 - 2*indent}} "
        f"{s['n']:>5} "
        f"{wr_color}{s['wr']*100:5.1f}%{_RESET} "
        f"{exp_color}{s['expectancy']:>8.2f}{_RESET} "
        f"{sh_color}{s['sharpe']:>7.2f}{_RESET} "
        f"{pf_str} "
        f"{s['total_pnl']:>9.2f} "
        f"{s['max_dd']:>8.2f} "
        f"{s['avg_hold']:>10}"
    )

# ANSI colour helpers (disabled on Windows if not in a VT100 terminal)
_USE_COLOR = sys.stdout.isatty() and os.name != 'nt'
_RESET = "\033[0m" if _USE_COLOR else ""
_GREEN = "\033[32m" if _USE_COLOR else ""
_AMBER = "\033[33m" if _USE_COLOR else ""
_RED   = "\033[31m" if _USE_COLOR else ""

def _color_wr(wr: float) -> str:
    if not _USE_COLOR: return ""
    if wr >= 0.55: return _GREEN
    if wr >= 0.45: return _AMBER
    return _RED

def _color_val(v: float) -> str:
    if not _USE_COLOR: return ""
    if v > 0:   return _GREEN
    if v > -5:  return _AMBER
    return _RED


def print_report(trades: List[Trade], min_trades: int) -> None:
    print()
    print(f"  OMEGA SHADOW ANALYSIS  ({len(trades)} total trades)")
    print(f"  Generated: {datetime.datetime.utcnow().strftime('%Y-%m-%d %H:%M UTC')}")
    print()

    # ── Overall ──────────────────────────────────────────────────────────────
    s_all = stats(trades)
    print(HEADER_FMT)
    print(SEP)
    print(_row("ALL TRADES", s_all))
    print(SEP)
    print()

    # ── Per engine ───────────────────────────────────────────────────────────
    by_engine: Dict[str, List[Trade]] = defaultdict(list)
    for t in trades:
        by_engine[t.engine].append(t)

    print("  BY ENGINE")
    print(HEADER_FMT)
    print(SEP)
    for eng in sorted(by_engine):
        ts = by_engine[eng]
        if len(ts) < min_trades:
            continue
        print(_row(eng, stats(ts)))
    print(SEP)
    print()

    # ── Per symbol ───────────────────────────────────────────────────────────
    by_sym: Dict[str, List[Trade]] = defaultdict(list)
    for t in trades:
        by_sym[t.symbol].append(t)

    print("  BY SYMBOL")
    print(HEADER_FMT)
    print(SEP)
    for sym in sorted(by_sym):
        ts = by_sym[sym]
        if len(ts) < min_trades:
            continue
        print(_row(sym, stats(ts)))
    print(SEP)
    print()

    # ── Per engine × symbol ──────────────────────────────────────────────────
    by_eng_sym: Dict[Tuple[str, str], List[Trade]] = defaultdict(list)
    for t in trades:
        by_eng_sym[(t.engine, t.symbol)].append(t)

    print("  BY ENGINE × SYMBOL")
    print(HEADER_FMT)
    print(SEP)
    current_eng = None
    for (eng, sym) in sorted(by_eng_sym):
        ts = by_eng_sym[(eng, sym)]
        if len(ts) < min_trades:
            continue
        if eng != current_eng:
            # Engine sub-header
            eng_trades = by_engine[eng]
            if len(eng_trades) >= min_trades:
                print(_row(f"[{eng}]", stats(eng_trades), indent=0))
            current_eng = eng
        print(_row(sym, stats(ts), indent=1))
    print(SEP)
    print()

    # ── Exit reason breakdown ─────────────────────────────────────────────────
    by_exit: Dict[str, List[Trade]] = defaultdict(list)
    for t in trades:
        by_exit[t.exitReason].append(t)

    print("  BY EXIT REASON")
    print(HEADER_FMT)
    print(SEP)
    for reason in sorted(by_exit, key=lambda r: -len(by_exit[r])):
        ts = by_exit[reason]
        if len(ts) < min_trades:
            continue
        print(_row(reason or "(unknown)", stats(ts)))
    print(SEP)
    print()

    # ── Regime breakdown ─────────────────────────────────────────────────────
    by_regime: Dict[str, List[Trade]] = defaultdict(list)
    for t in trades:
        by_regime[t.regime or "UNKNOWN"].append(t)

    if len(by_regime) > 1:
        print("  BY MACRO REGIME")
        print(HEADER_FMT)
        print(SEP)
        for reg in sorted(by_regime, key=lambda r: -len(by_regime[r])):
            ts = by_regime[reg]
            if len(ts) < min_trades:
                continue
            print(_row(reg, stats(ts)))
        print(SEP)
        print()

    # ── Daily PnL summary ────────────────────────────────────────────────────
    by_day: Dict[str, float] = defaultdict(float)
    for t in trades:
        day = datetime.datetime.utcfromtimestamp(t.entryTs).strftime("%Y-%m-%d")
        by_day[day] += t.pnl

    if by_day:
        print("  DAILY SESSION RESULTS")
        print(f"  {'DATE':<12} {'PNL':>10}  {'RESULT'}")
        print(f"  {'-'*32}")
        consec_loss = 0
        for day in sorted(by_day):
            pnl = by_day[day]
            result = "WIN" if pnl > 0 else "LOSS"
            if pnl <= 0:
                consec_loss += 1
            else:
                consec_loss = 0
            streak_warn = f"  ⚠ {consec_loss} consec loss" if consec_loss >= 2 else ""
            color = _GREEN if pnl > 0 else _RED
            print(f"  {day:<12} {color}{pnl:>10.2f}{_RESET}  {result}{streak_warn}")
        print()


# ─────────────────────────────────────────────────────────────────────────────
# CSV export
# ─────────────────────────────────────────────────────────────────────────────

def write_csv_report(trades: List[Trade], path: str, min_trades: int) -> None:
    rows = []

    def add(group_type, group_name, ts):
        if len(ts) < min_trades:
            return
        s = stats(ts)
        rows.append({
            "group_type":    group_type,
            "group_name":    group_name,
            "n":             s["n"],
            "win_rate":      round(s["wr"], 4),
            "expectancy":    round(s["expectancy"], 2),
            "sharpe":        round(s["sharpe"], 3),
            "profit_factor": round(s["profit_factor"], 2) if s["profit_factor"] != float('inf') else 9999,
            "total_pnl":     round(s["total_pnl"], 2),
            "max_dd":        round(s["max_dd"], 2),
            "avg_hold":      s["avg_hold"],
        })

    add("ALL", "ALL", trades)

    by_engine: Dict[str, List[Trade]] = defaultdict(list)
    for t in trades: by_engine[t.engine].append(t)
    for k, v in by_engine.items(): add("ENGINE", k, v)

    by_sym: Dict[str, List[Trade]] = defaultdict(list)
    for t in trades: by_sym[t.symbol].append(t)
    for k, v in by_sym.items(): add("SYMBOL", k, v)

    by_eng_sym: Dict[Tuple[str, str], List[Trade]] = defaultdict(list)
    for t in trades: by_eng_sym[(t.engine, t.symbol)].append(t)
    for (eng, sym), v in sorted(by_eng_sym.items()): add("ENGINE_SYMBOL", f"{eng}:{sym}", v)

    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else [])
        writer.writeheader()
        writer.writerows(rows)
    print(f"  CSV report written to: {path}")


# ─────────────────────────────────────────────────────────────────────────────
# Equity curve plot
# ─────────────────────────────────────────────────────────────────────────────

def plot_equity(trades: List[Trade]) -> None:
    try:
        import matplotlib.pyplot as plt
        import matplotlib.dates as mdates
    except ImportError:
        print("  matplotlib not available — skipping plot")
        return

    # Sort by entry timestamp
    trades = sorted(trades, key=lambda t: t.entryTs)

    # Cumulative equity
    ts    = [datetime.datetime.utcfromtimestamp(t.entryTs) for t in trades]
    pnls  = [t.pnl for t in trades]
    cumulative = []
    cum = 0.0
    for p in pnls:
        cum += p
        cumulative.append(cum)

    # Per-engine equity
    engines = sorted(set(t.engine for t in trades))
    eng_pnl: Dict[str, List[Tuple[datetime.datetime, float]]] = defaultdict(list)
    eng_cum: Dict[str, float] = defaultdict(float)
    for t in sorted(trades, key=lambda x: x.entryTs):
        eng_cum[t.engine] += t.pnl
        eng_pnl[t.engine].append(
            (datetime.datetime.utcfromtimestamp(t.entryTs), eng_cum[t.engine])
        )

    fig, axes = plt.subplots(2, 1, figsize=(14, 8), sharex=False)
    fig.suptitle("Omega Shadow Mode — Equity Analysis", fontsize=13, fontweight='bold')

    # Top: combined equity
    ax0 = axes[0]
    ax0.plot(ts, cumulative, color='#00d4aa', linewidth=1.5, label='Combined')
    ax0.axhline(0, color='rgba(255,255,255,0.15)', linewidth=0.5)
    ax0.fill_between(ts, cumulative, 0,
                      where=[c >= 0 for c in cumulative], alpha=0.15, color='#00d4aa')
    ax0.fill_between(ts, cumulative, 0,
                      where=[c < 0 for c in cumulative], alpha=0.15, color='#ff4444')
    ax0.set_title("Combined Equity Curve", fontsize=10)
    ax0.set_ylabel("USD PnL")
    ax0.xaxis.set_major_formatter(mdates.DateFormatter('%m/%d'))
    ax0.grid(True, alpha=0.1)
    ax0.legend(fontsize=8)

    # Bottom: per-engine
    ax1 = axes[1]
    cmap = plt.cm.get_cmap('tab10', len(engines))
    for i, eng in enumerate(engines):
        pts = eng_pnl[eng]
        if len(pts) < 2:
            continue
        xs, ys = zip(*pts)
        ax1.plot(xs, ys, linewidth=1.2, label=eng, color=cmap(i))
    ax1.axhline(0, color='rgba(255,255,255,0.15)', linewidth=0.5)
    ax1.set_title("Per-Engine Equity", fontsize=10)
    ax1.set_ylabel("USD PnL")
    ax1.xaxis.set_major_formatter(mdates.DateFormatter('%m/%d'))
    ax1.grid(True, alpha=0.1)
    ax1.legend(fontsize=7, ncol=3)

    plt.tight_layout()
    plt.show()


# ─────────────────────────────────────────────────────────────────────────────
# CSV loader
# ─────────────────────────────────────────────────────────────────────────────

def load_csv(path: str, days_filter: Optional[int]) -> List[Trade]:
    trades = []
    cutoff_ts = 0
    if days_filter:
        cutoff_ts = int(
            (datetime.datetime.utcnow() - datetime.timedelta(days=days_filter)).timestamp()
        )

    with open(path, newline="", encoding="utf-8") as f:
        # Try to auto-detect header vs headerless
        sample = f.read(512)
        f.seek(0)
        has_header = sample.startswith("entryTs") or sample.startswith('"entryTs')

        if has_header:
            reader = csv.DictReader(f)
        else:
            reader = csv.DictReader(f, fieldnames=COLUMNS)

        for row in reader:
            try:
                t = Trade(row)
                if t.symbol == "UNKNOWN" or t.entryTs == 0:
                    continue
                if cutoff_ts and t.entryTs < cutoff_ts:
                    continue
                trades.append(t)
            except (ValueError, KeyError):
                continue

    return trades


def find_default_csv() -> Optional[str]:
    candidates = [
        "logs/shadow/omega_shadow.csv",
        "C:/Omega/logs/shadow/omega_shadow.csv",
        os.path.join(os.path.dirname(__file__), "..", "logs", "shadow", "omega_shadow.csv"),
    ]
    for p in candidates:
        if os.path.isfile(p):
            return os.path.abspath(p)
    return None


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Omega shadow mode performance analyser")
    parser.add_argument("csv_path", nargs="?", default=None,
                        help="Path to omega_shadow.csv (auto-detected if omitted)")
    parser.add_argument("--days", type=int, default=None,
                        help="Only include trades from the last N calendar days")
    parser.add_argument("--min-trades", type=int, default=5,
                        help="Suppress rows with fewer than N trades (default: 5)")
    parser.add_argument("--csv", action="store_true",
                        help="Write shadow_report.csv alongside this script")
    parser.add_argument("--plot", action="store_true",
                        help="Show equity curve plots (requires matplotlib)")
    args = parser.parse_args()

    csv_path = args.csv_path
    if not csv_path:
        csv_path = find_default_csv()
    if not csv_path or not os.path.isfile(csv_path):
        print(f"ERROR: shadow CSV not found. Pass path as argument or run from C:\\Omega.")
        sys.exit(1)

    print(f"\n  Loading: {csv_path}")
    trades = load_csv(csv_path, args.days)

    if not trades:
        print("  No trades found (check date filter or CSV format).")
        sys.exit(0)

    if args.days:
        print(f"  Filter: last {args.days} days  ({len(trades)} trades)")

    print_report(trades, args.min_trades)

    if args.csv:
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shadow_report.csv")
        write_csv_report(trades, out, args.min_trades)

    if args.plot:
        plot_equity(trades)


if __name__ == "__main__":
    main()
