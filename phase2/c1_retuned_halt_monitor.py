#!/usr/bin/env python3
"""
phase2/c1_retuned_halt_monitor.py
==================================

Read-only monitor for the C1_retuned shadow ledger. Designed to run
independently of the main shadow runner so a human or supervisor cron
can audit halt status without re-running the simulation.

What it checks
--------------
1. **Drawdown breach.** Rolling drawdown from peak equity. Breach when
   max DD <= HALT_DD_PCT (default -7.5%, ie 1.5x the backtest -5.85%).

2. **Cluster-day frequency.** A cluster day = >= 4 cells losing in the
   same UTC session. Halt when observed cluster days >= 2x the expected
   rate (per CHOSEN.md: 6 cluster days in 26 backtest months).

3. **Cell concentration.** If ANY cell contributes < -50% of total P&L
   on its own, flag — that's regime divergence on one cell.

4. **Daily P&L distribution.** Reports worst-day, worst-week, win-day
   share, current streak.

5. **Cells silent.** Each cell should fire at least once per ~14 days
   on the backtest cadence; flag any cell that hasn't fired in 21 days.

Usage
-----
    python3 phase2/c1_retuned_halt_monitor.py
        [--ledger PATH]    default phase2/live_shadow/c1_retuned_shadow.csv
        [--report PATH]    default phase2/live_shadow/halt_report_<UTC>.md
        [--quiet]          do not print to stdout
        [--exit-on-breach] return code 3 if any breach (cron-friendly)

Exit codes
----------
    0  no breach (or no data yet)
    2  argument / file error
    3  breach detected (only if --exit-on-breach passed)

This monitor does NOT modify any core code, the shadow runner, or any
ledgers. It only reads + reports. The shadow runner has its own light
halt check; this monitor is the deeper periodic audit.
"""
from __future__ import annotations

import argparse
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import pandas as pd

# -- Locked thresholds -------------------------------------------------------
HALT_DD_PCT = -0.075                        # 1.5x backtest -5.85%
HALT_CLUSTER_MIN_CELLS = 4                  # 4-cell loss day = "cluster"
HALT_CLUSTER_MULT = 2.0                     # observed/expected ratio
BACKTEST_CLUSTER_DAYS = 6
BACKTEST_MONTHS = 26.0
EXPECTED_CLUSTERS_PER_MONTH = BACKTEST_CLUSTER_DAYS / BACKTEST_MONTHS  # 0.231
START_EQUITY = 10_000.0
SILENT_CELL_DAYS_WARN = 21                  # days without fire → warn
EXPECTED_CELLS = [
    "donchian_H1_long_retuned",
    "bollinger_H2_long",
    "bollinger_H4_long",
    "bollinger_H6_long",
]
CELL_DOMINANCE_NEG_PCT = -0.50              # one cell <= -50% of total = flag

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LEDGER = REPO_ROOT / "phase2" / "live_shadow" / "c1_retuned_shadow.csv"
DEFAULT_OUT_DIR = REPO_ROOT / "phase2" / "live_shadow"


# ----------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------

def _load_ledger(path: Path) -> pd.DataFrame:
    if not path.exists():
        return pd.DataFrame()
    df = pd.read_csv(path)
    if df.empty:
        return df
    for c in ("entry_time", "exit_time"):
        if c in df.columns:
            df[c] = pd.to_datetime(df[c], utc=True)
    if "shadow_run_ts" in df.columns:
        df["shadow_run_ts"] = pd.to_datetime(df["shadow_run_ts"], utc=True)
    return df.sort_values("exit_time").reset_index(drop=True)


def _now_utc() -> pd.Timestamp:
    t = pd.Timestamp.utcnow()
    return t.tz_localize("UTC") if t.tzinfo is None else t


def _fmt_money(x: float) -> str:
    return f"${x:,.2f}"


def _fmt_pct(x: float) -> str:
    return f"{x*100:+.2f}%"


# ----------------------------------------------------------------------------
# Analyses
# ----------------------------------------------------------------------------

