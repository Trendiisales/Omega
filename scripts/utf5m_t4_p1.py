#!/usr/bin/env python3
# =============================================================================
# utf5m_t4_p1.py -- UTF5m Tier 4 vol-regime gate Phase 1 sweep driver
# =============================================================================
# Adapted from scripts/utf5m_wf_t1.py (the Phase 3 WF driver, S73). Phase 1
# runs the FULL TAPE under a Cartesian grid of (ATR_LOOKBACK_DAYS,
# VOL_PCT_THRESHOLD) cells, plus a baseline run with the gate OFF, and
# reports best-cell vs baseline.
#
# Background: Phase 3 closure memo §7 item 1
# (outputs/UTF5M_PHASE3_RESULTS_2026-05-14.md) identified w1
# (2024-07-27 → 2025-02-19) as a low-vol regime with hostile Donchian/
# Keltner breakout behaviour: 17.2M ticks vs 25.9-46.1M elsewhere,
# avg_pnl=-0.376. The Tier 4 vol-regime entry gate (S90 engine /
# S91 harness CLI) suppresses entries on bars whose current ATR(14)
# percentile rank falls below VOL_PCT_THRESHOLD within a rolling
# ATR_LOOKBACK_DAYS window. This sweep tests whether any (L, T)
# combination lifts aggregate PF above the 1.20 gate while preserving
# enough trade count to remain economically meaningful.
#
# Sweep grid (2 lookbacks × 7 thresholds = 14 cells):
#   ATR_LOOKBACK_DAYS: 30, 60
#   VOL_PCT_THRESHOLD: 50, 60, 70, 75, 80, 85, 90
#
# Form: entry-blocking (S90 form), NOT S85's gate-S63 form. UTF5m S63
# in-flight management is held OUT-of-band by `--mode baseline`, which
# zeroes the LOSS_CUT_PCT / BE_ARM_PCT / BE_BUFFER_PCT trio at engine
# instance setup. This matches the Phase 3 S73 setup so the comparison
# is apples-to-apples with the closure memo's measured PF=1.1154
# baseline. The Tier 4 gate operates on top of the (S63-zeroed)
# baseline engine.
#
# Decision rule (per part-AB handoff §"2. UTF5m Tier 4 vol-regime gate
# port" step 3):
#   PASS = best_cell_pf >= 1.20                         (absolute gate)
#          AND best_cell_pf >= 1.10 * baseline_pf        (>=10% relative uplift)
#   FAIL = otherwise; track closes, no engine_init.hpp change, no S93.
#
# Wall-clock estimate: 1 baseline + 14 cells × ~85s/cell ≈ ~21 min on
# the 4.43 GB NSXUSD_merged.csv tape. Each harness invocation streams
# the full tape once.
#
# Output structure:
#   <output-dir>/
#     cells_summary.csv      1 row per cell + baseline + BEST_CELL.
#                            Columns: cell, lookback, threshold,
#                                     trades, wins, win_rate_pct,
#                                     gross_pnl, avg_pnl, sum_pos,
#                                     sum_neg, pf, n_tp_hit, n_sl_hit,
#                                     n_prove_it_fail, n_loss_cut,
#                                     n_be_cut, n_end_of_data, n_other,
#                                     worst_trade, pf_delta_pct, pass.
#     verdict.txt            Plain-text decision-rule verdict.
#     cells/
#       baseline_report.csv  Harness report for the baseline (gate OFF) run.
#       baseline_trades.csv  Harness trades CSV.
#       baseline_stderr.log
#       L<L>_T<T>_report.csv per-cell report (e.g. L30_T75_report.csv)
#       L<L>_T<T>_trades.csv per-cell trades
#       L<L>_T<T>_stderr.log per-cell stderr
#
# Usage:
#   scripts/utf5m_t4_p1.py <tape.csv> [options]
#
# Options:
#   --output-dir DIR       Output directory.
#                          Default: outputs/utf5m_t4_p1_<timestamp>/.
#   --harness PATH         Path to the harness binary.
#                          Default: ./build/UstecTrendFollow5mBacktest
#   --abs-pf-threshold X   Absolute PF gate. Default: 1.20.
#   --rel-pf-uplift X      Required PF uplift vs baseline (multiplicative).
#                          Default: 1.10 (i.e. best cell must beat baseline
#                          by at least +10% PF).
#   --skip-baseline        Skip the baseline run (only meaningful if a prior
#                          run's baseline outputs are reused via --baseline-pf).
#   --baseline-pf X        Override baseline PF (skip the run + use this
#                          value for the decision rule).
#   --lookbacks "L,L,..."  Override the lookbacks list. Default: 30,60.
#   --thresholds "T,T,..." Override the thresholds list.
#                          Default: 50,60,70,75,80,85,90.
# =============================================================================

