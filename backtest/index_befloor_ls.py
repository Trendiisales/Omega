#!/usr/bin/env python3
"""Index per-symbol Pos/Neg BE-floor companion — VIABILITY RESEARCH (faithful port of the gold
mechanism in backtest/gold_befloor_ls.py / fx_befloor_ls.py, adapted to index CFDs).

Flavors are named PER SYMBOL (US500Pos/US500Neg, NAS100Pos/NAS100Neg, …) — distinct from the
gold AUPOS/AUNEG and the FX <PAIR>Pos/<PAIR>Neg live books.

Same STANDALONE additive companion (feedback-companion-independent-engine): a 2h (W=2 H1) jump
detector arms an up-window (<SYM>Pos/LONG) or down-window (<SYM>Neg/SHORT); inside it a BE-floor
leg stays FLAT until price clears +be_bp from ref (covers RT cost), opens THERE, stop sits
at/above entry (long) or at/below entry (short) and trails only the favourable way => exit >=
entry (long) / <= entry (short) ALWAYS => net_bp >= 0 on EVERY clip BY CONSTRUCTION. neg MUST
be 0. Judge STANDALONE (net>0 both WF halves), NEVER vs a parent / vs riding WIDE.

Index adaptations vs FX: thr HIGHER (indices move more than FX majors in 2h; sweep 0.3-1.5%),
be_bp similar (index CFD RT spread ~1-2bp). Book in PRICE POINTS -> USD via per-symbol
point-value (US500.F $50/pt, NAS100 $1/pt, DJ30.F $5/pt, GER40 ~$1.10/pt) — the gold convention,
NOT FX percent-of-notional, because index point-values differ hugely.

  python3 index_befloor_ls.py [file.csv ...]   # default: the 4 clean index H1 warmups
"""
import csv, sys, os

W = 2                                        # detector window (H1 bars) = 2h
THRS = [0.003, 0.005, 0.007, 0.010, 0.015]   # 2h jump arm thresholds (index: wider than FX)
BE_BPS = [3.0, 6.0, 10.0]                     # RT cost floor to sweep (bp)
TIGHT = [20, 40]                             # banker giveback (bp)
WIDE  = [80, 150, 300]                       # runner giveback (bp)

# Per-symbol: real RT round-trip cost (bp) + point-value ($/pt/lot). Cost = one spread per clip
# (open+close). Index CFD spreads ~1-2bp; point-values from include/sizing.hpp tick_value_multiplier.
SYMS = {
    "US500":  dict(cost_bp=1.2, dpp=50.0,  live="US500.F"),
    "NAS100": dict(cost_bp=1.5, dpp=1.0,   live="NAS100"),
    "US30":   dict(cost_bp=1.5, dpp=5.0,   live="DJ30.F"),   # warmup file US30 -> live DJ30.F
    "DJ30":   dict(cost_bp=1.5, dpp=5.0,   live="DJ30.F"),
    "GER40":  dict(cost_bp=1.5, dpp=1.10,  live="GER40"),
}
_COST = 0.0   # bp, set per-file in run()


def load(path):
    ts=[];o=[];h=[];l=[];c=[]
    with open(path) as f:
        r=csv.reader(f)
        for row in r:
            if len(row)<5 or not row[0].lstrip("-").isdigit(): continue
            ts.append(int(row[0])); o.append(float(row[1]))
            h.append(float(row[2])); l.append(float(row[3])); c.append(float(row[4]))
    return ts,o,h,l,c,len(ts)


def parent(bars,W,thr,up):
    ts,o,h,l,c,N=bars; trades=[];pos=False;ent=None
    for i in range(W,N):
        j=c[i]/c[i-W]-1.0
        enter = (j>=thr) if up else (j<=-thr)
        exit_ = (j<=-thr) if up else (j>=thr)
        if not pos and enter:
            ei=i+1
            if ei>=N: continue
            pos=True; ent=(ei,o[ei])
        elif pos and exit_:
            xi=i+1
            if xi>=N: xi=N-1
            trades.append((ent[0],xi,ent[1])); pos=False;ent=None
    if pos: trades.append((ent[0],N-1,ent[1]))
    return trades


