#!/usr/bin/env python3
"""
Phase 2 Step 2.3 - Walk-forward validation of Variant C (strictest 4-cell).

Test plan:
  TRAIN    : 2024-03-01 -> 2025-09-30  (in-sample)
  VALIDATE : 2025-10-01 -> 2026-01-27  (pre-regime tail, out-of-sample)
  TEST     : 2026-01-28 -> 2026-04-24  (post-regime, deepest out-of-sample)

For each window we re-run the same Variant C portfolio simulator
(0.5% risk, max 5 concurrent, $10k start, $1k margin call).
Each window starts FRESH at $10k - we are testing whether the portfolio
behaviour is stable, not whether equity persists across windows.

Output: per-window stats + degradation flags.

Inputs:
  /Users/jo/omega_repo/phase1/trades_net/<combo>_net.parquet  (4 cells)

Outputs:
  /Users/jo/omega_repo/phase2/optionD/walkforward_C_report.txt
  /Users/jo/omega_repo/phase2/optionD/walkforward_C_<window>_trades.parquet
  /Users/jo/omega_repo/phase2/optionD/walkforward_C_<window>_equity.parquet
"""

from __future__ import annotations
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, List, Tuple
import numpy as np
import pandas as pd

PHASE1_DIR = Path("/Users/jo/omega_repo/phase1/trades_net")
PHASE2_DIR = Path("/Users/jo/omega_repo/phase2/optionD")
REPORT_PATH = PHASE2_DIR / "walkforward_C_report.txt"

START_EQUITY = 10_000.0
MARGIN_CALL  = 1_000.0
MIN_LOT      = 0.01
LOT_STEP     = 0.01

PRICE_PER_POINT = 0.01
USD_PER_POINT_PER_LOT = 1.0

CELLS_C: Dict[str, Dict] = {
    "donchian_H1_long":  {"file": "donchian_H1_long_net.parquet",  "side": "long"},
    "bollinger_H2_long": {"file": "bollinger_H2_long_net.parquet", "side": "long"},
    "bollinger_H4_long": {"file": "bollinger_H4_long_net.parquet", "side": "long"},
    "bollinger_H6_long": {"file": "bollinger_H6_long_net.parquet", "side": "long"},
}

RISK_PCT = 0.005
MAX_CONCURRENT = 5

WINDOWS: Dict[str, Tuple[str, str]] = {
    "TRAIN":    ("2024-03-01", "2025-09-30"),
    "VALIDATE": ("2025-10-01", "2026-01-27"),
    "TEST":     ("2026-01-28", "2026-04-24"),
}

CANONICAL_COLS = {
    "entry_time":  ["entry_ts", "entry_time"],
    "exit_time":   ["exit_ts",  "exit_time"],
    "entry_price": ["entry_px", "entry_price"],
    "exit_price":  ["exit_px",  "exit_price"],
    "sl_price":    ["sl",       "sl_price"],
    "direction":   ["direction","side"],
    "pnl_pts_net": ["pnl_pts_net"],
}


# ---------------------------------------------------------------------------
# Load (matches portfolio_optionD.py conventions)
# ---------------------------------------------------------------------------

def _resolve(df: pd.DataFrame, canon: str, cands: List[str]) -> str:
    for c in cands:
        if c in df.columns:
            return c
    raise KeyError(f"col '{canon}' not found in {list(df.columns)}")


def _to_utc(s: pd.Series) -> pd.Series:
    if pd.api.types.is_integer_dtype(s) or pd.api.types.is_float_dtype(s):
        return pd.to_datetime(s, unit="ms", utc=True)
    return pd.to_datetime(s, utc=True)


def load_cell(combo_id: str) -> pd.DataFrame:
    spec = CELLS_C[combo_id]
    path = PHASE1_DIR / spec["file"]
    df = pd.read_parquet(path)
    rename = {}
    for canon, cands in CANONICAL_COLS.items():
        actual = _resolve(df, canon, cands)
        if actual != canon:
            rename[actual] = canon
    if rename:
        df = df.rename(columns=rename)
    df["entry_time"] = _to_utc(df["entry_time"])
    df["exit_time"]  = _to_utc(df["exit_time"])
    for c in ("entry_price", "exit_price", "sl_price", "pnl_pts_net"):
        df[c] = pd.to_numeric(df[c], errors="raise")
    df["direction"] = df["direction"].astype(str).str.lower()
    df["combo_id"] = combo_id
    df["sl_distance_px"]  = (df["entry_price"] - df["sl_price"]).abs()
    df["sl_distance_pts"] = df["sl_distance_px"] / PRICE_PER_POINT
    return df.sort_values("entry_time").reset_index(drop=True)


