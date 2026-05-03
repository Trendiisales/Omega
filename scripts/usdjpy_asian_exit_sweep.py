#!/usr/bin/env python3
"""
usdjpy_asian_exit_sweep.py
==========================
Sweep the unswept exit/sizing axes on top of the 2026-05-02 trail-fix
chosen winner.

Anchor (chosen winner from prior session):
    MIN_RANGE       = 0.20
    SL_FRAC         = 0.80
    MFE_TRAIL_FRAC  = 0.15
    (everything else from the trail-sweep ANCHOR dict)

Phases (each phase anchors on the prior phase's winner by net_pnl):

    PHASE A — TP_RR
        currently 2.0 (TP fires 1 / 278 trades = 0.4%)
        sweep:  {0.3, 0.5, 0.7, 1.0, 1.5, 2.0}
        rationale: at 80% MFE-capture, TP at ~p75 MFE (~9 pips, RR≈0.45)
        could lock more winners early and reduce trail-clip risk.

    PHASE B — SL_FRAC
        currently 0.80 (avg_loss ≈ $26.32 = ~19.8 pips at lot 0.20)
        sweep: {0.40, 0.50, 0.60, 0.70, 0.75, 0.80, 0.85, 0.90, 1.00}
        rationale: fine grid only covered 0.75-0.85; tightening could
        shrink avg_loss meaningfully.

    PHASE C — BE_TRIGGER_PTS
        currently 0.06 (6 pips MFE arms BE-lock)
        sweep: {0.03, 0.04, 0.06, 0.08, 0.10, 0.15, 0.20, 0.30}
        rationale: never swept. Tighter BE could shield more trades
        from full-SL hits at the cost of converting some marginal
        winners to BE.

    PHASE D — LOT_MAX (informational only — no parameter promotion)
        currently 0.20.
        sweep: {0.05, 0.07, 0.10, 0.15, 0.20}
        rationale: PF is invariant to LOT_MAX, but DD scales with it.
        Sweep gives a clean DD-vs-LOT curve to pick a risk-appropriate
        size for live promotion. We do NOT use this to pick a "winner";
        keep LOT_MAX = 0.20 as the standard sweep size.

Output
------
    build/usdjpy_exit_sweep_results.csv
    (label, overrides, full set of run metrics; resumable)

CLI
---
    python3 scripts/usdjpy_asian_exit_sweep.py phaseA
    python3 scripts/usdjpy_asian_exit_sweep.py phaseB        # uses A best
    python3 scripts/usdjpy_asian_exit_sweep.py phaseC        # uses A+B best
    python3 scripts/usdjpy_asian_exit_sweep.py phaseD        # info-only
    python3 scripts/usdjpy_asian_exit_sweep.py all
    python3 scripts/usdjpy_asian_exit_sweep.py oos LABEL     # train/test split on a labelled cell
    python3 scripts/usdjpy_asian_exit_sweep.py leaderboard

The chosen production-engine header in include/ is never modified.
Only the backtest copy in backtest/usdjpy_bt/ is sed-rewritten via
scripts/usdjpy_asian_trail_sweep.py's write_engine_copy().
"""

import csv
import importlib.util
import os
import sys
import time
from pathlib import Path

REPO       = Path(__file__).resolve().parent.parent
SWEEP_PATH = REPO / "scripts" / "usdjpy_asian_trail_sweep.py"
RESULTS    = REPO / "build" / "usdjpy_exit_sweep_results.csv"

# Chosen-winner anchor (overrides ANCHOR in trail_sweep)
ANCHOR_OVERRIDE = {
    "MIN_RANGE":       0.20,
    "SL_FRAC":         0.80,
    "MFE_TRAIL_FRAC":  0.15,
}

PHASE_A_TP_RR = [0.3, 0.5, 0.7, 1.0, 1.5, 2.0]
PHASE_B_SL_FRAC = [0.40, 0.50, 0.60, 0.70, 0.75, 0.80, 0.85, 0.90, 1.00]
PHASE_C_BE = [0.03, 0.04, 0.06, 0.08, 0.10, 0.15, 0.20, 0.30]
PHASE_D_LOT = [0.05, 0.07, 0.10, 0.15, 0.20]


