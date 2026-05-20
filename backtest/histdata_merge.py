#!/usr/bin/env python3
"""Merge histdata.com per-month tick CSVs (YYYYMMDD HHMMSSfff,bid,ask,0)
into a single ts_ms,bid,ask CSV per symbol."""
import sys, os, glob, calendar, time

def main():
    sym = sys.argv[1]
    base = f"/Users/jo/Tick/{sym}"
    out = f"/Users/jo/Tick/{sym}_merged.csv"
    files = sorted(glob.glob(f"{base}/HISTDATA_COM_ASCII_{sym}_T*/DAT_ASCII_{sym}_T_*.csv"))
    if not files: sys.exit(f"no files for {sym} in {base}")
    print(f"[{sym}] {len(files)} monthly files")
    t0 = time.time(); total = 0
    with open(out, "w") as o:
        o.write("ts_ms,bid,ask\n")
        for f in files:
            with open(f) as inp:
                for line in inp:
                    parts = line.strip().split(",")
                    if len(parts) < 3: continue
                    d = parts[0]
                    bid = parts[1]; ask = parts[2]
                    if len(d) < 18: continue
                    try:
                        yr=int(d[0:4]); mo=int(d[4:6]); da=int(d[6:8])
                        hh=int(d[9:11]); mm=int(d[11:13]); ss=int(d[13:15])
                        ms=int(d[15:18])
                        epoch = calendar.timegm((yr,mo,da,hh,mm,ss,0,0,0))
                        ts_ms = epoch*1000 + ms
                        o.write(f"{ts_ms},{bid},{ask}\n")
                        total += 1
                    except ValueError:
                        continue
            print(f"  done {os.path.basename(f)} total={total/1e6:.2f}M ({time.time()-t0:.0f}s)")
    print(f"[{sym}] DONE total={total/1e6:.2f}M -> {out} in {time.time()-t0:.0f}s")

if __name__ == "__main__":
    main()
