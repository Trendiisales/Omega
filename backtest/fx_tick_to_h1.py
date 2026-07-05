#!/usr/bin/env python3
"""Aggregate HistData DAT_ASCII tick (YYYYMMDD HHMMSS<ms>,bid,ask,vol) -> H1 mid-OHLC bars
matching the EURUSD_merged.h1.csv convention (ts_sec,o,h,l,c). Also prints the pair's median
bid-ask spread in bp -> the HONEST RT cost floor for the BE-floor companion sweep.

  python3 fx_tick_to_h1.py AUDUSD [NZDUSD ...]
Writes /Users/jo/Tick/<PAIR>_befloor_h1.csv (picked up by fx_befloor_ls.py's default glob).
"""
import sys, os, glob, statistics, calendar

TICK="/Users/jo/Tick"

def agg(pair):
    files=sorted(glob.glob(f"{TICK}/{pair}/DAT_ASCII_{pair}_T_*.csv"))
    if not files:
        print(f"  {pair}: no tick files"); return None
    bars={}                # hour_ts -> [o,h,l,c]
    spreads=[]
    ntick=0
    for fp in files:
        with open(fp) as f:
            for line in f:
                p=line.split(",")
                if len(p)<3: continue
                dts=p[0]
                try:
                    bid=float(p[1]); ask=float(p[2])
                except ValueError:
                    continue
                mid=(bid+ask)/2.0
                if mid<=0: continue
                # 'YYYYMMDD HHMMSS...' -> epoch sec (treat as UTC; TZ offset is jump-invariant)
                try:
                    d=dts[:8]; hh=dts[9:11]; mm=dts[11:13]; ss=dts[13:15]
                    t=calendar.timegm((int(d[:4]),int(d[4:6]),int(d[6:8]),int(hh),int(mm),int(ss),0,0,0))
                except (ValueError, IndexError):
                    continue
                hts=t-(t%3600)
                if len(spreads)<2_000_000 and ask>bid:
                    spreads.append((ask-bid)/mid*1e4)   # spread in bp
                ntick+=1
                b=bars.get(hts)
                if b is None: bars[hts]=[mid,mid,mid,mid]
                else:
                    if mid>b[1]: b[1]=mid
                    if mid<b[2]: b[2]=mid
                    b[3]=mid
    if not bars:
        print(f"  {pair}: 0 bars"); return None
    out=f"{TICK}/{pair}_befloor_h1.csv"
    with open(out,"w") as f:
        f.write("ts,o,h,l,c\n")
        for hts in sorted(bars):
            o,h,l,c=bars[hts]
            f.write(f"{hts},{o:.6f},{h:.6f},{l:.6f},{c:.6f}\n")
    msp=statistics.median(spreads) if spreads else float("nan")
    print(f"  {pair}: {ntick:,} ticks -> {len(bars):,} H1 bars  median_spread={msp:.2f}bp  -> {out}")
    return msp

def main():
    for pair in sys.argv[1:]:
        agg(pair)

if __name__=="__main__": main()
