#!/usr/bin/env python3
"""
phase2/c1_retuned_shadow.py
============================

Live-shadow runner for the C1_retuned portfolio (System B).

What it is
----------
"Switch on" entry point for shadow paper-trading the verdict from
`phase2/donchian_postregime/CHOSEN.md`:

    Donchian H1 long retuned (period=20, sl_atr=3.0, tp_r=5.0)
    + Bollinger H2 long
    + Bollinger H4 long
    + Bollinger H6 long
    max_concurrent = 4
    risk_pct       = 0.5%
    start_equity   = $10,000
    margin_call    = $1,000

This is the same C1_retuned variant that A/B-dominated C1_max4 in
`phase2/portfolio_C1_C2.py` (return +74.12%, max DD -5.85%, PF 1.486,
Sharpe 2.651, WR 55.2%; walk-forward TRAIN/VALIDATE/TEST all PASS).

What it does (each invocation)
------------------------------
1. Reads the four cell ledgers from `phase1/trades_net/`:
     - donchian_H1_long_3.0_5.0_net.parquet  (the retuned ledger)
     - bollinger_H2_long_net.parquet
     - bollinger_H4_long_net.parquet
     - bollinger_H6_long_net.parquet
2. Reuses `phase2/portfolio_C1_C2.simulate()` to apply the C1_retuned
   portfolio rules (the canonical sim — no duplicated logic).
3. Filters the resulting trade list to entries where
   entry_time >= SHADOW_START.
4. Loads the cumulative shadow ledger CSV (if it exists) at
   `phase2/live_shadow/c1_retuned_shadow.csv`. Computes the diff vs
   what's already on disk and APPENDS only new trades.
5. Writes/updates:
     phase2/live_shadow/c1_retuned_shadow.csv         cumulative trade ledger
     phase2/live_shadow/c1_retuned_equity.csv         cumulative equity curve
     phase2/live_shadow/c1_retuned_summary_<UTC>.md   per-run summary
     phase2/live_shadow/last_run.json                 metadata + run history
6. Idempotent. Safe to cron hourly / daily; producing nothing is normal
   when no new trades have closed (e.g., bars haven't been refreshed yet).

How "live" trades appear in the ledger
--------------------------------------
The C1_retuned cells are deterministic functions of the bars in
`phase0/bars_<TF>_final.parquet`. New bars arrive via your existing
download/refresh pipeline (download_dukascopy.py + tick reindex + the
bar-build steps that produced the current parquets). Each time you
refresh bars and rebuild the per-cell ledgers, this script re-runs the
portfolio sim and emits any new closed trades to the shadow ledger.

The script does not fetch data itself. Bar refresh is intentionally
decoupled — wire it via cron / launchd separately. See README at
`phase2/C1_RETUNED_SHADOW.md` for operational details.

CLI
---
    python3 phase2/c1_retuned_shadow.py
        [--shadow-start ISO]   (default: today 00:00 UTC; first run sets it)
        [--out-dir PATH]       (default: phase2/live_shadow)
        [--phase1-dir PATH]    (default: phase1/trades_net)
        [--dry-run]            (do not write any files)
        [--reset]              (delete the cumulative ledger and start over)

Exit codes
----------
    0  success (new trades found OR no-op)
    1  data missing (one or more cell ledgers absent)
    2  argument / config error
    3  halt-criteria tripped (writes a HALT.flag in out-dir)

Halt-criteria check (light)
---------------------------
This runner emits a halt flag when the cumulative shadow ledger shows:
    - cluster days (4+ cells losing same UTC session) > 2x expected
      frequency, normalised by elapsed shadow days,
or
    - drawdown breach: rolling DD from peak > -7.5% (1.5x backtest -5.85%).

The dedicated halt monitor (`phase2/c1_retuned_halt_monitor.py`) does the
deeper analysis on demand; this runner only emits a coarse breach flag
so the cron path can short-circuit if something is clearly wrong.

Conventions
-----------
- All times stored as ISO-8601 UTC strings (matches portfolio_C1_C2 output).
- Numeric columns: equity, pnl, lot stored as floats with 6dp.
- This file does NOT modify any core code (sim_lib.py, portfolio_C1_C2.py,
  any C++ engine source). It only imports portfolio_C1_C2 as a library.
"""
from __future__ import annotations

