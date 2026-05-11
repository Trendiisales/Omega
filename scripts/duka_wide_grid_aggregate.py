#!/usr/bin/env python3
"""
scripts/duka_wide_grid_aggregate.py
S28 / S27 §4.2 — aggregate the wide-grid sweep on Dukascopy 2yr XAU.

Inputs:
  outputs/duka_wide_grid.csv     (52 configs × 2 fill_models × 623 days)

For each (tp, sl, z, fill_model):
  - total_trades, total_$, expectancy/trade
  - daily mean and median PnL across the 623 days
  - 1000-iter daily-mean bootstrap CI95 (seed=42)
  - net-of-Prime $0.06/round-trip column

Outputs:
  outputs/duka_wide_grid_summary.csv   (one row per config × fill_model)
  outputs/duka_wide_grid_top10.md      (top-10 by lower CI95 bound, honest fill)
  outputs/duka_wide_grid_verdict.md    (one-page narrative verdict)

Runs in ~10–30 seconds on this hardware.
"""
from __future__ import annotations

import csv
import os
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
IN_CSV = REPO_ROOT / "outputs" / "duka_wide_grid.csv"
OUT_SUMMARY = REPO_ROOT / "outputs" / "duka_wide_grid_summary.csv"
OUT_TOP10 = REPO_ROOT / "outputs" / "duka_wide_grid_top10.md"
OUT_VERDICT = REPO_ROOT / "outputs" / "duka_wide_grid_verdict.md"

COMMISSION_RT = 0.06  # USD per round-trip @ 0.01-lot, BlackBull ECN Prime
BOOTSTRAP_ITERS = 1000
BOOTSTRAP_SEED = 42
CI_PCT = 95.0


def load_rows(path: Path) -> List[dict]:
    if not path.exists():
        sys.exit(f"ERROR: {path} not found")
    with path.open("r") as f:
        reader = csv.DictReader(f)
        return list(reader)


def bootstrap_ci(values: np.ndarray, iters: int, seed: int,
                 ci_pct: float) -> Tuple[float, float]:
    """Bootstrap CI on the MEAN of `values`."""
    if values.size == 0:
        return float("nan"), float("nan")
    rng = np.random.default_rng(seed)
    n = values.size
    idx = rng.integers(0, n, size=(iters, n))
    means = values[idx].mean(axis=1)
    lo = float(np.percentile(means, (100 - ci_pct) / 2))
    hi = float(np.percentile(means, 100 - (100 - ci_pct) / 2))
    return lo, hi


def aggregate(rows: List[dict]) -> List[dict]:
    # Group by (tp, sl, z, fill_model). Each group has one row per day.
    groups: Dict[Tuple[str, str, str, str], List[dict]] = defaultdict(list)
    for r in rows:
        key = (r["tp"], r["sl"], r["z"], r["fill_model"])
        groups[key].append(r)

    results = []
    for (tp, sl, z, fm), grp in groups.items():
        sums = np.array([float(g["sum_usd"]) for g in grp], dtype=np.float64)
        trades = np.array([int(g["n_trades"]) for g in grp], dtype=np.int64)
        wins = np.array([int(g["n_wins"]) for g in grp], dtype=np.int64)
        losses = np.array([int(g["n_losses"]) for g in grp], dtype=np.int64)

        n_days = sums.size
        total_trades = int(trades.sum())
        total_wins = int(wins.sum())
        total_losses = int(losses.sum())
        total_gross = float(sums.sum())
        total_net = total_gross - total_trades * COMMISSION_RT
        wr = (total_wins / total_trades * 100.0) if total_trades else 0.0

        # Daily PnL gross AND net of commission (commission applies per-trade)
        daily_gross = sums
        daily_net = sums - trades * COMMISSION_RT
        mean_gross = float(daily_gross.mean()) if n_days else 0.0
        mean_net = float(daily_net.mean()) if n_days else 0.0
        median_gross = float(np.median(daily_gross)) if n_days else 0.0

        lo_g, hi_g = bootstrap_ci(daily_gross, BOOTSTRAP_ITERS, BOOTSTRAP_SEED,
                                   CI_PCT)
        lo_n, hi_n = bootstrap_ci(daily_net, BOOTSTRAP_ITERS, BOOTSTRAP_SEED,
                                   CI_PCT)

        exp_gross = (total_gross / total_trades) if total_trades else 0.0
        exp_net = (total_net / total_trades) if total_trades else 0.0

        results.append({
            "tp": float(tp), "sl": float(sl), "z": float(z),
            "fill_model": fm,
            "n_days": n_days,
            "total_trades": total_trades,
            "total_wins": total_wins,
            "total_losses": total_losses,
            "wr_pct": round(wr, 2),
            "total_gross_usd": round(total_gross, 2),
            "total_net_prime_usd": round(total_net, 2),
            "exp_gross_per_trade": round(exp_gross, 4),
            "exp_net_per_trade": round(exp_net, 4),
            "daily_mean_gross": round(mean_gross, 4),
            "daily_mean_net": round(mean_net, 4),
            "daily_median_gross": round(median_gross, 4),
            "ci95_lo_gross": round(lo_g, 4),
            "ci95_hi_gross": round(hi_g, 4),
            "ci95_lo_net": round(lo_n, 4),
            "ci95_hi_net": round(hi_n, 4),
        })
    return results


