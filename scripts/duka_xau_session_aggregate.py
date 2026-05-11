#!/usr/bin/env python3
"""
scripts/duka_xau_session_aggregate.py

S29 Phase 1/4 — aggregate the S29 session-stratified wide-grid sweep over the
623-day Dukascopy XAU corpus, alongside the S28 24h baseline.

Inputs (all share the harness's per-day CSV schema):
  outputs/duka_wide_grid.csv            — S28 24h baseline   (label=duka_wide_ungated)
  outputs/duka_wide_session_london.csv  — S29 London 07-12 UTC
  outputs/duka_wide_session_ny.csv      — S29 NY     13-18 UTC
  outputs/duka_wide_session_asia.csv    — S29 Asia   00-07 UTC

Per (session, tp, sl, z, fill_model):
  - total trades, gross/net $, expectancy/trade
  - daily mean and median PnL across days seen
  - 1000-iter daily-mean bootstrap CI95 (seed=42), net-of-Prime $0.06/RT

Outputs:
  outputs/duka_xau_session_summary.csv  — one row per (session × config × fill)
  outputs/duka_xau_session_top10.md     — per-session top-5 + global top-10
  outputs/duka_xau_session_verdict.md   — verdict: any (session, config) CI-positive?

Usage:
  python3 scripts/duka_xau_session_aggregate.py
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
]

OUT_SUMMARY = REPO_ROOT / "outputs" / "duka_xau_session_summary.csv"
OUT_TOP10   = REPO_ROOT / "outputs" / "duka_xau_session_top10.md"
OUT_VERDICT = REPO_ROOT / "outputs" / "duka_xau_session_verdict.md"

COMMISSION_RT   = 0.06        # USD per round-trip @ 0.01-lot, BlackBull ECN Prime
BOOTSTRAP_ITERS = 1000
BOOTSTRAP_SEED  = 42
CI_PCT          = 95.0


def bootstrap_ci(values: np.ndarray,
                 iters: int,
                 seed: int,
                 ci_pct: float) -> Tuple[float, float]:
    """1000-iter bootstrap CI on the MEAN of values (per-day PnL)."""
    if values.size == 0:
        return float("nan"), float("nan")
    rng = np.random.default_rng(seed)
    n = values.size
    idx = rng.integers(0, n, size=(iters, n))
    means = values[idx].mean(axis=1)
    lo = float(np.percentile(means, (100 - ci_pct) / 2))
    hi = float(np.percentile(means, 100 - (100 - ci_pct) / 2))
    return lo, hi


def aggregate_one(session_label: str, csv_path: Path) -> List[dict]:
    if not csv_path.exists():
        print(f"[WARN] missing {csv_path}; skipping {session_label}")
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
        n_days = sums.size
        total_trades = int(trades.sum())
        total_wins   = int(wins.sum())
        total_gross  = float(sums.sum())
        total_net    = total_gross - total_trades * COMMISSION_RT
        wr           = (total_wins / total_trades * 100.0) if total_trades else 0.0
        daily_gross  = sums
        daily_net    = sums - trades * COMMISSION_RT
        mean_net     = float(daily_net.mean()) if n_days else 0.0
        median_gross = float(np.median(daily_gross)) if n_days else 0.0
        lo_n, hi_n   = bootstrap_ci(daily_net, BOOTSTRAP_ITERS,
                                    BOOTSTRAP_SEED, CI_PCT)
        exp_net      = (total_net / total_trades) if total_trades else 0.0
        out.append({
            "session": session_label,
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


def write_summary_csv(rows: List[dict], path: Path) -> None:
    if not rows:
        return
    cols = list(rows[0].keys())
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)


SESSION_ORDER = ["all_day", "london", "ny", "asia"]


def write_top10(rows: List[dict], path: Path) -> None:
    honest = [r for r in rows if r["fill_model"] == "honest"]
    L: List[str] = []
    L.append("# S29 Phase 1 — session-stratified wide-grid — top configs (honest fill, net-of-Prime)\n\n")
    L.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}, "
             f"CI={CI_PCT}%, commission ${COMMISSION_RT:.2f}/RT, ungated.\n\n")

    for sess in SESSION_ORDER:
        sub = sorted([r for r in honest if r["session"] == sess],
                     key=lambda r: r["ci95_lo_net"], reverse=True)
        if not sub:
            continue
        L.append(f"## Session: **{sess}**  (n_days_seen={sub[0]['n_days']})\n\n")
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
    L.append("## Global top 10 across all sessions (by lower CI95 net)\n\n")
    L.append("| Rank | Session | TP | SL | z | Trades | WR% | "
             "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
    L.append("|---|---|---|---|---|---|---|---|---|---|\n")
    for i, r in enumerate(glob[:10], 1):
        ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
        L.append(f"| {i} | {r['session']} | {r['tp']:.1f} | {r['sl']:.1f} | "
                 f"{r['z']:.1f} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                 f"{r['total_net_prime_usd']:+.2f} | "
                 f"{r['daily_mean_net']:+.4f} | {ci} |\n")
    path.write_text("".join(L))


def write_verdict(rows: List[dict], path: Path) -> None:
    honest = [r for r in rows if r["fill_model"] == "honest"]
    ci_pos  = [r for r in honest if r["ci95_lo_net"] > 0]
    net_pos = [r for r in honest if r["total_net_prime_usd"] > 0]

    L: List[str] = []
    L.append("# S29 Phase 1 — session-stratified wide grid — VERDICT\n\n")
    L.append("Sample: 623 daily files, 2023-09-27 → 2025-09-26 "
             "(Dukascopy 2yr XAU).\n")
    L.append("Sessions: 24h baseline (S28), London 07-12 UTC, NY 13-18 UTC, "
             "Asia 00-07 UTC.\n")
    L.append("Configs/session: 52 = (TP∈{3,5,8,12} × SL∈{8,12,16,24} | SL>TP) × "
             "z∈{1.5,2,2.5,3}.\n")
    L.append("Fill: HONEST (next-tick worst-side). Commission: "
             f"${COMMISSION_RT}/RT. Ungated.\n")
    L.append(f"Bootstrap: {BOOTSTRAP_ITERS} iters, seed={BOOTSTRAP_SEED}, "
             f"CI={CI_PCT}%.\n\n")

    L.append("## Headline\n\n")
    L.append(f"- Honest-fill (session × config) tuples with "
             f"**CI95 lower > 0** (net of Prime): **{len(ci_pos)} / {len(honest)}**.\n")
    L.append(f"- Honest-fill tuples with positive total net $: "
             f"**{len(net_pos)} / {len(honest)}**.\n\n")

    if ci_pos:
        L.append("## CI-positive tuples (an actual edge before further validation)\n\n")
        L.append("| Session | TP | SL | z | Trades | WR% | "
                 "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
        L.append("|---|---|---|---|---|---|---|---|---|\n")
        for r in sorted(ci_pos, key=lambda r: r["ci95_lo_net"], reverse=True):
            ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
            L.append(f"| {r['session']} | {r['tp']:.1f} | {r['sl']:.1f} | "
                     f"{r['z']:.1f} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['daily_mean_net']:+.4f} | {ci} |\n")
        L.append("\n")

    L.append("## Per-session best (by lower CI95 net)\n\n")
    L.append("| Session | TP | SL | z | Trades | WR% | "
             "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
    L.append("|---|---|---|---|---|---|---|---|---|\n")
    for sess in SESSION_ORDER:
        sub = sorted([r for r in honest if r["session"] == sess],
                     key=lambda r: r["ci95_lo_net"], reverse=True)
        if sub:
            r = sub[0]
            ci = f"[{r['ci95_lo_net']:+.2f}, {r['ci95_hi_net']:+.2f}]"
            L.append(f"| {sess} | {r['tp']:.1f} | {r['sl']:.1f} | "
                     f"{r['z']:.1f} | {r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['daily_mean_net']:+.4f} | {ci} |\n")

    L.append("\n## Conclusion\n\n")
    if ci_pos:
        L.append(f"**{len(ci_pos)} CI-positive tuple(s) found.** This is a candidate "
                 "edge under honest fill + Prime commission, ungated. Required "
                 "validation before trusting:\n"
                 "  1. Walk-forward stability (split into in-sample / out-of-sample halves).\n"
                 "  2. Concentration check: is the edge dominated by ≤5 outlier days?\n"
                 "  3. Cross-window sensitivity: rerun at W∈{50, 100, 400, 800, 1600}.\n"
                 "  4. Independent Python replay verification (Phase 5) on one positive day.\n"
                 "  5. Compare with Phase 3 (wider geometry) and Phase 3b (z-momentum) "
                 "to confirm whether this is a real session effect or a spurious sliver.\n")
    else:
        L.append("**SIGNAL-EMPTY across all four sessions** (24h baseline + London + NY + Asia). "
                 "No tuple in the (52 configs × 4 sessions = 208) honest-fill cells achieves a "
                 "strictly positive 95% CI on net daily PnL. The negative-expectancy result "
                 "from S28 holds under session stratification — restricting to a single "
                 "trading window does not surface a hidden positive sub-population.\n\n"
                 "Next levers to try, in order:\n"
                 "  - **Phase 3:** wider/asymmetric geometry beyond the (3-12, 8-24) box.\n"
                 "  - **Phase 3b:** signal inversion (z-momentum, not z-MR) on the same grid.\n"
                 "  - If both negative → the candidate signal family is structurally dead on "
                 "tick-only XAU and the next move is an entirely different signal "
                 "(TSMOM, bar-MR at multiple timeframes, vol-regime, time-of-day breakouts).\n")

    path.write_text("".join(L))


def main() -> int:
    all_rows: List[dict] = []
    for sess, p in INPUTS:
        out = aggregate_one(sess, p)
        print(f"[INFO] {sess}: {len(out)} (config × fill) groups from {p.name}")
        all_rows.extend(out)
    write_summary_csv(all_rows, OUT_SUMMARY)
    write_top10(all_rows, OUT_TOP10)
    write_verdict(all_rows, OUT_VERDICT)
    print(f"[INFO] wrote {OUT_SUMMARY}")
    print(f"[INFO] wrote {OUT_TOP10}")
    print(f"[INFO] wrote {OUT_VERDICT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
