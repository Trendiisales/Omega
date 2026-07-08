#!/usr/bin/env python3
"""
Generic x2-mirror companion check over ANY parent engine's dumped trades
(S-2026-07-07t operator ask: "check all the others by testing").

Parents: ShadowBook_mi trade CSV (entryTs,symbol,side,engine,entryPrice,exitPrice,
...,exitTs derivable) OR the connors-style dump CSV. Mirror sim = the validated
ConnorsMirror mechanism (connors_mirror_bt.sim, close-eval worse-of): arm at
parent gain >= ARM over parent entry on the symbol's H1 closes, x2 add-on at that
close, trail GB from own peak, retrig, flat-on-parent-close. Judged STANDALONE.

usage:
  TICKDIR=~/Tick python3 backtest/mirror_parents_bt.py <parents.csv> [<parents2.csv> ...]

Groups by engine tag; per (engine, symbol) prints the validated cell + plateau.
Supports LONG and SHORT parents (short mirrors = mirrored arithmetic via price
negation trick is NOT used; shorts are simmed with a sign-flipped series).
"""
import os, sys, csv, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("TICKDIR", os.path.expanduser("~/Tick"))
import rider_sweep_wf as RW
import connors_mirror_bt as CM

RT_BP = {"XAUUSD": 3.4, "NAS100": 3.0, "USTEC.F": 3.0, "US500": 4.0, "US500.F": 4.0,
         "DJ30": 2.0, "DJ30.F": 2.0, "GER40": 2.0, "UK100": 3.0,
         "MGC": 1.2}   # MGC: 0.4pt flat rt (MgcFastDonchianBacktest convention) ~= 1.2bp @3300
SYM_MAP = {"USTEC.F": "NAS100", "US500.F": "US500", "DJ30.F": "DJ30"}

ARMS = [0.015, 0.020]
GBS  = [0.005, 0.0075, 0.010]
RETS = [0.0, 0.02]
# BE-floor exit modes (operator 2026-07-07u: "reverse on entry -> close, loss = cost only"):
#   "-"   = pure trail (validated mechanism, unchanged)
#   "beC" = floor stop at entry px, H1 CLOSE-eval worse-of (wireable on current
#           MirrorBook chassis as-is; a close gapping below entry still fills at
#           the close, so loss can exceed cost -- honest close-eval)
#   "beT" = floor at entry px hit INTRABAR (H1 low touch -> fill AT entry px,
#           loss = cost exactly; live needs the 60s registry drive to eval the
#           floor between H1 closes -- feasible, MirrorBook already ticks 60s)
FLOORS = ["-", "beC", "beT"]

def sim_live(parents, H1, arm, gb, rt_bp, retrig, floor="-"):
    """Wired-MirrorBook window: H1 bars strictly after parent ENTRY TS through
    parent EXIT TS (the live book arms from the real entry moment; the
    connors_mirror_bt.sim day-skip was an anti-lookahead device for D1-close
    parents and makes sub-day parents untestable). Same close-eval worse-of
    state machine otherwise. floor: see FLOORS."""
    import bisect
    ts, o, h, l, c = H1
    trades = []
    for (ets, xts, epx, xpx, psz) in parents:
        i = bisect.bisect_right(ts, ets)
        j = bisect.bisect_right(ts, xts)
        if i >= j: continue
        armed = False; peak = 0.0; en = 0.0; last_clip_peak = None; done = False
        ent_ts = 0
        for k in range(i, j):
            if not armed:
                if done and not retrig: break
                trig = last_clip_peak * (1.0 + retrig) if done else epx * (1.0 + arm)
                if c[k] >= trig:
                    armed = True; en = c[k]; peak = c[k]; ent_ts = ts[k]
                continue
            if floor == "beT" and k > i and l[k] <= en:
                # intrabar BE-floor touch: fill at entry -> pnl = -cost
                trades.append((ent_ts, -abs(en) * rt_bp * 1e-4))
                armed = False; last_clip_peak = peak; done = True
                continue
            stop = peak * (1.0 - gb)
            if floor == "beC":
                stop = max(stop, en)
            if c[k] <= stop:
                fill = min(c[k], stop)
                trades.append((ent_ts, (fill - en) - abs(en) * rt_bp * 1e-4))
                armed = False; last_clip_peak = peak; done = True
                continue
            if c[k] > peak: peak = c[k]
        if armed:
            fill = c[j - 1]
            trades.append((ent_ts, (fill - en) - abs(en) * rt_bp * 1e-4))
    return trades

