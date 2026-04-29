#!/usr/bin/env python3
"""
Phase 2 Step 2.2 follow-up - Variants C1, C1_retuned, C2.

C1_max4: original cells, max_concurrent dropped from 5 -> 4.
         (canonical Donchian H1 long params: sl_atr=1.0, tp_r=2.5)

C1_retuned: SAME as C1_max4 except Donchian H1 long uses the v3-sweep-locked
            retuned params (sl_atr=3.0, tp_r=5.0 in sweep vocab; equivalently
            sl_atr=3.0, tp_r=1.6667 in sim_family_a vocab giving TP=5.0 ATR).
            Ledger built by phase1/build_donchian_H1_long_retuned.py.
            Direct A/B vs C1_max4: same Bollinger cells, same risk, same cap.

C2: Donchian H1 long (canonical) + Bollinger H2 long only.
    Tests whether dropping H4+H6 (cluster-correlated) costs much P/L.

All variants: 0.5% risk, $10k start, $1k margin call, max_concurrent per cfg.

Outputs:
  /Users/jo/omega_repo/phase2/optionD/<variant>_trades.parquet
  /Users/jo/omega_repo/phase2/optionD/<variant>_equity.parquet
  /Users/jo/omega_repo/phase2/optionD/C1_C2_summary.txt
"""

from __future__ import annotations
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, List, Tuple
import numpy as np
import pandas as pd

PHASE1_DIR  = Path("/Users/jo/omega_repo/phase1/trades_net")
PHASE2_DIR  = Path("/Users/jo/omega_repo/phase2/optionD")

START_EQUITY = 10_000.0
MARGIN_CALL  = 1_000.0
MIN_LOT      = 0.01
LOT_STEP     = 0.01
PRICE_PER_POINT = 0.01
USD_PER_POINT_PER_LOT = 1.0

REGIME_SPLIT = pd.Timestamp("2026-01-28 00:00:00", tz="UTC")

ALL_CELLS: Dict[str, Dict] = {
    "donchian_H1_long":         {"file": "donchian_H1_long_net.parquet",         "side": "long"},
    "donchian_H1_long_retuned": {"file": "donchian_H1_long_3.0_5.0_net.parquet", "side": "long"},
    "bollinger_H2_long":        {"file": "bollinger_H2_long_net.parquet",        "side": "long"},
    "bollinger_H4_long":        {"file": "bollinger_H4_long_net.parquet",        "side": "long"},
    "bollinger_H6_long":        {"file": "bollinger_H6_long_net.parquet",        "side": "long"},
}

