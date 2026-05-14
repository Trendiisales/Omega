#!/usr/bin/env python3
# =============================================================================
# vrev_wf_t1.py -- Tier 1 Phase 3 walk-forward driver for VWR USTEC.F
# =============================================================================
# Splits the input tape into N non-overlapping equal-duration windows, runs
# the VWAPReversionBacktest harness on each at the carry-forward session
# window (default open=12, close=21 from the Phase 2 winner), and aggregates
# per-window and OOS-aggregate metrics. Applies the decision rule from the
# Phase 2 results memo §5.
#
# 2026-05-14: rewritten to stream one window at a time to disk, run the
# harness, and delete the split before producing the next window. The
# initial version held all N split files open concurrently and ran out of
# disk on a 4.4 GB tape (peak ~4.4 GB). The current single-window approach
# caps peak temp disk at ~1/N of the tape (~1.1 GB at N=4).
#
# Default N = 4 windows per scoping memo §5. Mode --mode baseline (S63
# trio zeroed) matches the Phase 1/2 protocol.
#
# Tape format auto-detection mirrors backtest/VWAPReversionBacktest.cpp
# lines 95-201:
#   A: timestamp_ms,bid,ask                       (numeric ms epoch)
#   B: timestamp_ms,bid,ask,vol                   (numeric ms epoch)
#   C: YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol        (Dukascopy ISO date)
#   D: timestamp_ms,open,high,low,close,vol       (OHLCV)
#
# Usage:
#   scripts/vrev_wf_t1.py <tape.csv> [options]
#
# Options:
#   --output-dir DIR        Output directory.
#                           Default: outputs/vrev_t1_p3_<timestamp>/.
#   --open-hour H           Session open UTC hour. Default: 12.
#   --close-hour H          Session close UTC hour. Default: 21.
#   --n-windows N           Number of WF windows. Default: 4.
#   --harness PATH          Path to the harness binary.
#                           Default: ./build/VWAPReversionBacktest
#   --symbol SYM            Symbol to pass to harness. Default: USTEC.F.
#   --avg-pnl-threshold X   Per-window avg_pnl pass threshold. Default: 0.001.
#   --pf-threshold X        Aggregate PF pass threshold. Default: 1.20.
#   --min-free-gb N         Pre-flight disk-free check, GB. Default: 1.5.
#                           Set to 0 to skip the check.
#
# Output structure:
#   <output-dir>/
#     wf_summary.csv        One row per window + an "AGGREGATE" row.
#                           Columns: window, start_iso, end_iso, days,
#                                    trades, wins, win_rate_pct,
#                                    gross_pnl, avg_pnl, sum_pos,
#                                    sum_neg, pf, n_tp_hit, n_sl_hit,
#                                    n_timeout, n_mae_early_exit,
#                                    worst_trade, pass
#     wf_verdict.txt        Plain-text decision-rule verdict.
#     cells/
#       w<N>_report.csv     Harness report CSV per window.
#       w<N>_trades.csv     Harness trades CSV per window.
#       w<N>_stderr.log     Harness stderr per window.
#
# Decision rule (per Phase 2 memo §5 + scoping memo §5):
#   PASS = (windows with avg_pnl >= +0.001) >= 3
#          AND aggregate_pf >= 1.20
#
# A passing PF is computed as sum_positive_gross / abs(sum_negative_gross)
# across all 4 windows pooled. The "cost-pessimistic" framing in the
# scoping memo refers to using the harness's reported per-trade gross_pnl
# directly (no further cost overlay); the harness's entry-time
# ExecutionCostGuard already filters trades that cannot cover costs at TP
# at fire time, so non-viable trades never enter the dataset.
#
# Wall-clock estimate:
#   - tape streamed once + 4 harness runs sequentially
#   - ~30s tape streaming + 4 * ~28s harness = ~3 min total
# =============================================================================

from __future__ import annotations

import argparse
import calendar
import csv
import datetime as _dt
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

# =============================================================================
# Format detection -- mirrors backtest/VWAPReversionBacktest.cpp:99-122
# =============================================================================

