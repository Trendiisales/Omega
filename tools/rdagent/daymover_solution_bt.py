#!/usr/bin/env python3
"""
DAY-MOVER SOLUTION (S-2026-07-02) — the operator's full spec in one harness.

Operator spec (verbatim intent):
  "biggest daily movers ... if the daily chart shows a good trade we trade, we enter,
   we trade AGGRESSIVELY with the BE kicking in after a certain period/percentage
   (you determine what is optimum), and then we have all those COMPANION engines
   running INDEPENDENTLY of this trade at the percentages i gave [30/80/90], same
   as we have with crypto."

Two SEPARATE things, judged separately:

  (1) MAIN engine (the real aggressive trade). Enter on a big-day-mover daily signal,
      full size, RIDE. Operator (2026-07-02): "i don't want a breakeven STOP, i want a
      breakeven AND trade so we are covered a little if it reverses." So this is NOT an
      exit-at-flat: once the trade has captured >= ARM_PCT we raise a protective FLOOR
      to entry + LOCK (a little locked profit) and KEEP RIDING. Upside stays uncapped
      (rides to max_hold); the trade only exits early if price falls back through that
      floor (=> we still bank the small locked gain — "covered a little"). We SWEEP
      ARM_PCT x LOCK and pick the optimum on the MAIN book's own standalone metrics.
      This is the main engine's backtested adverse-protection verdict (Adverse-Protection
      Mandate).

  (2) COMPANIONS — three SEPARATE, INDEPENDENT, ADDITIVE books (operator rule + global
      CLAUDE.md CompanionDominanceError): a companion does NOT touch the main trade and
      is NEVER judged vs WIDE. It is judged STANDALONE — is its OWN book net-positive
      after costs, walk-forward both halves, both regimes? Each companion opens on the
      SAME entry as the main trade and clips on a give-back of PEAK PROFIT, using the
      CRYPTO stall_accountant semantics: clip when  fav <= mfe_pct * (1 - GB).
      Operator's staggered ladder: GB in {0.30, 0.80, 0.90}.
        GB=0.30 -> bank after giving back 30% of peak profit (tightest, locks early)
        GB=0.80 -> ride until 80% of peak profit is gone (loose)
        GB=0.90 -> ride until 90% of peak profit is gone (loosest, near ride-wide)
      All arm only after the profit-GATE (GATE_PCT) — the validated gold ingredient
      (no clip on chop). Operator (2026-07-02): "the companions should have the SAME
      protection we have for crypto to stop u losing money" — i.e. crypto's
      COLD_LOSS_CLIP: a hard adverse loss-cut so the companion book cannot bleed. We
      sweep that loss-cut (both the plain crypto-style adverse stop AND the narrower
      never-worked-only variant) per companion and ship the standalone verdict.

DATA: data/rdagent/sp500_long_close.csv — DAILY CLOSES, 27-name validated largecap
tier, 2019-06 -> 2026-06 (7yr: 2020 crash, 2022 bear, 2023-26 bull). CLOSE-ONLY =>
DAILY-CLOSE grade (honest provenance; not intraday). Equal-weight per-trade %, real
20bp round-trip cost, no pyramiding, regime split via basket close>200d-SMA.
"""
from __future__ import annotations
import numpy as np, pandas as pd
from pathlib import Path

CSV = Path.home() / "Omega" / "data" / "rdagent" / "sp500_long_close.csv"
GLITCH = {"POM", "CPWR", "MI"}
COST_RT = 0.002          # 20bp round-trip (IBKR commission + slip proxy)
COST_RT_STRESS = 0.004   # 40bp stress
ATR_WIN = 14

GATE      = 0.05         # entry: day return >= 5%
CONTIN_K  = 20           # + a new 20-day closing high (continuation, not a dead-cat)
MAX_HOLD  = 60           # ride window (trading days)

# --- companion (crypto stall_accountant) params ---
GATE_PCT  = 0.015        # profit-GATE: companion arms only after +1.5% captured
# operator (2026-07-02d): open companions in 10% increments up to 90% (up to 8 books);
# start from whatever the profit-onset scan says is the smallest net-positive GB.
GB_LADDER      = [0.30, 0.80, 0.90]                                   # legacy subset (PART2/3)
GB_LADDER_FULL = [0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90]  # full 10% grid

