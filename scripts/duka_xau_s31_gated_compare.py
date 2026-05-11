#!/usr/bin/env python3
"""
scripts/duka_xau_s31_gated_compare.py

S31 §3.1 unified gated-variant comparison aggregator.

The S30 handoff §3.1 prescribed: "run --wide-fine --gated and compare to
ungated to see if S27 gates AMPLIFY, leave UNCHANGED, or KILL the new
TOP-1 (TP=35, SL=12, z=2.0, W=200, Asia 0-7 UTC)."

S31 finding-while-investigating: the 623-day Dukascopy corpus has no
L2/regime data (bid/ask only); the literal --gated path zeroes out every
trade because the default l2_imb=0.5 fails the imb_long=0.502 and
imb_short=0.498 thresholds.  Two complementary experiments were run
instead:

  (a) FULL L2 GATES on a 19-day L2-equipped recent corpus
      (/Users/jo/omega_repo/data/l2_ticks_XAUUSD_*.csv
       + /Users/jo/Tick/l2_data/l2_ticks_*.csv).
      Real L2 + regime gates active.  Small sample (wide CI95) but
      directly answers the gated-vs-ungated question on real data.
  (b) SPREAD-ONLY GATE on the full 623-day Dukascopy corpus, achieved
      by passing --gated --imb-long 0.5 --imb-short 0.5.  This
      neutralizes the L2 part of the gate; regime/watchdog defaults
      are inert on tick-only data; the only remaining filter is
      "spread > 1.0 USD/oz blocks the signal".

This script aggregates all four CSVs (ungated 623d, spread-only 623d,
ungated 19d L2, gated 19d L2) with the same 1000-iter bootstrap CI95
methodology, then emits:

  outputs/s31_gated_compare_summary.csv   (all cells × conditions)
  outputs/s31_gated_compare_verdict.md    (TOP-1 across conditions
                                           + per-condition CI-positive
                                           cell counts + headline verdict)
"""
from __future__ import annotations

import csv
import sys
from collections import defaultdict
from pathlib import Path
from typing import List, Tuple

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]

COMMISSION_RT   = 0.06
BOOTSTRAP_ITERS = 1000
BOOTSTRAP_SEED  = 42
CI_PCT          = 95.0

# The S30 new TOP-1 candidate
TOP1 = {"tp": 35.0, "sl": 12.0, "z": 2.0, "window": 200}

CONDITIONS = [
    {
        "name":   "Ungated 623d (S30 baseline)",
        "short":  "ungated_623",
        "csv":    REPO_ROOT / "outputs" / "duka_wide_fine_asia.csv",
        "n_days_expected": 623,
        "corpus": "Dukascopy 2023-09-27..2025-09-26",
        "gate":   "OFF",
    },
    {
        "name":   "Spread-only gate, 623d",
        "short":  "spread_only_623",
        "csv":    REPO_ROOT / "outputs"
                  / "duka_wide_fine_asia_gated_spread.csv",
        "n_days_expected": 623,
        "corpus": "Dukascopy 2023-09-27..2025-09-26",
        "gate":   "spread>1.0 USD/oz only (L2 neutralized via imb=0.5)",
    },
    {
        "name":   "Ungated 19d L2",
        "short":  "ungated_l2_19",
        "csv":    REPO_ROOT / "outputs" / "wide_fine_l2_ungated.csv",
        "n_days_expected": 19,
        "corpus": "BlackBull L2 captures 2026-04-09..2026-05-08",
        "gate":   "OFF",
    },
    {
        "name":   "Full L2 + regime gate, 19d L2",
        "short":  "gated_l2_19",
        "csv":    REPO_ROOT / "outputs" / "wide_fine_l2_gated.csv",
        "n_days_expected": 19,
        "corpus": "BlackBull L2 captures 2026-04-09..2026-05-08",
        "gate":   "spread>1.0 + regime in {0,2} + l2_imb thresholds",
    },
]

OUT_SUMMARY = REPO_ROOT / "outputs" / "s31_gated_compare_summary.csv"
OUT_VERDICT = REPO_ROOT / "outputs" / "s31_gated_compare_verdict.md"


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


def aggregate(csv_path: Path, condition_short: str) -> List[dict]:
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
        sums   = np.array([float(g["sum_usd"]) for g in grp],
                          dtype=np.float64)
        trades = np.array([int(g["n_trades"]) for g in grp],
                          dtype=np.int64)
        wins   = np.array([int(g["n_wins"])  for g in grp],
                          dtype=np.int64)
        n_days       = sums.size
        total_trades = int(trades.sum())
        total_wins   = int(wins.sum())
        total_gross  = float(sums.sum())
        total_net    = total_gross - total_trades * COMMISSION_RT
        wr           = (total_wins / total_trades * 100.0) \
            if total_trades else 0.0
        daily_net    = sums - trades * COMMISSION_RT
        mean_net     = float(daily_net.mean()) if n_days else 0.0
        lo_n, hi_n   = bootstrap_ci(daily_net, BOOTSTRAP_ITERS,
                                    BOOTSTRAP_SEED, CI_PCT)
        exp_net      = (total_net / total_trades) \
            if total_trades else 0.0
        out.append({
            "condition": condition_short,
            "tp": float(tp), "sl": float(sl), "z": float(z),
            "window": int(w), "fill_model": fm,
            "n_days": n_days,
            "total_trades": total_trades,
            "wr_pct": round(wr, 2),
            "total_gross_usd": round(total_gross, 2),
            "total_net_prime_usd": round(total_net, 2),
            "exp_net_per_trade": round(exp_net, 4),
            "daily_mean_net": round(mean_net, 4),
            "ci95_lo_net": round(lo_n, 4),
            "ci95_hi_net": round(hi_n, 4),
        })
    return out


