#!/usr/bin/env python3
"""congress_trades_check.py -- daily congress-trading context feed (S-2026-07-23a).

Operator: fold the Kadoa congress-trading-monitor data into our checks.
VERIFIED (research agent, 2026-07-23): the full dataset the kadoa.com/congress site
uses is committed DAILY to the public repo as static JSON -- no API key, MIT code,
public-record data. We consume the 5,000-most-recent-filings slice:

    https://raw.githubusercontent.com/kadoa-org/congress-trading-monitor/main/public/data/trades.json

HONESTY / SCOPE (why this is a CHECK, not an engine): congress-only median
days_to_file = 27 (mean 36; STOCK Act allows 45 from transaction). Post-2012
academic evidence (NBER w26975; J.Pub.Econ 2022; Econ.Letters 2025 on NANC/KRUZ)
says the broad follow-the-filing signal is market/mega-cap beta after the lag.
Residual pockets: cluster-buys (multiple filers, same name, short window) and
FAST filers (days_to_file <= 10). We SURFACE those for the BIGCAP-45 universe;
nothing trades on this without a faithful filing-date backtest passing
BACKTEST_TRUTH first.

Output: data/congress_bigcap.json
  { "asof", "rows": N, "window_days", "names": { TICKER: {buys, sells, buy_usd_mid,
    sell_usd_mid, fast_buys, filers:[...]} }, "cluster_flags": [...], "fast_flags": [...] }

Run: daily (Mac cron / VPS task). Read-only GET; ~4.3 MB; fails soft (keeps last
good file, writes STALE flag into the json) so a GitHub hiccup can't break checks.
"""
import json, os, ssl, sys, urllib.request, datetime as dt
from collections import defaultdict

URL = "https://raw.githubusercontent.com/kadoa-org/congress-trading-monitor/main/public/data/trades.json"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "congress_bigcap.json")
WINDOW_D = 30          # filings window we aggregate
FAST_D   = 10          # days_to_file <= this = fast filer (the screenable pocket)
CLUSTER_MIN_FILERS = 3 # >=3 distinct congress filers buying same name in window

BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO WDC STX DD TPR BMY SWKS").split()

def main():
    today = dt.date.today()
    try:
        ctx = ssl.create_default_context()
        with urllib.request.urlopen(URL, timeout=60, context=ctx) as r:
            rows = json.load(r)
    except Exception as e:
        # fail-soft: stamp staleness into the existing file rather than dying
        try:
            with open(OUT) as f: prev = json.load(f)
        except Exception: prev = {}
        prev["stale_error"] = f"{today}: fetch failed: {e}"
        with open(OUT, "w") as f: json.dump(prev, f, indent=1)
        print(f"[congress] FETCH FAIL ({e}) -> kept last good, stamped stale")
        return 1

    cut = (today - dt.timedelta(days=WINDOW_D)).isoformat()
    agg = {t: dict(buys=0, sells=0, buy_usd_mid=0.0, sell_usd_mid=0.0,
                   fast_buys=0, filers=set()) for t in BIGCAP}
    n_used = 0
    for r in rows:
        if r.get("branch") != "congress": continue
        t = r.get("ticker")
        if t not in agg: continue
        fd = r.get("filing_date") or ""
        if fd < cut: continue
        mid = ((r.get("amount_range_low") or 0) + (r.get("amount_range_high") or 0)) / 2.0
        tt = (r.get("transaction_type") or "")
        a = agg[t]; n_used += 1
        if tt.startswith("Purchase"):
            a["buys"] += 1; a["buy_usd_mid"] += mid
            a["filers"].add(r.get("filer_name") or "?")
            d2f = r.get("days_to_file")
            if isinstance(d2f, (int, float)) and d2f <= FAST_D: a["fast_buys"] += 1
        elif tt.startswith("Sale"):
            a["sells"] += 1; a["sell_usd_mid"] += mid

    cluster, fast = [], []
    names = {}
    for t, a in agg.items():
        if a["buys"] + a["sells"] == 0: continue
        names[t] = dict(buys=a["buys"], sells=a["sells"],
                        buy_usd_mid=round(a["buy_usd_mid"]), sell_usd_mid=round(a["sell_usd_mid"]),
                        fast_buys=a["fast_buys"], filers=sorted(a["filers"]))
        if len(a["filers"]) >= CLUSTER_MIN_FILERS and a["buys"] > a["sells"]:
            cluster.append(dict(ticker=t, filers=len(a["filers"]), buys=a["buys"], sells=a["sells"]))
        if a["fast_buys"] >= 2:
            fast.append(dict(ticker=t, fast_buys=a["fast_buys"]))

    out = dict(asof=str(today), source=URL, rows=n_used, window_days=WINDOW_D,
               note=("CONTEXT ONLY -- median filing lag 27d; broad follow-signal is beta "
                     "post-2012 (NBER w26975). Nothing trades on this without a faithful "
                     "filing-date backtest."),
               names=names,
               cluster_flags=sorted(cluster, key=lambda x: -x["filers"]),
               fast_flags=sorted(fast, key=lambda x: -x["fast_buys"]))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f: json.dump(out, f, indent=1)
    print(f"[congress] {today}: {n_used} bigcap filings in {WINDOW_D}d window | "
          f"cluster={[c['ticker'] for c in out['cluster_flags']]} "
          f"fast={[c['ticker'] for c in out['fast_flags']]} -> {os.path.normpath(OUT)}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
