#!/usr/bin/env python3
"""
Luke-system faithful daily backtester.
Implements the 3 entry setups from the Martin Luke / Zero-to-Alpha video onto a
big-cap / high-ADR momentum universe, with tight-stop risk-based sizing and
flexible partial exits. Portfolio-level event loop, faithful next-bar fills,
real costs. Every behavior is a lever in the params dict.
"""
import os, glob, math, json, sys
import numpy as np, pandas as pd

DATA='/tmp/luke_data'

# ---------- load + indicators ----------
def ema(s,n): return s.ewm(span=n,adjust=False).mean()

def load_one(path):
    t=os.path.basename(path)[:-4]
    df=pd.read_csv(path)
    df['date']=pd.to_datetime(df['date'])
    df=df.sort_values('date').reset_index(drop=True)
    df=df[(df[['open','high','low','close']]>0).all(axis=1)].reset_index(drop=True)
    if len(df)<260: return None
    c,h,l=df['close'],df['high'],df['low']
    df['ema9']=ema(c,9); df['ema21']=ema(c,21); df['ema50']=ema(c,50)
    df['sma200']=c.rolling(200).mean()
    tr=pd.concat([h-l,(h-c.shift()).abs(),(l-c.shift()).abs()],axis=1).max(axis=1)
    df['atr14']=tr.rolling(14).mean()
    df['adr']=((h-l)/c*100).rolling(20).mean()        # avg daily range %
    df['hh20']=h.rolling(20).max()
    df['ll10']=l.rolling(10).min()
    df['ema21_slope']=df['ema21']-df['ema21'].shift(5)
    df['ema9_slope']=df['ema9']-df['ema9'].shift(3)
    df['ticker']=t
    return df

def load_universe():
    out={}
    for p in sorted(glob.glob(f'{DATA}/*.csv')):
        if os.path.basename(p).startswith('_'): continue
        d=load_one(p)
        if d is not None: out[d['ticker'].iloc[0]]=d
    return out

def load_regime(univ=None, sma=100):
    """QQQ close>SMA200 = bull, else bear. If no QQQ, build an equal-weight breadth
    index from the universe and use index>SMA(sma) as the market regime."""
    p=f'{DATA}/_QQQ.csv'
    if os.path.exists(p):
        q=pd.read_csv(p); q['date']=pd.to_datetime(q['date']); q=q.sort_values('date')
        q['sma200']=q['close'].rolling(200).mean()
        q['reg']=np.where(q['close']>q['sma200'],'bull','bear')
        return dict(zip(q['date'],q['reg']))
    if univ is None: return None
    dates=sorted(set().union(*[set(d['date']) for d in univ.values()]))
    acc={}
    for d in univ.values():
        s=d.set_index('date')['close']; s=s/s.iloc[0]
        for dt,v in s.items(): acc.setdefault(dt,[]).append(v)
    idx=pd.Series({dt:np.mean(acc[dt]) for dt in dates}).sort_index()
    smas=idx.rolling(sma).mean()
    reg=np.where(idx>smas,'bull','bear')
    return dict(zip(idx.index,reg))

# ---------- anchored vwap ----------
def add_avwap(df, lookback=40):
    """AVWAP anchored at the most recent swing low (lowest low in trailing window
    that is a local min). Recomputed forward from that anchor."""
    h,l,c,v=df['high'].values,df['low'].values,df['close'].values,df['volume'].values
    tp=(h+l+c)/3.0
    n=len(df); avwap=np.full(n,np.nan); anchor=0
    swing=np.full(n,False)
    for i in range(2,n-2):
        if l[i]==min(l[max(0,i-lookback):i+1]) and l[i]<l[i-1] and l[i]<l[i+1]:
            swing[i]=True
    cum_pv=0.0; cum_v=0.0
    for i in range(n):
        if swing[i]:
            anchor=i; cum_pv=tp[i]*v[i]; cum_v=v[i]
        else:
            cum_pv+=tp[i]*v[i]; cum_v+=v[i]
        avwap[i]=cum_pv/cum_v if cum_v>0 else np.nan
    df['avwap']=avwap
    return df

