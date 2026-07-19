#!/usr/bin/env python3
"""yfinance close refresh with RETRY-THE-STRAGGLERS + ACTIVE-NAME GUARANTEE.

The old blend_daily.sh did ONE bulk yf.download -> ~75% returned under throttle, the missing
~25% (~230 names) never refreshed and froze at the 2024 build. This re-pulls the missing/stale
names in small backed-off batches over several rounds, so coverage approaches 100% without
changing the source. Falls through to KEEP-cached if a round can't improve (never writes stale).

S-2026-07-15 ACTIVE-NAME GUARANTEE: the whole-universe freshness gate (needs ~60% of 744 names
fresh) KEPT the file stale whenever the bulk pull was throttled/thin (Mac DNS/thread failures:
"getaddrinfo() thread failed to start", "unable to open database file"). That froze DELL at its
07-13 close (427.11) while it had actually risen to 457.54 -> the STOCK BASKET desk panel showed
a phantom -$19 instead of the real +$300 on the held DELL lot. But the paper book only needs
FRESH marks for the names it HOLDS or RANKS (~2 dozen) -- a targeted per-name pull of just those
reliably succeeds even when the broad pull chokes. So: always refresh the active set individually,
and permit an EXTEND-write (non-destructive merge onto the cached file) when the active names are
fresh, even if broad coverage is thin. Names that stay stale keep their cached value and are
dropped by execute_basket's own per-name STALE_DAYS gate -- no ghost prices are introduced.

    python3 refresh_close_yf.py            # refresh sp500_long_close.csv (extend, not replace)
"""
import os, sys, time, json, datetime as dt
import pandas as pd
try:
    import yfinance as yf
except Exception:
    print("yfinance not importable"); sys.exit(2)

DATA=os.path.expanduser("~/Omega/data/rdagent")
TICKERS=f"{DATA}/sp500_tickers.txt"; CLOSE=f"{DATA}/sp500_long_close.csv"
LATEST=f"{DATA}/latest.json"; POS=f"{DATA}/factor_basket_positions.json"
STALE_DAYS=5

def dl(tickers, period='1mo'):
    try:
        d=yf.download(tickers, period=period, auto_adjust=True, progress=False, threads=(len(tickers)>1))
        c=d['Close'] if 'Close' in d else d
        if isinstance(c, pd.Series): c=c.to_frame(tickers[0] if len(tickers)==1 else c.name)
        return c.dropna(axis=1, how='all')
    except Exception:
        return pd.DataFrame()

def active_names():
    """Names the paper basket HOLDS or RANKS -- the only names whose mark MUST be fresh for a
    correct book. Held (factor_basket_positions.json shares>0) UNION ranking (latest.json
    signal.basket). A handful, so a targeted per-name pull reliably succeeds when the bulk pull
    is thin. Best-effort: any missing source just yields fewer names, never an error."""
    names=set()
    try:
        pos=json.load(open(POS))
        names |= {s for s,sh in pos.items() if float(sh)>0}
    except Exception: pass
    try:
        sig=json.load(open(LATEST)).get('signal',{})
        for b in sig.get('basket',[]):
            n=b.get('instrument') or b.get('sym')
            if n: names.add(n)
    except Exception: pass
    return sorted(names)

def _fresh(df, s, today):
    if s not in df.columns: return False
    col=df[s].dropna()
    return (not col.empty) and (today - col.index.max().date()).days <= STALE_DAYS

def _merge_in(full, got):
    """Merge pulled columns into `full` non-destructively: union rows, keep existing values,
    let the newly-pulled cells win where present."""
    for s in got.columns:
        if got[s].dropna().empty: continue
        if s in full.columns:
            full=full.combine_first(got[[s]]); full.update(got[[s]])
        else:
            full=full.join(got[[s]], how='outer')
    return full

def main():
    tk=open(TICKERS).read().split()
    print(f"[refresh_yf] universe {len(tk)} names")
    full=dl(tk, '2y')
    today=dt.datetime.now(dt.timezone.utc).date()  # UTC: Mac local NZ (+12) mislabels fresh closes stale
    def stale_or_missing(df, names=None):
        return [s for s in (names or tk) if not _fresh(df, s, today)]
    bad=stale_or_missing(full)
    print(f"[refresh_yf] after bulk: {len(tk)-len(bad)}/{len(tk)} fresh, retrying {len(bad)} stragglers")
    # retry stragglers in small batches, several rounds with backoff
    for rnd in range(4):
        if not bad: break
        for i in range(0, len(bad), 15):
            got=dl(bad[i:i+15], '1mo')
            full=_merge_in(full, got)
            time.sleep(2.5)
        bad=stale_or_missing(full)
        print(f"[refresh_yf] round {rnd+1}: {len(tk)-len(bad)}/{len(tk)} fresh, {len(bad)} still stragglers")

    # --- ACTIVE-NAME GUARANTEE (S-2026-07-15): reliably freshen held+ranked names one-by-one ---
    active=active_names()
    act_bad=stale_or_missing(full, active)
    for i in range(0, len(act_bad), 8):
        got=dl(act_bad[i:i+8], '1mo')
        full=_merge_in(full, got)
        time.sleep(1.5)
    active_fresh = bool(active) and all(_fresh(full, s, today) for s in active)
    still_stale=[s for s in active if not _fresh(full, s, today)]
    print(f"[refresh_yf] active set {len(active)} names: active_fresh={active_fresh}"
          + (f" (still stale: {still_stale})" if still_stale else ""))

    # freshness gate: never write a GHOST pull (newest date old) or a thin pull UNLESS the
    # active (held/ranked) names are fresh -- those are all the book actually marks against.
    fresh_cnt=len(tk)-len(stale_or_missing(full))
    age=(today - full.index.max().date()).days if len(full.index) else 999
    broad_ok = fresh_cnt >= max(100, int(0.6*len(tk)))
    if age>4 or not (broad_ok or active_fresh):
        print(f"[refresh_yf] STALE/THIN (age={age}d fresh={fresh_cnt} active_fresh={active_fresh}) -> KEEP cached, NOT overwriting"); return 3
    if not broad_ok:
        print(f"[refresh_yf] broad pull THIN (fresh={fresh_cnt}) but ACTIVE names fresh -> EXTEND-write "
              f"(held/ranked marks current; other names keep cached value, execute_basket per-name gate drops any still-stale)")
    if os.path.exists(CLOSE):
        old=pd.read_csv(CLOSE, index_col=0, parse_dates=True)
        # non-destructive: keep ALL cached columns/values, add full's rows/cells, full wins where present
        merged=old.reindex(old.index.union(full.index))
        for c in full.columns:
            merged[c]=full[c].combine_first(merged[c]) if c in merged.columns else full[c]
        full=merged.sort_index()
        full.update(dl(tk,'5d'))  # ensure newest cells win where the light pull succeeds
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
