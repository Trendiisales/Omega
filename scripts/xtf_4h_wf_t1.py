#!/usr/bin/env python3
# =============================================================================
# xtf_4h_wf_t1.py -- Tier 1 Phase 3 walk-forward driver for XauTrendFollow4h
# =============================================================================
# Created 2026-05-14 (session 2026-05-14 part-Z, follow-up to part-Y handoff
# SESSION_HANDOFF_2026-05-14n.md §"Recommended next-session focus" item 1).
#
# Adapted from scripts/xtf_2h_wf_t1.py via the 5-line substitution that the
# part-W skeleton-state comment in that file anticipated:
#
#   1. Harness invocation passes `--engine 4h` instead of `--engine 2h`.
#      Drives XauTrendFollow4hEngine via the run_4h_engine path that was
#      filled in during part-Z (backtest/XauTrendFollowBacktest.cpp -- see
#      the inline comment block above run_4h_engine for the bar-feed
#      derivation from the 2h dispatch).
#   2. Output directory default uses xtf_4h_t1_p3_<timestamp>/.
#   3. Verdict text references XauTrendFollow4hEngine + --engine 4h.
#   4. Verdict FAIL implication text references the 4h-specific
#      cross-timeframe inference (if 4h fails alongside 2h, the S63-adverse
#      pattern generalises across timeframes; if 4h passes and 2h failed
#      -- a notable timeframe-shape inversion that deserves its own analysis).
#   5. No engine logic changes. No harness changes. No engine_init.hpp
#      changes. Pre-commit pass criterion unchanged from VWR/UTF5m Phase 3.
#
# Decision rule (unchanged from VWR/UTF5m Phase 3):
#   PASS = (windows with avg_pnl >= +0.001) >= 3
#          AND aggregate_pf >= 1.20
#
# Tape format auto-detection: same five formats the XTF harness shares
# with VWR/UTF5m. The XTF harness adds HistData format D for parity but
# the expected XAU tape is Dukascopy C.
#
# Usage:
#   scripts/xtf_4h_wf_t1.py <tape.csv> [options]
#
# Options:
#   --output-dir DIR        Output directory.
#                           Default: outputs/xtf_4h_t1_p3_<timestamp>/.
#   --n-windows N           Number of WF windows. Default: 4.
#   --harness PATH          Path to the harness binary.
#                           Default: ./build/XauTrendFollowBacktest
#   --avg-pnl-threshold X   Per-window avg_pnl pass threshold. Default: 0.001.
#   --pf-threshold X        Aggregate PF pass threshold. Default: 1.20.
#   --min-free-gb N         Pre-flight disk-free check, GB. Default: 0.5.
#                           XAU tick tapes are smaller than NSXUSD;
#                           per-window stream cap ~150 MB. Set 0 to skip.
#
# Output structure:
#   <output-dir>/
#     wf_summary.csv        One row per window + an "AGGREGATE" row.
#                           Columns: window, start_iso, end_iso, days,
#                                    trades, wins, win_rate_pct,
#                                    gross_pnl, avg_pnl, sum_pos,
#                                    sum_neg, pf, n_tp_hit, n_sl_hit,
#                                    n_loss_cut, n_be_cut, n_other,
#                                    worst_trade, pass
#     wf_verdict.txt        Plain-text decision-rule verdict.
#     cells/
#       w<N>_report.csv     Harness report CSV per window.
#       w<N>_trades.csv     Harness trades CSV per window.
#       w<N>_stderr.log     Harness stderr per window.
#
# Wall-clock estimate:
#   - tape streamed once + 4 harness runs sequentially
#   - 4h cadence has fewer bar emissions than 2h (half the bars per unit
#     time) so the harness loop is slightly faster, but tick-replay is
#     unchanged. Expect ~60-70s total on a 30mo Dukascopy XAU tape,
#     comparable to xtf_2h_wf_t1.py wall-clock.
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
# Format detection (mirrors XauTrendFollowBacktest.cpp:115-185)
# =============================================================================

FMT_A_TBA   = "A_TBA"
FMT_B_TBAV  = "B_TBAV"
FMT_C_DUKA  = "C_DUKA"
FMT_D_HIST  = "D_HIST"
FMT_E_OHLCV = "E_OHLCV"
FMT_UNKNOWN = "UNKNOWN"


