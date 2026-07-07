import datetime
TICK="/Users/jo/Tick"
def load(p):
    rows=[]
    for ln in open(p):
        x=ln.strip().split(',')
        if len(x)<5: continue
        try: ts=int(x[0]); o,h,l,c=map(float,x[1:5])
        except: continue
        rows.append((datetime.datetime.utcfromtimestamp(ts).date(),o,h,l,c))
    return rows
FILES={"NAS100":"NDX_daily_2016_2026.csv","US500":"SPX_daily_2016_2026.csv",
       "DJ30":"DJ30_daily_2016_2026.csv","GER40":"GER40_daily_2016_2026.csv"}
COST=12  # bp round trip, conservative CFD
def test(thr,hold,botfrac,label):
    print(f"--- CAPITULATION LONG: ret<={-thr}%{' & close in bottom '+str(int(botfrac*100))+'%' if botfrac<1 else ''} hold={hold}d ---")
    agg=0
    for sym,f in FILES.items():
        rows=load(f"{TICK}/{f}"); pnl=[]; byyear={}; worst=0; eq=0; peak=0; mdd=0
        i=1
        while i<len(rows)-hold:
            d,o,h,l,c=rows[i]; pc=rows[i-1][4]
            ret=100*(c/pc-1)
            ibs=(c-l)/(h-l) if h>l else .5
            if ret<=-thr and ibs<=botfrac:
                p=10000*(rows[i+hold][4]/c-1)-COST
                pnl.append(p); byyear.setdefault(d.year,[]).append(p)
                worst=min(worst,p); eq+=p; peak=max(peak,eq); mdd=max(mdd,peak-eq)
                i+=hold
            i+=1
        if not pnl: continue
        n=len(pnl); tot=sum(pnl); wr=100*sum(1 for x in pnl if x>0)/n
        h1=sum(pnl[:n//2]); h2=sum(pnl[n//2:]); agg+=tot
        neg_years=[y for y,v in byyear.items() if sum(v)<-100]
        print(f"  {sym:7s} n={n:3d} tot={tot:+7.0f} mean={tot/n:+6.1f} WR={wr:.0f}% h1={h1:+6.0f} h2={h2:+6.0f} worst={worst:+5.0f} maxDD={mdd:.0f} negYrs={neg_years}")
    print(f"  BOOK tot={agg:+.0f}bp")
for thr,hold,bot in ((2.0,1,1.0),(2.0,1,0.5),(2.5,1,1.0),(2.5,1,0.5),(3.0,1,1.0),(2.0,2,0.5),(2.5,2,1.0)):
    test(thr,hold,bot,"")
