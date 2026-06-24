#!/usr/bin/env python3
# (S-2026-06-24) Test the COLD-CUT + close-GIVE-BACK on the ACTUAL live BigCapMomo
# trades, grounded in real 5m bars (Yahoo). Faithful method: baseline = the ledger's
# real net_pnl; each rule only changes the trades IT touches (computed from the bars),
# everything else = actual. So we measure the rule's true delta on the live losers --
# the data the mega-cap backtest didn't contain.
import urllib.request, json, csv

LED="/tmp/closes2.csv"
_C={}
def bars(sym):
    if sym in _C: return _C[sym]
    try:
        u=f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?interval=5m&range=1mo"
        r=urllib.request.Request(u,headers={"User-Agent":"Mozilla/5.0"})
        j=json.load(urllib.request.urlopen(r,timeout=20)); res=j["chart"]["result"][0]
        ts=res["timestamp"]; q=res["indicators"]["quote"][0]; out=[]
        for i in range(len(ts)):
            o,h,l,c=q["open"][i],q["high"][i],q["low"][i],q["close"][i]
            if None not in (o,h,l,c): out.append((ts[i],o,h,l,c))
        _C[sym]=out; return out
    except Exception: _C[sym]=[]; return []

def coldcut(r, cut_sec, green_pct):
    # returns new pnl IF the cold-cut fires on this trade, else None (untouched -> actual)
    sym=r['symbol']; e_ts=int(r['entry_ts_unix']); entry=float(r['entry_px']); size=float(r['size'])
    bb=bars(sym)
    if not bb: return None
    thr=entry*(1+green_pct/100.0); peak=entry
    for ts,o,h,l,c in bb:
        if ts<e_ts: continue
        peak=max(peak,h)
        if ts-e_ts>=cut_sec:
            if peak<thr:                       # never went green by cut time -> CUT here
                return (c-entry)*size
            return None                        # went green before cut -> rule doesn't fire
    return None

def giveback(r, frac):
    # bank on a CLOSE that retraces frac of peak gain; only fires if it went green
    sym=r['symbol']; e_ts=int(r['entry_ts_unix']); entry=float(r['entry_px']); size=float(r['size'])
    bb=bars(sym)
    if not bb: return None
    peak=entry; fired=False
    for ts,o,h,l,c in bb:
        if ts<e_ts: continue
        peak=max(peak,h); gain=peak-entry
        if gain>0 and (peak-c)>=frac*gain:     # close retraced frac of peak gain
            return (c-entry)*size
    return None

def main():
    rows=[r for r in csv.DictReader(open(LED)) if 'BigCap' in r['engine']]
    actual=sum(float(r['net_pnl']) for r in rows)
    print(f"{len(rows)} real BigCapMomo trades | ACTUAL net ${actual:+.0f}\n")
    print("COLD-CUT (cut never-green trades; green=peak>+0.5%):")
    for cs in (1800,2700,3600,5400):
        tot=touched=0.0; nt=0; saved=0.0
        for r in rows:
            base=float(r['net_pnl']); alt=coldcut(r,cs,0.5)
            if alt is None: tot+=base
            else: tot+=alt; nt+=1; saved+=(alt-base)
        print(f"  cut@{cs//60:>2}min: new net ${tot:+6.0f}  (vs ${actual:+.0f}; {nt} trades cut, delta ${saved:+.0f})")
    print("\nCLOSE GIVE-BACK (bank winners that fade):")
    for f in (0.33,0.4,0.5):
        tot=0.0; nt=0; saved=0.0
        for r in rows:
            base=float(r['net_pnl']); alt=giveback(r,f)
            if alt is None: tot+=base
            else: tot+=alt; nt+=1; saved+=(alt-base)
        print(f"  retrace {f}: new net ${tot:+6.0f}  (vs ${actual:+.0f}; {nt} touched, delta ${saved:+.0f})")
    print("\nCOMBINED (cold-cut 45min + give-back 0.4):")
    tot=0.0
    for r in rows:
        base=float(r['net_pnl']); a1=coldcut(r,2700,0.5); a2=giveback(r,0.4)
        # apply whichever fires first conceptually: take the worse-case (the rule that triggers)
        cands=[x for x in (a1,a2) if x is not None]
        tot += min(cands) if cands else base   # min = conservative (earlier/ tighter exit)
    print(f"  new net ${tot:+.0f}  (vs ${actual:+.0f})")
if __name__=="__main__": main()
