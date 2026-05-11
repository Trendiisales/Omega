#!/usr/bin/env python3
"""
scripts/duka_xau_phase_aggregate.py

S29 Phase 4 — unified aggregator across the entire S29 edge-hunt:

  Phase 1 (session-stratified MR wide-grid, S28 24h baseline + 3 sessions):
    outputs/duka_wide_grid.csv             label=all_day
    outputs/duka_wide_session_london.csv   label=london   (07-12 UTC)
    outputs/duka_wide_session_ny.csv       label=ny       (13-18 UTC)
    outputs/duka_wide_session_asia.csv     label=asia     (00-07 UTC)

  Phase 3  (wider/asymmetric geometry, all-day):
    outputs/duka_wide_extreme.csv          label=extreme

  Phase 3b (signal inversion = z-momentum, all-day):
    outputs/duka_wide_invert.csv           label=invert

For each (phase, tp, sl, z, fill_model):
  - n_days, total trades, gross + net-of-Prime $0.06/RT
  - daily mean PnL (gross + net), median gross
  - 1000-iter daily-mean bootstrap CI95 (seed=42), net-of-Prime

Outputs:
  outputs/duka_xau_phase_summary.csv
  outputs/duka_xau_phase_top10.md
  outputs/duka_xau_phase_verdict.md   ← THE answer to "find a positive edge"
"""
from __future__ import annotations

import csv
import sys
from collections import defaultdict
from pathlib import Path
from typing import List, Tuple

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]

INPUTS: List[Tuple[str, Path]] = [
    ("all_day", REPO_ROOT / "outputs" / "duka_wide_grid.csv"),
    ("london",  REPO_ROOT / "outputs" / "duka_wide_session_london.csv"),
    ("ny",      REPO_ROOT / "outputs" / "duka_wide_session_ny.csv"),
    ("asia",    REPO_ROOT / "outputs" / "duka_wide_session_asia.csv"),
    ("extreme",      REPO_ROOT / "outputs" / "duka_wide_extreme.csv"),
    ("invert",       REPO_ROOT / "outputs" / "duka_wide_invert.csv"),
    ("extreme_asia", REPO_ROOT / "outputs" / "duka_wide_extreme_asia.csv"),
]

OUT_SUMMARY = REPO_ROOT / "outputs" / "duka_xau_phase_summary.csv"
OUT_TOP10   = REPO_ROOT / "outputs" / "duka_xau_phase_top10.md"
OUT_VERDICT = REPO_ROOT / "outputs" / "duka_xau_phase_verdict.md"

COMMISSION_RT   = 0.06
BOOTSTRAP_ITERS = 1000
BOOTSTRAP_SEED  = 42
CI_PCT          = 95.0

