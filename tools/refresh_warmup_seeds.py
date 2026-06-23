#!/usr/bin/env python3
# SEED-REFRESH TASK (2026-06-23) — the CURE for the stale warm-seed corpus.
#
# WHY: seed_freshness_audit.py found 28/44 active warm-seeds months stale -> engines/gates boot
# with a price view detached from reality (gold_regime + gold_d1_trend bought gold into a
# downtrend). This task regenerates the warmup CSVs from CURRENT data so every restart seeds
# fresh. Schedule it (Task Scheduler / launchd) DAILY; VERIFY_STARTUP CHECK 18 then stays green.
#
# DESIGN: pull from IBKR (gateway @ 127.0.0.1:PORT, ib_insync) and write
# phase1/signal_discovery/warmup_<SYM>_<TF>.csv in each engine's expected format.
#   - GOLD (XAUUSD_*): MGC COMEX future is the trend/regime proxy (basis negligible for EMA
#     direction/slope). This is the HIGH-VALUE set -- it feeds the two gold gates that bled.
#   - INDICES: best-effort CFD qualify (SMART). Symbols that don't qualify are SKIPPED + logged
#     (not fatal) -- the freshness audit then still flags them so nothing is silently missed.
# Each pull is independent + wrapped in try/except: one failure never aborts the rest.
#
# Run:  python tools/refresh_warmup_seeds.py [port]   (default 4001)
import sys, os, datetime
try:
    from ib_insync import IB, ContFuture, Contract
except Exception as e:
    print(f"[refresh] ib_insync import failed: {e}"); sys.exit(2)

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4001
SEED_DIR = "phase1/signal_discovery"
os.makedirs(SEED_DIR, exist_ok=True)

# (engine TF label) -> (IB barSizeSetting, durationStr). Durations chosen to cover EMA200+slope
# warmups (regime needs ~300 H1; D1 trend needs ~250 D1 worth of H4).
TF = {
    "M5":  ("5 mins",  "10 D"),
    "M15": ("15 mins", "20 D"),
    "M30": ("30 mins", "40 D"),
    "H1":  ("1 hour",  "90 D"),
    "M240":("4 hours", "300 D"),  # IB caps 4h-bar duration; >~1Y returns nothing
    "H4":  ("4 hours", "300 D"),
    "D1":  ("1 day",   "5 Y"),
}
GOLD_TFS  = ["M5", "M15", "M30", "H1", "H4", "D1"]   # XAUUSD_* warmups (from MGC proxy)
# Index FUTURES (ContFuture) -- reliable like NQ/MGC, basis negligible for trend/EMA seeds.
# (exchange, ib_symbol). engine warmup symbol -> contract + the TFs its engines seed.
INDEX = {
    "NAS100": (("CME","NQ"),     ["H1","M30","M15","M5"]),
    "US500":  (("CME","ES"),     ["H1","D1"]),
    "GER40":  (("EUREX","DAX"),  ["H1","H4","M30","M15","D1"]),
    "UK100":  (("ICEEU","Z"),    ["M30","M240","D1"]),
    "DJ30":   (("CBOT","YM"),    ["D1"]),
    "ESTX50": (("EUREX","ESTX50"),["D1","M5"]),
}

def write_csv(path, bars, ms=True):
    with open(path, "w") as f:
        f.write("ts,o,h,l,c\n")
        for b in bars:
            t = b.date
            ep = int(t.timestamp()) if hasattr(t, "timestamp") else int(datetime.datetime(t.year,t.month,t.day).timestamp())
            if ms: ep *= 1000
            f.write(f"{ep},{b.open},{b.high},{b.low},{b.close}\n")
    return len(bars)

def pull(ib, con, tf):
    bs, dur = TF[tf]
    # timeout 120->45 (S-2026-06-24): a stalled/empty IBKR pull was costing up to 2min EACH and
    # the refresh runs every deploy (committed warmups are re-staled by git reset). 45s is ample
    # for a normal hist pull; a pull that can't return in 45s is skipped (seed kept) not waited on.
    return ib.reqHistoricalData(con, "", durationStr=dur, barSizeSetting=bs,
                                whatToShow="TRADES", useRTH=False, timeout=45)

ib = IB()
try:
    ib.connect("127.0.0.1", PORT, clientId=91, timeout=20)
except Exception as e:
    print(f"[refresh] cannot connect IB @ {PORT}: {e}"); sys.exit(2)

ok, fail = [], []