# --- companion loss-cut grid (crypto COLD_LOSS_CLIP; standalone protection verdict) ---
# PLAIN (crypto-style): clip whenever fav <= -cut (hard adverse stop, ungated).
# GATED (never-worked-only): peak MFE <= eps AND held >= minhold AND adverse <= -cut.
CUT_MFE_EPS = 0.003
CUT_MINHOLD = 3
CUT_GRID    = [0.0, 0.05, 0.08, 0.12, 0.16, 0.20, 0.30]

# --- main-engine break-even-and-RIDE arm grid ---
# BE_ARM = capture % after which the protective floor arms.
# BE_LOCK = profit locked into the floor (entry+LOCK) so we're "covered a little";
#           the trade KEEPS RIDING and only exits if price falls back through the floor.
BE_ARM_GRID  = [0.02, 0.03, 0.04, 0.05, 0.06, 0.08]
BE_LOCK_GRID = [0.0, 0.003, 0.005, 0.010]   # 0 / +0.3% / +0.5% / +1.0% locked floor

LARGECAP = ("AAPL MSFT NVDA AMZN GOOGL GOOG META TSLA AVGO ORCL AMD MU INTC QCOM TXN ADI "
            "LRCX AMAT KLAC MRVL ARM SMCI PLTR CRWD PANW SNOW NOW CRM ADBE SHOP NFLX UBER "
            "ABNB COIN MSTR DELL ANET CDNS SNPS WDC ON MCHP NXPI HPQ").split()


def load():
    df = pd.read_csv(CSV, index_col=0, parse_dates=True).sort_index()
    cov = df.notna().mean()
    keep = [c for c in df.columns[cov >= 0.90] if c not in GLITCH]
    df = df[keep]
    return df[[c for c in dict.fromkeys(LARGECAP) if c in df.columns]]


def regime_flags(df):
    norm = df / df.iloc[0]
    basket = norm.mean(axis=1)
    sma = basket.rolling(200, min_periods=200).mean()
    return (basket > sma).reset_index(drop=True)   # bull=True; warmup NaN -> False


def entry_indices(closes, n):
    """Exogenous entries: day >= GATE and a new CONTIN_K-day closing high. These are
    the positions the parent hands to each companion (independent of any exit rule)."""
    ret = np.empty(n); ret[0] = 0.0; ret[1:] = closes[1:] / closes[:-1] - 1.0
    out = []; i = ATR_WIN + 1
    while i < n - 1:
        if ret[i] >= GATE and np.isfinite(closes[i]) and \
           closes[i] >= np.nanmax(closes[max(0, i - CONTIN_K):i + 1]):
            out.append(i)
            i += 1                 # allow same-name re-arm next day; overlap resolved per-book
        else:
            i += 1
    return out


def _finite_exit(closes, exit_i, i):
    ex = closes[exit_i]
    if not np.isfinite(ex):
        j = exit_i
        while j > i and not np.isfinite(closes[j]):
            j -= 1
        ex = closes[j]; exit_i = j
    return ex, exit_i


def atr_pct_arr(closes):
    """Close-only ATR proxy: rolling mean of |daily return| over ATR_WIN. Used to place
    an ATR-multiple fast-reversal stop (we only have daily closes, so this is a
    volatility-scaled stop, not a true-range ATR — honest provenance)."""
    n = len(closes); ret = np.zeros(n)
    with np.errstate(invalid="ignore", divide="ignore"):
        ret[1:] = np.abs(closes[1:] / closes[:-1] - 1.0)
    return pd.Series(ret).rolling(ATR_WIN, min_periods=ATR_WIN).mean().to_numpy()