from __future__ import annotations

import argparse
import calendar
import csv
import datetime as _dt
import itertools
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple


# =============================================================================
# Sweep grid (S92 Phase 1)
# =============================================================================
# Defaults per part-AB handoff §"2. UTF5m Tier 4 vol-regime gate port" step 3.
# Both axes are overridable via CLI flags so a follow-up cell-refinement run
# can drill in around any winner without touching this file.
DEFAULT_LOOKBACKS_DAYS = [30, 60]
DEFAULT_THRESHOLDS_PCT = [50.0, 60.0, 70.0, 75.0, 80.0, 85.0, 90.0]


# =============================================================================
# Format detection -- copied verbatim from scripts/utf5m_wf_t1.py so this
# driver is self-contained. We only need the timestamps for the banner;
# the harness re-detects format independently when it processes the tape.
# =============================================================================

FMT_A_TBA   = "A_TBA"
FMT_B_TBAV  = "B_TBAV"
FMT_C_DUKA  = "C_DUKA"
FMT_D_OHLCV = "D_OHLCV"
FMT_UNKNOWN = "UNKNOWN"


def detect_format(line: str) -> str:
    if not line:
        return FMT_UNKNOWN
    # Dukascopy: starts with "YYYY.MM.DD"
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
    if not line:
        return None
    if fmt != FMT_C_DUKA:
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
    # FMT_C_DUKA
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
    raise RuntimeError(
        f"No parseable last-line found in last {tail_bytes} bytes of {tape}"
    )


