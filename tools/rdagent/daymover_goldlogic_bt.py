#!/usr/bin/env python3
"""
Day-mover basket with the GOLD COMPANION logic ported faithfully (S-2026-06-30).

Operator: "use whatever engine we use on gold, the logic should be the same."
Gold companion = stall-accountant-clip (stall_accountant.py). Its WINNING ingredient
is the PROFIT-GATE: no clip trigger arms until the trade has captured >= GATE_PCT.
Chop/weak trades never arm -> they ride wide; only real runners get clipped on stall.

The prior sweep (daymover_companion_sweep.py) OMITTED that gate -> it clipped weak
trades at ~0% captured everywhere -> wide-trail trivially won. This harness fixes that
and adds a regime split (operator: "should not trade in bear unless cap met").

Gold params (from stall_accountant.py):
  GATE_PCT = 1.5%   (arm only after capturing >= this)
  N_STALL  = 2      (H4 bars w/ no new favourable extreme = stalled). Here 1 bar = 1 day.
  REV_GB   = 0.05   (clip when price gives back >= 5% of MFE peak)

DATA: sp500_long_close.csv (565 names, daily closes, 2019-06 -> 2026-06).
Regime: market = equal-weight basket; bull = basket close > its 200d SMA (the same
close>SMA200 gate the rest of the Omega book uses for bear exclusion).
"""
from __future__ import annotations
import numpy as np, pandas as pd
from pathlib import Path

CSV = Path.home() / "Omega" / "data" / "rdagent" / "sp500_long_close.csv"
GLITCH = {"POM", "CPWR", "MI"}
COST_RT = 0.002
ATR_WIN = 14

# --- gold companion params (faithful) ---
GATE_PCT = 0.015
N_STALL  = 2
REV_GB   = 0.05

# --- COLD-LOSS CUT (S-2026-07-01) -------------------------------------------
# The STALL and REVERSAL clips are BOTH AND-armed (armed = fav >= GATE_PCT), so a
# trade that goes adverse and never captures GATE_PCT never arms -> neither clip
# can fire -> it rides to MAX_HOLD unprotected (the live SOL companion sat at
# -$6.40 / MFE 0% / "stall 1" exactly this way). COLD_CUT is an INDEPENDENT
# adverse stop measured from ENTRY, ungated by the profit-arm. It only bites
# never-armed trades (fav < GATE_PCT), so runners -- which arm by definition --
# are exempt and still ride wide; it cannot scalp the runners that carry PF.
# 0.0 = OFF (no behaviour change). Sweep with --sweep-coldcut, then ship the
# value only if net-positive (per the CLAUDE.md Adverse-Protection mandate).
COLD_CUT = 0.0            # e.g. 0.015 once the sweep confirms a viable level
COLD_CUT_GRID = [0.0, 0.010, 0.015, 0.020, 0.030]

GATE     = 0.05      # day-mover entry: >= 5% day
CONTIN_K = 20        # + new 20-day high
MAX_HOLD = 60        # ride wide; clip is what exits early when armed

LARGECAP = ("AAPL MSFT NVDA AMZN GOOGL GOOG META TSLA AVGO ORCL AMD MU INTC QCOM TXN ADI "
            "LRCX AMAT KLAC MRVL ARM SMCI PLTR CRWD PANW SNOW NOW CRM ADBE SHOP NFLX UBER "
            "ABNB COIN MSTR DELL ANET CDNS SNPS WDC ON MCHP NXPI HPQ").split()


def load():
    df = pd.read_csv(CSV, index_col=0, parse_dates=True).sort_index()
    cov = df.notna().mean()
    keep = [c for c in df.columns[cov >= 0.90] if c not in GLITCH]
    return df[keep]


def regime_flags(df):
    """bull[date] = equal-weight basket close > its 200d SMA (else bear/off)."""
    norm = df / df.iloc[0]
    basket = norm.mean(axis=1)
    sma = basket.rolling(200, min_periods=200).mean()
    bull = (basket > sma)
    return bull  # bool Series indexed by date; NaN-SMA warmup -> False


