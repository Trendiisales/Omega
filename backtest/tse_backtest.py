"""TSE Backtest v4 -- drift acceleration gate"""
import sys, csv
from collections import deque

TSE_MAX_LOT=0.01; TSE_COOLDOWN_MS=15000; TSE_PAUSE_MS=300000
TSE_MAX_SPREAD=0.50; TSE_COMMISSION_RT=0.20; TSE_ATR_MIN=1.0; TSE_ATR_MAX=8.0
TSE_P1_TICKS=8; TSE_P1_MIN_MOVE=0.50; TSE_P1_TP_MULT=2.5; TSE_P1_SL_MULT=1.0
TSE_BE_FRAC=0.50; TSE_RSI_PERIOD=14; TSE_RSI_SLOPE_ALPHA=2.0/6.0
TSE_RSI_SLOPE_THRESH=0.05; TSE_RSI_REVERSAL_THRESH=0.12
TSE_REVERSAL_MIN_HOLD_MS=3000; TSE_WARMUP_TICKS=60
TSE_P1_DRIFT_MIN=0.30; TSE_MAX_CONSEC_LOSSES=3

class TSE:
    def __init__(self):
        self.rg=deque(); self.rl=deque()
        self.rpm=0.0; self.rc=50.0; self.rpv=50.0; self.rse=0.0; self.rw=False
        self.bh=deque(); self.vb=0.0; self.tw=0; self.wms=0; self.tt=0
        self.pa=False; self.pl=False; self.pe=0.0
        self.pts=0; self.pm=0.0; self.psl=0.0; self.ptp=0.0; self.pbe=False
        self.dpnl=0.0; self.lem=0; self.pum=0; self.cl=0
        self.prev_drift=0.0
        self.trades=[]; self.blk={k:0 for k in ['atr','cool','rsi','warm','sprd','nb','cost','drift','accel']}

    def ursi(self,bid):
        if self.rpm==0.0: self.rpm=bid; return
        c=bid-self.rpm; self.rpm=bid
        self.rg.append(c if c>0 else 0.0); self.rl.append(-c if c<0 else 0.0)
        if len(self.rg)>TSE_RSI_PERIOD: self.rg.popleft(); self.rl.popleft()
        if len(self.rg)>=TSE_RSI_PERIOD:
            ag=sum(self.rg)/TSE_RSI_PERIOD; al=sum(self.rl)/TSE_RSI_PERIOD
            self.rpv=self.rc
            self.rc=100.0 if al==0 else 100.0-100.0/(1.0+ag/al)
            s=max(-5.0,min(5.0,self.rc-self.rpv))
            if not self.rw: self.rse=s; self.rw=True
            else: self.rse=s*TSE_RSI_SLOPE_ALPHA+self.rse*(1.0-TSE_RSI_SLOPE_ALPHA)

    def uvel(self,ms):
        if self.wms==0: self.wms=ms
        self.tw+=1
        if ms-self.wms>=30000:
            self.vb=self.vb*0.7+self.tw*0.3; self.tw=0; self.wms=ms

    def tick(self,ms,bid,ask,atr,drift):
        self.prev_drift=drift  # store BEFORE this tick's use
        self.tt+=1; sp=ask-bid
        self.bh.append(bid)
        if len(self.bh)>60: self.bh.popleft()
        self.ursi(bid); self.uvel(ms)
        mid=(bid+ask)*0.5
        if self.pa:
            mv=(mid-self.pe) if self.pl else (self.pe-mid)
            if mv>self.pm: self.pm=mv
            eff=bid if self.pl else ask
            td=abs(self.ptp-self.pe)
            if not self.pbe and td>0 and self.pm>=td*TSE_BE_FRAC:
                self.psl=self.pe; self.pbe=True
            held=ms-self.pts
            ag=(self.pl and self.rse<-TSE_RSI_REVERSAL_THRESH) or \
               (not self.pl and self.rse>TSE_RSI_REVERSAL_THRESH)
            if self.rw and held>=TSE_REVERSAL_MIN_HOLD_MS and self.pm<0.05 and ag:
                self.close(eff,'RSI_REV',ms); return
            if(self.pl and bid>=self.ptp)or(not self.pl and ask<=self.ptp):
                self.close(eff,'TP',ms); return
            if(self.pl and bid<=self.psl)or(not self.pl and ask>=self.psl):
                self.close(eff,'BE' if self.pbe else 'SL',ms); return
            if ms-self.pts>300000: self.close(eff,'TO',ms); return
            return
        if sp>TSE_MAX_SPREAD: self.blk['sprd']+=1; return
        if atr<TSE_ATR_MIN or atr>TSE_ATR_MAX: self.blk['atr']+=1; return
        if not(self.vb>=10 and self.rw and self.tt>=TSE_WARMUP_TICKS):
            self.blk['warm']+=1; return
        if ms<self.pum: return
        if ms-self.lem<TSE_COOLDOWN_MS: self.blk['cool']+=1; return
        if len(self.bh)<TSE_P1_TICKS+1: return
        b=list(self.bh); n=len(b); up=dn=0
        for i in range(n-TSE_P1_TICKS,n):
            d=b[i]-b[i-1]
            if d>0: up+=1
            elif d<0: dn+=1
        bu=(up>=TSE_P1_TICKS-1 and dn==0); bd=(dn>=TSE_P1_TICKS-1 and up==0)
        if not bu and not bd: self.blk['nb']+=1; return
        if self.rw:
            if bu and self.rse<-TSE_RSI_SLOPE_THRESH: self.blk['rsi']+=1; return
            if bd and self.rse>TSE_RSI_SLOPE_THRESH: self.blk['rsi']+=1; return
        # Drift agrees
        agrees=bu and drift>=TSE_P1_DRIFT_MIN or bd and drift<=-TSE_P1_DRIFT_MIN
        if not agrees: self.blk['drift']+=1; return
        # Drift accelerating (building not fading)
        accel=bu and drift>=self.prev_drift or bd and drift<=self.prev_drift
        if not accel: self.blk['accel']+=1; return
        net=abs(b[n-1]-b[n-1-TSE_P1_TICKS])
        if net<TSE_P1_MIN_MOVE: return
        if net*TSE_P1_TP_MULT<=sp+TSE_COMMISSION_RT: self.blk['cost']+=1; return
        sl=net*TSE_P1_SL_MULT; tp=net*TSE_P1_TP_MULT
        self.pa=True; self.pl=bu
        self.pe=ask if self.pl else bid
        self.pts=ms; self.pm=0.0; self.pbe=False
        self.psl=self.pe-sl if self.pl else self.pe+sl
        self.ptp=self.pe+tp if self.pl else self.pe-tp

    def close(self,ep,reason,ms):
        pp=(ep-self.pe) if self.pl else (self.pe-ep)
        pu=pp*TSE_MAX_LOT*100.0
        self.dpnl+=pu; self.lem=ms
        if pu>0: self.cl=0
        else:
            self.cl+=1
            if self.cl>=TSE_MAX_CONSEC_LOSSES: self.pum=ms+TSE_PAUSE_MS; self.cl=0
        from datetime import datetime,timezone
        ts=datetime.fromtimestamp(ms/1000,tz=timezone.utc).strftime('%H:%M:%S')
        self.trades.append({'t':ts,'s':'L' if self.pl else 'S',
            'e':self.pe,'x':ep,'r':reason,'p':pu,'m':self.pm,'h':(ms-self.pts)//1000})
        self.pa=False

if len(sys.argv)<2: print("Usage: python tse_bt_v4.py <csv>"); sys.exit(1)
tse=TSE(); tc=0; atr_w=deque(); pb=0.0; av=2.0
with open(sys.argv[1],newline='') as f:
    rd=csv.DictReader(f)
    for row in rd:
        try:
            ms=int(float(row.get('ts_ms',row.get('timestamp_ms',0))))
            bid=float(row['bid']); ask=float(row['ask'])
            drift=float(row.get('ewm_drift',0.0))
            atr_pts=float(row.get('atr',0.0))
        except: continue
        if bid<=0 or ask<=0 or ask<bid: continue
        if atr_pts<=0:
            if pb>0:
                tr=ask-bid+abs(bid-pb); atr_w.append(tr)
                if len(atr_w)>200: atr_w.popleft()
                if len(atr_w)>=50: av=sum(atr_w)/len(atr_w)*14
            atr_pts=av
        pb=bid; tse.tick(ms,bid,ask,atr_pts,drift); tc+=1

if tse.pa: tse.close(tse.pe,'END',tc*250)
print(f"\n{'='*55}")
print(f"TSE BT v4  drift>={TSE_P1_DRIFT_MIN}+accel SL_MULT={TSE_P1_SL_MULT}")
print(f"Ticks:{tc:,}  Trades:{len(tse.trades)}")
if tse.trades:
    ws=[t for t in tse.trades if t['p']>0]
    ls=[t for t in tse.trades if t['p']<=0]
    tot=sum(t['p'] for t in tse.trades)
    print(f"WR:{100*len(ws)/len(tse.trades):.1f}% ({len(ws)}W/{len(ls)}L)")
    print(f"PnL:${tot:+.2f}  Exp:${tot/len(tse.trades):+.3f}/trade")
    if ws: print(f"AvgW:${sum(t['p'] for t in ws)/len(ws):+.2f}")
    if ls: print(f"AvgL:${sum(t['p'] for t in ls)/len(ls):+.2f}")
    from collections import Counter
    print("\nReasons:")
    for r,c in Counter(t['r'] for t in tse.trades).most_common():
        p=sum(t['p'] for t in tse.trades if t['r']==r)
        print(f"  {r:10s} {c:3d}  ${p:+.2f}")
    print("\nBlocks:")
    for k,v in sorted(tse.blk.items(),key=lambda x:-x[1]):
        if v: print(f"  {k:8s} {v:6,d}")
    print(f"\n  {'T':8s} {'S':2s} {'Entry':8s} {'Exit':8s} {'R':8s} {'PnL':7s} {'MFE':6s} {'H'}")
    for t in tse.trades:
        print(f"  {t['t']:8s} {t['s']:2s} {t['e']:8.2f} {t['x']:8.2f} {t['r']:8s} ${t['p']:+5.2f} {t['m']:6.3f} {t['h']}s")
else:
    print("NO TRADES")
    for k,v in sorted(tse.blk.items(),key=lambda x:-x[1]):
        if v: print(f"  {k}:{v:,}")
