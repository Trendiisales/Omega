#!/usr/bin/env python3
"""Compare this month's ACTUAL BigCapMomo live trades vs the new ADR+Luke gate:
which trades the new engine would KEEP (high-ADR + valid A/C daily setup) vs SKIP.
The new filter changes WHICH trades, not the exit chassis -> kept trades keep their
actual net_pnl; the value is avoiding the skipped (mostly low-ADR) losers."""
import csv, glob, os, datetime as dt
import numpy as np, pandas as pd
import luke_bt as L

LEDGER='/tmp/june_ledger.csv'; DATA='/tmp/ib_data'
ADR_MIN=6.0; MAX_STOPW=0.06

def load(sym):
    p=f'{DATA}/{sym}.csv'
    if not os.path.exists(p): return None
    d=pd.read_csv(p); d['date']=pd.to_datetime(d['date']); d=d.sort_values('date').reset_index(drop=True)
    if len(d)<60: return None
    c,h,l=d['close'],d['high'],d['low']
    d['ema9']=c.ewm(span=9,adjust=False).mean(); d['ema21']=c.ewm(span=21,adjust=False).mean()
    d['ema50']=c.ewm(span=50,adjust=False).mean(); d['adr']=((h-l)/c*100).rolling(20).mean()
    d['ema21_slope']=d['ema21']-d['ema21'].shift(5)
    return d

def luke_decision(sym, entry_date):
    """Would the new ADR>=6 + A/C gate have TAKEN this name on entry_date?"""
    d=load(sym)
    if d is None: return ('NODATA',0.0,0)
    ed=pd.Timestamp(entry_date).normalize()
    # decision bar = last daily bar strictly before the entry session (setup decided on prior close)
    idx=d.index[d['date']<=ed]
    if len(idx)<56: return ('NODATA',0.0,0)
    i=idx[-1]
    adr=d['adr'].iloc[i]
    return _eval(d,i,adr)

def _eval(d,i,adr):
    # mirror omega::luke / luke_bt.gen_signals A/C on daily, decision bar i
    h=d['high'].values; l=d['low'].values; c=d['close'].values
    e9=d['ema9'].values; e21=d['ema21'].values; e50=d['ema50'].values; sl=d['ema21_slope'].values
    if np.isnan(e50[i]) or np.isnan(adr): return ('NODATA',adr,0)
    if adr<ADR_MIN: return ('SKIP_ADR',adr,0)
    up=sl[i]>0; lead=e9[i]>e21[i]>e50[i]
    hh20=max(h[max(0,i-19):i+1])
    setup=0; trig=0; stop=0
    if lead and up:
        near21=l[i]<=e21[i]*1.015 and c[i]>e21[i]; near9=l[i]<=e9[i]*1.015 and c[i]>e9[i]
        if (near21 or near9) and c[i]>=hh20*0.94: setup='A'; trig=h[i]; stop=min(l[i],e21[i] if near21 else e9[i])*0.997
    if not setup and up and e9[i]>e21[i] and c[i]>e21[i]:
        in1=h[i]<h[i-1] and l[i]>l[i-1]; in2=in1 and h[i-1]<h[i-2] and l[i-1]>l[i-2]
        if in1 or in2: setup='C'; trig=h[i]; stop=l[i]*0.997
    if not setup: return ('SKIP_NOSETUP',adr,0)
    if trig<=stop: return ('SKIP_NOSETUP',adr,0)
    sw=(trig-stop)/trig
    if sw>MAX_STOPW: return ('SKIP_WIDESTOP',adr,sw)
    return ('KEEP_'+setup,adr,sw)

rows=[r for r in csv.DictReader(open(LEDGER))]
bc=[r for r in rows if r['engine'] in ('BigCapMomo','BigCapMomoCons') and r['entry_ts_utc'][:7]=='2026-06']
print(f"{'SYM':6} {'date':11} {'engine':14} {'net$':>8} {'exit':8} {'ADR%':>5} -> DECISION")
keep=skip=0.0; nk=ns=nd=0; detail=[]
for r in sorted(bc,key=lambda r:r['entry_ts_utc']):
    sym=r['symbol']; ed=r['entry_ts_utc'][:10]; net=float(r['net_pnl'])
    dec,adr,sw=luke_decision(sym, ed)
    detail.append((sym,ed,r['engine'],net,r['exit_reason'],adr,dec))
    tag='KEEP' if dec.startswith('KEEP') else ('SKIP' if dec.startswith('SKIP') else 'NODATA')
    if tag=='KEEP': keep+=net; nk+=1
    elif tag=='SKIP': skip+=net; ns+=1
    else: nd+=1
    print(f"{sym:6} {ed:11} {r['engine']:14} {net:8.2f} {r['exit_reason']:8} {adr:5.1f} -> {dec}")
print()
print(f"ACTUAL book ({len(bc)} trades): net ${sum(float(r['net_pnl']) for r in bc):.2f}")
print(f"  KEEP (new engine takes): {nk} trades, net ${keep:.2f}")
print(f"  SKIP (new engine avoids): {ns} trades, net ${skip:.2f}")
print(f"  NODATA (can't assess):   {nd} trades")
print(f"NEW-ENGINE book = KEEP only: net ${keep:.2f}  (delta vs actual on assessable: ${keep - (keep+skip):.2f} avoided)")
