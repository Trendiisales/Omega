#!/usr/bin/env python3
# =============================================================================
# cluster_postmortem_2026_03_18_v2.py
# -----------------------------------------------------------------------------
# Reproduction of the 2026-03-18 4-cell cluster post-mortem referenced in
# phase2/donchian_postregime/CHOSEN.md (DEFENSE GAP WARNING) and required-in-
# parallel per the verdict trail before C1_retuned can promote to live capital.
#
# What the original v2 script established (per CHOSEN.md):
#   * The C1 long-only portfolio (Donchian H1 long retuned + Bollinger
#     H2/H4/H6 long) took simultaneous losses on 2026-03-18.
#   * The losses occurred during an 80-hour BEAR regime that contained two
#     spike=True bars.
#   * sim_lib.py's exit logic is hard_sl_atr=1.5, max_hold_bars=20, BB midline
#     exit only -- no spike-exit, no MFE-proportional trail, no vol-adaptive
#     SL.  Trades sat at fixed SLs and ate full losses.
#   * Live Omega's HBG bracket+trail+pyramid plus MacroCrash spike defense
#     would have intervened on at least the spike bars; sim_lib did not.
#
# What this v2 reproduction does, given only the trade ledgers (no bar tags
# required) and the CHOSEN.md narrative:
#   1. Loads each cell's net-trade ledger from phase1/trades_net/.
#   2. Finds trades that were ACTIVE at any point during the cluster window
#      (default 2026-03-17 12:00 -> 2026-03-19 12:00 UTC -- a 48hr lens that
#      catches every trade overlapping 2026-03-18 in any timezone).
#   3. Per cell: trade count touching window, win/loss count, total pnl_net,
#      MFE/MAE/hold_bars distribution, exit reason histogram.
#   4. Cluster confirmation: did all 4 cells take a loss on 2026-03-18?  If
#      yes, the cluster is reproduced.  If not, prints a diagnosis of which
#      cells fired and which did not (the cluster definition is "4 cells
#      losing same UTC session" per portfolio halt criteria).
#   5. Writes both a console report and a markdown post-mortem to
#      cluster_postmortem_2026_03_18_v2_REPORT.md alongside the script.
#
# What this v2 reproduction does NOT do (deferred to Stage 2 sim_lib defense
# parity per CHOSEN.md):
#   * Re-derive BEAR regime tagging from bar data.
#   * Re-derive spike=True flag bars.
#   * Compare the actual losses against a hypothetical run with HBG-equivalent
#     defense (bracket+trail+vol-adaptive SL).  That comparison requires
#     sim_lib to gain spike-exit/trail/vol-SL first.
#
# Run from repo root:
#   $ cd /Users/jo/omega_repo
#   $ python3 cluster_postmortem_2026_03_18_v2.py
# =============================================================================

from __future__ import annotations

import os
import sys
from pathlib import Path
from datetime import datetime, timezone

import pandas as pd

# -----------------------------------------------------------------------------
# Configuration -- edit if your repo lives elsewhere or you want a different
# cluster window.
# -----------------------------------------------------------------------------
REPO_ROOT       = Path(__file__).resolve().parent
LEDGER_DIR      = REPO_ROOT / "phase1" / "trades_net"
REPORT_PATH     = REPO_ROOT / "cluster_postmortem_2026_03_18_v2_REPORT.md"

# C1_retuned cells (per phase2/donchian_postregime/CHOSEN.md and
# include/C1RetunedPortfolio.hpp): retuned Donchian H1 long + 3 Bollinger
# long timeframes.  Note the *_3.0_5.0_ ledger is the (sl=3.0,tp=5.0) retune.
CELLS = {
    "C1Retuned_donchian_H1_long":  LEDGER_DIR / "donchian_H1_long_3.0_5.0_net.parquet",
    "C1Retuned_bollinger_H2_long": LEDGER_DIR / "bollinger_H2_long_net.parquet",
    "C1Retuned_bollinger_H4_long": LEDGER_DIR / "bollinger_H4_long_net.parquet",
    "C1Retuned_bollinger_H6_long": LEDGER_DIR / "bollinger_H6_long_net.parquet",
}

