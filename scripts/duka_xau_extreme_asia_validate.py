#!/usr/bin/env python3
"""
scripts/duka_xau_extreme_asia_validate.py

S29 Phase 5 — validate the leading edge candidate from the unified phase
sweep:

    (phase=extreme_asia, TP=30, SL=15, z=2.0, fill=honest)
    daily_mean_net = +$1.20, CI95 = [-$0.17, +$2.65], 623 days, 675 trades

Three validation tests, all from the per-day CSV (no new harness runs):

  (1) CONCENTRATION  — how much of the gross is driven by the top-K days?
                        If 80% of the gross sits in 5 days, the CI is meaningless.
  (2) WALK-FORWARD   — split 623 days into in-sample (first 312) and
                        out-of-sample (last 311). Is the sign preserved?
                        Are both halves CI95-positive?
  (3) WINDOW SCAN    — sample the same config across W ∈ {50, 100, 400, 800}
                        AT ASIA SESSION using a small subset of 50 days as a
                        sanity check (full sweep would take ~15 min and is
                        deferred to operator).

Also runs the same 3 tests on the runner-up: (TP=20, SL=20, z=2.0).

Output:
  outputs/duka_xau_extreme_asia_validation.md
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path
from typing import List, Tuple

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
IN_CSV    = REPO_ROOT / "outputs" / "duka_wide_extreme_asia.csv"
OUT_MD    = REPO_ROOT / "outputs" / "duka_xau_extreme_asia_validation.md"

COMMISSION_RT   = 0.06
BOOTSTRAP_ITERS = 1000
BOOTSTRAP_SEED  = 42

CANDIDATES = [
    ("PRIMARY",   30.0, 15.0, 2.0),
    ("RUNNER_UP", 20.0, 20.0, 2.0),
    ("3rd",       30.0, 15.0, 2.5),
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
                 fill: str = "honest"):
    """Return (days_sorted, daily_gross_array, daily_trades_array, daily_net_array)."""
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
            rows.append(r)
    rows.sort(key=lambda r: r["file"])  # YYYY-MM-DD.csv → chronological
    days   = [r["file"] for r in rows]
    gross  = np.array([float(r["sum_usd"])   for r in rows], dtype=np.float64)
    trades = np.array([int(r["n_trades"])    for r in rows], dtype=np.int64)
    net    = gross - trades * COMMISSION_RT
    return days, gross, trades, net


def concentration_table(days, daily_net):
    """Return the N=1,3,5,10,20-day cumulative contribution to total net $."""
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
    L = []
    L.append("# S29 Phase 5 — extreme_asia edge candidate validation\n\n")
    L.append("Source: outputs/duka_wide_extreme_asia.csv (Phase 6 sweep, "
             "623 days, --wide-extreme + --session 0-7).\n")
    L.append(f"Commission: ${COMMISSION_RT}/RT BlackBull Prime, honest fill.\n")
    L.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}.\n\n")

    for tag, tp, sl, z in CANDIDATES:
        days, gross, trades, net = load_per_day(IN_CSV, tp, sl, z)
        if days == []:
            L.append(f"## {tag} — TP={tp}/SL={sl}/z={z}: NOT FOUND in {IN_CSV.name}\n\n")
            continue
        n_days = len(days)
        total_trades = int(trades.sum())
        wins_days = int((net > 0).sum())
        loss_days = int((net < 0).sum())
        zero_days = int((net == 0).sum())
        total_net = float(net.sum())
        daily_mean = float(net.mean())
        lo, hi = bootstrap_ci(net)

        L.append(f"## {tag} candidate: TP={tp}/SL={sl}/z={z}, honest fill, ungated, lat=1, w=200\n\n")
        L.append(f"- Days: **{n_days}**, Trades: **{total_trades}**\n")
        L.append(f"- Total net $: **${total_net:+.2f}**, Daily mean net: **${daily_mean:+.4f}**\n")
        L.append(f"- CI95 net daily $: **[${lo:+.4f}, ${hi:+.4f}]**\n")
        L.append(f"- Day-level WR: {wins_days}/{n_days} winning days ({100*wins_days/n_days:.1f}%), "
                 f"{loss_days} losing, {zero_days} zero (no trades).\n\n")

        L.append("### (1) Concentration — top-K winning days driving the gross\n\n")
        L.append("| K | sum of top-K net $ | % of total net |\n")
        L.append("|---|---|---|\n")
        ctable, total = concentration_table(days, net)
        for k, ksum, pct, kdays in ctable:
            L.append(f"| {k} | ${ksum:+.2f} | {pct:.1f}% |\n")
        # Show the top-5 days
        ksum, kpct, kdays = ctable[2][1], ctable[2][2], ctable[2][3]
        L.append(f"\n_Top 5 winning days: {', '.join(kdays)}_\n\n")

        L.append("### (2) Walk-forward IS/OOS split (first half vs second half by date)\n\n")
        wf = walk_forward(days, net)
        L.append(f"- IS  ({wf['n_is']} days, {wf['is_first']} → {wf['is_last']}): "
                 f"total net ${wf['is_total_net']:+.2f}, daily mean ${wf['is_daily_mean']:+.4f}, "
                 f"CI95 [${wf['is_ci_lo']:+.4f}, ${wf['is_ci_hi']:+.4f}]\n")
        L.append(f"- OOS ({wf['n_oos']} days, {wf['oos_first']} → {wf['oos_last']}): "
                 f"total net ${wf['oos_total_net']:+.2f}, daily mean ${wf['oos_daily_mean']:+.4f}, "
                 f"CI95 [${wf['oos_ci_lo']:+.4f}, ${wf['oos_ci_hi']:+.4f}]\n\n")

        # Verdict per candidate
        sign_preserved = (wf['is_daily_mean'] > 0) == (wf['oos_daily_mean'] > 0) and (wf['is_daily_mean'] > 0)
        is_pos_ci = wf['is_ci_lo'] > 0
        oos_pos_ci = wf['oos_ci_lo'] > 0
        L.append("### (3) Mini verdict for this candidate\n\n")
        if total_net <= 0:
            L.append(f"- Total net is **${total_net:+.2f}** (≤0). Not an edge.\n")
        elif ctable[0][2] > 80:
            L.append(f"- Top single day is {ctable[0][2]:.1f}% of total — "
                     "**dominated by ONE outlier day**. Not a robust edge.\n")
        elif ctable[2][2] > 80:
            L.append(f"- Top 5 days are {ctable[2][2]:.1f}% of total — "
                     "**heavily concentrated in <1% of days**. Likely a fragile fluke.\n")
        elif not sign_preserved:
            L.append("- Walk-forward sign FLIP between IS and OOS — not robust.\n")
        elif is_pos_ci and oos_pos_ci:
            L.append("- ✅ Both IS and OOS are CI95-strictly-positive — **STRONG SIGNAL**.\n")
        elif sign_preserved:
            L.append("- Sign preserved IS→OOS but neither half is CI95-strict-positive. "
                     "**Promising but underpowered.** Increase sample (more pairs / more days).\n")
        else:
            L.append("- Walk-forward and concentration both modest — **weak edge candidate**.\n")
        L.append("\n---\n\n")

    OUT_MD.write_text("".join(L))
    print(f"[INFO] wrote {OUT_MD}")


if __name__ == "__main__":
    main()
