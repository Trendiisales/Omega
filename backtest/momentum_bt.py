"""
MomentumScalp Backtest
Simple symmetric engine: RSI slope + drift = enter, trail out
Both LONG and SHORT
"""
import sys, csv, itertools
from collections import deque
from datetime import datetime, timezone

class MomentumEngine:
    def __init__(self, p):
        self.p = p
        # Tick RSI
        self.rg=deque(); self.rl=deque()
        self.rpm=0.0; self.rc=50.0; self.rp=50.0
        self.rs=0.0  # slope EMA
        self.rw=False; self.ra=2.0/(p['rsi_n']+1)
        # State
        self.ticks=0
        self.pa=False; self.pl=False
        self.pe=0.0; self.psl=0.0; self.ptp=0.0
        self.pts=0; self.pm=0.0; self.pbe=False
        self.ps=0.01
        self.cd_until=0
        self.trades=[]
        self.prev_drift=0.0

    def ursi(self, bid):
        if self.rpm==0: self.rpm=bid; return
        c=bid-self.rpm; self.rpm=bid
        self.rg.append(c if c>0 else 0)
        self.rl.append(-c if c<0 else 0)
        n=self.p['rsi_n']
        if len(self.rg)>n: self.rg.popleft(); self.rl.popleft()
        if len(self.rg)>=n:
            ag=sum(self.rg)/n; al=sum(self.rl)/n
            self.rp=self.rc
            self.rc=100 if al==0 else 100-100/(1+ag/al)
            slope=max(-5.0,min(5.0,self.rc-self.rp))
            if not self.rw: self.rs=slope; self.rw=True
            else: self.rs=slope*self.ra+self.rs*(1-self.ra)

    def tick(self, ms, bid, ask, drift, atr):
        self.ticks+=1
        sp=ask-bid
        self.ursi(bid)
        mid=(bid+ask)/2

        # Manage open position
        if self.pa:
            mv=(mid-self.pe) if self.pl else (self.pe-mid)
            if mv>self.pm: self.pm=mv
            eff=bid if self.pl else ask

            # BE at 50% TP
            td=abs(self.ptp-self.pe)
            if not self.pbe and td>0 and mv>=td*0.5:
                self.psl=self.pe; self.pbe=True

            # Trail after trail_arm
            if mv>=self.p['trail_arm']:
                tsl=(mid-self.p['trail_dist']) if self.pl else (mid+self.p['trail_dist'])
                if self.pl and tsl>self.psl: self.psl=tsl
                if not self.pl and tsl<self.psl: self.psl=tsl

            # TP
            if (self.pl and bid>=self.ptp) or (not self.pl and ask<=self.ptp):
                self._close(eff,'TP',ms); return
            # SL
            if (self.pl and bid<=self.psl) or (not self.pl and ask>=self.psl):
                self._close(eff,'BE' if self.pbe else 'SL',ms); return
            # Timeout
            if ms-self.pts>self.p['max_hold_ms']:
                self._close(eff,'TO',ms); return

            self.prev_drift=drift
            return

        # Entry guards
        if self.ticks < 300: self.prev_drift=drift; return  # warmup
        if sp > 0.40: self.prev_drift=drift; return
        if atr < 1.0: self.prev_drift=drift; return
        if ms < self.cd_until: self.prev_drift=drift; return
        if not self.rw: self.prev_drift=drift; return

        # === ENTRY LOGIC -- SIMPLE AND SYMMETRIC ===
        # RSI slope must be clearly trending
        rsi_long  = self.rs >  self.p['rsi_thresh']
        rsi_short = self.rs < -self.p['rsi_thresh']

        # Drift must agree with RSI
        drift_long  = drift >=  self.p['drift_min']
        drift_short = drift <= -self.p['drift_min']

        # Signal
        go_long  = rsi_long  and drift_long
        go_short = rsi_short and drift_short

        if not go_long and not go_short:
            self.prev_drift=drift; return

        # Drift must not be fading (acceleration check)
        if go_long  and drift < self.prev_drift - 0.1:
            self.prev_drift=drift; return
        if go_short and drift > self.prev_drift + 0.1:
            self.prev_drift=drift; return

        # Enter
        is_long = go_long
        sl = self.p['sl_pts']
        tp = sl * self.p['tp_rr']
        entry = ask if is_long else bid
        self.pa=True; self.pl=is_long
        self.pe=entry
        self.psl=entry-sl if is_long else entry+sl
        self.ptp=entry+tp if is_long else entry-tp
        self.pts=ms; self.pm=0.0; self.pbe=False
        self.ps=min(0.10, max(0.01, round(self.p['risk']/(sl*100)/0.01)*0.01))
        self.prev_drift=drift

    def _close(self, ep, reason, ms):
        pp=(ep-self.pe) if self.pl else (self.pe-ep)
        pu=pp*self.ps*100
        if pu<0: self.cd_until=ms+self.p['cooldown_ms']
        ts=datetime.fromtimestamp(ms/1000,tz=timezone.utc).strftime('%H:%M:%S')
        self.trades.append({
            't':ts,'s':'L' if self.pl else 'S',
            'p':pu,'m':self.pm,'r':reason,
            'h':(ms-self.pts)//1000,
            'e':self.pe,'x':ep
        })
        self.pa=False

