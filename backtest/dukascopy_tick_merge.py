#!/usr/bin/env python3
"""
Convert Dukascopy-style tick CSV (Time (EET),Ask,Bid,AskVolume,BidVolume)
into ts_ms,bid,ask format matching EURUSD_merged.csv.

EET = UTC+2 (winter) / UTC+3 (summer DST). Treat as UTC+2 (minor offset for
intraday signal logic; H1 buckets unaffected over months).
"""
import sys, os, calendar

def main():
    src = sys.argv[1]
    out = sys.argv[2]
    n = 0
    with open(src) as f, open(out, "w") as o:
        o.write("ts_ms,bid,ask\n")
        # skip header
        header = f.readline()
        for line in f:
            line = line.strip()
            if not line: continue
            parts = line.split(",")
            if len(parts) < 3: continue
            try:
                ts_str = parts[0]  # "2026.01.02 00:04:01.135"
                ask = parts[1]; bid = parts[2]
                date_part, time_part = ts_str.split(" ")
                yr,mo,da = [int(x) for x in date_part.split(".")]
                hh,mm,rest = time_part.split(":")
                ss, ms = rest.split(".")
                hh=int(hh); mm=int(mm); ss=int(ss); ms=int(ms)
                # EET = UTC+2 -> subtract 2 hours for UTC
                epoch = calendar.timegm((yr,mo,da,hh,mm,ss,0,0,0)) - 7200
                ts_ms = epoch*1000 + ms
                o.write(f"{ts_ms},{bid},{ask}\n")
                n += 1
            except (ValueError, IndexError):
                continue
    print(f"[CONVERT] {n} ticks -> {out}")

if __name__ == "__main__":
    main()
