#!/usr/bin/env python3
# Live MGC TRADES-bar producer (30m + 15m + 10m) for the MGC engine book.
# Loops: pulls recent CLOSED TRADES bars per grain (real OHLCV+volume, same
# source as the backtests), appends NEW closed bars to the per-grain CSV, and
# refreshes data/mgc_hvn.json (prior-day POC+HVN, from the 30m pull). The
# Omega engines poll these files. Distinct client-id (88) from the DOM
# bridge (99).
# Run as a scheduled/loop service:  python tools/mgc_live_bars.py [port] [period_s]
#
# S-2026-06-23 robustness fix: the connect() was OUTSIDE the loop with no reconnect,
# so any IB drop / failed initial connect crashed the script -> non-zero task result
# (0x800710E0, hidden by pythonw). Now self-healing: retry connect, detect drops and
# reconnect inside the loop, exit 0 on clean shutdown.
# S-2026-07-14: 15m + 10m grains added (GoldDon15m/GoldDon10m native-bar
# engines, sub-30m BIG GO). A missing/empty fine CSV self-backfills with a
# one-shot "2 W" pull (~1300 bars, covers engine warm_bars), then steady-state
# "2 D" like the 30m grain. HVN stays 30m-sourced.
import sys, json, time, os
from collections import defaultdict
from ib_async import IB, ContFuture

PORT   = int(sys.argv[1]) if len(sys.argv) > 1 else 4001
PERIOD = int(sys.argv[2]) if len(sys.argv) > 2 else 300
HVN    = "data/mgc_hvn.json"
BINS   = 30
# (barSizeSetting, csv path). 30m FIRST -- it also feeds the HVN write.
GRAINS = [
    ("30 mins", "data/mgc_30m_live.csv"),
    ("15 mins", "data/mgc_15m_live.csv"),
    ("10 mins", "data/mgc_10m_live.csv"),
]

def last_ts(csv):
    if not os.path.exists(csv): return 0
    try:
        with open(csv) as f: rows = f.read().splitlines()[1:]
        return int(rows[-1].split(",")[0]) if rows else 0
    except Exception: return 0

def write_hvn(bars):
    # prior COMPLETED UTC day profile
    byday = defaultdict(list)
    for b in bars: byday[int(b.date.timestamp()) // 86400].append(b)
    days = sorted(byday)
    if len(days) < 2: return
    g = byday[days[-2]]
    hi = max(x.high for x in g); lo = min(x.low for x in g)
    if hi <= lo: return
    bs = (hi - lo) / BINS; vb = [0.0]*BINS
    for x in g:
        i = min(BINS-1, max(0, int((x.close - lo)/bs))); vb[i] += (x.volume or 0)
    mx = max(vb) or 1; pi = vb.index(mx)
    poc = lo + bs*(pi+0.5)
    hvn = [round(lo+bs*(i+0.5),2) for i in range(BINS) if vb[i] >= 0.60*mx]
    json.dump({"poc": round(poc,2), "hvn": hvn, "basis": 0.0,
               "day": str(days[-2])}, open(HVN, "w"))

def connect():
    ib = IB()
    for attempt in range(1, 9):
        try:
            ib.connect("127.0.0.1", PORT, clientId=88, timeout=20)
            c = ContFuture("MGC", "COMEX", "USD"); ib.qualifyContracts(c)
            print(f"[mgc-live] connected port={PORT} (attempt {attempt})", flush=True)
            return ib, c
        except Exception as e:
            print(f"[mgc-live] connect attempt {attempt} failed: {e}", flush=True)
            try: ib.disconnect()
            except Exception: pass
            time.sleep(min(60, 5*attempt))
    return None, None

def main():
    for _, csv in GRAINS:
        if not os.path.exists(csv):
            open(csv, "w").write("ts,o,h,l,c,v\n")
    ib, c = connect()
    if ib is None:
        print("[mgc-live] FATAL: could not connect after retries -- exiting cleanly for task retry", flush=True)
        return 0   # clean exit; the 5-min task trigger retries
    print(f"[mgc-live] producing {[g[1] for g in GRAINS]} + {HVN} every {PERIOD}s", flush=True)
    while True:
        try:
            if not ib.isConnected():
                print("[mgc-live] connection lost -- reconnecting", flush=True)
                try: ib.disconnect()
                except Exception: pass
                ib, c = connect()
                if ib is None: return 0
            for bar_size, csv in GRAINS:
                # one-shot backfill on an empty fine CSV (engine warm cover);
                # steady-state stays the cheap 2-day tail pull.
                dur = "2 D" if last_ts(csv) > 0 else "2 W"
                bars = ib.reqHistoricalData(c, "", barSizeSetting=bar_size,
                                            durationStr=dur, whatToShow="TRADES",
                                            useRTH=False, timeout=60)
                if not bars: continue
                lt = last_ts(csv); appended = 0
                with open(csv, "a") as f:
                    for b in bars[:-1]:               # exclude the still-forming bar
                        ts = int(b.date.timestamp())
                        if ts > lt:
                            f.write(f"{ts},{b.open},{b.high},{b.low},{b.close},{b.volume or 0}\n"); appended += 1
                if bar_size == "30 mins": write_hvn(bars)
                if appended: print(f"[mgc-live] {bar_size}: +{appended} closed bars, last close {bars[-2].close}", flush=True)
        except Exception as e:
            print(f"[mgc-live] err: {e}", flush=True)
        ib.sleep(PERIOD)

if __name__ == "__main__":
    try:
        sys.exit(main() or 0)
    except (KeyboardInterrupt, SystemExit):
        print("[mgc-live] shutdown (clean exit 0)", flush=True)
        sys.exit(0)
    except Exception as e:
        print(f"[mgc-live] FATAL unhandled: {e} -- exit 0 for task retry", flush=True)
        sys.exit(0)   # never leave a non-zero task result; the trigger restarts us
