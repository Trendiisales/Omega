#!/usr/bin/env python3
"""stall_clip_sweep_xau_threebar30m.py -- certify a DEDICATED BE-ENTRY FLOORED
StallBook clip cell for XauThreeBar30m (the only LIVE engine still covered by
the uncertified main-book gold-era defaults gate_pct=1.5 / cold -50). S-2026-07-17t
IBS pattern (stall_clip_sweep_idx_bearshort.py) applied at the XAU basis.

Input: path CSV from backtest/clip_path_xau_threebar30m.cpp (REAL engine at the
FULL LIVE engine_init config, M1-close path grain = the live 60s companion drive
cadence): trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt.

$-BASIS (honest, stated): XAUUSD live lot = 0.01 (engine_init L4004);
tick_value_multiplier("XAUUSD") = $100/pt/lot (sizing.hpp) => $1/pt at live lot.
StallCompanion be-mode gauges fav_usd in RAW price points (its NOTE: valid while
parent size x tick-mult == 1) -- for this parent 0.01 x 100 = 1.0 EXACTLY, so
companion-USD == price-points == real USD. cost_rt*entry = IBKR RT cost in USD
at live lot (2*1.5bp commission + $0.30 spread; project-ibkr-cost-basis).

Simulates the EXACT StallCompanion.hpp S-2026-07-17t be-mode semantics per leg:
  - BE-ENTRY: flat until fav_usd >= confirm_usd (parent losers never mirrored)
  - open ANCHORED at parent entry (le=epx; anchor=0 fresh, level on reclip)
  - FLOOR_CLIP when fav_usd <= max(mfe - trail_usd, anchor + floor_cost)
    -- HONEST fill: banks the actual M1 close at detection (gap tails book < BE)
  - reclip variant A (shipped S-17t): reopen fav >= peak+retrig+confirm,
    anchor = peak+retrig
  - reclip variant B (gap-25, operator switch): reopen fav >= peak+retrig,
    anchor = peak+retrig-confirm (leg opens with confirm already in hand)
  - parent exit with companion open -> ENGINE_EXIT bank last mark - anchor
Judged STANDALONE (companion-independent rule; never vs WIDE).

usage: stall_clip_sweep_xau_threebar30m.py <path.csv> <LABEL>
"""
import sys, csv
from collections import defaultdict

def load(path):
    trades = defaultdict(list)
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            trades[int(row["trade_id"])].append((
                int(row["exit_ms"]), int(row["dir"]), float(row["entry_px"]),
                float(row["px"]), int(row["bull"]), float(row["cost_rt"])))
    return [trades[k] for k in sorted(trades)]

