#!/usr/bin/env python3
"""
L2 / order-flow archetype sweep on XAUUSD L2 data (24 days late Apr-mid May).

Tests killed/disabled engines that need L2 + order flow:
- DomPersist (l2_imb extreme + persistence)
- GoldFlow (cumulative micro-flow direction)
- LiquiditySweep (depth events spike + price move)
- Micro-Edge (micro_edge signal persistence)
- VPIN-direction (high VPIN + close direction)

Format per row:
  ts_ms,mid,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,
  depth_bid_levels,depth_ask_levels,depth_events_total,
  watchdog_dead,vol_ratio,regime,vpin,has_pos,micro_edge,ewm_drift
"""
import csv, math, os, glob, time
from collections import deque

t0 = time.time()

# Aggregate L2 ticks to M5 OHLC + L2 features (avg imb, max vpin, etc)
def aggregate_m5_with_l2(paths):
    bars = {}  # bucket_ts -> [open, high, low, close, sum_imb, n_imb, max_vpin, sum_micro, n_micro]
    for path in paths:
        with open(path) as f:
            r = csv.reader(f); next(r, None)
            for row in r:
                try:
                    ts_ms = int(row[0]); mid = float(row[1])
                    imb = float(row[4]); vpin = float(row[13]); micro = float(row[15])
                except (ValueError, IndexError): continue
                ts = ts_ms // 1000
                bucket = ts // 300 * 300
                b = bars.get(bucket)
                if b is None:
                    bars[bucket] = [mid, mid, mid, mid, imb, 1, vpin, micro, 1]
                else:
                    if mid > b[1]: b[1] = mid
                    if mid < b[2]: b[2] = mid
                    b[3] = mid
                    b[4] += imb; b[5] += 1
                    if vpin > b[6]: b[6] = vpin
                    b[7] += micro; b[8] += 1
    # Sort + flatten
    out = []
    for k in sorted(bars):
        b = bars[k]
        avg_imb = b[4]/b[5] if b[5] else 0.5
        avg_micro = b[7]/b[8] if b[8] else 0.0
        out.append((k, b[0], b[1], b[2], b[3], avg_imb, b[6], avg_micro))
    return out

print("Loading L2 ticks...")
paths = sorted(glob.glob("/Users/jo/Tick/l2_xau_vps/l2_ticks_XAUUSD_*.csv"))
print(f"  {len(paths)} files")
bars = aggregate_m5_with_l2(paths)
print(f"  {len(bars)} M5 bars built ({(time.time()-t0):.0f}s)")
print(f"  ts range: {bars[0][0]} -> {bars[-1][0]} ({(bars[-1][0]-bars[0][0])/86400:.1f} days)")

# Also aggregate to H1
def to_h1(m5_bars):
    out = {}
    for b in m5_bars:
        bkt = b[0] // 3600 * 3600
        if bkt not in out:
            out[bkt] = list(b)
            out[bkt][0] = bkt
        else:
            o = out[bkt]
            if b[2] > o[2]: o[2] = b[2]
            if b[3] < o[3]: o[3] = b[3]
            o[4] = b[4]
            # avg imb/vpin/micro: simple last (since aggregated already from M5)
            o[5] = (o[5] + b[5])/2
            o[6] = max(o[6], b[6])
            o[7] = (o[7] + b[7])/2
    return [out[k] for k in sorted(out)]

h1_bars = to_h1(bars)
print(f"  {len(h1_bars)} H1 bars")

def stats(pnls):
    if not pnls: return None
    n=len(pnls); pnl=sum(pnls); wr=100*sum(1 for p in pnls if p>0)/n
    m=pnl/n; v=sum((p-m)**2 for p in pnls)/max(1,n-1); sd=math.sqrt(v)
    sh=(m/sd*math.sqrt(252)) if sd>0 else 0
    return dict(n=n,pnl=pnl,wr=wr,sharpe=sh)

# bar tuple: (ts, o, h, l, c, imb, vpin, micro)
def atr_series(bars, period=14):
    out=[None]*len(bars); trs=deque(maxlen=period); pc=None
    for i,b in enumerate(bars):
        h=b[2]; l=b[3]; c=b[4]
        tr=h-l
        if pc is not None: tr=max(tr,abs(h-pc),abs(l-pc))
        trs.append(tr); pc=c
        if len(trs)==period: out[i]=sum(trs)/period
    return out