def main_trade(closes, n, i, be_arm, cost_rt, be_lock=0.0, time_arm=None,
               init_stop=0.0, atr_stop=0.0, atr=None, rev_days=0, rev_cut=0.0):
    """MAIN engine exit for entry i — BREAK-EVEN-AND-RIDE (not an exit-at-flat).
    Rides to MAX_HOLD. Once peak-fav >= be_arm (or, if time_arm set, held>=time_arm
    AND in profit), a protective FLOOR arms at entry*(1+be_lock). The trade KEEPS
    RIDING for upside; it exits early ONLY if a close falls back through that floor
    (=> banks the small locked gain: "covered a little if it reverses").

    FAST-REVERSAL protection (operator Q, §3b.3 — catches the straight-down-from-entry
    loser the be-floor cannot, because the floor only arms after profit). Any of these
    fire regardless of arm state:
      init_stop>0 -> hard stop at entry*(1-init_stop).
      atr_stop>0  -> vol-scaled stop at entry*(1 - atr_stop*atr_pct[i]) (close-only ATR).
      rev_days>0  -> day-N adverse cut: within the first rev_days bars, cut if fav<=-rev_cut.
    Returns (exit_i, pnl, tag) or None."""
    entry_px = closes[i]; peak = entry_px; armed = False; exit_i = None; tag = "max_hold"
    floor_px = entry_px * (1.0 + be_lock)
    hard_px = entry_px * (1.0 - init_stop) if init_stop > 0 else None
    astop_px = (entry_px * (1.0 - atr_stop * atr[i])
                if atr_stop > 0 and atr is not None and np.isfinite(atr[i]) else None)
    for k in range(i + 1, min(i + 1 + MAX_HOLD, n)):
        c = closes[k]
        if not np.isfinite(c):
            continue
        held = k - i
        fav_now = c / entry_px - 1.0
        # --- fast-reversal stops (fire before/independent of the be-floor) ---
        if hard_px is not None and c <= hard_px:
            exit_i = k; tag = "init_stop"; break
        if astop_px is not None and c <= astop_px:
            exit_i = k; tag = "atr_stop"; break
        if rev_days > 0 and held <= rev_days and fav_now <= -rev_cut:
            exit_i = k; tag = "rev_cut"; break
        if c > peak:
            peak = c
        fav = peak / entry_px - 1.0
        if not armed and (fav >= be_arm or
                          (time_arm is not None and held >= time_arm and c > entry_px)):
            armed = True
        if armed and c <= floor_px:
            exit_i = k; tag = "be_floor"; break
    if exit_i is None:
        exit_i = min(i + MAX_HOLD, n - 1)
    ex, exit_i = _finite_exit(closes, exit_i, i)
    if not np.isfinite(ex) or ex <= 0:
        return None
    return exit_i, ex / entry_px - 1.0 - cost_rt, tag


def companion_trade(closes, n, i, gb, cut, cost_rt, cut_mode="plain"):
    """COMPANION exit for entry i (crypto stall_accountant semantics). Arms after the
    profit-GATE; clips when fav <= mfe_pct*(1-gb) (give-back of PEAK PROFIT).
    Loss-cut (crypto COLD_LOSS_CLIP, cut>0):
      cut_mode='plain' -> hard adverse stop, ungated: clip whenever fav <= -cut.
      cut_mode='gated' -> never-worked-only: mfe<=eps AND held>=minhold AND fav<=-cut.
    Independent book — never touches the main trade."""
    entry_px = closes[i]; peak = entry_px; exit_i = None; tag = "max_hold"
    for k in range(i + 1, min(i + 1 + MAX_HOLD, n)):
        c = closes[k]
        if not np.isfinite(c):
            continue
        if c > peak:
            peak = c
        mfe = peak / entry_px - 1.0
        fav = c / entry_px - 1.0
        held = k - i
        armed = mfe >= GATE_PCT
        # crypto-style loss-cut (stop the companion bleeding)
        if cut > 0.0:
            if cut_mode == "plain" and fav <= -cut:
                exit_i = k; tag = "loss_cut"; break
            if cut_mode == "gated" and mfe <= CUT_MFE_EPS and held >= CUT_MINHOLD and fav <= -cut:
                exit_i = k; tag = "loss_cut"; break
        # give-back-of-peak-profit clip (crypto: fav <= mfe*(1-gb))
        if armed and fav <= mfe * (1.0 - gb):
            exit_i = k; tag = "gb_clip"; break
    if exit_i is None:
        exit_i = min(i + MAX_HOLD, n - 1)
    ex, exit_i = _finite_exit(closes, exit_i, i)
    if not np.isfinite(ex) or ex <= 0:
        return None
    return exit_i, ex / entry_px - 1.0 - cost_rt, tag


def build_book(df, bull, kind, cost_rt=COST_RT, **kw):
    """kind: 'wide' (ride, no floor) | 'be' (main+breakeven) | 'comp' (companion).
    Fast-reversal kwargs (init_stop/atr_stop/rev_days+rev_cut) apply to 'wide' and 'be'."""
    dates = df.index.to_numpy(); allt = []
    fr = dict(init_stop=kw.get("init_stop", 0.0), atr_stop=kw.get("atr_stop", 0.0),
              rev_days=kw.get("rev_days", 0), rev_cut=kw.get("rev_cut", 0.0))
    need_atr = fr["atr_stop"] > 0
    for col in df.columns:
        closes = df[col].to_numpy(float); n = len(closes)
        atr = atr_pct_arr(closes) if need_atr else None
        for i in entry_indices(closes, n):
            if kind == "wide":
                r = main_trade(closes, n, i, be_arm=1e9, cost_rt=cost_rt, atr=atr, **fr)
            elif kind == "be":
                r = main_trade(closes, n, i, be_arm=kw["be_arm"], cost_rt=cost_rt,
                               be_lock=kw.get("be_lock", 0.0), time_arm=kw.get("time_arm"),
                               atr=atr, **fr)
            else:
                r = companion_trade(closes, n, i, kw["gb"], kw.get("cut", 0.0), cost_rt,
                                    cut_mode=kw.get("cut_mode", "plain"))
            if r:
                be = bool(bull.iloc[i]) if i < len(bull) else False
                allt.append((dates[i], dates[r[0]], r[1], be, r[2]))
    return allt