def load_all() -> pd.DataFrame:
    frames = [load_cell(c) for c in CELLS_C]
    return pd.concat(frames, ignore_index=True).sort_values(
        ["entry_time", "combo_id"]).reset_index(drop=True)


# ---------------------------------------------------------------------------
# Sizing & PnL (verified-correct formulas)
# ---------------------------------------------------------------------------

def required_lot(equity: float, risk_pct: float, sl_pts: float) -> float:
    risk_target = equity * risk_pct
    risk_per_lot = sl_pts * USD_PER_POINT_PER_LOT
    if risk_per_lot <= 0:
        return MIN_LOT
    lot = risk_target / risk_per_lot
    return max(MIN_LOT, np.floor(lot / LOT_STEP) * LOT_STEP)


def usd_pnl(pnl_price_dollars: float, lot: float) -> float:
    """pnl_pts_net is in price dollars. $ P/L = price$ * lot * 100"""
    return pnl_price_dollars * lot * 100.0


# ---------------------------------------------------------------------------
# Simulator (filter trades to window first, then run)
# ---------------------------------------------------------------------------

@dataclass
class OpenPos:
    combo_id: str
    entry_time: pd.Timestamp
    exit_time: pd.Timestamp
    lot: float
    pnl_at_exit: float


@dataclass
class WFResult:
    name: str
    start: pd.Timestamp
    end: pd.Timestamp
    trades: pd.DataFrame
    equity_curve: pd.DataFrame
    blocked: int
    margin_called: bool


def simulate_window(name: str,
                    trades_all: pd.DataFrame,
                    start: pd.Timestamp,
                    end: pd.Timestamp) -> WFResult:
    # Filter signals whose entry falls in [start, end]. Exits may extend past end;
    # that's fine, we let them close naturally and include the realised PnL.
    sigs = trades_all[
        (trades_all["entry_time"] >= start) &
        (trades_all["entry_time"] <= end)
    ].sort_values("entry_time").reset_index(drop=True)

    equity = START_EQUITY
    open_pos: List[OpenPos] = []
    closed: List[Dict] = []
    eq_rows: List[Dict] = []
    margin_called = False
    blocked = 0

    for _, sig in sigs.iterrows():
        et = sig["entry_time"]

        still = []
        for p in open_pos:
            if p.exit_time <= et:
                equity += p.pnl_at_exit
                closed.append({
                    "combo_id": p.combo_id,
                    "entry_time": p.entry_time, "exit_time": p.exit_time,
                    "lot": p.lot, "pnl": p.pnl_at_exit, "equity_after": equity,
                })
                eq_rows.append({"time": p.exit_time, "equity": equity})
                if equity < MARGIN_CALL:
                    margin_called = True
            else:
                still.append(p)
        open_pos = still

        if margin_called or equity < MARGIN_CALL:
            margin_called = True
            blocked += 1
            continue
        if len(open_pos) >= MAX_CONCURRENT:
            blocked += 1
            continue

        lot = required_lot(equity, RISK_PCT, sig["sl_distance_pts"])
        pnl = usd_pnl(sig["pnl_pts_net"], lot)
        open_pos.append(OpenPos(sig["combo_id"], et, sig["exit_time"], lot, pnl))

    for p in sorted(open_pos, key=lambda x: x.exit_time):
        equity += p.pnl_at_exit
        closed.append({
            "combo_id": p.combo_id,
            "entry_time": p.entry_time, "exit_time": p.exit_time,
            "lot": p.lot, "pnl": p.pnl_at_exit, "equity_after": equity,
        })
        eq_rows.append({"time": p.exit_time, "equity": equity})
        if equity < MARGIN_CALL:
            margin_called = True

    trades_df = pd.DataFrame(closed).sort_values("exit_time").reset_index(drop=True)
    eq_df = pd.DataFrame(eq_rows).sort_values("time").reset_index(drop=True)
    return WFResult(name, start, end, trades_df, eq_df, blocked, margin_called)


# ---------------------------------------------------------------------------
# Stats per window
# ---------------------------------------------------------------------------

def fmt_money(x):
    return f"${x:,.2f}" if pd.notna(x) and np.isfinite(x) else "n/a"

def fmt_pct(x):
    return f"{x*100:+.2f}%" if pd.notna(x) and np.isfinite(x) else "n/a"


