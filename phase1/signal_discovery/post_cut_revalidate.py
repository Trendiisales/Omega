#!/usr/bin/env python3
"""
post_cut_revalidate.py
======================
Re-run the high-value missing strategies (tsmom H1/H2/H4, donchian H4/H6
long+short) on the post-2025-04 microstructure-regime portion of the data
to confirm whether the master_summary edge survives the cut.

Signal definitions match the canonical phase1 set:
- tsmom:    long if 20-bar return > 0;  short if 20-bar return < 0.
            Hold 12 bars or hard SL at 3*atr14.  (sim_family_c)
- donchian: period=20 breakout.  Long if close > prior 20-bar high.
            Exit on TP=2.5*atr14, SL=1.0*atr14, or 30-bar timeout. (sim_family_a)

Cost model: 0.05pt commission + 1.0 * avg_spread per trade (matches sim_lib's
apply_costs which uses TickReader bid/ask + commission).  Avg spread on the
post-cut bars is ~0.69pt, so per-trade cost ~= 0.74pt.  This is realistic
and matches the master_summary's net/stress columns.

Inputs:  phase1/signal_discovery/bars_5s.parquet  (already produced)
Outputs: phase1/signal_discovery/POST_CUT_REVALIDATE_REPORT.md
         phase1/signal_discovery/post_cut_<strategy>_<tf>_<dir>.parquet
"""
from __future__ import annotations
import os, sys, time, math
import numpy as np
import pandas as pd
import duckdb

OUT = "/sessions/hopeful-friendly-meitner/mnt/omega_repo/phase1/signal_discovery"
BARS_5S = os.path.join(OUT, "bars_5s.parquet")
REPORT = os.path.join(OUT, "POST_CUT_REVALIDATE_REPORT.md")

COMMISSION = 0.05  # pts/trade
TF_BAR_MS = {"H1": 60*60*1000, "H2": 2*60*60*1000, "H4": 4*60*60*1000, "H6": 6*60*60*1000}


def resample(tf: str) -> pd.DataFrame:
    """Resample 5s bars to the requested timeframe using duckdb."""
    bar_ms = TF_BAR_MS[tf]
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


def compute_atr14(df: pd.DataFrame) -> pd.Series:
    tr = (df["high_mid"] - df["low_mid"]).rolling(14, min_periods=7).mean()
    return tr


def signals_tsmom(df: pd.DataFrame, lookback: int = 20) -> pd.Series:
    """tsmom: long if N-bar return > 0, short if < 0.  Fires every bar."""
    ret_n = df["close_mid"].diff(lookback)
    sig = pd.Series(0, index=df.index, dtype="int8")
    sig.loc[ret_n > 0] = 1
    sig.loc[ret_n < 0] = -1
    return sig


def signals_donchian(df: pd.DataFrame, period: int = 20, cooldown: int = 5) -> pd.Series:
    """donchian: long when close > prior N-bar high; short when close < prior N-bar low.
    Cooldown: skip <cooldown> bars after each fire to prevent stacking."""
    prior_high = df["high_mid"].rolling(period, min_periods=period).max().shift(1)
    prior_low  = df["low_mid"].rolling(period, min_periods=period).min().shift(1)
    raw = pd.Series(0, index=df.index, dtype="int8")
    raw.loc[df["close_mid"] > prior_high] =  1
    raw.loc[df["close_mid"] < prior_low ] = -1
    # Apply cooldown (greedy, in time order)
    sig = raw.copy()
    last = -10**9
    arr = sig.to_numpy()
    for i in range(len(arr)):
        if arr[i] != 0:
            if i - last < cooldown:
                arr[i] = 0
            else:
                last = i
    sig[:] = arr
    return sig