def manage(bars, i, entry, sl, tp, hold, ei, is_long, cost):
    h=bars[i][2]; l=bars[i][3]; c=bars[i][4]
    sl_hit=(l<=sl) if is_long else (h>=sl)
    tp_hit=(h>=tp) if is_long else (l<=tp)
    timed=(i-ei)>=hold
    if sl_hit:
        r=(sl-entry)/entry if is_long else (entry-sl)/entry
        return r-cost, True
    if tp_hit:
        r=(tp-entry)/entry if is_long else (entry-tp)/entry
        return r-cost, True
    if timed:
        r=(c-entry)/entry if is_long else (entry-c)/entry
        return r-cost, True
    return 0.0, False

# ── ARCHETYPE 1: DomPersist — l2_imb extreme + persistence ───────────────────
def dom_persist(bars, imb_threshold, persist_bars, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """imb > threshold for persist_bars consecutive bars -> long. (imb > 0.5 = bid heavy)."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    persist_run=0
    for i in range(15,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        imb=bars[i][5]
        if imb > imb_threshold:
            persist_run += 1
            if persist_run >= persist_bars:
                is_long=True
                c=bars[i][4]; apct=atrs[i]/c
                entry=c; sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
                active=True; ei=i; persist_run=0
        elif imb < (1-imb_threshold):
            persist_run += 1
            if persist_run >= persist_bars:
                is_long=False
                c=bars[i][4]; apct=atrs[i]/c
                entry=c; sl=entry*(1+sl_atr*apct); tp=entry*(1-tp_atr*apct)
                active=True; ei=i; persist_run=0
        else:
            persist_run = 0
    return pnls

# ── ARCHETYPE 2: GoldFlow — micro_edge persistence + direction ───────────────
def gold_flow(bars, micro_thresh, persist_bars, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi, long_only=True):
    """avg_micro_edge > thresh for N bars (long-only mode)."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    run=0
    for i in range(15,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        micro=bars[i][7]
        if micro > micro_thresh:
            run += 1
            if run >= persist_bars:
                is_long=True
                c=bars[i][4]; apct=atrs[i]/c
                entry=c; sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
                active=True; ei=i; run=0
        elif micro < -micro_thresh and not long_only:
            run += 1
            if run >= persist_bars:
                is_long=False
                c=bars[i][4]; apct=atrs[i]/c
                entry=c; sl=entry*(1+sl_atr*apct); tp=entry*(1-tp_atr*apct)
                active=True; ei=i; run=0
        else:
            run = 0
    return pnls

# ── ARCHETYPE 3: VPIN-direction — high VPIN + dir ────────────────────────────
def vpin_dir(bars, vpin_thresh, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """High VPIN (>thresh) + bar bullish close -> long."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    for i in range(15,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        vpin=bars[i][6]
        if vpin < vpin_thresh: continue
        o=bars[i][1]; c=bars[i][4]
        if c <= o: continue
        apct=atrs[i]/c
        entry=c; sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

# ── ARCHETYPE 4: IMB-fade — imb extreme = exhaustion, FADE ──────────────────
def imb_fade(bars, imb_threshold, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """Extreme imb = local exhaustion. Fade direction."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; is_long=False; ei=0
    for i in range(15,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,is_long,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        imb=bars[i][5]
        if imb > imb_threshold:
            # Heavy bid -> fade short
            is_long=False
        elif imb < (1-imb_threshold):
            # Heavy ask -> fade long
            is_long=True
        else: continue
        c=bars[i][4]; apct=atrs[i]/c
        entry=c
        sl=entry*(1-sl_atr*apct) if is_long else entry*(1+sl_atr*apct)
        tp=entry*(1+tp_atr*apct) if is_long else entry*(1-tp_atr*apct)
        active=True; ei=i
    return pnls

# ── ARCHETYPE 5: Combined IMB + MICRO ────────────────────────────────────────
def imb_micro_combo(bars, imb_thresh, micro_thresh, hold, sl_atr, tp_atr, cost, ts_lo, ts_hi):
    """imb > thresh AND micro_edge > thresh -> long."""
    pnls=[]; atrs=atr_series(bars)
    active=False; entry=sl=tp=0; ei=0
    for i in range(15,len(bars)):
        ts=bars[i][0]
        if ts<ts_lo or ts>=ts_hi: continue
        if active:
            p,closed=manage(bars,i,entry,sl,tp,hold,ei,True,cost)
            if closed: pnls.append(p); active=False
            continue
        if atrs[i] is None: continue
        imb=bars[i][5]; micro=bars[i][7]
        if imb < imb_thresh or micro < micro_thresh: continue
        c=bars[i][4]; apct=atrs[i]/c
        entry=c; sl=entry*(1-sl_atr*apct); tp=entry*(1+tp_atr*apct)
        active=True; ei=i
    return pnls

# ── Sweep ────────────────────────────────────────────────────────────────────
def sweep(label, fn, params_grid, bars, tf, results, min_n=10, min_sh=0.5, cost=0.0001):
    if not bars or len(bars)<150: return
    mid=bars[len(bars)//2][0]
    best=None
    for params in params_grid:
        try:
            p_is=fn(bars, *params, cost, bars[0][0], mid)
            p_oos=fn(bars, *params, cost, mid, bars[-1][0]+1)
            p_ful=fn(bars, *params, cost, 0, 10**12)
        except: continue
        s_is=stats(p_is); s_oos=stats(p_oos); s_ful=stats(p_ful)
        if not s_is or not s_oos or not s_ful: continue
        if s_ful["n"]<min_n: continue
        if s_is["sharpe"]<=0 or s_oos["sharpe"]<=0: continue
        mn=min(s_is["sharpe"],s_oos["sharpe"])
        if best is None or mn>best[5]: best=(params,s_is,s_oos,s_ful,mn,mn)
    if best:
        params,s_is,s_oos,s_ful,_,mn=best
        if mn>=min_sh:
            results.append((label, tf, params, s_is, s_oos, s_ful, mn))
            print(f"  [{label:<22}] {tf:<3} min_sh={mn:.2f} IS={s_is['sharpe']:.2f} OOS={s_oos['sharpe']:.2f} FUL={s_ful['sharpe']:.2f} n={s_ful['n']} PnL={s_ful['pnl']*100:.2f}% WR={s_ful['wr']:.1f}%")

results = []

print("\n=== DOM_PERSIST (l2_imb extreme + persistence) ===")
GR = [(t,p,h,sl,tp) for t in [0.55,0.60,0.65,0.70] for p in [2,3,5] for h in [3,5,10,20] for sl in [1.0,1.5,2.0] for tp in [1.5,2.0,3.0]]
sweep("DOM_PERSIST", dom_persist, GR, bars, "M5", results)
sweep("DOM_PERSIST", dom_persist, GR, h1_bars, "H1", results)

print("\n=== GOLD_FLOW (micro_edge persistence) ===")
GR = [(t,p,h,sl,tp) for t in [0.3,0.4,0.5,0.6] for p in [2,3,5] for h in [3,5,10,20] for sl in [1.0,1.5,2.0] for tp in [1.5,2.0,3.0]]
sweep("GOLD_FLOW_LONG", gold_flow, GR, bars, "M5", results)
sweep("GOLD_FLOW_LONG", gold_flow, GR, h1_bars, "H1", results)

print("\n=== VPIN_DIR (high VPIN + bull close) ===")
GR = [(v,h,sl,tp) for v in [0.3,0.5,0.7,0.9] for h in [3,5,10,20] for sl in [1.0,1.5,2.0] for tp in [1.5,2.0,3.0]]
sweep("VPIN_DIR_LONG", vpin_dir, GR, bars, "M5", results)
sweep("VPIN_DIR_LONG", vpin_dir, GR, h1_bars, "H1", results)

print("\n=== IMB_FADE (extreme imb = exhaustion) ===")
GR = [(t,h,sl,tp) for t in [0.55,0.60,0.65,0.70,0.75] for h in [3,5,10] for sl in [1.0,1.5,2.0] for tp in [1.0,1.5,2.0]]
sweep("IMB_FADE", imb_fade, GR, bars, "M5", results)
sweep("IMB_FADE", imb_fade, GR, h1_bars, "H1", results)

print("\n=== IMB+MICRO COMBO ===")
GR = [(i,m,h,sl,tp) for i in [0.55,0.60,0.65] for m in [0.3,0.5,0.7] for h in [3,5,10,20] for sl in [1.0,1.5,2.0] for tp in [1.5,2.0,3.0]]
sweep("IMB_MICRO_COMBO", imb_micro_combo, GR, bars, "M5", results)
sweep("IMB_MICRO_COMBO", imb_micro_combo, GR, h1_bars, "H1", results)

print(f"\n=== L2 WINNERS RANKING ===")
results.sort(key=lambda x:-x[6])
print(f"{'Rank':>4} {'Archetype':<20} {'TF':<3} {'IS_Sh':>6} {'OOS_Sh':>6} {'FUL_Sh':>6} {'n':>4} {'PnL%':>6}")
print("-"*80)
for i,(label,tf,params,s_is,s_oos,s_ful,mn) in enumerate(results,1):
    print(f"{i:>4} {label:<20} {tf:<3} {s_is['sharpe']:>6.2f} {s_oos['sharpe']:>6.2f} {s_ful['sharpe']:>6.2f} {s_ful['n']:>4} {s_ful['pnl']*100:>5.2f}%")

print(f"\nTotal winners: {len(results)}")
print(f"Elapsed: {time.time()-t0:.0f}s")
