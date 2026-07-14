#!/usr/bin/env python3
"""Build a single true-UTC XAUUSD spot 1m series covering the S-2026-07-14ba
sub-30m gold study window (warmup + 2026-01-14 .. 2026-07-14).

Sources (precedence = listed order; earlier file wins its span):
  1. /Users/jo/Tick/xau_m1_2024_2026.csv      ts sec TRUE UTC, o,h,l,c,spr
     (built from the dukascopy tick corpus; ends 2026-04-24 21:00 UTC)
  2. /Users/jo/Tick/xau_h2026mar_jun_m1.csv   clock is Europe/London local
     stored as epoch (EMPIRICALLY verified against source 1 on their 8-week
     overlap: close diff median 0.00 at shift 0 before the EU DST switch
     2026-03-29 and at -3600 after it; the documented duka_ticks_to_h1_fill
     EST-fixed convention gave RMSE 43pt and is NOT this file's clock).
     Converted piecewise London->UTC here, then re-verified per segment.
     (ends Fri 2026-06-26 close; the T202606 zip was pulled 06-28 so the
     last two June sessions are NOT in it)
  3. dukascopy 1m-candle tail (ts sec UTC, o,h,l,c mid=(BID+ASK)/2) fetched
     by backtest/fetch_xau_candles_daily.py (daily BID/ASK_candles_min_1.bi5;
     decode verified EXACT vs the histdata overlap on 2026-06-25/26: close
     absdiff median 0.0, max 0.0005). Covers 2026-06-27 .. 2026-07-14.

Output: /Users/jo/Tick/xau_1m_spliced_2024_2026.csv  (ts,o,h,l,c ; ts sec UTC)
Every input and the output must pass backtest/data_integrity_gate.py.
Usage: xau_1m_splice_2026h1.py <duka_tail_1m.csv> [out_csv]
"""
import sys, csv

SRC1 = "/Users/jo/Tick/xau_m1_2024_2026.csv"
SRC2 = "/Users/jo/Tick/xau_h2026mar_jun_m1.csv"

def load_m1(path):
    out = {}
    with open(path) as f:
        for line in f:
            p = line.strip().split(',')
            if len(p) < 5 or not p[0].isdigit(): continue
            out[int(p[0])] = (float(p[1]), float(p[2]), float(p[3]), float(p[4]))
    return out

def main():
    tail_ticks = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else \
        "/Users/jo/Tick/xau_1m_spliced_2024_2026.csv"

    b1 = load_m1(SRC1)
    b2raw = load_m1(SRC2)
    end1 = max(b1)
    print(f"src1 {len(b1)} bars, ends {end1}")

    # --- Europe/London clock -> UTC, piecewise at the EU DST switch ---------
    # London = GMT (UTC+0) until 2026-03-29 01:00 UTC, BST (UTC+1) after.
    # File clock jumps 01:00 -> 02:00 local at the switch; local ts >= 01:00
    # on 2026-03-29 are BST, so UTC = ts - 3600 for those.
    LON_DST = 1774746000  # 2026-03-29 01:00 (file-clock value at the switch)
    b2 = {}
    for ts, bar in b2raw.items():
        utc = ts - 3600 if ts >= LON_DST else ts
        b2[utc] = bar
    # verify each segment against source 1 on the overlap
    for name, lo, hi in (("pre-DST", 0, LON_DST), ("post-DST", LON_DST, 1 << 62)):
        common = 0; sq = 0.0
        for ts, bar in b2.items():
            src_clock = ts if ts < LON_DST else ts + 3600
            if not (lo <= src_clock < hi): continue
            o = b1.get(ts)
            if o: common += 1; sq += (bar[3] - o[3]) ** 2
        rmse = (sq / common) ** 0.5 if common else 1e9
        print(f"  {name}: overlap bars {common}, close RMSE {rmse:.4f}")
        if common < 5000 or rmse > 2.0:
            print(f"FATAL: London-clock conversion failed segment {name}")
            sys.exit(1)
    end2 = max(b2)

    # --- duka candle tail: already 1m bars (ts,o,h,l,c) ---
    b3 = load_m1(tail_ticks)
    print(f"src3 (duka tail) {len(b3)} 1m bars, "
          f"{min(b3) if b3 else 0}..{max(b3) if b3 else 0}")

    # seam check: src3 vs shifted src2 on their Jun-26 overlap
    ov = [ts for ts in b3 if ts in b2]
    if ov:
        sq = sum((b3[ts][3] - b2[ts][3]) ** 2 for ts in ov)
        print(f"seam2/3 overlap {len(ov)} bars, close RMSE {(sq/len(ov))**0.5:.4f}")

    # --- splice: src1 wins <= end1; src2 wins (end1, end2]; src3 after ---
    merged = dict(b3)
    for ts, bar in b2.items():
        if ts <= end2: merged[ts] = bar          # src2 over tail on overlap
    for ts, bar in b1.items():
        merged[ts] = bar                          # src1 always wins its span
    # drop any src2/src3 bars that predate src1 coverage start
    ts_sorted = sorted(merged)
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        for ts in ts_sorted:
            o, h, l, c = merged[ts]
            w.writerow([ts, f"{o:.3f}", f"{h:.3f}", f"{l:.3f}", f"{c:.3f}"])
    print(f"wrote {len(ts_sorted)} bars -> {out_path}")
    print(f"span {ts_sorted[0]}..{ts_sorted[-1]}")

if __name__ == "__main__":
    main()
