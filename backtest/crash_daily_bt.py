import datetime, statistics
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
COST_BP=6  # ~spread+slip+comm round trip in bp, conservative index CFD
def test(thr, hold, mode):
    print(f"--- crash-day short: day_ret<={-thr}% mode={mode} hold={hold}d cost={COST_BP}bp ---")
    for sym,f in FILES.items():
        rows=load(f"{TICK}/{f}")
        pnl=[]; byyear={}
        i=1
        while i<len(rows)-hold:
            d,o,h,l,c=rows[i]; pc=rows[i-1][4]
            ret=100*(c/pc-1)
            sma200=None
            if i>=200: sma200=sum(r[4] for r in rows[i-200:i])/200
            cond = ret<=-thr
            if mode=="fromhigh" and sma200: cond = cond and c>sma200*0.97  # crash FROM strength
            if cond:
                entry=c; exitp=rows[i+hold][4]
                p=10000*(entry-exitp)/entry - COST_BP  # bp
                pnl.append(p); y=d.year; byyear.setdefault(y,[]).append(p)
                i+=hold
            i+=1
        if pnl:
            n=len(pnl); tot=sum(pnl); wr=100*sum(1 for x in pnl if x>0)/n
            h1=pnl[:n//2]; h2=pnl[n//2:]
            print(f"  {sym:7s} n={n:3d} tot={tot:+7.0f}bp mean={tot/n:+6.1f}bp WR={wr:.0f}% h1={sum(h1):+6.0f} h2={sum(h2):+6.0f} "
                  +" ".join(f"{y}:{sum(v):+4.0f}" for y,v in sorted(byyear.items()) if abs(sum(v))>80))
for thr in (2.0,2.5,3.0):
    for hold in (1,2):
        test(thr,hold,"all")
test(2.0,1,"fromhigh"); test(2.5,1,"fromhigh")
