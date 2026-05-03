#!/usr/bin/env python3
"""
usdjpy_asian_trail_sweep.py
===========================
Trail-axis sweep driver for UsdjpyAsianOpenEngine, anchored on the best
config found by the prior single-axis sweep (MIN_RANGE=0.15, MFE_TRAIL_FRAC=0.5).

Why this exists (and why the original scripts/usdjpy_asian_sweep.py was not enough)
----------------------------------------------------------------------------------
The original 14-month single-axis sweep concluded "no profitable config".
The MFE distribution analysis on its trade ledger
(scripts/mae_mfe_analysis.py-style probe) shows otherwise:

    * Winners reach an average MFE of  9.97 pips.
    * Average realised exit pips      6.13.
    * That is  3.84 pips clipped from every winner.
    * 30 percent of winners ever reach MFE > 10 pips, 11 percent > 15 pips,
      6 percent > 20 pips, max 34.85 pips.
    * 597 of 600 winners exit via TRAIL.  TP fires only 3 times in 832
      trades. The trail dominates the win side completely.

The trailing stop is governed by

    trail_dist = min(range * TRAIL_FRAC,  mfe * MFE_TRAIL_FRAC)

For range = 0.15 (15 pips) and TRAIL_FRAC = 0.30, the range-component caps
the trail at 4.5 pips.  Past that, the trail never widens, so even
mfe * MFE_TRAIL_FRAC cannot release the cap until MFE_TRAIL_FRAC is raised
high enough -- which then makes the trail too loose at small MFE.

The original sweep tested

    TRAIL_FRAC      in [0.20, 0.25, 0.30, 0.40]   -- never released the cap
    MFE_TRAIL_FRAC  in [0.30, 0.40, 0.50, 0.60]   -- never tested below 0.30
    MIN_TRAIL_ARM_PTS                             -- never swept at all
    MIN_TRAIL_ARM_SECS                            -- never swept at all

This driver fills those gaps:

    PHASE 1 (trail single-axis on the MIN_RANGE=0.15 anchor)
        TRAIL_FRAC          {0.30, 0.50, 0.75, 1.00, 1.50, 2.00}
        MFE_TRAIL_FRAC      {0.10, 0.15, 0.20, 0.25, 0.30, 0.50}
        MIN_TRAIL_ARM_PTS   {0.06, 0.10, 0.15, 0.20, 0.30}
        MIN_TRAIL_ARM_SECS  {30, 60, 120, 240}

    PHASE 2 (top-2 from each axis, pairwise composite)
        Picks best two by net_pnl per axis, runs every product.

    PHASE 3 (STRUCTURE_LOOKBACK from the prior session queue, anchored on
             the best composite from Phase 2)
        STRUCTURE_LOOKBACK  {200, 300, 400, 600, 900}

    PHASE 4 (OOS validation on the best composite)
        Train 2025-03..2025-09, Test 2025-10..2026-04.

The production engine in include/UsdjpyAsianOpenEngine.hpp is never modified.
Only the backtest copy in backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp is
sed-rewritten per cell, just like scripts/usdjpy_asian_sweep.py does.

CLI
---
    python3 scripts/usdjpy_asian_trail_sweep.py phase1       # single-axis
    python3 scripts/usdjpy_asian_trail_sweep.py phase2       # composites
    python3 scripts/usdjpy_asian_trail_sweep.py phase2_fine  # MRxSLxMFE 3x3x5 grid
    python3 scripts/usdjpy_asian_trail_sweep.py phase3       # structure lookback
    python3 scripts/usdjpy_asian_trail_sweep.py oos KEY=VAL ...
    python3 scripts/usdjpy_asian_trail_sweep.py all          # phase1+phase2+phase3+oos on winner

Results CSV
-----------
    build/usdjpy_trail_sweep_results.csv

Resumable: cells already in the CSV are skipped on rerun.

Author note
-----------
Per project policy the production engine in include/ is OFF-LIMITS to this
script.  Only the backtest copy is rewritten.  The script does not modify
scripts/usdjpy_asian_sweep.py either; this is an independent driver.
"""