FMT_A_TBA   = "A_TBA"
FMT_B_TBAV  = "B_TBAV"
FMT_C_DUKA  = "C_DUKA"
FMT_D_OHLCV = "D_OHLCV"
FMT_UNKNOWN = "UNKNOWN"


def detect_format(line: str) -> str:
    """Return one of FMT_*, mirroring detect_format() in the harness."""
    if not line:
        return FMT_UNKNOWN
    # Dukascopy: starts with "YYYY.MM.DD" (year.month.day with dots)
    if (len(line) >= 10
            and line[0].isdigit() and line[1].isdigit()
            and line[2].isdigit() and line[3].isdigit()
            and line[4] == "."
            and line[5].isdigit() and line[6].isdigit()
            and line[7] == "."):
        return FMT_C_DUKA
    commas = line.count(",")
    if commas == 2:
        return FMT_A_TBA
    if commas == 3:
        return FMT_B_TBAV
    if commas == 5:
        return FMT_D_OHLCV
    return FMT_UNKNOWN


def parse_ts_ms(line: str, fmt: str) -> Optional[int]:
    """
    Parse just the timestamp from a tape line. Returns ms-since-epoch as
    an int, or None if the line cannot be parsed. Mirrors the parsing logic
    in backtest/VWAPReversionBacktest.cpp:126-202 but only extracts the
    timestamp for speed (we do not need bid/ask here).
    """
    if not line:
        return None
    # Skip header-like lines (any letter in the first 12 chars besides
    # Dukascopy's dots/colons)
    if fmt != FMT_C_DUKA:
        for c in line[:12]:
            if c.isalpha():
                return None
        # First field is the integer epoch ms.
        first_comma = line.find(",")
        if first_comma <= 0:
            return None
        try:
            return int(line[:first_comma])
        except ValueError:
            return None
    # FMT_C_DUKA: YYYY.MM.DD,HH:MM:SS.mmm,bid,ask,vol
    if len(line) < 23:
        return None
    try:
        Y = int(line[0:4])
        M = int(line[5:7])
        D = int(line[8:10])
        if line[10] != ",":
            return None
        h = int(line[11:13])
        m = int(line[14:16])
        s = int(line[17:19])
        if line[19] != ".":
            return None
        comma2 = line.find(",", 20)
        if comma2 < 0:
            return None
        ms_str = line[20:comma2]
        ms = int(ms_str) if ms_str else 0
        tm = (Y, M, D, h, m, s, 0, 0, 0)
        t_sec = calendar.timegm(tm)
        return t_sec * 1000 + ms
    except (ValueError, IndexError):
        return None


# =============================================================================
# Tape boundary detection
# =============================================================================

def read_first_data_line(tape: Path) -> Tuple[str, int]:
    """
    Read the first non-empty, non-header data line from `tape`. Returns
    (line, byte_offset_of_line_start). Raises on error.
    """
    with tape.open("rb") as f:
        offset = 0
        for raw in f:
            line = raw.decode("ascii", errors="replace").rstrip("\r\n")
            if not line:
                offset += len(raw)
                continue
            fmt = detect_format(line)
            if fmt == FMT_UNKNOWN:
                offset += len(raw)
                continue
            ts = parse_ts_ms(line, fmt)
            if ts is None:
                offset += len(raw)
                continue
            return line, offset
    raise RuntimeError(f"No parseable data line found in {tape}")


def read_last_data_line(tape: Path, fmt: str, tail_bytes: int = 65536) -> str:
    """
    Read the last parseable data line from `tape`. Seeks to `tail_bytes`
    before EOF (or beginning, whichever is later) and walks forward.
    """
    size = tape.stat().st_size
    start = max(0, size - tail_bytes)
    with tape.open("rb") as f:
        f.seek(start)
        chunk = f.read()
    lines = chunk.decode("ascii", errors="replace").split("\n")
    if start > 0 and len(lines) > 1:
        lines = lines[1:]
    for raw in reversed(lines):
        line = raw.rstrip("\r")
        if not line:
            continue
        ts = parse_ts_ms(line, fmt)
        if ts is not None:
            return line
    raise RuntimeError(
        f"No parseable last-line found in last {tail_bytes} bytes of {tape}"
    )


