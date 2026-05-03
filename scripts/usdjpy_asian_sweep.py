#!/usr/bin/env python3
"""
usdjpy_asian_sweep.py
=====================
Parameter sweep driver for UsdjpyAsianOpenEngine.

Mirrors the methodology used for the EURUSD S55/S56 sweeps:
  1. For each parameter / value pair, sed-rewrite a tmp copy of the engine
     header in backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp.
  2. Recompile the harness (build/usdjpy_asian_bt).
  3. Run against the staged 14-month USDJPY tick set.
  4. Aggregate per-run summary into one results CSV.
  5. Print a sorted leaderboard.

The production engine in include/UsdjpyAsianOpenEngine.hpp is never modified.
"""

import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO   = Path(__file__).resolve().parent.parent
INC    = REPO / "include" / "UsdjpyAsianOpenEngine.hpp"
BT_DIR = REPO / "backtest" / "usdjpy_bt"
BT_HPP = BT_DIR / "UsdjpyAsianOpenEngine.hpp"
BT_CPP = BT_DIR / "UsdjpyAsianOpenBacktest.cpp"
BIN    = REPO / "build" / "usdjpy_asian_bt"
# Allow override via env var (e.g. when running inside a sandbox where ~ resolves
# differently from the host's ~/Tick/USDJPY).
TICKS  = os.environ.get("USDJPY_TICKS",
                       "/sessions/laughing-trusting-lovelace/mnt/USDJPY"
                       if os.path.isdir("/sessions/laughing-trusting-lovelace/mnt/USDJPY")
                       else os.path.expanduser("~/Tick/USDJPY"))
RESULTS_CSV = REPO / "build" / "usdjpy_sweep_results.csv"

# Default parameter values matching the production engine.hpp at port time.
# Used as the "all-other-axes" baseline when sweeping a single axis.
BASELINE = {
    "STRUCTURE_LOOKBACK":         600,
    "MIN_RANGE":                  0.08,
    "MAX_RANGE":                  0.50,
    "SL_FRAC":                    0.80,
    "SL_BUFFER":                  0.02,
    "TP_RR":                      2.0,
    "TRAIL_FRAC":                 0.30,
    "MIN_TRAIL_ARM_PTS":          0.06,
    "MIN_TRAIL_ARM_SECS":         30,
    "MFE_TRAIL_FRAC":             0.40,
    "BE_TRIGGER_PTS":             0.06,
    "SAME_LEVEL_BLOCK_PTS":       0.08,
    "SAME_LEVEL_POST_SL_BLOCK_S": 1200,
    "SAME_LEVEL_POST_WIN_BLOCK_S":600,
    "MAX_SPREAD":                 0.02,
    "COOLDOWN_S":                 120,
    "SESSION_START_HOUR_UTC":     0,
    "SESSION_END_HOUR_UTC":       4,
    "LOT_MAX":                    0.20,
}

INT_PARAMS = {
    "STRUCTURE_LOOKBACK", "MIN_TRAIL_ARM_SECS", "SAME_LEVEL_POST_SL_BLOCK_S",
    "SAME_LEVEL_POST_WIN_BLOCK_S", "COOLDOWN_S", "SESSION_START_HOUR_UTC",
    "SESSION_END_HOUR_UTC",
}

# Format a value the way the engine.hpp constexpr lines use.
def fmt_val(name, v):
    if name in INT_PARAMS:
        return str(int(v))
    # Floats: keep enough precision for the parser to round-trip cleanly.
    s = f"{v:.6f}".rstrip("0").rstrip(".")
    return s

# Rewrite the bt copy of the engine.hpp with overrides applied to BASELINE.
def write_engine_copy(overrides):
    src = INC.read_text()
    out = src
    merged = {**BASELINE, **overrides}
    for name, v in merged.items():
        # Match: static constexpr <type> <name> = <something>;
        pattern = re.compile(
            rf"(static\s+constexpr\s+(?:int|double)\s+{name}\s*=\s*)([^;]+)(;)"
        )
        repl = lambda m, val=fmt_val(name, v): f"{m.group(1)}{val}{m.group(3)}"
        new, n = pattern.subn(repl, out, count=1)
        if n == 0:
            print(f"[WARN] could not substitute {name}", file=sys.stderr)
        out = new
    BT_HPP.write_text(out)

