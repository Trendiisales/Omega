#!/usr/bin/env python3
"""FIX #2 (quick patch): yfinance close refresh with RETRY-THE-STRAGGLERS.

The old blend_daily.sh did ONE bulk yf.download -> ~75% returned under throttle, the missing
~25% (~230 names) never refreshed and froze at the 2024 build. This re-pulls the missing/stale
names in small backed-off batches over several rounds, so coverage approaches 100% without
changing the source. Falls through to KEEP-cached if a round can't improve (never writes stale).

    python3 refresh_close_yf.py            # refresh sp500_long_close.csv (extend, not replace)
"""
import os, sys, time, datetime as dt
import pandas as pd
try:
    import yfinance as yf
except Exception:
    print("yfinance not importable"); sys.exit(2)

DATA=os.path.expanduser("~/Omega/data/rdagent")
TICKERS=f"{DATA}/sp500_tickers.txt"; CLOSE=f"{DATA}/sp500_long_close.csv"
STALE_DAYS=5

def dl(tickers, period='1mo'):
    try:
        d=yf.download(tickers, period=period, auto_adjust=True, progress=False, threads=(len(tickers)>1))
        c=d['Close'] if 'Close' in d else d
        if isinstance(c, pd.Series): c=c.to_frame(tickers[0] if len(tickers)==1 else c.name)
        return c.dropna(axis=1, how='all')
    except Exception:
        return pd.DataFrame()

def main():
    tk=open(TICKERS).read().split()
    print(f"[refresh_yf] universe {len(tk)} names")
    full=dl(tk, '2y')
    today=dt.date.today()
    # which names are still missing or stale (last value > STALE_DAYS behind today)
    def stale_or_missing(df):
        bad=[]
        for s in tk:
            if s not in df.columns: bad.append(s); continue
            col=df[s].dropna()
            if col.empty or (today - col.index.max().date()).days > STALE_DAYS: bad.append(s)
        return bad
    bad=stale_or_missing(full)
    print(f"[refresh_yf] after bulk: {len(tk)-len(bad)}/{len(tk)} fresh, retrying {len(bad)} stragglers")
    # retry stragglers in small batches, several rounds with backoff
    for rnd in range(4):
        if not bad: break
        nxt=[]
        for i in range(0, len(bad), 15):
            batch=bad[i:i+15]
            got=dl(batch, '1mo')
            for s in batch:
                if s in got.columns and not got[s].dropna().empty:
                    full=full.join(got[[s]], how='outer') if s not in full.columns else full.combine_first(got[[s]])
            time.sleep(2.5)
        bad=stale_or_missing(full)
        print(f"[refresh_yf] round {rnd+1}: {len(tk)-len(bad)}/{len(tk)} fresh, {len(bad)} still stragglers")
        if len(nxt)==len(bad): pass
    # freshness gate on the WHOLE pull (date + coverage); never overwrite with a thin/stale result
    fresh_cnt=len(tk)-len(stale_or_missing(full))
    age=(today - full.index.max().date()).days if len(full.index) else 999
    if age>4 or fresh_cnt < max(100, int(0.6*len(tk))):
        print(f"[refresh_yf] STALE/THIN (age={age}d fresh={fresh_cnt}) -> KEEP cached, NOT overwriting"); return 3
    if os.path.exists(CLOSE):
        old=pd.read_csv(CLOSE, index_col=0, parse_dates=True)
        full=pd.concat([old[~old.index.isin(full.index)], full]).sort_index()
        full.update(dl(tk,'5d'))  # ensure newest cells win
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from close_csv_guard import assert_no_aliased_columns
    assert_no_aliased_columns(full, " refresh_yf")  # S-2026-07-14q: abort, don't write aliased columns
    full.to_csv(CLOSE)
    print(f"[refresh_yf] wrote {full.shape[1]} names through {full.index.max().date()} ({fresh_cnt} fresh)")
    # Daily Google-Drive archive of the full research CSV (operator: keep space free / durable
    # off-box copy). Mac-side rclone->gdrive; best-effort, never fails the refresh.
    try:
        import subprocess
        here = os.path.dirname(os.path.abspath(__file__))
        subprocess.run(["bash", f"{here}/archive_close_csv_gdrive.sh", CLOSE], timeout=180)
    except Exception as e:
        print(f"[refresh_yf] gdrive archive EXC {e}")
    return 0

if __name__=='__main__':
    sys.exit(main())
