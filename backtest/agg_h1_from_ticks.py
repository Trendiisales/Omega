#!/usr/bin/env python3
# Aggregate HISTDATA tick file (YYYYMMDD HHMMSSmmm,bid,ask[,vol]) -> H1 OHLC bars.
# mid = (bid+ask)/2. Output: epoch_seconds,o,h,l,c per hour. Streaming, O(1) mem.
import sys, datetime
inp, out = sys.argv[1], sys.argv[2]
cur_hour=None; o=h=l=c=None; n=0; bars=0
with open(inp) as f, open(out,'w') as w:
    for line in f:
        p=line.split(',')
        if len(p)<3: continue
        dt=p[0].split(' ')
        if len(dt)<2: continue
        try:
            ymd=int(dt[0]); hms=int(dt[1])
            bid=float(p[1]); ask=float(p[2])
        except: continue
        mid=(bid+ask)/2.0
        if mid<=0: continue
        y=ymd//10000; mo=(ymd//100)%100; d=ymd%100
        t=hms//1000; hh=t//10000
        hour_key=(ymd, hh)
        if hour_key!=cur_hour:
            if cur_hour is not None:
                ts=int(datetime.datetime(cur_hour[0]//10000,(cur_hour[0]//100)%100,cur_hour[0]%100,cur_hour[1],tzinfo=datetime.timezone.utc).timestamp())
                w.write(f"{ts},{o:.3f},{h:.3f},{l:.3f},{c:.3f}\n"); bars+=1
            cur_hour=hour_key; o=h=l=c=mid
        else:
            if mid>h: h=mid
            if mid<l: l=mid
            c=mid
        n+=1
    if cur_hour is not None:
        ts=int(datetime.datetime(cur_hour[0]//10000,(cur_hour[0]//100)%100,cur_hour[0]%100,cur_hour[1],tzinfo=datetime.timezone.utc).timestamp())
        w.write(f"{ts},{o:.3f},{h:.3f},{l:.3f},{c:.3f}\n"); bars+=1
print(f"ticks={n} bars={bars} -> {out}")
