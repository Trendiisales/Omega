#!/usr/bin/env python3
"""stall_clip_sweep_xau_rider4h.py -- certify (or honestly kill) a BE-ENTRY
FLOORED StallCompanion clip cell for XauTrendRider4h (bank-and-reload rider on
the XauTrendFollow4h host cells; LIVE, no giveback cover -- coverage audit OWED).

Input: leg-path CSV from backtest/clip_path_xau_rider4h.cpp (REAL engine + REAL
rider over certified 2022-2026 XAUUSD H1): trade_id,seq,exit_ms,dir,entry_px,
px,atr_pct,bull,cost_rt. Each rider leg (initial arm AND each bank-reload
segment) is its own companion leg -- the live snapshot keys exactly that way.

$-basis: rider lot 0.01 x tick_value_multiplier(XAUUSD)=100 = $1.00/XAU-point,
so upnl$ = dir*(px-entry) 1:1 (IBS $1/pt convention holds unchanged).
cost_usd = cost_rt * entry (IBKR XAU: 2*0.00015*px + spread).

Semantics = exact StallCompanion.hpp be-mode at H1-close grain, HONEST fills
(bank = actual close/exit at detection; gap tails book real negatives, S-17f).
Two LEVEL-anchored reclip variants (both restart-safe, only state = peak):
  cert  -- reopen at upnl >= peak+retrig+confirm, anchor = peak+retrig
           (the IBS-certified S-17t shape)
  gap25 -- reopen at upnl >= peak+retrig,        anchor = max(0,peak+retrig-confirm)
           (the SHIPPED S-17u StallCompanion be-mode; operator standard)
Plus the always-on USD mirror grid (kill/redundancy axis: mirrors every rider
loser -- expected to fail on a bank-and-reload parent whose losers are the
final host-exit legs). Judged STANDALONE (companion-independent rule).

usage: stall_clip_sweep_xau_rider4h.py <legs.csv> <LABEL>
"""
import sys, csv
from collections import defaultdict


def load(path):
    trades = defaultdict(list)
    with open(path) as f:
        for row in csv.DictReader(f):
            trades[int(row["trade_id"])].append((
                int(row["exit_ms"]), int(row["dir"]), float(row["entry_px"]),
                float(row["px"]), int(row["bull"]), float(row["cost_rt"])))
    return [trades[k] for k in sorted(trades)]


def sim_leg_floored(rows, confirm_usd, trail_usd, retrig_usd=25.0, cost_mult=1.0,
                    reclip_mode="cert"):
    """BE-ENTRY FLOORED mimic (BeFloorOnOpenFoundation). Books NOTHING until
    fav_usd >= confirm; opens ANCHORED; stop floored at anchor+cost; honest
    close fills (real gap tail can book < BE -- the LEVEL is >=BE, the fill is
    whatever it is). reclip_mode selects the LEVEL-anchored reclip variant."""
    entry = rows[0][2]; d = rows[0][1]
    cost_usd = rows[0][5] * entry * cost_mult
    banks = []
    open_pos = False; mfe = 0.0; last_upnl = 0.0
    clipped = False; peak = 0.0; anchor = 0.0
    for (_, _, _, px, _, _) in rows:
        upnl = d * (px - entry)                    # $ ($1/pt at 0.01 lot XAU)
        if clipped:
            if retrig_usd > 0.0 and peak > 0.0:
                if reclip_mode == "cert" and upnl >= peak + retrig_usd + confirm_usd:
                    clipped = False; anchor = peak + retrig_usd
                    open_pos = True; mfe = upnl
                elif reclip_mode == "gap25" and upnl >= peak + retrig_usd:
                    clipped = False; anchor = max(0.0, peak + retrig_usd - confirm_usd)
                    open_pos = True; mfe = upnl
                else:
                    continue
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