import argparse
import json
import sys
import os
from dataclasses import asdict
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import pandas as pd

# -- Local imports (reuse canonical sim) -------------------------------------
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "phase2"))

# portfolio_C1_C2 is the canonical simulator. We import its pure functions
# rather than re-implement them, so the shadow ledger and the backtest
# always agree on sizing / fills / margin behaviour.
import portfolio_C1_C2 as canon  # noqa: E402

# -- Constants (locked to CHOSEN.md verdict) ---------------------------------
VARIANT_NAME = "C1_retuned"
VARIANT_CFG = canon.VARIANTS[VARIANT_NAME]
START_EQUITY = canon.START_EQUITY
MARGIN_CALL = canon.MARGIN_CALL
PHASE1_DEFAULT = canon.PHASE1_DIR
OUT_DEFAULT = REPO_ROOT / "phase2" / "live_shadow"

# Halt thresholds (from CHOSEN.md and TL;DR halt rule)
HALT_DD_PCT = -0.075  # 1.5x the backtest -5.85% max DD
HALT_CLUSTER_MIN_CELLS = 4  # 4 cells losing same UTC session = "cluster day"
HALT_CLUSTER_MULT = 2.0     # >2x expected frequency in shadow window

# Backtest reference for cluster-rate normalisation
# From CHOSEN.md: 6 cluster days over the full backtest window 2024-03..2026-04
# (~26 months). Expected cluster rate ~6/26 ≈ 0.23 cluster-days/month.
BACKTEST_CLUSTER_DAYS = 6
BACKTEST_MONTHS = 26.0
EXPECTED_CLUSTERS_PER_MONTH = BACKTEST_CLUSTER_DAYS / BACKTEST_MONTHS

LEDGER_COLS = [
    "combo_id",
    "entry_time",
    "exit_time",
    "lot",
    "pnl",
    "equity_after",
    "shadow_run_ts",
]
EQUITY_COLS = ["time", "equity", "shadow_run_ts"]


# ----------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------

def _utc_now() -> pd.Timestamp:
    return pd.Timestamp.utcnow().tz_localize(None).tz_localize("UTC") \
        if pd.Timestamp.utcnow().tzinfo is None else pd.Timestamp.utcnow()


def _parse_iso(s: str) -> pd.Timestamp:
    ts = pd.to_datetime(s, utc=True)
    if pd.isna(ts):
        raise ValueError(f"Could not parse ISO timestamp: {s!r}")
    return ts


def _today_utc_midnight() -> pd.Timestamp:
    now = pd.Timestamp.utcnow()
    if now.tzinfo is None:
        now = now.tz_localize("UTC")
    return pd.Timestamp(year=now.year, month=now.month, day=now.day, tz="UTC")


def _ensure_out_dir(out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)


def _check_inputs(phase1_dir: Path) -> List[str]:
    """Return list of missing cell ledgers (empty if all present)."""
    missing = []
    for cell in VARIANT_CFG["cells"]:
        spec = canon.ALL_CELLS[cell]
        path = phase1_dir / spec["file"]
        if not path.exists():
            missing.append(str(path))
    return missing


# ----------------------------------------------------------------------------
# Core run
# ----------------------------------------------------------------------------

def run_simulation(phase1_dir: Path) -> pd.DataFrame:
    """Run the C1_retuned simulation and return the trade ledger as a
    DataFrame indexed by exit_time. Reuses canonical loader + simulator."""
    # Patch the canonical phase1 dir if user overrode it
    if phase1_dir != PHASE1_DEFAULT:
        canon.PHASE1_DIR = phase1_dir

    trades_all = canon.load_all_cells(VARIANT_CFG["cells"])
    result = canon.simulate(
        VARIANT_NAME,
        trades_all,
        VARIANT_CFG["risk_pct"],
        VARIANT_CFG["max_concurrent"],
    )
    df = result.trades.copy()
    if df.empty:
        return df
    df["entry_time"] = pd.to_datetime(df["entry_time"], utc=True)
    df["exit_time"] = pd.to_datetime(df["exit_time"], utc=True)
    return df


