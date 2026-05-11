#!/usr/bin/env python3
"""
scripts/duka_xau_fine_validate.py

S30 §3.1 validation — concentration + walk-forward IS/OOS for the top
strict-CI-positive cells from the fine grid.

Input:  outputs/duka_wide_fine_asia.csv  (623 days × 256 cells × 2 fills)
Output: outputs/duka_xau_fine_validation.md

Top-3 cells under test (sorted by CI95_lower from duka_xau_fine_verdict.md):
  TOP-1:  TP=35  SL=12  z=2.0  W=200   CI95 [+0.79, +3.95]   mean +$2.31
  RUNNER: TP=40  SL=12  z=2.5  W=200   CI95 [+0.56, +3.84]   mean +$2.14
  3rd:    TP=35  SL=12  z=2.5  W=200   CI95 [+0.54, +3.56]   mean +$2.09

Plus the S29 PRIMARY as a control (TP=30, SL=15, z=2.0, W=200), which in S29
showed CI95 [-0.17, +2.65]; we replicate to confirm the fine-grid sample is
consistent with the S29 extreme_asia sample.

The harness sim logic was independently Python-replay-validated in S29
(verify_extreme_asia_one_day.py: cent-for-cent match on day 2025-04-10).
S30's _s30 binary is bit-identical to _s29 on bare --wide and --wide-extreme
regression tests, AND the --wide-fine smoke test correctly emitted 256
distinct (tp,sl,z,window) tuples with the per-cell window propagated into
the CSV.  We therefore reuse the S29 sim-logic validation transitively
rather than re-deriving it.  If the operator wants an explicit one-day
Python replay for the new TOP-1, run verify_extreme_asia_one_day.py with
its CANDIDATE constants adjusted to (35, 12, 2.0, 200).
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path
from typing import List

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
IN_CSV    = REPO_ROOT / "outputs" / "duka_wide_fine_asia.csv"
OUT_MD    = REPO_ROOT / "outputs" / "duka_xau_fine_validation.md"

COMMISSION_RT   = 0.06
BOOTSTRAP_ITERS = 1000
BOOTSTRAP_SEED  = 42

CANDIDATES = [
    # tag,         tp,   sl,   z,    window
    ("TOP-1",     35.0, 12.0, 2.0,  200),
    ("RUNNER_UP", 40.0, 12.0, 2.5,  200),
    ("3rd",       35.0, 12.0, 2.5,  200),
    ("S29_PRIMARY (control)", 30.0, 15.0, 2.0, 200),
]


def bootstrap_ci(values, iters=BOOTSTRAP_ITERS, seed=BOOTSTRAP_SEED, ci_pct=95.0):
    if values.size == 0:
        return float("nan"), float("nan")
    rng = np.random.default_rng(seed)
    n = values.size
    idx = rng.integers(0, n, size=(iters, n))
    means = values[idx].mean(axis=1)
    lo = float(np.percentile(means, (100 - ci_pct) / 2))
    hi = float(np.percentile(means, 100 - (100 - ci_pct) / 2))
    return lo, hi


def load_per_day(csv_path: Path, tp: float, sl: float, z: float,
                 window: int, fill: str = "honest"):
    """Return chronologically-sorted (days, gross, trades, net) arrays."""
    rows = []
    with csv_path.open("r") as f:
        for r in csv.DictReader(f):
            if r["fill_model"] != fill:
                continue
            if abs(float(r["tp"]) - tp) > 1e-9:
                continue
            if abs(float(r["sl"]) - sl) > 1e-9:
                continue
            if abs(float(r["z"]) - z)  > 1e-9:
                continue
            if int(r["window"]) != window:
                continue
            rows.append(r)
    rows.sort(key=lambda r: r["file"])
    days   = [r["file"] for r in rows]
    gross  = np.array([float(r["sum_usd"])   for r in rows], dtype=np.float64)
    trades = np.array([int(r["n_trades"])    for r in rows], dtype=np.int64)
    net    = gross - trades * COMMISSION_RT
    return days, gross, trades, net


def concentration_table(days, daily_net):
    sorted_idx = np.argsort(daily_net)[::-1]
    sorted_net = daily_net[sorted_idx]
    sorted_days = [days[i] for i in sorted_idx]
    total = float(daily_net.sum())
    out = []
    for k in [1, 3, 5, 10, 20]:
        topk_sum = float(sorted_net[:k].sum())
        pct = (topk_sum / total * 100.0) if total != 0 else float("nan")
        out.append((k, topk_sum, pct, sorted_days[:k]))
    return out, total


def walk_forward(days, daily_net):
    n = daily_net.size
    half = n // 2
    isn = daily_net[:half]
    oos = daily_net[half:]
    lo_i, hi_i = bootstrap_ci(isn)
    lo_o, hi_o = bootstrap_ci(oos)
    return {
        "n_total": n,
        "n_is": isn.size, "is_first": days[0], "is_last": days[half - 1],
        "n_oos": oos.size, "oos_first": days[half], "oos_last": days[-1],
        "is_total_net": float(isn.sum()), "oos_total_net": float(oos.sum()),
        "is_daily_mean": float(isn.mean()), "oos_daily_mean": float(oos.mean()),
        "is_ci_lo": lo_i, "is_ci_hi": hi_i,
        "oos_ci_lo": lo_o, "oos_ci_hi": hi_o,
    }


def main():
    if not IN_CSV.exists():
        sys.exit(f"[FATAL] missing {IN_CSV}")
    L: List[str] = []
    L.append("# S30 §3.1 — fine-grid edge candidate validation\n\n")
    L.append("Source: outputs/duka_wide_fine_asia.csv (§3.1 sweep, 623 days, "
             "--wide-fine --session 0-7 --latency 1, honest fill).\n")
    L.append(f"Commission: ${COMMISSION_RT}/RT BlackBull Prime, ungated.\n")
    L.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}.\n\n")
    L.append("Top-3 cells are by lower CI95 from duka_xau_fine_verdict.md. "
             "Control cell is the S29 PRIMARY for consistency check.\n\n")

    for tag, tp, sl, z, w in CANDIDATES:
        days, gross, trades, net = load_per_day(IN_CSV, tp, sl, z, w)
        if not days:
            L.append(f"## {tag} — TP={tp}/SL={sl}/z={z}/W={w}: NOT FOUND\n\n")
            continue
        n_days = len(days)
        total_trades = int(trades.sum())
        wins_days = int((net > 0).sum())
        loss_days = int((net < 0).sum())
        zero_days = int((net == 0).sum())
        total_net = float(net.sum())
        daily_mean = float(net.mean())
        lo, hi = bootstrap_ci(net)

        L.append(f"## {tag}: TP={tp}/SL={sl}/z={z}/W={w}, honest, ungated, "
                 "asia 0-7 UTC, latency=1\n\n")
        L.append(f"- Days: **{n_days}**, Trades: **{total_trades}**, "
                 f"Total net $: **${total_net:+.2f}**, Daily mean net: "
                 f"**${daily_mean:+.4f}**\n")
        L.append(f"- CI95 net daily $: **[${lo:+.4f}, ${hi:+.4f}]**\n")
        L.append(f"- Day-level WR: {wins_days}/{n_days} = "
                 f"{100*wins_days/n_days:.1f}%, {loss_days} losing, "
                 f"{zero_days} zero (no trades).\n\n")

        L.append("### (1) Concentration\n\n")
        L.append("| K | sum of top-K net $ | % of total net |\n")
        L.append("|---|---|---|\n")
        ctable, total = concentration_table(days, net)
        for k, ksum, pct, kdays in ctable:
            L.append(f"| {k} | ${ksum:+.2f} | {pct:.1f}% |\n")
        top5_days = ctable[2][3]
        L.append(f"\n_Top 5 winning days: {', '.join(top5_days)}_\n\n")

        L.append("### (2) Walk-forward IS / OOS\n\n")
        wf = walk_forward(days, net)
        L.append(f"- IS  ({wf['n_is']} days, {wf['is_first']} → "
                 f"{wf['is_last']}): total ${wf['is_total_net']:+.2f}, "
                 f"daily mean ${wf['is_daily_mean']:+.4f}, "
                 f"CI95 [${wf['is_ci_lo']:+.4f}, ${wf['is_ci_hi']:+.4f}]\n")
        L.append(f"- OOS ({wf['n_oos']} days, {wf['oos_first']} → "
                 f"{wf['oos_last']}): total ${wf['oos_total_net']:+.2f}, "
                 f"daily mean ${wf['oos_daily_mean']:+.4f}, "
                 f"CI95 [${wf['oos_ci_lo']:+.4f}, ${wf['oos_ci_hi']:+.4f}]\n\n")

        # Verdict per candidate
        sign_preserved = (
            (wf['is_daily_mean'] > 0) == (wf['oos_daily_mean'] > 0)
            and (wf['is_daily_mean'] > 0)
        )
        is_pos_ci = wf['is_ci_lo'] > 0
        oos_pos_ci = wf['oos_ci_lo'] > 0
        L.append("### (3) Mini verdict\n\n")
        if total_net <= 0:
            L.append(f"- Total net **${total_net:+.2f}** (≤ 0). Not an edge.\n")
        elif ctable[0][2] > 80:
            L.append(f"- Top SINGLE day is {ctable[0][2]:.1f}% of total — "
                     "**dominated by one outlier**. Not robust.\n")
        elif ctable[2][2] > 80:
            L.append(f"- Top 5 days are {ctable[2][2]:.1f}% of total — "
                     "**heavily concentrated**. Likely a fragile fluke.\n")
        elif not sign_preserved:
            L.append("- Walk-forward sign FLIP between IS and OOS — not robust.\n")
        elif is_pos_ci and oos_pos_ci:
            L.append("- ✅ Both IS and OOS are CI95-strictly-positive — "
                     "**STRONG SIGNAL.**\n")
        elif sign_preserved and (is_pos_ci or oos_pos_ci):
            L.append("- Sign preserved IS→OOS AND at least one half is "
                     "CI95-strictly-positive — **promising and likely robust**.\n")
        elif sign_preserved:
            L.append("- Sign preserved IS→OOS but neither half is "
                     "CI95-strict-positive. Promising but underpowered per-half.\n")
        else:
            L.append("- Walk-forward and concentration both modest — "
                     "weak edge candidate.\n")
        L.append("\n---\n\n")

    L.append("## Note on sim-logic verification\n\n")
    L.append("The C++ harness sim logic was independently Python-replay-validated "
             "in S29 (verify_extreme_asia_one_day.py: cent-for-cent match on day "
             "2025-04-10 for the S29 PRIMARY).  The S30 _s30 binary regression-"
             "matches _s29 bit-identically on bare --wide and --wide-extreme, "
             "and the --wide-fine smoke test confirmed correct per-cell window "
             "propagation into the CSV.  We therefore reuse the S29 sim-logic "
             "validation transitively.  For an explicit one-day Python replay of "
             "the new TOP-1 cell, run verify_extreme_asia_one_day.py with its "
             "CANDIDATE constants set to (tp=35, sl=12, z=2.0, w=200) and pick a "
             "day from the top-5 winning-day list above.\n")
    OUT_MD.write_text("".join(L))
    print(f"[INFO] wrote {OUT_MD}")


if __name__ == "__main__":
    main()