def sim_leg_floored(rows, confirm_usd, trail_usd, retrig_usd=25.0, cost_mult=1.0,
                    gap_reclip=False):
    """BE-ENTRY FLOORED mimic, exact StallCompanion.hpp be-mode.
    gap_reclip=False: variant A (S-17t shipped) reopen at peak+retrig+confirm,
                      anchor=peak+retrig.
    gap_reclip=True:  variant B (gap-25)      reopen at peak+retrig,
                      anchor=peak+retrig-confirm."""
    entry = rows[0][2]; d = rows[0][1]
    cost_usd = rows[0][5] * entry * cost_mult      # RT cost in USD at $1/pt
    banks = []
    open_pos = False; mfe = 0.0; last_upnl = 0.0
    clipped = False; peak = 0.0; anchor = 0.0
    for (_, _, _, px, _, _) in rows:
        upnl = d * (px - entry)                    # $ ($1/pt) == fav_usd
        if clipped:
            if gap_reclip:
                if retrig_usd > 0.0 and peak > 0.0 and upnl >= peak + retrig_usd:
                    clipped = False; anchor = peak + retrig_usd - confirm_usd
                    open_pos = True; mfe = upnl
                else:
                    continue
            else:
                if retrig_usd > 0.0 and peak > 0.0 and upnl >= peak + retrig_usd + confirm_usd:
                    clipped = False; anchor = peak + retrig_usd
                    open_pos = True; mfe = upnl
                else:
                    continue
        if not open_pos:
            if upnl < confirm_usd: continue        # BE-ENTRY: flat until confirmed
            open_pos = True; mfe = upnl
        if upnl > mfe: mfe = upnl
        last_upnl = upnl
        floor_lvl = anchor + cost_usd              # BE floor (anchored)
        stop_lvl = max(mfe - trail_usd, floor_lvl)
        if upnl <= stop_lvl:
            banks.append((upnl - anchor - cost_usd, "FLOOR_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
    if open_pos:
        banks.append((last_upnl - anchor - cost_usd, "ENGINE_EXIT"))
    return banks

def sim_leg_pct(rows, gate, gb, retrig=0.05, cold=-50.0):
    """Always-on PCT-gauge mirror = the CURRENT main-book cover shape
    (gate_pct/rev_gb/cold_loss_omega; anchored-honest reclip accounting)."""
    entry = rows[0][2]; d = rows[0][1]
    cost_usd = rows[0][5] * entry
    banks = []
    open_pos = False; mfe = 0.0; last_upnl = 0.0
    clipped = False; peak = 0.0; anchor = 0.0
    for (_, _, _, px, _, _) in rows:
        fav = (d * (px - entry) / entry) * 100.0
        upnl = d * (px - entry)
        if clipped:
            if retrig > 0.0 and peak > 0.0 and fav > peak * (1.0 + retrig):
                clipped = False; anchor = upnl
            else:
                continue
        if not open_pos:
            open_pos = True; mfe = fav
        if fav > mfe: mfe = fav
        last_upnl = upnl
        armed = mfe >= gate
        if armed and fav <= mfe * (1.0 - gb):
            banks.append((upnl - anchor - cost_usd, "REVERSAL_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
        if cold < 0.0 and upnl - anchor <= cold:
            banks.append((upnl - anchor - cost_usd, "LOSS_CUT_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
    if open_pos:
        banks.append((last_upnl - anchor - cost_usd, "ENGINE_EXIT"))
    return banks

def sim_leg_usd(rows, arm_usd, trail_usd, retrig_usd=0.0, cold=-50.0):
    """Always-on USD-gauge mirror (gold xau_tf*_usd precedent shape)."""
    entry = rows[0][2]; d = rows[0][1]
    cost_usd = rows[0][5] * entry
    banks = []
    open_pos = False; mfe = 0.0; last_upnl = 0.0
    clipped = False; peak = 0.0; anchor = 0.0
    for (_, _, _, px, _, _) in rows:
        upnl = d * (px - entry)
        if clipped:
            if retrig_usd > 0.0 and peak > 0.0 and upnl > peak + retrig_usd:
                clipped = False; anchor = upnl
            else:
                continue
        if not open_pos:
            open_pos = True; mfe = upnl
        if upnl > mfe: mfe = upnl
        last_upnl = upnl
        armed = mfe >= arm_usd
        if armed and upnl <= mfe - trail_usd:
            banks.append((upnl - anchor - cost_usd, "REVERSAL_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
        if cold < 0.0 and upnl - anchor <= cold:
            banks.append((upnl - anchor - cost_usd, "LOSS_CUT_CLIP")); open_pos = False
            clipped = True; peak = mfe; continue
    if open_pos:
        banks.append((last_upnl - anchor - cost_usd, "ENGINE_EXIT"))
    return banks

def stats(nets):
    n = len(nets); net = sum(nets)
    gp = sum(x for x in nets if x > 0); gl = -sum(x for x in nets if x < 0)
    pf = gp / gl if gl > 1e-9 else 999.0
    worst = min(nets) if nets else 0.0
    return n, net, pf, worst

def evaluate(legs, simfn):
    per_leg = [(rows[0][0], rows[0][4], simfn(rows)) for rows in legs]
    all_banks = [b for (_, _, bs) in per_leg for (b, _) in bs]
    n, net, pf, worst = stats(all_banks)
    mid = len(per_leg) // 2
    h1 = [b for (_, _, bs) in per_leg[:mid] for (b, _) in bs]
    h2 = [b for (_, _, bs) in per_leg[mid:] for (b, _) in bs]
    bear = [b for (_, bu, bs) in per_leg if bu == 0 for (b, _) in bs]
    bull = [b for (_, bu, bs) in per_leg if bu == 1 for (b, _) in bs]
    return dict(n=n, net=net, pf=pf, worst=worst,
                h1=sum(h1), h2=sum(h2), bear=sum(bear), bull=sum(bull))

def prow(tag, a, b, r):
    ok = (r["n"] >= 1 and r["net"] > 0 and r["pf"] >= 1.3 and r["h1"] > 0 and r["h2"] > 0)
    v = "PASS" if ok else "fail"
    print(f"{a:5.1f} {b:5.1f} | {r['n']:4d} {r['net']:8.2f} {r['pf']:6.2f} "
          f"{r['worst']:7.2f} {r['h1']:8.2f} {r['h2']:8.2f} {v}  {tag}")
    return ok

def main():
    path, label = sys.argv[1], sys.argv[2]
    legs = load(path)
    print(f"[{label}] {len(legs)} parent legs")
    hdr = (f"{'cnf$':>5} {'tr$':>5} | {'n':>4} {'net$':>8} {'PF':>6} {'worst':>7} "
           f"{'WF-H1':>8} {'WF-H2':>8} verdict")

    # ── FLOORED be-entry: MANDATED IBS-scale grid + GOLD-scaled grid, both variants ──
    for gap in (False, True):
        vname = "GAP-RECLIP(B)" if gap else "STD-RECLIP(A)"
        for grid_name, cnfs, trs, rtgs in (
            ("IBS-scale", (10.0, 25.0, 50.0), (25.0, 50.0, 75.0, 100.0), (0.0, 25.0)),
            ("GOLD-scale", (3.0, 5.0, 10.0), (3.0, 5.0, 10.0, 15.0, 25.0), (0.0, 5.0)),
        ):
            for rtg in rtgs:
                for cm in (1.0, 2.0):
                    print(f"-- FLOORED {vname} {grid_name} retrig_usd={rtg} cost_x{cm:.0f} --")
                    print(hdr)
                    for cnf in cnfs:
                        for tr in trs:
                            r = evaluate(legs, lambda rows, c=cnf, t=tr, rr=rtg, m=cm, g=gap:
                                         sim_leg_floored(rows, c, t, rr, m, g))
                            prow(f"{vname[:3]}/{grid_name}", cnf, tr, r)

    # ── Always-on mirrors (kill-evidence): PCT gauge incl. the LIVE main-book cell ──
    print("-- ALWAYS-ON PCT gauge (main-book shape; cold=-50, retrig=0.05) --")
    print(hdr.replace('cnf$', 'gate').replace('tr$', ' gb'))
    for gate in (0.25, 0.5, 0.75, 1.0, 1.5):
        for gb in (0.3, 0.5, 0.7):
            r = evaluate(legs, lambda rows, g=gate, b=gb: sim_leg_pct(rows, g, b))
            prow("PCT", gate, gb, r)
    print("-- ALWAYS-ON USD gauge (cold=-50, retrig off) --")
    print(hdr.replace('cnf$', 'arm$'))
    for arm in (5.0, 10.0, 25.0, 50.0):
        for tr in (3.0, 5.0, 10.0, 25.0):
            r = evaluate(legs, lambda rows, a=arm, t=tr: sim_leg_usd(rows, a, t))
            prow("USD", arm, tr, r)

if __name__ == "__main__":
    main()
