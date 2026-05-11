#!/usr/bin/env python3
"""
scripts/duka_xau_fine_aggregate.py

S30 §3.1 aggregator for the --wide-fine sweep:
    outputs/duka_wide_fine_asia.csv
keyed on (tp, sl, z, window, fill_model).

This is a fork of scripts/duka_xau_phase_aggregate.py.  The phase aggregator
groups by (tp, sl, z, fill_model) — collapsing windows together — which is
wrong for the S30 fine-grid sweep where window is a varied dimension.

Pipeline per (tp, sl, z, window, fill_model):
  - aggregate per-day rows into n_days, total trades, gross + net-of-Prime
  - 1000-iter daily-mean bootstrap CI95 (seed=42), net of $0.06/RT BlackBull Prime
  - identify cells with strictly CI95_lower > 0 (the "edge" threshold)
  - rank by lower CI95 and emit per-window leaderboards

Compares the top S30 fine-grid candidate to the S29 PRIMARY
(TP=30, SL=15, z=2.0, W=200, asia, ungated) which had CI95 [-0.17, +2.65].

Outputs:
  outputs/duka_xau_fine_summary.csv
  outputs/duka_xau_fine_top10.md
  outputs/duka_xau_fine_verdict.md
"""
from __future__ import annotations

import csv
import sys
from collections import defaultdict
from pathlib import Path
from typing import List, Tuple

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]

INPUT_CSV   = REPO_ROOT / "outputs" / "duka_wide_fine_asia.csv"
OUT_SUMMARY = REPO_ROOT / "outputs" / "duka_xau_fine_summary.csv"
OUT_TOP10   = REPO_ROOT / "outputs" / "duka_xau_fine_top10.md"
OUT_VERDICT = REPO_ROOT / "outputs" / "duka_xau_fine_verdict.md"

COMMISSION_RT   = 0.06
BOOTSTRAP_ITERS = 1000
BOOTSTRAP_SEED  = 42
CI_PCT          = 95.0

# S29 PRIMARY for direct comparison
S29_PRIMARY = {
    "tp": 30.0, "sl": 15.0, "z": 2.0, "window": 200,
    "phase": "extreme_asia (S29)",
    "daily_mean_net": 1.2006,
    "ci95_lo_net": -0.17,
    "ci95_hi_net": 2.65,
    "total_net_prime_usd": 747.96,
    "total_trades": 675,
}


def bootstrap_ci(values: np.ndarray, iters: int, seed: int,
                 ci_pct: float) -> Tuple[float, float]:
    if values.size == 0:
        return float("nan"), float("nan")
    rng = np.random.default_rng(seed)
    n = values.size
    idx = rng.integers(0, n, size=(iters, n))
    means = values[idx].mean(axis=1)
    lo = float(np.percentile(means, (100 - ci_pct) / 2))
    hi = float(np.percentile(means, 100 - (100 - ci_pct) / 2))
    return lo, hi


def aggregate(csv_path: Path) -> List[dict]:
    if not csv_path.exists():
        print(f"[FATAL] missing {csv_path}")
        return []
    with csv_path.open("r") as f:
        rows = list(csv.DictReader(f))
    groups: dict[tuple, list[dict]] = defaultdict(list)
    for r in rows:
        key = (r["tp"], r["sl"], r["z"], r["window"], r["fill_model"])
        groups[key].append(r)
    out: List[dict] = []
    for (tp, sl, z, w, fm), grp in groups.items():
        sums   = np.array([float(g["sum_usd"])  for g in grp], dtype=np.float64)
        trades = np.array([int(g["n_trades"])   for g in grp], dtype=np.int64)
        wins   = np.array([int(g["n_wins"])     for g in grp], dtype=np.int64)
        n_days       = sums.size
        total_trades = int(trades.sum())
        total_wins   = int(wins.sum())
        total_gross  = float(sums.sum())
        total_net    = total_gross - total_trades * COMMISSION_RT
        wr           = (total_wins / total_trades * 100.0) if total_trades else 0.0
        daily_net    = sums - trades * COMMISSION_RT
        mean_net     = float(daily_net.mean()) if n_days else 0.0
        median_gross = float(np.median(sums)) if n_days else 0.0
        lo_n, hi_n   = bootstrap_ci(daily_net, BOOTSTRAP_ITERS,
                                    BOOTSTRAP_SEED, CI_PCT)
        exp_net      = (total_net / total_trades) if total_trades else 0.0
        out.append({
            "tp": float(tp), "sl": float(sl), "z": float(z),
            "window": int(w), "fill_model": fm,
            "n_days": n_days,
            "total_trades": total_trades,
            "wr_pct": round(wr, 2),
            "total_gross_usd": round(total_gross, 2),
            "total_net_prime_usd": round(total_net, 2),
            "exp_net_per_trade": round(exp_net, 4),
            "daily_mean_net": round(mean_net, 4),
            "daily_median_gross": round(median_gross, 4),
            "ci95_lo_net": round(lo_n, 4),
            "ci95_hi_net": round(hi_n, 4),
        })
    return out


