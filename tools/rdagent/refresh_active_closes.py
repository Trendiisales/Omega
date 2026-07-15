#!/usr/bin/env python3
"""Targeted close refresh for the ACTIVE basket names only (held + ranked).

WHY (S-2026-07-15): the paper book marks against sp500_long_close.csv. The full 744-name
refresh_close_yf.py bulk pull chokes on the Mac (DNS/thread throttling) and its anti-thin guard
KEEPS the file stale -> DELL froze at its 07-13 close (427.11) while it had risen to 457.54, so
the STOCK BASKET desk panel showed a phantom -$19 instead of the real gain. The book only needs
FRESH marks for the ~2 dozen names it holds/ranks, and a small per-name pull in a CLEAN process
(no 744 bulk before it) succeeds reliably. This runs BEFORE the bulk in run_basket_daily.sh so the
held/ranked marks are always current regardless of broad coverage. Non-destructive: only adds/
overwrites cells for the active names; every other name keeps its cached value.

    python3 refresh_active_closes.py
"""
import os, sys, json, time, datetime as dt
import pandas as pd
try:
    import yfinance as yf
except Exception:
    print("yfinance not importable"); sys.exit(2)

DATA=os.path.expanduser("~/Omega/data/rdagent")
CLOSE=f"{DATA}/sp500_long_close.csv"
LATEST=f"{DATA}/latest.json"; POS=f"{DATA}/factor_basket_positions.json"
STALE_DAYS=5

def active_names():
    names=set()
    try:
        pos=json.load(open(POS)); names |= {s for s,sh in pos.items() if float(sh)>0}
    except Exception: pass
    try:
        sig=json.load(open(LATEST)).get('signal',{})
        for b in sig.get('basket',[]):
            n=b.get('instrument') or b.get('sym')
            if n: names.add(n)
    except Exception: pass
    return sorted(names)

def one(sym):
    try:
        h=yf.Ticker(sym).history(period='10d', auto_adjust=True)
        c=h['Close'].dropna()
        return c if not c.empty else None
    except Exception:
        return None

def main():
    active=active_names()
    if not active:
        print("[active_yf] no active names (no positions/latest.json) -- nothing to do"); return 0
    if not os.path.exists(CLOSE):
        print(f"[active_yf] close file missing {CLOSE}"); return 2
    df=pd.read_csv(CLOSE, index_col=0, parse_dates=True)
    today=dt.date.today(); wrote=0
    for sym in active:
        c=one(sym)
        if c is None: print(f"[active_yf] {sym}: no data"); continue
        c.index=pd.to_datetime(c.index).tz_localize(None).normalize()
        if sym not in df.columns: df[sym]=pd.NA
        for ts,val in c.items():
            if ts not in df.index: df.loc[ts]=pd.NA
            df.at[ts,sym]=float(val); wrote+=1
        newest=c.index.max().date()
        print(f"[active_yf] {sym}: {c.iloc[-1]:.2f} @ {newest} ({'FRESH' if (today-newest).days<=STALE_DAYS else 'STALE'})")
        time.sleep(0.6)
    df=df.sort_index()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from close_csv_guard import assert_no_aliased_columns
    assert_no_aliased_columns(df, " active_yf")
    df.to_csv(CLOSE)
    print(f"[active_yf] wrote {wrote} cells for {len(active)} active names -> file newest {df.index.max().date()}")
    return 0

if __name__=='__main__':
    sys.exit(main())
