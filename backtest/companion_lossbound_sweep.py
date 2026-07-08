#!/usr/bin/env python3
"""COMPANION LOSS-BOUND study (S-2026-07-08, operator ask after the 2x -$51
XauTrendFollow4h companion LOSS_CUT clips in a 60pt/30min gold drop).

Question: can the USD-gauge stall-companion books' losses be bounded MORE
aggressively without killing the edge?  Levers tested (in order):
  1. COLD-LOSS LEVEL SWEEP   cold_loss_omega in {-15,-20,-25,-35,-50}
  2. VELOCITY FREEZE         block NEW companion opens/re-opens while the last
                             closed H1 bar dropped >= $X (existing rides keep
                             their normal trail/cut; NO exit is touched)
  3. REGIME-CONDITIONAL COLD tighter cut only while price-bear (RegimeState
                             port: H1 EMA200 falling w/ persist100 + EMA50<EMA200
                             + c<EMA200); baseline -50 otherwise
  4. combinations of best cells

Model = live-faithful StallBook USD-mode replay (include/StallCompanion.hpp):
  - companion opens when the parent trade appears (parent trades come from the
    REAL engine classes via backtest/clip_path_xau_tf.cpp paths)
  - arms once fav_usd >= arm_usd; hard $-trail peak-trail_usd once armed
  - cold-loss LOSS_CUT_CLIP at any time while open (pre-arm it is the only stop;
    post-arm peak-trail >= arm-trail > 0 so cold never fires post-arm on the
    gold configs)
  - RETRIG: after any clip, re-opens when fav_usd > prior_peak + retrig_usd
  - bull_only books gate opens/re-opens on gold_4h_bull_ (H4 SMA10>=SMA30,
    exact port of StallCompanion.hpp::gold_4h_bull_)
  - evaluation is per H1 bar, INTRABAR ADVERSE-FIRST: the bar's adverse extreme
    is applied to exits BEFORE the favourable extreme can arm/raise the peak
    (conservative; live polls every 60s so highs/lows are effectively seen)
  - clip exits are booked at the crossed level minus $1 poll slip (today's live
    -$50 cut booked -$50.57); gap-through opens book at the bar open.

STANDALONE judgment only (never vs riding WIDE — CompanionDominanceError).
All-6 bar: net>0, PF>=1.3, both WF halves>0, 2022-window>=0, ex-best>0, 2x-cost>0.

usage: companion_lossbound_sweep.py <paths.csv> <h1.csv> <h4.csv> <LABEL> <arm> <trail> <retrig> [bull_only]
"""
import sys, csv, bisect
from collections import defaultdict

# ---------- data ----------
def load_paths(fn):
    """trade list from clip_path_xau_tf.cpp output."""
    tr = {}
    with open(fn) as f:
        r = csv.reader(f); next(r)
        for row in r:
            tid = int(row[0]); ms = int(row[2])
            if tid not in tr:
                tr[tid] = dict(dir=int(row[3]), ent=float(row[4]), cost=float(row[8]) * float(row[4]),
                               bull=int(row[7]), first_ms=ms, last_ms=ms, last_px=float(row[5]))
            t = tr[tid]
            if ms >= t["last_ms"]:
                t["last_ms"] = ms; t["last_px"] = float(row[5])
    out, seen = [], set()
    for k in sorted(tr):
        t = tr[k]
        # LIVE KEYING: StallBook keys companions by book|eng|sym|round4(entry) -> two engine
        # cells holding the same entry price collapse into ONE companion. Dedupe to match.
        kk = (t["first_ms"], t["dir"], round(t["ent"], 4))
        if kk in seen: continue
        seen.add(kk); out.append(t)
    return out

