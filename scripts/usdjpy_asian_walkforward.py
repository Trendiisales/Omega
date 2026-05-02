#!/usr/bin/env python3
"""
usdjpy_asian_walkforward.py
===========================
Rolling 8-fold walk-forward validation of the USDJPY Asian-Open chosen
winner from the 2026-05-02 trail-fix sweep:

    MIN_RANGE       = 0.20
    SL_FRAC         = 0.80
    MFE_TRAIL_FRAC  = 0.15

Why this exists
---------------
The chosen winner was selected against a single fixed train/test split
(train 2025-03..2025-09, test 2025-10..2026-04). One split can hide
regime-shift fragility -- if Q4 2025 happens to suit the strategy and
Q1 2026 doesn't, a fixed 7-month aggregate test will conceal that.
Walk-forward across 8 rolling 1-month test windows answers that
question.

Design (per the walk-forward handoff doc)
-----------------------------------------
Train window: 6 months. Test window: 1 month. Step: 1 month.
14 - 6 = 8 folds:

    fold | train               | test
    -----+---------------------+--------
      1  | 2025-03 .. 2025-08  | 2025-09
      2  | 2025-04 .. 2025-09  | 2025-10
      3  | 2025-05 .. 2025-10  | 2025-11
      4  | 2025-06 .. 2025-11  | 2025-12
      5  | 2025-07 .. 2025-12  | 2026-01
      6  | 2025-08 .. 2026-01  | 2026-02
      7  | 2025-09 .. 2026-02  | 2026-03
      8  | 2025-10 .. 2026-03  | 2026-04

Params are FIXED at the chosen-winner config across all folds. No
per-fold re-fit. The question being asked is "does this single fixed
config hold up under rolling out-of-sample tests" -- not "what is the
best per-window config".

Compile-once
------------
The chosen-winner config does not change between folds, so the harness
binary is constant. We write the engine copy + compile ONCE up front,
then call run_bt 16 times (8 train + 8 test). At ~7 s/run that's ~2 min
total instead of ~3 min if we recompiled each call.

Resumability
------------
Per-fold rows are appended to build/walkforward_results.csv. On rerun,
already-completed (fold, phase) pairs are skipped, so the script can
be invoked in chunks if the calling environment has a wallclock limit.

The 16 walk-forward runs do NOT pollute build/usdjpy_trail_sweep_results.csv.
Their dedicated CSV has fold/phase columns the trail-sweep CSV does not.

Pre-flight: baseline reproduction check
---------------------------------------
Before the 8 folds, the script re-runs the chosen winner over the full
14 months and compares against the prior-session baseline:

    Net PnL = $317.10, Max DD = $202.51, profitable months = 8/14.

If the numbers don't match within a 1-cent tolerance, the script halts:
that means tick-data alignment is wrong, not the code (handoff
gotcha #3). Use --skip-verify to bypass.

Pass criteria (handoff)
-----------------------
SOFT pass:
    - Walk-forward total test PnL  >= ~$56  (50% of fixed-test $113.38)
    - At most 2 of 8 test months are deeply negative (< -$80)
    - Walk-forward PF >= 1.05

HARD pass (promotable to live, after the 2-week shadow gate):
    - Walk-forward PF >= 1.15
    - At least 5 of 8 test months profitable
    - Worst single test-month DD < $200

HARD fail:
    - Walk-forward PF < 1.0, OR
    - Any single test month with PnL < -$200

CLI
---
    # Default: verify baseline, then run 8 folds (train+test), summarize
    python3 scripts/usdjpy_asian_walkforward.py

    # Skip the baseline-reproduction pre-flight
    python3 scripts/usdjpy_asian_walkforward.py --skip-verify

    # Skip the per-fold train run (test-only, halves wallclock cost)
    python3 scripts/usdjpy_asian_walkforward.py --skip-train

    # Run only a subset of folds (1-based, inclusive). Chunk-friendly.
    python3 scripts/usdjpy_asian_walkforward.py --folds 1-3

    # Don't (re)compile -- assume binary at build/usdjpy_asian_bt is
    # already at chosen-winner config. Use this on chunked re-invocations.
    python3 scripts/usdjpy_asian_walkforward.py --no-compile --folds 4-8

    # Read the existing CSV and print the summary + verdict only
    python3 scripts/usdjpy_asian_walkforward.py --summary-only

    # Wipe build/walkforward_results.csv before running
    python3 scripts/usdjpy_asian_walkforward.py --reset

    # Override the param set (added 2026-05-02 exit sweep). The chosen
    # winner is the default; pass --params to walk-forward a different
    # config. --baseline-* must also be passed if you want the
    # determinism check to apply to the new config.
    python3 scripts/usdjpy_asian_walkforward.py \
        --params SL_FRAC=1.0 TP_RR=0.5 BE_TRIGGER_PTS=0.06 \
        --baseline-pnl 407.60 --baseline-dd 186.14 --baseline-prof-m 9 \
        --csv build/walkforward_results_v2.csv --reset

Tick-data path
--------------
The script defers to scripts/usdjpy_asian_trail_sweep.py for tick-data
path resolution. Set USDJPY_TICKS in the environment before invoking
to override. Inside this sandbox the data is mounted at
/sessions/affectionate-funny-fermi/mnt/USDJPY; locally on the Mac it's
~/Tick/USDJPY.

Project policy
--------------
include/UsdjpyAsianOpenEngine.hpp is OFF-LIMITS. This script only
rewrites backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp via the
trail-sweep module's write_engine_copy(). The production header is
never touched.
"""