PHASE_ORDER = ["all_day", "london", "ny", "asia", "extreme", "invert", "extreme_asia"]
PHASE_DESC = {
    "all_day":      "S28 baseline: MR wide-grid, all-day, 52 configs",
    "london":       "S29 P1: MR wide-grid, 07-12 UTC, 52 configs",
    "ny":           "S29 P1: MR wide-grid, 13-18 UTC, 52 configs",
    "asia":         "S29 P1: MR wide-grid, 00-07 UTC, 52 configs",
    "extreme":      "S29 P3: MR wide-EXTREME geometry, all-day, 72 configs",
    "invert":       "S29 P3b: z-MOMENTUM (inverted), wide grid, all-day, 52 configs",
    "extreme_asia": "S29 P6: wide-EXTREME geometry × Asia 00-07 UTC, 72 configs",
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


def aggregate_one(phase: str, csv_path: Path) -> List[dict]:
    if not csv_path.exists():
        print(f"[WARN] missing {csv_path}; skipping {phase}")
        return []
    with csv_path.open("r") as f:
        rows = list(csv.DictReader(f))
    groups = defaultdict(list)
    for r in rows:
        key = (r["tp"], r["sl"], r["z"], r["fill_model"])
        groups[key].append(r)
    out = []
    for (tp, sl, z, fm), grp in groups.items():
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
            "phase": phase,
            "tp": float(tp), "sl": float(sl), "z": float(z),
            "fill_model": fm,
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


def write_top10(rows: List[dict], path: Path) -> None:
    honest = [r for r in rows if r["fill_model"] == "honest"]
    L: List[str] = []
    L.append("# S29 Phase 4 — unified top-configs across all S29 phases (honest fill, net-of-Prime)\n\n")
    L.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}, "
             f"CI={CI_PCT}%, commission ${COMMISSION_RT:.2f}/RT, ungated.\n\n")

    for ph in PHASE_ORDER:
        sub = sorted([r for r in honest if r["phase"] == ph],
                     key=lambda r: r["ci95_lo_net"], reverse=True)
        if not sub:
            continue
        L.append(f"## Phase: **{ph}** — {PHASE_DESC[ph]} (n_days={sub[0]['n_days']})\n\n")
        L.append("| Rank | TP | SL | z | Trades | WR% | Total net $ | "
                 "Exp/tr net $ | Daily mean net $ | CI95 net daily $ |\n")
        L.append("|---|---|---|---|---|---|---|---|---|---|\n")
        for i, r in enumerate(sub[:5], 1):
            ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
            L.append(f"| {i} | {r['tp']:.1f} | {r['sl']:.1f} | {r['z']:.1f} | "
                     f"{r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['exp_net_per_trade']:+.4f} | "
                     f"{r['daily_mean_net']:+.4f} | {ci} |\n")
        L.append("\n")

    glob = sorted(honest, key=lambda r: r["ci95_lo_net"], reverse=True)
    L.append("## Global top 15 across ALL phases (by lower CI95 net)\n\n")
    L.append("| Rank | Phase | TP | SL | z | Trades | WR% | "
             "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
    L.append("|---|---|---|---|---|---|---|---|---|---|\n")
    for i, r in enumerate(glob[:15], 1):
        ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
        L.append(f"| {i} | {r['phase']} | {r['tp']:.1f} | {r['sl']:.1f} | "
                 f"{r['z']:.1f} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                 f"{r['total_net_prime_usd']:+.2f} | "
                 f"{r['daily_mean_net']:+.4f} | {ci} |\n")
    path.write_text("".join(L))


def write_verdict(rows: List[dict], path: Path) -> None:
    honest  = [r for r in rows if r["fill_model"] == "honest"]
    ci_pos  = [r for r in honest if r["ci95_lo_net"] > 0]
    net_pos = [r for r in honest if r["total_net_prime_usd"] > 0]
    near    = [r for r in honest if r["ci95_lo_net"] > -0.20 and r["ci95_hi_net"] > 0]

    L: List[str] = []
    L.append("# S29 Phase 4 — UNIFIED VERDICT — \"is there a positive edge in this XAU data?\"\n\n")
    L.append("Sample: 623 daily files, 2023-09-27 → 2025-09-26 (Dukascopy 2yr XAU).\n")
    L.append("Fill model: HONEST (next-tick worst-side). Commission: "
             f"${COMMISSION_RT}/RT BlackBull Prime. Ungated.\n")
    L.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}, "
             f"CI={CI_PCT}%.\n\n")

    L.append("## Phases tested (orthogonal levers)\n\n")
    L.append("| Phase | Description | configs | total honest tuples |\n")
    L.append("|---|---|---|---|\n")
    for ph in PHASE_ORDER:
        n = len([r for r in honest if r["phase"] == ph])
        L.append(f"| {ph} | {PHASE_DESC[ph]} | {n} | {n} |\n")

    L.append("\n## Headline\n\n")
    L.append(f"- Honest-fill (phase × config) tuples with **CI95 lower > 0** "
             f"(net of Prime): **{len(ci_pos)} / {len(honest)}**.\n")
    L.append(f"- Tuples with positive total net $: **{len(net_pos)} / {len(honest)}**.\n")
    L.append(f"- Tuples with NEAR-edge geometry (CI95 lower > −$0.20 AND upper > $0): "
             f"**{len(near)}**.\n\n")

    if ci_pos:
        L.append("## ★ CI-positive tuples — candidate edges ★\n\n")
        L.append("| Phase | TP | SL | z | Trades | WR% | "
                 "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
        L.append("|---|---|---|---|---|---|---|---|---|\n")
        for r in sorted(ci_pos, key=lambda r: r["ci95_lo_net"], reverse=True):
            ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
            L.append(f"| {r['phase']} | {r['tp']:.1f} | {r['sl']:.1f} | "
                     f"{r['z']:.1f} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['daily_mean_net']:+.4f} | {ci} |\n")
        L.append("\n**Required validation before declaring this an edge:**\n"
                 "  1. Walk-forward stability (split into in-sample / out-of-sample halves).\n"
                 "  2. Concentration check: is the edge dominated by ≤5 outlier days?\n"
                 "  3. Cross-window sensitivity: rerun at W∈{50, 100, 400, 800, 1600}.\n"
                 "  4. Independent Python replay (Phase 5) on one positive day.\n"
                 "  5. Re-run gated to see if production gates amplify or destroy the result.\n\n")
    else:
        L.append("## (no CI-positive tuples)\n\n")
        L.append("Across every phase tested — session-of-day, wider geometry, "
                 "and signal inversion — no honest-fill (phase × config) tuple "
                 "achieves a strictly positive 95% CI on net daily PnL.\n\n")

    L.append("## Per-phase top configuration (by lower CI95 net)\n\n")
    L.append("| Phase | TP | SL | z | Trades | WR% | "
             "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
    L.append("|---|---|---|---|---|---|---|---|---|\n")
    for ph in PHASE_ORDER:
        sub = sorted([r for r in honest if r["phase"] == ph],
                     key=lambda r: r["ci95_lo_net"], reverse=True)
        if sub:
            r = sub[0]
            ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
            L.append(f"| {ph} | {r['tp']:.1f} | {r['sl']:.1f} | "
                     f"{r['z']:.1f} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['daily_mean_net']:+.4f} | {ci} |\n")

    L.append("\n## Conclusion (operator: read carefully)\n\n")
    if ci_pos:
        L.append(f"**EDGE CANDIDATE FOUND.** {len(ci_pos)} CI-positive tuple(s) survive "
                 "the honest-fill + Prime-commission ungated test. **This is a candidate, "
                 "not a deployable strategy.** Apply the validation checklist above before "
                 "trusting it. Most likely failure mode: 1-3 outlier days driving the mean.\n")
    elif near:
        L.append(f"**No CI-positive tuple, but {len(near)} NEAR-edge tuples exist** "
                 "(daily-mean upper CI > 0, lower CI > −$0.20/day). These are statistically "
                 "consistent with zero true expectancy but lean positive. The most "
                 "interesting are listed in the per-phase table above. None are deployable, "
                 "but they identify (phase × geometry) corners worth deeper investigation:\n"
                 "  - cross-window sensitivity (W ∈ {50, 100, 400, 800, 1600})\n"
                 "  - finer geometry sweep around the corner\n"
                 "  - production-gates ON (S26P4 showed gates can flip negative-expectancy signals positive)\n")
    else:
        L.append("**SIGNAL FAMILY EXHAUSTED ON TICK-ONLY XAU.** Across session-of-day, "
                 "wider geometry, and signal inversion — three orthogonal levers tested "
                 "exhaustively over 623 days — no configuration achieves a CI-positive net "
                 "daily PnL under honest fill + Prime commission. The z-MR/z-momentum signal "
                 "family at session scale is structurally signal-empty on tick-only XAU.\n\n"
                 "Pivot recommended to ENTIRELY DIFFERENT signal families:\n"
                 "  - TSMOM at multiple horizons (1h, 4h, 1d)\n"
                 "  - Bar-aggregated MR at 5min / 15min / 1h\n"
                 "  - Volatility-regime trading\n"
                 "  - Time-of-day breakouts (e.g. London/NY open ranges)\n"
                 "  - News-event responses (would need event timestamps)\n")

    L.append("\n## Do NOT deploy live based on any verdict in this file.\n"
             "Operator owns the live-trade decision. The candidate strategy from S27 "
             "(5/16/2.0 with L2 + regime gates, BlackBull Prime $0.69/trade) remains the "
             "best deployment candidate IF you trust the 21-day BlackBull capture is "
             "representative. This S29 work confirms the signal portion of that edge is "
             "near-zero on tick-only data; the gates are doing the work.\n")
    path.write_text("".join(L))


def main() -> int:
    all_rows: List[dict] = []
    for ph, p in INPUTS:
        out = aggregate_one(ph, p)
        print(f"[INFO] {ph}: {len(out)} (config × fill) groups from {p.name}")
        all_rows.extend(out)
    write_summary(all_rows, OUT_SUMMARY)
    write_top10(all_rows, OUT_TOP10)
    write_verdict(all_rows, OUT_VERDICT)
    print(f"[INFO] wrote {OUT_SUMMARY}")
    print(f"[INFO] wrote {OUT_TOP10}")
    print(f"[INFO] wrote {OUT_VERDICT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