# ---------------------------------------------------------------------------
def import_sweep():
    spec = importlib.util.spec_from_file_location(
        "usdjpy_asian_trail_sweep", SWEEP_PATH
    )
    if spec is None or spec.loader is None:
        print(f"[exit_sweep] cannot load {SWEEP_PATH}", file=sys.stderr)
        sys.exit(1)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
COLS = ["label", "overrides", "trades", "wr", "wins", "losses", "bes",
        "tp_hit", "trail_hit", "sl_hit", "be_hit",
        "avg_win", "avg_loss", "net_pnl", "pf", "max_dd",
        "prof_months", "months"]


def init_results():
    RESULTS.parent.mkdir(parents=True, exist_ok=True)
    if not RESULTS.exists() or RESULTS.stat().st_size == 0:
        with RESULTS.open("w", newline="") as f:
            csv.writer(f).writerow(COLS)


def append_result(label, overrides, s):
    row = [label,
           ";".join(f"{k}={v}" for k, v in overrides.items())]
    for col in COLS[2:]:
        row.append(s.get(col, ""))
    with RESULTS.open("a", newline="") as f:
        csv.writer(f).writerow(row)


def already_done():
    done = set()
    if not RESULTS.exists():
        return done
    with RESULTS.open() as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if row:
                done.add(row[0])
    return done


def read_results():
    rows = []
    if not RESULTS.exists():
        return rows
    with RESULTS.open() as f:
        for row in csv.DictReader(f):
            try:
                row["net_pnl"] = float(row.get("net_pnl") or 0)
            except ValueError:
                row["net_pnl"] = 0
            try:
                row["pf"] = float(row.get("pf") or 0)
            except ValueError:
                row["pf"] = 0
            try:
                row["max_dd"] = float(row.get("max_dd") or 0)
            except ValueError:
                row["max_dd"] = 0
            try:
                row["prof_months"] = int(row.get("prof_months") or 0)
            except ValueError:
                row["prof_months"] = 0
            rows.append(row)
    return rows


# ---------------------------------------------------------------------------
def run_cell(sw, label, overrides):
    """Compile + run one cell. overrides layered on top of ANCHOR_OVERRIDE."""
    full = {**ANCHOR_OVERRIDE, **overrides}
    sw.write_engine_copy(full)
    sw.compile_bt()
    s = sw.run_bt(label)
    sw._print_row(label, s)
    return s


def sweep_axis(sw, axis, values, base_overrides, prefix):
    """Sweep a single axis on top of base_overrides; append + print each cell."""
    init_results()
    done = already_done()
    print(f"\n[{prefix}] sweeping {axis} over {values}")
    print(f"[{prefix}] base overrides: {base_overrides}")
    t0 = time.time()
    n_run = n_skip = 0
    for v in values:
        label = f"{prefix}_{axis}={v}"
        if label in done:
            print(f"[{prefix}] skipping {label} (already in CSV)")
            n_skip += 1
            continue
        overrides = {**base_overrides, axis: v}
        s = run_cell(sw, label, overrides)
        append_result(label, overrides, s)
        n_run += 1
    elapsed = time.time() - t0
    print(f"[{prefix}] ran {n_run}, skipped {n_skip}, "
          f"elapsed {elapsed:.1f}s")


def best_of(prefix):
    """Return (label, overrides_dict, metric_dict) of best by net_pnl among
    rows whose label starts with prefix."""
    rows = [r for r in read_results() if r["label"].startswith(prefix)]
    if not rows:
        return None
    rows.sort(key=lambda r: r["net_pnl"], reverse=True)
    best = rows[0]
    overrides = {}
    for kv in (best.get("overrides") or "").split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            overrides[k] = float(v) if "." in v else (
                int(v) if v.lstrip("-").isdigit() else float(v)
            )
    return best["label"], overrides, best


# ---------------------------------------------------------------------------
def cmd_phaseA(sw):
    sweep_axis(sw, "TP_RR", PHASE_A_TP_RR, dict(ANCHOR_OVERRIDE),
               prefix="PHA")


def cmd_phaseB(sw):
    a = best_of("PHA_")
    if a is None:
        print("[phaseB] no Phase A results", file=sys.stderr); sys.exit(1)
    label, overrides, _ = a
    print(f"[phaseB] anchoring on Phase A best: {label} "
          f"(overrides {overrides})")
    sweep_axis(sw, "SL_FRAC", PHASE_B_SL_FRAC, overrides, prefix="PHB")