def filter_to_shadow_window(trades: pd.DataFrame,
                            shadow_start: pd.Timestamp) -> pd.DataFrame:
    if trades.empty:
        return trades
    return trades[trades["entry_time"] >= shadow_start].reset_index(drop=True)


def load_existing_ledger(path: Path) -> pd.DataFrame:
    if not path.exists():
        return pd.DataFrame(columns=LEDGER_COLS)
    df = pd.read_csv(path)
    if df.empty:
        return pd.DataFrame(columns=LEDGER_COLS)
    df["entry_time"] = pd.to_datetime(df["entry_time"], utc=True)
    df["exit_time"] = pd.to_datetime(df["exit_time"], utc=True)
    if "shadow_run_ts" in df.columns:
        df["shadow_run_ts"] = pd.to_datetime(df["shadow_run_ts"], utc=True)
    return df


def diff_new_trades(simulated: pd.DataFrame,
                    existing: pd.DataFrame) -> pd.DataFrame:
    """Return rows in simulated that are not already in existing.
    Match key: (combo_id, entry_time, exit_time)."""
    if simulated.empty:
        return simulated
    if existing.empty:
        return simulated.copy()
    key_cols = ["combo_id", "entry_time", "exit_time"]
    merged = simulated.merge(
        existing[key_cols].assign(_seen=True),
        on=key_cols,
        how="left",
    )
    new_rows = merged[merged["_seen"].isna()].drop(columns=["_seen"])
    return new_rows.reset_index(drop=True)


def build_equity_curve(ledger: pd.DataFrame) -> pd.DataFrame:
    """Cumulative equity curve from the ledger, recomputed each run.
    Equity column already exists per row but we re-derive to guarantee
    monotonicity given the shadow-window cut."""
    if ledger.empty:
        return pd.DataFrame(columns=EQUITY_COLS)
    sorted_l = ledger.sort_values("exit_time").reset_index(drop=True)
    eq = START_EQUITY + sorted_l["pnl"].cumsum()
    out = pd.DataFrame({
        "time": sorted_l["exit_time"],
        "equity": eq.values,
    })
    if "shadow_run_ts" in sorted_l.columns:
        out["shadow_run_ts"] = sorted_l["shadow_run_ts"].values
    return out


def compute_halt_status(ledger: pd.DataFrame,
                        shadow_start: pd.Timestamp) -> Dict:
    """Light halt-criteria check. Returns status dict with breach flags."""
    if ledger.empty:
        return {
            "ok": True,
            "trades": 0,
            "current_equity": START_EQUITY,
            "max_dd_pct": 0.0,
            "cluster_days": 0,
            "expected_clusters": 0.0,
            "cluster_ratio": 0.0,
            "dd_breach": False,
            "cluster_breach": False,
            "reason": "no trades yet",
        }
    sorted_l = ledger.sort_values("exit_time").reset_index(drop=True)
    eq = START_EQUITY + sorted_l["pnl"].cumsum()
    peak = eq.cummax()
    dd = (eq - peak) / peak
    max_dd = float(dd.min()) if len(dd) else 0.0

    # Cluster days
    sorted_l["day"] = sorted_l["exit_time"].dt.tz_convert("UTC").dt.date
    sorted_l["is_loss"] = sorted_l["pnl"] < 0
    losing_cells = (
        sorted_l[sorted_l["is_loss"]]
        .groupby("day")["combo_id"]
        .nunique()
    )
    cluster_days = int((losing_cells >= HALT_CLUSTER_MIN_CELLS).sum())

    elapsed_days = max(
        1.0,
        (pd.Timestamp.utcnow().tz_localize("UTC") - shadow_start).total_seconds() / 86400.0,
    )
    elapsed_months = elapsed_days / 30.4375
    expected_clusters = EXPECTED_CLUSTERS_PER_MONTH * elapsed_months
    cluster_ratio = (cluster_days / expected_clusters) if expected_clusters > 0 else 0.0

    dd_breach = max_dd <= HALT_DD_PCT
    cluster_breach = (cluster_days >= 1) and (cluster_ratio >= HALT_CLUSTER_MULT)
    ok = not (dd_breach or cluster_breach)

    reasons = []
    if dd_breach:
        reasons.append(f"DD {max_dd*100:.2f}% breached threshold {HALT_DD_PCT*100:.2f}%")
    if cluster_breach:
        reasons.append(
            f"cluster_days={cluster_days} ratio={cluster_ratio:.2f}x "
            f"(threshold {HALT_CLUSTER_MULT}x, expected~{expected_clusters:.2f})"
        )
    return {
        "ok": ok,
        "trades": int(len(sorted_l)),
        "current_equity": float(eq.iloc[-1]),
        "max_dd_pct": max_dd,
        "cluster_days": cluster_days,
        "expected_clusters": expected_clusters,
        "cluster_ratio": cluster_ratio,
        "dd_breach": dd_breach,
        "cluster_breach": cluster_breach,
        "reason": "; ".join(reasons) if reasons else "within bounds",
    }


