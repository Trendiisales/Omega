#!/usr/bin/env python3
# ============================================================================
# TIER-2 5m BEAR-DATA PULLER + SCOPING (S-2026-06-24)
# ============================================================================
# GOAL: feed the EXISTING faithful harness (bigcap_momo_faithful.cpp, which drives
# the REAL PumpScalpManager with production config) a MULTI-YEAR 5m bin spanning a
# real equity bear (2018Q4 / 2020-covid / 2022), so the live engine's INTRADAY exits
# (ATR-trail/BE/ride) get a faithful bear test. Tier-1 (daily proxy) validated the
# GATE; Tier-2 validates the EXIT chassis + real microstructure/cost. The harness
# already exists — this only acquires the data + writes the .bin loader expects.
#
# ---------------------------------------------------------------------------
# SCOPING DECISION (two data paths)
# ---------------------------------------------------------------------------
# PATH A — POLYGON.IO  (RECOMMENDED for the bear test)
#   + 5m aggregates back to 2003 -> covers 2018Q4 + 2020 + 2022 in ONE pull.
#   + REST bulk: /v2/aggs/ticker/{T}/range/5/minute/{from}/{to}, 50k bars/req, paginated.
#   + Adjusted or raw; survivorship-safe if you pull the as-was universe per date.
#   - Paid: Stocks Starter ~$29/mo (15m delayed, 5y history, unlimited calls) or
#     Developer ~$79-199/mo (full history, more calls). $29 tier is enough — history,
#     not realtime, is what we need. One month, pull, cancel.
#   COST TO ANSWER THE BEAR QUESTION: ~$29, ~1 evening of pulling 500 names x 3 bears.
#
# PATH B — IBKR reqHistoricalData (we already have the gateway)
#   + Free with the held entitlement (no new sub) IF historical equity 5m is permitted.
#   - HARD CAP: ~6 months of 5m per request; full multi-year = many chunked requests.
#   - PACING: ~60 historical reqs / 10 min -> 500 names x several chunks each = hours,
#     careful throttling, and it shares the ONE live login (clientId contention).
#   - The SAME entitlement that froze live SQF quotes may gate equity historical too
#     (needs a probe to confirm 5m STK historical returns bars, not err 162/354).
#   COST: $0 but slow + operational risk on the shared prod gateway.
#
# RECOMMENDATION: PATH A (Polygon $29). The bear test is a ONE-OFF; $29 buys 2018+2020+
# 2022 5m in one clean pull with zero prod-gateway risk. Reserve PATH B only if Polygon
# is off the table — and probe equity-historical entitlement FIRST (see ibkr_probe note).
#
# IBKR FEASIBILITY PROBE (run on VPS, read-only) before committing to PATH B:
#   reqHistoricalData(AAPL STK SMART, durationStr="2 M", barSizeSetting="5 mins",
#                     whatToShow="TRADES", useRTH=1, formatDate=2)
#   -> bars back = OK; err 162/165/354 = entitlement gate (then PATH A only).
# ============================================================================
import os, sys, time, csv, urllib.request, json

UNIVERSE = "pump/bigcap_universe.txt"
OUT_BIN  = "/tmp/bigcap_bear_5m.bin"   # format the faithful harness loader expects
# Bear windows to pull (UNIX-friendly date strings); pull each + the bull tail for WF.
WINDOWS  = [("2018-09-01","2019-01-31"),   # 2018Q4 selloff
            ("2020-01-01","2020-06-30"),   # covid crash + V
            ("2021-11-01","2023-01-31")]   # 2022 bear (full)

def load_universe():
    with open(UNIVERSE) as f:
        return [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]

# ---- PATH A: Polygon 5m aggregates ----
def polygon_pull(sym, frm, to, key):
    url=(f"https://api.polygon.io/v2/aggs/ticker/{sym}/range/5/minute/{frm}/{to}"
         f"?adjusted=true&sort=asc&limit=50000&apiKey={key}")
    bars=[]
    while url:
        with urllib.request.urlopen(url, timeout=30) as r:
            j=json.load(r)
        for b in j.get("results",[]):
            # t=ms epoch, o/h/l/c, v
            bars.append((b["t"]//1000, b["o"], b["h"], b["l"], b["c"], b["v"]))
        nxt=j.get("next_url")
        url=(nxt+f"&apiKey={key}") if nxt else None
        time.sleep(0.2)
    return bars

def main():
    key=os.environ.get("POLYGON_API_KEY")
    if not key:
        print("Set POLYGON_API_KEY to pull (PATH A). This is the scoping skeleton — see header.")
        print("Universe:", len(load_universe()), "names. Windows:", WINDOWS)
        print("Next: subscribe Polygon Starter ($29), export key, rerun -> writes", OUT_BIN)
        print("Then: build bigcap_bear_5m.bin loader in bigcap_momo_faithful.cpp + run faithful BT.")
        return
    syms=load_universe()
    # ... bulk pull loop (paginated, throttled) -> aggregate to OUT_BIN ...
    print(f"PATH A: pulling {len(syms)} names x {len(WINDOWS)} windows from Polygon...")
    # (full pull loop intentionally left for the funded run; structure above is complete.)

if __name__=="__main__":
    main()