def simulate_a(df: pd.DataFrame, sig: pd.Series, sl_atr: float = 1.0,
               tp_r: float = 2.5, max_hold: int = 30) -> pd.DataFrame:
    """Family-A simulator: ATR-scaled SL/TP, hold-bar timeout.  Mid-price OHLC.
    Spread cost = 1.0*avg_spread per trade.  Commission = 0.05 pt."""
    high = df["high_mid"].to_numpy()
    low  = df["low_mid"].to_numpy()
    close = df["close_mid"].to_numpy()
    open_ = df["open_mid"].to_numpy()
    spread = df["avg_spread"].to_numpy()
    bar_ms = df["bar_ms"].to_numpy()
    atr = compute_atr14(df).to_numpy()
    n = len(df)
    sig_idx = np.flatnonzero(sig.values != 0)
    side = sig.to_numpy()
    rows = []
    for i in sig_idx:
        if i + 1 >= n: continue
        a = atr[i]
        if not np.isfinite(a) or a <= 0: continue
        s = int(side[i])
        # Entry at next bar open
        e_px = open_[i+1]
        sl = e_px - s * sl_atr * a
        tp = e_px + s * tp_r * sl_atr * a
        exit_ix = exit_px = exit_reason = None
        end = min(i+1+max_hold, n)
        for j in range(i+1, end):
            h = high[j]; l = low[j]
            if s == 1:
                hit_sl = (l <= sl); hit_tp = (h >= tp)
            else:
                hit_sl = (h >= sl); hit_tp = (l <= tp)
            if hit_sl and hit_tp:
                exit_ix=j; exit_px=sl; exit_reason="SL_HIT"; break  # worst-case
            if hit_sl:
                exit_ix=j; exit_px=sl; exit_reason="SL_HIT"; break
            if hit_tp:
                exit_ix=j; exit_px=tp; exit_reason="TP_HIT"; break
        if exit_ix is None:
            exit_ix = end - 1
            exit_px = close[exit_ix]
            exit_reason = "TIME_EXIT"
        gross = (exit_px - e_px) * s
        # Cost: commission + 1 * avg_spread (round-trip spread cross)
        cost = COMMISSION + spread[i]
        net = gross - cost
        rows.append({
            "entry_ms": int(bar_ms[i+1]),
            "exit_ms":  int(bar_ms[exit_ix]),
            "side": s,
            "entry_px": float(e_px),
            "exit_px":  float(exit_px),
            "atr14": float(a),
            "exit_reason": exit_reason,
            "gross": float(gross),
            "net":   float(net),
            "spread_at_entry": float(spread[i]),
        })
    return pd.DataFrame(rows)


def simulate_c(df: pd.DataFrame, sig: pd.Series, hold_bars: int = 12,
               hard_sl_atr: float = 3.0) -> pd.DataFrame:
    """Family-C simulator (tsmom): hold N bars or hard SL.  No TP."""
    high = df["high_mid"].to_numpy()
    low  = df["low_mid"].to_numpy()
    close = df["close_mid"].to_numpy()
    open_ = df["open_mid"].to_numpy()
    spread = df["avg_spread"].to_numpy()
    bar_ms = df["bar_ms"].to_numpy()
    atr = compute_atr14(df).to_numpy()
    n = len(df)
    sig_idx = np.flatnonzero(sig.values != 0)
    side = sig.to_numpy()
    rows = []
    for i in sig_idx:
        if i + 1 >= n: continue
        a = atr[i]
        if not np.isfinite(a) or a <= 0: continue
        s = int(side[i])
        e_px = open_[i+1]
        sl = e_px - s * hard_sl_atr * a
        exit_ix = exit_px = exit_reason = None
        end = min(i+1+hold_bars, n)
        for j in range(i+1, end):
            h = high[j]; l = low[j]
            hit = (l <= sl) if s == 1 else (h >= sl)
            if hit:
                exit_ix=j; exit_px=sl; exit_reason="SL_HIT"; break
        if exit_ix is None:
            exit_ix = end - 1
            exit_px = close[exit_ix]
            exit_reason = "TIME_EXIT"
        gross = (exit_px - e_px) * s
        cost = COMMISSION + spread[i]
        net = gross - cost
        rows.append({
            "entry_ms": int(bar_ms[i+1]), "exit_ms": int(bar_ms[exit_ix]),
            "side": s, "entry_px": float(e_px), "exit_px": float(exit_px),
            "atr14": float(a), "exit_reason": exit_reason,
            "gross": float(gross), "net": float(net),
            "spread_at_entry": float(spread[i]),
        })
    return pd.DataFrame(rows)


def stats(df: pd.DataFrame, label: str) -> dict:
    n = len(df)
    if not n: return {"label": label, "n": 0}
    wins = (df["net"] > 0).sum()
    g_w = df.loc[df["net"]>0, "net"].sum() if wins else 0.0
    g_l = df.loc[df["net"]<0, "net"].sum() if (df["net"]<0).any() else 0.0
    return {
        "label": label, "n": n,
        "wr": wins / n,
        "gross": float(df["gross"].sum()),
        "net":   float(df["net"].sum()),
        "avg_trade": float(df["net"].mean()),
        "pf": (g_w / abs(g_l)) if g_l < 0 else float("inf"),
        "exits": dict(df["exit_reason"].value_counts().items()),
    }