def stats(r: WFResult) -> Dict:
    t = r.trades
    if t.empty:
        return {"name": r.name, "n": 0, "blocked": r.blocked,
                "final": START_EQUITY, "ret": 0.0,
                "pf": float("nan"), "sharpe": 0.0, "max_dd": 0.0,
                "win_rate": float("nan"),
                "avg_win": 0.0, "avg_loss": 0.0,
                "expectancy": 0.0, "per_cell": pd.DataFrame(),
                "margin_called": r.margin_called}
    eq = r.equity_curve.copy()
    eq["peak"] = eq["equity"].cummax()
    eq["dd"]   = (eq["equity"] - eq["peak"]) / eq["peak"]
    max_dd = eq["dd"].min()
    final = eq["equity"].iloc[-1]
    ret = final / START_EQUITY - 1.0

    wins = t[t["pnl"] > 0]["pnl"]
    losses = t[t["pnl"] < 0]["pnl"]
    pf = (wins.sum() / -losses.sum()) if len(losses) and losses.sum() != 0 else float("inf")
    win_rate = len(wins) / len(t)
    avg_win = wins.mean() if len(wins) else 0.0
    avg_loss = losses.mean() if len(losses) else 0.0
    expectancy = t["pnl"].mean()

    daily = t.copy()
    daily["day"] = daily["exit_time"].dt.tz_convert("UTC").dt.date
    by_day = daily.groupby("day")["pnl"].sum()
    sharpe = (by_day.mean() / by_day.std(ddof=1)) * np.sqrt(252) \
             if by_day.std(ddof=1) > 0 and len(by_day) > 1 else 0.0

    per_cell = (t.groupby("combo_id")["pnl"].agg(["count", "sum", "mean"])
                  .rename(columns={"count":"n", "sum":"total", "mean":"avg"}))

    return {"name": r.name, "n": len(t), "blocked": r.blocked,
            "final": final, "ret": ret, "pf": pf, "sharpe": sharpe,
            "max_dd": max_dd, "win_rate": win_rate,
            "avg_win": avg_win, "avg_loss": avg_loss,
            "expectancy": expectancy, "per_cell": per_cell,
            "margin_called": r.margin_called}