VARIANTS: Dict[str, Dict] = {
    "C1_max4": {
        "cells": ["donchian_H1_long", "bollinger_H2_long",
                  "bollinger_H4_long", "bollinger_H6_long"],
        "risk_pct": 0.005,
        "max_concurrent": 4,
    },
    "C1_retuned": {
        "cells": ["donchian_H1_long_retuned", "bollinger_H2_long",
                  "bollinger_H4_long", "bollinger_H6_long"],
        "risk_pct": 0.005,
        "max_concurrent": 4,
    },
    "C2_donchian_boll2": {
        "cells": ["donchian_H1_long", "bollinger_H2_long"],
        "risk_pct": 0.005,
        "max_concurrent": 3,
    },
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
# Load
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
    spec = ALL_CELLS[combo_id]
    path = PHASE1_DIR / spec["file"]
    if not path.exists():
        raise FileNotFoundError(f"Missing trade file: {path}")
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
    if (df["sl_distance_pts"] <= 0).any():
        raise ValueError(f"{combo_id}: non-positive SL distance")
    return df.sort_values("entry_time").reset_index(drop=True)


def load_all_cells(combo_ids: List[str]) -> pd.DataFrame:
    frames = [load_cell(c) for c in combo_ids]
    return pd.concat(frames, ignore_index=True).sort_values(
        ["entry_time", "combo_id"]).reset_index(drop=True)


# ---------------------------------------------------------------------------
# Sizing & PnL (verified)
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
# Simulator
# ---------------------------------------------------------------------------

@dataclass
class OpenPos:
    combo_id: str
    entry_time: pd.Timestamp
    exit_time: pd.Timestamp
    lot: float
    pnl_at_exit: float


@dataclass
class Result:
    name: str
    trades: pd.DataFrame
    equity_curve: pd.DataFrame
    blocked: int = 0
    margin_called: bool = False
    margin_call_time: pd.Timestamp | None = None


def simulate(name: str, trades_all: pd.DataFrame,
             risk_pct: float, max_concurrent: int) -> Result:
    equity = START_EQUITY
    open_pos: List[OpenPos] = []
    closed: List[Dict] = []
    eq_rows: List[Dict] = []
    margin_called = False
    margin_call_time = None
    blocked = 0

    for _, sig in trades_all.iterrows():
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
                if equity < MARGIN_CALL and not margin_called:
                    margin_called = True
                    margin_call_time = p.exit_time
            else:
                still.append(p)
        open_pos = still

        if margin_called or equity < MARGIN_CALL:
            margin_called = True
            blocked += 1
            continue
        if len(open_pos) >= max_concurrent:
            blocked += 1
            continue

        lot = required_lot(equity, risk_pct, sig["sl_distance_pts"])
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
        if equity < MARGIN_CALL and not margin_called:
            margin_called = True
            margin_call_time = p.exit_time

    trades_df = pd.DataFrame(closed).sort_values("exit_time").reset_index(drop=True)
    eq_df = pd.DataFrame(eq_rows).sort_values("time").reset_index(drop=True)
    return Result(name, trades_df, eq_df, blocked, margin_called, margin_call_time)


# ---------------------------------------------------------------------------
# Stats
# ---------------------------------------------------------------------------

def fmt_money(x):
    return f"${x:,.2f}" if pd.notna(x) and np.isfinite(x) else "n/a"

def fmt_pct(x):
    return f"{x*100:+.2f}%" if pd.notna(x) and np.isfinite(x) else "n/a"


def summarise(r: Result) -> Dict:
    t = r.trades
    if t.empty:
        return {"name": r.name, "n": 0, "blocked": r.blocked,
                "final": START_EQUITY, "ret": 0.0, "pf": float("nan"),
                "sharpe": 0.0, "max_dd": 0.0, "win_rate": 0.0,
                "pre": {"n": 0, "pf": float("nan"), "pnl": 0.0},
                "post":{"n": 0, "pf": float("nan"), "pnl": 0.0},
                "per_cell": pd.DataFrame(),
                "max_concurrent_seen": 0,
                "margin_called": r.margin_called}
    eq = r.equity_curve.copy()
    eq["peak"] = eq["equity"].cummax()
    eq["dd"] = (eq["equity"] - eq["peak"]) / eq["peak"]
    max_dd = eq["dd"].min()
    final = eq["equity"].iloc[-1]
    ret = final / START_EQUITY - 1.0

    wins = t[t["pnl"] > 0]["pnl"]
    losses = t[t["pnl"] < 0]["pnl"]
    pf = (wins.sum() / -losses.sum()) if len(losses) and losses.sum() != 0 else float("inf")
    win_rate = len(wins) / len(t)

    daily = t.copy()
    daily["day"] = daily["exit_time"].dt.tz_convert("UTC").dt.date
    by_day = daily.groupby("day")["pnl"].sum()
    sharpe = (by_day.mean() / by_day.std(ddof=1)) * np.sqrt(252) \
             if by_day.std(ddof=1) > 0 and len(by_day) > 1 else 0.0

    pre = t[t["entry_time"] < REGIME_SPLIT]
    post = t[t["entry_time"] >= REGIME_SPLIT]
    def _slice(s):
        if s.empty:
            return {"n": 0, "pf": float("nan"), "pnl": 0.0}
        w = s[s["pnl"] > 0]["pnl"].sum()
        l = -s[s["pnl"] < 0]["pnl"].sum()
        return {"n": len(s), "pf": (w/l) if l > 0 else float("inf"), "pnl": s["pnl"].sum()}

    per_cell = (t.groupby("combo_id")["pnl"].agg(["count", "sum"])
                  .rename(columns={"count": "n", "sum": "net_pnl"})
                  .sort_values("net_pnl", ascending=False))

    # max concurrent actually hit
    events = []
    for _, row in t.iterrows():
        events.append((row["entry_time"], +1))
        events.append((row["exit_time"], -1))
    ev = pd.DataFrame(events, columns=["t", "d"]).sort_values(
        ["t", "d"], ascending=[True, False]).reset_index(drop=True)
    ev["cnt"] = ev["d"].cumsum()
    max_conc = int(ev["cnt"].max()) if len(ev) else 0

    return {"name": r.name, "n": len(t), "blocked": r.blocked,
            "final": final, "ret": ret, "pf": pf, "sharpe": sharpe,
            "max_dd": max_dd, "win_rate": win_rate,
            "pre": _slice(pre), "post": _slice(post),
            "per_cell": per_cell,
            "max_concurrent_seen": max_conc,
            "margin_called": r.margin_called}


def loss_clusters(t: pd.DataFrame, min_cells: int = 2) -> pd.DataFrame:
    """Days where >= min_cells different cells lost. min_cells=2 makes sense
    for C2 since it only has 2 cells."""
    daily = t.copy()
    daily["day"] = daily["exit_time"].dt.tz_convert("UTC").dt.date
    daily["is_loss"] = daily["pnl"] < 0
    losing = (daily[daily["is_loss"]].groupby("day")["combo_id"].nunique()
              .rename("losing_cells"))
    daily_pnl = daily.groupby("day")["pnl"].sum()
    days = losing[losing >= min_cells].index
    rows = []
    for d in days:
        sub = daily[daily["day"] == d]
        losers = sub[sub["pnl"] < 0]["combo_id"].unique().tolist()
        rows.append({
            "day": d,
            "losing_cells": int(losing.loc[d]),
            "total_pnl": daily_pnl.loc[d],
            "cells": ",".join(sorted(losers)),
        })
    return pd.DataFrame(rows).sort_values("total_pnl") if rows else pd.DataFrame()


def worst_days_top10(t: pd.DataFrame) -> pd.DataFrame:
    daily = t.copy()
    daily["day"] = daily["exit_time"].dt.tz_convert("UTC").dt.date
    by_day = daily.groupby("day").agg(pnl=("pnl", "sum"), n=("pnl", "size"))
    return by_day.nsmallest(10, "pnl")


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> None:
    PHASE2_DIR.mkdir(parents=True, exist_ok=True)
    out: List[str] = []

    out.append("=" * 78)
    out.append("PHASE 2 STEP 2.2 FOLLOW-UP - VARIANTS C1, C1_retuned, C2")
    out.append("=" * 78)
    out.append(f"Start equity   : {fmt_money(START_EQUITY)}")
    out.append(f"Margin call    : {fmt_money(MARGIN_CALL)}")
    out.append(f"Regime split   : {REGIME_SPLIT}")
    out.append("")
    out.append("Donchian H1 long ledgers in play:")
    out.append(f"  canonical : donchian_H1_long_net.parquet         (sl_atr=1.0, tp_r=2.5 -> TP=2.5 ATR)")
    out.append(f"  retuned   : donchian_H1_long_3.0_5.0_net.parquet (sl_atr=3.0, tp_r=1.6667 -> TP=5.0 ATR)")
    out.append("")

    # Sanity check
    lot_check = required_lot(10_000.0, 0.005, 500.0)
    pnl_check = usd_pnl(-5.0, lot_check)
    out.append(f"[sizing check] equity=$10k risk=0.5% SL=$5 (500pts)")
    out.append(f"               -> lot={lot_check:.4f} (expect 0.10)")
    out.append(f"               -> SL-hit P/L={fmt_money(pnl_check)} (expect -$50.00)")
    out.append("")

    summaries: Dict[str, Dict] = {}
    results: Dict[str, Result] = {}

    for vname, cfg in VARIANTS.items():
        out.append("-" * 78)
        out.append(f"Variant {vname}")
        out.append(f"  cells          : {cfg['cells']}")
        out.append(f"  risk_pct       : {cfg['risk_pct']}")
        out.append(f"  max_concurrent : {cfg['max_concurrent']}")

        trades_all = load_all_cells(cfg["cells"])
        out.append(f"  loaded         : {len(trades_all)} signal rows from "
                   f"{trades_all['combo_id'].nunique()} cells")

        r = simulate(vname, trades_all, cfg["risk_pct"], cfg["max_concurrent"])
        results[vname] = r
        s = summarise(r)
        summaries[vname] = s

        if not r.trades.empty:
            r.trades.to_parquet(PHASE2_DIR / f"{vname}_trades.parquet")
        if not r.equity_curve.empty:
            r.equity_curve.to_parquet(PHASE2_DIR / f"{vname}_equity.parquet")

        pf_str = f"{s['pf']:.3f}" if np.isfinite(s['pf']) else "inf"
        out.append(f"  trades taken   : {s['n']}  (blocked: {s['blocked']})")
        out.append(f"  final equity   : {fmt_money(s['final'])}  "
                   f"return: {fmt_pct(s['ret'])}")
        out.append(f"  profit factor  : {pf_str}")
        out.append(f"  sharpe (252)   : {s['sharpe']:.3f}")
        out.append(f"  max DD         : {fmt_pct(s['max_dd'])}")
        out.append(f"  win rate       : {s['win_rate']*100:.1f}%")
        out.append(f"  max concurrent : {s['max_concurrent_seen']}  "
                   f"(cap {cfg['max_concurrent']})")
        out.append(f"  pre-split  : n={s['pre']['n']:>4}  "
                   f"PF={s['pre']['pf']:.3f}  PnL={fmt_money(s['pre']['pnl'])}")
        out.append(f"  post-split : n={s['post']['n']:>4}  "
                   f"PF={s['post']['pf']:.3f}  PnL={fmt_money(s['post']['pnl'])}")
        out.append("  per-cell:")
        for cell, row in s["per_cell"].iterrows():
            out.append(f"    {cell:30s}  n={int(row['n']):>4}  "
                       f"net={fmt_money(row['net_pnl'])}")

        # Loss clusters - threshold = (n_cells if 2 cells else 3 if 4 cells)
        n_cells = len(cfg["cells"])
        threshold = n_cells if n_cells <= 2 else 3
        clusters = loss_clusters(r.trades, min_cells=threshold)
        if not clusters.empty:
            out.append(f"  loss clusters ({threshold}+ cells losing same day): {len(clusters)}")
            for _, row in clusters.iterrows():
                out.append(f"    {row['day']}  cells={row['losing_cells']}  "
                           f"pnl={fmt_money(row['total_pnl'])}  ({row['cells']})")
            out.append(f"  cluster-day total: {fmt_money(clusters['total_pnl'].sum())}")
        else:
            out.append(f"  loss clusters ({threshold}+ cells): none")

        # Worst 5 days
        wd = worst_days_top10(r.trades).head(5)
        out.append("  worst 5 days:")
        for d, row in wd.iterrows():
            out.append(f"    {d}  n={int(row['n'])}  pnl={fmt_money(row['pnl'])}")
        out.append("")

    # Side-by-side
    out.append("=" * 78)
    out.append("SIDE-BY-SIDE (variants vs original C from earlier)")
    out.append("=" * 78)
    out.append(f"{'variant':24s} {'n':>5} {'final$':>12} {'ret':>9} {'PF':>6} "
               f"{'Sharpe':>8} {'maxDD':>8} {'maxConc':>8}")
    out.append("-" * 90)

    # Reference: original C from prior run
    ref_C_path = PHASE2_DIR / "C_strictest_4_trades.parquet"
    ref_final = None
    ref_t = None
    ref_eq = None
    if ref_C_path.exists():
        ref_t = pd.read_parquet(ref_C_path)
        ref_eq_path = PHASE2_DIR / "C_strictest_4_equity.parquet"
        ref_eq = pd.read_parquet(ref_eq_path)
        ref_eq["peak"] = ref_eq["equity"].cummax()
        ref_eq["dd"] = (ref_eq["equity"] - ref_eq["peak"]) / ref_eq["peak"]
        ref_pf = (ref_t[ref_t["pnl"]>0]["pnl"].sum() /
                  -ref_t[ref_t["pnl"]<0]["pnl"].sum())
        ref_final = ref_eq["equity"].iloc[-1]
        ref_daily = ref_t.copy()
        ref_daily["day"] = ref_daily["exit_time"].dt.tz_convert("UTC").dt.date
        ref_by_day = ref_daily.groupby("day")["pnl"].sum()
        ref_sharpe = (ref_by_day.mean()/ref_by_day.std(ddof=1))*np.sqrt(252) \
                     if ref_by_day.std(ddof=1) > 0 else 0.0
        ref_events = []
        for _, row in ref_t.iterrows():
            ref_events.append((row["entry_time"], +1))
            ref_events.append((row["exit_time"], -1))
        ref_ev = pd.DataFrame(ref_events, columns=["t","d"]).sort_values(
            ["t","d"], ascending=[True, False]).reset_index(drop=True)
        ref_ev["cnt"] = ref_ev["d"].cumsum()
        ref_max_conc = int(ref_ev["cnt"].max())
        out.append(f"{'C_orig (cap5)':24s} {len(ref_t):>5} "
                   f"{fmt_money(ref_final):>12} "
                   f"{fmt_pct(ref_final/START_EQUITY - 1):>9} "
                   f"{ref_pf:>6.2f} {ref_sharpe:>8.3f} "
                   f"{fmt_pct(ref_eq['dd'].min()):>8} {ref_max_conc:>8}")

    for v, s in summaries.items():
        pf_str = f"{s['pf']:6.2f}" if np.isfinite(s['pf']) else "   inf"
        out.append(f"{v:24s} {s['n']:>5} {fmt_money(s['final']):>12} "
                   f"{fmt_pct(s['ret']):>9} {pf_str} "
                   f"{s['sharpe']:>8.3f} {fmt_pct(s['max_dd']):>8} "
                   f"{s['max_concurrent_seen']:>8}")
    out.append("")

    # Verdict
    out.append("=" * 78)
    out.append("VERDICT")
    out.append("=" * 78)

    # C1_max4 vs C_orig
    if ref_C_path.exists():
        c1 = summaries["C1_max4"]
        out.append(f"C1_max4 vs C_orig:")
        out.append(f"  delta trades: {c1['n']} vs {len(ref_t)} "
                   f"({c1['n'] - len(ref_t):+d})")
        out.append(f"  delta final : {fmt_money(c1['final'] - ref_final)}")
        out.append(f"  delta maxDD : {(c1['max_dd'] - ref_eq['dd'].min())*100:+.2f}pp")
        if c1['n'] == len(ref_t) and abs(c1['final'] - ref_final) < 1.0:
            out.append("  -> cap=5 was non-binding; cap=4 is safe.")
        else:
            out.append("  -> cap change had impact; review trade differences.")
        out.append("")

    # C1_retuned vs C1_max4 — the key A/B
    c1_base = summaries["C1_max4"]
    c1_ret  = summaries["C1_retuned"]
    out.append(f"C1_retuned vs C1_max4 (DONCHIAN H1 PARAM RETUNE A/B):")
    out.append(f"  delta trades   : {c1_ret['n']} vs {c1_base['n']} "
               f"({c1_ret['n'] - c1_base['n']:+d})")
    out.append(f"  delta final    : {fmt_money(c1_ret['final'] - c1_base['final'])}")
    out.append(f"  delta return   : {(c1_ret['ret'] - c1_base['ret'])*100:+.2f}pp")
    pf_b = c1_base['pf'] if np.isfinite(c1_base['pf']) else float('nan')
    pf_r = c1_ret['pf']  if np.isfinite(c1_ret['pf'])  else float('nan')
    if np.isfinite(pf_b) and np.isfinite(pf_r):
        out.append(f"  delta PF       : {pf_r - pf_b:+.3f}  ({pf_b:.3f} -> {pf_r:.3f})")
    out.append(f"  delta maxDD    : {(c1_ret['max_dd'] - c1_base['max_dd'])*100:+.2f}pp "
               f"(negative = deeper drawdown)")
    out.append(f"  delta sharpe   : {c1_ret['sharpe'] - c1_base['sharpe']:+.3f}")
    out.append(f"  delta winrate  : {(c1_ret['win_rate'] - c1_base['win_rate'])*100:+.1f}pp")
    out.append(f"  base post-split: n={c1_base['post']['n']}  "
               f"PF={c1_base['post']['pf']:.3f}  PnL={fmt_money(c1_base['post']['pnl'])}")
    out.append(f"  retn post-split: n={c1_ret['post']['n']}  "
               f"PF={c1_ret['post']['pf']:.3f}  PnL={fmt_money(c1_ret['post']['pnl'])}")
    # Decision heuristic
    ret_better = c1_ret['ret'] > c1_base['ret']
    dd_better  = c1_ret['max_dd'] >= c1_base['max_dd']  # less negative = better
    pf_better  = (np.isfinite(pf_r) and np.isfinite(pf_b) and pf_r > pf_b)
    if ret_better and dd_better and pf_better:
        out.append("  -> RETUNE WINS on return, DD, and PF. Ship C1_retuned.")
    elif ret_better and not dd_better:
        out.append("  -> retune raises return but worsens DD. Decide on risk preference.")
    elif (not ret_better) and dd_better:
        out.append("  -> retune lowers return but improves DD. Decide on risk preference.")
    else:
        out.append("  -> retune does NOT clearly dominate. Stick with canonical C1_max4.")
    out.append("")

    # C2 vs C_orig
    c2 = summaries["C2_donchian_boll2"]
    if ref_C_path.exists():
        out.append(f"C2 vs C_orig:")
        out.append(f"  delta trades: {c2['n']} vs {len(ref_t)} "
                   f"({c2['n'] - len(ref_t):+d})")
        out.append(f"  delta final : {fmt_money(c2['final'] - ref_final)}")
        cost_per_pnl = (ref_final - c2['final'])
        out.append(f"  cost of dropping H4+H6: {fmt_money(cost_per_pnl)} of P/L")
        out.append(f"  delta maxDD : {(c2['max_dd'] - ref_eq['dd'].min())*100:+.2f}pp "
                   f"(negative = improvement)")
    out.append("")

    summary_path = PHASE2_DIR / "C1_C2_summary.txt"
    text = "\n".join(out)
    print(text)
    summary_path.write_text(text)
    print(f"\nSummary written to {summary_path}")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        import traceback
        traceback.print_exc()
        print(f"\nFATAL: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(1)