def sim_leg_usd(rows, arm_usd, trail_usd, retrig_usd=0.0, cold=-50.0):
    """Always-on USD-gauge StallBook mirror (existing book shape) -- the
    redundancy/kill axis: opens with the leg, mirrors every rider loser."""
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
        if mfe >= arm_usd and upnl <= mfe - trail_usd:
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


def evaluate(legs, fn):
    per_leg = [(rows[0][0], rows[0][4], fn(rows)) for rows in legs]
    all_banks = [b for (_, _, bs) in per_leg for (b, _) in bs]
    n, net, pf, worst = stats(all_banks)
    mid = len(per_leg) // 2
    h1 = sum(b for (_, _, bs) in per_leg[:mid] for (b, _) in bs)
    h2 = sum(b for (_, _, bs) in per_leg[mid:] for (b, _) in bs)
    bear = sum(b for (_, bu, bs) in per_leg if bu == 0 for (b, _) in bs)
    bull = sum(b for (_, bu, bs) in per_leg if bu == 1 for (b, _) in bs)
    nbear = sum(len(bs) for (_, bu, bs) in per_leg if bu == 0)
    return dict(n=n, net=net, pf=pf, worst=worst, h1=h1, h2=h2,
                bear=bear, bull=bull, nbear=nbear)


def main():
    path, label = sys.argv[1], sys.argv[2]
    legs = load(path)
    print(f"[{label}] {len(legs)} rider legs")
    hdr = (f"{'cnf$':>5} {'tr$':>5} | {'n':>4} {'net$':>8} {'PF':>6} {'worst':>7} "
           f"{'WF-H1':>8} {'WF-H2':>8} {'bear$':>8} verdict")
    for mode in ("cert", "gap25"):
        for retrig_usd in (0.0, 25.0):
            if retrig_usd == 0.0 and mode == "gap25":
                continue                       # reclip-off is variant-independent
            for cm in (1.0, 2.0):
                print(f"-- FLOORED be-entry [{mode}] retrig_usd={retrig_usd} cost_x{cm:.0f} --")
                print(hdr)
                for cnf in (10.0, 25.0, 50.0):
                    for tr in (25.0, 50.0, 75.0, 100.0):
                        r = evaluate(legs, lambda rows: sim_leg_floored(
                            rows, cnf, tr, retrig_usd, cm, mode))
                        ok = (r["net"] > 0 and r["pf"] >= 1.3
                              and r["h1"] > 0 and r["h2"] > 0)
                        v = "PASS" if ok else "fail"
                        print(f"{cnf:5.0f} {tr:5.0f} | {r['n']:4d} {r['net']:8.1f} "
                              f"{r['pf']:6.2f} {r['worst']:7.1f} {r['h1']:8.1f} "
                              f"{r['h2']:8.1f} {r['bear']:8.1f} {v}")
    # ── always-on USD mirror (existing StallBook shape; redundancy/kill axis) ──
    for retrig_usd in (0.0, 25.0):
        for cold in (-50.0, 0.0):
            print(f"-- ALWAYS-ON USD mirror retrig_usd={retrig_usd} "
                  f"cold_loss={'OFF' if cold == 0 else cold} --")
            print(hdr.replace('cnf$', 'arm$'))
            for arm in (25.0, 50.0, 75.0, 100.0):
                for tr in (25.0, 50.0, 75.0, 100.0):
                    r = evaluate(legs, lambda rows: sim_leg_usd(
                        rows, arm, tr, retrig_usd, cold))
                    ok = (r["net"] > 0 and r["pf"] >= 1.3
                          and r["h1"] > 0 and r["h2"] > 0)
                    v = "PASS" if ok else "fail"
                    print(f"{arm:5.0f} {tr:5.0f} | {r['n']:4d} {r['net']:8.1f} "
                          f"{r['pf']:6.2f} {r['worst']:7.1f} {r['h1']:8.1f} "
                          f"{r['h2']:8.1f} {r['bear']:8.1f} {v}")


if __name__ == "__main__":
    main()
