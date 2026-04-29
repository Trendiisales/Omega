#!/usr/bin/env python3
"""
post_cut_revalidate_all.py
==========================
Re-validates EVERY profitable cell from master_summary.parquet on the
post-2025-04 microstructure-regime cut.  Same signal-family conventions as
sim_lib.py:

  Family A (ATR TP/SL + max_hold):   donchian, ema_pullback
  Family B (RSI / BB exit + hard SL): rsi_revert, bollinger
  Family C (hold N bars + hard SL):   tsmom
  Family D (NY-close exit + hard SL): asian_break

All signal definitions are the canonical phase1 set:
  donchian:     period=20 breakout, cooldown=5
  ema_pullback: 9/21 EMA cross direction + close on opposite side of fast EMA
  bollinger:    period=20, sigma=2; long when close < lower band, short when > upper
  rsi_revert:   period=14; long when RSI < 30, short when RSI > 70
  tsmom:        20-bar return sign; long if positive, short if negative
  asian_break:  Asian range (22:00-06:00 UTC) breakout; exit at NY close or SL

Cost model: 0.05pt commission + 1 * avg_spread per trade.

Outputs: phase1/signal_discovery/POST_CUT_FULL_REPORT.md  +  per-cell parquets.
"""
from __future__ import annotations
import os, sys, time, math
from datetime import datetime, timezone
import numpy as np
import pandas as pd
import duckdb

OUT = "/sessions/hopeful-friendly-meitner/mnt/omega_repo/phase1/signal_discovery"
BARS_5S = os.path.join(OUT, "bars_5s.parquet")
REPORT = os.path.join(OUT, "POST_CUT_FULL_REPORT.md")

COMMISSION = 0.05
TF_MS = {"M15": 15*60*1000, "H1": 60*60*1000, "H2": 2*60*60*1000,
         "H4": 4*60*60*1000, "H6": 6*60*60*1000, "D1": 24*60*60*1000}


def resample(tf: str) -> pd.DataFrame:
    bar_ms = TF_MS[tf]
    out = os.path.join(OUT, f"bars_{tf}.parquet")
    if os.path.exists(out) and os.path.getsize(out) > 0:
        return pd.read_parquet(out)
    con = duckdb.connect()
    con.execute(f"""
      COPY (
        SELECT
          CAST(bar_ms // {bar_ms} AS BIGINT) * {bar_ms} AS bar_ms,
          FIRST(open_mid  ORDER BY bar_ms) AS open_mid,
          MAX(high_mid)                    AS high_mid,
          MIN(low_mid)                     AS low_mid,
          LAST(close_mid  ORDER BY bar_ms) AS close_mid,
          SUM(tick_n)                      AS tick_n,
          AVG(avg_spread)                  AS avg_spread
        FROM read_parquet('{BARS_5S}')
        GROUP BY bar_ms // {bar_ms}
        ORDER BY bar_ms
      )
      TO '{out}' (FORMAT 'parquet', COMPRESSION 'zstd');
    """)
    con.close()
    return pd.read_parquet(out)


def atr14(df: pd.DataFrame) -> np.ndarray:
    return (df["high_mid"] - df["low_mid"]).rolling(14, min_periods=7).mean().to_numpy()


# =============================================================================
# Signal generators
# =============================================================================
def sig_donchian(df, period=20, cooldown=5):
    prior_high = df["high_mid"].rolling(period, min_periods=period).max().shift(1)
    prior_low  = df["low_mid"].rolling(period, min_periods=period).min().shift(1)
    raw = pd.Series(0, index=df.index, dtype="int8")
    raw.loc[df["close_mid"] > prior_high] =  1
    raw.loc[df["close_mid"] < prior_low ] = -1
    arr = raw.to_numpy().copy()
    last = -10**9
    for i in range(len(arr)):
        if arr[i] != 0:
            if i - last < cooldown:
                arr[i] = 0
            else:
                last = i
    return pd.Series(arr, index=df.index, dtype="int8")