import os
import re
import shutil
import subprocess
import sys
import time
from itertools import product
from pathlib import Path

# -- Paths -------------------------------------------------------------------
REPO   = Path(__file__).resolve().parent.parent
INC    = REPO / "include" / "UsdjpyAsianOpenEngine.hpp"
BT_DIR = REPO / "backtest" / "usdjpy_bt"
BT_HPP = BT_DIR / "UsdjpyAsianOpenEngine.hpp"
BT_CPP = BT_DIR / "UsdjpyAsianOpenBacktest.cpp"
BIN    = REPO / "build" / "usdjpy_asian_bt"

TICKS  = os.environ.get(
    "USDJPY_TICKS",
    "/sessions/laughing-trusting-lovelace/mnt/USDJPY"
    if os.path.isdir("/sessions/laughing-trusting-lovelace/mnt/USDJPY")
    else (
        "/sessions/epic-affectionate-lamport/mnt/USDJPY"
        if os.path.isdir("/sessions/epic-affectionate-lamport/mnt/USDJPY")
        else os.path.expanduser("~/Tick/USDJPY")
    ),
)

RESULTS_CSV = REPO / "build" / "usdjpy_trail_sweep_results.csv"

# -- Anchor config (best from prior session: MIN_RANGE=0.15) -----------------
# Every Phase 1/2/3 cell starts from this; cell-specific overrides layer on top.
ANCHOR = {
    "STRUCTURE_LOOKBACK":         600,
    "MIN_RANGE":                  0.15,   # winner from prior sweep
    "MAX_RANGE":                  0.50,
    "SL_FRAC":                    0.80,
    "SL_BUFFER":                  0.02,
    "TP_RR":                      2.0,
    "TRAIL_FRAC":                 0.30,
    "MIN_TRAIL_ARM_PTS":          0.06,
    "MIN_TRAIL_ARM_SECS":         30,
    "MFE_TRAIL_FRAC":             0.50,   # winner from prior sweep
    "BE_TRIGGER_PTS":             0.06,
    "SAME_LEVEL_BLOCK_PTS":       0.08,
    "SAME_LEVEL_POST_SL_BLOCK_S": 1200,
    "SAME_LEVEL_POST_WIN_BLOCK_S": 600,
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


def fmt_val(name, v):
    if name in INT_PARAMS:
        return str(int(v))
    s = f"{v:.6f}".rstrip("0").rstrip(".")
    return s


def write_engine_copy(overrides):
    """Sed-rewrite the backtest copy of the engine header with overrides applied
    to ANCHOR. The production header in include/ is never touched."""
    src = INC.read_text()
    out = src
    merged = {**ANCHOR, **overrides}
    for name, v in merged.items():
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
        print(r.stderr, file=sys.stderr)
        sys.exit(1)


def run_bt(label, frm="2025-03", to="2026-04"):
    trades_csv = REPO / "build" / f"trail_trades_{label}.csv"
    report_csv = REPO / "build" / f"trail_report_{label}.csv"
    cmd = [str(BIN),
           "--ticks", TICKS,
           "--from", frm, "--to", to,
           "--trades", str(trades_csv),
           "--report", str(report_csv),
           "--label", label]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        sys.exit(1)
    return parse_summary(r.stderr)


def parse_summary(stderr):
    s = {}
    for line in stderr.splitlines():
        m = re.search(r"Trades\s*:\s*(\d+)", line)
        if m and "trades" not in s:
            s["trades"] = int(m.group(1))
        m = re.search(r"TP_HIT / TRAIL_HIT / SL_HIT / BE_HIT\s*:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)", line)
        if m:
            s["tp_hit"], s["trail_hit"], s["sl_hit"], s["be_hit"] = (int(m.group(i)) for i in (1, 2, 3, 4))
        m = re.search(r"Wins / Losses / BE\s*:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)\s*\(decided WR = ([\d.]+)%\)", line)
        if m:
            s["wins"], s["losses"], s["bes"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
            s["wr"] = float(m.group(4))
        m = re.search(r"Avg win / loss\s*:\s*\$([-\d.]+)\s*/\s*\$([-\d.]+)", line)
        if m:
            s["avg_win"], s["avg_loss"] = float(m.group(1)), float(m.group(2))
        m = re.search(r"Net PnL\s*:\s*\$([-\d.]+)", line)
        if m:
            s["net_pnl"] = float(m.group(1))
        m = re.search(r"Profit factor\s*:\s*([-\d.]+)", line)
        if m:
            s["pf"] = float(m.group(1))
        m = re.search(r"Max drawdown\s*:\s*\$([-\d.]+)", line)
        if m:
            s["max_dd"] = float(m.group(1))
        m = re.search(r"Profitable months\s*:\s*(\d+)\s*/\s*(\d+)", line)
        if m:
            s["prof_months"], s["months"] = int(m.group(1)), int(m.group(2))
    return s


def append_row(label, overrides, d, header_done):
    cols = ["label", "overrides", "trades", "wr", "wins", "losses", "bes",
            "tp_hit", "trail_hit", "sl_hit", "be_hit",
            "avg_win", "avg_loss", "net_pnl", "pf", "max_dd",
            "prof_months", "months"]
    if not header_done:
        RESULTS_CSV.write_text(",".join(cols) + "\n")
    row = {"label": label, "overrides": ";".join(f"{k}={v}" for k, v in overrides.items())}
    row.update(d)
    with RESULTS_CSV.open("a") as f:
        f.write(",".join(str(row.get(c, "")) for c in cols) + "\n")


def already_done():
    done = set()
    if not RESULTS_CSV.exists():
        return done
    try:
        with RESULTS_CSV.open() as f:
            f.readline()
            for line in f:
                if line.strip():
                    done.add(line.split(",")[0])
    except Exception:
        pass
    return done


def run_one(label, overrides, frm="2025-03", to="2026-04"):
    write_engine_copy(overrides)
    compile_bt()
    s = run_bt(label, frm, to)
    return s


# -- Cell plans --------------------------------------------------------------
PHASE1_AXES = [
    ("TRAIL_FRAC",         [0.30, 0.50, 0.75, 1.00, 1.50, 2.00]),
    ("MFE_TRAIL_FRAC",     [0.10, 0.15, 0.20, 0.25, 0.30, 0.50]),
    ("MIN_TRAIL_ARM_PTS",  [0.06, 0.10, 0.15, 0.20, 0.30]),
    ("MIN_TRAIL_ARM_SECS", [30, 60, 120, 240]),
]

PHASE3_LOOKBACKS = [200, 300, 400, 600, 900]

# Phase 2 fine grid (added 2026-05-02, queue item 1):
# 3x3x5 = 45 cells around the chosen winner from the trail-fix sweep
# (MIN_RANGE=0.20, SL_FRAC=0.80, MFE_TRAIL_FRAC=0.15). Confirms the plateau
# extends in 2-D and looks for a sharper interior optimum. Labels use a
# consistent 2-decimal format (e.g. MR0.20_SL0.80_MFE0.15) which means the
# three pre-existing rows MR0.20_SL0.8_MFE0.{10,15,20} (1-decimal SL) are
# distinct and will be re-run. They reproduce identical results since the
# backtest is deterministic; the cost is ~30 seconds of redundant compute
# in exchange for a fully consistent leaderboard label scheme.
PHASE2_FINE_GRID = {
    "MIN_RANGE":      [0.19, 0.20, 0.21],
    "SL_FRAC":        [0.75, 0.80, 0.85],
    "MFE_TRAIL_FRAC": [0.10, 0.12, 0.15, 0.18, 0.20],
}


def run_phase1():
    done = already_done()
    header_done = RESULTS_CSV.exists() and RESULTS_CSV.stat().st_size > 0
    print(f"[phase1] anchor: {ANCHOR}")

    # Anchor itself, so we have a clean baseline row.
    if "anchor" not in done:
        s = run_one("anchor", {})
        append_row("anchor", {}, s, header_done); header_done = True
        _print_row("anchor", s)

    for axis, values in PHASE1_AXES:
        for v in values:
            label = f"{axis}={v}"
            if label in done:
                continue
            overrides = {axis: v}
            s = run_one(label, overrides)
            append_row(label, overrides, s, header_done); header_done = True
            _print_row(label, s)


def _print_row(label, s):
    print(
        f"{label:30s}: trades={s.get('trades',0):5d} "
        f"WR={s.get('wr',0):5.1f}% "
        f"avg_win=${s.get('avg_win',0):5.2f} "
        f"avg_loss=${s.get('avg_loss',0):6.2f} "
        f"PF={s.get('pf',0):4.2f} "
        f"PnL=${s.get('net_pnl',0):8.2f} "
        f"DD=${s.get('max_dd',0):7.2f} "
        f"months={s.get('prof_months',0)}/{s.get('months',0)}"
    )


def _read_results():
    out = []
    if not RESULTS_CSV.exists():
        return out
    with RESULTS_CSV.open() as f:
        header = f.readline().strip().split(",")
        for line in f:
            parts = line.rstrip("\n").split(",")
            row = dict(zip(header, parts))
            try:
                row["net_pnl"] = float(row.get("net_pnl") or "0")
            except Exception:
                row["net_pnl"] = 0.0
            out.append(row)
    return out


def run_phase2():
    """Take the top-2 single-axis winners (by net_pnl) per axis, run pairwise
    composites of those four (or fewer) overrides."""
    rows = _read_results()
    if not rows:
        print("[phase2] no Phase 1 results; run phase1 first", file=sys.stderr)
        sys.exit(1)
    by_axis = {a: [] for a, _ in PHASE1_AXES}
    for row in rows:
        for axis, _ in PHASE1_AXES:
            if row["label"].startswith(axis + "="):
                by_axis[axis].append(row)
    picks = {}
    for axis, axis_rows in by_axis.items():
        if not axis_rows:
            continue
        axis_rows.sort(key=lambda r: r["net_pnl"], reverse=True)
        picks[axis] = []
        for r in axis_rows[:2]:
            v_str = r["label"].split("=", 1)[1]
            v = float(v_str) if "." in v_str else int(v_str)
            picks[axis].append(v)

    print(f"[phase2] axis top-2: {picks}")

    done = already_done()
    header_done = RESULTS_CSV.exists() and RESULTS_CSV.stat().st_size > 0

    axes = list(picks.keys())
    val_lists = [picks[a] for a in axes]

    for combo in product(*val_lists):
        overrides = dict(zip(axes, combo))
        label = "comp:" + ",".join(f"{k}={v}" for k, v in sorted(overrides.items()))
        if label in done:
            continue
        s = run_one(label, overrides)
        append_row(label, overrides, s, header_done); header_done = True
        _print_row(label, s)


def run_phase2_fine():
    """Phase 2 fine grid (queue item 1, 2026-05-02 session).

    Walks MIN_RANGE x SL_FRAC x MFE_TRAIL_FRAC around the chosen winner
    (MR=0.20, SL=0.80, MFE=0.15). 3 x 3 x 5 = 45 cells. Resumable: cells
    already in build/usdjpy_trail_sweep_results.csv are skipped.

    Goal: confirm the 1-D plateau (cells #1-#3 of the prior leaderboard)
    extends in 2-D, and look for a sharper interior optimum. If the
    full-period PnL surface has a clean ridge around MR=0.20 / MFE in
    [0.10, 0.20], the OOS edge is more likely to be real than over-fit.
    If instead the surface is bumpy with isolated peaks, the prior
    "winner" is suspect.
    """
    done = already_done()
    header_done = RESULTS_CSV.exists() and RESULTS_CSV.stat().st_size > 0

    mrs  = PHASE2_FINE_GRID["MIN_RANGE"]
    sls  = PHASE2_FINE_GRID["SL_FRAC"]
    mfes = PHASE2_FINE_GRID["MFE_TRAIL_FRAC"]

    cells = list(product(mrs, sls, mfes))
    n_total = len(cells)
    print(f"[phase2_fine] {n_total} cells: "
          f"MIN_RANGE={mrs} x SL_FRAC={sls} x MFE_TRAIL_FRAC={mfes}")

    n_done = n_run = 0
    t0 = time.time()
    for mr, sl, mfe in cells:
        label = f"MR{mr:.2f}_SL{sl:.2f}_MFE{mfe:.2f}"
        overrides = {
            "MIN_RANGE":      mr,
            "SL_FRAC":        sl,
            "MFE_TRAIL_FRAC": mfe,
        }
        if label in done:
            n_done += 1
            continue
        s = run_one(label, overrides)
        append_row(label, overrides, s, header_done); header_done = True
        _print_row(label, s)
        n_run += 1

    elapsed = time.time() - t0
    print(f"[phase2_fine] done. ran {n_run}, skipped {n_done}, "
          f"elapsed {elapsed:.1f}s ({elapsed/max(n_run,1):.1f}s/cell)")


def run_phase3():
    """Sweep STRUCTURE_LOOKBACK on the best composite."""
    rows = _read_results()
    comps = [r for r in rows if r["label"].startswith("comp:")]
    if not comps:
        print("[phase3] no Phase 2 composites; run phase2 first", file=sys.stderr)
        sys.exit(1)
    comps.sort(key=lambda r: r["net_pnl"], reverse=True)
    best = comps[0]
    base_overrides = {}
    for kv in best["overrides"].split(";"):
        if not kv:
            continue
        k, v = kv.split("=", 1)
        base_overrides[k] = float(v) if "." in v else int(v)
    print(f"[phase3] best composite: {best['label']}  PnL=${best['net_pnl']:.2f}")
    print(f"[phase3] anchoring on overrides: {base_overrides}")

    done = already_done()
    header_done = RESULTS_CSV.exists() and RESULTS_CSV.stat().st_size > 0
    for sl in PHASE3_LOOKBACKS:
        overrides = {**base_overrides, "STRUCTURE_LOOKBACK": sl}
        label = f"sl:{sl}|" + ",".join(f"{k}={v}" for k, v in sorted(base_overrides.items()))
        if label in done:
            continue
        s = run_one(label, overrides)
        append_row(label, overrides, s, header_done); header_done = True
        _print_row(label, s)


def run_oos(extra_overrides=None):
    """Train 2025-03..2025-09, Test 2025-10..2026-04 on best config so far."""
    rows = _read_results()
    if extra_overrides:
        overrides = dict(extra_overrides)
        label = "manual_oos"
    else:
        # Pick the best of {comps, sl:*} by PnL.
        candidates = [r for r in rows if r["label"].startswith(("comp:", "sl:"))]
        if not candidates:
            print("[oos] no composites/sl rows; run phase2 first", file=sys.stderr)
            sys.exit(1)
        candidates.sort(key=lambda r: r["net_pnl"], reverse=True)
        best = candidates[0]
        overrides = {}
        for kv in best["overrides"].split(";"):
            if not kv:
                continue
            k, v = kv.split("=", 1)
            overrides[k] = float(v) if "." in v else int(v)
        label = f"oos_of:{best['label']}"
        print(f"[oos] using best: {best['label']}  full-range PnL=${best['net_pnl']:.2f}")

    print(f"[oos] overrides: {overrides}")
    print("-- TRAIN (2025-03..2025-09) --")
    s_tr = run_one("train", overrides, frm="2025-03", to="2025-09")
    _print_row("train", s_tr)
    print("-- TEST  (2025-10..2026-04) --")
    s_te = run_one("test", overrides, frm="2025-10", to="2026-04")
    _print_row("test", s_te)

    header_done = RESULTS_CSV.exists() and RESULTS_CSV.stat().st_size > 0
    append_row(f"{label}/train", overrides, s_tr, header_done); header_done = True
    append_row(f"{label}/test",  overrides, s_te, header_done)


def cmd_all():
    run_phase1()
    run_phase2()
    run_phase3()
    run_oos()


def cmd_leaderboard():
    rows = _read_results()
    rows.sort(key=lambda r: r["net_pnl"], reverse=True)
    print(f"{'rank':4s}  {'label':45s}  {'pnl':>10s}  {'pf':>5s}  {'wr':>5s}  {'avg_w':>6s}  {'avg_l':>7s}")
    for i, r in enumerate(rows[:25], 1):
        print(f"{i:4d}  {r['label'][:45]:45s}  ${r['net_pnl']:8.2f}  "
              f"{r.get('pf',''):>5s}  {r.get('wr',''):>5s}  "
              f"{r.get('avg_win',''):>6s}  {r.get('avg_loss',''):>7s}")


def run_cell(label, overrides_kv, frm="2025-03", to="2026-04"):
    """Run a single cell. Used by bash orchestration when each cell must
    fit inside a 45s call window."""
    overrides = {}
    for kv in overrides_kv:
        if "=" in kv:
            k, v = kv.split("=", 1)
            overrides[k] = float(v) if "." in v else (
                int(v) if v.lstrip("-").isdigit() else float(v)
            )
    done = already_done()
    header_done = RESULTS_CSV.exists() and RESULTS_CSV.stat().st_size > 0
    if label in done:
        print(f"[skip] {label} already in results CSV")
        return
    s = run_one(label, overrides, frm=frm, to=to)
    append_row(label, overrides, s, header_done)
    _print_row(label, s)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "phase1"
    if cmd == "phase1":
        run_phase1()
    elif cmd == "phase2":
        run_phase2()
    elif cmd == "phase2_fine":
        run_phase2_fine()
    elif cmd == "phase3":
        run_phase3()
    elif cmd == "oos":
        extra = {}
        for a in sys.argv[2:]:
            if "=" in a:
                k, v = a.split("=", 1)
                extra[k] = float(v) if "." in v else (int(v) if v.lstrip("-").isdigit() else float(v))
        run_oos(extra or None)
    elif cmd == "cell":
        # cell LABEL KEY=VAL [KEY=VAL ...] [--from YYYY-MM] [--to YYYY-MM]
        if len(sys.argv) < 3:
            print("usage: cell LABEL KEY=VAL ...", file=sys.stderr); sys.exit(2)
        label = sys.argv[2]
        rest = sys.argv[3:]
        frm, to = "2025-03", "2026-04"
        kvs = []
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == "--from" and i + 1 < len(rest):
                frm = rest[i + 1]; i += 2
            elif a == "--to" and i + 1 < len(rest):
                to = rest[i + 1]; i += 2
            else:
                kvs.append(a); i += 1
        run_cell(label, kvs, frm=frm, to=to)
    elif cmd == "leaderboard":
        cmd_leaderboard()
    elif cmd == "all":
        cmd_all()
    else:
        print(f"unknown subcommand: {cmd}", file=sys.stderr)
        print("usage: phase1 | phase2 | phase2_fine | phase3 | oos [KEY=VAL ...] | cell LABEL KEY=VAL ... | leaderboard | all", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