# ----------------------------------------------------------------------------
# Output writers
# ----------------------------------------------------------------------------

def write_ledger(path: Path, ledger: pd.DataFrame) -> None:
    out = ledger.copy()
    if not out.empty:
        out["entry_time"] = out["entry_time"].dt.strftime("%Y-%m-%dT%H:%M:%S%z")
        out["exit_time"] = out["exit_time"].dt.strftime("%Y-%m-%dT%H:%M:%S%z")
        if "shadow_run_ts" in out.columns:
            out["shadow_run_ts"] = pd.to_datetime(out["shadow_run_ts"], utc=True) \
                .dt.strftime("%Y-%m-%dT%H:%M:%S%z")
        for col in ["lot", "pnl", "equity_after"]:
            if col in out.columns:
                out[col] = out[col].astype(float).round(6)
    out[LEDGER_COLS].to_csv(path, index=False)


def write_equity(path: Path, eq: pd.DataFrame) -> None:
    out = eq.copy()
    if not out.empty:
        out["time"] = pd.to_datetime(out["time"], utc=True) \
            .dt.strftime("%Y-%m-%dT%H:%M:%S%z")
        out["equity"] = out["equity"].astype(float).round(6)
        if "shadow_run_ts" in out.columns:
            out["shadow_run_ts"] = pd.to_datetime(out["shadow_run_ts"], utc=True) \
                .dt.strftime("%Y-%m-%dT%H:%M:%S%z")
    cols = [c for c in EQUITY_COLS if c in out.columns]
    out[cols].to_csv(path, index=False)


def write_summary_md(path: Path,
                     ledger: pd.DataFrame,
                     new_trades: pd.DataFrame,
                     halt: Dict,
                     shadow_start: pd.Timestamp,
                     run_ts: pd.Timestamp) -> None:
    lines: List[str] = []
    lines.append(f"# C1_retuned Shadow Run — {run_ts.strftime('%Y-%m-%d %H:%M:%S UTC')}")
    lines.append("")
    lines.append(f"- shadow_start : {shadow_start.isoformat()}")
    lines.append(f"- variant      : {VARIANT_NAME} (4 cells, max_concurrent=4, risk=0.5%)")
    lines.append(f"- start_equity : ${START_EQUITY:,.2f}")
    lines.append(f"- new trades   : {len(new_trades)}")
    lines.append(f"- total trades : {len(ledger)}")
    lines.append(f"- equity now   : ${halt['current_equity']:,.2f}")
    lines.append(f"- return       : {(halt['current_equity']/START_EQUITY - 1)*100:+.2f}%")
    lines.append(f"- max DD       : {halt['max_dd_pct']*100:+.2f}%")
    lines.append(f"- cluster days : {halt['cluster_days']} "
                 f"(expected ~{halt['expected_clusters']:.2f}, "
                 f"ratio {halt['cluster_ratio']:.2f}x)")
    lines.append(f"- halt status  : {'OK' if halt['ok'] else 'BREACH'} — {halt['reason']}")
    lines.append("")
    if not new_trades.empty:
        lines.append("## New trades this run")
        lines.append("")
        nt = new_trades.copy()
        nt["entry_time"] = nt["entry_time"].dt.strftime("%Y-%m-%d %H:%M")
        nt["exit_time"] = nt["exit_time"].dt.strftime("%Y-%m-%d %H:%M")
        nt["pnl"] = nt["pnl"].round(2)
        nt["lot"] = nt["lot"].round(4)
        nt["equity_after"] = nt["equity_after"].round(2)
        lines.append(nt[["combo_id", "entry_time", "exit_time", "lot", "pnl",
                         "equity_after"]].to_string(index=False))
        lines.append("")
    else:
        lines.append("_No new trades since previous run._")
        lines.append("")
    path.write_text("\n".join(lines))


