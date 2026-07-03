#!/usr/bin/env python3
"""DOLLAR-GAUGE trailing companion — backtest (S-2026-07-03, operator redesign).

Operator spec: "use a financial gauge not percentage. If price has moved more than
$X in the favourable direction we begin to ride the trend; hard-trail each companion
separately to the original trade so it does not affect the edge; exit when it turns."

So the trigger is ABSOLUTE PRICE DOLLARS, not %:
  * ARM_USD  : companion 'arms' (starts riding) once dir*(px-entry) >= ARM_USD.
  * TRAIL_USD: once armed, a HARD trailing stop — exit when price gives back TRAIL_USD
               from its peak favourable $ ($ turn). This is 'exit when it turns'.
  * Never armed / never turned -> rides to the parent's natural exit (same flip), untouched.

The companion is a SEPARATE INDEPENDENT additive book (never edits the parent; parent rides
its own exit). Judged STANDALONE all-6 (net>0, PF>1, both WF halves>0, both regimes>=0),
NEVER vs WIDE. See CompanionDominanceError.md / feedback-companion-independent-engine.

MULTIPLE COMPANIONS: each (ARM_USD, TRAIL_USD) pair is one independent companion. A ladder
= several rungs riding in parallel, each hard-trailed on its own. `ladder()` sums a chosen set.

P&L booked in instrument price-$ (gold = $/oz). cost_usd = cost_rt(frac) * entry.
Reuses standalone_clip_overlay.load_paths/halves for path IO; uses a LOCAL $-native metrics()
(S-2026-07-03) so net$/DD$/WF$/regime$ print as true $/oz — scl.metrics() ×100 was for fractional
returns and over-inflated the dollar columns. PF/MAR are ratios -> unchanged.

usage: dollar_trail_companion.py <path_csv> <LABEL>
"""
import importlib.util, sys, os

scl_spec = importlib.util.spec_from_file_location("scl",
    os.path.expanduser("~/IBKRCrypto/backtest/standalone_clip_overlay.py"))
scl = importlib.util.module_from_spec(scl_spec); scl_spec.loader.exec_module(scl)

ARM_GRID   = [5, 10, 15, 20, 30, 40, 50]     # $ favourable move to start riding
TRAIL_GRID = [5, 10, 15, 20, 30]             # $ hard trailing give-back to exit-on-turn

def metrics(rows):
    """$-NATIVE metrics (S-2026-07-03). scl.metrics() multiplies net/dd by 100 (it was written for
    FRACTIONAL per-unit returns -> % / pool-$). Here pnl is ALREADY in instrument $/oz, so that ×100
    over-inflates every dollar column. This mirrors scl.metrics() EXACTLY but drops the ×100, so
    net$/DD$/WF$/regime$ print as true $/oz. PF and MAR are ratios -> identical either way (kept as a
    cross-check that the only change is the dollar scale)."""
    if not rows: return dict(n=0,pf=0,net=0,dd=0,mar=0)
    rows=sorted(rows,key=lambda x:x[0]); pnl=[r[1] for r in rows]
    w=sum(p for p in pnl if p>0); l=sum(-p for p in pnl if p<0)
    pf=w/l if l>0 else float('inf')
    eq=pk=dd=0.0
    for p in pnl:
        eq+=p; pk=max(pk,eq); dd=min(dd,eq-pk)
    net=sum(pnl)                               # $/oz, NOT ×100
    return dict(n=len(pnl),pf=pf,net=net,dd=dd,mar=(net/abs(dd) if dd<0 else float('inf')))

def dollar_trail(t, arm_usd, trail_usd):
    """Return (exit_ms, pnl_usd) for one $-trailed companion on one parent trade."""
    d, ent, cost = t["dir"], t["ent"], t["cost"] * t["ent"]   # cost frac -> $
    peak = None                        # peak favourable $ (None until armed)
    last = t["path"][-1]
    for seq, ms, px in t["path"]:
        if seq == 0: continue
        fav = d * (px - ent)           # favourable $ move (gold $/oz)
        if peak is None:
            if fav >= arm_usd: peak = fav      # arm: start riding
            continue
        if fav > peak: peak = fav
        if fav <= peak - trail_usd:            # hard $ trail hit -> turn -> exit
            return ms, fav - cost
    # never turned (or never armed): ride to parent's natural flip
    return last[1], d * (last[2] - ent) - cost