# ---------- signal generation ----------
def gen_signals(df, P):
    """Return list of dicts: decision bar index i (signal on close of i, act on i+1)."""
    sig=[]
    h,l,c=df['high'].values,df['low'].values,df['close'].values
    e9,e21,e50=df['ema9'].values,df['ema21'].values,df['ema50'].values
    sl21=df['ema21_slope'].values; adr=df['adr'].values
    hh20=df['hh20'].values; av=df['avwap'].values; atr=df['atr14'].values
    modes=P['entry_modes']
    tb=P['touch_buf']; adr_min=P['adr_min']
    for i in range(55,len(df)-1):
        if np.isnan(e50[i]) or np.isnan(adr[i]) or np.isnan(atr[i]): continue
        if adr[i]<adr_min: continue
        lead = e9[i]>e21[i]>e50[i]
        up   = sl21[i]>0
        cand=None
        # ---- A: pullback to rising 9/21 EMA ----
        if 'A' in modes and lead and up:
            near21 = l[i]<=e21[i]*(1+tb) and c[i]>e21[i]
            near9  = l[i]<=e9[i]*(1+tb)  and c[i]>e9[i]
            prior_strength = c[i] >= hh20[i]*(1-P['base_buf'])  # was near recent high
            if (near21 or near9) and prior_strength:
                trig=h[i]; stop=min(l[i], (e21[i] if near21 else e9[i]))*(1-P['stop_buf'])
                cand=('A',trig,stop)
        # ---- C: inside-day / micro-VCP breakout ----
        if cand is None and 'C' in modes and (e9[i]>e21[i]) and c[i]>e21[i] and up:
            inside1 = h[i]<h[i-1] and l[i]>l[i-1]
            inside2 = inside1 and h[i-1]<h[i-2] and l[i-1]>l[i-2]
            if inside1 or inside2:
                trig=h[i]; stop=l[i]*(1-P['stop_buf'])
                cand=('C',trig,stop)
        # ---- B: anchored-VWAP break+retest / cluster ----
        if cand is None and 'B' in modes and not np.isnan(av[i]) and up and c[i]>e50[i]:
            near_av = abs(l[i]-av[i])/c[i] < P['avwap_band'] and c[i]>av[i]
            if near_av:
                # cluster: count supports within band of the low
                lowpx=l[i]; band=P['cluster_band']*c[i]
                levels=[av[i], e21[i], e9[i], round(c[i])]  # avwap, emas, round number
                ncl=sum(1 for x in levels if abs(x-lowpx)<=band)
                if ncl>=P['cluster_min']:
                    trig=h[i]; stop=(min(av[i],lowpx))*(1-P['stop_buf'])
                    cand=('B',trig,stop)
        if cand is None: continue
        setup,trig,stop=cand
        if trig<=stop: continue
        stopw=(trig-stop)/trig
        if stopw< P['min_stopw'] or stopw> P['max_stopw']: continue
        sig.append(dict(i=i,date=df['date'].iloc[i],setup=setup,trig=trig,stop=stop,
                        adr=adr[i],atr=atr[i]))
    return sig

