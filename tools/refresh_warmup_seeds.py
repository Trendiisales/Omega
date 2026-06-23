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
    "H4":  ("4 hours", "730 D"),
    "D1":  ("1 day",   "5 Y"),
}
GOLD_TFS  = ["M5", "M15", "M30", "H1", "H4", "D1"]   # XAUUSD_* warmups (from MGC proxy)
INDEX = {   # engine warmup symbol -> (CFD symbol candidates), TFs
    "NAS100": (["IBUS-TECH-100","IBUST100","NDX"],        ["H1","M30","M15","M5"]),
    "US500":  (["IBUS-500","SPX","IBUS500"],              ["H1","D1"]),
    "GER40":  (["IBDE-40","DAX","IBDE40"],                ["H1","H4","M30","M15","D1"]),
    "UK100":  (["IBGB-100","UKX","IBGB100"],              ["M30","M240","D1"]),
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
    return ib.reqHistoricalData(con, "", durationStr=dur, barSizeSetting=bs,
                                whatToShow="TRADES", useRTH=False, timeout=120)

ib = IB()
try:
    ib.connect("127.0.0.1", PORT, clientId=91, timeout=20)
except Exception as e:
    print(f"[refresh] cannot connect IB @ {PORT}: {e}"); sys.exit(2)

ok, fail = [], []

# ---- GOLD (XAUUSD_*) from MGC COMEX future ----
try:
    mgc = ContFuture("MGC", "COMEX", "USD"); ib.qualifyContracts(mgc)
    for tf in GOLD_TFS:
        try:
            bars = pull(ib, mgc, tf)
            if bars and len(bars) > 10:
                # H4 uses ms ts (engine_seed_helpers/GoldD1TrendState); others ms too (RegimeState handles both)
                n = write_csv(f"{SEED_DIR}/warmup_XAUUSD_{tf}.csv", bars, ms=True)
                ok.append(f"XAUUSD_{tf}({n})")
            else:
                fail.append(f"XAUUSD_{tf}(no bars)")
        except Exception as e:
            fail.append(f"XAUUSD_{tf}({e})")
except Exception as e:
    fail.append(f"MGC-qualify({e})")

# ---- the tsmom regime seed (gold_regime) is H1 -> mirror warmup_XAUUSD_H1 ----
try:
    if os.path.exists(f"{SEED_DIR}/warmup_XAUUSD_H1.csv"):
        import shutil; shutil.copyfile(f"{SEED_DIR}/warmup_XAUUSD_H1.csv", f"{SEED_DIR}/tsmom_warmup_H1.csv")
        ok.append("tsmom_warmup_H1(<-XAUUSD_H1)")
except Exception as e:
    fail.append(f"tsmom_copy({e})")

# ---- INDICES (best-effort CFD) ----
for sym, (cands, tfs) in INDEX.items():
    con = None
    for cs in cands:
        try:
            c = Contract(secType="CFD", symbol=cs, exchange="SMART", currency="USD")
            d = ib.reqContractDetails(c)
            if d: con = d[0].contract; break
        except Exception: continue
    if con is None:
        fail.append(f"{sym}(no CFD qualified)"); continue
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