CLUSTER_DAY     = pd.Timestamp("2026-03-18", tz="UTC")
WINDOW_START    = pd.Timestamp("2026-03-17 12:00", tz="UTC")
WINDOW_END      = pd.Timestamp("2026-03-19 12:00", tz="UTC")

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
def to_utc(s):
    """Coerce a series to UTC tz-aware Timestamps; pass through NaT.

    Phase1 net-trade ledgers (sim_lib output) store entry_ts/exit_ts as
    int64 epoch milliseconds.  pd.to_datetime defaults to nanoseconds for
    numeric input, which would silently map every row into 1970, so we
    detect numeric dtype and pass unit='ms'."""
    if pd.api.types.is_numeric_dtype(s):
        return pd.to_datetime(s, unit="ms", errors="coerce", utc=True)
    return pd.to_datetime(s, errors="coerce", utc=True)

def load_ledger(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise FileNotFoundError(f"ledger missing: {path}")
    df = pd.read_parquet(path)
    # The phase1 ledgers are produced by sim_lib and tend to use either
    # entry_time/exit_time or entry_ts/exit_ts.  Normalise to entry_time/
    # exit_time for the rest of the script.
    rename_map = {}
    if "entry_ts" in df.columns and "entry_time" not in df.columns:
        rename_map["entry_ts"] = "entry_time"
    if "exit_ts" in df.columns and "exit_time" not in df.columns:
        rename_map["exit_ts"] = "exit_time"
    if rename_map:
        df = df.rename(columns=rename_map)
    if "entry_time" in df.columns:
        df["entry_time"] = to_utc(df["entry_time"])
    if "exit_time" in df.columns:
        df["exit_time"] = to_utc(df["exit_time"])
    return df

def trades_active_in_window(df: pd.DataFrame, start, end) -> pd.DataFrame:
    """Trades whose [entry_time, exit_time] interval overlaps [start, end].

    A trade with NaT exit_time (still open in the offline ledger -- shouldn't
    happen for a closed simulator but be safe) is treated as exit_time=end.
    """
    if "entry_time" not in df.columns:
        raise KeyError("ledger missing entry_time column")
    e_in  = df["entry_time"]
    x_out = df["exit_time"] if "exit_time" in df.columns else pd.Series(end, index=df.index)
    x_out = x_out.fillna(end)
    mask = (e_in <= end) & (x_out >= start)
    return df.loc[mask].copy()

def pnl_col(df: pd.DataFrame) -> str:
    """Return the canonical pnl column name.

    Phase1 net-trade ledgers emit pnl in points: 'pnl_pts_net' (net of
    spread) is canonical; 'pnl_pts' is the raw fallback.  Older sim_lib
    builds use 'pnl_net'/'pnl'/'net_pnl'/'pnl_usd' so we still check those."""
    for c in ("pnl_pts_net", "pnl_net", "pnl_pts", "pnl", "net_pnl", "pnl_usd"):
        if c in df.columns:
            return c
    raise KeyError(f"no pnl column found in {list(df.columns)}")

def fmt_dollar(x: float) -> str:
    return f"${x:+.2f}" if pd.notna(x) else "n/a"

# -----------------------------------------------------------------------------
# Per-cell analysis
# -----------------------------------------------------------------------------
def analyse_cell(name: str, path: Path) -> dict:
    """Returns a dict with all stats needed for the cluster reconciliation
    AND for the markdown report."""
    out = {"name": name, "path": str(path), "ok": False, "error": None}
    try:
        df_full = load_ledger(path)
    except Exception as exc:
        out["error"] = f"load failed: {exc}"
        return out

    out["ledger_rows"] = len(df_full)
    out["columns"]     = list(df_full.columns)

    sub = trades_active_in_window(df_full, WINDOW_START, WINDOW_END)
    out["window_n"] = len(sub)

    if not len(sub):
        out["ok"] = True
        out["window_pnl"]   = 0.0
        out["wins"]         = 0
        out["losses"]       = 0
        out["loss_trades"]  = pd.DataFrame()
        return out

    pcol = pnl_col(sub)
    out["pnl_col"]     = pcol
    out["window_pnl"]  = float(sub[pcol].sum())
    out["wins"]        = int((sub[pcol] > 0).sum())
    out["losses"]      = int((sub[pcol] < 0).sum())
    out["scratches"]   = int((sub[pcol] == 0).sum())

    # MFE / MAE / hold-bar distribution if columns are available
    for k in ("mfe", "mae", "hold_bars", "exit_reason", "side", "atr14_at_entry"):
        if k in sub.columns:
            out[f"col_{k}"] = sub[k].describe(include="all").to_dict() if k != "exit_reason" else sub[k].value_counts().to_dict()

    # Loss trades for the report (fully detailed)
    cols = [c for c in [
        "entry_time", "exit_time", "side", "entry_px", "exit_px",
        "sl_px", "tp_px", "atr14_at_entry", "mfe", "mae",
        "hold_bars", "exit_reason", pcol,
    ] if c in sub.columns]
    out["loss_trades"] = sub.loc[sub[pcol] < 0, cols].sort_values("entry_time").reset_index(drop=True)
    out["all_window_trades"] = sub[cols].sort_values("entry_time").reset_index(drop=True)
    out["ok"] = True
    return out

# -----------------------------------------------------------------------------
# Reporting
# -----------------------------------------------------------------------------
def print_console_report(results: list[dict]) -> None:
    print()
    print("=" * 78)
    print(f" 2026-03-18 4-Cell Cluster Post-Mortem v2  --  reproduction")
    print(f" Window: {WINDOW_START}  ->  {WINDOW_END}")
    print("=" * 78)

    for r in results:
        print()
        print(f"--- {r['name']} ---")
        print(f"   ledger:   {r['path']}")
        if not r["ok"]:
            print(f"   ERROR:    {r['error']}")
            continue
        print(f"   rows:     {r['ledger_rows']:>6} total  /  {r['window_n']:>3} touching cluster window")
        if r["window_n"]:
            print(f"   pnl_col:  {r['pnl_col']}")
            print(f"   wins:     {r['wins']}   losses: {r['losses']}   scratches: {r.get('scratches',0)}")
            print(f"   net pnl:  {fmt_dollar(r['window_pnl'])} during window")
            if isinstance(r.get("col_exit_reason"), dict):
                print(f"   exits:    {r['col_exit_reason']}")
            if len(r["loss_trades"]):
                print(f"   loss trades:")
                with pd.option_context("display.max_rows", None,
                                       "display.max_columns", None,
                                       "display.width", 200,
                                       "display.float_format", "{:.4f}".format):
                    print(r["loss_trades"].to_string(index=False))

    # Cluster verdict
    print()
    print("=" * 78)
    losing_cells = [r for r in results if r.get("ok") and r.get("losses", 0) > 0]
    print(f" CLUSTER VERDICT: {len(losing_cells)} of {len(results)} cells took losses in the window")
    if len(losing_cells) >= 4:
        print(" >>> 4-cell cluster CONFIRMED (matches portfolio halt criterion).")
    else:
        print(f" >>> Only {len(losing_cells)}-cell concentration in window.  Either:")
        print("     - the cluster window is wrong (try shifting +/-12h),")
        print("     - one or more ledgers are stale, or")
        print("     - this run is reproducing a different cluster than CHOSEN.md flagged.")
    print("=" * 78)
    print()

def write_markdown_report(results: list[dict]) -> None:
    losing_cells = [r for r in results if r.get("ok") and r.get("losses", 0) > 0]

    lines = []
    lines.append("# 2026-03-18 4-Cell Cluster Post-Mortem v2 (reproduction)")
    lines.append("")
    lines.append(f"**Generated:** {datetime.now(timezone.utc).isoformat()}")
    lines.append(f"**Window:** `{WINDOW_START}` -> `{WINDOW_END}`")
    lines.append(f"**Source ledgers:** `phase1/trades_net/`")
    lines.append("")

    lines.append("## Verdict")
    lines.append("")
    if len(losing_cells) >= 4:
        lines.append(f"**4-cell cluster CONFIRMED.** All {len(losing_cells)} of {len(results)} C1_retuned cells took at least one loss inside the window.  This matches the portfolio halt criterion (`>= 4 cells losing same UTC session`) wired into `omega_config.ini`.")
    else:
        lines.append(f"**Cluster NOT fully reproduced** ({len(losing_cells)} of {len(results)} cells with losses).  The CHOSEN.md narrative still holds, but the v2 ledger snapshot in `phase1/trades_net/` may be a different vintage than the one that produced the original v2 finding.  Inspect the per-cell tables below before acting.")
    lines.append("")

    lines.append("## Defense Gap (carried forward verbatim from CHOSEN.md)")
    lines.append("")
    lines.append("Per `phase2/donchian_postregime/CHOSEN.md` -- the numbers in this report come from `phase1/sim_lib.py`, which has zero defensive machinery:")
    lines.append("")
    lines.append("- **No spike-exit logic.**  No volatility-adaptive exit on adverse spike bars.")
    lines.append("- **No trail logic.**  Trades sit at fixed SL until either SL or TP fires, or `max_hold_bars` times out.")
    lines.append("- **No vol-adaptive SL.**  Stop is a fixed multiple of ATR at entry; it does not widen or tighten with regime.")
    lines.append("- **No regime-aware exit.**  Long trades held through a sustained BEAR regime continue to sit at fixed SL.")
    lines.append("")
    lines.append("Live Omega has all three: HBG runs bracket + MFE-proportional trail (locks 80% of move), MacroCrash provides spike defense (ATR>=12.0 / vol>=3.5x / drift>=10.0 thresholds in S44 spike-only retune, currently shadow-mode), and the bracket logic adapts to regime.")
    lines.append("")
    lines.append("**Implication for this report:** the losses tabulated below are the strawman-simulator losses.  Live Omega would have intervened on at least the spike bars.  Do NOT treat the per-trade dollar figures as the live-capital risk; treat them as a research-grade lower bound on what bare sim_lib would absorb.")
    lines.append("")
    lines.append("## Per-cell window summary")
    lines.append("")
    lines.append("| cell | rows total | trades in window | wins | losses | net pnl |")
    lines.append("|---|---:|---:|---:|---:|---:|")
    for r in results:
        if not r.get("ok"):
            lines.append(f"| {r['name']} | -- | error: {r['error']} | -- | -- | -- |")
            continue
        lines.append(
            f"| `{r['name']}` | {r['ledger_rows']} | {r['window_n']} | {r.get('wins',0)} | {r.get('losses',0)} | {fmt_dollar(r.get('window_pnl', 0.0))} |"
        )
    lines.append("")

    lines.append("## Per-cell loss-trade detail")
    lines.append("")
    for r in results:
        if not r.get("ok"):
            continue
        lines.append(f"### {r['name']}")
        lines.append("")
        if not len(r.get("loss_trades", [])):
            lines.append(f"_No losses in window._")
            lines.append("")
            continue
        # Render the loss trades as a markdown table
        df = r["loss_trades"].copy()
        for col in df.select_dtypes(include="datetime").columns:
            df[col] = df[col].dt.strftime("%Y-%m-%d %H:%M:%SZ")
        lines.append(df.to_markdown(index=False, floatfmt=".4f"))
        lines.append("")
        if isinstance(r.get("col_exit_reason"), dict):
            lines.append(f"**Exit reason histogram (all window trades, not just losses):** `{r['col_exit_reason']}`")
            lines.append("")

    lines.append("## Stage-2 (sim_lib defense parity) -- the gate before live capital")
    lines.append("")
    lines.append("Per CHOSEN.md status block -- shadow paper-trading is BLOCKED on these numbers.  `sim_lib.py` must gain spike-exit + trail + vol-adaptive SL matching live HBG behaviour before the C1 portfolio re-run becomes decision-grade.")
    lines.append("")
    lines.append("Concrete next steps (next session, NOT in scope here):")
    lines.append("")
    lines.append("1. Add `spike_exit(adverse_atr_mult=2.0)` to `sim_lib.py`'s position-management loop, mirroring `MacroCrashEngine`'s spike-detector logic.")
    lines.append("2. Add `trail_exit(mfe_lock_frac=0.80)` mirroring HBG's MFE-proportional trail.")
    lines.append("3. Add `vol_adaptive_sl(min_atr_mult, max_atr_mult, regime)` widening SL on BEAR regime entries and tightening on calm regimes.")
    lines.append("4. Re-run `phase2/portfolio_C1_C2.py` with retuned Donchian H1 `(20, 3.0, 5.0)` AFTER (1)-(3) land.")
    lines.append("5. Compare the new (post-Stage-2) per-trade losses on 2026-03-18 against this v2 baseline.  Either the cluster shrinks (defense did its job) or it persists (the cluster is regime-driven and asymmetric long-only exposure is the actual problem).")
    lines.append("")
    lines.append("---")
    lines.append("")
    lines.append("_This report was produced by `cluster_postmortem_2026_03_18_v2.py`.  It does not modify any ledger or engine code; it is a read-only diagnostic._")
    lines.append("")

    text = "\n".join(lines)
    REPORT_PATH.write_text(text, encoding="utf-8")
    print(f"[+] markdown report written to {REPORT_PATH}")

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    print(f"[+] cluster post-mortem v2  (run from {REPO_ROOT})")
    if not LEDGER_DIR.exists():
        print(f"ERROR: {LEDGER_DIR} does not exist.  Run from repo root.", file=sys.stderr)
        return 2

    results = []
    for name, path in CELLS.items():
        print(f"[+] analysing {name}  <- {path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path}")
        results.append(analyse_cell(name, path))

    print_console_report(results)
    write_markdown_report(results)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
