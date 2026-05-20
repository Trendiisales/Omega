#!/usr/bin/env python3
"""Build H1+H4+M5 bars from ts_ms,bid,ask tick CSV."""
import sys, csv, time

def main():
    src = sys.argv[1]
    out_m5 = src.replace(".csv", ".m5.csv")
    out_h1 = src.replace(".csv", ".h1.csv")
    out_h4 = src.replace(".csv", ".h4.csv")
    m5={}; h1={}; h4={}
    n=0; t0=time.time()
    with open(src) as f:
        r=csv.reader(f); next(r,None)
        for row in r:
            try: ts_ms=int(row[0]); bid=float(row[1]); ask=float(row[2])
            except (ValueError, IndexError): continue
            ts_s=ts_ms//1000; mid=(ask+bid)*0.5
            for bucket,store in ((ts_s//300*300, m5),(ts_s//3600*3600, h1),(ts_s//14400*14400, h4)):
                b=store.get(bucket)
                if b is None: store[bucket]=[bucket,mid,mid,mid,mid]
                else:
                    if mid>b[2]: b[2]=mid
                    if mid<b[3]: b[3]=mid
                    b[4]=mid
            n+=1
            if n%10_000_000==0:
                print(f"  ... {n/1e6:.1f}M ({time.time()-t0:.0f}s)")
    print(f"[BUILD] {n/1e6:.1f}M ticks -> m5={len(m5)} h1={len(h1)} h4={len(h4)} ({time.time()-t0:.0f}s)")
    for path,src_d in ((out_m5,m5),(out_h1,h1),(out_h4,h4)):
        with open(path,"w") as o:
            o.write("ts,o,h,l,c\n")
            for k in sorted(src_d):
                b=src_d[k]
                o.write(f"{b[0]},{b[1]:.6f},{b[2]:.6f},{b[3]:.6f},{b[4]:.6f}\n")
    print(f"[OUT] {out_m5} {out_h1} {out_h4}")

if __name__ == "__main__":
    main()