def load_bars(fn):
    ts, o, h, l, c = [], [], [], [], []
    with open(fn) as f:
        for ln in f:
            p = ln.strip().split(",")
            if len(p) < 5 or not p[0][:1].isdigit(): continue
            t = int(float(p[0]));  t = t // 1000 if t > 10**11 else t
            ts.append(t); o.append(float(p[1])); h.append(float(p[2])); l.append(float(p[3])); c.append(float(p[4]))
    return ts, o, h, l, c

# ---------- regime / gates ----------
def regime_bear(closes):
    """RegimeState.hpp price-core port: EMA200 H1 + persist100 + EMA50; bear[i] usable at i+1."""
    EMA_S, EMA_F, PERSIST = 200, 50, 100
    kS, kF = 2.0 / (EMA_S + 1), 2.0 / (EMA_F + 1)
    emaS = emaF = None; hist = []; bear = []
    for i, cl in enumerate(closes):
        if emaS is None: emaS = emaF = cl
        else: emaS += kS * (cl - emaS); emaF += kF * (cl - emaF)
        hist.append(emaS)
        if len(hist) > PERSIST + 1: hist.pop(0)
        b = False
        if i + 1 >= EMA_S + PERSIST and len(hist) >= PERSIST + 1:
            if cl < emaS and emaS < hist[0] and emaF < emaS: b = True
        bear.append(b)
    return bear

def gold_bull_series(h4ts, h4c):
    """StallCompanion.hpp gold_4h_bull_ port: SMA10 vs SMA30 of H4 closes, per closed H4 bar."""
    out = []
    for i in range(len(h4c)):
        if i + 1 < 30: out.append(1 if h4c[i] >= sum(h4c[:i+1]) / (i+1) else 0); continue
        f = sum(h4c[i-9:i+1]) / 10.0; s = sum(h4c[i-29:i+1]) / 30.0
        out.append(1 if f >= s else 0)
    return out