def metrics(trades, label):
    if not trades:
        return {"label": label, "n": 0, "pf": 0, "wr": 0, "avg": 0, "worst": 0,
                "dd": 0, "h1": 0, "h2": 0, "tot": 0}
    pnl = np.array([t[2] for t in trades])
    w = pnl[pnl > 0].sum(); l = abs(pnl[pnl < 0].sum())
    pf = w / l if l else float("inf")
    order = np.argsort(pd.to_datetime([t[1] for t in trades]).values)
    eq = np.cumsum(pnl[order]); dd = (eq - np.maximum.accumulate(eq)).min()
    ent = pd.to_datetime([t[0] for t in trades]).values
    mid = np.median(ent.astype("int64"))
    a1 = pnl[ent.astype("int64") <= mid]; a2 = pnl[ent.astype("int64") > mid]
    def _pf(a):
        ww = a[a > 0].sum(); ll = abs(a[a < 0].sum()); return ww / ll if ll else float("inf")
    return {"label": label, "n": len(trades), "pf": pf, "wr": (pnl > 0).mean() * 100,
            "avg": pnl.mean() * 100, "worst": pnl.min() * 100, "dd": dd * 100,
            "h1": _pf(a1), "h2": _pf(a2), "tot": pnl.sum() * 100}


def split(tr):
    return [t for t in tr if t[3]], [t for t in tr if not t[3]]


def hdr(t):
    print(f"\n{t}")
    print(f"{'config':32s} {'n':>5s} {'PF':>6s} {'WR%':>5s} {'avg%':>6s} {'tot%':>8s} "
          f"{'worst%':>7s} {'maxDD%':>7s} {'H1':>5s} {'H2':>5s}")
    print("-" * 98)


def line(r):
    print(f"{r['label']:32s} {r['n']:5d} {r['pf']:6.2f} {r['wr']:5.0f} {r['avg']:6.2f} "
          f"{r['tot']:8.1f} {r['worst']:7.1f} {r['dd']:7.1f} {r['h1']:5.2f} {r['h2']:5.2f}")


