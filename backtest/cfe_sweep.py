"""CFE Fast Sweep -- focused params only"""
import sys, csv, itertools
from collections import deque
from datetime import datetime, timezone

class CFE:
    def __init__(self, p):
        self.p=p; self.rg=deque(); self.rl=deque()
        self.rpm=0.0; self.rc=50.0; self.rp=50.0; self.rt=0.0; self.rw=False
        self.ra=2.0/(p['rn']+1); self.rm=deque()
        self.dsd=0; self.dss=0
        self.pa=False; self.pl=False; self.pe=0.0
        self.psl=0.0; self.ptp=0.0; self.pts=0; self.pm=0.0
        self.pbe=False; self.ps=0.0
        self.cd=0; self.wt=0; self.trades=[]
        self.lcd=0; self.lcm=0; self.ab=False; self.lex=0.0; self.lld=0

    def ursi(self,mid):
        if self.rpm==0: self.rpm=mid; return
        c=mid-self.rpm; self.rpm=mid
        self.rg.append(c if c>0 else 0); self.rl.append(-c if c<0 else 0)
        n=self.p['rn']
        if len(self.rg)>n: self.rg.popleft(); self.rl.popleft()
        if len(self.rg)>=n:
            ag=sum(self.rg)/n; al=sum(self.rl)/n
            self.rp=self.rc
            self.rc=100 if al==0 else 100-100/(1+ag/al)
            s=self.rc-self.rp
            if not self.rw: self.rt=s; self.rw=True
            else: self.rt=s*self.ra+self.rt*(1-self.ra)

    def tick(self,ms,bid,ask,drift,atr):
        mid=(bid+ask)/2; sp=ask-bid
        self.wt+=1; self.ursi(mid)
        self.rm.append(mid)
        if len(self.rm)>10: self.rm.popleft()
        dt=self.p['st']
        if drift>=dt:
            if self.dsd!=1: self.dsd=1; self.dss=ms
        elif drift<=-dt:
            if self.dsd!=-1: self.dsd=-1; self.dss=ms
        else:
            self.dsd=0; self.dss=0
        dsms=(ms-self.dss) if self.dsd!=0 else 0

        if self.pa:
            mv=(mid-self.pe) if self.pl else (self.pe-mid)
            if mv>self.pm: self.pm=mv
            eff=bid if self.pl else ask
            td=abs(self.ptp-self.pe)
            if not self.pbe and td>0 and mv>=td*0.5:
                self.psl=self.pe; self.pbe=True
            if self.p['tr'] and mv>=self.p['ta']:
                tsl=(mid-self.p['td']) if self.pl else (mid+self.p['td'])
                if self.pl and tsl>self.psl: self.psl=tsl
                if not self.pl and tsl<self.psl: self.psl=tsl
            if(self.pl and bid>=self.ptp)or(not self.pl and ask<=self.ptp):
                self._cl(eff,'TP',ms); return
            if(self.pl and bid<=self.psl)or(not self.pl and ask>=self.psl):
                self._cl(eff,'BE' if self.pbe else 'SL',ms); return
            if ms-self.pts>self.p['mh']: self._cl(eff,'TO',ms); return
            return

        if self.wt<300 or sp>0.40 or atr<1.0: return
        if ms<self.cd: return
        if not self.rw: return
        # RSI dir
        t=self.p['rt']; mx=self.p['rm']
        if self.rt>t and self.rt<mx: rd=1
        elif self.rt<-t and self.rt>-mx: rd=-1
        else: return
        if rd==1 and drift<0: return
        if rd==-1 and drift>0: return
        if abs(drift)<self.p['dm']: return
        if dsms<self.p['sm']: return
        # price confirm
        if len(self.rm)>=3:
            n3=list(self.rm)[-3]; mv3=mid-n3
            if rd==1 and mv3<-atr*0.3: return
            if rd==-1 and mv3>atr*0.3: return
        # opp dir cooldown
        if self.lcd!=0 and ms-self.lcm<45000:
            if (rd!=self.lcd): return
        # adverse block
        if self.ab and self.lld!=0:
            dist=(self.lex-mid) if self.lld==1 else (mid-self.lex)
            same=(rd==1 and self.lld==1)or(rd==-1 and self.lld==-1)
            if same and dist>atr*0.4: return
            else: self.ab=False
        sl=atr*self.p['sl']; tp=sl*self.p['tp']
        il=rd==1; e=ask if il else bid
        self.pa=True; self.pl=il; self.pe=e
        self.psl=e-sl if il else e+sl
        self.ptp=e+tp if il else e-tp
        self.pts=ms; self.pm=0.0; self.pbe=False
        self.ps=min(0.10,max(0.01,round(10/(sl*100)/0.01)*0.01))

    def _cl(self,ep,r,ms):
        pp=(ep-self.pe) if self.pl else (self.pe-ep)
        pu=pp*self.ps*100
        self.lcd=1 if self.pl else -1; self.lcm=ms
        if pu<0:
            self.ab=True; self.lex=ep
            self.lld=1 if self.pl else -1
            self.cd=ms+self.p['cd']
        ts=datetime.fromtimestamp(ms/1000,tz=timezone.utc).strftime('%H:%M')
        self.trades.append({'t':ts,'s':'L' if self.pl else 'S','p':pu,'m':self.pm,'r':r,'h':(ms-self.pts)//1000})
        self.pa=False

def run(f,p):
    cfe=CFE(p); aw=deque(); pb=0; av=2
    with open(f,newline='') as fh:
        for row in csv.DictReader(fh):
            try:
                ms=int(float(row.get('ts_ms',0)))
                bid=float(row['bid']); ask=float(row['ask'])
                drift=float(row.get('ewm_drift',0))
                atr=float(row.get('atr',0))
            except: continue
            if bid<=0 or ask<bid: continue
            if atr<=0:
                if pb>0:
                    aw.append(ask-bid+abs(bid-pb))
                    if len(aw)>200: aw.popleft()
                    if len(aw)>=50: av=sum(aw)/len(aw)*14
                atr=av
            pb=bid; cfe.tick(ms,bid,ask,drift,atr)
    if cfe.pa: cfe._cl(cfe.pe,'END',0)
    return cfe.trades

if len(sys.argv)<2: sys.exit(1)

# Focused grid -- parameters most likely to catch London trend moves
grid = {
    'rn':  [20,30],           # RSI period
    'ra':  [None],            # computed from rn
    'rt':  [2.0,4.0],         # RSI threshold
    'rm':  [15.0],            # RSI max
    'dm':  [0.3,0.5,0.8],     # drift min
    'st':  [0.3,0.5],         # sustained threshold
    'sm':  [10000,20000,30000],# sustained ms
    'sl':  [0.4,0.6,0.8],     # SL = sl*ATR
    'tp':  [2.0,3.0,4.0],     # TP = tp*SL
    'cd':  [15000,30000],     # cooldown ms
    'mh':  [300000,600000],   # max hold ms
    'tr':  [False,True],      # trail
    'ta':  [2.0],             # trail arm pts
    'td':  [1.0],             # trail dist pts
}

keys=list(grid.keys())
combos=list(itertools.product(*grid.values()))
print(f"Testing {len(combos)} combinations...")

results=[]
for i,combo in enumerate(combos):
    p=dict(zip(keys,combo))
    p['rn']=p['rn']; p['ra']=2.0/(p['rn']+1)  # fix alpha
    trades=run(sys.argv[1],p)
    if len(trades)>=3:
        tot=sum(t['p'] for t in trades)
        ws=[t for t in trades if t['p']>0]
        wr=len(ws)/len(trades)
        lp=sum(t['p'] for t in trades if t['s']=='L')
        sp=sum(t['p'] for t in trades if t['s']=='S')
        ln=len([t for t in trades if t['s']=='L'])
        sn=len([t for t in trades if t['s']=='S'])
        exp=tot/len(trades)
        results.append((tot,wr,len(trades),exp,lp,sp,ln,sn,p,trades))
    if (i+1)%200==0:
        best=max((r[0] for r in results),default=0)
        print(f"  {i+1}/{len(combos)}  best=${best:+.2f}")

results.sort(key=lambda x:x[0],reverse=True)

print(f"\n{'='*75}")
print(f"TOP 20 CFE CONFIGURATIONS")
print(f"{'='*75}")
print(f"{'#':3} {'PnL':8} {'WR':6} {'N':4} {'Exp':7} {'L$':7} {'S$':7} {'dm':5} {'sm':5} {'sl':5} {'tp':5} {'rt':5} {'tr'}")
for rank,(tot,wr,n,exp,lp,sp,ln,sn,p,trades) in enumerate(results[:20],1):
    print(f"{rank:3d} ${tot:+7.2f} {100*wr:5.1f}% {n:4d} ${exp:+5.3f} "
          f"${lp:+6.2f} ${sp:+6.2f} "
          f"{p['dm']:5.1f} {p['sm']//1000:4d}s "
          f"{p['sl']:5.2f} {p['tp']:5.1f} {p['rt']:5.1f} {'Y' if p['tr'] else 'N'}")

if results:
    tot,wr,n,exp,lp,sp,ln,sn,p,trades=results[0]
    print(f"\n{'='*75}")
    print(f"BEST: PnL=${tot:+.2f} WR={100*wr:.1f}% ({n} trades, {ln}L {sn}S)")
    print(f"Params: drift>={p['dm']} sus>={p['sm']//1000}s SL={p['sl']}xATR TP={p['tp']}xSL RSI_t={p['rt']} trail={'Y' if p['tr'] else 'N'} hold={p['mh']//60000}min")
    print(f"LONG: {ln} trades ${lp:+.2f}   SHORT: {sn} trades ${sp:+.2f}")
    print(f"\n  {'Time':6} {'S':2} {'R':6} {'PnL':8} {'MFE':6} {'Held'}")
    for t in trades:
        print(f"  {t['t']:6} {t['s']:2} {t['r']:6} ${t['p']:+6.2f} {t['m']:6.3f} {t['h']}s")
