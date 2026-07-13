#!/usr/bin/env python3
"""
VPS-NATIVE daily feed for the StockDayMoverBeFloorCompanion (S-2026-07-06).

Runs ON THE VPS (operator: "fix this properly to run on vps"). Pulls ONLY the 39
BIGCAP daily closes via yfinance and writes a SLIM wide CSV to
  C:\\Omega\\data\\rdagent\\sp500_long_close.csv
in the SAME format the companion polls: header ",SYM1,SYM2,..."; rows "YYYY-MM-DD,c1,c2,..".

Slim by design (39 cols x ~260 rows ~= 40KB, not the 15MB research file) so there is NO
space problem on the RAM-tight VPS -> nothing to purge. The full 565-name research CSV +
its Google-Drive archive stay on the Mac (where rclone->gdrive is configured); the VPS never
holds the 15MB blob. Scheduled by Windows Task Scheduler (install_vps_stockmover_task.ps1),
NOT cron (operator: "dont want crons caused issues").

Idempotent: extends the existing slim CSV (never overwrites with a thin/stale pull); the
companion's poller picks up only NEW dates. yfinance retries handle the yahoo 429 the VPS IP
sometimes gets; on total failure the last-good CSV is left untouched (book simply holds).
"""
from __future__ import annotations
import os, sys, time, datetime as dt, tempfile

# S-2026-07-13: roster MUST equal the ladder engine's registered names, else the
# unpulled names FREEZE (bars carried-forward stale) and RED the stock_daily_book
# content check. WDC STX DD TPR BMY SWKS were registered in the engine (45 names) but
# missing here (39) -> they were one-time-backfilled 07-10 then re-froze 2 days later.
# Added so all 45 refresh DAILY. Keep this list == the engine roster on any change.
BIGCAP = ("NVDA AMD AVGO MU MRVL SMCI ARM PLTR TSLA META NFLX CRWD SHOP COIN MSTR "
          "SNOW NOW PANW UBER ABNB DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL CRM ADBE "
          "IONQ RGTI QBTS ASTS RKLB NBIS CRWV ALAB CRDO "
          "WDC STX DD TPR BMY SWKS").split()

# VPS path (C:\Omega\data\rdagent). Overridable via env for a Mac dry-run.
OUT = os.environ.get("STOCKMOVER_CSV",
                     r"C:\Omega\data\rdagent\sp500_long_close.csv")
LOOKBACK = "1y"   # ~250 daily bars: plenty for the W=1 day-mover detector + a few WF checks


def pull():
    try:
        import yfinance as yf
    except ImportError:
        print("[vps-stockmover] yfinance not importable -> run: python -m pip install yfinance", flush=True)
        return None
    import pandas as pd
    frames = {}
    for i, sym in enumerate(BIGCAP):
        for attempt in range(3):
            try:
                df = yf.download(sym, period=LOOKBACK, interval="1d",
                                 auto_adjust=False, progress=False, threads=False)
                if df is not None and len(df) and "Close" in df:
                    frames[sym] = df["Close"].dropna()
                    break
            except Exception as e:
                print(f"[vps-stockmover] {sym} attempt {attempt+1} err {e}", flush=True)
            time.sleep(2.0 + attempt)      # backoff on 429
        time.sleep(0.4)
    if not frames:
        return None
    full = pd.concat(frames, axis=1)
    full.columns = [c if isinstance(c, str) else c[0] for c in full.columns]
    full.index = full.index.tz_localize(None)
    return full


def main():
    import pandas as pd
    fresh = pull()
    if fresh is None or fresh.empty:
        print("[vps-stockmover] pull FAILED/empty -> KEEP cached, not overwriting", flush=True)
        return 3
    today = dt.date.today()
    age = (today - fresh.index.max().date()).days
    ncov = int(fresh.iloc[-1].notna().sum())
    if age > 5 or ncov < 20:
        print(f"[vps-stockmover] STALE/THIN (age={age}d cov={ncov}/{len(BIGCAP)}) -> KEEP cached", flush=True)
        return 3
    # merge with any existing slim CSV so the history extends (newest cells win)
    if os.path.exists(OUT):
        try:
            old = pd.read_csv(OUT, index_col=0, parse_dates=True)
            fresh = pd.concat([old[~old.index.isin(fresh.index)], fresh]).sort_index()
        except Exception as e:
            print(f"[vps-stockmover] merge skip ({e})", flush=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fd = tempfile.NamedTemporaryFile("w", delete=False, dir=os.path.dirname(OUT), newline="")
    fresh.to_csv(fd); fd.close(); os.replace(fd.name, OUT)
    print(f"[vps-stockmover] wrote {fresh.shape[1]} names x {fresh.shape[0]} rows through "
          f"{fresh.index.max().date()} (cov={ncov}/{len(BIGCAP)}) -> {OUT}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