import argparse
import csv
import importlib.util
import os
import sys
import time
from pathlib import Path

# -- Paths -------------------------------------------------------------------
REPO       = Path(__file__).resolve().parent.parent
SWEEP_PATH = REPO / "scripts" / "usdjpy_asian_trail_sweep.py"
WF_CSV     = REPO / "build" / "walkforward_results.csv"

# -- Chosen-winner config (fixed across all folds) ---------------------------
PARAMS = {
    "MIN_RANGE":       0.20,
    "SL_FRAC":         0.80,
    "MFE_TRAIL_FRAC":  0.15,
}

# -- Folds (1-based) ---------------------------------------------------------
# (fold_id, train_start_YYYY-MM, train_end_YYYY-MM, test_month_YYYY-MM)
FOLDS = [
    (1, "2025-03", "2025-08", "2025-09"),
    (2, "2025-04", "2025-09", "2025-10"),
    (3, "2025-05", "2025-10", "2025-11"),
    (4, "2025-06", "2025-11", "2025-12"),
    (5, "2025-07", "2025-12", "2026-01"),
    (6, "2025-08", "2026-01", "2026-02"),
    (7, "2025-09", "2026-02", "2026-03"),
    (8, "2025-10", "2026-03", "2026-04"),
]

# -- Baseline expected values (handoff gotcha #3, prior-session figures) -----
BASELINE_PNL    = 317.10
BASELINE_DD     = 202.51
BASELINE_PROF_M = 8
BASELINE_MONTHS = 14
BASELINE_TOL    = 0.01     # exact reproduction expected

# -- Pass-criteria thresholds (handoff) --------------------------------------
SOFT_TOTAL_PNL_MIN    = 56.0    # ~50% of fixed-test $113.38
SOFT_DEEP_NEG_MAX     = 2       # months with PnL < -$80
SOFT_PF_MIN           = 1.05
DEEP_NEG_THRESHOLD    = -80.0
HARD_PF_MIN           = 1.15
HARD_PROF_MONTHS_MIN  = 5
HARD_WORST_DD_MAX     = 200.0
FAIL_PF_MAX           = 1.00
FAIL_WORST_PNL_MAX    = -200.0

# -- CSV schema --------------------------------------------------------------
WF_COLS = [
    "fold", "phase", "frm", "to",
    "trades", "wr", "wins", "losses", "bes",
    "tp_hit", "trail_hit", "sl_hit", "be_hit",
    "avg_win", "avg_loss", "net_pnl", "pf", "max_dd",
    "prof_months", "months",
]