def simulate(closes, dates, bull, mode, cold_cut=0.0):
    """
    mode: 'wide'  = ride to max_hold (real-engine proxy, no clip)
          'gold'  = gold companion: profit-GATE then STALL/REVERSAL clip
    cold_cut: ungated adverse stop from ENTRY (0.0 = off). Bites only never-armed
              trades (fav < GATE_PCT), independent of mode -- see COLD_CUT note.
    bear_gate is applied at ENTRY by caller via the `allow` mask in dates.
    Returns list of (entry_date, exit_date, pnl_pct, was_bull_entry).
    """
    n = len(closes)
    ret = np.empty(n); ret[0] = 0.0
    ret[1:] = closes[1:] / closes[:-1] - 1.0
    trades = []
    i = ATR_WIN + 1
    while i < n - 1:
        if not (ret[i] >= GATE and np.isfinite(closes[i])):
            i += 1; continue
        if closes[i] < np.nanmax(closes[max(0, i - CONTIN_K):i + 1]):
            i += 1; continue
        entry_px = closes[i]
        bull_entry = bool(bull.iloc[i]) if i < len(bull) else False
        peak = entry_px; since_high = 0; exit_i = None
        for k in range(i + 1, min(i + 1 + MAX_HOLD, n)):
            c = closes[k]
            if not np.isfinite(c):
                continue
            if c > peak:
                peak = c; since_high = 0
            else:
                since_high += 1
            fav = peak / entry_px - 1.0              # MFE% captured so far
            armed = fav >= GATE_PCT                  # PROFIT-GATE (the missing ingredient)
            # COLD-LOSS CUT: ungated by the arm; only never-armed trades. LONG
            # day-movers -> adverse = c/entry - 1 (invert for shorts). Bounds the
            # cold loser the AND-armed STALL/REVERSAL clips can never reach.
            if cold_cut > 0.0 and not armed and (c / entry_px - 1.0) <= -cold_cut:
                exit_i = k; break
            if mode == "gold":
                if armed and since_high >= N_STALL:                 # STALL_CLIP
                    exit_i = k; break
                if armed and c <= peak * (1.0 - REV_GB):            # REVERSAL_CLIP
                    exit_i = k; break
        if exit_i is None:
            exit_i = min(i + MAX_HOLD, n - 1)
        ex_px = closes[exit_i]
        if not np.isfinite(ex_px):
            j = exit_i
            while j > i and not np.isfinite(closes[j]):
                j -= 1
            ex_px = closes[j]; exit_i = j
        if not np.isfinite(ex_px) or ex_px <= 0:
            i = exit_i + 1; continue
        pnl = ex_px / entry_px - 1.0 - COST_RT
        trades.append((dates[i], dates[exit_i], pnl, bull_entry))
        i = exit_i + 1
    return trades


def metrics(trades, label):
    if not trades:
        return {"label": label, "n": 0, "pf": 0, "wr": 0, "avg": 0,
                "worst": 0, "dd": 0, "h1": 0, "h2": 0, "tot": 0}
    pnl = np.array([t[2] for t in trades])
    wins = pnl[pnl > 0]; losses = pnl[pnl < 0]
    pf = wins.sum() / abs(losses.sum()) if losses.sum() != 0 else float("inf")
    wr = (pnl > 0).mean() * 100
    order = np.argsort(pd.to_datetime([t[1] for t in trades]).values)
    eq = np.cumsum(pnl[order]); pk = np.maximum.accumulate(eq); dd = (eq - pk).min()
    ent = pd.to_datetime([t[0] for t in trades]).values
    mid = np.median(ent.astype("int64"))
    h1 = pnl[ent.astype("int64") <= mid]; h2 = pnl[ent.astype("int64") > mid]
    def _pf(a):
        w = a[a > 0].sum(); l = abs(a[a < 0].sum()); return w / l if l else float("inf")
    return {"label": label, "n": len(trades), "pf": pf, "wr": wr,
            "avg": pnl.mean() * 100, "worst": pnl.min() * 100, "dd": dd * 100,
            "h1": _pf(h1), "h2": _pf(h2), "tot": pnl.sum() * 100}


def run(df, bull, mode, entry_filter, cold_cut=0.0):
    """entry_filter: 'all' | 'bull' (drop bear-entry trades = exclude-bear)."""
    dates = df.index.to_numpy()
    allt = []
    for col in df.columns:
        allt += simulate(df[col].to_numpy(float), dates, bull, mode, cold_cut)
    if entry_filter == "bull":
        allt = [t for t in allt if t[3]]
    return allt


def split(trades):
    bull = [t for t in trades if t[3]]
    bear = [t for t in trades if not t[3]]
    return bull, bear