def write_summary(rows: List[dict], path: Path) -> None:
    if not rows:
        return
    cols = list(rows[0].keys())
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def write_top10(rows: List[dict], path: Path) -> None:
    honest = [r for r in rows if r["fill_model"] == "honest"]
    # Rank by lower CI95 bound on net daily mean (most conservative).
    honest.sort(key=lambda r: r["ci95_lo_net"], reverse=True)

    lines = []
    lines.append("# Duka 2yr XAU wide-grid — top 10 by lower CI95(net daily) [honest fill]\n")
    lines.append("Sample: 623 daily files, 2023-09-27 → 2025-09-26.\n")
    lines.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}, CI={CI_PCT}%.\n")
    lines.append(f"Commission: ${COMMISSION_RT:.2f} round-trip (BlackBull ECN Prime, 0.01-lot).\n\n")
    lines.append("| Rank | TP | SL | z | Trades | WR% | Total gross $ | Total net $ | Exp/tr net $ | Daily mean net $ | CI95 net daily $ |\n")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|\n")
    for i, r in enumerate(honest[:10], 1):
        ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
        lines.append(
            f"| {i} | {r['tp']:.1f} | {r['sl']:.1f} | {r['z']:.1f} | "
            f"{r['total_trades']} | {r['wr_pct']:.1f} | "
            f"{r['total_gross_usd']:+.2f} | {r['total_net_prime_usd']:+.2f} | "
            f"{r['exp_net_per_trade']:+.4f} | {r['daily_mean_net']:+.4f} | {ci} |\n"
        )
    lines.append("\n## Worst 5 (for context)\n\n")
    lines.append("| Rank | TP | SL | z | Trades | WR% | Total net $ | Daily mean net $ | CI95 net daily $ |\n")
    lines.append("|---|---|---|---|---|---|---|---|---|\n")
    for i, r in enumerate(honest[-5:], len(honest) - 4):
        ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
        lines.append(
            f"| {i} | {r['tp']:.1f} | {r['sl']:.1f} | {r['z']:.1f} | "
            f"{r['total_trades']} | {r['wr_pct']:.1f} | "
            f"{r['total_net_prime_usd']:+.2f} | {r['daily_mean_net']:+.4f} | {ci} |\n"
        )
    path.write_text("".join(lines))