# ---- GOLD (XAUUSD_*) from MGC COMEX future ----
def agg_h4_from_h1(h1_bars, out_path):
    # group H1 into 4h buckets (UTC 0/4/8/.. boundaries); o=first h=max l=min c=last
    buckets = {}
    for b in h1_bars:
        ep = int(b.date.timestamp())
        k = (ep // 14400) * 14400
        if k not in buckets: buckets[k] = [b.open, b.high, b.low, b.close]
        else:
            buckets[k][1] = max(buckets[k][1], b.high); buckets[k][2] = min(buckets[k][2], b.low); buckets[k][3] = b.close
    rows = sorted(buckets.items())
    with open(out_path, "w") as f:
        f.write("ts,o,h,l,c\n")
        for k, (o,h,l,c) in rows: f.write(f"{k*1000},{o},{h},{l},{c}\n")
    return len(rows)
try:
    mgc = ContFuture("MGC", "COMEX", "USD"); ib.qualifyContracts(mgc)
    h1_gold = None; m30_gold = None
    for tf in GOLD_TFS:
        try:
            bars = pull(ib, mgc, tf)
            if bars and len(bars) > 10:
                n = write_csv(f"{SEED_DIR}/warmup_XAUUSD_{tf}.csv", bars, ms=True)
                ok.append(f"XAUUSD_{tf}({n})")
                if tf == "H1":  h1_gold  = bars
                if tf == "M30": m30_gold = bars
            elif tf == "H4" and h1_gold:
                n = agg_h4_from_h1(h1_gold, f"{SEED_DIR}/warmup_XAUUSD_H4.csv")  # MGC 4h pull empty -> aggregate
                ok.append(f"XAUUSD_H4(agg<-H1,{n})")
            else:
                fail.append(f"XAUUSD_{tf}(no bars)")
        except Exception as e:
            fail.append(f"XAUUSD_{tf}({e})")
    # ensure H4 exists even if it errored above
    if h1_gold and not any(s.startswith("XAUUSD_H4") for s in ok):
        try:
            n = agg_h4_from_h1(h1_gold, f"{SEED_DIR}/warmup_XAUUSD_H4.csv"); ok.append(f"XAUUSD_H4(agg<-H1,{n})")
        except Exception as e:
            fail.append(f"XAUUSD_H4-agg({e})")
    # S-2026-06-24: also regenerate g_mgc_volbrk's seed data files (data/mgc_{h1,30m}_hist.csv).
    # These are SEPARATE from the XAUUSD_* warmups (MgcVolBreakout reads them via seed_*_from_csv)
    # and were the last 2 stale seeds the deploy refresh didn't cover. Format = the engine's:
    # "ts,o,h,l,c,v" with ts in SECONDS (NOT ms). MGC M30+H1 bars are already pulled above.
    def write_mgc_hist(path, bars):
        with open(path, "w") as f:
            f.write("ts,o,h,l,c,v\n")
            for b in bars:
                f.write(f"{int(b.date.timestamp())},{b.open},{b.high},{b.low},{b.close},{getattr(b,'volume',0) or 0}\n")
        return len(bars)
    if h1_gold:
        try:    ok.append(f"mgc_h1_hist({write_mgc_hist('data/mgc_h1_hist.csv', h1_gold)})")
        except Exception as e: fail.append(f"mgc_h1_hist({e})")
    if m30_gold:
        try:    ok.append(f"mgc_30m_hist({write_mgc_hist('data/mgc_30m_hist.csv', m30_gold)})")
        except Exception as e: fail.append(f"mgc_30m_hist({e})")
except Exception as e:
    fail.append(f"MGC-qualify({e})")

# ---- the tsmom regime seed (gold_regime) is H1 -> mirror warmup_XAUUSD_H1 ----
try:
    if os.path.exists(f"{SEED_DIR}/warmup_XAUUSD_H1.csv"):
        import shutil; shutil.copyfile(f"{SEED_DIR}/warmup_XAUUSD_H1.csv", f"{SEED_DIR}/tsmom_warmup_H1.csv")
        ok.append("tsmom_warmup_H1(<-XAUUSD_H1)")
except Exception as e:
    fail.append(f"tsmom_copy({e})")

# ---- INDICES (index futures -- reliable like NQ/MGC) ----
for sym, ((exch, ibsym), tfs) in INDEX.items():
    con = None
    try:
        c = ContFuture(ibsym, exch); ib.qualifyContracts(c)
        if getattr(c, "conId", 0): con = c
    except Exception as e:
        fail.append(f"{sym}(qualify {e})"); continue
    if con is None:
        fail.append(f"{sym}(no future {exch}:{ibsym})"); continue
    for tf in tfs:
        try:
            bars = pull(ib, con, tf)
            if bars and len(bars) > 10:
                n = write_csv(f"{SEED_DIR}/warmup_{sym}_{tf}.csv", bars, ms=True)
                ok.append(f"{sym}_{tf}({n})")
            else:
                fail.append(f"{sym}_{tf}(no bars)")
        except Exception as e:
            fail.append(f"{sym}_{tf}({e})")

ib.disconnect()
print("===== SEED REFRESH =====")
print(f"refreshed ({len(ok)}): " + ", ".join(ok))
print(f"skipped/failed ({len(fail)}): " + ", ".join(fail))
print("\nRun seed_freshness_audit.py to confirm the refreshed seeds are now fresh.")
sys.exit(0 if ok else 1)