def hdr(t):
    print(f"\n{t}")
    print(f"{'config':34s} {'n':>5s} {'PF':>6s} {'WR%':>5s} {'avg%':>6s} {'tot%':>8s} "
          f"{'worst%':>7s} {'maxDD%':>7s} {'H1':>5s} {'H2':>5s}")
    print("-" * 100)


def line(r):
    print(f"{r['label']:34s} {r['n']:5d} {r['pf']:6.2f} {r['wr']:5.0f} {r['avg']:6.2f} "
          f"{r['tot']:8.1f} {r['worst']:7.1f} {r['dd']:7.1f} {r['h1']:5.2f} {r['h2']:5.2f}")


def main():
    df = load()
    large = df[[c for c in dict.fromkeys(LARGECAP) if c in df.columns]]
    bull = regime_flags(large).reset_index(drop=True)
    print(f"largecap={large.shape[1]} dates={df.index.min().date()}->{df.index.max().date()} "
          f"rows={len(df)} bull-days={int(bull.sum())}/{len(bull)} "
          f"({100*bull.mean():.0f}% bull)")
    print(f"gold params: GATE_PCT={GATE_PCT:.3f} N_STALL={N_STALL} REV_GB={REV_GB} | "
          f"entry: day>=5% + new {CONTIN_K}d-high | max_hold={MAX_HOLD}")

    wide = run(large, bull, "wide", "all")
    gold = run(large, bull, "gold", "all")
    gold_bullentry = run(large, bull, "gold", "bull")

    hdr("=== WIDE (real-engine proxy) vs GOLD-COMPANION-LOGIC, blended + regime split ===")
    line(metrics(wide, "WIDE  | all regimes"))
    line(metrics(gold, "GOLD  | all regimes"))
    wb, wr_ = split(wide); gb, gr = split(gold)
    line(metrics(wb, "WIDE  | bull-entry only"))
    line(metrics(gb, "GOLD  | bull-entry only"))
    line(metrics(wr_, "WIDE  | bear-entry only"))
    line(metrics(gr, "GOLD  | bear-entry only"))
    line(metrics(gold_bullentry, "GOLD  | EXCLUDE-BEAR (bull-gate entry)"))

    # operator's actual spec: ride WIDE in bull, CLIP (gold) in bear
    wb2, _ = split(wide); _, gr2 = split(gold)
    switch = wb2 + gr2
    line(metrics(wb2, "SWITCH| WIDE bull-entry only"))
    line(metrics(switch, "SWITCH| WIDE-bull + GOLD-clip-bear"))


def sweep_coldcut():
    """Sweep the ungated COLD-LOSS CUT against the un-cut baseline (cold_cut=0).

    Ship a non-zero COLD_CUT into export_signals._mark_and_exit / stall_accountant
    ONLY if it is net-positive here: total% up (or flat) with worst%/maxDD improved.
    The cut only touches never-armed trades, so PF should hold; verify it does."""
    df = load()
    large = df[[c for c in dict.fromkeys(LARGECAP) if c in df.columns]]
    bull = regime_flags(large).reset_index(drop=True)
    print(f"largecap={large.shape[1]} rows={len(df)} "
          f"({100*bull.mean():.0f}% bull) | GATE_PCT={GATE_PCT:.3f} grid={COLD_CUT_GRID}")

    def _lbl(cc):
        return "off" if cc == 0 else f"{cc*100:.1f}%"

    hdr("=== COLD-CUT sweep · GOLD (all regimes) ===")
    for cc in COLD_CUT_GRID:
        line(metrics(run(large, bull, "gold", "all", cc), f"GOLD  cut={_lbl(cc)}"))
    hdr("=== COLD-CUT sweep · WIDE (all regimes) ===")
    for cc in COLD_CUT_GRID:
        line(metrics(run(large, bull, "wide", "all", cc), f"WIDE  cut={_lbl(cc)}"))
    hdr("=== COLD-CUT sweep · SWITCH (WIDE-bull + GOLD-clip-bear) ===")
    for cc in COLD_CUT_GRID:
        wb, _ = split(run(large, bull, "wide", "all", cc))
        _, gr = split(run(large, bull, "gold", "all", cc))
        line(metrics(wb + gr, f"SWITCH cut={_lbl(cc)}"))


if __name__ == "__main__":
    import sys
    if "--sweep-coldcut" in sys.argv:
        sweep_coldcut()
    else:
        main()