def peak_fav_usd(t):
    d, ent = t["dir"], t["ent"]
    return max((d*(px-ent) for seq,ms,px in t["path"] if seq>0), default=0.0)

def verdict(reg):
    m  = metrics([(x[0],x[1]) for x in reg])
    bl = metrics([(x[0],x[1]) for x in reg if x[2]])
    br = metrics([(x[0],x[1]) for x in reg if not x[2]])
    h1,h2 = scl.halves([(x[0],x[1]) for x in reg]); m1,m2 = metrics(h1), metrics(h2)
    ok = (m["net"]>0 and m["pf"]>1 and m1["net"]>0 and m2["net"]>0 and bl["net"]>=0 and br["net"]>=0)
    return m, m1, m2, bl, br, ok

def run(csvf, label):
    trades = scl.load_paths(csvf)
    entries = [t["ent"] for t in trades]
    peaks = [peak_fav_usd(t) for t in trades]
    print(f"\n########## {label}  ({len(trades)} trades) ##########")
    print(f"ENTRY-PRICE range in sample: ${min(entries):.0f} .. ${max(entries):.0f} "
          f"(a fixed $ gauge = {100*10/min(entries):.2f}% at low, {100*10/max(entries):.2f}% at high, for $10)")
    ps = sorted(peaks)
    def q(p): return ps[min(len(ps)-1,int(p*len(ps)))]
    print(f"PEAK favourable-$ per trade: p25 ${q(.25):.1f}  p50 ${q(.5):.1f}  p75 ${q(.75):.1f}  "
          f"p90 ${q(.9):.1f}  max ${max(peaks):.1f}")
    print("  trades reaching each ARM_USD:")
    for a in ARM_GRID:
        n = sum(1 for p in peaks if p>=a)
        print(f"     arm ${a:>3}  ->  {n:3d}/{len(trades)} ({100*n/len(trades):3.0f}%)")

    print("\nSWEEP  arm$ x trail$   (net$/PF/maxDD$/MAR/WF-H1$/WF-H2$/bull$/bear$ | nArm | V):")
    print(f"  {'arm$':>5s} {'trl$':>5s} {'net$':>8s} {'PF':>5s} {'DD$':>8s} {'MAR':>6s} "
          f"{'H1$':>7s} {'H2$':>7s} {'bull$':>7s} {'bear$':>7s} {'nArm':>5s}  V")
    passing = []
    for a in ARM_GRID:
        for tr in TRAIL_GRID:
            reg = [(*dollar_trail(t,a,tr), t["bull"]) for t in trades]
            m,m1,m2,bl,br,ok = verdict(reg)
            narm = sum(1 for p in peaks if p>=a)
            if ok: passing.append((a,tr,m,bl,br,narm))
            print(f"  {a:5d} {tr:5d} {m['net']:8.1f} {m['pf']:5.2f} {m['dd']:8.1f} {m['mar']:6.2f} "
                  f"{m1['net']:7.1f} {m2['net']:7.1f} {bl['net']:7.1f} {br['net']:7.1f} {narm:5d}  {'P' if ok else '.'}")
    if passing:
        passing.sort(key=lambda r:(-r[2]["mar"], -r[4]["net"], -r[2]["net"]))
        print("\nTOP-6 PASSING (rank MAR, bear$, net$):")
        for a,tr,m,bl,br,narm in passing[:6]:
            print(f"   arm ${a} trail ${tr}: net ${m['net']:.0f} PF {m['pf']:.2f} MAR {m['mar']:.2f} "
                  f"bear ${br['net']:.0f} nArm {narm}")
    else:
        print("\n  NO single $-trail config passes all-6.")
    return trades, passing

