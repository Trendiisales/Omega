#!/usr/bin/env python3
"""refresh_daily_feeds.py — ZERO-STALE daily-feed refresher (yfinance, Mac-side, NO IB Gateway).

THE durable fix for recurring index/gold daily staleness. One script refreshes EVERY daily
feed the engines + monitor consume, from yfinance (reliable, Gateway-independent). Clones the
proven fetch_ndx.py contract per feed:
  * FRESHNESS GATE: refuse to write a source that is itself stale (keeps the cached file).
  * ATOMIC write (temp + os.replace) — a crash mid-write cannot corrupt the live file.
  * MERGE into existing history (full history preserved; only newest bars updated).
  * FAIL-LOUD per feed: a feed that can't refresh prints FAIL + the script exits non-zero so
    cron/monitor alarms. NEVER writes stale, NEVER silently passes.

Wire on Mac cron (e.g. hourly during market days). No Gateway, no clientId, not VPS — so it
cannot be killed by the IB API / port / scheduling failures that broke the old IBKR pullers.

  python3 refresh_daily_feeds.py            # refresh all; exit 0 = all fresh, 1 = >=1 stale/failed
"""
import sys, os, time, tempfile, datetime as dt

SESSION_OFFSET = 48600   # 13:30 UTC (NYSE open) — matches existing bar stamping
TICK = "/Users/jo/Tick"

# (yfinance ticker, target file, max_age_days, label)
FEEDS = [
    ("^NDX",   f"{TICK}/NDX_daily_2016_2026.csv",  4.0, "NDX"),
    ("^GSPC",  f"{TICK}/SPX_daily_2016_2026.csv",  4.0, "SPX"),
    ("^DJI",   f"{TICK}/DJ30_daily_2016_2026.csv", 4.0, "DJ30"),
    ("^GDAXI", f"{TICK}/GER40_daily_2016_2026.csv",4.0, "GER40"),
    ("GC=F",   f"{TICK}/2yr_XAUUSD_daily.csv",     4.0, "XAUUSD(GC=F proxy)"),
]

try:
    import yfinance as yf
except Exception as e:
    print(f"[refresh_daily] FATAL yfinance import: {e}", flush=True); sys.exit(1)

def col(df, name):
    for c in df.columns:
        cc = c[0] if isinstance(c, tuple) else c
        if cc == name: return df[c]
    return None

def refresh_one(ticker, path, max_age, label):
    try:
        df = yf.download(ticker, period="60d", interval="1d", auto_adjust=False, progress=False)
    except Exception as e:
        print(f"[refresh_daily] {label} FAIL download ({e}) — kept cache"); return False
    if df is None or df.empty:
        print(f"[refresh_daily] {label} FAIL no rows — kept cache"); return False
    o, h, l, c = col(df,"Open"), col(df,"High"), col(df,"Low"), col(df,"Close")
    if any(x is None for x in (o,h,l,c)):
        print(f"[refresh_daily] {label} FAIL bad columns {list(df.columns)} — kept cache"); return False
    fresh = {}
    for i, idx in enumerate(df.index):
        d = idx.date()
        ts = int(dt.datetime(d.year,d.month,d.day,tzinfo=dt.timezone.utc).timestamp()) + SESSION_OFFSET
        try: ov,hv,lv,cv = float(o.iloc[i]),float(h.iloc[i]),float(l.iloc[i]),float(c.iloc[i])
        except Exception: continue
        if cv > 0 and hv >= lv: fresh[ts] = (ov,hv,lv,cv)
    if not fresh:
        print(f"[refresh_daily] {label} FAIL no valid bars — kept cache"); return False
    newest = max(fresh); age = (time.time()-newest)/86400.0
    if age > max_age:
        nd = dt.datetime.fromtimestamp(newest,dt.timezone.utc).date()
        print(f"[refresh_daily] {label} FAIL source stale (newest {nd}, {age:.1f}d > {max_age}d) — NOT overwriting"); return False
    rows = {}
    if os.path.exists(path):
        with open(path) as fh:
            for ln in fh:
                p = ln.strip().split(",")
                if len(p) >= 5 and p[0].lstrip("-").isdigit(): rows[int(p[0])] = tuple(p[1:5])
    for ts,(ov,hv,lv,cv) in fresh.items():
        rows[ts] = (f"{ov:.2f}",f"{hv:.2f}",f"{lv:.2f}",f"{cv:.2f}")
    ordered = sorted(rows)
    try:
        fd = tempfile.NamedTemporaryFile("w", delete=False, dir=os.path.dirname(path), newline="")
        for ts in ordered:
            r = rows[ts]; fd.write(f"{ts},{r[0]},{r[1]},{r[2]},{r[3]}\n")
        fd.close(); os.replace(fd.name, path)
    except Exception as e:
        try: os.unlink(fd.name)
        except Exception: pass
        print(f"[refresh_daily] {label} FAIL write ({e}) — original untouched"); return False
    nd = dt.datetime.fromtimestamp(newest,dt.timezone.utc).date()
    print(f"[refresh_daily] {label} OK — {len(ordered)} bars, newest {nd} ({age:.1f}d)"); return True

def main():
    ok_all = True
    for t,p,a,lbl in FEEDS:
        if not refresh_one(t,p,a,lbl): ok_all = False
    print(f"[refresh_daily] {'ALL FRESH' if ok_all else 'ONE OR MORE STALE/FAILED — ALARM'}", flush=True)
    sys.exit(0 if ok_all else 1)

if __name__ == "__main__":
    main()
