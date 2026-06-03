import csv, sys, numpy as np
csv.field_size_limit(10**7)
files=["/tmp/l2_2026-06-01.csv","/tmp/l2_2026-06-02.csv","/tmp/l2_today.csv"]
ts=[];mid=[];imb=[];bv=[];av=[];bl=[];al=[];ev=[];bid=[];ask=[]
for fn in files:
    r=csv.reader(open(fn)); h=next(r); ix={c:i for i,c in enumerate(h)}
    for row in r:
        try:
            ts.append(int(row[ix['ts_ms']])); mid.append(float(row[ix['mid']]))
            bid.append(float(row[ix['bid']])); ask.append(float(row[ix['ask']]))
            imb.append(float(row[ix['l2_imb']])); bv.append(float(row[ix['l2_bid_vol']])); av.append(float(row[ix['l2_ask_vol']]))
            bl.append(float(row[ix['depth_bid_levels']])); al.append(float(row[ix['depth_ask_levels']])); ev.append(float(row[ix['depth_events_total']]))
        except: pass
ts=np.array(ts);mid=np.array(mid);imb=np.array(imb);bv=np.array(bv);av=np.array(av)
bl=np.array(bl);al=np.array(al);ev=np.array(ev);bid=np.array(bid);ask=np.array(ask)
n=len(ts); print(f"rows={n}")
# forward mid move over H ms (per row find index ~H ahead by ts)
def fwd_ret(H):
    out=np.full(n,np.nan); j=0
    for i in range(n):
        tgt=ts[i]+H
        if j<i: j=i
        while j<n and ts[j]<tgt: j+=1
        if j<n: out[i]=mid[j]-mid[i]
    return out
def hit(feat, fr, q=0.2):
    m=~np.isnan(fr)&~np.isnan(feat); f=feat[m];r=fr[m]
    if len(f)<5000: return None
    hi=np.quantile(f,1-q); lo=np.quantile(f,q)
    up=r[f>=hi]; dn=r[f<=lo]
    # directional: top-quintile -> up? bottom -> down?
    return (np.mean(up>0)*100, np.mean(dn<0)*100, np.mean(up), np.mean(dn), len(up))
def volcorr(feat, afr):
    m=~np.isnan(afr)&~np.isnan(feat); 
    if m.sum()<5000: return None
    return np.corrcoef(feat[m],afr[m])[0,1]
for H in (2000,5000):
    fr=fwd_ret(H); afr=np.abs(fr)
    print(f"\n=== H={H}ms  (directional hit%: top-quintile->up / bottom->down; >50 each = edge) ===")
    imb_flow=np.concatenate([[np.nan]*20, imb[20:]-imb[:-20]])  # ~flow over ~20 ticks
    lvl_imb=(bl-al)/np.maximum(bl+al,1)
    totv=bv+av; spread=ask-bid
    for name,feat in [("imb_level",imb-0.5),("imb_flow",imb_flow),("lvl_imb",lvl_imb)]:
        h=hit(feat,fr)
        if h: print(f"  {name:10} up={h[0]:.1f}% dn={h[1]:.1f}%  meanUp={h[2]:+.3f} meanDn={h[3]:+.3f} n={h[4]}")
    print(f"  -- vol/expansion predictors (corr with |fwd move|; >0 = predicts expansion) --")
    for name,feat in [("events",ev),("tot_vol",totv),("spread",spread)]:
        c=volcorr(feat,afr)
        if c is not None: print(f"  {name:10} corr|move|={c:+.3f}")