# ---------------------------------------------------------------------------
# Sweep-module import (preserves all path resolution + sed-rewrite logic)
# ---------------------------------------------------------------------------
def import_sweep():
    spec = importlib.util.spec_from_file_location(
        "usdjpy_asian_trail_sweep", SWEEP_PATH
    )
    if spec is None or spec.loader is None:
        print(f"[wf] cannot load {SWEEP_PATH}", file=sys.stderr)
        sys.exit(1)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# CSV helpers (dedicated walkforward CSV with fold/phase columns)
# ---------------------------------------------------------------------------
def init_csv():
    WF_CSV.parent.mkdir(parents=True, exist_ok=True)
    if not WF_CSV.exists() or WF_CSV.stat().st_size == 0:
        with WF_CSV.open("w", newline="") as f:
            csv.writer(f).writerow(WF_COLS)


def reset_csv():
    if WF_CSV.exists():
        WF_CSV.unlink()
    init_csv()


def append_csv(fold, phase, frm, to, s):
    row = [fold, phase, frm, to]
    for col in WF_COLS[4:]:
        row.append(s.get(col, ""))
    with WF_CSV.open("a", newline="") as f:
        csv.writer(f).writerow(row)


def already_done():
    """Return set of (fold:int, phase:str) tuples present in WF_CSV."""
    done = set()
    if not WF_CSV.exists():
        return done
    with WF_CSV.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                done.add((int(row["fold"]), row["phase"]))
            except (KeyError, ValueError):
                continue
    return done


def read_csv_rows():
    """Return all rows from WF_CSV as list of dicts (numeric fields parsed)."""
    rows = []
    if not WF_CSV.exists():
        return rows
    with WF_CSV.open() as f:
        for raw in csv.DictReader(f):
            row = dict(raw)
            for k in ("trades", "wins", "losses", "bes", "tp_hit", "trail_hit",
                      "sl_hit", "be_hit", "prof_months", "months"):
                try:
                    row[k] = int(row.get(k) or 0)
                except (TypeError, ValueError):
                    row[k] = 0
            for k in ("wr", "avg_win", "avg_loss", "net_pnl", "pf", "max_dd"):
                try:
                    row[k] = float(row.get(k) or 0.0)
                except (TypeError, ValueError):
                    row[k] = 0.0
            try:
                row["fold"] = int(row.get("fold") or 0)
            except (TypeError, ValueError):
                row["fold"] = 0
            rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# Pre-flight: confirm chosen-winner reproduces the prior-session baseline
# ---------------------------------------------------------------------------
def verify_baseline(sw):
    print("[verify] re-running chosen-winner over full 14 months "
          "(2025-03..2026-04) to confirm deterministic reproduction "
          f"of PnL=${BASELINE_PNL:.2f}, DD=${BASELINE_DD:.2f}, "
          f"months={BASELINE_PROF_M}/{BASELINE_MONTHS}.")
    s = sw.run_bt("WF_baseline_verify", frm="2025-03", to="2026-04")
    sw._print_row("WF_baseline_verify", s)
    pnl = s.get("net_pnl", 0.0)
    dd  = s.get("max_dd", 0.0)
    pm  = s.get("prof_months", 0)
    mo  = s.get("months", 0)
    if (abs(pnl - BASELINE_PNL) > BASELINE_TOL or
            abs(dd - BASELINE_DD) > BASELINE_TOL or
            pm != BASELINE_PROF_M or mo != BASELINE_MONTHS):
        print(
            f"[verify] FAIL: got PnL=${pnl:.2f} (expected ${BASELINE_PNL:.2f}), "
            f"DD=${dd:.2f} (expected ${BASELINE_DD:.2f}), "
            f"months={pm}/{mo} (expected {BASELINE_PROF_M}/{BASELINE_MONTHS}).",
            file=sys.stderr,
        )
        print(
            "[verify] Per handoff gotcha #3 this means tick-data alignment is "
            "wrong, not the code. Aborting before walk-forward. Re-run with "
            "--skip-verify if you intentionally want to proceed.",
            file=sys.stderr,
        )
        sys.exit(2)
    print(f"[verify] OK: PnL=${pnl:.2f}, DD=${dd:.2f}, months={pm}/{mo}")