def analyse_drawdown(ledger: pd.DataFrame) -> Dict:
    if ledger.empty:
        return {"max_dd_pct": 0.0, "current_dd_pct": 0.0, "peak_equity": START_EQUITY,
                "current_equity": START_EQUITY, "breach": False}
    eq = START_EQUITY + ledger["pnl"].cumsum()
    peak = eq.cummax()
    dd = (eq - peak) / peak
    max_dd = float(dd.min())
    current_dd = float(dd.iloc[-1])
    return {
        "max_dd_pct": max_dd,
        "current_dd_pct": current_dd,
        "peak_equity": float(peak.iloc[-1]),
        "current_equity": float(eq.iloc[-1]),
        "breach": max_dd <= HALT_DD_PCT,
    }


def analyse_clusters(ledger: pd.DataFrame,
                     shadow_start: pd.Timestamp) -> Dict:
    if ledger.empty:
        return {"cluster_days": 0, "expected": 0.0, "ratio": 0.0,
                "breach": False, "details": pd.DataFrame()}
    df = ledger.copy()
    df["day"] = df["exit_time"].dt.tz_convert("UTC").dt.date
    df["is_loss"] = df["pnl"] < 0
    by_day = (
        df[df["is_loss"]]
        .groupby("day")["combo_id"]
        .nunique()
        .rename("losing_cells")
    )
    cluster_days_idx = by_day[by_day >= HALT_CLUSTER_MIN_CELLS].index
    cluster_count = int(len(cluster_days_idx))

    elapsed_days = max(
        1.0,
        (_now_utc() - shadow_start).total_seconds() / 86400.0,
    )
    elapsed_months = elapsed_days / 30.4375
    expected = EXPECTED_CLUSTERS_PER_MONTH * elapsed_months
    ratio = (cluster_count / expected) if expected > 0 else 0.0

    details_rows = []
    for d in cluster_days_idx:
        sub = df[df["day"] == d]
        details_rows.append({
            "day": str(d),
            "losing_cells": int(by_day.loc[d]),
            "total_pnl": float(sub["pnl"].sum()),
            "cells": ",".join(sorted(sub.loc[sub["is_loss"], "combo_id"].unique())),
        })
    details = pd.DataFrame(details_rows)
    if not details.empty:
        details = details.sort_values("total_pnl").reset_index(drop=True)

    return {
        "cluster_days": cluster_count,
        "expected": expected,
        "ratio": ratio,
        "breach": (cluster_count >= 1) and (ratio >= HALT_CLUSTER_MULT),
        "details": details,
        "elapsed_days": elapsed_days,
    }


def analyse_cell_concentration(ledger: pd.DataFrame) -> Dict:
    if ledger.empty:
        return {"per_cell": pd.DataFrame(), "breach": False, "worst_cell": None,
                "worst_share": 0.0}
    per_cell = (
        ledger.groupby("combo_id")["pnl"]
        .agg(["count", "sum"])
        .rename(columns={"count": "n", "sum": "net_pnl"})
        .sort_values("net_pnl")
    )
    total = per_cell["net_pnl"].sum()
    if total == 0:
        per_cell["share"] = 0.0
    else:
        per_cell["share"] = per_cell["net_pnl"] / abs(total)
    worst = per_cell.iloc[0]
    breach = bool(worst["share"] <= CELL_DOMINANCE_NEG_PCT and worst["net_pnl"] < 0)
    return {
        "per_cell": per_cell,
        "breach": breach,
        "worst_cell": worst.name if breach else None,
        "worst_share": float(worst["share"]),
    }


def analyse_daily(ledger: pd.DataFrame) -> Dict:
    if ledger.empty:
        return {"worst_day": None, "best_day": None, "win_day_pct": 0.0,
                "n_days": 0, "current_streak": 0}
    df = ledger.copy()
    df["day"] = df["exit_time"].dt.tz_convert("UTC").dt.date
    by_day = df.groupby("day")["pnl"].sum().sort_index()
    n_days = len(by_day)
    win_days = int((by_day > 0).sum())
    win_pct = win_days / n_days if n_days else 0.0
    worst = by_day.idxmin() if n_days else None
    best = by_day.idxmax() if n_days else None

    # Current streak: count of consecutive same-sign days at the tail
    streak = 0
    if n_days:
        last_sign = np.sign(by_day.iloc[-1])
        for v in reversed(by_day.values):
            if np.sign(v) == last_sign and last_sign != 0:
                streak += 1
            else:
                break
        streak = int(streak * (1 if last_sign >= 0 else -1))

    return {
        "n_days": n_days,
        "win_day_pct": win_pct,
        "worst_day": (str(worst), float(by_day.loc[worst])) if worst else None,
        "best_day": (str(best), float(by_day.loc[best])) if best else None,
        "current_streak": streak,
    }