def run(f, p):
    eng=MomentumEngine(p)
    aw=deque(); pb=0; av=2.0
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
            pb=bid
            eng.tick(ms,bid,ask,drift,atr)
    if eng.pa: eng._close(eng.pe,'END',0)
    return eng.trades

def score(trades):
    if len(trades)<3: return None
    tot=sum(t['p'] for t in trades)
    ws=[t for t in trades if t['p']>0]
    wr=len(ws)/len(trades)
    lp=sum(t['p'] for t in trades if t['s']=='L')
    sp=sum(t['p'] for t in trades if t['s']=='S')
    ln=len([t for t in trades if t['s']=='L'])
    sn=len([t for t in trades if t['s']=='S'])
    return tot,wr,len(trades),tot/len(trades),lp,sp,ln,sn

if len(sys.argv)<2: print("Usage: python momentum_bt.py <csv>"); sys.exit(1)

# Grid -- focused on the key params
grid = {
    'rsi_n':      [10, 14, 20],
    'rsi_thresh': [0.3, 0.5, 0.8, 1.2],
    'drift_min':  [0.2, 0.3, 0.5],
    'sl_pts':     [0.6, 0.8, 1.0, 1.5],
    'tp_rr':      [2.0, 3.0, 4.0],
    'trail_arm':  [1.0, 1.5, 2.0],
    'trail_dist': [0.5, 0.8],
    'max_hold_ms':[180000, 300000, 600000],
    'cooldown_ms':[10000, 20000],
    'risk':       [10.0],
}

keys=list(grid.keys())
combos=list(itertools.product(*grid.values()))
print(f"Testing {len(combos):,} combinations on {sys.argv[1].split(chr(92))[-1]}...")

results=[]
for i,combo in enumerate(combos):
    p=dict(zip(keys,combo))
    trades=run(sys.argv[1],p)
    s=score(trades)
    if s: results.append((*s,p,trades))
    if (i+1)%500==0:
        best=max((r[0] for r in results),default=0)
        print(f"  {i+1:,}/{len(combos):,}  best=${best:+.2f}")

results.sort(key=lambda x:x[0],reverse=True)

print(f"\n{'='*80}")
print(f"MOMENTUM SWEEP -- TOP 20")
print(f"{'='*80}")
print(f"{'#':>3} {'PnL':>8} {'WR':>6} {'N':>4} {'Exp':>7} {'L$':>7} {'S$':>7} "
      f"{'rsi_t':>6} {'drft':>5} {'sl':>5} {'tp':>5} {'arm':>5} {'hold':>5}")
for rank,(tot,wr,n,exp,lp,sp,ln,sn,p,trades) in enumerate(results[:20],1):
    print(f"{rank:>3d} ${tot:>+7.2f} {100*wr:>5.1f}% {n:>4d} ${exp:>+5.3f} "
          f"${lp:>+6.2f} ${sp:>+6.2f} "
          f"{p['rsi_thresh']:>6.2f} {p['drift_min']:>5.2f} "
          f"{p['sl_pts']:>5.2f} {p['tp_rr']:>5.1f} "
          f"{p['trail_arm']:>5.1f} {p['max_hold_ms']//60000:>4d}m")

# Best config detail
if results:
    tot,wr,n,exp,lp,sp,ln,sn,p,trades=results[0]
    print(f"\n{'='*80}")
    print(f"BEST CONFIG: PnL=${tot:+.2f}  WR={100*wr:.1f}%  {n} trades ({ln}L / {sn}S)")
    print(f"rsi_n={p['rsi_n']} rsi_thresh={p['rsi_thresh']} drift_min={p['drift_min']}")
    print(f"sl={p['sl_pts']}pt tp={p['tp_rr']}xSL trail_arm={p['trail_arm']}pt trail_dist={p['trail_dist']}pt")
    print(f"hold={p['max_hold_ms']//60000}min cooldown={p['cooldown_ms']//1000}s")
    print(f"\nLONG:  {ln} trades ${lp:+.2f}")
    print(f"SHORT: {sn} trades ${sp:+.2f}")
    from collections import Counter
    reasons=Counter(t['r'] for t in trades)
    print(f"Exits: {dict(reasons)}")
    print(f"\n  {'Time':9} {'S':2} {'Entry':8} {'Exit':8} {'R':6} {'PnL':8} {'MFE':6} {'Held'}")
    for t in trades:
        print(f"  {t['t']:9} {t['s']:2} {t['e']:8.2f} {t['x']:8.2f} "
              f"{t['r']:6} ${t['p']:+6.2f} {t['m']:6.3f} {t['h']}s")