def leg_book(bars,trades,be_bp,gb_bp,is_long):
    """Returns list of (ts, net_bp) per clip. BE-floor -> net_bp >= 0 by construction."""
    ts,o,h,l,c,N=bars; rows=[]
    for ei,xi,epx in trades:
        ref=o[ei]; entry=None; wm=None
        for i in range(ei,xi):
            cur=c[i]
            if entry is None:
                cond = ((cur/ref-1.0)*1e4>=be_bp) if is_long else ((1.0-cur/ref)*1e4>=be_bp)
                if cond: entry=cur; wm=cur
                continue
            if is_long:
                if cur>wm: wm=cur
                stop=max(entry, wm*(1.0-gb_bp/1e4))
                if cur<=stop:
                    g=max(0.0,(stop/entry-1.0)*1e4); rows.append((ts[i],g))
                    ref=stop; entry=None; wm=None
            else:
                if cur<wm: wm=cur
                stop=min(entry, wm*(1.0+gb_bp/1e4))
                if cur>=stop:
                    g=max(0.0,(entry/stop-1.0)*1e4); rows.append((ts[i],g))
                    ref=stop; entry=None; wm=None
        if entry is not None:
            last=c[xi-1] if xi-1>=ei else o[ei]
            g = (last/entry-1.0)*1e4 if is_long else (entry/last-1.0)*1e4
            rows.append((ts[xi],max(0.0,g)))
    return rows   # net_bp per clip


def x2(bars,trades,be,tg,wg,is_long):
    return leg_book(bars,trades,be,tg,is_long)+leg_book(bars,trades,be,wg,is_long)


def met(r):
    if not r: return None
    gross=sum(x[1] for x in r)                    # gross bp
    net=gross - len(r)*_COST                       # deduct real RT spread (bp) per clip
    return dict(n=len(r), gross=gross, net=net,
                neg=sum(1 for x in r if x[1]<-1e-9), wins=sum(1 for x in r if x[1]>1e-9))
def sl(r,lo): return met([x for x in r if x[0]>=lo])
def half_lo(r,hi): return met([x for x in r if x[0]<hi])


def sym_of(base):
    up=base.upper()
    for k in SYMS:
        if k in up: return k
    return None


def run(path):
    global _COST
    bars=load(path); N=bars[5]
    if N<300:
        print(f"  SKIP {os.path.basename(path)} — only {N} bars"); return
    base=os.path.basename(path)
    sym=sym_of(base)
    meta=SYMS.get(sym, dict(cost_bp=1.5, dpp=1.0, live=sym or "?"))
    _COST=meta["cost_bp"]; dpp=meta["dpp"]
    POS,NEG=f"{sym}Pos", f"{sym}Neg"
    half=(bars[0][0]+bars[0][-1])//2
    span_d=(bars[0][-1]-bars[0][0])/86400.0
    print(f"\n########## {base}  sym={sym}  live={meta['live']}  bars={N}  span={span_d:.0f}d  "
          f"W={W}h  RT_cost={_COST:.2f}bp/clip  dpp=${dpp:g}/pt/lot")
    print(f"  net = GROSS bp minus real RT spread per clip. Report ONLY cost-adjusted net (both WF halves must be +).")
    print(f"{'thr%':>5s} {'be':>3s} {'flavor':>11s} {'BEST T/W':>9s} {'clips':>6s} {'neg':>4s} {'wins':>5s} "
          f"{'GROSSbp':>8s} {'NETbp':>8s} {'WF-H1':>7s} {'WF-H2':>7s} {'~$/lot':>9s}")
    for thr in THRS:
        tl=parent(bars,W,thr,True); tsr=parent(bars,W,thr,False)
        for flav,trades,is_long in [(POS,tl,True),(NEG,tsr,False)]:
            for be in BE_BPS:
                best=None
                for tg in TIGHT:
                    for wg in WIDE:
                        r=x2(bars,trades,be,tg,wg,is_long); m=met(r)
                        if not m: continue
                        cand=(tg,wg,m,half_lo(r,half),sl(r,half))
                        if best is None or m["net"]>best[2]["net"]: best=cand
                if not best: continue
                tg,wg,m,h1,h2=best
                h1n=f"{h1['net']:+7.1f}" if h1 else "    n/a"
                h2n=f"{h2['net']:+7.1f}" if h2 else "    n/a"
                # net bp -> $ per lot: net_bp/1e4 * price * dpp. Use median price as reference.
                med_px=sorted(bars[4])[N//2]
                usd=(m["net"]/1e4)*med_px*dpp
                print(f"{thr*100:5.2f} {be:3.0f} {flav:>11s} {tg:>4d}/{wg:<4d} {m['n']:6d} {m['neg']:4d} "
                      f"{m['wins']:5d} {m['gross']:+8.1f} {m['net']:+8.1f} {h1n} {h2n} {usd:+9.0f}")


def main():
    args=sys.argv[1:]
    if not args:
        D="phase1/signal_discovery"
        cands=[f"{D}/warmup_US500_H1.csv", f"{D}/warmup_NAS100_H1_clean.csv",
               f"{D}/warmup_US30_H1.csv", f"{D}/warmup_GER40_H1.csv"]
        args=[p for p in cands if os.path.exists(p)]
    print("INDEX BE-FLOOR per-symbol Pos/Neg companion — STANDALONE additive, neg MUST=0 (BE-floor by construction).")
    print("Book in PRICE POINTS -> USD via per-symbol point-value. Best T/W tier shown per (thr,be,flavor).")
    for p in args: run(p)


if __name__=="__main__": main()
