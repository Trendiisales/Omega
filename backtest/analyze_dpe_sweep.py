#!/usr/bin/env python3
"""
backtest/analyze_dpe_sweep.py

Reduces the output of run_dpe_sweep.ps1 into a ranked leaderboard with
out-of-sample validation.

Reads:
    <sweep_dir>/summary.csv                       -- whole-period metrics per cell
    <sweep_dir>/trades/i{imb}_p{persist}_s{session}.csv -- per-trade rows per cell

Produces:
    <sweep_dir>/leaderboard_whole.csv   -- cells ranked by whole-period expectancy
    <sweep_dir>/leaderboard_oos.csv     -- train/test split (9-15 train, 16-22 test)
    <sweep_dir>/report.md               -- human-readable writeup

Usage:
    python3 analyze_dpe_sweep.py C:/Omega/backtest/dpe_sweep
    python3 analyze_dpe_sweep.py C:/Omega/backtest/dpe_sweep --train-end 2026-04-15

No side effects on input files. Pure reducer.

Session 15 - walk-forward validation of DomPersist signal quality per
DomPersist_entry_audit_2026-04-23.md recommendation 5b.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Tuple


# -------- Data structures ---------------------------------------------------

@dataclass
class CellSummary:
    imb_thresh: float
    persist_ticks: int
    session_filter: int
    trades: int
    wins: int
    wr_pct: float
    pnl_usd: float
    maxdd_usd: float
    expectancy_usd: float
    status: str

    @property
    def tag(self) -> str:
        return f"i{self.imb_thresh:.2f}_p{self.persist_ticks}_s{self.session_filter}"


@dataclass
class TradeRow:
    ts_entry_ms: int
    ts_exit_ms: int
    side: str
    pnl_usd: float
    mfe_pts: float
    held_s: int
    reason: str


@dataclass
class SplitMetrics:
    trades: int = 0
    wins: int = 0
    pnl_usd: float = 0.0
    maxdd_usd: float = 0.0
    expectancy_usd: float = 0.0
    wr_pct: float = 0.0
    sharpe_like: float = 0.0  # mean / stdev of trade pnl (unitless)

    @classmethod
    def from_trades(cls, trades: List[TradeRow]) -> "SplitMetrics":
        m = cls()
        m.trades = len(trades)
        if not trades:
            return m
        m.wins = sum(1 for t in trades if t.pnl_usd > 0)
        m.pnl_usd = sum(t.pnl_usd for t in trades)
        m.wr_pct = 100.0 * m.wins / m.trades
        m.expectancy_usd = m.pnl_usd / m.trades

        # Rolling peak->trough drawdown
        equity = 0.0
        peak = 0.0
        max_dd = 0.0
        for t in trades:
            equity += t.pnl_usd
            if equity > peak:
                peak = equity
            dd = peak - equity
            if dd > max_dd:
                max_dd = dd
        m.maxdd_usd = max_dd

        # Sharpe-like: mean/stdev of per-trade pnl
        mean = m.expectancy_usd
        if m.trades > 1:
            var = sum((t.pnl_usd - mean) ** 2 for t in trades) / (m.trades - 1)
            sd = math.sqrt(var)
            m.sharpe_like = (mean / sd) if sd > 1e-9 else 0.0

        return m


# -------- Readers -----------------------------------------------------------

def read_summary(path: Path) -> List[CellSummary]:
    cells: List[CellSummary] = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                cells.append(CellSummary(
                    imb_thresh=float(row["imb_thresh"]),
                    persist_ticks=int(row["persist_ticks"]),
                    session_filter=int(row["session_filter"]),
                    trades=int(row["trades"]),
                    wins=int(row["wins"]),
                    wr_pct=float(row["wr_pct"]),
                    pnl_usd=float(row["pnl_usd"]),
                    maxdd_usd=float(row["maxdd_usd"]),
                    expectancy_usd=float(row["expectancy_usd"]),
                    status=row["status"],
                ))
            except (KeyError, ValueError) as e:
                print(f"WARN: skipping bad summary row: {row} ({e})", file=sys.stderr)
    return cells


def read_trades(path: Path) -> List[TradeRow]:
    rows: List[TradeRow] = []
    if not path.exists():
        return rows
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                rows.append(TradeRow(
                    ts_entry_ms=int(r["ts_entry_ms"]),
                    ts_exit_ms=int(r["ts_exit_ms"]),
                    side=r["side"],
                    pnl_usd=float(r["pnl_usd"]),
                    mfe_pts=float(r["mfe_pts"]),
                    held_s=int(r["held_s"]),
                    reason=r["reason"],
                ))
            except (KeyError, ValueError) as e:
                print(f"WARN: bad trade row in {path.name}: {e}", file=sys.stderr)
    return rows


# -------- Splitting ---------------------------------------------------------

def split_trades(trades: List[TradeRow], train_end_ms: int
                 ) -> Tuple[List[TradeRow], List[TradeRow]]:
    """Split by entry timestamp. Anything entered <= train_end_ms is train."""
    train = [t for t in trades if t.ts_entry_ms <= train_end_ms]
    test  = [t for t in trades if t.ts_entry_ms >  train_end_ms]
    return train, test


# -------- Report writers ----------------------------------------------------

def write_whole_leaderboard(cells: List[CellSummary], out_path: Path) -> None:
    ok_cells = [c for c in cells if c.status == "OK"]
    # Rank by expectancy (primary), then by sample size (tiebreak)
    ok_cells.sort(key=lambda c: (-c.expectancy_usd, -c.trades))
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["rank", "tag", "imb", "persist", "session",
                    "trades", "wr_pct", "pnl_usd", "maxdd_usd",
                    "expectancy_usd"])
        for rank, c in enumerate(ok_cells, 1):
            w.writerow([rank, c.tag, f"{c.imb_thresh:.2f}", c.persist_ticks,
                        c.session_filter, c.trades, f"{c.wr_pct:.2f}",
                        f"{c.pnl_usd:.2f}", f"{c.maxdd_usd:.2f}",
                        f"{c.expectancy_usd:.2f}"])


def write_oos_leaderboard(cells: List[CellSummary], trades_dir: Path,
                          train_end_ms: int, out_path: Path
                          ) -> List[Dict[str, object]]:
    """For each OK cell, split its per-trade file into train/test and compute
    metrics on each side. Rank by test-set expectancy (primary)."""
    rows: List[Dict[str, object]] = []
    for c in cells:
        if c.status != "OK":
            continue
        tp = trades_dir / f"{c.tag}.csv"
        trades = read_trades(tp)
        train, test = split_trades(trades, train_end_ms)
        m_train = SplitMetrics.from_trades(train)
        m_test  = SplitMetrics.from_trades(test)
        rows.append({
            "tag": c.tag,
            "imb": c.imb_thresh,
            "persist": c.persist_ticks,
            "session": c.session_filter,
            "train_trades": m_train.trades,
            "train_wr_pct": m_train.wr_pct,
            "train_pnl_usd": m_train.pnl_usd,
            "train_exp_usd": m_train.expectancy_usd,
            "test_trades": m_test.trades,
            "test_wr_pct": m_test.wr_pct,
            "test_pnl_usd": m_test.pnl_usd,
            "test_exp_usd": m_test.expectancy_usd,
            "test_maxdd_usd": m_test.maxdd_usd,
            "test_sharpe_like": m_test.sharpe_like,
        })

    # Rank by test expectancy, tiebreak by test sample size
    rows.sort(key=lambda r: (-float(r["test_exp_usd"]), -int(r["test_trades"])))

    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["rank", "tag", "imb", "persist", "session",
                    "train_trades", "train_wr_pct", "train_pnl_usd", "train_exp_usd",
                    "test_trades", "test_wr_pct", "test_pnl_usd", "test_exp_usd",
                    "test_maxdd_usd", "test_sharpe_like"])
        for rank, r in enumerate(rows, 1):
            w.writerow([rank, r["tag"], f"{r['imb']:.2f}", r["persist"],
                        r["session"], r["train_trades"],
                        f"{r['train_wr_pct']:.2f}", f"{r['train_pnl_usd']:.2f}",
                        f"{r['train_exp_usd']:.2f}", r["test_trades"],
                        f"{r['test_wr_pct']:.2f}", f"{r['test_pnl_usd']:.2f}",
                        f"{r['test_exp_usd']:.2f}", f"{r['test_maxdd_usd']:.2f}",
                        f"{r['test_sharpe_like']:.3f}"])
    return rows


def write_report(cells: List[CellSummary], oos_rows: List[Dict[str, object]],
                 train_end: datetime, sweep_dir: Path, out_path: Path) -> None:
    ok_cells = [c for c in cells if c.status == "OK"]
    fail_cells = [c for c in cells if c.status != "OK"]

    # Session/persist/threshold histograms among top-10 by test expectancy
    top10 = oos_rows[:10]
    session_counts = {1: 0, 2: 0, 3: 0, 4: 0}
    persist_counts: Dict[int, int] = {}
    for r in top10:
        session_counts[int(r["session"])] += 1
        persist_counts[int(r["persist"])] = persist_counts.get(int(r["persist"]), 0) + 1

    # Production default performance
    prod_row = next((r for r in oos_rows
                     if abs(float(r["imb"]) - 0.05) < 1e-6
                     and int(r["persist"]) == 5
                     and int(r["session"]) == 3), None)

    with open(out_path, "w") as f:
        f.write("# DomPersist Walk-Forward Sweep Report\n\n")
        f.write(f"Sweep dir: `{sweep_dir}`  \n")
        f.write(f"Train/test boundary: entries <= {train_end.isoformat()} are train\n\n")
        f.write(f"Cells attempted: {len(cells)}  \n")
        f.write(f"Cells OK:        {len(ok_cells)}  \n")
        f.write(f"Cells failed:    {len(fail_cells)}\n\n")

        if fail_cells:
            f.write("## Failed cells\n\n")
            for c in fail_cells:
                f.write(f"- {c.tag}: {c.status}\n")
            f.write("\n")

        f.write("## Production default cell (i=0.05, p=5, session=London+NY)\n\n")
        if prod_row:
            f.write(f"- train: trades={prod_row['train_trades']}, "
                    f"wr={prod_row['train_wr_pct']:.1f}%, "
                    f"exp=${prod_row['train_exp_usd']:.2f}, "
                    f"pnl=${prod_row['train_pnl_usd']:.2f}\n")
            f.write(f"- test:  trades={prod_row['test_trades']}, "
                    f"wr={prod_row['test_wr_pct']:.1f}%, "
                    f"exp=${prod_row['test_exp_usd']:.2f}, "
                    f"pnl=${prod_row['test_pnl_usd']:.2f}\n\n")
        else:
            f.write("Production default cell not in results.\n\n")

        f.write("## Top 10 cells by test-set expectancy\n\n")
        f.write("| rank | tag | train T | train exp | test T | test WR | test exp | test maxDD |\n")
        f.write("|---:|---|---:|---:|---:|---:|---:|---:|\n")
        for rank, r in enumerate(top10, 1):
            f.write(f"| {rank} | {r['tag']} "
                    f"| {r['train_trades']} "
                    f"| ${float(r['train_exp_usd']):.2f} "
                    f"| {r['test_trades']} "
                    f"| {float(r['test_wr_pct']):.1f}% "
                    f"| ${float(r['test_exp_usd']):.2f} "
                    f"| ${float(r['test_maxdd_usd']):.2f} |\n")
        f.write("\n")

        f.write("## Top-10 composition\n\n")
        f.write("Session filter distribution (1=London, 2=NY, 3=London+NY, 4=Overlap):\n\n")
        for s, n in sorted(session_counts.items()):
            if n: f.write(f"- filter={s}: {n}\n")
        f.write("\nPersist-ticks distribution:\n\n")
        for p, n in sorted(persist_counts.items()):
            f.write(f"- persist={p}: {n}\n")
        f.write("\n")

        f.write("## Interpretation gate\n\n")
        f.write("Do NOT ship a winner unless ALL of the following hold:\n\n")
        f.write("1. Test-set trades >= 30 (statistical minimum)\n")
        f.write("2. Test-set expectancy > $0.50 after slippage\n")
        f.write("3. Train expectancy and test expectancy have the same sign\n")
        f.write("4. Test sharpe-like > 0.10\n")
        f.write("5. Test maxDD < 3x whole-period expectancy * trades\n\n")
        f.write("Any cell failing any of these is NOT a winner regardless of rank.\n")


# -------- Main --------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("sweep_dir", help="path written by run_dpe_sweep.ps1")
    ap.add_argument("--train-end", default="2026-04-15",
                    help="train/test boundary (YYYY-MM-DD). Entries on or before this day are train.")
    args = ap.parse_args()

    sweep_dir = Path(args.sweep_dir)
    summary_path = sweep_dir / "summary.csv"
    trades_dir   = sweep_dir / "trades"

    if not summary_path.exists():
        print(f"ERROR: {summary_path} not found", file=sys.stderr)
        return 1
    if not trades_dir.exists():
        print(f"ERROR: {trades_dir} not found", file=sys.stderr)
        return 1

    # Parse train-end day: inclusive, boundary = end of that day UTC
    try:
        te = datetime.strptime(args.train_end, "%Y-%m-%d")
    except ValueError as e:
        print(f"ERROR: --train-end not YYYY-MM-DD: {e}", file=sys.stderr)
        return 1
    train_end = datetime(te.year, te.month, te.day, 23, 59, 59, tzinfo=timezone.utc)
    train_end_ms = int(train_end.timestamp() * 1000)

    cells = read_summary(summary_path)
    if not cells:
        print(f"ERROR: no cells in {summary_path}", file=sys.stderr)
        return 1

    whole_path = sweep_dir / "leaderboard_whole.csv"
    oos_path   = sweep_dir / "leaderboard_oos.csv"
    report_path = sweep_dir / "report.md"

    write_whole_leaderboard(cells, whole_path)
    oos_rows = write_oos_leaderboard(cells, trades_dir, train_end_ms, oos_path)
    write_report(cells, oos_rows, train_end, sweep_dir, report_path)

    print(f"Wrote {whole_path}")
    print(f"Wrote {oos_path}")
    print(f"Wrote {report_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