# ---------------------------------------------------------------------------
# Walk-forward runner
# ---------------------------------------------------------------------------
def parse_fold_range(spec):
    """Parse a --folds spec into a sorted list of fold ids.

    Accepts:  '1-8'  '1-3,5,7'  '4'  ''  None
    """
    if not spec:
        return [f[0] for f in FOLDS]
    out = set()
    for chunk in spec.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        if "-" in chunk:
            a, b = chunk.split("-", 1)
            for k in range(int(a), int(b) + 1):
                out.add(k)
        else:
            out.add(int(chunk))
    valid = {f[0] for f in FOLDS}
    bad = out - valid
    if bad:
        print(f"[wf] bad fold ids: {sorted(bad)} (valid: {sorted(valid)})",
              file=sys.stderr)
        sys.exit(2)
    return sorted(out)


def run_walkforward(sw, fold_ids, skip_train=False):
    init_csv()
    done = already_done()
    print(f"[wf] params: {PARAMS}")
    print(f"[wf] folds to run: {fold_ids}")
    if done:
        print(f"[wf] {len(done)} (fold,phase) rows already in CSV; skipping those")

    fold_map = {f[0]: f for f in FOLDS}
    n_runs = 0
    t0 = time.time()
    for fid in fold_ids:
        _, tr_start, tr_end, te_month = fold_map[fid]
        print(f"\n-- fold {fid}: train {tr_start}..{tr_end}, "
              f"test {te_month} --")

        if not skip_train and (fid, "train") not in done:
            label_tr = f"WF_fold{fid}_train"
            s_tr = sw.run_bt(label_tr, frm=tr_start, to=tr_end)
            sw._print_row(label_tr, s_tr)
            append_csv(fid, "train", tr_start, tr_end, s_tr)
            n_runs += 1
        elif skip_train:
            print(f"   (train skipped via --skip-train)")
        else:
            print(f"   (train already in CSV; skipping)")

        if (fid, "test") not in done:
            label_te = f"WF_fold{fid}_test"
            s_te = sw.run_bt(label_te, frm=te_month, to=te_month)
            sw._print_row(label_te, s_te)
            append_csv(fid, "test", te_month, te_month, s_te)
            n_runs += 1
        else:
            print(f"   (test already in CSV; skipping)")

    elapsed = time.time() - t0
    avg = elapsed / n_runs if n_runs else 0.0
    print(f"\n[wf] {n_runs} runs in {elapsed:.1f}s ({avg:.1f}s/run)")