def find_top1(rows: List[dict]) -> dict | None:
    """Find the TOP-1 cell (honest fill) in the aggregated rows."""
    for r in rows:
        if (r["fill_model"] == "honest"
            and r["tp"] == TOP1["tp"]
            and r["sl"] == TOP1["sl"]
            and r["z"]  == TOP1["z"]
            and r["window"] == TOP1["window"]):
            return r
    return None


def fmt_ci(r: dict) -> str:
    return f"[{r['ci95_lo_net']:+.4f}, {r['ci95_hi_net']:+.4f}]"


def verdict_label(ungated: dict, gated: dict, sample_label: str) -> str:
    """AMPLIFY / UNCHANGED / KILL by comparing daily means & CI95 lower."""
    if ungated is None or gated is None:
        return f"INCONCLUSIVE ({sample_label}: cell missing in one condition)"
    du = ungated["daily_mean_net"]
    dg = gated["daily_mean_net"]
    lu = ungated["ci95_lo_net"]
    lg = gated["ci95_lo_net"]
    # Heuristics: AMPLIFY if gated mean > ungated mean by > 10% and CI_lo strictly higher
    # UNCHANGED if |Δmean| < 10% AND CIs overlap substantially
    # KILL if gated mean is below ungated by > 20% OR gated mean flips negative
    pct = (dg - du) / abs(du) if abs(du) > 1e-9 else 0.0
    if dg < 0 and du > 0:
        return "KILL (gated mean flipped negative)"
    if pct > 0.10 and lg > lu:
        return "AMPLIFY"
    if pct < -0.20:
        return "KILL"
    return "UNCHANGED"


def write_summary(all_rows: List[dict], path: Path) -> None:
    if not all_rows:
        return
    cols = list(all_rows[0].keys())
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in all_rows:
            w.writerow(r)