def write_summary(rows: List[dict], path: Path) -> None:
    if not rows:
        return
    cols = list(rows[0].keys())
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def fmt_ci(r: dict) -> str:
    return f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"


def write_top10(rows: List[dict], path: Path) -> None:
    honest = [r for r in rows if r["fill_model"] == "honest"]
    L: List[str] = []
    L.append("# S30 §3.1 — fine-grid (256 cells) leaderboards (honest fill, net-of-Prime)\n\n")
    L.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}, "
             f"CI={CI_PCT}%, commission ${COMMISSION_RT:.2f}/RT, ungated.\n")
    L.append("Geometry: TP ∈ {25,30,35,40} × SL ∈ {12,15,18,20} × "
             "z ∈ {1.5,2.0,2.5,3.0} × W ∈ {100,200,400,800}.\n")
    L.append("Session: UTC 00–07 (Asia).  Latency: 1 tick.\n\n")

    L.append("## Global top 20 by lower CI95 (honest fill, net-of-Prime)\n\n")
    L.append("| Rank | TP | SL | z | W | Trades | WR% | "
             "Total net $ | Exp/tr net $ | Daily mean net $ | CI95 net daily $ |\n")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|\n")
    glob = sorted(honest, key=lambda r: r["ci95_lo_net"], reverse=True)
    for i, r in enumerate(glob[:20], 1):
        L.append(f"| {i} | {r['tp']:.1f} | {r['sl']:.1f} | {r['z']:.1f} | "
                 f"{r['window']} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                 f"{r['total_net_prime_usd']:+.2f} | "
                 f"{r['exp_net_per_trade']:+.4f} | "
                 f"{r['daily_mean_net']:+.4f} | {fmt_ci(r)} |\n")
    L.append("\n")

    # Per-window top-5
    for w in [100, 200, 400, 800]:
        sub = [r for r in honest if r["window"] == w]
        sub_sorted = sorted(sub, key=lambda r: r["ci95_lo_net"], reverse=True)
        L.append(f"## Top 5 at W={w}\n\n")
        L.append("| Rank | TP | SL | z | Trades | WR% | "
                 "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
        L.append("|---|---|---|---|---|---|---|---|---|\n")
        for i, r in enumerate(sub_sorted[:5], 1):
            L.append(f"| {i} | {r['tp']:.1f} | {r['sl']:.1f} | {r['z']:.1f} | "
                     f"{r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['daily_mean_net']:+.4f} | {fmt_ci(r)} |\n")
        L.append("\n")

    # Per-z top-5
    for z in [1.5, 2.0, 2.5, 3.0]:
        sub = [r for r in honest if r["z"] == z]
        sub_sorted = sorted(sub, key=lambda r: r["ci95_lo_net"], reverse=True)
        L.append(f"## Top 5 at z={z}\n\n")
        L.append("| Rank | TP | SL | W | Trades | WR% | "
                 "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
        L.append("|---|---|---|---|---|---|---|---|---|\n")
        for i, r in enumerate(sub_sorted[:5], 1):
            L.append(f"| {i} | {r['tp']:.1f} | {r['sl']:.1f} | {r['window']} | "
                     f"{r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['daily_mean_net']:+.4f} | {fmt_ci(r)} |\n")
        L.append("\n")

    path.write_text("".join(L))


def write_verdict(rows: List[dict], path: Path) -> None:
    honest  = [r for r in rows if r["fill_model"] == "honest"]
    ci_pos  = [r for r in honest if r["ci95_lo_net"] > 0]
    net_pos = [r for r in honest if r["total_net_prime_usd"] > 0]
    near    = [r for r in honest
               if -0.30 < r["ci95_lo_net"] <= 0 and r["ci95_hi_net"] > 0]
    beats_primary_ci = [
        r for r in honest
        if r["ci95_lo_net"] > S29_PRIMARY["ci95_lo_net"]
    ]
    beats_primary_mean = [
        r for r in honest
        if r["daily_mean_net"] > S29_PRIMARY["daily_mean_net"]
    ]

    # Locate the S29 PRIMARY cell in the fine grid for direct apples-to-apples
    primary_in_grid = [
        r for r in honest
        if r["tp"] == 30.0 and r["sl"] == 15.0
        and r["z"] == 2.0 and r["window"] == 200
    ]

    L: List[str] = []
    L.append("# S30 §3.1 — VERDICT — fine-grid hunt around the PRIMARY candidate\n\n")
    L.append(f"Sample: 623 daily files, 2023-09-27 → 2025-09-26.  "
             "Fill: HONEST.  Session: UTC 00–07.  Ungated.  "
             f"Commission: ${COMMISSION_RT}/RT BlackBull Prime.  "
             f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}, "
             f"CI={CI_PCT}%.\n\n")
    L.append("## Grid\n\n")
    L.append("- **TP ∈ {25, 30, 35, 40}** USD/oz (4)\n")
    L.append("- **SL ∈ {12, 15, 18, 20}** USD/oz (4)\n")
    L.append("- **z ∈ {1.5, 2.0, 2.5, 3.0}** (4)\n")
    L.append("- **W ∈ {100, 200, 400, 800}** ticks (4)\n")
    L.append(f"- Total honest cells: **{len(honest)} / 256**\n\n")

    L.append("## Reference: S29 PRIMARY (extreme_asia)\n\n")
    L.append(f"- TP={S29_PRIMARY['tp']}, SL={S29_PRIMARY['sl']}, "
             f"z={S29_PRIMARY['z']}, W={S29_PRIMARY['window']}\n")
    L.append(f"- Daily mean net: **{S29_PRIMARY['daily_mean_net']:+.4f}**\n")
    L.append(f"- CI95 net daily $: **[{S29_PRIMARY['ci95_lo_net']:+.2f}, "
             f"{S29_PRIMARY['ci95_hi_net']:+.2f}]**\n")
    L.append(f"- Total net (Prime): {S29_PRIMARY['total_net_prime_usd']:+.2f}, "
             f"trades: {S29_PRIMARY['total_trades']}\n\n")

    if primary_in_grid:
        p = primary_in_grid[0]
        L.append("### Same cell re-measured under S30 fine-grid (should ≈ S29)\n\n")
        L.append(f"- TP={p['tp']:.1f}, SL={p['sl']:.1f}, z={p['z']:.1f}, "
                 f"W={p['window']}\n")
        L.append(f"- Daily mean net: **{p['daily_mean_net']:+.4f}**  "
                 f"(S29: {S29_PRIMARY['daily_mean_net']:+.4f})\n")
        L.append(f"- CI95 net daily $: **{fmt_ci(p)}**  "
                 f"(S29: [{S29_PRIMARY['ci95_lo_net']:+.2f}, "
                 f"{S29_PRIMARY['ci95_hi_net']:+.2f}])\n")
        L.append(f"- Total net (Prime): {p['total_net_prime_usd']:+.2f}, "
                 f"trades: {p['total_trades']}\n\n")
        # Sanity: this should match S29 to within tiny commission/bootstrap noise
        delta_mean = p["daily_mean_net"] - S29_PRIMARY["daily_mean_net"]
        delta_lo   = p["ci95_lo_net"]   - S29_PRIMARY["ci95_lo_net"]
        if abs(delta_mean) > 0.05 or abs(delta_lo) > 0.10:
            L.append(f"**WARNING: PRIMARY cell metrics diverge from S29 "
                     f"by Δmean={delta_mean:+.4f}, Δci_lo={delta_lo:+.4f}. "
                     f"Investigate before trusting other fine-grid results.**\n\n")

    L.append("## Headline\n\n")
    L.append(f"- Honest-fill cells with **CI95 lower > 0** (strict edge): "
             f"**{len(ci_pos)} / {len(honest)}**\n")
    L.append(f"- Honest-fill cells with positive total net $: "
             f"**{len(net_pos)} / {len(honest)}**\n")
    L.append(f"- Cells that **beat the S29 PRIMARY's CI95 lower** "
             f"({S29_PRIMARY['ci95_lo_net']:+.2f}): **{len(beats_primary_ci)} / {len(honest)}**\n")
    L.append(f"- Cells that **beat the S29 PRIMARY's daily mean** "
             f"({S29_PRIMARY['daily_mean_net']:+.4f}): **{len(beats_primary_mean)} / {len(honest)}**\n")
    L.append(f"- Near-edge cells (−$0.30 < CI95_lo ≤ 0 AND CI95_hi > 0): "
             f"**{len(near)}**\n\n")

    if ci_pos:
        L.append("## ★ Strict CI-positive cells (the answer to '§3.1 push it over the line') ★\n\n")
        L.append("| TP | SL | z | W | Trades | WR% | "
                 "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
        L.append("|---|---|---|---|---|---|---|---|---|\n")
        for r in sorted(ci_pos, key=lambda r: r["ci95_lo_net"], reverse=True):
            L.append(f"| {r['tp']:.1f} | {r['sl']:.1f} | {r['z']:.1f} | "
                     f"{r['window']} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['daily_mean_net']:+.4f} | {fmt_ci(r)} |\n")
        L.append("\n**Required validation before declaring this a deployable edge:**\n"
                 "  1. Walk-forward IS/OOS (split 311/312 days) — sign preserved both halves.\n"
                 "  2. Concentration: top-K winning-day contribution ≤ 70% of total net.\n"
                 "  3. Independent Python replay on one positive day matches C++ harness.\n"
                 "  4. (Recommended next) Re-run gated to test if S27 gates amplify.\n\n")
    else:
        L.append("## No strict CI-positive cell in the fine grid\n\n")

    L.append("## Top 5 by CI95 lower (honest fill)\n\n")
    L.append("| TP | SL | z | W | Trades | WR% | "
             "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
    L.append("|---|---|---|---|---|---|---|---|---|\n")
    glob = sorted(honest, key=lambda r: r["ci95_lo_net"], reverse=True)
    for r in glob[:5]:
        L.append(f"| {r['tp']:.1f} | {r['sl']:.1f} | {r['z']:.1f} | "
                 f"{r['window']} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                 f"{r['total_net_prime_usd']:+.2f} | "
                 f"{r['daily_mean_net']:+.4f} | {fmt_ci(r)} |\n")

    L.append("\n## Conclusion (operator: read carefully)\n\n")
    if ci_pos:
        L.append(f"**S30 FINE GRID PRODUCED {len(ci_pos)} CI-POSITIVE CELL(S).** "
                 "These are candidates that pass the strict CI95-lower-bound > 0 "
                 "test the S29 PRIMARY narrowly missed.  They are candidates, NOT "
                 "deployable strategies.  Apply the validation checklist above and "
                 "consider the S27/S26P4 gating tests before any further commitment.\n")
    elif beats_primary_ci:
        L.append(f"**No strict CI-positive cell, but {len(beats_primary_ci)} cells "
                 "have a *higher* CI95 lower than the S29 PRIMARY** "
                 f"({S29_PRIMARY['ci95_lo_net']:+.2f}).  The PRIMARY remains the "
                 "best candidate by daily-mean magnitude but the geometry it lives "
                 "on is not unique — adjacent cells are competitive.  Next priority "
                 "from handoff §3.1: (2) cooldown scan, (3) latency scan, OR jump to "
                 "§3.2 gated variant to see if S26P4-style gates amplify the family.\n")
    else:
        L.append("**No cell in the 256-cell fine grid beats the S29 PRIMARY's "
                 "CI95 lower bound.**  The PRIMARY (TP=30, SL=15, z=2.0, W=200) is "
                 "the local optimum for this geometry/window/z-threshold family in "
                 "Asia session.  Recommended next: §3.2 gated variant — if the S27 "
                 "L2 + regime gates flip the near-zero signal into a clear edge here, "
                 "that becomes the deployment candidate.\n")

    L.append("\n## Do NOT deploy live based on any verdict in this file.\n"
             "Operator owns the live-trade decision.  Mode=SHADOW remains in effect.\n")
    path.write_text("".join(L))


def main() -> int:
    rows = aggregate(INPUT_CSV)
    print(f"[INFO] aggregated {len(rows)} (cell × fill) tuples from {INPUT_CSV.name}")
    write_summary(rows, OUT_SUMMARY)
    write_top10(rows, OUT_TOP10)
    write_verdict(rows, OUT_VERDICT)
    print(f"[INFO] wrote {OUT_SUMMARY}")
    print(f"[INFO] wrote {OUT_TOP10}")
    print(f"[INFO] wrote {OUT_VERDICT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
