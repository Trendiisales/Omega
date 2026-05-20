#!/usr/bin/env python3
"""Build M5 OHLC bars from ts_ms,bid,ask tick CSV."""
import sys, csv, time

def main():
    src = sys.argv[1]
    out_m5 = src.replace(".csv", ".m5.csv")
    m5 = {}
    n = 0; t0 = time.time()
    with open(src) as f:
        r = csv.reader(f); next(r, None)
        for row in r:
            try:
                ts_ms = int(row[0]); bid = float(row[1]); ask = float(row[2])
            except (ValueError, IndexError):
                continue
            ts_s = ts_ms // 1000
            mid = (ask + bid) * 0.5
            bucket = ts_s // 300 * 300
            b = m5.get(bucket)
            if b is None:
                m5[bucket] = [bucket, mid, mid, mid, mid]
            else:
                if mid > b[2]: b[2] = mid
                if mid < b[3]: b[3] = mid
                b[4] = mid
            n += 1
            if n % 10_000_000 == 0:
                print(f"  ... {n/1e6:.1f}M ticks ({time.time()-t0:.0f}s)")
    print(f"[BUILD] {n/1e6:.1f}M ticks -> m5={len(m5)} in {time.time()-t0:.0f}s")
    with open(out_m5, "w") as o:
        o.write("ts,o,h,l,c\n")
        for k in sorted(m5):
            b = m5[k]
            o.write(f"{b[0]},{b[1]:.4f},{b[2]:.4f},{b[3]:.4f},{b[4]:.4f}\n")
    print(f"[OUT] {out_m5}")

if __name__ == "__main__":
    main()