def fmt_iso_utc(ts_ms: int) -> str:
    """Format a ms-epoch timestamp as ISO 8601 UTC ('YYYY-MM-DDTHH:MM:SSZ')."""
    dt = _dt.datetime.fromtimestamp(ts_ms / 1000.0, tz=_dt.timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def disk_free_bytes(path: Path) -> int:
    """Return free bytes on the filesystem containing `path` (or its parent)."""
    target = path if path.exists() else path.parent
    if not target.exists():
        target = Path(".")
    st = os.statvfs(target)
    return st.f_bavail * st.f_frsize


# =============================================================================
# Harness runner + report parsing
# =============================================================================

def run_harness(harness: Path, tape: Path, report: Path, trades: Path,
                stderr_log: Path, symbol: str, open_h: int, close_h: int) -> None:
    """Run the VWAPReversionBacktest harness on `tape` with the session
    window settings. Raises subprocess.CalledProcessError on non-zero exit."""
    cmd = [
        str(harness),
        str(tape),
        "--symbol", symbol,
        "--mode", "baseline",
        "--quiet",
        "--session-open-hour", str(open_h),
        "--session-close-hour", str(close_h),
        "--report", str(report),
        "--trades", str(trades),
    ]
    with stderr_log.open("wb") as ferr:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=ferr, check=True)


def parse_report_csv(path: Path) -> dict:
    """Read the harness's metric,value CSV and return a dict of values."""
    out: dict = {}
    with path.open() as f:
        reader = csv.reader(f)
        for row in reader:
            if len(row) < 2:
                continue
            if row[0] == "metric" and row[1] == "value":
                continue
            out[row[0]] = row[1]
    return out


def compute_pf_from_trades(trades_path: Path) -> Tuple[float, float, float]:
    """
    Read the harness's trades CSV and compute (sum_positive_gross_pnl,
    sum_negative_gross_pnl, profit_factor). The trades CSV has columns:
      entry_ts_unix,exit_ts_unix,symbol,side,engine,entry,exit,gross_pnl,
      mfe,mae,exit_reason,hold_sec,spread_at_entry,confluence_score

    PF = sum_positive / abs(sum_negative). If sum_negative == 0, returns
    +inf (formatted later as a sentinel). Empty file -> (0.0, 0.0, 0.0).
    """
    sum_pos = 0.0
    sum_neg = 0.0
    with trades_path.open() as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            return 0.0, 0.0, 0.0
        try:
            pnl_idx = header.index("gross_pnl")
        except ValueError:
            raise RuntimeError(
                f"trades CSV {trades_path} missing 'gross_pnl' column; got {header}")
        for row in reader:
            if len(row) <= pnl_idx:
                continue
            try:
                p = float(row[pnl_idx])
            except ValueError:
                continue
            if p > 0:
                sum_pos += p
            elif p < 0:
                sum_neg += p
    if sum_neg == 0:
        pf = float("inf") if sum_pos > 0 else 0.0
    else:
        pf = sum_pos / abs(sum_neg)
    return sum_pos, sum_neg, pf


# =============================================================================
# Summary writing + decision rule
# =============================================================================

def write_summary_csv(summary_path: Path, rows: List[dict],
                       aggregate: dict) -> None:
    """Write the per-window rows + aggregate row to wf_summary.csv."""
    fieldnames = [
        "window", "start_iso", "end_iso", "days",
        "trades", "wins", "win_rate_pct",
        "gross_pnl", "avg_pnl", "sum_pos", "sum_neg", "pf",
        "n_tp_hit", "n_sl_hit", "n_timeout", "n_mae_early_exit",
        "worst_trade", "pass",
    ]
    with summary_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in fieldnames})
        w.writerow({k: aggregate.get(k, "") for k in fieldnames})


