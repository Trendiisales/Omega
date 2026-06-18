#!/usr/bin/env python3
# DATA-INTEGRITY GATE — certify a tick dataset BEFORE any edge research.
# Catches the failures that wasted 2026-06-15 (x1000 glitches, column swaps,
# dead spreads) AND the ones that wasted 2026-06-19 (a 367-day coverage hole +
# a 10x-downsampled file + a block-shuffled concat — all of which the OLD gate
# passed as "clean" because they were WARN-only).
#
# HARD REJECTS (exit 1): price x1000 glitch, bid/ask column swap, price-band
# miss, coverage hole > 10 days, any hour-plus backward timestamp jump
# (non-chronological file), or decimated tick spacing (downsampled, breaks bars).
#
# STREAMING: single pass, O(1) memory (sampled medians) — runs on 10GB+ files
# in minutes, not an OOM grind. A gate too slow/heavy to run is a gate that
# gets skipped.
#
# Usage: data_integrity_gate.py <file.csv> [expected_price_lo] [expected_price_hi]
# Auto-detects HISTDATA (YYYYMMDD HHMMSSmmm,bid,ask) and MS_TS (ts_ms,c1,c2).
import sys, statistics, datetime

def parse_ts_px(line, histdata):
    parts = line.split(',')
    if len(parts) < 3: return None
    if histdata:
        dt = parts[0].split(' ')
        if len(dt) < 2: return None
        try:
            ymd = int(dt[0]); hms = int(dt[1])
            y = ymd//10000; mo = (ymd//100)%100; d = ymd%100
            ms = hms%1000; t = hms//1000; h = t//10000; mi = (t//100)%100; s = t%100
            ts = int(datetime.datetime(y,mo,d,h,mi,s,ms*1000,tzinfo=datetime.timezone.utc).timestamp()*1000)
            return ts, float(parts[1]), float(parts[2])
        except Exception: return None
    else:
        try:
            ts = int(parts[0])
            if ts < 100000000000: ts *= 1000   # epoch SECONDS (10-digit) -> ms; 13-digit already ms
            return ts, float(parts[1]), float(parts[2])
        except Exception: return None

