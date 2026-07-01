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
# THE COMPANION IS A SEPARATE ENGINE. This cut bounds the COMPANION's OWN loser;
# it never touches the real engine (which has no clip -> WIDE). Judge it STANDALONE
# on the companion book, not by whole-book tot%.
#
# v1 (arm-gated, REJECTED): `not armed AND adverse <= -cut` fired on ANY trade still
# under GATE_PCT -- including trades that ran to +1.4%, dipped, then recovered. It
# converted dip-then-recover WINNERS into losers (WR 67->38, GOLD net 838->302).
# Too broad: "not armed yet" != "cold loser".
#
# v2 (MFE + time gated, this): target the SPECIFIC failure the operator named --
# a companion position that NEVER WORKED (opened, went red, never went green: the
# live SOL case MFE 0% / stall 1) or a bad re-open. Cut fires ONLY when:
#   (a) peak favourable excursion so far <= CUT_MFE_EPS  (never went green), AND
#   (b) held >= CUT_MINHOLD bars                         (genuinely stalled, not a
#                                                          1-bar noise spike), AND
#   (c) adverse from entry <= -CUT_ADV                   (direction-aware).
# A trade that showed ANY strength first (peak > eps) is EXEMPT -> the cut cannot
# scalp dip-then-recover winners; it only bounds the never-worked cold loser.
# CUT_ADV = 0.0 -> OFF. Sweep with --sweep-smartcut and read the GOLD (companion)
# book standalone: ship only if the companion book stays net-positive with
# worst%/maxDD improved (operator allocates on the risk/return, not vs WIDE).
CUT_MFE_EPS  = 0.003      # <=0.3% peak = "never went green"
CUT_MINHOLD  = 3          # bars held before the cut may fire
CUT_ADV      = 0.20       # catastrophe floor; VIABLE @0.20 (whole book PF 1.76->1.85,
                          # tot 838->882, worst -45->-41, maxDD -282->-235, ~15 fires/7yr).
                          # Self-harm below ~0.18 (starts scalping dip-then-recover winners).
CUT_ADV_GRID = [0.0, 0.080, 0.120, 0.160, 0.180, 0.200, 0.250, 0.300]
# legacy v1 knobs kept so --sweep-coldcut still reproduces the REJECTED result
COLD_CUT = 0.0
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


def simulate(closes, dates, bull, mode, cold_cut=0.0,
             cut_adv=0.0, cut_mfe_eps=CUT_MFE_EPS, cut_minhold=CUT_MINHOLD):
    """
    mode: 'wide'  = ride to max_hold (real-engine proxy, no clip)
          'gold'  = gold companion: profit-GATE then STALL/REVERSAL clip
    cold_cut: v1 arm-gated adverse stop (REJECTED, kept for reproducibility).
    cut_adv:  v2 MFE+time-gated cold cut (0.0 = off). Fires ONLY on a never-worked
              loser: peak MFE so far <= cut_mfe_eps AND held >= cut_minhold AND
              adverse <= -cut_adv. Exempts anything that showed strength first.
    Returns list of (entry_date, exit_date, pnl_pct, was_bull_entry, exit_tag).
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
        peak = entry_px; since_high = 0; exit_i = None; tag = "max_hold"
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
            held = k - i
            adverse = c / entry_px - 1.0             # LONG day-movers (invert for shorts live)
            # v1 (REJECTED): arm-gated -- fired on dip-then-recover winners too.
            if cold_cut > 0.0 and not armed and adverse <= -cold_cut:
                exit_i = k; tag = "cold_cut_v1"; break
            # v2 COLD CUT: never-worked loser only. peak<=eps (never went green)
            # + held>=minhold (not a 1-bar spike) + adverse past threshold. This
            # is the SOL "MFE 0% / stall 1 / pre-gate" case exactly. A trade that
            # showed ANY strength (fav>eps) is exempt -> cannot scalp recoverers.
            if (cut_adv > 0.0 and fav <= cut_mfe_eps and held >= cut_minhold
                    and adverse <= -cut_adv):
                exit_i = k; tag = "cold_cut"; break
            if mode == "gold":
                if armed and since_high >= N_STALL:                 # STALL_CLIP
                    exit_i = k; tag = "stall_clip"; break
                if armed and c <= peak * (1.0 - REV_GB):            # REVERSAL_CLIP
                    exit_i = k; tag = "reversal_clip"; break
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
        trades.append((dates[i], dates[exit_i], pnl, bull_entry, tag))
        i = exit_i + 1
    return trades


def _exit_for_entry(closes, n, i, mode, cut_adv, cut_mfe_eps, cut_minhold):
    """Compute exit for a SINGLE entry at index i (exogenous, parent-driven).
    Returns (exit_i, pnl, tag). Does NOT advance the entry scanner -- used by the
    paired evaluation so the cut changes only the exit, never entry timing (the
    live companion opens when the parent opens; cutting never spawns a new open)."""
    entry_px = closes[i]
    peak = entry_px; since_high = 0; exit_i = None; tag = "max_hold"
    for k in range(i + 1, min(i + 1 + MAX_HOLD, n)):
        c = closes[k]
        if not np.isfinite(c):
            continue
        if c > peak:
            peak = c; since_high = 0
        else:
            since_high += 1
        fav = peak / entry_px - 1.0
        armed = fav >= GATE_PCT
        held = k - i
        adverse = c / entry_px - 1.0
        if (cut_adv > 0.0 and fav <= cut_mfe_eps and held >= cut_minhold
                and adverse <= -cut_adv):
            exit_i = k; tag = "cold_cut"; break
        if mode == "gold":
            if armed and since_high >= N_STALL:
                exit_i = k; tag = "stall_clip"; break
            if armed and c <= peak * (1.0 - REV_GB):
                exit_i = k; tag = "reversal_clip"; break
    if exit_i is None:
        exit_i = min(i + MAX_HOLD, n - 1)
    ex_px = closes[exit_i]
    if not np.isfinite(ex_px):
        j = exit_i
        while j > i and not np.isfinite(closes[j]):
            j -= 1
        ex_px = closes[j]; exit_i = j
    if not np.isfinite(ex_px) or ex_px <= 0:
        return None
    return exit_i, ex_px / entry_px - 1.0 - COST_RT, tag


def entry_indices(closes, n):
    """The exogenous companion-entry indices (day>=5% + new-20d-high), independent
    of any exit rule -- these are the positions the parent hands the companion."""
    ret = np.empty(n); ret[0] = 0.0
    ret[1:] = closes[1:] / closes[:-1] - 1.0
    out = []; i = ATR_WIN + 1
    while i < n - 1:
        if not (ret[i] >= GATE and np.isfinite(closes[i])):
            i += 1; continue
        if closes[i] < np.nanmax(closes[max(0, i - CONTIN_K):i + 1]):
            i += 1; continue
        out.append(i)
        # advance past a nominal (cut=off) exit so entries don't overlap, matching
        # baseline entry cadence; exit rule is applied later, entries stay FIXED.
        r = _exit_for_entry(closes, n, i, "gold", 0.0, CUT_MFE_EPS, CUT_MINHOLD)
        i = (r[0] if r else i) + 1
    return out


def run_paired(df, bull, cut_adv):
    """Companion (gold) book on a FIXED exogenous entry set: only the EXIT changes
    with the cut, entries never move. This is the faithful standalone test -- the
    live companion opens on the parent, so a cut can't create new opens."""
    dates = df.index.to_numpy()
    allt = []
    for col in df.columns:
        closes = df[col].to_numpy(float); n = len(closes)
        for i in entry_indices(closes, n):
            r = _exit_for_entry(closes, n, i, "gold", cut_adv, CUT_MFE_EPS, CUT_MINHOLD)
            if r:
                bull_entry = bool(bull.iloc[i]) if i < len(bull) else False
                allt.append((dates[i], dates[r[0]], r[1], bull_entry, r[2]))
    return allt


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


