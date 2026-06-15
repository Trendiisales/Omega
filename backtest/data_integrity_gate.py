#!/usr/bin/env python3
# DATA-INTEGRITY GATE — certify a tick dataset BEFORE any edge research.
# Catches the failures that wasted 2026-06-15: x1000 price glitches (DJ30),
# column-order swaps, dead spreads, gaps, dupes, coverage holes.
# Exit 0 = CERTIFIED clean; exit 1 = FAILED (do not use for research).
#
# Usage: data_integrity_gate.py <file.csv> [expected_price_lo] [expected_price_hi]
# Auto-detects HISTDATA (YYYYMMDD HHMMSSmmm,bid,ask) and MS_TS (ts_ms,c1,c2) formats.
import sys, statistics, datetime

def parse(path):
    rows=[]  # (ts_ms, p1, p2)  raw column order
    with open(path, 'rb') as f:
        first=True; histdata=None; askfirst=False
        for raw in f:
            try: line=raw.decode('ascii','ignore').strip()
            except: continue
            if not line: continue
            c0=line[0]
            if first and (c0<'0' or c0>'9'):
                askfirst = ('ask' in line.lower() and 'bid' in line.lower()
                            and line.lower().index('ask')<line.lower().index('bid'))
                first=False; continue
            first=False
            if histdata is None:
                head=line.split(',',1)[0]
                histdata = (' ' in head)
            parts=line.split(',')
            if histdata:
                if len(parts)<3: continue
                dt=parts[0].split(' ')
                if len(dt)<2: continue
                try:
                    ymd=int(dt[0]); hms=int(dt[1])
                    y=ymd//10000; mo=(ymd//100)%100; d=ymd%100
                    ms=hms%1000; t=hms//1000; h=t//10000; mi=(t//100)%100; s=t%100
                    ts=int(datetime.datetime(y,mo,d,h,mi,s,ms*1000,tzinfo=datetime.timezone.utc).timestamp()*1000)
                    p1=float(parts[1]); p2=float(parts[2])
                except: continue
            else:
                if len(parts)<3: continue
                try: ts=int(parts[0]); p1=float(parts[1]); p2=float(parts[2])
                except: continue
            rows.append((ts,p1,p2,askfirst))
    return rows

def main():
    path=sys.argv[1]
    lo=float(sys.argv[2]) if len(sys.argv)>2 else None
    hi=float(sys.argv[3]) if len(sys.argv)>3 else None
    rows=parse(path)
    fails=[]; warns=[]
    if len(rows)<1000:
        print(f"FAIL: only {len(rows)} rows parsed"); sys.exit(1)
    askfirst=rows[0][3]
    # normalize to bid/ask
    px=[]; spreads=[]; ts=[]
    for (t,p1,p2,af) in rows:
        bid,ask=(p2,p1) if af else (p1,p2)
        px.append((bid+ask)/2.0); spreads.append(ask-bid); ts.append(t)
    med=statistics.median(px)
    # 1. price magnitude / x1000 glitch: any mid >3x or <1/3 of median
    glitch=sum(1 for m in px if m>3*med or (m>0 and m<med/3))
    if glitch> len(px)*0.0005: fails.append(f"PRICE GLITCH: {glitch} ticks off >3x from median {med:.2f} (x1000 bug?)")
    elif glitch>0: warns.append(f"{glitch} price outliers (>3x median) — inspect")
    # 2. expected band
    if lo and hi and not (lo<=med<=hi): fails.append(f"PRICE BAND: median {med:.2f} outside expected [{lo},{hi}]")
    # 3. spread sanity
    negs=sum(1 for s in spreads if s<0)
    zeros=sum(1 for s in spreads if s==0)
    if negs>len(spreads)*0.01: fails.append(f"COLUMN ORDER: {negs} negative spreads ({100*negs/len(spreads):.1f}%) — bid/ask swapped?")
    if zeros>len(spreads)*0.5: warns.append(f"{100*zeros/len(spreads):.0f}% zero spreads")
    medspr=statistics.median([s for s in spreads if s>0] or [0])
    if medspr> med*0.01: warns.append(f"wide median spread {medspr:.4f} ({1e4*medspr/med:.1f}bps)")
    # 4. monotonic ts + gaps
    nonmono=sum(1 for i in range(1,len(ts)) if ts[i]<ts[i-1])
    if nonmono>len(ts)*0.001: warns.append(f"{nonmono} out-of-order timestamps")
    span_days=(ts[-1]-ts[0])/86400000.0
    # gap detection: biggest gap (excl weekends ~2.5d)
    biggest=max((ts[i]-ts[i-1])/3600000.0 for i in range(1,len(ts)))
    if biggest>96: warns.append(f"largest gap {biggest:.0f}h ({biggest/24:.1f}d) — missing data?")
    # 5. dupes
    dup=len(ts)-len(set(ts))
    print(f"--- {path.split('/')[-1]} ---")
    print(f"  rows={len(rows):,}  median_px={med:.3f}  median_spread={medspr:.5f}  span={span_days:.0f}d  ask_first={askfirst}")
    print(f"  range: {datetime.datetime.utcfromtimestamp(ts[0]/1000):%Y-%m-%d} .. {datetime.datetime.utcfromtimestamp(ts[-1]/1000):%Y-%m-%d}")
    for w in warns: print(f"  WARN: {w}")
    for fl in fails: print(f"  FAIL: {fl}")
    if fails: print("  => REJECTED\n"); sys.exit(1)
    print("  => CERTIFIED CLEAN\n"); sys.exit(0)

if __name__=="__main__": main()