def compile_bt():
    cmd = [
        "g++", "-std=c++17", "-O3", "-Wno-unused-parameter",
        "-DOMEGA_BACKTEST",
        "-I", str(BT_DIR),
        str(BT_CPP),
        "-o", str(BIN),
        "-pthread",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        sys.exit(1)

# Run the harness; return (summary_dict, raw_stderr).
def run_bt(label, frm="2025-03", to="2026-04", trades_csv=None):
    if trades_csv is None:
        trades_csv = REPO / "build" / f"sweep_trades_{label}.csv"
    report_csv = REPO / "build" / f"sweep_report_{label}.csv"
    cmd = [str(BIN),
           "--ticks", TICKS,
           "--from", frm, "--to", to,
           "--trades", str(trades_csv),
           "--report", str(report_csv),
           "--label", label]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        sys.exit(1)
    summary = parse_summary(r.stderr)
    summary["label"] = label
    return summary, r.stderr

def parse_summary(stderr):
    s = {}
    for line in stderr.splitlines():
        m = re.search(r"Trades\s*:\s*(\d+)", line);              s.setdefault("trades", int(m.group(1))) if m else None
        m = re.search(r"TP_HIT / TRAIL_HIT / SL_HIT / BE_HIT\s*:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)", line)
        if m:
            s["tp_hit"], s["trail_hit"], s["sl_hit"], s["be_hit"] = (int(m.group(i)) for i in (1,2,3,4))
        m = re.search(r"Wins / Losses / BE\s*:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)\s*\(decided WR = ([\d.]+)%\)", line)
        if m:
            s["wins"], s["losses"], s["bes"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
            s["wr"] = float(m.group(4))
        m = re.search(r"Avg win / loss\s*:\s*\$([-\d.]+)\s*/\s*\$([-\d.]+)", line)
        if m: s["avg_win"], s["avg_loss"] = float(m.group(1)), float(m.group(2))
        m = re.search(r"Net PnL\s*:\s*\$([-\d.]+)", line);       s["net_pnl"] = float(m.group(1)) if m else s.get("net_pnl")
        m = re.search(r"Profit factor\s*:\s*([-\d.]+)", line);   s["pf"]      = float(m.group(1)) if m else s.get("pf")
        m = re.search(r"Max drawdown\s*:\s*\$([-\d.]+)", line);  s["max_dd"]  = float(m.group(1)) if m else s.get("max_dd")
        m = re.search(r"Profitable months\s*:\s*(\d+)\s*/\s*(\d+)", line)
        if m: s["prof_months"], s["months"] = int(m.group(1)), int(m.group(2))
    return s

def append_row(d, header_done):
    cols = ["label","trades","wr","wins","losses","bes","tp_hit","trail_hit","sl_hit","be_hit",
            "avg_win","avg_loss","net_pnl","pf","max_dd","prof_months","months"]
    if not header_done:
        RESULTS_CSV.write_text(",".join(cols) + "\n")
    with RESULTS_CSV.open("a") as f:
        row = [str(d.get(c, "")) for c in cols]
        f.write(",".join(row) + "\n")

# -- Single-axis sweep specification ----------------------------------------
SWEEPS = [
    ("TP_RR",          [1.5, 2.0, 2.5, 3.0, 3.5]),
    ("SL_FRAC",        [0.40, 0.50, 0.60, 0.70, 0.80, 0.90, 1.00]),
    ("BE_TRIGGER_PTS", [0.0, 0.04, 0.06, 0.08, 0.10, 0.15]),
    ("MIN_RANGE",      [0.06, 0.08, 0.10, 0.12, 0.15]),
    ("MFE_TRAIL_FRAC", [0.30, 0.40, 0.50, 0.60]),
    ("COOLDOWN_S",     [60, 120, 240, 600]),
    ("TRAIL_FRAC",     [0.20, 0.25, 0.30, 0.40]),
    ("SAME_LEVEL_BLOCK_PTS", [0.04, 0.06, 0.08, 0.10, 0.12]),
]

def already_done():
    """Return set of labels already present in the results CSV."""
    done = set()
    if not RESULTS_CSV.exists():
        return done
    try:
        with RESULTS_CSV.open() as f:
            f.readline()  # header (or empty)
            for line in f:
                if line.strip():
                    done.add(line.split(",")[0])
    except Exception:
        pass
    return done

def run_sweep(frm="2025-03", to="2026-04", max_cells=None):
    """Resumable sweep: skips labels already in RESULTS_CSV. Stops after
    max_cells new cells (None = run all)."""
    done = already_done()
    header_done = RESULTS_CSV.exists() and RESULTS_CSV.stat().st_size > 0
    cells_run = 0
    rows = []

    plan = [("baseline", {})]
    for name, values in SWEEPS:
        for v in values:
            plan.append((f"{name}={v}", {name: v}))

    for label, overrides in plan:
        if label in done:
            continue
        if max_cells is not None and cells_run >= max_cells:
            print(f"[stop] reached max_cells={max_cells}; remaining: {len(plan) - len(done) - cells_run}")
            return
        write_engine_copy(overrides)
        compile_bt()
        s, _ = run_bt(label, frm=frm, to=to)
        append_row(s, header_done); header_done = True
        rows.append(s)
        print(f"{label:25s}: trades={s['trades']:5d} WR={s['wr']:5.1f}% PF={s['pf']:5.2f} PnL=${s['net_pnl']:8.2f} DD=${s['max_dd']:7.2f} months={s['prof_months']}/{s['months']}")
        cells_run += 1

    print(f"\n[done] ran {cells_run} new cells, total in CSV: {len(already_done())}")

# -- Composite & OOS modes --------------------------------------------------
def run_composite(label, overrides, frm="2025-03", to="2026-04"):
    write_engine_copy(overrides)
    compile_bt()
    s, _ = run_bt(label, frm=frm, to=to)
    print(f"{label:30s}: trades={s['trades']:5d} WR={s['wr']:5.1f}% PF={s['pf']:5.2f} PnL=${s['net_pnl']:8.2f} DD=${s['max_dd']:7.2f} months={s['prof_months']}/{s['months']}")
    return s

def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "single"
    if cmd == "single":
        max_cells = int(sys.argv[2]) if len(sys.argv) > 2 else None
        run_sweep(max_cells=max_cells)
    elif cmd == "oos":
        # Train: 2025-03..2025-09. Test: 2025-10..2026-04.
        from_train, to_train = "2025-03", "2025-09"
        from_test,  to_test  = "2025-10", "2026-04"
        # The user passes overrides as KEY=VAL on the command line.
        overrides = {}
        for arg in sys.argv[2:]:
            if "=" in arg:
                k, v = arg.split("=", 1)
                overrides[k] = float(v) if "." in v else int(v) if v.isdigit() else float(v)
        print(f"-- TRAIN ({from_train}..{to_train}) --")
        run_composite("train", overrides, from_train, to_train)
        print(f"-- TEST/OOS ({from_test}..{to_test}) --")
        run_composite("oos", overrides, from_test, to_test)
    elif cmd == "run":
        # Single-config full-range run with overrides from CLI.
        label = sys.argv[2] if len(sys.argv) > 2 else "manual"
        overrides = {}
        for arg in sys.argv[3:]:
            if "=" in arg:
                k, v = arg.split("=", 1)
                overrides[k] = float(v) if "." in v else int(v)
        run_composite(label, overrides)
    else:
        print(f"unknown subcommand: {cmd}")
        sys.exit(2)

if __name__ == "__main__":
    main()