def detect_format(line: str) -> str:
    if not line:
        return FMT_UNKNOWN
    if (len(line) >= 10
            and line[0].isdigit() and line[1].isdigit()
            and line[2].isdigit() and line[3].isdigit()
            and line[4] == "."
            and line[5].isdigit() and line[6].isdigit()
            and line[7] == "."):
        return FMT_C_DUKA
    if (len(line) >= 19 and line[8] == " "
            and line[0].isdigit() and line[7].isdigit()
            and line[9].isdigit() and line[17].isdigit()):
        return FMT_D_HIST
    commas = line.count(",")
    if commas == 2: return FMT_A_TBA
    if commas == 3: return FMT_B_TBAV
    if commas == 5: return FMT_E_OHLCV
    return FMT_UNKNOWN


def parse_ts_ms(line: str, fmt: str) -> Optional[int]:
    if not line:
        return None
    if fmt == FMT_C_DUKA:
        if len(line) < 23:
            return None
        try:
            Y = int(line[0:4]); M = int(line[5:7]); D = int(line[8:10])
            if line[10] != ",": return None
            h = int(line[11:13]); m = int(line[14:16]); s = int(line[17:19])
            if line[19] != ".": return None
            ms_str = line[20:23]
            ms = int(ms_str) if ms_str.isdigit() else 0
            return calendar.timegm((Y, M, D, h, m, s, 0, 0, 0)) * 1000 + ms
        except (ValueError, IndexError):
            return None
    if fmt == FMT_D_HIST:
        if len(line) < 18:
            return None
        try:
            Y = int(line[0:4]); M = int(line[4:6]); D = int(line[6:8])
            h = int(line[9:11]); m = int(line[11:13]); s = int(line[13:15])
            ms = int(line[15:18])
            return calendar.timegm((Y, M, D, h, m, s, 0, 0, 0)) * 1000 + ms
        except (ValueError, IndexError):
            return None
    # numeric-leading
    for c in line[:12]:
        if c.isalpha():
            return None
    first_comma = line.find(",")
    if first_comma <= 0:
        return None
    try:
        return int(line[:first_comma])
    except ValueError:
        return None


# =============================================================================
# Tape boundary detection -- same logic as xtf_2h_wf_t1.py / utf5m_wf_t1.py
# =============================================================================

def read_first_data_line(tape: Path) -> Tuple[str, int]:
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
    raise RuntimeError(f"No parseable last-line found in last {tail_bytes} bytes of {tape}")


def fmt_iso_utc(ts_ms: int) -> str:
    dt = _dt.datetime.fromtimestamp(ts_ms / 1000.0, tz=_dt.timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


# =============================================================================
# Harness runner
# =============================================================================

def run_harness(harness: Path, tape: Path, report: Path, trades: Path,
                stderr_log: Path) -> None:
    """Run the XauTrendFollowBacktest harness in --engine 4h --mode baseline."""
    cmd = [
        str(harness),
        str(tape),
        "--engine", "4h",
        "--mode", "baseline",
        "--quiet",
        "--report", str(report),
        "--trades", str(trades),
    ]
    with stderr_log.open("wb") as ferr:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=ferr, check=True)


def parse_report_csv(path: Path) -> dict:
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

SUMMARY_FIELDS = [
    "window", "start_iso", "end_iso", "days",
    "trades", "wins", "win_rate_pct",
    "gross_pnl", "avg_pnl", "sum_pos", "sum_neg", "pf",
    "n_tp_hit", "n_sl_hit", "n_loss_cut", "n_be_cut", "n_other",
    "worst_trade", "pass",
]


def write_summary_csv(summary_path: Path, rows: List[dict], aggregate: dict) -> None:
    with summary_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=SUMMARY_FIELDS)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in SUMMARY_FIELDS})
        w.writerow({k: aggregate.get(k, "") for k in SUMMARY_FIELDS})