def main():
    path = sys.argv[1]
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else None
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else None

    n = 0; histdata = None; askfirst = False
    prev_ts = None; first_ts = None; last_ts = None
    max_gap_h = 0.0; nonmono = 0; max_back_h = 0.0
    neg_spr = 0; zero_spr = 0
    px_samp = []; spr_samp = []; dt_samp = []   # sampled for medians (every Nth)
    SAMP = 997  # prime stride

    with open(path, 'rb') as f:
        firstline = True
        for raw in f:
            try: line = raw.decode('ascii','ignore').strip()
            except Exception: continue
            if not line: continue
            c0 = line[0]
            if firstline and (c0 < '0' or c0 > '9'):
                lc = line.lower()
                askfirst = ('ask' in lc and 'bid' in lc and lc.index('ask') < lc.index('bid'))
                firstline = False; continue
            firstline = False
            if histdata is None:
                histdata = (' ' in line.split(',',1)[0])
            r = parse_ts_px(line, histdata)
            if r is None: continue
            ts, p1, p2 = r
            bid, ask = (p2, p1) if askfirst else (p1, p2)
            mid = (bid+ask)/2.0; spr = ask-bid
            n += 1
            if first_ts is None: first_ts = ts
            last_ts = ts
            if spr < 0: neg_spr += 1
            elif spr == 0: zero_spr += 1
            if prev_ts is not None:
                d = ts - prev_ts
                if d < 0:
                    nonmono += 1
                    bh = (-d)/3600000.0
                    if bh > max_back_h: max_back_h = bh
                else:
                    gh = d/3600000.0
                    if gh > max_gap_h: max_gap_h = gh
                    if 0 < d <= 3600000 and (n % 7 == 0): dt_samp.append(d)
            prev_ts = ts
            if n % SAMP == 0:
                px_samp.append(mid)
                if spr > 0: spr_samp.append(spr)

    if n < 1000:
        print(f"FAIL: only {n} rows parsed"); sys.exit(1)
    med = statistics.median(px_samp) if px_samp else 0.0
    medspr = statistics.median(spr_samp) if spr_samp else 0.0
    med_dt = statistics.median(dt_samp)/1000.0 if dt_samp else 0.0
    span_days = (last_ts-first_ts)/86400000.0

    fails = []; warns = []
    # 1. price magnitude / x1000 glitch (vs sampled median)
    glitch = sum(1 for m in px_samp if m > 3*med or (m > 0 and m < med/3))
    if glitch > len(px_samp)*0.0005: fails.append(f"PRICE GLITCH: {glitch}/{len(px_samp)} sampled ticks off >3x median {med:.2f} (x1000 bug?)")
    elif glitch > 0: warns.append(f"{glitch} sampled price outliers (>3x median)")
    # 2. expected band
    if lo and hi and not (lo <= med <= hi): fails.append(f"PRICE BAND: median {med:.2f} outside [{lo},{hi}]")
    # 3. spread / column order
    if neg_spr > n*0.01: fails.append(f"COLUMN ORDER: {neg_spr} negative spreads ({100*neg_spr/n:.1f}%) — bid/ask swapped?")
    if zero_spr > n*0.5: warns.append(f"{100*zero_spr/n:.0f}% zero spreads")
    if medspr > med*0.01: warns.append(f"wide median spread {medspr:.4f} ({1e4*medspr/med:.1f}bps)")
    # 4. chronology — HARD REJECT on structural reorder
    if max_back_h > 1.0:
        fails.append(f"OUT-OF-ORDER: {nonmono} backward ts, largest jump back {max_back_h:.0f}h — file not chronological")
    elif nonmono > n*0.001:
        warns.append(f"{nonmono} out-of-order timestamps (sub-hour)")
    # 5. coverage hole — HARD REJECT > 10d (legit weekends ~49h, holidays ~96h)
    if max_gap_h > 240: fails.append(f"COVERAGE HOLE: largest gap {max_gap_h:.0f}h ({max_gap_h/24:.1f}d) — missing data, not continuous")
    elif max_gap_h > 96: warns.append(f"largest gap {max_gap_h:.0f}h ({max_gap_h/24:.1f}d) — inspect (holiday?)")
    # 6. downsample / density — distinguish a BAR file (regular interval, legit) from a
    #    DECIMATED tick file (irregular sparse, breaks bar agg). A bar file has tightly
    #    clustered dt == the bar interval; a downsampled tick stream is irregular.
    regular = (sum(1 for d in dt_samp if abs(d/1000.0 - med_dt) <= 0.15*med_dt) / len(dt_samp)) if dt_samp and med_dt > 0 else 0.0
    BAR_INTERVALS = {60:'M1',300:'M5',900:'M15',1800:'M30',3600:'H1',14400:'H4',86400:'D1'}
    is_bar = med_dt >= 55 and regular >= 0.5
    if is_bar:
        lbl = next((v for k,v in BAR_INTERVALS.items() if abs(med_dt-k) <= 0.15*k), f'{med_dt:.0f}s')
        warns.append(f"BAR FILE ({lbl} bars, {100*regular:.0f}% regular) — not raw tick; gate validated continuity+prices only, not tick density")
    elif med_dt > 10.0:
        fails.append(f"DOWNSAMPLED: median tick spacing {med_dt:.1f}s, only {100*regular:.0f}% regular — decimated tick stream, not raw tick and not clean bars (breaks bar agg)")
    elif med_dt > 3.0:
        warns.append(f"sparse: median tick spacing {med_dt:.1f}s — confirm not downsampled")

    print(f"--- {path.split('/')[-1]} ---")
    print(f"  rows={n:,}  median_px={med:.3f}  median_spread={medspr:.5f}  span={span_days:.0f}d  median_tick_dt={med_dt:.2f}s  ask_first={askfirst}")
    print(f"  range: {datetime.datetime.fromtimestamp(first_ts/1000, datetime.timezone.utc):%Y-%m-%d} .. {datetime.datetime.fromtimestamp(last_ts/1000, datetime.timezone.utc):%Y-%m-%d}")
    for w in warns: print(f"  WARN: {w}")
    for fl in fails: print(f"  FAIL: {fl}")
    if fails: print("  => REJECTED\n"); sys.exit(1)
    print("  => CERTIFIED CLEAN\n"); sys.exit(0)

if __name__ == "__main__": main()