FAMILY = [  # collapse ShadowBook per-cell tags -> live engine family (mirror keys on the family)
    ("XauTrendFollow1h", "XauTrendFollow1h"), ("XauTrendFollow2h", "XauTrendFollow2h"),
    ("XauTrendFollow4h", "XauTrendFollow4h"), ("XAU_4h_", "XauTrendFollow4h"),
    ("XauTrendFollowD1", "XauTrendFollowD1"), ("XAU_D1", "XauTrendFollowD1"),
    ("XauTurtleD1", "XauTurtleD1"), ("GoldVolBreakoutM30", "GoldVolBreakoutM30"),
]
# ONLY the operator-named viable parents get simmed from a ShadowBook dump
# (ShadowBook drives every engine in ITS list incl. ones no longer enabled on main)
INCLUDE_FAMS = {"XauTrendFollow1h", "XauTrendFollow2h", "XauTrendFollow4h", "XauTrendFollowD1",
                "XauTurtleD1", "GoldVolBreakoutM30", "NAS100", "US500", "DJ30",
                "MgcFastDonchian30m", "MgcVolBrkM30"}
def fam_of(eng):
    for pre, fam in FAMILY:
        if pre in eng: return fam
    return eng

def load_shadowbook(path):
    """ShadowBook trade csv -> {(engine,symbol,side): [(ets,xts,epx,xpx,size)]}"""
    out = {}
    with open(path) as f:
        rd = csv.DictReader(f)
        if rd.fieldnames and "entry_ts" in rd.fieldnames:
            # connors_mirror_dump.cpp schema (ConnorsRSI2 NAS100 parents)
            for r in rd:
                try:
                    out.setdefault(("ConnorsRSI2", "NAS100", r.get("side", "LONG")), []).append(
                        (int(r["entry_ts"]), int(r["exit_ts"]),
                         float(r["entry_px"]), float(r["exit_px"]),
                         float(r.get("size") or 1.0)))
                except (KeyError, ValueError):
                    continue
            return out
        for r in rd:
            try:
                ets = int(r["entryTs"])
                xts = ets + int(float(r.get("hold_sec") or 0))
                epx = float(r["entryPrice"]); xpx = float(r["exitPrice"])
                eng = fam_of(r["engine"]); sym = r["symbol"]; side = r.get("side", "LONG")
                sz = float(r.get("size") or 1.0)
            except (KeyError, ValueError):
                continue
            if eng not in INCLUDE_FAMS and not eng.startswith(("CalendarTom", "MondayRiskOn")):
                continue
            out.setdefault((eng, sym, side), []).append((ets, xts, epx, xpx, sz))
    return out

# S-2026-07-08c FIX: cost must use abs(entry). Sign-flipped SHORT parents have
# negative prices, so `- en*rt_bp` ADDED cost as a subsidy on every short clip
# (symptom: net@2x > net, impossible). LONG rows unaffected (en>0).
def flip(H1):
    ts, o, h, l, c = H1
    return (ts, [-x for x in o], [-x for x in l], [-x for x in h], [-x for x in c])