def write_verdict(
    cond_rows: dict[str, List[dict]],
    path: Path,
) -> None:
    L: List[str] = []
    L.append("# S31 §3.1 — VERDICT — gated variant of the S30 TOP-1 candidate\n\n")
    L.append("**The headline question from the S30 handoff:** "
             "do the S27 L2 + regime gates AMPLIFY, leave UNCHANGED, "
             "or KILL the new TOP-1 candidate "
             "(TP=35, SL=12, z=2.0, W=200, Asia 0-7 UTC, ungated daily "
             "mean +$2.31, CI95 [+$0.79, +$3.95])?\n\n")
    L.append("**The literal §3.1 experiment cannot be run on the 623-day "
             "Dukascopy corpus** because that corpus is bid/ask-only "
             "(no `l2_imb`, no `regime`, no `watchdog_dead` columns). "
             "Defaults (`l2_imb=0.5` vs `imb_long=0.502 / imb_short=0.498`) "
             "cause the gate to block every signal: 0/256 cells trade. "
             "S31 ran two complementary experiments instead.\n\n")
    L.append(f"Methodology: 1000-iter daily-mean bootstrap CI95 "
             f"(seed={BOOTSTRAP_SEED}), commission "
             f"${COMMISSION_RT:.2f}/RT BlackBull Prime, honest fill model.\n\n")

    L.append("## Conditions compared\n\n")
    L.append("| Short | Days | Corpus | Gate |\n")
    L.append("|---|---|---|---|\n")
    for c in CONDITIONS:
        L.append(f"| `{c['short']}` | {c['n_days_expected']} | "
                 f"{c['corpus']} | {c['gate']} |\n")
    L.append("\n")

    # TOP-1 across conditions
    L.append("## ★ TOP-1 cell (TP=35, SL=12, z=2.0, W=200) across conditions ★\n\n")
    L.append("| Condition | Days | Trades | WR% | "
             "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
    L.append("|---|---|---|---|---|---|---|\n")
    top1s: dict[str, dict | None] = {}
    for c in CONDITIONS:
        rows = cond_rows.get(c["short"], [])
        t = find_top1(rows)
        top1s[c["short"]] = t
        if t is None:
            L.append(f"| {c['short']} | — | — | — | — | — | (cell missing) |\n")
        else:
            L.append(f"| {c['short']} | {t['n_days']} | "
                     f"{t['total_trades']} | {t['wr_pct']:.1f} | "
                     f"{t['total_net_prime_usd']:+.2f} | "
                     f"{t['daily_mean_net']:+.4f} | {fmt_ci(t)} |\n")
    L.append("\n")

    # Verdicts
    L.append("## Verdicts\n\n")
    v_623 = verdict_label(
        top1s.get("ungated_623"),
        top1s.get("spread_only_623"),
        "623d spread-only",
    )
    v_l2 = verdict_label(
        top1s.get("ungated_l2_19"),
        top1s.get("gated_l2_19"),
        "19d L2 full-gate",
    )
    L.append(f"- **Spread-only gate on 623d (large sample):** {v_623}\n")
    L.append(f"- **Full L2 + regime gate on 19d L2 (small sample):** {v_l2}\n\n")

    # CI-positive cell counts per condition
    L.append("## Strict CI-positive cells per condition (honest fill, "
             "CI95 lower > 0)\n\n")
    L.append("| Condition | Total cells | CI95_lo > 0 | "
             "max CI95_lo | best cell |\n")
    L.append("|---|---|---|---|---|\n")
    for c in CONDITIONS:
        rows = cond_rows.get(c["short"], [])
        honest = [r for r in rows if r["fill_model"] == "honest"]
        if not honest:
            L.append(f"| {c['short']} | — | — | — | (no rows) |\n")
            continue
        ci_pos = [r for r in honest if r["ci95_lo_net"] > 0]
        best = max(honest, key=lambda r: r["ci95_lo_net"])
        bc = (f"TP={best['tp']:.1f} SL={best['sl']:.1f} "
              f"z={best['z']:.1f} W={best['window']}")
        L.append(f"| {c['short']} | {len(honest)} | {len(ci_pos)} | "
                 f"{best['ci95_lo_net']:+.4f} | {bc} |\n")
    L.append("\n")

    # Top-10 strict CI-positive per condition
    for c in CONDITIONS:
        rows = cond_rows.get(c["short"], [])
        honest = [r for r in rows if r["fill_model"] == "honest"]
        ci_pos = [r for r in honest if r["ci95_lo_net"] > 0]
        L.append(f"## `{c['short']}` — top 10 by CI95 lower\n\n")
        if not honest:
            L.append("(no data)\n\n")
            continue
        ranked = sorted(honest, key=lambda r: r["ci95_lo_net"], reverse=True)
        L.append("| Rank | TP | SL | z | W | Trades | WR% | "
                 "Total net $ | Daily mean net $ | CI95 net daily $ |\n")
        L.append("|---|---|---|---|---|---|---|---|---|---|\n")
        for i, r in enumerate(ranked[:10], 1):
            mark = " ★" if r["ci95_lo_net"] > 0 else ""
            L.append(f"| {i}{mark} | {r['tp']:.1f} | {r['sl']:.1f} | "
                     f"{r['z']:.1f} | {r['window']} | "
                     f"{r['total_trades']} | {r['wr_pct']:.1f} | "
                     f"{r['total_net_prime_usd']:+.2f} | "
                     f"{r['daily_mean_net']:+.4f} | {fmt_ci(r)} |\n")
        L.append(f"\nCI-positive cells: **{len(ci_pos)} / {len(honest)}**\n\n")

    L.append("## Interpretation guidance\n\n")
    L.append("- The **19d L2 sample is small**; CI95 widths are wide and "
             "regime is recent (2026-04..05). It cannot disprove the "
             "623-day ungated edge — it can only show whether gates "
             "directionally help or hurt on the days where we have real "
             "L2 data.\n")
    L.append("- The **623d spread-only gate** isolates the effect of "
             "avoiding wide-spread fills. The S27 L2 + regime gates are "
             "not testable on this corpus; the spread filter is the only "
             "L2-independent filter from the gate stack.\n")
    L.append("- A clean **AMPLIFY** would be: spread-only gate raises "
             "the daily mean and lifts CI95_lo even higher above zero. "
             "A **KILL** would be: spread-only gate cuts the mean by "
             "more than 20% or flips it negative.\n")
    L.append("- The S30 ungated TOP-1 daily mean was +$2.3113, "
             "CI95 [+$0.79, +$3.95]. Reference values for comparison.\n\n")

    L.append("## DO NOT deploy live based on this verdict\n\n")
    L.append("Operator owns the live-trade decision. Mode=SHADOW remains "
             "in effect. max_lot_gold=0.01 remains in effect. Both the "
             "ungated S30 TOP-1 and any gated variant uncovered here "
             "remain candidates, NOT deployable strategies.\n")
    path.write_text("".join(L))


def main() -> int:
    all_rows: List[dict] = []
    cond_rows: dict[str, List[dict]] = {}
    for c in CONDITIONS:
        rows = aggregate(c["csv"], c["short"])
        print(f"[INFO] aggregated {len(rows):>5} (cell × fill) tuples "
              f"from {c['csv'].name}  -> {c['name']}")
        cond_rows[c["short"]] = rows
        all_rows.extend(rows)
    write_summary(all_rows, OUT_SUMMARY)
    write_verdict(cond_rows, OUT_VERDICT)
    print(f"[INFO] wrote {OUT_SUMMARY}")
    print(f"[INFO] wrote {OUT_VERDICT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