def fmt_iso_utc(ts_ms: int) -> str:
    dt = _dt.datetime.fromtimestamp(ts_ms / 1000.0, tz=_dt.timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def disk_free_bytes(path: Path) -> int:
    target = path if path.exists() else path.parent
    if not target.exists():
        target = Path(".")
    st = os.statvfs(target)
    return st.f_bavail * st.f_frsize


# =============================================================================
# Harness runner + report parsing
# =============================================================================

def run_harness(harness: Path, tape: Path, report: Path, trades: Path,
                stderr_log: Path,
                vol_gate_enabled: bool = False,
                lookback: Optional[int] = None,
                threshold: Optional[float] = None) -> None:
    """Run UstecTrendFollow5mBacktest on `tape` in --mode baseline.

    With vol_gate_enabled=True the three new S91 CLI flags are passed in
    addition. With vol_gate_enabled=False this run is the baseline reference
    (default-OFF gate, behaviour bit-for-bit identical to pre-S90 per the
    engine's default-OFF guarantee at UstecTrendFollow5mEngine.hpp).

    Mode is hard-pinned to --mode baseline so the comparison is
    apples-to-apples with S73 Phase 3 (which also ran --mode baseline).
    The Tier 4 entry-gate operates on top of the (S63-zeroed) baseline
    engine; it does NOT touch S63 management.
    """
    cmd = [
        str(harness),
        str(tape),
        "--mode", "baseline",
        "--quiet",
        "--report", str(report),
        "--trades", str(trades),
    ]
    if vol_gate_enabled:
        cmd.append("--vol-gate-enabled")
        if lookback is None or threshold is None:
            raise ValueError(
                "vol_gate_enabled=True requires both lookback and threshold "
                "to be passed."
            )
        cmd += ["--atr-lookback-days", str(int(lookback))]
        cmd += ["--vol-pct-threshold", f"{float(threshold):.4f}"]
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
    """Read the harness's trades CSV and compute
    (sum_positive_gross_pnl, sum_negative_gross_pnl, profit_factor).

    PF = sum_positive / abs(sum_negative). If sum_negative == 0:
      - returns +inf if sum_positive > 0 (degenerate "all winners")
      - returns 0.0 if sum_positive == 0 (no trades or all zero)
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
# Cell record + summary writing
# =============================================================================

# Column order for cells_summary.csv. `cell` is the human label
# ("baseline" / "L30_T75" / "BEST_CELL"); lookback/threshold are blank
# for the baseline row. pf_delta_pct is (cell_pf / baseline_pf - 1) * 100,
# i.e. percentage uplift vs baseline; blank for the baseline row.
SUMMARY_FIELDS = [
    "cell", "lookback", "threshold",
    "trades", "wins", "win_rate_pct",
    "gross_pnl", "avg_pnl", "sum_pos", "sum_neg", "pf",
    "n_tp_hit", "n_sl_hit", "n_prove_it_fail",
    "n_loss_cut", "n_be_cut", "n_end_of_data", "n_other",
    "worst_trade", "pf_delta_pct", "pass",
]


def build_row(name: str, lookback: Optional[int], threshold: Optional[float],
              rep: dict, sum_pos: float, sum_neg: float, pf: float,
              baseline_pf: Optional[float],
              abs_pf_threshold: float,
              rel_pf_uplift: float) -> dict:
    """Build a summary row dict for one cell (or the baseline / BEST_CELL).

    pass criterion (per-cell): pf >= abs_pf_threshold AND (baseline_pf is None
    OR pf >= rel_pf_uplift * baseline_pf). The baseline row itself uses
    baseline_pf=None so only the absolute gate is checked (used for sanity
    reporting; the baseline is never the "winner").
    """
    trades_n = int(rep.get("trades", "0"))
    wins     = int(rep.get("wins", "0"))
    win_rate = float(rep.get("win_rate_pct", "0"))
    gross    = float(rep.get("gross_pnl", "0"))
    avg      = float(rep.get("avg_pnl", "0"))
    n_tp     = int(rep.get("n_tp_hit", "0"))
    n_sl     = int(rep.get("n_sl_hit", "0"))
    n_pi     = int(rep.get("n_prove_it_fail", "0"))
    n_lc     = int(rep.get("n_loss_cut", "0"))
    n_be     = int(rep.get("n_be_cut", "0"))
    n_eod    = int(rep.get("n_end_of_data", "0"))
    n_oth    = int(rep.get("n_other", "0"))
    worst    = float(rep.get("worst_trade", "0"))

    if baseline_pf is None or baseline_pf <= 0.0:
        pf_delta_pct_str = ""
    else:
        # If pf is +inf (no losses), the delta is +inf too -- format as 'inf'
        # so CSV consumers can distinguish from a numeric ratio.
        if pf == float("inf"):
            pf_delta_pct_str = "inf"
        else:
            pf_delta_pct_str = f"{(pf / baseline_pf - 1.0) * 100.0:+.2f}"

    abs_ok = pf >= abs_pf_threshold
    rel_ok = (baseline_pf is None) or (pf == float("inf")) \
             or (pf >= rel_pf_uplift * baseline_pf)
    passed = abs_ok and rel_ok

    return {
        "cell":       name,
        "lookback":   "" if lookback   is None else str(int(lookback)),
        "threshold":  "" if threshold  is None else f"{float(threshold):.2f}",
        "trades":     trades_n,
        "wins":       wins,
        "win_rate_pct": f"{win_rate:.2f}",
        "gross_pnl":  f"{gross:.6f}",
        "avg_pnl":    f"{avg:.6f}",
        "sum_pos":    f"{sum_pos:.6f}",
        "sum_neg":    f"{sum_neg:.6f}",
        "pf":         ("inf" if pf == float("inf") else f"{pf:.4f}"),
        "n_tp_hit":   n_tp,
        "n_sl_hit":   n_sl,
        "n_prove_it_fail": n_pi,
        "n_loss_cut": n_lc,
        "n_be_cut":   n_be,
        "n_end_of_data": n_eod,
        "n_other":    n_oth,
        "worst_trade": f"{worst:.6f}",
        "pf_delta_pct": pf_delta_pct_str,
        "pass":       "yes" if passed else "no",
    }


def write_summary_csv(summary_path: Path,
                      baseline_row: dict,
                      cell_rows: List[dict],
                      best_row: dict) -> None:
    """Write baseline + cells + BEST_CELL rows to cells_summary.csv."""
    with summary_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=SUMMARY_FIELDS)
        w.writeheader()
        w.writerow({k: baseline_row.get(k, "") for k in SUMMARY_FIELDS})
        for r in cell_rows:
            w.writerow({k: r.get(k, "") for k in SUMMARY_FIELDS})
        w.writerow({k: best_row.get(k, "") for k in SUMMARY_FIELDS})


# =============================================================================
# Decision rule + verdict
# =============================================================================

def decide(baseline_row: dict, cell_rows: List[dict],
           abs_pf_threshold: float, rel_pf_uplift: float
           ) -> Tuple[bool, Optional[dict], str]:
    """Apply the Phase 1 decision rule.

    Returns (passed, best_row_or_None, verdict_text).
    Best cell is the maximum-PF cell among cell_rows; +inf is treated as
    "best" but the verdict text flags it as suspicious (no losses likely
    indicates degenerate trade-count collapse).
    """
    # Baseline PF (raw value for the verdict + ratio math)
    base_pf_raw = baseline_row.get("pf", "")
    if base_pf_raw == "inf":
        base_pf = float("inf")
    else:
        try:
            base_pf = float(base_pf_raw)
        except ValueError:
            base_pf = 0.0
    base_trades = int(baseline_row.get("trades", "0"))

    # Find best cell by PF. Skip degenerate cells with trades==0.
    best = None
    for r in cell_rows:
        try:
            cell_pf = float("inf") if r["pf"] == "inf" else float(r["pf"])
        except (ValueError, KeyError):
            continue
        try:
            cell_trades = int(r["trades"])
        except (ValueError, KeyError):
            continue
        if cell_trades <= 0:
            continue
        if best is None or cell_pf > best_pf:
            best = r
            best_pf = cell_pf  # noqa: F841  (assigned above)

    if best is None:
        verdict = (
            "=" * 72 + "\n"
            "  UTF5m USTEC.F Tier 4 vol-regime gate Phase 1 verdict\n"
            + "=" * 72 + "\n\n"
            "  FAIL -- no cell produced any trades. Gate is too aggressive\n"
            "  across the entire grid (every threshold suppresses all entries\n"
            "  at every lookback). Lower thresholds or shorter lookbacks\n"
            "  needed before any data is generated.\n"
        )
        return False, None, verdict

    best_pf = float("inf") if best["pf"] == "inf" else float(best["pf"])
    best_trades = int(best["trades"])

    abs_ok = best_pf >= abs_pf_threshold
    if base_pf > 0.0 and base_pf != float("inf"):
        required_uplift_pf = rel_pf_uplift * base_pf
        rel_ok = (best_pf == float("inf")) or (best_pf >= required_uplift_pf)
    else:
        # Degenerate baseline -- relative test cannot be applied. Fall back to
        # absolute-only and document this in the verdict.
        required_uplift_pf = None
        rel_ok = True

    overall = abs_ok and rel_ok

    lines = []
    lines.append("=" * 72)
    lines.append("  UTF5m USTEC.F Tier 4 vol-regime gate Phase 1 verdict")
    lines.append("=" * 72)
    lines.append("")
    lines.append("  Configuration:")
    lines.append("    Engine        : UstecTrendFollow5mEngine")
    lines.append("    Mode          : --mode baseline (S63 trio = 0.0/0.0/0.0)")
    lines.append("    Gate form     : entry-blocking (S90), strict-less-than percentile")
    lines.append("    Sweep grid    : (lookback, threshold) Cartesian")
    lines.append("")
    lines.append("  Baseline reference (gate OFF):")
    lines.append(f"    trades            : {base_trades}")
    if base_pf == float("inf"):
        lines.append("    PF                : inf  (degenerate: no losses)")
    else:
        lines.append(f"    PF                : {base_pf:.4f}")
    lines.append(f"    gross_pnl         : {baseline_row.get('gross_pnl', '')}")
    lines.append(f"    avg_pnl           : {baseline_row.get('avg_pnl', '')}")
    lines.append("")
    lines.append("  Best cell:")
    lines.append(f"    name              : {best['cell']}")
    lines.append(f"    lookback days     : {best['lookback']}")
    lines.append(f"    threshold pct     : {best['threshold']}")
    lines.append(f"    trades            : {best_trades}")
    if best_pf == float("inf"):
        lines.append("    PF                : inf  (degenerate: no losses)")
    else:
        lines.append(f"    PF                : {best_pf:.4f}")
    lines.append(f"    gross_pnl         : {best.get('gross_pnl', '')}")
    lines.append(f"    avg_pnl           : {best.get('avg_pnl', '')}")
    if best.get("pf_delta_pct") not in ("", None):
        lines.append(f"    pf vs baseline    : {best['pf_delta_pct']}%")
    lines.append("")
    lines.append("  Decision rule:")
    lines.append(f"    absolute PF gate  : best_pf >= {abs_pf_threshold:.2f}")
    if required_uplift_pf is not None:
        lines.append(
            f"    relative PF gate  : best_pf >= {rel_pf_uplift:.2f} * baseline_pf "
            f"= {required_uplift_pf:.4f}")
    else:
        lines.append(
            "    relative PF gate  : SKIPPED (baseline PF is 0 or inf -- "
            "absolute-only test)")
    lines.append("")
    lines.append("  Result:")
    lines.append(f"    absolute gate met : {'yes' if abs_ok else 'no'}")
    lines.append(f"    relative gate met : {'yes' if rel_ok else 'no'}")
    lines.append("")
    if overall:
        lines.append("  >>> OVERALL: PASS <<<")
        lines.append("")
        lines.append("  Next step: S93 -- Phase 3 walk-forward on the winning cell to")
        lines.append("  confirm the lift survives temporal subdivision. Use the existing")
        lines.append("  scripts/utf5m_wf_t1.py with the winning (lookback, threshold)")
        lines.append("  passed through to the harness (will need a small --vol-gate-*")
        lines.append("  passthrough patch to the WF driver). Verdict gates (S93):")
        lines.append("    aggregate PF >= 1.20  AND  >= 3 of 4 windows pass.")
        lines.append("")
        lines.append("  If S93 also passes, the re-enable path is:")
        lines.append("    1. Bump VOL_GATE_ENABLED=true / VOL_PCT_THRESHOLD / ")
        lines.append("       ATR_LOOKBACK_DAYS at the g_ustec_tf_5m init in")
        lines.append("       engine_init.hpp.")
        lines.append("    2. Flip g_ustec_tf_5m.enabled = true at the same site.")
        lines.append("    3. Keep shadow_mode = true for the first 6 months per the")
        lines.append("       engine header caveat at UstecTrendFollow5mEngine.hpp.")
    else:
        lines.append("  >>> OVERALL: FAIL <<<")
        lines.append("")
        lines.append("  No (lookback, threshold) cell cleared both the absolute and")
        lines.append("  relative PF gates. The Tier 4 vol-regime gate does not")
        lines.append("  rescue UTF5m to the 1.20 PF bar on this tape.")
        lines.append("")
        lines.append("  Recommendation: closure memo. Engine stays disabled at")
        lines.append("  engine_init.hpp (g_ustec_tf_5m.enabled = false, S68 stop-")
        lines.append("  bleed). Document the negative result in")
        lines.append("  outputs/UTF5M_TIER4_PHASE1_RESULTS_<date>.md. Do NOT proceed")
        lines.append("  to S93 (Phase 3 WF) -- the Phase 1 sweep is decisive.")
        lines.append("")
        lines.append("  Do NOT retune the grid post-hoc to fit -- per Phase 3 memo §7")
        lines.append("  item 4, post-hoc relaxation of the decision rule sets a")
        lines.append("  precedent that softens every future engine validation.")
    lines.append("")
    return overall, best, "\n".join(lines)


# =============================================================================
# Main
# =============================================================================

def parse_int_list(s: str) -> List[int]:
    return [int(x) for x in s.split(",") if x.strip()]


def parse_float_list(s: str) -> List[float]:
    return [float(x) for x in s.split(",") if x.strip()]


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(
        description="UTF5m USTEC.F Tier 4 vol-regime gate Phase 1 sweep driver",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("tape", type=Path, help="Path to the tape CSV.")
    ap.add_argument("--output-dir", type=Path, default=None,
                    help="Output directory. Default: outputs/utf5m_t4_p1_<ts>/")
    ap.add_argument("--harness", type=Path,
                    default=Path("./build/UstecTrendFollow5mBacktest"),
                    help="Path to harness binary.")
    ap.add_argument("--abs-pf-threshold", type=float, default=1.20,
                    help="Absolute PF gate. Default: 1.20.")
    ap.add_argument("--rel-pf-uplift", type=float, default=1.10,
                    help="Required PF uplift vs baseline (multiplicative). Default: 1.10.")
    ap.add_argument("--skip-baseline", action="store_true",
                    help="Skip the baseline harness run (use --baseline-pf instead).")
    ap.add_argument("--baseline-pf", type=float, default=None,
                    help="Override baseline PF (only with --skip-baseline).")
    ap.add_argument("--lookbacks", type=str, default=None,
                    help="Comma-separated lookback list. Default: 30,60.")
    ap.add_argument("--thresholds", type=str, default=None,
                    help="Comma-separated threshold list. Default: 50,60,70,75,80,85,90.")
    args = ap.parse_args(argv)

    # ----- Pre-flight checks -----------------------------------------------
    if not args.tape.is_file():
        print(f"[ERROR] Tape not found: {args.tape}", file=sys.stderr)
        return 1
    if not args.harness.is_file() or not os.access(args.harness, os.X_OK):
        print(f"[ERROR] Harness not found or not executable: {args.harness}",
              file=sys.stderr)
        print("        Build with:", file=sys.stderr)
        print("          cmake --build build --target UstecTrendFollow5mBacktest "
              "--config Release -j", file=sys.stderr)
        return 1
    if args.skip_baseline and args.baseline_pf is None:
        print("[ERROR] --skip-baseline requires --baseline-pf <value>", file=sys.stderr)
        return 1

    lookbacks  = parse_int_list(args.lookbacks)   if args.lookbacks   else list(DEFAULT_LOOKBACKS_DAYS)
    thresholds = parse_float_list(args.thresholds) if args.thresholds else list(DEFAULT_THRESHOLDS_PCT)

    if not lookbacks or not thresholds:
        print("[ERROR] --lookbacks and --thresholds must be non-empty.",
              file=sys.stderr)
        return 1

    if args.output_dir is None:
        ts = time.strftime("%Y%m%d_%H%M%S")
        args.output_dir = Path("outputs") / f"utf5m_t4_p1_{ts}"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    cells_dir = args.output_dir / "cells"
    cells_dir.mkdir(parents=True, exist_ok=True)

    # ----- Header banner ----------------------------------------------------
    n_cells = len(lookbacks) * len(thresholds)
    runs    = (0 if args.skip_baseline else 1) + n_cells
    print("=" * 70, file=sys.stderr)
    print("  UTF5m USTEC.F Tier 4 vol-regime gate Phase 1 sweep", file=sys.stderr)
    print(f"  Tape           : {args.tape}", file=sys.stderr)
    print(f"  Output         : {args.output_dir}", file=sys.stderr)
    print(f"  Harness        : {args.harness}", file=sys.stderr)
    print(f"  Mode           : --mode baseline (S63 trio = 0.0)", file=sys.stderr)
    print(f"  Lookbacks (d)  : {lookbacks}", file=sys.stderr)
    print(f"  Thresholds (%) : {thresholds}", file=sys.stderr)
    print(f"  Sweep cells    : {n_cells}", file=sys.stderr)
    print(f"  Total runs     : {runs}", file=sys.stderr)
    print(f"  abs PF gate    : {args.abs_pf_threshold:.2f}", file=sys.stderr)
    print(f"  rel PF uplift  : {args.rel_pf_uplift:.2f}x baseline", file=sys.stderr)
    print("=" * 70, file=sys.stderr)
    print("", file=sys.stderr)

    # ----- Tape boundary detection (for verdict banner only) ---------------
    try:
        first_line, _ = read_first_data_line(args.tape)
        fmt = detect_format(first_line)
        start_ms = parse_ts_ms(first_line, fmt) if fmt != FMT_UNKNOWN else None
        last_line = read_last_data_line(args.tape, fmt) if fmt != FMT_UNKNOWN else ""
        end_ms = parse_ts_ms(last_line, fmt) if last_line else None
        if start_ms and end_ms and end_ms > start_ms:
            days = (end_ms - start_ms) / (1000.0 * 86400.0)
            print(f"[detect] fmt={fmt}  start={fmt_iso_utc(start_ms)}"
                  f"  end={fmt_iso_utc(end_ms)}  days={days:.2f}",
                  file=sys.stderr)
            print("", file=sys.stderr)
    except Exception as e:  # noqa: BLE001
        print(f"[detect] boundary detection skipped: {e}", file=sys.stderr)
        print("", file=sys.stderr)

    # ----- Baseline run (gate OFF) -----------------------------------------
    baseline_row: dict
    if args.skip_baseline:
        # Synthesize a baseline row using the user-supplied PF only. trades,
        # gross etc. unknown; left blank. This path is intended for re-runs
        # where the baseline has already been captured.
        baseline_row = {
            "cell":       "baseline (provided)",
            "lookback":   "",
            "threshold":  "",
            "trades":     "",
            "wins":       "",
            "win_rate_pct": "",
            "gross_pnl":  "",
            "avg_pnl":    "",
            "sum_pos":    "",
            "sum_neg":    "",
            "pf":         f"{args.baseline_pf:.4f}",
            "n_tp_hit":   "",
            "n_sl_hit":   "",
            "n_prove_it_fail": "",
            "n_loss_cut": "",
            "n_be_cut":   "",
            "n_end_of_data": "",
            "n_other":    "",
            "worst_trade": "",
            "pf_delta_pct": "",
            "pass":       "n/a",
        }
        print(f"[baseline] skipped per --skip-baseline; using PF={args.baseline_pf:.4f}",
              file=sys.stderr)
    else:
        t0 = time.time()
        report_b     = cells_dir / "baseline_report.csv"
        trades_b     = cells_dir / "baseline_trades.csv"
        stderr_log_b = cells_dir / "baseline_stderr.log"
        sys.stderr.write("[baseline] harness ... ")
        sys.stderr.flush()
        try:
            run_harness(args.harness, args.tape, report_b, trades_b, stderr_log_b,
                        vol_gate_enabled=False)
        except subprocess.CalledProcessError as e:
            sys.stderr.write(f"FAILED rc={e.returncode}\n")
            sys.stderr.write(f"          see {stderr_log_b}\n")
            return 1
        rep_b = parse_report_csv(report_b)
        sum_pos_b, sum_neg_b, pf_b = compute_pf_from_trades(trades_b)
        baseline_row = build_row("baseline", None, None,
                                 rep_b, sum_pos_b, sum_neg_b, pf_b,
                                 baseline_pf=None,
                                 abs_pf_threshold=args.abs_pf_threshold,
                                 rel_pf_uplift=args.rel_pf_uplift)
        dt = time.time() - t0
        pf_str = "inf" if pf_b == float("inf") else f"{pf_b:.3f}"
        sys.stderr.write(
            f"trades={baseline_row['trades']:>6}  "
            f"avg={float(baseline_row['avg_pnl']):>+10.6f}  "
            f"pf={pf_str:>6}  ({dt:.0f}s)\n")

    # Extract baseline_pf (numeric) for downstream comparisons.
    base_pf_str = baseline_row.get("pf", "")
    if base_pf_str == "inf":
        baseline_pf = float("inf")
    else:
        try:
            baseline_pf = float(base_pf_str)
        except ValueError:
            baseline_pf = 0.0

    # ----- Cell sweep ------------------------------------------------------
    cell_rows: List[dict] = []
    for L, T in itertools.product(lookbacks, thresholds):
        cell_name = f"L{int(L)}_T{int(round(T))}"
        report     = cells_dir / f"{cell_name}_report.csv"
        trades     = cells_dir / f"{cell_name}_trades.csv"
        stderr_log = cells_dir / f"{cell_name}_stderr.log"

        sys.stderr.write(f"[{cell_name}] harness ... ")
        sys.stderr.flush()
        t0 = time.time()
        try:
            run_harness(args.harness, args.tape, report, trades, stderr_log,
                        vol_gate_enabled=True, lookback=L, threshold=T)
        except subprocess.CalledProcessError as e:
            sys.stderr.write(f"FAILED rc={e.returncode}\n")
            sys.stderr.write(f"          see {stderr_log}\n")
            return 1
        rep = parse_report_csv(report)
        sum_pos, sum_neg, pf = compute_pf_from_trades(trades)
        row = build_row(cell_name, L, T, rep, sum_pos, sum_neg, pf,
                        baseline_pf=(baseline_pf if baseline_pf > 0.0
                                     and baseline_pf != float("inf") else None),
                        abs_pf_threshold=args.abs_pf_threshold,
                        rel_pf_uplift=args.rel_pf_uplift)
        cell_rows.append(row)
        dt = time.time() - t0
        pf_str = "inf" if pf == float("inf") else f"{pf:.3f}"
        delta_str = row.get("pf_delta_pct", "")
        delta_disp = f"{delta_str}%" if delta_str not in ("", None) else "n/a"
        sys.stderr.write(
            f"trades={row['trades']:>6}  "
            f"avg={float(row['avg_pnl']):>+10.6f}  "
            f"pf={pf_str:>6}  Δ={delta_disp:>10}  "
            f"({dt:.0f}s) {row['pass'].upper()}\n")

    # ----- Decision + verdict ----------------------------------------------
    print("", file=sys.stderr)
    overall, best, verdict_text = decide(baseline_row, cell_rows,
                                         args.abs_pf_threshold,
                                         args.rel_pf_uplift)

    # Build BEST_CELL row for the summary CSV (either the actual best cell
    # promoted to a "BEST_CELL" label, or a sentinel "no_winner" row).
    if best is None:
        best_row = {f: ("BEST_CELL (none)" if f == "cell" else "") for f in SUMMARY_FIELDS}
        best_row["pass"] = "no"
    else:
        best_row = dict(best)
        best_row["cell"] = f"BEST_CELL ({best['cell']})"

    summary_path = args.output_dir / "cells_summary.csv"
    write_summary_csv(summary_path, baseline_row, cell_rows, best_row)
    print(f"[summary] {summary_path}", file=sys.stderr)

    verdict_path = args.output_dir / "verdict.txt"
    verdict_path.write_text(verdict_text + "\n")
    print("", file=sys.stderr)
    print(verdict_text, file=sys.stderr)
    print(f"[verdict] {verdict_path}", file=sys.stderr)

    return 0 if overall else 2  # exit 2 on FAIL so CI can distinguish from error


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