def run(df, bull, mode, entry_filter, cold_cut=0.0, cut_adv=0.0):
    """entry_filter: 'all' | 'bull' (drop bear-entry trades = exclude-bear)."""
    dates = df.index.to_numpy()
    allt = []
    for col in df.columns:
        allt += simulate(df[col].to_numpy(float), dates, bull, mode,
                         cold_cut=cold_cut, cut_adv=cut_adv)
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


def sweep_smartcut():
    """v2 MFE+time-gated cold cut, judged STANDALONE on the companion (gold) book.

    The companion is a SEPARATE engine -- this asks only: is the companion's OWN
    book better protected with the cut? Ship a non-zero CUT_ADV if the companion
    book stays net-positive with worst%/maxDD improved (operator allocates on
    risk/return; NOT compared to WIDE). WIDE is shown once only as context."""
    df = load()
    large = df[[c for c in dict.fromkeys(LARGECAP) if c in df.columns]]
    bull = regime_flags(large).reset_index(drop=True)
    print(f"largecap={large.shape[1]} rows={len(df)} ({100*bull.mean():.0f}% bull) | "
          f"GATE_PCT={GATE_PCT:.3f} | v2 cut: MFE_EPS={CUT_MFE_EPS} MINHOLD={CUT_MINHOLD} "
          f"grid={CUT_ADV_GRID}")

    def _lbl(cc):
        return "off" if cc == 0 else f"{cc*100:.1f}%"

    def _cutstats(trades):
        cut = [t for t in trades if t[4] == "cold_cut"]
        if not cut:
            return "cuts=0"
        cp = np.array([t[2] for t in cut])
        return f"cuts={len(cut)} avg={cp.mean()*100:+.1f}% worst={cp.min()*100:+.1f}%"

    print("PAIRED: fixed exogenous entry set, only the EXIT changes with the cut "
          "(n is constant -> no re-entry confound).")
    hdr("=== v2 SMART-CUT · COMPANION (gold), ALL regimes [PAIRED STANDALONE] ===")
    for cc in CUT_ADV_GRID:
        t = run_paired(large, bull, cc)
        line(metrics(t, f"GOLD  cut={_lbl(cc)}")); print(f"        {_cutstats(t)}")
    hdr("=== v2 SMART-CUT · COMPANION (gold), BEAR-entry only [qualified sleeve] ===")
    for cc in CUT_ADV_GRID:
        _, gr = split(run_paired(large, bull, cc))
        line(metrics(gr, f"GOLD-bear cut={_lbl(cc)}")); print(f"        {_cutstats(gr)}")
    hdr("=== v2 SMART-CUT · COMPANION (gold), BULL-entry only ===")
    for cc in CUT_ADV_GRID:
        gb, _ = split(run_paired(large, bull, cc))
        line(metrics(gb, f"GOLD-bull cut={_lbl(cc)}")); print(f"        {_cutstats(gb)}")


if __name__ == "__main__":
    import sys
    if "--sweep-coldcut" in sys.argv:
        sweep_coldcut()
    elif "--sweep-smartcut" in sys.argv:
        sweep_smartcut()
    else:
        main()