def sig_tsmom(df, lookback=20):
    ret_n = df["close_mid"].diff(lookback)
    sig = pd.Series(0, index=df.index, dtype="int8")
    sig.loc[ret_n > 0] = 1
    sig.loc[ret_n < 0] = -1
    return sig


def sig_ema_pullback(df, fast=9, slow=21, cooldown=5):
    """Long when slow EMA rising and close pulls back to/below fast EMA, then
    closes above fast EMA on next bar.  Short mirror."""
    ema_f = df["close_mid"].ewm(span=fast, adjust=False).mean()
    ema_s = df["close_mid"].ewm(span=slow, adjust=False).mean()
    slow_up   = ema_s > ema_s.shift(1)
    slow_down = ema_s < ema_s.shift(1)
    pull_lo   = (df["low_mid"]  <= ema_f) & (df["close_mid"] > ema_f)
    pull_hi   = (df["high_mid"] >= ema_f) & (df["close_mid"] < ema_f)
    raw = pd.Series(0, index=df.index, dtype="int8")
    raw.loc[slow_up   & pull_lo & (ema_f > ema_s)] =  1
    raw.loc[slow_down & pull_hi & (ema_f < ema_s)] = -1
    arr = raw.to_numpy().copy()
    last = -10**9
    for i in range(len(arr)):
        if arr[i] != 0:
            if i - last < cooldown:
                arr[i] = 0
            else:
                last = i
    return pd.Series(arr, index=df.index, dtype="int8")


def sig_bollinger(df, period=20, sigma=2.0, cooldown=2):
    """Long when close pierces lower band; short when close pierces upper band."""
    m = df["close_mid"].rolling(period, min_periods=period).mean()
    s = df["close_mid"].rolling(period, min_periods=period).std()
    upper = m + sigma * s
    lower = m - sigma * s
    raw = pd.Series(0, index=df.index, dtype="int8")
    raw.loc[df["close_mid"] < lower] =  1
    raw.loc[df["close_mid"] > upper] = -1
    arr = raw.to_numpy().copy()
    last = -10**9
    for i in range(len(arr)):
        if arr[i] != 0:
            if i - last < cooldown:
                arr[i] = 0
            else:
                last = i
    return pd.Series(arr, index=df.index, dtype="int8")


def sig_rsi_revert(df, period=14, hi=70, lo=30, cooldown=2):
    delta = df["close_mid"].diff()
    gain = delta.clip(lower=0).ewm(alpha=1/period, adjust=False).mean()
    loss = (-delta.clip(upper=0)).ewm(alpha=1/period, adjust=False).mean()
    rs = gain / loss.replace(0, np.nan)
    rsi = 100 - 100/(1+rs)
    raw = pd.Series(0, index=df.index, dtype="int8")
    raw.loc[rsi < lo] =  1
    raw.loc[rsi > hi] = -1
    arr = raw.to_numpy().copy()
    last = -10**9
    for i in range(len(arr)):
        if arr[i] != 0:
            if i - last < cooldown:
                arr[i] = 0
            else:
                last = i
    return pd.Series(arr, index=df.index, dtype="int8")


