#!/usr/bin/env python3
# XAU/XAG ratio mean-reversion -- same structure as the proven EURGBP cross-reversion
# (the one FX edge that survived). Two macro-correlated metals; the ratio has no drift
# and mean-reverts. Trade the ratio: z-score fade of log(gold/silver), long+short.
# Faithful replication of FxCrossRevEngine logic (log-close z over rolling window).
# Gold D1 from /Users/jo/Tick/2yr_XAUUSD_daily.csv; silver D1 aggregated from
# duka_ticks/XAGUSD_*.csv. Cost = both legs' round-trip spread in ratio-return bps.
import csv,glob,datetime,sys
import numpy as np

def gold_d1():
    d={}
    for r in csv.reader(open('/Users/jo/Tick/2yr_XAUUSD_daily.csv')):
        if not r or not r[0][0].isdigit(): continue
        ts=int(r[0]); c=float(r[4]); day=ts//86400
        d[day]=c
    return d

def silver_d1():
    d={}; acc={}
    for f in sorted(glob.glob('/Users/jo/Omega/duka_ticks/XAGUSD_*.csv')):
        r=csv.reader(open(f)); next(r,None)
        for row in r:
            try:
                tms=int(row[0]); a=float(row[1]); b=float(row[2])
                if a<=0 or b<=0: continue
                day=(tms//1000)//86400; mid=(a+b)/2
                acc[day]=mid  # last mid of day = close
            except: pass
    return acc

def run(days,lr,w,zin,zout,zstop,hold,cost_bps):
    # faithful FxCrossRevEngine: z from CLOSED window before pushing today; manage then enter
    lp=[]; zprev=0.0
    pos=None # dict side,entry_idx,entry_lr,bars
    trades=[]
    cost=cost_bps/10000.0
    for i,(day,r) in enumerate(zip(days,lr)):
        have=False; z=0.0
        if len(lp)>=w:
            m=np.mean(lp[-w:]); sd=np.std(lp[-w:],ddof=1)
            if sd>0: z=(r-m)/sd; have=True
        if pos and have:
            pos['bars']+=1; ex=False
            if pos['long']:
                if z>=-zout: ex=True
                elif z<=-zstop: ex=True
            else:
                if z<=zout: ex=True
                elif z>=zstop: ex=True
            if not ex and pos['bars']>=hold: ex=True
            if ex:
                ret=(r-pos['entry_lr']) if pos['long'] else (pos['entry_lr']-r)  # log-ratio return
                trades.append((day,pos['long'],ret-cost))
                pos=None
        if pos is None and have:
            if z>zin: pos={'long':False,'entry_lr':r,'bars':0}
            elif z<-zin: pos={'long':True,'entry_lr':r,'bars':0}
        lp.append(r)
        if have: zprev=z
    return trades

def stats(tr):
    if not tr: return None
    p=np.array([x[2] for x in tr])
    gw=p[p>0].sum(); gl=-p[p<0].sum()
    return dict(n=len(p),wr=100*np.mean(p>0),pf=gw/gl if gl>0 else 99,net=p.sum())

if __name__=='__main__':
    g=gold_d1(); s=silver_d1()
    common=sorted(set(g)&set(s))
    days=[d for d in common]
    print(f'gold D1={len(g)} silver D1={len(s)} aligned={len(days)}')
    if len(days)<120:
        print('INSUFFICIENT silver data yet (<120 bars) -- waiting on download'); sys.exit()
    ratio=np.array([g[d]/s[d] for d in days])
    lr=np.log(ratio)
    span=(datetime.datetime.utcfromtimestamp(days[0]*86400),datetime.datetime.utcfromtimestamp(days[-1]*86400))
    print(f'span {span[0].date()} .. {span[1].date()}  ratio {ratio.min():.1f}-{ratio.max():.1f}')
    cost_bps=float(sys.argv[1]) if len(sys.argv)>1 else 6.0  # both legs round-trip ~ (0.9+1.3)*2 bps
    tmid=len(days)//2
    print(f'cost={cost_bps}bps round-trip (both legs)\n')
    print(f'{"w":>4}{"zin":>5}{"zout":>5}{"hold":>5} | {"n":>4}{"WR":>5}{"PF":>6}{"net_bps":>9} | {"h1pf":>6}{"h2pf":>6}{"verdict":>10}')
    best=[]
    for w in [40,60,90,120]:
        for zin in [1.5,2.0,2.5]:
            for zout in [0.0,0.3,0.5]:
                for hold in [10,20,40]:
                    tr=run(days,lr,w,zin,zout,3.5,hold,cost_bps)
                    st=stats(tr)
                    if not st or st['n']<20: continue
                    h1=stats([t for t in tr if days.index(t[0])<tmid]) if tr else None
                    h2=stats([t for t in tr if days.index(t[0])>=tmid]) if tr else None
                    h1pf=h1['pf'] if h1 else 0; h2pf=h2['pf'] if h2 else 0
                    rob = st['pf']>1.2 and h1 and h2 and h1['net']>0 and h2['net']>0 and st['n']>=30
                    best.append((st,w,zin,zout,hold,h1pf,h2pf,rob))
    best.sort(key=lambda x:-x[0]['net'])
    for st,w,zin,zout,hold,h1pf,h2pf,rob in best[:20]:
        print(f'{w:>4}{zin:>5.1f}{zout:>5.1f}{hold:>5} | {st["n"]:>4}{st["wr"]:>4.0f}%{st["pf"]:>6.2f}{st["net"]*10000:>9.0f} | {h1pf:>6.2f}{h2pf:>6.2f}{("ROBUST" if rob else "")>10}')
    nrob=sum(1 for x in best if x[7])
    print(f'\nROBUST configs (pf>1.2, both-halves net+, n>=30): {nrob} / {len(best)}')