def main():
    groups = {}
    for path in sys.argv[1:]:
        for k, v in load_shadowbook(path).items():
            groups.setdefault(k, []).extend(v)
    h1cache = {}
    for (eng, sym, side), parents in sorted(groups.items()):
        parents.sort()
        s = SYM_MAP.get(sym, sym)
        if s not in RT_BP and sym not in RT_BP:
            print(f"\n=== {eng} {sym} {side}: {len(parents)} parents — no rt_bp/H1 for symbol, SKIPPED")
            continue
        if s not in h1cache:
            try:
                if s == "MGC":   # 30m closes (native book timeframe; not in TICK_SPECS)
                    ts, o, h, l, c = [], [], [], [], []
                    with open(os.path.expanduser("~/Tick/mgc_30m_hist.csv")) as mf:
                        next(mf)
                        for ln in mf:
                            p = ln.split(",")
                            if len(p) < 5: continue
                            ts.append(int(p[0])); o.append(float(p[1])); h.append(float(p[2]))
                            l.append(float(p[3])); c.append(float(p[4]))
                    H1, src = (ts, o, h, l, c), "mgc_30m_hist.csv (30m)"
                else:
                    H1, src = RW.find_data(s)
            except Exception as e:
                print(f"\n=== {eng} {sym}: H1 load failed ({e}) — SKIPPED"); continue
            h1cache[s] = (H1, src)
            print(f"\n### {s} H1: {len(H1[0])} bars ({src}) "
                  f"{time.strftime('%Y-%m-%d', time.gmtime(H1[0][0]))} .. "
                  f"{time.strftime('%Y-%m-%d', time.gmtime(H1[0][-1]))}")
        H1, _ = h1cache[s]
        rt = RT_BP.get(s, RT_BP.get(sym))
        ts0, ts1 = H1[0][0], H1[0][-1]
        cov = [p for p in parents if p[0] >= ts0 - 86400 and p[1] <= ts1 + 86400]
        print(f"\n=== {eng} {sym} {side} (rt {rt}bp): {len(parents)} parents, {len(cov)} in H1 coverage ===")
        if len(cov) < 3:
            print("    <3 parents in coverage -> UNTESTABLE"); continue
        simH1, simcov = H1, cov
        if side.upper().startswith("S"):
            # short parents: negate prices so the long-only sim arithmetic applies
            simH1 = flip(H1)
            simcov = [(a, b, -e, -x, sz) for (a, b, e, x, sz) in cov]
            print("    (SHORT parents -> sign-flipped sim; % thresholds approximate)")
        print(f"  {'arm%':>5s} {'gb%':>5s} {'ret':>3s} {'flr':>3s} | {'n':>4s} {'net_pt':>9s} {'PF':>5s} "
              f"{'WR%':>5s} {'bothH':>5s} {'H1':>8s} {'H2':>8s} {'bear22':>8s} {'exbest':>9s} {'net@2x':>9s}")
        for arm in ARMS:
            for gb in GBS:
                for ret in RETS:
                    for flr in FLOORS:
                        tr = sim_live(simcov, simH1, arm, gb, rt, ret, flr)
                        st = CM.stat(tr)
                        if st["n"] == 0: continue
                        st2 = CM.stat(sim_live(simcov, simH1, arm, gb, rt * 2, ret, flr))
                        best = max(p for _, p in tr)
                        exb = st["net"] - best if st["n"] >= 2 else 0.0
                        star = " <== validated cell" if (arm, gb, ret, flr) == (0.020, 0.0075, 0.02, "-") else ""
                        print(f"  {arm*100:5.2f} {gb*100:5.2f} {('Y' if ret else '-'):>3s} {flr:>3s} | "
                              f"{st['n']:4d} {st['net']:9.1f} {st['pf']:5.2f} {st['wr']:5.1f} "
                              f"{('YES' if st['bh'] else 'no'):>5s} {st['h1']:8.1f} {st['h2']:8.1f} "
                              f"{st['bear']:8.1f} {exb:9.1f} {st2['net']:9.1f}{star}")

if __name__ == "__main__":
    main()