# ---------------------------------------------------------------------------
# Summary + verdict
# ---------------------------------------------------------------------------
def summarize():
    rows = read_csv_rows()
    test_rows = sorted(
        [r for r in rows if r.get("phase") == "test"], key=lambda r: r["fold"]
    )
    train_rows = sorted(
        [r for r in rows if r.get("phase") == "train"], key=lambda r: r["fold"]
    )

    if not test_rows:
        print("[summary] no test rows in CSV; nothing to summarize")
        return

    # Train table
    if train_rows:
        print("\n" + "=" * 88)
        print("WALK-FORWARD TRAIN-WINDOW RESULTS (sanity check)")
        print("=" * 88)
        print(f"{'fold':>4s}  {'train':19s}  {'trades':>6s}  {'wr':>5s}  "
              f"{'pf':>5s}  {'pnl':>9s}  {'dd':>8s}  {'p_mo':>5s}")
        for r in train_rows:
            window = f"{r['frm']}..{r['to']}"
            print(f"{r['fold']:>4d}  {window:19s}  {r['trades']:>6d}  "
                  f"{r['wr']:>5.1f}  {r['pf']:>5.2f}  ${r['net_pnl']:>7.2f}  "
                  f"${r['max_dd']:>6.2f}  "
                  f"{r['prof_months']:>2d}/{r['months']:<2d}")

    # Test table
    print("\n" + "=" * 88)
    print("WALK-FORWARD TEST-MONTH RESULTS (out-of-sample)")
    print("=" * 88)
    print(f"{'fold':>4s}  {'month':9s}  {'trades':>6s}  {'wr':>5s}  "
          f"{'pf':>5s}  {'pnl':>9s}  {'dd':>8s}")
    sum_pnl_test = 0.0
    sum_wins_dollars   = 0.0
    sum_losses_dollars = 0.0
    pos_months         = 0
    deep_neg_months    = 0
    worst_dd           = 0.0
    worst_pnl          = 0.0
    for r in test_rows:
        pnl = r["net_pnl"]
        dd  = r["max_dd"]
        sum_pnl_test       += pnl
        sum_wins_dollars   += r["wins"]   * r["avg_win"]
        sum_losses_dollars += r["losses"] * abs(r["avg_loss"])
        if pnl > 0:
            pos_months += 1
        if pnl < DEEP_NEG_THRESHOLD:
            deep_neg_months += 1
        if dd > worst_dd:
            worst_dd = dd
        if pnl < worst_pnl:
            worst_pnl = pnl
        print(f"{r['fold']:>4d}  {r['frm']:9s}  {r['trades']:>6d}  "
              f"{r['wr']:>5.1f}  {r['pf']:>5.2f}  ${pnl:>7.2f}  ${dd:>6.2f}")

    n = len(test_rows)
    if sum_losses_dollars > 0:
        wf_pf = sum_wins_dollars / sum_losses_dollars
    else:
        wf_pf = float("inf")

    print("-" * 88)
    print(f"WF aggregate over {n} test months:")
    print(f"  total PnL:             ${sum_pnl_test:.2f}")
    print(f"  profitable months:     {pos_months}/{n}")
    print(f"  deeply-neg months:     {deep_neg_months} (threshold "
          f"${DEEP_NEG_THRESHOLD:.0f})")
    print(f"  worst test-month PnL:  ${worst_pnl:.2f}")
    print(f"  worst test-month DD:   ${worst_dd:.2f}")
    print(f"  WF profit factor:      {wf_pf:.2f} "
          f"(${sum_wins_dollars:.2f} / ${sum_losses_dollars:.2f})")

    # Verdict
    soft = (sum_pnl_test    >= SOFT_TOTAL_PNL_MIN
            and deep_neg_months <= SOFT_DEEP_NEG_MAX
            and wf_pf       >= SOFT_PF_MIN)
    hard = (wf_pf           >= HARD_PF_MIN
            and pos_months  >= HARD_PROF_MONTHS_MIN
            and worst_dd    <  HARD_WORST_DD_MAX)
    fail = (wf_pf           <  FAIL_PF_MAX
            or worst_pnl    <  FAIL_WORST_PNL_MAX)

    print()
    print("VERDICT:")
    print(f"  hard_pass = {hard}   "
          f"(PF>={HARD_PF_MIN}, prof_months>={HARD_PROF_MONTHS_MIN}, "
          f"worst_DD<${HARD_WORST_DD_MAX:.0f})")
    print(f"  soft_pass = {soft}   "
          f"(PnL>=${SOFT_TOTAL_PNL_MIN:.0f}, deep_neg<={SOFT_DEEP_NEG_MAX}, "
          f"PF>={SOFT_PF_MIN})")
    print(f"  hard_fail = {fail}   "
          f"(PF<{FAIL_PF_MAX} OR worst_PnL<${FAIL_WORST_PNL_MAX:.0f})")
    print()
    if hard:
        print("  --> HARD PASS: chosen winner survives walk-forward; "
              "promotable subject to the 2-week shadow gate.")
    elif fail:
        print("  --> HARD FAIL: chosen winner does not survive walk-forward; "
              "do not promote.")
    elif soft:
        print("  --> SOFT PASS: chosen winner holds up under fixed-config "
              "rolling test.")
    else:
        print("  --> MARGINAL: between soft pass and hard fail; "
              "judgement call.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Rolling 8-fold walk-forward validation of the USDJPY "
                    "Asian-Open chosen-winner config.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--skip-verify", action="store_true",
        help="Skip the baseline-reproduction pre-flight.",
    )
    ap.add_argument(
        "--skip-train", action="store_true",
        help="Skip the per-fold train run; only run the 8 test months.",
    )
    ap.add_argument(
        "--folds", default="",
        help="Subset of folds to run, e.g. '1-3' or '1-3,5,7'. "
             "Default: all 8.",
    )
    ap.add_argument(
        "--no-compile", action="store_true",
        help="Skip the up-front engine-rewrite + compile. Use on chunked "
             "re-invocations after an initial setup call.",
    )
    ap.add_argument(
        "--summary-only", action="store_true",
        help="Don't run any backtests. Read the existing CSV and print "
             "the summary + verdict.",
    )
    ap.add_argument(
        "--reset", action="store_true",
        help="Wipe build/walkforward_results.csv before running.",
    )
    ap.add_argument(
        "--params", nargs="+", default=[],
        help="Override PARAMS as KEY=VAL pairs. Layered on top of the "
             "default chosen-winner config. e.g. "
             "--params SL_FRAC=1.0 TP_RR=0.5 BE_TRIGGER_PTS=0.06",
    )
    ap.add_argument(
        "--baseline-pnl", type=float, default=None,
        help="Override the baseline PnL for the determinism pre-flight. "
             "Required (along with --baseline-dd / --baseline-prof-m) "
             "when running --params with a non-default config and not "
             "passing --skip-verify.",
    )
    ap.add_argument(
        "--baseline-dd", type=float, default=None,
        help="Override the baseline max-DD for the determinism pre-flight.",
    )
    ap.add_argument(
        "--baseline-prof-m", type=int, default=None,
        help="Override the baseline profitable-month count for the "
             "determinism pre-flight.",
    )
    ap.add_argument(
        "--csv", default=None,
        help="Override the walkforward CSV path. Useful when running a "
             "second walk-forward against a different param set so its "
             "rows don't collide with the chosen-winner CSV.",
    )
    args = ap.parse_args()

    # --- apply CLI overrides to module-level config ---
    global PARAMS, WF_CSV, BASELINE_PNL, BASELINE_DD, BASELINE_PROF_M
    if args.params:
        for kv in args.params:
            if "=" not in kv:
                print(f"[wf] bad --params token: {kv!r} "
                      f"(expected KEY=VAL)", file=sys.stderr)
                sys.exit(2)
            k, v = kv.split("=", 1)
            try:
                PARAMS[k] = int(v) if v.lstrip("-").isdigit() else float(v)
            except ValueError:
                PARAMS[k] = v
        print(f"[wf] PARAMS after --params overrides: {PARAMS}")
    if args.csv:
        WF_CSV = Path(args.csv)
        print(f"[wf] CSV path overridden to {WF_CSV}")
    if args.baseline_pnl is not None:
        BASELINE_PNL = args.baseline_pnl
    if args.baseline_dd is not None:
        BASELINE_DD = args.baseline_dd
    if args.baseline_prof_m is not None:
        BASELINE_PROF_M = args.baseline_prof_m

    if args.reset:
        print(f"[wf] resetting {WF_CSV}")
        reset_csv()

    if args.summary_only:
        summarize()
        return

    sw = import_sweep()
    print(f"[wf] using TICKS = {sw.TICKS}")
    if not os.path.isdir(sw.TICKS):
        print(f"[wf] FATAL: tick directory does not exist: {sw.TICKS}",
              file=sys.stderr)
        print(f"[wf] set USDJPY_TICKS in the environment to point at "
              f"the USDJPY tick-data folder.", file=sys.stderr)
        sys.exit(2)

    if not args.no_compile:
        print(f"[wf] writing engine copy + compiling (one-time, "
              f"~5 s)")
        sw.write_engine_copy(PARAMS)
        sw.compile_bt()
    else:
        print(f"[wf] skipping engine rewrite + compile (--no-compile)")

    if not args.skip_verify:
        verify_baseline(sw)

    fold_ids = parse_fold_range(args.folds)
    run_walkforward(sw, fold_ids, skip_train=args.skip_train)
    summarize()


if __name__ == "__main__":
    main()
