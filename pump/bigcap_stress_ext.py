#!/usr/bin/env python3
"""bigcap_scalp_stress — stress the BigCapMomo edge vs SLIPPAGE + finer gate/trail.
Reuses the cached 5m pull (/tmp/bigcap_5m.pkl). The question: does gate5/trail3 hold
when fills are worse (15-30 bps/side, realistic for momentum-name market orders)?"""
import pickle, datetime, sys
CACHE="/tmp/bigcap_5m.pkl"
LB=6; VOLX=3.0; MAX_HOLD=48; MIN_PRICE=10.0; IG=3.0
def day_key(ts): return datetime.datetime.utcfromtimestamp(ts).date()

def sim(b, day_gate, trail, slip):
    n=len(b); trades=[]
    if n<30: return trades
    inpos=False; entry=peak=0.0; hold=0; cur_day=None; day_open=0.0
    for i in range(21,n):
        ts,o,h,l,c,v=b[i]
        dk=day_key(ts)
        if dk!=cur_day: cur_day=dk; day_open=o
        if inpos:
            hold+=1
            if h>peak: peak=h
            tstop=peak*(1-trail)
            if l<=tstop: trades.append(tstop*(1-slip)/entry-1); inpos=False; continue
            if hold>=MAX_HOLD: trades.append(c*(1-slip)/entry-1); inpos=False; continue
            continue
        if c<MIN_PRICE or day_open<=0: continue
        if (c/day_open-1)*100<day_gate: continue
        avgv=sum(b[k][5] for k in range(i-20,i))/20.0
        if avgv<=0 or v<VOLX*avgv: continue
        base=b[i-LB][4]
        if base<=0 or (c/base-1)*100<IG: continue
        entry=c*(1+slip); peak=h; hold=0; inpos=True
    return trades

def main():
    data=pickle.load(open(CACHE,"rb"))
    print(f"=== BigCapMomo SLIPPAGE STRESS ({len(data)} NAS/SPX, ~2-3mo 5m, IG>={IG}% vol>={VOLX}x) ===")
    for slip_bps in [8, 15, 20, 30]:
        slip=slip_bps/1e4
        print(f"\n-- slip {slip_bps} bps/side (round-trip ~{2*slip_bps}bps) --")
        print(f"{'gate%':>6} {'trail%':>7} {'n':>5} {'WR%':>6} {'avg%':>7} {'PF':>6} {'tot%':>9}")
        for dg in [3,4,5]:
            for tr in [0.040,0.050]:
                allr=[]
                for s,b in data.items(): allr+=sim(b,dg,tr,slip)
                if not allr: print(f"{dg:6} {tr*100:7.1f} {0:5}"); continue
                w=[r for r in allr if r>0]; gw=sum(w); gl=-sum(r for r in allr if r<=0)
                pf=gw/gl if gl>0 else (99 if gw>0 else 0)
                flag=" OK" if pf>=1.3 and len(allr)>=30 else (" thin" if pf>=1.3 else "")
                print(f"{dg:6} {tr*100:7.1f} {len(allr):5} {100*len(w)/len(allr):6.1f} {1e2*sum(allr)/len(allr):7.3f} {pf:6.2f} {1e2*sum(allr):9.1f}{flag}")
    print("\nOK = PF>=1.3 & n>=30 at that slippage. If gate5/trail3 dies by 15-20bps -> slippage-fragile.")

if __name__=="__main__": main()