def decide(rows: List[dict], aggregate: dict,
           avg_pnl_threshold: float, pf_threshold: float) -> Tuple[bool, str]:
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
    lines.append("  XTF-4h XAUUSD Tier 1 Phase 3 walk-forward verdict")
    lines.append("=" * 72)
    lines.append("")
    lines.append("  Configuration:")
    lines.append("    Engine    : XauTrendFollow4hEngine")
    lines.append("    Mode      : --mode baseline (S63 trio = 0.0/0.0/0.0)")
    lines.append("    Harness   : ./build/XauTrendFollowBacktest --engine 4h")
    lines.append("    Windows   : %d" % len(rows))
    lines.append("    avg_pnl threshold : %.4f" % avg_pnl_threshold)
    lines.append("    PF threshold      : %.2f" % pf_threshold)
    lines.append("")
    lines.append("  Per-window:")
    for r in rows:
        lines.append("    Window %s: avg_pnl=%s pf=%s trades=%s pass=%s" % (
            r["window"], r["avg_pnl"], r["pf"], r["trades"], r["pass"]))
    lines.append("")
    lines.append("  Aggregate: avg_pnl=%s pf=%s trades=%s" % (
        aggregate.get("avg_pnl", ""),
        aggregate.get("pf", ""),
        aggregate.get("trades", "")))
    lines.append("")
    lines.append("  Decision: %d/%d windows pass (need >=3), aggregate PF %s %s (need >=%.2f)" % (
        n_pass, len(rows), aggregate.get("pf", ""),
        ">=" if pf_ok else "<", pf_threshold))
    lines.append("")
    lines.append("  VERDICT: %s" % ("PASS" if overall else "FAIL"))
    lines.append("")
    if not overall:
        lines.append("  Implication: baseline XAU 4h trend-follow does NOT clear")
        lines.append("    the WF gate. NOTE: this run used --mode baseline (S63")
        lines.append("    trio = 0.0 / 0.0 / 0.0). The FAIL is evidence of baseline-")
        lines.append("    engine underperformance on this tape, NOT evidence about")
        lines.append("    S63 specifically -- the in-flight protection trio was OFF")
        lines.append("    throughout this run, so it could neither help nor hurt.")
        lines.append("    Engines remain at class-default state-B values (S63 OFF).")
        lines.append("    Next-session investigation order:")
        lines.append("      (a) Reconcile this verdict with the engine-header")
        lines.append("          PROVENANCE block (XauTrendFollow4hEngine.hpp:7-25).")
        lines.append("          PROVENANCE reports each cell net positive across")
        lines.append("          three Duka years (2023-09 .. 2025-09), yet this WF")
        lines.append("          on the substitute tape shows the cells deeply")
        lines.append("          underwater on the overlapping 2024-03 .. 2025-10")
        lines.append("          period. Tape-source, bar-construction, or cost-")
        lines.append("          model divergence is the most likely explanation.")
        lines.append("          S63 evaluation on a losing baseline is structurally")
        lines.append("          moot -- find the divergence first.")
        lines.append("      (b) Cross-reference the xtf_2h_wf_t1.py verdict and")
        lines.append("          scripts/xtf_d1_wf_t1.py (queued). Per-timeframe")
        lines.append("          divergence is regime-sensitivity intelligence; same-")
        lines.append("          tape consistency across timeframes is a signal that")
        lines.append("          the baseline-engine issue is tape-wide rather than")
        lines.append("          R:R-specific.")
        lines.append("      (c) Tier 4 vol-regime gate work (part-Y next-session")
        lines.append("          item #3) is independent of XTF and remains the")
        lines.append("          highest-leverage research track.")
    else:
        lines.append("  Implication: XTF 4h passes Phase 3 walk-forward with S63")
        lines.append("    baseline -- a noteworthy outcome given the VWR/UTF5m prior.")
        lines.append("    Recommend a separate session to (a) tighten the parameters")
        lines.append("    via a Phase 1 sweep over LOSS_CUT/BE_ARM/BE_BUFFER axes in")
        lines.append("    the operational band of same-family XAU engines (0.04-0.05%")
        lines.append("    LOSS_CUT for XAU, per the part-Y cross-engine inspection),")
        lines.append("    then (b) re-run Phase 3 with the tuned cell to confirm.")
        lines.append("    Engine init in engine_init.hpp should NOT be flipped to")
        lines.append("    non-zero values until the tuned cell is confirmed.")
    return overall, "\n".join(lines)


# =============================================================================
# Window slicing -- mirrors xtf_2h_wf_t1.py / utf5m_wf_t1.py
# =============================================================================

def slice_window(tape: Path, fmt: str, start_ts: int, end_ts: int,
                 out_path: Path) -> int:
    """Stream all rows in [start_ts, end_ts) to out_path. Returns row count."""
    count = 0
    with tape.open("rb") as f_in, out_path.open("wb") as f_out:
        for raw in f_in:
            line = raw.decode("ascii", errors="replace").rstrip("\r\n")
            if not line:
                continue
            ts = parse_ts_ms(line, fmt)
            if ts is None:
                continue
            if ts < start_ts:
                continue
            if ts >= end_ts:
                break
            f_out.write(raw)
            count += 1
    return count