# ---------- replay ----------
def replay(t, bars, cfg, aux):
    """One parent trade -> list of (exit_ms, pnl, reason). Live-faithful StallBook USD-mode."""
    ts, o, h, l, c = bars
    i0 = bisect.bisect_right(ts, t["first_ms"] // 1000) - 1
    i1 = bisect.bisect_right(ts, t["last_ms"] // 1000) - 1
    if i0 < 1: i0 = 1
    if i1 < i0: i1 = i0
    d, ent, cost = t["dir"], t["ent"], t["cost"]
    arm, trail, retrig, slip = cfg["arm"], cfg["trail"], cfg["retrig"], cfg["slip"]
    bear, bull4, h4ts = aux["bear"], aux["bull4"], aux["h4ts"]
    out = []
    open_, mfe = False, 0.0          # mfe tracked ALWAYS while open (live p.mfe_usd); armed = mfe >= arm
    clipped, prior_peak = False, 0.0
    cyc_open_fav = 0.0               # fav at THIS cycle's open (economic accounting: the live ledger
                                     # banks parent-entry-anchored upnl on every retrig cycle, which
                                     # double-counts overlapping price segments; econ pnl = fav_exit -
                                     # fav_at_cycle_open - cost is the tradeable per-contract truth)

    def gates_ok(i):
        # velocity freeze: last CLOSED H1 bar dropped >= $X -> no new opens/re-opens
        if cfg["freeze_x"] > 0 and i >= 2 and (c[i-1] - c[i-2]) <= -cfg["freeze_x"]: return False
        if cfg["bull_only"]:
            j = bisect.bisect_right(h4ts, ts[i] - 14400) - 1   # last CLOSED H4 before bar i
            if j < 0 or bull4[j] != 1: return False
        return True

    def cold_at(i):
        if cfg["cold_bear"] is not None and bear[i-1]: return cfg["cold_bear"]
        return cfg["cold"]

    for i in range(i0, i1 + 1):
        fav_o = d * (o[i] - ent)
        fav_lo = d * ((l[i] if d > 0 else h[i]) - ent)   # adverse extreme
        fav_hi = d * ((h[i] if d > 0 else l[i]) - ent)   # favourable extreme
        if not open_ and not clipped:
            if gates_ok(i):
                open_, mfe, cyc_open_fav = True, fav_o, fav_o
        if open_:
            # -- adverse leg first (conservative intrabar ordering) --
            armed = mfe >= arm
            cand = []
            if armed and fav_lo <= mfe - trail: cand.append((mfe - trail, "REVERSAL_CLIP"))
            cl = cold_at(i)
            if cl is not None and fav_lo <= cl: cand.append((cl, "LOSS_CUT_CLIP"))
            if cand:
                lvl, reason = max(cand)                   # highest level is crossed first on the way down
                eff = min(lvl, fav_o)                     # gap-through opens book at the open
                out.append((ts[i] * 1000, eff - slip - cost, reason, eff - slip - cyc_open_fav - cost))
                clipped, prior_peak, open_ = True, mfe, False
                # retrig may NOT re-fire inside the same bar (live needs a new poll extreme
                # beyond prior_peak+retrig; adverse-first already consumed this bar)
                continue
            # -- favourable leg --
            if fav_hi > mfe: mfe = fav_hi
        elif clipped and retrig > 0 and prior_peak > 0 and fav_hi > prior_peak + retrig:
            if gates_ok(i):
                open_, mfe, clipped = True, fav_hi, False
                cyc_open_fav = prior_peak + retrig        # econ: the re-opened contract fills at the
                                                          # retrig crossing, not back at parent entry
    if open_:
        fx = d * (t["last_px"] - ent)
        out.append((t["last_ms"], fx - cost, "ENGINE_EXIT", fx - cyc_open_fav - cost))
    return out

# ---------- metrics ----------
def metrics(rows, t_mid, y2022_end):
    """verdict metrics on ECONOMIC per-cycle pnl (rows[3]); ledger banked net (rows[1]) alongside."""
    if not rows: return None
    rows = sorted(rows); pnl = [r[3] for r in rows]
    w = sum(p for p in pnl if p > 0); lo = sum(-p for p in pnl if p < 0)
    pf = w / lo if lo > 0 else float("inf")
    eq = pk = dd = 0.0
    for p in pnl:
        eq += p; pk = max(pk, eq); dd = min(dd, eq - pk)
    net = sum(pnl)
    h1 = sum(r[3] for r in rows if r[0] <= t_mid); h2 = net - h1
    y22 = sum(r[3] for r in rows if r[0] < y2022_end)
    exb = net - max(pnl)
    nlc = sum(1 for r in rows if r[2] == "LOSS_CUT_CLIP")
    lcs = sum(r[3] for r in rows if r[2] == "LOSS_CUT_CLIP")
    return dict(n=len(pnl), net=net, led=sum(r[1] for r in rows), pf=pf, dd=dd, worst=min(pnl),
                h1=h1, h2=h2, y22=y22, exb=exb, nlc=nlc, lcs=lcs)

def run_cfg(trades, bars, aux, t_mid, y22e, **kw):
    cfg = dict(arm=kw["arm"], trail=kw["trail"], retrig=kw["retrig"], cold=kw.get("cold"),
               cold_bear=kw.get("cold_bear"), freeze_x=kw.get("freeze_x", 0.0),
               bull_only=kw.get("bull_only", False), slip=kw.get("slip", 1.0))
    rows = []
    for t in trades: rows += replay(t, bars, cfg, aux)
    m = metrics(rows, t_mid, y22e)
    # 2x-cost re-run
    rows2 = []
    for t in trades:
        t2 = dict(t); t2["cost"] = 2 * t["cost"]
        rows2 += replay(t2, bars, cfg, aux)
    m["x2"] = sum(r[3] for r in rows2)
    m["ok"] = (m["net"] > 0 and m["pf"] >= 1.3 and m["h1"] > 0 and m["h2"] > 0
               and m["y22"] >= 0 and m["exb"] > 0 and m["x2"] > 0)
    return m

HDR = (f"  {'cell':<20s}{'n':>5s}{'net$':>8s}{'ledg$':>8s}{'PF':>6s}{'DD$':>8s}{'worst':>7s}"
       f"{'H1$':>7s}{'H2$':>7s}{'2022$':>7s}{'exB$':>7s}{'2xC$':>7s}{'nLC':>5s}{'LC$':>7s}  V")
def prow(name, m):
    print(f"  {name:<20s}{m['n']:>5d}{m['net']:>8.0f}{m['led']:>8.0f}{m['pf']:>6.2f}{m['dd']:>8.0f}{m['worst']:>7.0f}"
          f"{m['h1']:>7.0f}{m['h2']:>7.0f}{m['y22']:>7.0f}{m['exb']:>7.0f}{m['x2']:>7.0f}{m['nlc']:>5d}{m['lcs']:>7.0f}  {'P' if m['ok'] else '.'}")

def main():
    paths, h1f, h4f, label = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    ARM, TRAIL, RETRIG = float(sys.argv[5]), float(sys.argv[6]), float(sys.argv[7])
    BULL = len(sys.argv) > 8 and sys.argv[8] == "bull_only"
    trades = load_paths(paths)
    bars = load_bars(h1f)
    h4ts, _, _, _, h4c = load_bars(h4f)
    aux = dict(bear=regime_bear(bars[4]), bull4=gold_bull_series(h4ts, h4c), h4ts=h4ts)
    t_mid = (trades[0]["first_ms"] + trades[-1]["last_ms"]) // 2
    y22e = 1672531200 * 1000  # 2023-01-01 UTC
    base = dict(arm=ARM, trail=TRAIL, retrig=RETRIG, bull_only=BULL)
    print(f"\n########## {label}  arm${ARM:.0f}/trail${TRAIL:.0f}/retrig${RETRIG:.0f}"
          f"{' BULL-GATED' if BULL else ''}  ({len(trades)} parent trades) ##########")
    pctbear = 100.0 * sum(aux["bear"]) / len(aux["bear"])
    print(f"  H1 bars={len(bars[0])}  price-bear bars={pctbear:.1f}%")

    print("\n[1] COLD-LOSS LEVEL SWEEP (live baseline cold=-50):"); print(HDR)
    res = {}
    for cl in [None, -50, -35, -25, -20, -15]:
        m = run_cfg(trades, bars, aux, t_mid, y22e, **base, cold=cl)
        res[cl] = m; prow("cold=none" if cl is None else f"cold={cl:.0f}", m)
    b50 = res[-50]["net"]
    print("  frontier vs -50 baseline: " + "  ".join(
        f"{cl}:{100*res[cl]['net']/b50:.0f}%net,worst{res[cl]['worst']:.0f}" for cl in [-35, -25, -20, -15]))

    print("\n[2] VELOCITY FREEZE (block NEW opens/re-opens while last closed H1 bar <= -$X; cold=-50):"); print(HDR)
    for fx in [0, 10, 15, 20, 25, 30, 40, 60]:
        m = run_cfg(trades, bars, aux, t_mid, y22e, **base, cold=-50, freeze_x=fx)
        prow("freeze off" if fx == 0 else f"freeze X=${fx}", m)

    print("\n[3] REGIME-CONDITIONAL COLD (tight only while price-bear, else -50):"); print(HDR)
    for cb in [-35, -25, -20, -15]:
        m = run_cfg(trades, bars, aux, t_mid, y22e, **base, cold=-50, cold_bear=cb)
        prow(f"bearcold={cb}", m)

    print("\n[4] COMBOS (best cold x freeze):"); print(HDR)
    for cl in [-25, -20, -15]:
        for fx in [15, 20, 30]:
            m = run_cfg(trades, bars, aux, t_mid, y22e, **base, cold=cl, freeze_x=fx)
            prow(f"cold={cl} frz=${fx}", m)

if __name__ == "__main__":
    main()