def write_metadata(path: Path, payload: Dict) -> None:
    path.write_text(json.dumps(payload, indent=2, default=str))


def write_halt_flag(out_dir: Path, halt: Dict, run_ts: pd.Timestamp) -> None:
    flag_path = out_dir / "HALT.flag"
    payload = {
        "tripped_at": run_ts.isoformat(),
        "reason": halt["reason"],
        "max_dd_pct": halt["max_dd_pct"],
        "cluster_days": halt["cluster_days"],
        "cluster_ratio": halt["cluster_ratio"],
        "current_equity": halt["current_equity"],
    }
    flag_path.write_text(json.dumps(payload, indent=2, default=str))


# ----------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="c1_retuned_shadow",
        description="Live-shadow runner for the C1_retuned portfolio.",
    )
    p.add_argument("--shadow-start", type=str, default=None,
                   help="ISO-8601 UTC timestamp; trades with entry_time >= this "
                        "go into the shadow ledger. Defaults to today 00:00 UTC "
                        "if no last_run.json exists; otherwise reuses prior.")
    p.add_argument("--out-dir", type=str, default=str(OUT_DEFAULT),
                   help=f"Output directory (default: {OUT_DEFAULT})")
    p.add_argument("--phase1-dir", type=str, default=str(PHASE1_DEFAULT),
                   help=f"Trades-net dir (default: {PHASE1_DEFAULT})")
    p.add_argument("--dry-run", action="store_true",
                   help="Do not write any files; print summary only.")
    p.add_argument("--reset", action="store_true",
                   help="Delete the cumulative ledger and start over.")
    return p.parse_args()