# ---------- portfolio simulation ----------
def simulate(univ, regime, P):
    # build global trading calendar
    alldates=sorted(set().union(*[set(d['date']) for d in univ.values()]))
    # index per ticker: date->row
    idx={t:{dt:k for k,dt in enumerate(d['date'])} for t,d in univ.items()}
    # precompute signals keyed by (ticker, decision_date) -> signal
    sigmap={}
    for t,d in univ.items():
        for s in gen_signals(d,P):
            sigmap.setdefault(s['date'],[]).append((t,s))
    # --- live-scanner reconstruction: TOP_PERC_GAIN rolling watchlist (+ ADR pre-filter) ---
    # Mirrors how the live engine picks names: IBKR scans the day's biggest % gainers;
    # a name enters the watchlist when it ignites (>= scanner_gate_pct that day AND in the
    # top-K gainers of the universe) and stays eligible for scanner_lookback trading days,
    # AND must clear the ADR floor (the new pre-filter). The Luke setup then fires within it.
    scanner_active={}   # ticker -> set(global date-index) eligible
    if P.get('use_scanner'):
        gate=P.get('scanner_gate_pct',4.0); L=P.get('scanner_lookback',10); K=P.get('scanner_topk',15)
        adrmin=P.get('adr_min',0.0)
        # per-date list of (ticker, day_return%, adr_at_day)
        for gi,dt in enumerate(alldates):
            rows=[]
            for t,d in univ.items():
                k=idx[t].get(dt)
                if k is None or k<1: continue
                pc=d['close'].iloc[k-1]
                if pc<=0: continue
                ret=(d['close'].iloc[k]-pc)/pc*100.0
                adrv=d['adr'].iloc[k]
                if not np.isnan(adrv) and adrv>=adrmin and ret>=gate:
                    rows.append((t,ret))
            rows.sort(key=lambda x:-x[1])
            for t,_ in rows[:K]:               # top-K gainers this day = scanner hits
                for gj in range(gi, min(len(alldates), gi+L+1)):
                    scanner_active.setdefault(t,set()).add(gj)
    eq=P['equity0']; cash=eq
    open_pos={}            # ticker -> position dict
    trades=[]; eq_curve=[]
    cost=P['cost_bps']/1e4; stopslip=P['stop_slip_bps']/1e4
    for di,dt in enumerate(alldates):
        # ---- 1. manage open positions on today's bar ----
        for t in list(open_pos.keys()):
            d=univ[t]; k=idx[t].get(dt)
            if k is None: continue
            o,hi,lo,cl=d['open'].iloc[k],d['high'].iloc[k],d['low'].iloc[k],d['close'].iloc[k]
            pos=open_pos[t]
            e9c=d['ema9'].iloc[k]
            exited=False
            # stop first (gap aware)
            if lo<=pos['stop']:
                fill=min(o,pos['stop']) if o<pos['stop'] else pos['stop']
                fill*= (1-stopslip)
                sh=pos['shares']
                cash+=sh*fill*(1-cost)
                pos['realized']+=sh*(fill-pos['entry']); pos['shares']=0
                trades.append(_finalize(pos)); del open_pos[t]; exited=True
            if exited: continue
            # partial targets (sell into strength at xADR extension)
            while pos['plevel']<P['max_partials'] and hi>=pos['next_target']:
                fr=P['partial_frac']; sh=int(pos['init_shares']*fr)
                sh=min(sh,pos['shares'])
                if sh<=0: break
                fill=max(o,pos['next_target'])  # if gapped above, better fill
                cash+=sh*fill*(1-cost)
                pos['shares']-=sh; pos['realized']+=sh*(fill-pos['entry'])
                pos['plevel']+=1
                pos['next_target']=pos['entry']*(1+ (P['partial_mult']+P['partial_step']*pos['plevel'])*pos['adr']/100)
                if pos['shares']<=0:
                    trades.append(_finalize(pos)); del open_pos[t]; exited=True; break
            if exited: continue
            # trail stop to breakeven / ema after first partial (lever)
            if P['be_after_partial'] and pos['plevel']>=1 and pos['stop']<pos['entry']:
                pos['stop']=pos['entry']
            if P['trail_ema'] and not np.isnan(e9c):
                pos['stop']=max(pos['stop'], e9c*(1-P['stop_buf']))
            # final exit: close below 9EMA
            if not np.isnan(e9c) and cl<e9c:
                # exit next open faithfully -> mark for exit; simpler: exit at close with slip
                fill=cl*(1-cost)
                cash+=pos['shares']*fill*(1-cost)
                pos['realized']+=pos['shares']*(fill-pos['entry'])
                trades.append(_finalize(pos)); del open_pos[t]; exited=True
            # time stop
            if not exited and P['time_stop'] and (di-pos['di_entry'])>=P['time_stop']:
                fill=cl
                cash+=pos['shares']*fill*(1-cost)
                pos['realized']+=pos['shares']*(fill-pos['entry'])
                trades.append(_finalize(pos)); del open_pos[t]
        # ---- 2. mark-to-market equity ----
        mtm=cash
        for t,pos in open_pos.items():
            k=idx[t].get(dt)
            if k is not None: mtm+=pos['shares']*univ[t]['close'].iloc[k]
        eq=mtm; eq_curve.append((dt,eq,cash,len(open_pos)))
        # ---- 3. new entries: act on signals decided yesterday ----
        # signals decided on prior trading day fill today
        prevsig=sigmap.get(alldates[di-1]) if di>0 else None
        if prevsig and len(open_pos)<P['max_concurrent']:
            reg = regime.get(dt,'bull') if regime else 'bull'
            if not (P['regime_gate'] and reg=='bear'):
                # rank candidates by stop tightness (sniper) then ADR
                cands=[]
                for t,s in prevsig:
                    if t in open_pos: continue
                    k=idx[t].get(dt)
                    if k is None: continue
                    if P.get('use_scanner') and di not in scanner_active.get(t,()): continue
                    cands.append((t,s,k))
                cands.sort(key=lambda x:(x[1]['trig']-x[1]['stop'])/x[1]['trig'])
                for t,s,k in cands:
                    if len(open_pos)>=P['max_concurrent']: break
                    d=univ[t]; o,hi,lo=d['open'].iloc[k],d['high'].iloc[k],d['low'].iloc[k]
                    trig=s['trig']
                    if hi< trig: continue           # never triggered today
                    fill = o if o>trig else trig     # gap-through chases at open
                    fill*= (1+cost)
                    risk_dollar=eq*P['risk_pct']
                    stopw=fill-s['stop']
                    if stopw<=0: continue
                    shares=int(risk_dollar/stopw)
                    if shares<=0: continue
                    pos_val=shares*fill
                    cap=eq*P['max_pos_pct']
                    if pos_val>cap:
                        shares=int(cap/fill); pos_val=shares*fill
                    if shares<=0 or pos_val>cash:
                        if shares<=0: continue
                        shares=int(cash/fill); pos_val=shares*fill
                        if shares<=0: continue
                    cash-=pos_val*(1+cost)
                    adr=s['adr']
                    open_pos[t]=dict(ticker=t,entry=fill,init_shares=shares,shares=shares,
                        stop=s['stop'],adr=adr,setup=s['setup'],reg=reg,di_entry=di,
                        date_in=dt,realized=0.0,plevel=0,
                        next_target=fill*(1+P['partial_mult']*adr/100),
                        risk=risk_dollar)
    # close any still-open at last price
    for t,pos in list(open_pos.items()):
        last=univ[t]['close'].iloc[-1]
        pos['realized']+=pos['shares']*(last-pos['entry'])
        trades.append(_finalize(pos))
    return trades, eq_curve