def decide(rows: List[dict], aggregate: dict,
           avg_pnl_threshold: float, pf_threshold: float) -> Tuple[bool, str]:
    """Apply the decision rule and return (passed, verdict_text)."""
    n_pass = sum(1 for r in rows if r["pass"] == "yes")
    agg_pf_raw = aggregate.get("pf", "")
    if agg_pf_raw in ("", "inf"):
        agg_pf = float("inf")
    else:
        try:
            agg_pf = float(agg_pf_raw)
        except ValueError:
            agg_pf = 0.0
    pf_ok = agg_pf >= pf_threshold
    n_pass_ok = n_pass >= 3
    overall = pf_ok and n_pass_ok

    lines = []
    lines.append("=" * 72)
    lines.append("  VWR USTEC.F Tier 1 Phase 3 walk-forward verdict")
    lines.append("=" * 72)
    lines.append("")
    lines.append("  Decision rule:")
    lines.append(f"    PASS = (windows with avg_pnl >= +{avg_pnl_threshold:.3f}) >= 3")
    lines.append(f"           AND aggregate PF >= {pf_threshold:.2f}")
    lines.append("")
    lines.append("  Result:")
    lines.append(f"    windows passing avg_pnl threshold: {n_pass} of {len(rows)}")
    lines.append(f"    aggregate PF                      : {agg_pf:.4f}")
    lines.append(f"    avg_pnl threshold met            : {'yes' if n_pass_ok else 'no'}")
    lines.append(f"    PF threshold met                 : {'yes' if pf_ok else 'no'}")
    lines.append("")
    if overall:
        lines.append("  >>> OVERALL: PASS <<<")
        lines.append("")
        lines.append("  Next step: re-enable g_vwap_rev_nq at engine_init.hpp:608")
        lines.append("  with per-instance SESSION_OPEN_HOUR / SESSION_CLOSE_HOUR")
        lines.append("  overrides set to the Phase 2 carry-forward values.")
    else:
        lines.append("  >>> OVERALL: FAIL <<<")
        lines.append("")
        lines.append("  The Phase 2 (open, close) edge does not generalise across")
        lines.append("  the walk-forward windows. Recommend Tier 4 (signal-shape")
        lines.append("  redesign) per scoping memo §4 reasoning. Engine stays")
        lines.append("  disabled (engine_init.hpp:608, .enabled = false).")
    lines.append("")
    return overall, "\n".join(lines)


# =============================================================================
# Main
# =============================================================================