def aggr_trail(t, arm_usd, trail_usd, stag_ms):
    """AGGRESSIVE TF-INDEPENDENT trailer (S-2026-07-03 operator ask): arm fast, TIGHT tick-trail
    (reversal exit) on EVERY path point, PLUS wall-clock stagnation — exit if no NEW favourable
    peak within stag_ms. No bar-close / parent-TF dependence: pure tick trail + clock timeout.
    Runs live on the UNCHANGED executor via STALL_TF_HOURS=1 + STALL_BARS=hours (bar=now//TF_SEC
    is pure wall-clock; the 'TF' is the companion's own poll clock, not the parent engine's)."""
    d, ent, cost = t["dir"], t["ent"], t["cost"] * t["ent"]
    peak = None; peak_ms = None; last = t["path"][-1]
    for seq, ms, px in t["path"]:
        if seq == 0: continue
        fav = d * (px - ent)
        if peak is None:
            if fav >= arm_usd: peak = fav; peak_ms = ms
            continue
        if fav > peak: peak = fav; peak_ms = ms            # new extreme -> reset stagnation clock
        elif ms - peak_ms >= stag_ms: return ms, fav - cost # STAGNATION (wall clock) -> exit
        if fav <= peak - trail_usd: return ms, fav - cost   # REVERSAL (tight trail) -> exit
    return last[1], d * (last[2] - ent) - cost

def run_aggr(csvf, label, arms=(5,10,15), trails=(3,5,8,10), stag_hours=(2,4,8,12,24)):
    """Sweep the aggressive TF-independent trailer (arm x tight-trail x wall-clock stagnation)."""
    trades = scl.load_paths(csvf)
    print(f"\n######### AGGRESSIVE TF-INDEP TRAILER — {label} ({len(trades)} trades) #########")
    print(f"  {'arm$':>4} {'trl$':>4} {'stagH':>5} {'net$':>7} {'PF':>4} {'DD$':>7} {'MAR':>5} {'bull$':>6} {'bear$':>6}  V")
    passing = []
    for a in arms:
        for tr in trails:
            for sh in stag_hours:
                reg = [(*aggr_trail(t, a, tr, sh*3600*1000), t["bull"]) for t in trades]
                m,m1,m2,bl,br,ok = verdict(reg)
                if ok: passing.append((a,tr,sh,m,bl,br))
                print(f"  {a:4} {tr:4} {sh:5} {m['net']:7.0f} {m['pf']:4.2f} {m['dd']:7.0f} "
                      f"{m['mar']:5.2f} {bl['net']:6.0f} {br['net']:6.0f}  {'P' if ok else '.'}")
    passing.sort(key=lambda r:(-r[3]["mar"], -r[5]["net"]))
    print(f"  -> {len(passing)} pass all-6. TOP-4 (rank MAR, bear$):")
    for a,tr,sh,m,bl,br in passing[:4]:
        print(f"     arm${a} trail${tr} stag{sh}h: net${m['net']:.0f} PF{m['pf']:.2f} MAR{m['mar']:.2f} bear${br['net']:.0f}")
    return trades, passing

def ladder(trades, rungs, label):
    """Sum N independent companions (each its own arm$/trail$) into one additive book."""
    print(f"\nLADDER (multiple companions summed) — {label}: {rungs}")
    reg = []
    for a,tr in rungs:
        for t in trades:
            ms,pnl = dollar_trail(t,a,tr); reg.append((ms,pnl,t["bull"]))
    m,m1,m2,bl,br,ok = verdict(reg)
    print(f"   combined: net ${m['net']:.0f}  PF {m['pf']:.2f}  maxDD ${m['dd']:.0f}  MAR {m['mar']:.2f}  "
          f"WF ${m1['net']:.0f}/${m2['net']:.0f}  bull ${bl['net']:.0f}  bear ${br['net']:.0f}  "
          f"legs {len(reg)}  -> {'PASS' if ok else 'FAIL'}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: dollar_trail_companion.py <path_csv> <LABEL> [aggr]"); sys.exit(1)
    if len(sys.argv) > 3 and sys.argv[3] == "aggr":
        run_aggr(sys.argv[1], sys.argv[2])                 # aggressive TF-independent trailer sweep
    else:
        trades, passing = run(sys.argv[1], sys.argv[2])
        if len(passing) >= 3:
            ladder(trades, [(p[0],p[1]) for p in passing[:3]], sys.argv[2]+" top-3 ladder")
