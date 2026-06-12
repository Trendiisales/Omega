#!/usr/bin/env python3
"""
bigcap_mover_scalp_bt — backtest the operator's "major-mover scalp" on LIQUID big-caps
(NAS100 + SP500 constituents) over ~23 months, HOURLY bars.

Thesis (operator): the pump-scalp edge died on micro-cap SLIPPAGE, not the momentum.
On a deep-liquidity big-cap that ignites (jumps), jump in fast, ride a TIGHT TRAIL,
exit when the move is exhausted (trail hit). Big-cap slippage is small, so the edge
may survive where it died on pennies.

Data : Yahoo chart range=2y interval=1h  (hourly OHLCV, ~23 months -- the most history
        Yahoo gives intraday). Coarse for a true scalp but the right shape for a
        multi-hour intraday momentum ride; live would run finer (5m/L2).
Logic (per symbol, one position at a time):
  IGNITION (long): cumulative return over LB hourly bars >= IG_PCT, AND this bar's
    volume >= VOLX * trailing-20-bar avg volume (a real surge, not drift).
  ENTRY  : next price = ignition bar close * (1 + SLIP) (pay up to get in).
  TRAIL  : peak = max high since entry; trail_stop = peak * (1 - TRAIL_PCT).
           exit when a bar's low <= trail_stop -> fill trail_stop * (1 - SLIP).
  TIMEOUT: exit at MAX_HOLD bars (close).
  Cost is in SLIP (round trip ~2*SLIP). Sweep IG_PCT to find the lowest that works.
"""
import csv, sys, json, urllib.request, datetime, concurrent.futures as cf

UNIVERSE_FILE = "/tmp/bigcap_universe.txt"
INTERVAL  = "5m"     # 5-minute bars (true scalp resolution)
RANGE     = "60d"    # ~2-3 months (Yahoo 5m history limit)
LB        = 6        # ignition lookback = 6*5m = 30 min
VOLX      = 3.0      # volume surge multiple vs 20-bar avg
TRAIL_PCT = 0.020    # 2.0% tight trail from peak (5m scalp)
MAX_HOLD  = 48       # 48*5m = 4 hours backstop
SLIP      = 0.0008   # 8 bps per side (big-cap momentum fill); round-trip ~16 bps
IG_GRID   = [3.0, 5.0, 8.0, 10.0, 15.0]
MIN_PRICE = 10.0
UA={"User-Agent":"Mozilla/5.0"}

def get(u,t=20): return urllib.request.urlopen(urllib.request.Request(u,headers=UA),timeout=t).read().decode("utf-8","replace")

def bars(sym):
    url=f"https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range={RANGE}&interval={INTERVAL}"
    try:
        j=json.loads(get(url,15)); res=j["chart"]["result"][0]
        ts=res["timestamp"]; q=res["indicators"]["quote"][0]
        o,h,l,c,v=q["open"],q["high"],q["low"],q["close"],q["volume"]
        out=[]
        for i in range(len(ts)):
            if None in (o[i],h[i],l[i],c[i],v[i]): continue
            out.append((ts[i],o[i],h[i],l[i],c[i],v[i]))
        return out
    except Exception: return []

def sim(b, ig_pct):
    """run ignition->trail on one symbol's hourly bars; return list of trade returns (net)."""
    n=len(b); trades=[]
    if n < 30: return trades
    # trailing 20-bar avg volume + cumulative LB return
    inpos=False; entry=0.0; peak=0.0; hold=0; entry_i=0
    for i in range(21, n):
        ts,o,h,l,c,v = b[i]
        if inpos:
            hold+=1
            if h>peak: peak=h
            tstop = peak*(1-TRAIL_PCT)
            if l<=tstop:
                ex=tstop*(1-SLIP); trades.append(ex/entry-1); inpos=False; continue
            if hold>=MAX_HOLD:
                ex=c*(1-SLIP); trades.append(ex/entry-1); inpos=False; continue
            continue
        if c < MIN_PRICE: continue
        avgv=sum(b[k][5] for k in range(i-20,i))/20.0
        if avgv<=0 or v < VOLX*avgv: continue
        base=b[i-LB][4]
        if base<=0: continue
        cum=(c/base-1)*100
        if cum < ig_pct: continue
        entry=c*(1+SLIP); peak=h; hold=0; entry_i=i; inpos=True
    return trades

def main():
    syms=[s.strip() for s in open(UNIVERSE_FILE) if s.strip()]
    print(f"# {len(syms)} big-cap syms; Yahoo {RANGE} {INTERVAL}; trail={TRAIL_PCT*100:.1f}% slip={SLIP*1e4:.0f}bps/side maxhold={MAX_HOLD}bars",file=sys.stderr)
    data={}; done=0
    with cf.ThreadPoolExecutor(max_workers=10) as ex:
        futs={ex.submit(bars,s):s for s in syms}
        for fut in cf.as_completed(futs):
            s=futs[fut]; done+=1
            if done%100==0: print(f"# {done}/{len(syms)} pulled",file=sys.stderr)
            b=fut.result()
            if b: data[s]=b
    print(f"# pulled {len(data)} symbols with hourly data",file=sys.stderr)
    print(f"\n=== BIG-CAP MOVER SCALP (hourly, ~23mo, {len(data)} NAS/SPX names, ~2-3mo 5m) ===")
    print(f"trail={TRAIL_PCT*100:.1f}%  slip={SLIP*1e4:.0f}bps/side  vol_surge>={VOLX}x  maxhold={MAX_HOLD}h")
    print(f"{'IG%':>5} {'trades':>7} {'WR%':>6} {'avg%':>7} {'PF':>6} {'tot%':>9} {'avgWin%':>8} {'avgLoss%':>9}")
    for ig in IG_GRID:
        allr=[]
        for s,b in data.items(): allr += sim(b,ig)
        if not allr: print(f"{ig:5.0f} {0:7}"); continue
        w=[r for r in allr if r>0]; lo=[r for r in allr if r<=0]
        gw=sum(w); gl=-sum(lo)
        pf=gw/gl if gl>0 else (99 if gw>0 else 0)
        print(f"{ig:5.0f} {len(allr):7} {100*len(w)/len(allr):6.1f} {1e2*sum(allr)/len(allr):7.3f} {pf:6.2f} {1e2*sum(allr):9.1f} {1e2*(gw/len(w) if w else 0):8.3f} {1e2*(sum(lo)/len(lo) if lo else 0):9.3f}")
    print("\nNote: hourly resolution understates a true fast-trail (live 5m/L2 would exit tighter).")

if __name__=="__main__":
    main()