def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(
        description="VWR USTEC.F Tier 1 Phase 3 walk-forward driver",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("tape", type=Path, help="Path to the tape CSV.")
    ap.add_argument("--output-dir", type=Path, default=None,
                    help="Output directory. Default: outputs/vrev_t1_p3_<ts>/")
    ap.add_argument("--open-hour",  type=int, default=12,
                    help="Session open UTC hour. Default: 12 (Phase 2 winner).")
    ap.add_argument("--close-hour", type=int, default=21,
                    help="Session close UTC hour. Default: 21 (Phase 2 winner).")
    ap.add_argument("--n-windows", type=int, default=4,
                    help="Number of walk-forward windows. Default: 4.")
    ap.add_argument("--harness", type=Path,
                    default=Path("./build/VWAPReversionBacktest"),
                    help="Path to harness binary.")
    ap.add_argument("--symbol", default="USTEC.F",
                    help="Symbol passed to harness. Default: USTEC.F.")
    ap.add_argument("--avg-pnl-threshold", type=float, default=0.001,
                    help="Per-window avg_pnl pass threshold. Default: 0.001.")
    ap.add_argument("--pf-threshold", type=float, default=1.20,
                    help="Aggregate PF pass threshold. Default: 1.20.")
    ap.add_argument("--min-free-gb", type=float, default=1.5,
                    help="Pre-flight free-disk check, GB. 0 to skip. Default: 1.5.")
    args = ap.parse_args(argv)

    if not args.tape.is_file():
        print(f"[ERROR] Tape not found: {args.tape}", file=sys.stderr)
        return 1
    if not args.harness.is_file() or not os.access(args.harness, os.X_OK):
        print(f"[ERROR] Harness not found or not executable: {args.harness}",
              file=sys.stderr)
        print("        Build with:", file=sys.stderr)
        print("          cmake --build build --target VWAPReversionBacktest "
              "--config Release -j", file=sys.stderr)
        return 1

    if args.output_dir is None:
        ts = time.strftime("%Y%m%d_%H%M%S")
        args.output_dir = Path("outputs") / f"vrev_t1_p3_{ts}"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    splits_dir = args.output_dir / "splits"
    cells_dir  = args.output_dir / "cells"
    splits_dir.mkdir(parents=True, exist_ok=True)
    cells_dir.mkdir(parents=True, exist_ok=True)

    # ----- Header banner ----------------------------------------------------
    print("=" * 63, file=sys.stderr)
    print("  VWR USTEC.F Tier 1 Phase 3 walk-forward", file=sys.stderr)
    print(f"  Tape       : {args.tape}", file=sys.stderr)
    print(f"  Output     : {args.output_dir}", file=sys.stderr)
    print(f"  Symbol     : {args.symbol}", file=sys.stderr)
    print(f"  Open/Close : {args.open_hour} / {args.close_hour} UTC", file=sys.stderr)
    print(f"  Windows    : {args.n_windows}", file=sys.stderr)
    print(f"  Mode       : --mode baseline (S63 trio = 0.0)", file=sys.stderr)
    print("=" * 63, file=sys.stderr)
    print("", file=sys.stderr)

    # ----- Pre-flight disk-free check ---------------------------------------
    if args.min_free_gb > 0:
        free_b = disk_free_bytes(args.output_dir)
        free_gb = free_b / (1024.0 ** 3)
        tape_gb = args.tape.stat().st_size / (1024.0 ** 3)
        est_window_gb = tape_gb / max(1, args.n_windows)
        print(f"[preflight] tape size            = {tape_gb:.2f} GB", file=sys.stderr)
        print(f"[preflight] estimated peak temp  = {est_window_gb:.2f} GB "
              f"(largest window)", file=sys.stderr)
        print(f"[preflight] free disk            = {free_gb:.2f} GB", file=sys.stderr)
        if free_gb < args.min_free_gb:
            print(f"[ERROR] Free disk {free_gb:.2f} GB below threshold "
                  f"{args.min_free_gb:.2f} GB. Free some disk or pass "
                  f"--min-free-gb 0 to skip this check.", file=sys.stderr)
            return 1
        print("", file=sys.stderr)

    # ----- Detect format + tape boundaries ----------------------------------
    first_line, _ = read_first_data_line(args.tape)
    fmt = detect_format(first_line)
    if fmt == FMT_UNKNOWN:
        print(f"[ERROR] Cannot detect tape format from first line: {first_line[:80]}",
              file=sys.stderr)
        return 1
    start_ms = parse_ts_ms(first_line, fmt)
    last_line = read_last_data_line(args.tape, fmt)
    end_ms   = parse_ts_ms(last_line, fmt)
    if start_ms is None or end_ms is None or end_ms <= start_ms:
        print(f"[ERROR] Bad boundaries: start={start_ms} end={end_ms}", file=sys.stderr)
        return 1

    duration_ms   = end_ms - start_ms
    days          = duration_ms / (1000.0 * 86400.0)
    window_ms     = duration_ms // args.n_windows
    boundaries_ms = [start_ms + i * window_ms for i in range(args.n_windows)]
    boundaries_ms.append(end_ms)

    print(f"[detect] format       = {fmt}", file=sys.stderr)
    print(f"[detect] start_ms     = {start_ms}  ({fmt_iso_utc(start_ms)})",
          file=sys.stderr)
    print(f"[detect] end_ms       = {end_ms}  ({fmt_iso_utc(end_ms)})",
          file=sys.stderr)
    print(f"[detect] duration     = {days:.2f} days", file=sys.stderr)
    print(f"[detect] window_days  = {days/args.n_windows:.2f}", file=sys.stderr)
    for i in range(args.n_windows):
        print(f"[detect] w{i} window   = [{fmt_iso_utc(boundaries_ms[i])},"
              f" {fmt_iso_utc(boundaries_ms[i+1])})", file=sys.stderr)
    print("", file=sys.stderr)

    # ----- Sequential streaming: split, run, parse, delete per window -------
    rows: List[dict] = []
    agg_trades = 0
    agg_wins   = 0
    agg_gross  = 0.0
    agg_pos    = 0.0
    agg_neg    = 0.0
    agg_n_tp   = 0
    agg_n_sl   = 0
    agg_n_to   = 0
    agg_n_mae  = 0

    pending_line: Optional[str] = None
    fin = args.tape.open("r")
    try:
        for i in range(args.n_windows):
            window_t0 = time.time()
            split_path = splits_dir / f"w{i}.csv"
            split_t0 = time.time()
            written = 0
            with split_path.open("w") as fout:
                if pending_line is not None:
                    fout.write(pending_line)
                    pending_line = None
                    written += 1
                # Read ahead until we hit the next boundary (or EOF for the
                # final window). We compare timestamps to boundaries_ms[i+1].
                # Lines failing parse (headers, blanks, malformed) are silently
                # skipped -- the harness also skips them, so the trade-count
                # result is unaffected.
                hi = boundaries_ms[i + 1]
                is_last = (i == args.n_windows - 1)
                for line in fin:
                    stripped = line.rstrip("\r\n")
                    if not stripped:
                        continue
                    ts = parse_ts_ms(stripped, fmt)
                    if ts is None:
                        continue
                    if not is_last and ts >= hi:
                        pending_line = line
                        break
                    fout.write(line)
                    written += 1
            split_t1 = time.time()
            split_dur = split_t1 - split_t0

            report = cells_dir / f"w{i}_report.csv"
            trades = cells_dir / f"w{i}_trades.csv"
            stderr_log = cells_dir / f"w{i}_stderr.log"
            sys.stderr.write(
                f"[w{i}] split={written:>12,} ticks in {split_dur:.1f}s; harness ... ")
            sys.stderr.flush()
            try:
                run_harness(args.harness, split_path, report, trades, stderr_log,
                            args.symbol, args.open_hour, args.close_hour)
            except subprocess.CalledProcessError as e:
                sys.stderr.write(f"FAILED rc={e.returncode}\n")
                sys.stderr.write(f"       see {stderr_log}\n")
                return 1
            rep = parse_report_csv(report)
            sum_pos, sum_neg, pf = compute_pf_from_trades(trades)

            trades_n = int(rep.get("trades", "0"))
            wins     = int(rep.get("wins", "0"))
            win_rate = float(rep.get("win_rate_pct", "0"))
            gross    = float(rep.get("gross_pnl", "0"))
            avg      = float(rep.get("avg_pnl", "0"))
            n_tp     = int(rep.get("n_tp_hit", "0"))
            n_sl     = int(rep.get("n_sl_hit", "0"))
            n_to     = int(rep.get("n_timeout", "0"))
            n_mae    = int(rep.get("n_mae_early_exit", "0"))
            worst    = float(rep.get("worst_trade", "0"))

            passed = avg >= args.avg_pnl_threshold
            rows.append({
                "window": f"w{i}",
                "start_iso": fmt_iso_utc(boundaries_ms[i]),
                "end_iso":   fmt_iso_utc(boundaries_ms[i + 1]),
                "days":      f"{(boundaries_ms[i+1] - boundaries_ms[i]) / (1000.0*86400.0):.2f}",
                "trades":    trades_n,
                "wins":      wins,
                "win_rate_pct": f"{win_rate:.2f}",
                "gross_pnl": f"{gross:.6f}",
                "avg_pnl":   f"{avg:.6f}",
                "sum_pos":   f"{sum_pos:.6f}",
                "sum_neg":   f"{sum_neg:.6f}",
                "pf":        ("inf" if pf == float("inf") else f"{pf:.4f}"),
                "n_tp_hit":  n_tp,
                "n_sl_hit":  n_sl,
                "n_timeout": n_to,
                "n_mae_early_exit": n_mae,
                "worst_trade": f"{worst:.6f}",
                "pass":      "yes" if passed else "no",
            })

            agg_trades += trades_n
            agg_wins   += wins
            agg_gross  += gross
            agg_pos    += sum_pos
            agg_neg    += sum_neg
            agg_n_tp   += n_tp
            agg_n_sl   += n_sl
            agg_n_to   += n_to
            agg_n_mae  += n_mae

            # Delete the split file now that the harness is done with it.
            # The cells/ outputs (report.csv, trades.csv, stderr.log) are
            # small and stay for forensics.
            try:
                split_path.unlink()
            except OSError as e:
                sys.stderr.write(f"\n[warn] could not delete {split_path}: {e}\n")

            window_t1 = time.time()
            window_dur = window_t1 - window_t0
            pf_str = "inf" if pf == float("inf") else f"{pf:.3f}"
            sys.stderr.write(
                f"trades={trades_n:>6}  avg={avg:>+10.6f}  pf={pf_str:>6}  "
                f"({window_dur:.0f}s) {'PASS' if passed else 'FAIL'}\n")
    finally:
        fin.close()

    # ----- Aggregate row ----------------------------------------------------
    if agg_neg == 0:
        agg_pf = float("inf") if agg_pos > 0 else 0.0
    else:
        agg_pf = agg_pos / abs(agg_neg)

    agg_avg      = (agg_gross / agg_trades) if agg_trades > 0 else 0.0
    agg_win_rate = (100.0 * agg_wins / agg_trades) if agg_trades > 0 else 0.0
    agg_passed   = agg_avg >= args.avg_pnl_threshold
    agg_worst    = min((float(r["worst_trade"]) for r in rows), default=0.0)

    aggregate = {
        "window": "AGGREGATE",
        "start_iso": fmt_iso_utc(start_ms),
        "end_iso":   fmt_iso_utc(end_ms),
        "days":      f"{days:.2f}",
        "trades":    agg_trades,
        "wins":      agg_wins,
        "win_rate_pct": f"{agg_win_rate:.2f}",
        "gross_pnl": f"{agg_gross:.6f}",
        "avg_pnl":   f"{agg_avg:.6f}",
        "sum_pos":   f"{agg_pos:.6f}",
        "sum_neg":   f"{agg_neg:.6f}",
        "pf":        ("inf" if agg_pf == float("inf") else f"{agg_pf:.4f}"),
        "n_tp_hit":  agg_n_tp,
        "n_sl_hit":  agg_n_sl,
        "n_timeout": agg_n_to,
        "n_mae_early_exit": agg_n_mae,
        "worst_trade": f"{agg_worst:.6f}",
        "pass":      "yes" if agg_passed else "no",
    }

    summary_path = args.output_dir / "wf_summary.csv"
    write_summary_csv(summary_path, rows, aggregate)
    print("", file=sys.stderr)
    print(f"[summary] {summary_path}", file=sys.stderr)

    # ----- Decision rule + verdict -----------------------------------------
    overall, verdict_text = decide(rows, aggregate,
                                   args.avg_pnl_threshold, args.pf_threshold)
    verdict_path = args.output_dir / "wf_verdict.txt"
    verdict_path.write_text(verdict_text + "\n")
    print("", file=sys.stderr)
    print(verdict_text, file=sys.stderr)
    print(f"[verdict] {verdict_path}", file=sys.stderr)

    # ----- Clean up the now-empty splits/ dir -------------------------------
    try:
        # If everything went well, splits_dir is empty. rmdir is safer than
        # rmtree -- it errors if any unexpected file is left behind, surfacing
        # a corruption-style bug rather than silently deleting it.
        splits_dir.rmdir()
    except OSError:
        # Either non-empty (unexpected) or already gone. Either way, leave
        # whatever is there for forensics.
        pass

    return 0 if overall else 2  # exit 2 on FAIL so CI can distinguish from error


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