def main():
    df = load(); bull = regime_flags(df)
    print(f"largecap={df.shape[1]} dates={df.index.min().date()}->{df.index.max().date()} "
          f"rows={len(df)} bull={100*bull.mean():.0f}%")
    print(f"entry: day>=5% + new {CONTIN_K}d-high | max_hold={MAX_HOLD} | "
          f"cost={COST_RT*100:.2f}% ({COST_RT*10000:.0f}bp) rt")

    # ============ PART 1: MAIN ENGINE — breakeven-and-ride sweep ============
    print("\n" + "=" * 98)
    print("PART 1 — MAIN aggressive trade: BREAK-EVEN-AND-RIDE (arm a protective floor,")
    print("         keep riding for upside). Sweep arm% x locked-floor. Pick optimum on MAIN book.")
    wide = build_book(df, bull, "wide")
    hdr("=== MAIN book: WIDE (ride, no floor) vs breakeven-and-ride(arm, lock) ===")
    line(metrics(wide, "WIDE (no floor / pure ride)"))
    for arm in BE_ARM_GRID:
        for lock in BE_LOCK_GRID:
            r = build_book(df, bull, "be", be_arm=arm, be_lock=lock)
            line(metrics(r, f"arm={arm*100:.0f}% lock=+{lock*100:.1f}%"))

    # regime split: WIDE vs a representative floor
    hdr("=== MAIN book regime split: WIDE vs arm=4% lock=+0.5% ===")
    be4 = build_book(df, bull, "be", be_arm=0.04, be_lock=0.005)
    wb, wr = split(wide); bb, br = split(be4)
    line(metrics(wb, "WIDE bull-entry")); line(metrics(bb, "BE4L5 bull-entry"))
    line(metrics(wr, "WIDE bear-entry")); line(metrics(br, "BE4L5 bear-entry"))

    # ============ PART 2: COMPANIONS — 30/80/90, STANDALONE ============
    print("\n" + "=" * 98)
    print("PART 2 — COMPANIONS (independent/additive, judged STANDALONE, NEVER vs WIDE)")
    print("crypto semantics: arm after +1.5% gate, clip when fav <= peak_profit*(1-GB)")
    for gb in GB_LADDER:
        comp = build_book(df, bull, "comp", gb=gb)
        cb, cr = split(comp)
        hdr(f"=== COMPANION GB={gb*100:.0f}% (give back {gb*100:.0f}% of peak profit) — STANDALONE ===")
        line(metrics(comp, f"GB{gb*100:.0f} all regimes"))
        line(metrics(cb,   f"GB{gb*100:.0f} bull-entry"))
        line(metrics(cr,   f"GB{gb*100:.0f} bear-entry"))

    # ============ PART 3: companion loss-cut verdict (crypto COLD_LOSS_CLIP) ============
    print("\n" + "=" * 98)
    print("PART 3 — COMPANION loss-cut sweep (crypto-style protection, STANDALONE).")
    print("         PLAIN = hard adverse stop (fav<=-cut, ungated, exactly like crypto COLD_LOSS).")
    for gb in GB_LADDER:
        hdr(f"=== GB{gb*100:.0f} PLAIN loss-cut sweep (crypto-style) — STANDALONE ===")
        for cut in CUT_GRID:
            lbl = "off" if cut == 0 else f"-{cut*100:.0f}%"
            t = build_book(df, bull, "comp", gb=gb, cut=cut, cut_mode="plain")
            nc = sum(1 for x in t if x[4] == "loss_cut")
            line(metrics(t, f"GB{gb*100:.0f} plain-cut={lbl} (fires {nc})"))
    # narrower never-worked-only variant, for the tightest companion (GB30) as contrast
    hdr("=== GB30 GATED loss-cut (never-worked-only) — STANDALONE contrast ===")
    for cut in CUT_GRID:
        lbl = "off" if cut == 0 else f"-{cut*100:.0f}%"
        t = build_book(df, bull, "comp", gb=0.30, cut=cut, cut_mode="gated")
        nc = sum(1 for x in t if x[4] == "loss_cut")
        line(metrics(t, f"GB30 gated-cut={lbl} (fires {nc})"))

    # ============ PART 4: cost stress ============
    print("\n" + "=" * 98)
    print("PART 4 — cost stress (40bp rt) on MAIN (arm4/lock0.5) + each companion")
    hdr("=== 40bp round-trip stress ===")
    line(metrics(build_book(df, bull, "be", be_arm=0.04, be_lock=0.005,
                            cost_rt=COST_RT_STRESS), "MAIN arm4/lock5 @40bp"))
    for gb in GB_LADDER:
        line(metrics(build_book(df, bull, "comp", gb=gb, cost_rt=COST_RT_STRESS),
                     f"COMP GB{gb*100:.0f} @40bp"))

    # ============ PART 5: TWO independent books A(WIDE)+B(arm4) + FAST-REVERSAL =====
    # operator §3b.2/§3b.3: open TWO separate trades on the SAME signal, same time —
    # A = WIDE ride, B = arm=4% breakeven-and-ride floor. Then the open question:
    # how to protect against a QUICK REVERSAL (the straight-down worst%=-45 loser the
    # be-floor CANNOT catch). Sweep init/ATR/day-N stops on BOTH; quantify the cost.
    print("\n" + "=" * 98)
    print("PART 5 — TWO independent books A=WIDE + B=arm4/lock0.5, and FAST-REVERSAL")
    print("         protection for each (the be-floor arms only in profit; these catch the")
    print("         straight-down-from-entry loser). Report standalone cost of each stop.")
    A0 = build_book(df, bull, "wide")
    B0 = build_book(df, bull, "be", be_arm=0.04, be_lock=0.005)
    hdr("=== baseline: A=WIDE, B=arm4/lock0.5, and additive A+B book ===")
    line(metrics(A0, "A: WIDE (no fast-rev stop)"))
    line(metrics(B0, "B: arm4/lock0.5 (no fast-rev stop)"))
    line(metrics(A0 + B0, "A+B additive (both opened)"))

    hdr("=== A=WIDE + fast-reversal stop (cost of protecting the ride) ===")
    for isv in [0.08, 0.12, 0.16, 0.20]:
        t = build_book(df, bull, "wide", init_stop=isv)
        nf = sum(1 for x in t if x[4] == "init_stop")
        line(metrics(t, f"A init_stop -{isv*100:.0f}% (fires {nf})"))
    for av in [2.0, 3.0, 4.0]:
        t = build_book(df, bull, "wide", atr_stop=av)
        nf = sum(1 for x in t if x[4] == "atr_stop")
        line(metrics(t, f"A atr_stop {av:.0f}xATR (fires {nf})"))
    for rc in [0.08, 0.12, 0.16]:
        t = build_book(df, bull, "wide", rev_days=3, rev_cut=rc)
        nf = sum(1 for x in t if x[4] == "rev_cut")
        line(metrics(t, f"A day<=3 cut -{rc*100:.0f}% (fires {nf})"))

    hdr("=== B=arm4/lock0.5 + fast-reversal stop ===")
    for isv in [0.08, 0.12, 0.16, 0.20]:
        t = build_book(df, bull, "be", be_arm=0.04, be_lock=0.005, init_stop=isv)
        nf = sum(1 for x in t if x[4] == "init_stop")
        line(metrics(t, f"B init_stop -{isv*100:.0f}% (fires {nf})"))
    for av in [2.0, 3.0, 4.0]:
        t = build_book(df, bull, "be", be_arm=0.04, be_lock=0.005, atr_stop=av)
        nf = sum(1 for x in t if x[4] == "atr_stop")
        line(metrics(t, f"B atr_stop {av:.0f}xATR (fires {nf})"))
    for rc in [0.08, 0.12, 0.16]:
        t = build_book(df, bull, "be", be_arm=0.04, be_lock=0.005, rev_days=3, rev_cut=rc)
        nf = sum(1 for x in t if x[4] == "rev_cut")
        line(metrics(t, f"B day<=3 cut -{rc*100:.0f}% (fires {nf})"))

    # ============ PART 6: FULL COMPANION LADDER + PROFIT-ONSET SCAN ============
    # operator §3b.4: sweep GB 10%->90%; find the SMALLEST GB that is net-positive
    # STANDALONE (all-regime PF>1 & tot>0, BOTH walk-forward halves PF>1, BOTH regimes
    # net-positive). Then ladder companions in 10% increments up to 90% (up to 8 books).
    print("\n" + "=" * 98)
    print("PART 6 — FULL companion ladder GB 10->90% + PROFIT-ONSET scan (STANDALONE).")
    print("         pass = all PF>1 & tot>0, H1>1, H2>1, bull tot>0, bear tot>=0.")
    hdr("=== companion ladder — STANDALONE per GB (all / bull / bear) ===")
    onset = None
    for gb in GB_LADDER_FULL:
        comp = build_book(df, bull, "comp", gb=gb)
        cb, cr = split(comp)
        ma = metrics(comp, f"GB{gb*100:.0f} all"); mb = metrics(cb, f"GB{gb*100:.0f} bull")
        mr = metrics(cr, f"GB{gb*100:.0f} bear")
        ok = (ma["pf"] > 1 and ma["tot"] > 0 and ma["h1"] > 1 and ma["h2"] > 1
              and mb["tot"] > 0 and mr["tot"] >= 0)
        line(ma); line(mb); line(mr)
        print(f"    -> GB{gb*100:.0f} standalone verdict: {'PASS' if ok else 'FAIL'}")
        if ok and onset is None:
            onset = gb
    print(f"\nPROFIT-ONSET: smallest net-positive-standalone give-back = "
          f"{'GB'+str(int(onset*100)) if onset else 'NONE'}")
    if onset:
        ladder = [g for g in GB_LADDER_FULL if g >= onset]
        print(f"LADDER to run as independent books ({len(ladder)}): "
              f"{[f'GB{int(g*100)}' for g in ladder]}")
        hdr("=== ladder companions + crypto -20% loss-cut verdict (STANDALONE) ===")
        for gb in ladder:
            base = build_book(df, bull, "comp", gb=gb)
            cut20 = build_book(df, bull, "comp", gb=gb, cut=0.20, cut_mode="plain")
            nc = sum(1 for x in cut20 if x[4] == "loss_cut")
            line(metrics(base, f"GB{gb*100:.0f} no-cut"))
            line(metrics(cut20, f"GB{gb*100:.0f} +(-20% cut, fires {nc})"))


if __name__ == "__main__":
    main()