# =============================================================================
# Main
# =============================================================================

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tape", help="Tick CSV tape path")
    ap.add_argument("--output-dir", default=None)
    ap.add_argument("--n-windows", type=int, default=4)
    ap.add_argument("--harness", default="./build/XauTrendFollowBacktest")
    ap.add_argument("--avg-pnl-threshold", type=float, default=0.001)
    ap.add_argument("--pf-threshold", type=float, default=1.20)
    ap.add_argument("--min-free-gb", type=float, default=0.5)
    args = ap.parse_args()

    tape = Path(args.tape)
    if not tape.exists():
        print(f"ERROR: tape not found: {tape}", file=sys.stderr)
        return 1

    harness = Path(args.harness)
    if not harness.exists():
        print(f"ERROR: harness not found: {harness} (build it first)", file=sys.stderr)
        return 1

    # Output dir
    if args.output_dir:
        out_dir = Path(args.output_dir)
    else:
        ts = time.strftime("%Y%m%d_%H%M%S")
        out_dir = Path("outputs") / f"xtf_4h_t1_p3_{ts}"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "cells").mkdir(exist_ok=True)

    # Detect format + tape boundaries
    first_line, _ = read_first_data_line(tape)
    fmt = detect_format(first_line)
    first_ts = parse_ts_ms(first_line, fmt)
    last_line = read_last_data_line(tape, fmt)
    last_ts = parse_ts_ms(last_line, fmt)
    if first_ts is None or last_ts is None or last_ts <= first_ts:
        print("ERROR: cannot establish tape window", file=sys.stderr)
        return 1
    total_days = (last_ts - first_ts) / 86400000.0
    print(f"[XTF-WF-4h] tape: {tape}", file=sys.stderr)
    print(f"[XTF-WF-4h] format: {fmt}", file=sys.stderr)
    print(f"[XTF-WF-4h] range: {fmt_iso_utc(first_ts)} .. {fmt_iso_utc(last_ts)} ({total_days:.1f}d)", file=sys.stderr)

    # Window boundaries (evenly split)
    n = args.n_windows
    window_ms = (last_ts - first_ts) // n
    boundaries = [first_ts + i * window_ms for i in range(n + 1)]
    boundaries[-1] = last_ts + 1  # inclusive of last tick

    rows_summary: List[dict] = []
    agg_trades = 0
    agg_wins  = 0
    agg_gross = 0.0
    agg_sum_pos = 0.0
    agg_sum_neg = 0.0
    agg_tp = agg_sl = agg_lc = agg_be = agg_other = 0
    agg_worst = 0.0

    for wi in range(n):
        ws = boundaries[wi]
        we = boundaries[wi + 1]
        days = (we - ws) / 86400000.0
        print(f"[XTF-WF-4h] window {wi+1}/{n}: {fmt_iso_utc(ws)} .. {fmt_iso_utc(we)} ({days:.1f}d)", file=sys.stderr)

        slice_path = out_dir / "cells" / f"w{wi+1}_slice.csv"
        n_rows = slice_window(tape, fmt, ws, we, slice_path)
        if n_rows == 0:
            print(f"  WARN: empty slice", file=sys.stderr)
            rows_summary.append({
                "window": str(wi+1), "start_iso": fmt_iso_utc(ws),
                "end_iso": fmt_iso_utc(we), "days": f"{days:.1f}",
                "trades": "0", "wins": "0", "win_rate_pct": "0",
                "gross_pnl": "0", "avg_pnl": "0", "sum_pos": "0",
                "sum_neg": "0", "pf": "0", "n_tp_hit": "0", "n_sl_hit": "0",
                "n_loss_cut": "0", "n_be_cut": "0", "n_other": "0",
                "worst_trade": "0", "pass": "no",
            })
            slice_path.unlink(missing_ok=True)
            continue

        report = out_dir / "cells" / f"w{wi+1}_report.csv"
        trades = out_dir / "cells" / f"w{wi+1}_trades.csv"
        stderr_log = out_dir / "cells" / f"w{wi+1}_stderr.log"
        try:
            run_harness(harness, slice_path, report, trades, stderr_log)
        except subprocess.CalledProcessError as e:
            print(f"  ERROR: harness failed (rc={e.returncode}); see {stderr_log}", file=sys.stderr)
            slice_path.unlink(missing_ok=True)
            return 1

        # Free the slice immediately
        slice_path.unlink(missing_ok=True)

        rep = parse_report_csv(report)
        n_trades = int(rep.get("n_trades", "0") or "0")
        wins     = int(rep.get("wins", "0") or "0")
        gross    = float(rep.get("gross_pnl", "0") or "0")
        sum_pos  = float(rep.get("sum_pos", "0") or "0")
        sum_neg  = float(rep.get("sum_neg", "0") or "0")
        pf_raw   = rep.get("pf", "0")
        try:
            pf = float(pf_raw)
        except ValueError:
            pf = 0.0
        n_tp = int(rep.get("n_tp_hit", "0") or "0")
        n_sl = int(rep.get("n_sl_hit", "0") or "0")
        n_lc = int(rep.get("n_loss_cut", "0") or "0")
        n_be = int(rep.get("n_be_cut", "0") or "0")
        n_o  = int(rep.get("n_other", "0") or "0")
        worst = float(rep.get("worst_trade", "0") or "0")
        avg_pnl = (gross / n_trades) if n_trades > 0 else 0.0
        win_rate = (100.0 * wins / n_trades) if n_trades > 0 else 0.0
        passed = avg_pnl >= args.avg_pnl_threshold

        rows_summary.append({
            "window": str(wi+1), "start_iso": fmt_iso_utc(ws),
            "end_iso": fmt_iso_utc(we), "days": f"{days:.1f}",
            "trades": str(n_trades), "wins": str(wins),
            "win_rate_pct": f"{win_rate:.2f}",
            "gross_pnl": f"{gross:.4f}",
            "avg_pnl": f"{avg_pnl:.6f}",
            "sum_pos": f"{sum_pos:.4f}", "sum_neg": f"{sum_neg:.4f}",
            "pf": f"{pf:.4f}",
            "n_tp_hit": str(n_tp), "n_sl_hit": str(n_sl),
            "n_loss_cut": str(n_lc), "n_be_cut": str(n_be),
            "n_other": str(n_o),
            "worst_trade": f"{worst:.4f}",
            "pass": "yes" if passed else "no",
        })

        agg_trades += n_trades
        agg_wins   += wins
        agg_gross  += gross
        agg_sum_pos += sum_pos
        agg_sum_neg += sum_neg
        agg_tp += n_tp; agg_sl += n_sl; agg_lc += n_lc; agg_be += n_be; agg_other += n_o
        if worst < agg_worst: agg_worst = worst

    agg_pf = (agg_sum_pos / -agg_sum_neg) if agg_sum_neg < 0 else (float("inf") if agg_sum_pos > 0 else 0.0)
    agg_avg = (agg_gross / agg_trades) if agg_trades > 0 else 0.0
    agg_wr  = (100.0 * agg_wins / agg_trades) if agg_trades > 0 else 0.0
    aggregate = {
        "window": "AGGREGATE", "start_iso": "", "end_iso": "",
        "days": f"{total_days:.1f}",
        "trades": str(agg_trades), "wins": str(agg_wins),
        "win_rate_pct": f"{agg_wr:.2f}",
        "gross_pnl": f"{agg_gross:.4f}",
        "avg_pnl": f"{agg_avg:.6f}",
        "sum_pos": f"{agg_sum_pos:.4f}", "sum_neg": f"{agg_sum_neg:.4f}",
        "pf": f"{agg_pf:.4f}" if agg_pf != float("inf") else "inf",
        "n_tp_hit": str(agg_tp), "n_sl_hit": str(agg_sl),
        "n_loss_cut": str(agg_lc), "n_be_cut": str(agg_be),
        "n_other": str(agg_other),
        "worst_trade": f"{agg_worst:.4f}", "pass": "",
    }

    summary_path = out_dir / "wf_summary.csv"
    write_summary_csv(summary_path, rows_summary, aggregate)
    passed, verdict = decide(rows_summary, aggregate,
                              args.avg_pnl_threshold, args.pf_threshold)
    verdict_path = out_dir / "wf_verdict.txt"
    verdict_path.write_text(verdict + "\n")
    print(verdict)
    print(f"\n[XTF-WF-4h] Outputs: {out_dir}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