def write_verdict(rows: List[dict], path: Path) -> None:
    honest = sorted([r for r in rows if r["fill_model"] == "honest"],
                    key=lambda r: r["ci95_lo_net"], reverse=True)
    production = sorted([r for r in rows if r["fill_model"] == "production"],
                        key=lambda r: r["ci95_lo_net"], reverse=True)
    # Are any honest configs CI-positive (lower bound > 0)?
    ci_pos_honest = [r for r in honest if r["ci95_lo_net"] > 0]
    # Are any net-total-positive?
    net_pos_honest = [r for r in honest if r["total_net_prime_usd"] > 0]
    # The S27 candidate
    s27_cand = [r for r in honest
                if abs(r["tp"] - 5.0) < 1e-9 and abs(r["sl"] - 16.0) < 1e-9
                and abs(r["z"] - 2.0) < 1e-9]
    s27 = s27_cand[0] if s27_cand else None

    L = []
    L.append("# Dukascopy 2yr XAU wide-grid — VERDICT\n")
    L.append(f"Sample window: 2023-09-27 → 2025-09-26 (623 calendar/session days).\n")
    L.append(f"Configs: 52 = (TP∈{{3,5,8,12}} × SL∈{{8,12,16,24}} | SL>TP) × z∈{{1.5,2,2.5,3.0}}.\n")
    L.append(f"Fill model: HONEST (next-tick worst-side). Commission: ${COMMISSION_RT}/RT BlackBull Prime.\n")
    L.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}, CI={CI_PCT}%, ungated.\n\n")
    L.append("## Headline\n\n")
    L.append(f"- Honest-fill configs with **net-of-commission lower CI95 > 0**: "
             f"**{len(ci_pos_honest)} / {len(honest)}**.\n")
    L.append(f"- Honest-fill configs with **positive net total $**: "
             f"**{len(net_pos_honest)} / {len(honest)}**.\n")
    if honest:
        b = honest[0]
        L.append(f"- Best by lower CI95 bound (net): TP={b['tp']:.1f}, SL={b['sl']:.1f}, z={b['z']:.1f}, "
                 f"trades={b['total_trades']}, WR={b['wr_pct']:.1f}%, "
                 f"net=${b['total_net_prime_usd']:+.2f}, daily mean net=${b['daily_mean_net']:+.4f}, "
                 f"CI=[{b['ci95_lo_net']:+.4f}, {b['ci95_hi_net']:+.4f}].\n")
    L.append("\n## S27 candidate (TP=5/SL=16/z=2.0) on this corpus\n\n")
    if s27:
        L.append(f"- trades={s27['total_trades']}, WR={s27['wr_pct']:.1f}%\n")
        L.append(f"- total gross $: {s27['total_gross_usd']:+.2f}; "
                 f"total net $: {s27['total_net_prime_usd']:+.2f}\n")
        L.append(f"- exp/trade net: ${s27['exp_net_per_trade']:+.4f}\n")
        L.append(f"- daily mean net: ${s27['daily_mean_net']:+.4f}; "
                 f"CI95 net daily: [{s27['ci95_lo_net']:+.4f}, {s27['ci95_hi_net']:+.4f}]\n")
    else:
        L.append("Not present in summary.\n")

    L.append("\n## Conclusion (do not adapt without thinking)\n\n")
    if not ci_pos_honest:
        L.append(
            "**SIGNAL-EMPTY.** No configuration in the 52-cell wide grid produced a "
            "net-of-commission daily-mean PnL with a strictly positive 95% bootstrap CI "
            "across the full 623-day Dukascopy 2yr XAU corpus. The 5/16 mean-reversion "
            "family is not an edge on tick-only XAU data under honest fills + BlackBull "
            "Prime commission. This corroborates handoff S27 §4.4 (gates are the entire "
            "edge), §4.3 (S27 candidate CI was already fragile at latency≥3), and the "
            "phase-1 214-day result.\n")
    else:
        L.append(
            f"**{len(ci_pos_honest)} configuration(s) survive a CI-positive test.** "
            "Investigate before claiming an edge: check window-sensitivity, "
            "cross-instrument port, walk-forward stability, and whether the result is "
            "concentrated in a small number of outlier days.\n")

    L.append(
        "\nNext-session direction: HistData cross-currency port (S27 §4.3) to test "
        "whether the mean-reversion family has edge anywhere in liquid FX, and the "
        "tick-only regime-classifier work (S27 §4.5) to recover the gate-edge on "
        "tick-only data. **Do not deploy live based on this result.**\n"
    )
    path.write_text("".join(L))


def main() -> int:
    print(f"[INFO] loading {IN_CSV} ...")
    rows = load_rows(IN_CSV)
    print(f"[INFO] {len(rows)} rows loaded")
    results = aggregate(rows)
    print(f"[INFO] {len(results)} (config × fill_model) groups")
    write_summary(results, OUT_SUMMARY)
    write_top10(results, OUT_TOP10)
    write_verdict(results, OUT_VERDICT)
    print(f"[INFO] wrote {OUT_SUMMARY}")
    print(f"[INFO] wrote {OUT_TOP10}")
    print(f"[INFO] wrote {OUT_VERDICT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