def sig_asian_break(df, asia_start=22, asia_end=6, cooldown=12):
    """Long when daily close breaks above prior Asian session high during
    the day session (post-06:00); short on lower break.  One signal per
    day per direction."""
    sig = pd.Series(0, index=df.index, dtype="int8")
    bar_ms = df["bar_ms"].to_numpy()
    high = df["high_mid"].to_numpy()
    low  = df["low_mid"].to_numpy()
    close = df["close_mid"].to_numpy()
    n = len(df)
    # Compute hour of each bar
    hours = ((bar_ms // 1000 // 3600) % 24).astype(np.int16)
    days  = (bar_ms // 86400000)
    # Per-day Asian range: asia_start (prev day) -> asia_end (current day)
    arr = np.zeros(n, dtype=np.int8)
    asian_hi = -np.inf
    asian_lo =  np.inf
    last_day = -1
    last_fire = -10**9
    for i in range(n):
        d = int(days[i])
        h = int(hours[i])
        if d != last_day:
            asian_hi = -np.inf
            asian_lo =  np.inf
            last_day = d
        # Accumulate Asian range during 22..23, 0..5
        if (h >= asia_start) or (h < asia_end):
            if high[i] > asian_hi: asian_hi = high[i]
            if low[i]  < asian_lo: asian_lo = low[i]
            continue
        # Day session 6..21 -> check break
        if asian_hi == -np.inf: continue
        if close[i] > asian_hi and (i - last_fire) >= cooldown:
            arr[i] =  1; last_fire = i
        elif close[i] < asian_lo and (i - last_fire) >= cooldown:
            arr[i] = -1; last_fire = i
    sig[:] = arr
    return sig


# =============================================================================
# Simulators (mid-price OHLC, intrabar approximation)
# =============================================================================
def sim_a(df, sig, sl_atr=1.0, tp_r=2.5, max_hold=30):
    h = df["high_mid"].to_numpy(); l = df["low_mid"].to_numpy()
    c = df["close_mid"].to_numpy(); o = df["open_mid"].to_numpy()
    sp = df["avg_spread"].to_numpy()
    bm = df["bar_ms"].to_numpy()
    a = atr14(df); n = len(df)
    side = sig.to_numpy()
    rows = []
    for i in np.flatnonzero(side != 0):
        if i + 1 >= n: continue
        atrv = a[i]
        if not np.isfinite(atrv) or atrv <= 0: continue
        s = int(side[i])
        e = o[i+1]
        sl = e - s * sl_atr * atrv
        tp = e + s * tp_r * sl_atr * atrv
        end = min(i+1+max_hold, n)
        ex_ix = ex_px = ex_r = None
        for j in range(i+1, end):
            hit_sl = (l[j] <= sl) if s==1 else (h[j] >= sl)
            hit_tp = (h[j] >= tp) if s==1 else (l[j] <= tp)
            if hit_sl: ex_ix=j; ex_px=sl; ex_r="SL_HIT"; break
            if hit_tp: ex_ix=j; ex_px=tp; ex_r="TP_HIT"; break
        if ex_ix is None:
            ex_ix = end-1; ex_px = c[ex_ix]; ex_r = "TIME_EXIT"
        gross = (ex_px - e) * s
        cost = COMMISSION + sp[i]
        rows.append({"entry_ms":int(bm[i+1]), "exit_ms":int(bm[ex_ix]), "side":s,
                     "entry_px":float(e), "exit_px":float(ex_px),
                     "exit_reason":ex_r, "gross":float(gross), "net":float(gross-cost)})
    return pd.DataFrame(rows)


def sim_c(df, sig, hold_bars=12, hard_sl_atr=3.0):
    h = df["high_mid"].to_numpy(); l = df["low_mid"].to_numpy()
    c = df["close_mid"].to_numpy(); o = df["open_mid"].to_numpy()
    sp = df["avg_spread"].to_numpy()
    bm = df["bar_ms"].to_numpy()
    a = atr14(df); n = len(df)
    side = sig.to_numpy()
    rows = []
    for i in np.flatnonzero(side != 0):
        if i + 1 >= n: continue
        atrv = a[i]
        if not np.isfinite(atrv) or atrv <= 0: continue
        s = int(side[i])
        e = o[i+1]
        sl = e - s * hard_sl_atr * atrv
        end = min(i+1+hold_bars, n)
        ex_ix = ex_px = ex_r = None
        for j in range(i+1, end):
            hit = (l[j] <= sl) if s==1 else (h[j] >= sl)
            if hit: ex_ix=j; ex_px=sl; ex_r="SL_HIT"; break
        if ex_ix is None:
            ex_ix = end-1; ex_px = c[ex_ix]; ex_r="TIME_EXIT"
        gross = (ex_px - e) * s
        cost = COMMISSION + sp[i]
        rows.append({"entry_ms":int(bm[i+1]), "exit_ms":int(bm[ex_ix]), "side":s,
                     "entry_px":float(e), "exit_px":float(ex_px),
                     "exit_reason":ex_r, "gross":float(gross), "net":float(gross-cost)})
    return pd.DataFrame(rows)


def sim_b_bb(df, sig, hard_sl_atr=1.5, max_hold=20):
    """Bollinger family-B: exit when close crosses BB midline OR hard SL."""
    h = df["high_mid"].to_numpy(); l = df["low_mid"].to_numpy()
    c = df["close_mid"].to_numpy(); o = df["open_mid"].to_numpy()
    sp = df["avg_spread"].to_numpy()
    bm = df["bar_ms"].to_numpy()
    a = atr14(df); n = len(df)
    mid = df["close_mid"].rolling(20, min_periods=20).mean().to_numpy()
    side = sig.to_numpy()
    rows = []
    for i in np.flatnonzero(side != 0):
        if i + 1 >= n: continue
        atrv = a[i]
        if not np.isfinite(atrv) or atrv <= 0: continue
        s = int(side[i])
        e = o[i+1]
        sl = e - s * hard_sl_atr * atrv
        end = min(i+1+max_hold, n)
        ex_ix = ex_px = ex_r = None
        for j in range(i+1, end):
            hit_sl = (l[j] <= sl) if s==1 else (h[j] >= sl)
            if hit_sl: ex_ix=j; ex_px=sl; ex_r="SL_HIT"; break
            mj = mid[j]
            if not np.isfinite(mj): continue
            hit_mid = (c[j] >= mj) if s==1 else (c[j] <= mj)
            if hit_mid: ex_ix=j; ex_px=c[j]; ex_r="BB_MID"; break
        if ex_ix is None:
            ex_ix = end-1; ex_px = c[ex_ix]; ex_r="TIME_EXIT"
        gross = (ex_px - e) * s
        cost = COMMISSION + sp[i]
        rows.append({"entry_ms":int(bm[i+1]), "exit_ms":int(bm[ex_ix]), "side":s,
                     "entry_px":float(e), "exit_px":float(ex_px),
                     "exit_reason":ex_r, "gross":float(gross), "net":float(gross-cost)})
    return pd.DataFrame(rows)


def sim_b_rsi(df, sig, hard_sl_atr=1.5, max_hold=20, period=14):
    """RSI family-B: exit when RSI crosses 50 OR hard SL."""
    h = df["high_mid"].to_numpy(); l = df["low_mid"].to_numpy()
    c = df["close_mid"].to_numpy(); o = df["open_mid"].to_numpy()
    sp = df["avg_spread"].to_numpy()
    bm = df["bar_ms"].to_numpy()
    a = atr14(df); n = len(df)
    delta = df["close_mid"].diff()
    gain = delta.clip(lower=0).ewm(alpha=1/period, adjust=False).mean()
    loss = (-delta.clip(upper=0)).ewm(alpha=1/period, adjust=False).mean()
    rs = gain / loss.replace(0, np.nan)
    rsi = (100 - 100/(1+rs)).to_numpy()
    side = sig.to_numpy()
    rows = []
    for i in np.flatnonzero(side != 0):
        if i + 1 >= n: continue
        atrv = a[i]
        if not np.isfinite(atrv) or atrv <= 0: continue
        s = int(side[i])
        e = o[i+1]
        sl = e - s * hard_sl_atr * atrv
        end = min(i+1+max_hold, n)
        ex_ix = ex_px = ex_r = None
        for j in range(i+1, end):
            hit_sl = (l[j] <= sl) if s==1 else (h[j] >= sl)
            if hit_sl: ex_ix=j; ex_px=sl; ex_r="SL_HIT"; break
            r = rsi[j]
            if not np.isfinite(r): continue
            hit_50 = (r >= 50) if s==1 else (r <= 50)
            if hit_50: ex_ix=j; ex_px=c[j]; ex_r="RSI_50"; break
        if ex_ix is None:
            ex_ix = end-1; ex_px = c[ex_ix]; ex_r="TIME_EXIT"
        gross = (ex_px - e) * s
        cost = COMMISSION + sp[i]
        rows.append({"entry_ms":int(bm[i+1]), "exit_ms":int(bm[ex_ix]), "side":s,
                     "entry_px":float(e), "exit_px":float(ex_px),
                     "exit_reason":ex_r, "gross":float(gross), "net":float(gross-cost)})
    return pd.DataFrame(rows)


def sim_d(df, sig, hard_sl_atr=1.0, ny_close_h=21):
    """Asian-break family-D: exit at NY close hour OR hard SL."""
    h = df["high_mid"].to_numpy(); l = df["low_mid"].to_numpy()
    c = df["close_mid"].to_numpy(); o = df["open_mid"].to_numpy()
    sp = df["avg_spread"].to_numpy()
    bm = df["bar_ms"].to_numpy()
    a = atr14(df); n = len(df)
    hours = ((bm // 1000 // 3600) % 24).astype(np.int16)
    side = sig.to_numpy()
    rows = []
    for i in np.flatnonzero(side != 0):
        if i + 1 >= n: continue
        atrv = a[i]
        if not np.isfinite(atrv) or atrv <= 0: continue
        s = int(side[i])
        e = o[i+1]
        sl = e - s * hard_sl_atr * atrv
        ex_ix = ex_px = ex_r = None
        # Walk until hard SL or NY close hour reached
        for j in range(i+1, n):
            hit_sl = (l[j] <= sl) if s==1 else (h[j] >= sl)
            if hit_sl: ex_ix=j; ex_px=sl; ex_r="SL_HIT"; break
            if hours[j] >= ny_close_h:
                ex_ix=j; ex_px=c[j]; ex_r="NY_CLOSE"; break
            # safety cap: 24 hr later
            if j - i > 96 * 4:  # generous
                ex_ix=j; ex_px=c[j]; ex_r="SAFETY_TIMEOUT"; break
        if ex_ix is None: continue
        gross = (ex_px - e) * s
        cost = COMMISSION + sp[i]
        rows.append({"entry_ms":int(bm[i+1]), "exit_ms":int(bm[ex_ix]), "side":s,
                     "entry_px":float(e), "exit_px":float(ex_px),
                     "exit_reason":ex_r, "gross":float(gross), "net":float(gross-cost)})
    return pd.DataFrame(rows)


# =============================================================================
# Cell registry (matches master_summary's 32 profitable rows)
# =============================================================================
PROFITABLE_CELLS = [
    # tsmom (family C)
    ("tsmom", "H1", "long",  "C", {"hold_bars":12, "hard_sl_atr":3.0}),
    ("tsmom", "H2", "long",  "C", {"hold_bars":12, "hard_sl_atr":3.0}),
    ("tsmom", "H4", "long",  "C", {"hold_bars":12, "hard_sl_atr":3.0}),
    ("tsmom", "H6", "long",  "C", {"hold_bars":12, "hard_sl_atr":3.0}),
    ("tsmom", "D1", "long",  "C", {"hold_bars":12, "hard_sl_atr":3.0}),
    # donchian (family A)
    ("donchian", "H1", "long",  "A", {"sl_atr":3.0, "tp_r":1.6667, "max_hold":30}),  # retuned
    ("donchian", "H2", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("donchian", "H4", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("donchian", "H4", "short", "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("donchian", "H6", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("donchian", "H6", "short", "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("donchian", "D1", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("donchian", "D1", "short", "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    # ema_pullback (family A)
    ("ema_pullback", "H1", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("ema_pullback", "H2", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("ema_pullback", "H4", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ("ema_pullback", "H6", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    # bollinger (family B-BB)
    ("bollinger", "H2", "long",  "B-BB", {"hard_sl_atr":1.5, "max_hold":20}),
    ("bollinger", "H4", "long",  "B-BB", {"hard_sl_atr":1.5, "max_hold":20}),
    ("bollinger", "H6", "long",  "B-BB", {"hard_sl_atr":1.5, "max_hold":20}),
    ("bollinger", "D1", "long",  "B-BB", {"hard_sl_atr":1.5, "max_hold":20}),
    ("bollinger", "D1", "short", "B-BB", {"hard_sl_atr":1.5, "max_hold":20}),
    # rsi_revert (family B-RSI)
    ("rsi_revert", "M15", "long",  "B-RSI", {"hard_sl_atr":1.5, "max_hold":20}),
    ("rsi_revert", "H1",  "long",  "B-RSI", {"hard_sl_atr":1.5, "max_hold":20}),
    ("rsi_revert", "H2",  "long",  "B-RSI", {"hard_sl_atr":1.5, "max_hold":20}),
    ("rsi_revert", "H4",  "long",  "B-RSI", {"hard_sl_atr":1.5, "max_hold":20}),
    ("rsi_revert", "H6",  "long",  "B-RSI", {"hard_sl_atr":1.5, "max_hold":20}),
    ("rsi_revert", "D1",  "long",  "B-RSI", {"hard_sl_atr":1.5, "max_hold":20}),
    ("rsi_revert", "D1",  "short", "B-RSI", {"hard_sl_atr":1.5, "max_hold":20}),
    # asian_break (family D)
    ("asian_break", "M15", "short", "D", {"hard_sl_atr":1.0}),
    ("asian_break", "H1",  "short", "D", {"hard_sl_atr":1.0}),
    ("asian_break", "H2",  "short", "D", {"hard_sl_atr":1.0}),
]


def make_signals(strategy: str, df: pd.DataFrame, direc: str) -> pd.Series:
    if strategy == "tsmom":         sig = sig_tsmom(df)
    elif strategy == "donchian":    sig = sig_donchian(df)
    elif strategy == "ema_pullback": sig = sig_ema_pullback(df)
    elif strategy == "bollinger":   sig = sig_bollinger(df)
    elif strategy == "rsi_revert":  sig = sig_rsi_revert(df)
    elif strategy == "asian_break": sig = sig_asian_break(df)
    else: raise ValueError(strategy)
    if direc == "long":
        sig = sig.where(sig > 0, 0)
    else:
        sig = sig.where(sig < 0, 0)
    return sig


def stats(df: pd.DataFrame, label: str) -> dict:
    n = len(df)
    if not n: return {"label": label, "n": 0}
    wins = (df["net"] > 0).sum()
    g_w = df.loc[df["net"]>0, "net"].sum() if wins else 0.0
    g_l = df.loc[df["net"]<0, "net"].sum() if (df["net"]<0).any() else 0.0
    return {"label": label, "n": n, "wr": wins/n,
            "gross": float(df["gross"].sum()), "net": float(df["net"].sum()),
            "avg": float(df["net"].mean()),
            "pf": (g_w/abs(g_l)) if g_l < 0 else float("inf"),
            "exits": dict(df["exit_reason"].value_counts().items())}


def main():
    t0 = time.time()
    tfs_needed = sorted(set(c[1] for c in PROFITABLE_CELLS))
    print(f"[+] resampling to: {tfs_needed}", flush=True)
    bars = {tf: resample(tf) for tf in tfs_needed}
    for tf, b in bars.items():
        print(f"    {tf}: {len(b):,} bars", flush=True)

    results = []
    for strat, tf, direc, fam, params in PROFITABLE_CELLS:
        b = bars[tf]
        sig = make_signals(strat, b, direc)
        if (sig != 0).sum() == 0:
            results.append({"label": f"{strat} {tf} {direc}", "n": 0})
            print(f"  {strat} {tf} {direc:<5s}  n=0 (no signals)", flush=True)
            continue
        if fam == "A":
            tr = sim_a(b, sig, **params)
        elif fam == "B-BB":
            tr = sim_b_bb(b, sig, **params)
        elif fam == "B-RSI":
            tr = sim_b_rsi(b, sig, **params)
        elif fam == "C":
            tr = sim_c(b, sig, **params)
        elif fam == "D":
            tr = sim_d(b, sig, **params)
        else:
            raise ValueError(fam)
        out = os.path.join(OUT, f"post_cut_{strat}_{tf}_{direc}.parquet")
        if len(tr): tr.to_parquet(out, compression="zstd")
        st = stats(tr, f"{strat} {tf} {direc}")
        results.append(st)
        print(f"  {strat:<14s} {tf:<3s} {direc:<5s}  n={st.get('n',0):>5,}  "
              f"wr={st.get('wr',0):.1%}  net=${st.get('net',0):+.0f}  "
              f"avg=${st.get('avg',0):+.2f}  pf={st.get('pf',0):.2f}", flush=True)

    # Report
    sorted_r = sorted([r for r in results if r.get("n",0)>0], key=lambda x: x["net"], reverse=True)
    pos = [r for r in sorted_r if r["net"] > 0]
    lines = [
        "# Post-2025-04 Re-validation -- ALL Profitable Cells",
        "",
        f"_Generated: {pd.Timestamp.utcnow().isoformat()}_",
        "",
        f"**Period:** 2025-04-01 -> 2026-04-01 (post-microstructure-regime cut)",
        f"**Cost:** 0.05pt commission + 1 * avg_spread (~0.69pt) per trade",
        f"**Cells tested:** {len(PROFITABLE_CELLS)} (all profitable in master_summary.parquet)",
        f"**Cells profitable post-cut:** {len(pos)} of {len(PROFITABLE_CELLS)}",
        "",
        "## Ranked by net pnl (post-cut)",
        "",
        "| rank | strategy | tf | dir | family | n | trades/day | WR | net | avg | pf |",
        "|---:|---|---|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    cell_lookup = {f"{c[0]} {c[1]} {c[2]}": (c[3], c[4]) for c in PROFITABLE_CELLS}
    for rk, r in enumerate(sorted_r, 1):
        fam, params = cell_lookup.get(r["label"], ("?", {}))
        parts = r["label"].split()
        lines.append(f"| {rk} | {parts[0]} | {parts[1]} | {parts[2]} | {fam} | "
                     f"{r['n']:,} | {r['n']/365:.1f} | {r['wr']:.1%} | "
                     f"${r['net']:+.0f} | ${r['avg']:+.2f} | {r['pf']:.2f} |")
    lines.append("")
    lines.append("## Cells that LOST in post-cut")
    lost = [r for r in sorted_r if r["net"] <= 0]
    if lost:
        lines.append("")
        for r in lost:
            lines.append(f"- **{r['label']}** -- n={r['n']:,}, net=${r['net']:+.0f} -- DEPRECATE")
    else:
        lines.append("")
        lines.append("None -- every profitable cell from master_summary remains profitable post-cut.")
    lines.append("")
    lines.append("## Combined portfolio if all profitable cells deployed")
    lines.append("")
    total_net = sum(r["net"] for r in pos)
    total_n = sum(r["n"] for r in pos)
    lines.append(f"- **Total trades:** {total_n:,} ({total_n/365:.1f}/day)")
    lines.append(f"- **Total net pnl:** ${total_net:+,.0f} on 1 unit over 365 days")
    lines.append(f"- **Bidirectional cells:** "
                 f"{sum(1 for r in pos if 'short' in r['label'])} short + "
                 f"{sum(1 for r in pos if 'long' in r['label'])} long")
    lines.append("")
    with open(REPORT, "w") as fh:
        fh.write("\n".join(lines))
    print(f"\n[+] wrote {REPORT}", flush=True)
    print(f"[+] total elapsed: {time.time()-t0:.1f}s", flush=True)


if __name__ == "__main__":
    main()