def degradation_flags(stats_train: Dict, stats_test: Dict, label: str) -> List[str]:
    flags = []
    if stats_test["n"] == 0:
        flags.append(f"  [{label}] CRITICAL: zero trades in test window")
        return flags
    pf_drop = stats_train["pf"] - stats_test["pf"]
    if pf_drop > 0.30:
        flags.append(f"  [{label}] PF degradation: train {stats_train['pf']:.2f} "
                     f"-> {label} {stats_test['pf']:.2f} (drop {pf_drop:.2f})")
    sharpe_drop = stats_train["sharpe"] - stats_test["sharpe"]
    if sharpe_drop > 0.50:
        flags.append(f"  [{label}] Sharpe degradation: train {stats_train['sharpe']:.2f} "
                     f"-> {label} {stats_test['sharpe']:.2f}")
    if stats_test["pf"] < 1.0:
        flags.append(f"  [{label}] PF < 1.0: strategy unprofitable in {label}")
    if stats_test["max_dd"] < -0.20:
        flags.append(f"  [{label}] DD > 20%: {fmt_pct(stats_test['max_dd'])}")
    if stats_test["margin_called"]:
        flags.append(f"  [{label}] MARGIN CALLED in {label}")
    return flags


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> None:
    PHASE2_DIR.mkdir(parents=True, exist_ok=True)
    out: List[str] = []

    out.append("=" * 78)
    out.append("PHASE 2 STEP 2.3 - WALK-FORWARD VALIDATION (Variant C)")
    out.append("=" * 78)
    out.append(f"Cells          : {list(CELLS_C.keys())}")
    out.append(f"Risk %         : {RISK_PCT}")
    out.append(f"Max concurrent : {MAX_CONCURRENT}")
    out.append(f"Each window starts fresh at {fmt_money(START_EQUITY)}")
    out.append("")

    trades_all = load_all()
    out.append(f"Loaded {len(trades_all)} signals across {trades_all['combo_id'].nunique()} cells")
    out.append(f"Full span: {trades_all['entry_time'].min()} -> {trades_all['exit_time'].max()}")
    out.append("")

    results: Dict[str, WFResult] = {}
    summaries: Dict[str, Dict] = {}

    for w_name, (s, e) in WINDOWS.items():
        s_ts = pd.Timestamp(s, tz="UTC")
        e_ts = pd.Timestamp(e + " 23:59:59", tz="UTC")
        out.append("-" * 78)
        out.append(f"Window {w_name}: {s_ts.date()} -> {e_ts.date()}")
        out.append("-" * 78)
        r = simulate_window(w_name, trades_all, s_ts, e_ts)
        results[w_name] = r
        s_ = stats(r)
        summaries[w_name] = s_

        if not r.trades.empty:
            r.trades.to_parquet(PHASE2_DIR / f"walkforward_C_{w_name}_trades.parquet")
        if not r.equity_curve.empty:
            r.equity_curve.to_parquet(PHASE2_DIR / f"walkforward_C_{w_name}_equity.parquet")

        if s_["n"] == 0:
            out.append(f"  No trades in window.")
            out.append("")
            continue
        out.append(f"  trades       : {s_['n']}  (blocked: {s_['blocked']})")
        out.append(f"  final equity : {fmt_money(s_['final'])}  return: {fmt_pct(s_['ret'])}")
        pf_str = f"{s_['pf']:.3f}" if np.isfinite(s_['pf']) else "inf"
        out.append(f"  profit factor: {pf_str}")
        out.append(f"  sharpe (252) : {s_['sharpe']:.3f}")
        out.append(f"  max DD       : {fmt_pct(s_['max_dd'])}")
        out.append(f"  win rate     : {s_['win_rate']*100:.1f}%")
        out.append(f"  avg win/loss : {fmt_money(s_['avg_win'])} / {fmt_money(s_['avg_loss'])}")
        out.append(f"  expectancy   : {fmt_money(s_['expectancy'])}")
        if s_['margin_called']:
            out.append(f"  *** MARGIN CALLED ***")
        out.append("  per-cell:")
        for cell, row in s_["per_cell"].iterrows():
            out.append(f"    {cell:25s}  n={int(row['n']):>4}  "
                       f"total={fmt_money(row['total'])}  avg={fmt_money(row['avg'])}")
        out.append("")

    # Side-by-side
    out.append("=" * 78)
    out.append("SIDE-BY-SIDE WALK-FORWARD")
    out.append("=" * 78)
    hdr = f"{'window':10s} {'n':>5} {'final$':>12} {'ret':>9} {'PF':>6} " \
          f"{'Sharpe':>8} {'maxDD':>8} {'WinRate':>8}"
    out.append(hdr)
    out.append("-" * len(hdr))
    for w, s_ in summaries.items():
        if s_["n"] == 0:
            out.append(f"{w:10s} {'0':>5} {'-':>12} {'-':>9} {'-':>6} "
                       f"{'-':>8} {'-':>8} {'-':>8}")
            continue
        pf_str = f"{s_['pf']:6.2f}" if np.isfinite(s_['pf']) else "   inf"
        out.append(f"{w:10s} {s_['n']:>5} {fmt_money(s_['final']):>12} "
                   f"{fmt_pct(s_['ret']):>9} {pf_str} "
                   f"{s_['sharpe']:>8.3f} {fmt_pct(s_['max_dd']):>8} "
                   f"{s_['win_rate']*100:>7.1f}%")
    out.append("")

    # Degradation analysis
    out.append("=" * 78)
    out.append("DEGRADATION FLAGS")
    out.append("=" * 78)
    flags = []
    if summaries["TRAIN"]["n"] > 0:
        flags += degradation_flags(summaries["TRAIN"], summaries["VALIDATE"], "VALIDATE")
        flags += degradation_flags(summaries["TRAIN"], summaries["TEST"], "TEST")
    if not flags:
        out.append("  None. Walk-forward is clean.")
    else:
        out.extend(flags)
    out.append("")

    # Verdict
    out.append("=" * 78)
    out.append("VERDICT")
    out.append("=" * 78)
    train, validate, test = summaries["TRAIN"], summaries["VALIDATE"], summaries["TEST"]
    if test["n"] == 0:
        out.append("  TEST window has zero trades - cannot validate.")
    elif test["pf"] < 1.0:
        out.append("  FAIL: TEST window unprofitable.")
    elif test["margin_called"]:
        out.append("  FAIL: TEST window blew through margin call.")
    elif test["max_dd"] < -0.30:
        out.append("  FAIL: TEST window DD exceeds 30%.")
    elif (train["pf"] - test["pf"]) > 0.30:
        out.append("  CAUTION: significant PF degradation in TEST.")
    else:
        out.append("  PASS: portfolio behaviour holds out-of-sample.")
    out.append("")

    text = "\n".join(out)
    print(text)
    REPORT_PATH.write_text(text)
    print(f"\nReport written to {REPORT_PATH}")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        import traceback
        traceback.print_exc()
        print(f"\nFATAL: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(1)
