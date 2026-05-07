#!/usr/bin/env python3
"""
gbpusd_london_walkforward.py
============================
Rolling 10-fold walk-forward validation for the GBPUSD London-Open engine.

GBP has 16 months of HistData (2025-01..2026-04) vs EUR's 14 months (2025-03
onward), so the GBP fold layout is 6mo train + 1mo test rolling for 10
folds (2025-07 through 2026-04). Same SOFT/HARD/HARD-FAIL verdict logic as
the EURUSD walk-forward.

Folds (1-based)
---------------
    fold | train               | test
    -----+---------------------+--------
      1  | 2025-01 .. 2025-06  | 2025-07
      2  | 2025-02 .. 2025-07  | 2025-08
      3  | 2025-03 .. 2025-08  | 2025-09
      4  | 2025-04 .. 2025-09  | 2025-10
      5  | 2025-05 .. 2025-10  | 2025-11
      6  | 2025-06 .. 2025-11  | 2025-12
      7  | 2025-07 .. 2025-12  | 2026-01
      8  | 2025-08 .. 2026-01  | 2026-02
      9  | 2025-09 .. 2026-02  | 2026-03
     10  | 2025-10 .. 2026-03  | 2026-04

Params are FIXED at the GBP pre-sweep defaults inherited from EUR S56
(MIN_RANGE=0.0012, SL_FRAC=0.80, TP_RR=2.0, MFE_TRAIL_FRAC=0.40).
The first run answers a fundamental question: do the EUR-tuned parameters
work on GBP at all? The audit doc explicitly notes these are PRE-SWEEP
defaults that have never been validated for GBP.

Pre-flight: baseline reproduction check
---------------------------------------
On first run there is no baseline to verify against -- GBP has never been
backtested before. Use --skip-verify on the first run, capture the actual
PnL/DD/profitable-months, and update BASELINE_PNL / BASELINE_DD /
BASELINE_PROF_M.

Pass criteria (calibrated as for EUR):
    SOFT: total test PnL >= $50, deep-neg <= 2, WF PF >= 1.05
    HARD: WF PF >= 1.15, profitable months >= 6/10, worst test-month DD < $200
    FAIL: WF PF < 1.0 OR any single test month PnL < -$200

CLI mirrors scripts/eurusd_london_walkforward.py.

Project policy
--------------
include/GbpusdLondonOpenEngine.hpp is OFF-LIMITS. This script only rewrites
backtest/gbpusd_bt/GbpusdLondonOpenEngine.hpp via the sweep module.
"""

import argparse
import csv
import importlib.util
import os
import sys
import time
from pathlib import Path

REPO       = Path(__file__).resolve().parent.parent
SWEEP_PATH = REPO / "scripts" / "gbpusd_london_sweep.py"
WF_CSV     = REPO / "build" / "gbpusd_walkforward_results.csv"

# Pre-sweep defaults inherited from EUR S56.
PARAMS = {
    "MIN_RANGE":       0.0012,
    "SL_FRAC":         0.80,
    "TP_RR":           2.0,
    "MFE_TRAIL_FRAC":  0.40,
    "BE_TRIGGER_PTS":  0.0006,
    "TRAIL_FRAC":      0.30,
}

# 10 folds, 6mo train + 1mo test, sliding by 1 month.
FOLDS = [
    ( 1, "2025-01", "2025-06", "2025-07"),
    ( 2, "2025-02", "2025-07", "2025-08"),
    ( 3, "2025-03", "2025-08", "2025-09"),
    ( 4, "2025-04", "2025-09", "2025-10"),
    ( 5, "2025-05", "2025-10", "2025-11"),
    ( 6, "2025-06", "2025-11", "2025-12"),
    ( 7, "2025-07", "2025-12", "2026-01"),
    ( 8, "2025-08", "2026-01", "2026-02"),
    ( 9, "2025-09", "2026-02", "2026-03"),
    (10, "2025-10", "2026-03", "2026-04"),
]

# Baseline placeholders -- GBP has no prior reproduction reference.
# After first --skip-verify run, capture and update.
BASELINE_PNL    = 0.0
BASELINE_DD     = 0.0
BASELINE_PROF_M = 0
BASELINE_MONTHS = 16
BASELINE_TOL    = 0.01

SOFT_TOTAL_PNL_MIN    = 50.0
SOFT_DEEP_NEG_MAX     = 2
SOFT_PF_MIN           = 1.05
DEEP_NEG_THRESHOLD    = -80.0
HARD_PF_MIN           = 1.15
HARD_PROF_MONTHS_MIN  = 6   # GBP has 10 folds, scale up from EUR's 5/8
HARD_WORST_DD_MAX     = 200.0
FAIL_PF_MAX           = 1.00
FAIL_WORST_PNL_MAX    = -200.0

WF_COLS = [
    "fold", "phase", "frm", "to",
    "trades", "wr", "wins", "losses", "bes",
    "tp_hit", "trail_hit", "sl_hit", "be_hit",
    "avg_win", "avg_loss", "net_pnl", "pf", "max_dd",
    "prof_months", "months",
]


def import_sweep():
    spec = importlib.util.spec_from_file_location(
        "gbpusd_london_sweep", SWEEP_PATH
    )
    if spec is None or spec.loader is None:
        print(f"[wf] cannot load {SWEEP_PATH}", file=sys.stderr)
        sys.exit(1)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


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


def verify_baseline(sw):
    print("[verify] re-running pre-sweep config over full 16 months "
          "(2025-01..2026-04) to confirm deterministic reproduction "
          f"of PnL=${BASELINE_PNL:.2f}, DD=${BASELINE_DD:.2f}, "
          f"months={BASELINE_PROF_M}/{BASELINE_MONTHS}.")
    s = sw.run_bt("WF_baseline_verify", frm="2025-01", to="2026-04")
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
            "[verify] On first run this is expected -- GBP has never been "
            "backtested before. Re-run with --skip-verify, capture the actual "
            "PnL/DD, and update BASELINE_PNL / BASELINE_DD / BASELINE_PROF_M "
            "in this script.",
            file=sys.stderr,
        )
        sys.exit(2)
    print(f"[verify] OK: PnL=${pnl:.2f}, DD=${dd:.2f}, months={pm}/{mo}")


def parse_fold_range(spec):
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


def main():
    ap = argparse.ArgumentParser(
        description="Rolling 10-fold walk-forward validation of the GBPUSD "
                    "London-Open pre-sweep config.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--skip-verify", action="store_true")
    ap.add_argument("--skip-train", action="store_true")
    ap.add_argument("--folds", default="")
    ap.add_argument("--no-compile", action="store_true")
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--params", nargs="+", default=[])
    ap.add_argument("--baseline-pnl", type=float, default=None)
    ap.add_argument("--baseline-dd", type=float, default=None)
    ap.add_argument("--baseline-prof-m", type=int, default=None)
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    global PARAMS, WF_CSV, BASELINE_PNL, BASELINE_DD, BASELINE_PROF_M
    if args.params:
        for kv in args.params:
            if "=" not in kv:
                print(f"[wf] bad --params token: {kv!r}", file=sys.stderr)
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
        print(f"[wf] set GBPUSD_TICKS in the environment to point at "
              f"the GBPUSD tick-data folder.", file=sys.stderr)
        sys.exit(2)

    if not args.no_compile:
        print(f"[wf] writing engine copy + compiling (one-time, ~5s)")
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