def cmd_phaseC(sw):
    b = best_of("PHB_")
    if b is None:
        print("[phaseC] no Phase B results", file=sys.stderr); sys.exit(1)
    label, overrides, _ = b
    print(f"[phaseC] anchoring on Phase B best: {label} "
          f"(overrides {overrides})")
    sweep_axis(sw, "BE_TRIGGER_PTS", PHASE_C_BE, overrides, prefix="PHC")


def cmd_phaseD(sw):
    c = best_of("PHC_")
    if c is None:
        print("[phaseD] no Phase C results", file=sys.stderr); sys.exit(1)
    label, overrides, _ = c
    print(f"[phaseD] anchoring on Phase C best: {label} "
          f"(overrides {overrides})")
    sweep_axis(sw, "LOT_MAX", PHASE_D_LOT, overrides, prefix="PHD")


def cmd_all(sw):
    cmd_phaseA(sw)
    cmd_phaseB(sw)
    cmd_phaseC(sw)
    cmd_phaseD(sw)


def cmd_leaderboard():
    rows = read_results()
    rows.sort(key=lambda r: r["net_pnl"], reverse=True)
    print(f"{'rank':>4s}  {'label':40s}  {'pnl':>9s}  {'pf':>5s}  "
          f"{'wr':>5s}  {'p_mo':>5s}  {'dd':>9s}")
    for i, r in enumerate(rows[:25], 1):
        print(f"{i:>4d}  {r['label'][:40]:40s}  ${r['net_pnl']:>7.2f}  "
              f"{r.get('pf','')[:5]:>5s}  {r.get('wr','')[:5]:>5s}  "
              f"{r.get('prof_months',0):>2d}/{r.get('months',0):<2}  "
              f"${r['max_dd']:>7.2f}")


def cmd_oos(sw, label_substr):
    """Run train/test split on the BEST cell whose label contains label_substr."""
    rows = [r for r in read_results() if label_substr in r["label"]]
    if not rows:
        print(f"[oos] no cells matching '{label_substr}'", file=sys.stderr)
        sys.exit(1)
    rows.sort(key=lambda r: r["net_pnl"], reverse=True)
    best = rows[0]
    overrides = {}
    for kv in (best.get("overrides") or "").split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            overrides[k] = float(v) if "." in v else (
                int(v) if v.lstrip("-").isdigit() else float(v)
            )
    print(f"[oos] best matching cell: {best['label']} (full PnL ${best['net_pnl']:.2f})")
    print(f"[oos] overrides: {overrides}")
    sw.write_engine_copy(overrides)
    sw.compile_bt()
    print("-- TRAIN (2025-03..2025-09) --")
    s_tr = sw.run_bt("EXIT_train", frm="2025-03", to="2025-09")
    sw._print_row("EXIT_train", s_tr)
    print("-- TEST  (2025-10..2026-04) --")
    s_te = sw.run_bt("EXIT_test", frm="2025-10", to="2026-04")
    sw._print_row("EXIT_test", s_te)
    init_results()
    append_result(f"EXIT_oos:{best['label']}/train", overrides, s_tr)
    append_result(f"EXIT_oos:{best['label']}/test",  overrides, s_te)


# ---------------------------------------------------------------------------
def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "all"
    if cmd == "leaderboard":
        cmd_leaderboard()
        return
    sw = import_sweep()
    print(f"[exit_sweep] using TICKS = {sw.TICKS}")
    if not os.path.isdir(sw.TICKS):
        print(f"[exit_sweep] FATAL: tick dir missing: {sw.TICKS}",
              file=sys.stderr)
        sys.exit(2)
    if cmd == "phaseA":   cmd_phaseA(sw)
    elif cmd == "phaseB": cmd_phaseB(sw)
    elif cmd == "phaseC": cmd_phaseC(sw)
    elif cmd == "phaseD": cmd_phaseD(sw)
    elif cmd == "all":    cmd_all(sw)
    elif cmd == "oos":
        if len(sys.argv) < 3:
            print("usage: oos LABEL_SUBSTR", file=sys.stderr); sys.exit(2)
        cmd_oos(sw, sys.argv[2])
    else:
        print(f"unknown subcommand: {cmd}", file=sys.stderr)
        print("usage: phaseA | phaseB | phaseC | phaseD | all | "
              "oos LABEL_SUBSTR | leaderboard", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