def analyse_silent_cells(ledger: pd.DataFrame) -> Dict:
    now = _now_utc()
    out: List[Dict] = []
    for cell in EXPECTED_CELLS:
        sub = ledger[ledger["combo_id"] == cell] if not ledger.empty else pd.DataFrame()
        if sub.empty:
            out.append({
                "cell": cell, "last_seen": None, "days_silent": None,
                "n_total": 0, "warn": False,
            })
            continue
        last = sub["entry_time"].max()
        days = (now - last).total_seconds() / 86400.0
        out.append({
            "cell": cell,
            "last_seen": last.isoformat(),
            "days_silent": days,
            "n_total": int(len(sub)),
            "warn": days >= SILENT_CELL_DAYS_WARN,
        })
    breach = any(r["warn"] for r in out)
    return {"cells": out, "breach": breach}


# ----------------------------------------------------------------------------
# Report
# ----------------------------------------------------------------------------

def render_report(ledger: pd.DataFrame,
                  shadow_start: pd.Timestamp,
                  dd: Dict, clusters: Dict, conc: Dict,
                  daily: Dict, silent: Dict) -> str:
    now = _now_utc()
    any_breach = dd["breach"] or clusters["breach"] or conc["breach"] or silent["breach"]
    overall = "BREACH" if any_breach else "OK"

    lines: List[str] = []
    lines.append(f"# C1_retuned Halt Monitor — {now.strftime('%Y-%m-%d %H:%M:%S UTC')}")
    lines.append("")
    lines.append(f"**Overall status: {overall}**")
    lines.append("")
    lines.append(f"- shadow_start  : {shadow_start.isoformat()}")
    lines.append(f"- elapsed_days  : {clusters.get('elapsed_days', 0.0):.2f}")
    lines.append(f"- trades        : {len(ledger)}")
    lines.append(f"- current eq    : {_fmt_money(dd['current_equity'])} "
                 f"({_fmt_pct(dd['current_equity']/START_EQUITY - 1)})")
    lines.append("")
    lines.append("## 1. Drawdown")
    lines.append("")
    lines.append(f"- max DD        : {_fmt_pct(dd['max_dd_pct'])}  "
                 f"(threshold {_fmt_pct(HALT_DD_PCT)})")
    lines.append(f"- current DD    : {_fmt_pct(dd['current_dd_pct'])}")
    lines.append(f"- peak equity   : {_fmt_money(dd['peak_equity'])}")
    lines.append(f"- BREACH        : {dd['breach']}")
    lines.append("")
    lines.append("## 2. Cluster-day frequency")
    lines.append("")
    lines.append(f"- cluster days  : {clusters['cluster_days']}  "
                 f"(>= {HALT_CLUSTER_MIN_CELLS} cells losing same UTC session)")
    lines.append(f"- expected      : {clusters['expected']:.2f}  "
                 f"(based on backtest rate {EXPECTED_CLUSTERS_PER_MONTH:.3f}/month)")
    lines.append(f"- ratio         : {clusters['ratio']:.2f}x  "
                 f"(threshold {HALT_CLUSTER_MULT}x)")
    lines.append(f"- BREACH        : {clusters['breach']}")
    if not clusters["details"].empty:
        lines.append("")
        lines.append("### Cluster days detail")
        lines.append("")
        lines.append("```")
        lines.append(clusters["details"].to_string(index=False))
        lines.append("```")
    lines.append("")
    lines.append("## 3. Cell concentration")
    lines.append("")
    if not conc["per_cell"].empty:
        lines.append("```")
        pc = conc["per_cell"].copy()
        pc["net_pnl"] = pc["net_pnl"].round(2)
        pc["share"] = (pc["share"] * 100).round(1).astype(str) + "%"
        lines.append(pc.to_string())
        lines.append("```")
    lines.append("")
    lines.append(f"- worst cell    : {conc['worst_cell']} "
                 f"(share {conc['worst_share']*100:+.1f}%, threshold "
                 f"{CELL_DOMINANCE_NEG_PCT*100:+.0f}%)")
    lines.append(f"- BREACH        : {conc['breach']}")
    lines.append("")
    lines.append("## 4. Daily distribution")
    lines.append("")
    lines.append(f"- trading days  : {daily['n_days']}")
    lines.append(f"- win-day %     : {daily['win_day_pct']*100:.1f}%")
    if daily["worst_day"]:
        lines.append(f"- worst day     : {daily['worst_day'][0]}  "
                     f"({_fmt_money(daily['worst_day'][1])})")
    if daily["best_day"]:
        lines.append(f"- best day      : {daily['best_day'][0]}  "
                     f"({_fmt_money(daily['best_day'][1])})")
    lines.append(f"- current streak: {daily['current_streak']}  "
                 f"({'win' if daily['current_streak']>0 else 'loss' if daily['current_streak']<0 else 'flat'})")
    lines.append("")
    lines.append("## 5. Silent cells")
    lines.append("")
    for r in silent["cells"]:
        last = r["last_seen"] or "never"
        days = f"{r['days_silent']:.1f}d" if r["days_silent"] is not None else "n/a"
        lines.append(f"- {r['cell']:<32}  last_seen={last}  silent={days}  "
                     f"n_total={r['n_total']}  warn={r['warn']}")
    lines.append("")
    lines.append(f"- BREACH        : {silent['breach']}")
    lines.append("")
    lines.append("---")
    lines.append("")
    if any_breach:
        lines.append("**Action required.** Review the breach above. Per CHOSEN.md, "
                     "halt criteria during shadow: pause and re-evaluate before "
                     "going further. Investigate the regime / DXY / vol / session "
                     "conditions that drove the breach.")
    else:
        lines.append("All halt-criteria within bounds. C1_retuned shadow continues.")
    return "\n".join(lines)