def _close_lot(pos,sh,fill,cost,why,dt):
    pos['realized']+=sh*(fill-pos['entry']); pos['shares']-=sh
def _finalize(pos):
    return dict(ticker=pos['ticker'],setup=pos['setup'],reg=pos['reg'],
        date_in=pos['date_in'],pnl=pos['realized'],risk=pos['risk'],
        R=pos['realized']/pos['risk'] if pos['risk'] else 0.0)

# ---------- metrics ----------
def metrics(trades, eq_curve, subset=None):
    ts=[t for t in trades if subset is None or subset(t)]
    if not ts: return None
    pnl=np.array([t['pnl'] for t in ts]); R=np.array([t['R'] for t in ts])
    wins=pnl[pnl>0]; losses=pnl[pnl<0]
    pf=wins.sum()/abs(losses.sum()) if losses.sum()!=0 else float('inf')
    wr=len(wins)/len(ts)*100
    eq=np.array([e[1] for e in eq_curve])
    peak=np.maximum.accumulate(eq); dd=((peak-eq)/peak).max()*100 if len(eq) else 0
    # annualized Sharpe from daily portfolio returns
    sharpe=0.0
    if len(eq)>2:
        rets=np.diff(eq)/eq[:-1]; sd=rets.std()
        sharpe=round(rets.mean()/sd*np.sqrt(252),2) if sd>0 else 0.0
    # expectancy per trade (avg R)
    return dict(n=len(ts),pf=round(pf,2),wr=round(wr,1),avgR=round(R.mean(),2),
        sharpe=sharpe, net=round(pnl.sum(),0),maxDD=round(dd,1),
        ret=round((eq[-1]/eq[0]-1)*100,1) if len(eq) else 0)

if __name__=='__main__':
    P=json.loads(sys.argv[1]) if len(sys.argv)>1 else {}
    base=dict(entry_modes=['A','B','C'],touch_buf=0.015,adr_min=4.0,base_buf=0.06,
        stop_buf=0.003,avwap_band=0.02,cluster_band=0.02,cluster_min=2,
        min_stopw=0.005,max_stopw=0.12,risk_pct=0.005,max_pos_pct=0.35,
        max_concurrent=8,cost_bps=2.0,stop_slip_bps=5.0,partial_frac=0.15,
        partial_mult=3.0,partial_step=2.0,max_partials=3,be_after_partial=True,
        trail_ema=False,time_stop=0,regime_gate=False,equity0=100000.0)
    base.update(P)
    univ=load_universe(); regime=load_regime()
    print('universe',len(univ),'regime',('on' if regime else 'NONE'),file=sys.stderr)
    for d in univ.values(): add_avwap(d)
    trades,eqc=simulate(univ,regime,base)
    print(json.dumps(dict(
        ALL=metrics(trades,eqc),
        bull=metrics(trades,eqc,lambda t:t['reg']=='bull'),
        bear=metrics(trades,eqc,lambda t:t['reg']=='bear'),
        A=metrics(trades,eqc,lambda t:t['setup']=='A'),
        B=metrics(trades,eqc,lambda t:t['setup']=='B'),
        C=metrics(trades,eqc,lambda t:t['setup']=='C'),
    ),default=str))