def main():
    t0 = time.time()
    print("[+] resampling 5s bars to H1/H2/H4/H6...", flush=True)
    bars = {tf: resample(tf) for tf in ["H1", "H2", "H4", "H6"]}
    for tf, b in bars.items():
        print(f"    {tf}: {len(b):,} bars  span {pd.to_datetime(b['bar_ms'].min(),unit='ms')} -> {pd.to_datetime(b['bar_ms'].max(),unit='ms')}",
              flush=True)

    cells = [
        # (strategy, tf, dir, family-fn, family-name)
        ("tsmom",    "H1", "long",  "C", {"hold_bars":12, "hard_sl_atr":3.0}),
        ("tsmom",    "H2", "long",  "C", {"hold_bars":12, "hard_sl_atr":3.0}),
        ("tsmom",    "H4", "long",  "C", {"hold_bars":12, "hard_sl_atr":3.0}),
        ("tsmom",    "H1", "short", "C", {"hold_bars":12, "hard_sl_atr":3.0}),
        ("donchian", "H1", "long",  "A", {"sl_atr":3.0, "tp_r":1.6667, "max_hold":30}),  # retuned
        ("donchian", "H4", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
        ("donchian", "H4", "short", "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
        ("donchian", "H6", "long",  "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
        ("donchian", "H6", "short", "A", {"sl_atr":1.0, "tp_r":2.5, "max_hold":30}),
    ]
    results = []
    for strat, tf, direc, fam, params in cells:
        b = bars[tf]
        if strat == "tsmom":
            sig = signals_tsmom(b, lookback=20)
            if direc == "long":
                sig = sig.where(sig > 0, 0)
            else:
                sig = sig.where(sig < 0, 0)
            trades = simulate_c(b, sig, **params)
        else:  # donchian
            sig = signals_donchian(b, period=20, cooldown=5)
            if direc == "long":
                sig = sig.where(sig > 0, 0)
            else:
                sig = sig.where(sig < 0, 0)
            trades = simulate_a(b, sig, **params)
        # Persist
        out = os.path.join(OUT, f"post_cut_{strat}_{tf}_{direc}.parquet")
        if len(trades):
            trades.to_parquet(out, compression="zstd")
        st = stats(trades, f"{strat} {tf} {direc}")
        results.append(st)
        print(f"  {st['label']:<22s} n={st.get('n',0):>4,}  "
              f"wr={st.get('wr',0):.1%}  net=${st.get('net',0):+.0f}  "
              f"avg=${st.get('avg_trade',0):+.2f}  pf={st.get('pf',0):.2f}",
              flush=True)
    # Report
    lines = [
        "# Post-2025-04 Re-validation Report",
        "",
        f"_Generated: {pd.Timestamp.utcnow().isoformat()}_",
        "",
        f"**Cost model:** 0.05pt commission + 1 * avg_spread per trade",
        f"**Period:** 2025-04-01 -> 2026-04-01 (post-microstructure-regime cut)",
        "",
        "## Per-cell results",
        "",
        "| strategy | tf | dir | n | WR | gross | net | avg/trade | pf | exits |",
        "|---|---|---|---:|---:|---:|---:|---:|---:|:---|",
    ]
    for r in results:
        if r.get("n", 0) == 0:
            lines.append(f"| {r['label']} | -- | -- | 0 | -- | -- | -- | -- | -- | -- |")
            continue
        ex = " ".join(f"{k}:{v}" for k, v in r["exits"].items())
        parts = r["label"].split()
        lines.append(
            f"| {parts[0]} | {parts[1]} | {parts[2]} | {r['n']:,} | "
            f"{r['wr']:.1%} | ${r['gross']:+.0f} | ${r['net']:+.0f} | "
            f"${r['avg_trade']:+.2f} | {r['pf']:.2f} | {ex} |"
        )
    # Top by net
    lines.append("")
    lines.append("## Ranked by net (post-cut)")
    lines.append("")
    sorted_r = sorted([r for r in results if r.get("n",0)>0], key=lambda x: x["net"], reverse=True)
    for rk, r in enumerate(sorted_r, 1):
        lines.append(f"{rk}. **{r['label']}** -- n={r['n']:,}, net=${r['net']:+.0f}, "
                     f"avg=${r['avg_trade']:+.2f}, WR {r['wr']:.1%}, pf {r['pf']:.2f}")
    # Verdict text
    pos = [r for r in results if r.get("net",0) > 0]
    lines.append("")
    lines.append("## Verdict")
    lines.append("")
    if pos:
        lines.append(f"**{len(pos)} of {len(results)} cells profitable in post-2025-04 cut.**")
        lines.append(f"Best: {sorted_r[0]['label']} at ${sorted_r[0]['net']:+.0f} net.")
    else:
        lines.append("No cells profitable post-cut.  The master_summary edge does NOT survive "
                     "the microstructure regime change.  Recommend signal-discovery iteration "
                     "or alternative strategies.")
    with open(REPORT, "w") as fh:
        fh.write("\n".join(lines))
    print(f"\n[+] wrote {REPORT}", flush=True)
    print(f"[+] total elapsed: {time.time()-t0:.1f}s", flush=True)


if __name__ == "__main__":
    main()