# ----------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="c1_retuned_halt_monitor",
        description="Read-only halt-criteria monitor for the C1_retuned shadow ledger.",
    )
    p.add_argument("--ledger", type=str, default=str(DEFAULT_LEDGER),
                   help=f"Path to shadow ledger CSV (default: {DEFAULT_LEDGER})")
    p.add_argument("--out-dir", type=str, default=str(DEFAULT_OUT_DIR),
                   help=f"Directory for the report (default: {DEFAULT_OUT_DIR})")
    p.add_argument("--report", type=str, default=None,
                   help="Override report file path; default <out-dir>/halt_report_<UTC>.md")
    p.add_argument("--quiet", action="store_true",
                   help="Do not print report to stdout.")
    p.add_argument("--exit-on-breach", action="store_true",
                   help="Return exit code 3 if any breach detected (cron-friendly).")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    ledger_path = Path(args.ledger)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    now = _now_utc()
    report_path = Path(args.report) if args.report else \
        out_dir / f"halt_report_{now.strftime('%Y%m%dT%H%M%SZ')}.md"

    ledger = _load_ledger(ledger_path)
    # Resolve shadow_start from the metadata file if present, else infer
    meta_path = out_dir / "last_run.json"
    shadow_start: pd.Timestamp
    if meta_path.exists():
        try:
            import json
            payload = json.loads(meta_path.read_text())
            shadow_start = pd.to_datetime(payload.get("shadow_start"), utc=True)
        except Exception:
            shadow_start = ledger["entry_time"].min() if not ledger.empty else _now_utc()
    else:
        shadow_start = ledger["entry_time"].min() if not ledger.empty else _now_utc()

    dd = analyse_drawdown(ledger)
    clusters = analyse_clusters(ledger, shadow_start)
    conc = analyse_cell_concentration(ledger)
    daily = analyse_daily(ledger)
    silent = analyse_silent_cells(ledger)

    report = render_report(ledger, shadow_start, dd, clusters, conc, daily, silent)

    if not args.quiet:
        print(report)

    report_path.write_text(report)
    if not args.quiet:
        print(f"\n[ok] report written: {report_path}")

    any_breach = dd["breach"] or clusters["breach"] or conc["breach"] or silent["breach"]
    if args.exit_on_breach and any_breach:
        return 3
    return 0


if __name__ == "__main__":
    try:
        rc = main()
    except SystemExit:
        raise
    except Exception as e:
        import traceback
        traceback.print_exc()
        print(f"\nFATAL: {type(e).__name__}: {e}", file=sys.stderr)
        rc = 2
    sys.exit(rc)