def resolve_shadow_start(out_dir: Path, cli_value: Optional[str]) -> pd.Timestamp:
    if cli_value:
        return _parse_iso(cli_value)
    meta_path = out_dir / "last_run.json"
    if meta_path.exists():
        try:
            payload = json.loads(meta_path.read_text())
            if "shadow_start" in payload:
                return _parse_iso(payload["shadow_start"])
        except Exception:
            pass
    return _today_utc_midnight()


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir)
    phase1_dir = Path(args.phase1_dir)

    _ensure_out_dir(out_dir)

    if args.reset:
        for f in ["c1_retuned_shadow.csv", "c1_retuned_equity.csv",
                  "last_run.json", "HALT.flag"]:
            p = out_dir / f
            if p.exists():
                p.unlink()
        print(f"[reset] cleared {out_dir}")

    missing = _check_inputs(phase1_dir)
    if missing:
        print("ERROR: missing cell ledgers:", file=sys.stderr)
        for m in missing:
            print(f"  - {m}", file=sys.stderr)
        return 1

    shadow_start = resolve_shadow_start(out_dir, args.shadow_start)
    run_ts = pd.Timestamp.utcnow().tz_localize(None).tz_localize("UTC")
    ledger_path = out_dir / "c1_retuned_shadow.csv"
    equity_path = out_dir / "c1_retuned_equity.csv"
    summary_path = out_dir / f"c1_retuned_summary_{run_ts.strftime('%Y%m%dT%H%M%SZ')}.md"
    meta_path = out_dir / "last_run.json"

    print("=" * 72)
    print("C1_retuned LIVE SHADOW — runner")
    print("=" * 72)
    print(f"  shadow_start : {shadow_start.isoformat()}")
    print(f"  run_ts       : {run_ts.isoformat()}")
    print(f"  out_dir      : {out_dir}")
    print(f"  phase1_dir   : {phase1_dir}")
    print(f"  cells        : {VARIANT_CFG['cells']}")
    print()

    print("Running canonical C1_retuned simulation...")
    sim = run_simulation(phase1_dir)
    print(f"  total simulated trades (full backtest window): {len(sim)}")

    in_window = filter_to_shadow_window(sim, shadow_start)
    print(f"  trades within shadow window (>= {shadow_start.date()}): {len(in_window)}")

    existing = load_existing_ledger(ledger_path)
    print(f"  trades already on disk: {len(existing)}")

    new_trades = diff_new_trades(in_window, existing)
    new_trades = new_trades.copy()
    if not new_trades.empty:
        new_trades["shadow_run_ts"] = run_ts
    print(f"  new trades this run: {len(new_trades)}")

    if not existing.empty and "shadow_run_ts" not in existing.columns:
        existing["shadow_run_ts"] = pd.NaT

    combined = pd.concat([existing, new_trades], ignore_index=True) \
        if (not existing.empty or not new_trades.empty) else pd.DataFrame(columns=LEDGER_COLS)
    if not combined.empty:
        combined = combined.sort_values("exit_time").reset_index(drop=True)

    halt = compute_halt_status(combined, shadow_start)
    eq = build_equity_curve(combined)

    print()
    print("HALT-CRITERIA STATUS")
    print(f"  status        : {'OK' if halt['ok'] else 'BREACH'}")
    print(f"  current eq    : ${halt['current_equity']:,.2f} "
          f"({(halt['current_equity']/START_EQUITY - 1)*100:+.2f}%)")
    print(f"  max DD        : {halt['max_dd_pct']*100:+.2f}% "
          f"(threshold {HALT_DD_PCT*100:.2f}%)")
    print(f"  cluster days  : {halt['cluster_days']} "
          f"(expected ~{halt['expected_clusters']:.2f}, "
          f"ratio {halt['cluster_ratio']:.2f}x, threshold {HALT_CLUSTER_MULT:.1f}x)")
    print(f"  reason        : {halt['reason']}")
    print()

    if args.dry_run:
        print("[dry-run] not writing any files")
        return 0 if halt["ok"] else 3

    write_ledger(ledger_path, combined)
    write_equity(equity_path, eq)
    write_summary_md(summary_path, combined, new_trades, halt, shadow_start, run_ts)
    meta = {
        "variant": VARIANT_NAME,
        "shadow_start": shadow_start.isoformat(),
        "last_run_ts": run_ts.isoformat(),
        "trades_total": int(len(combined)),
        "new_trades_this_run": int(len(new_trades)),
        "current_equity": float(halt["current_equity"]),
        "max_dd_pct": float(halt["max_dd_pct"]),
        "halt_status": "OK" if halt["ok"] else "BREACH",
        "halt_reason": halt["reason"],
        "config": {
            "cells": list(VARIANT_CFG["cells"]),
            "risk_pct": VARIANT_CFG["risk_pct"],
            "max_concurrent": VARIANT_CFG["max_concurrent"],
            "start_equity": START_EQUITY,
            "margin_call": MARGIN_CALL,
            "halt_dd_pct": HALT_DD_PCT,
            "halt_cluster_min_cells": HALT_CLUSTER_MIN_CELLS,
            "halt_cluster_mult": HALT_CLUSTER_MULT,
        },
    }
    write_metadata(meta_path, meta)

    if not halt["ok"]:
        write_halt_flag(out_dir, halt, run_ts)
        print(f"!! HALT.flag written at {out_dir/'HALT.flag'}", file=sys.stderr)
        return 3

    # If we used to be tripped but now we're back within bounds, clear flag
    flag_path = out_dir / "HALT.flag"
    if flag_path.exists():
        flag_path.unlink()
        print(f"[recover] cleared previous HALT.flag")

    print(f"[ok] wrote {summary_path.name}")
    print(f"[ok] ledger has {len(combined)} trades, equity ${halt['current_equity']:,.2f}")
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
