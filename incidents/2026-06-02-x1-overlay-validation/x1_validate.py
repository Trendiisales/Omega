#!/usr/bin/env python3
"""
x1_validate.py
==============
Offline validation engine for an X1-Algo-style overlay (WaveTrend oscillator +
momentum / retracement chart tags), checked against real Omega fills.

What it does
------------
1. Reads tick data (Dukascopy-style: ts_ms, askPrice, bidPrice) OR a pre-built
   OHLC bars CSV, and aggregates mid-price into bars at a chosen timeframe.
2. Computes the WaveTrend oscillator (LazyBear formulation) plus four
   chart-tag families, ALL on closed bars only (non-repainting by construction:
   every tag at bar t uses data <= t via .shift(1)).
3. Forward-return validation: for each tag family, the distribution of returns
   over N bars ahead, with a directional hit-rate. Answers "do the tags carry
   edge" rather than "do they look good in hindsight".
4. Overlays your real trades (omega_trade_closes.csv, filtered to one symbol),
   normalising side (LONG/SHORT/BUY/SELL), and measures whether a confirming
   tag fired in the lead-up to each entry (the "leads vs tracks" question).
5. Renders a chart: candles + tags + trade entry/exit markers, with a
   WaveTrend sub-panel.

Symbol- and source-agnostic: point --ticks (or --bars) at any instrument and
set --symbol to the matching value in the trades file.

Usage
-----
  # from ticks (aggregates to M1 by default)
  python3 x1_validate.py --ticks XAUUSD_2026-05.csv --symbol XAUUSD --tf 1 \
      --trades omega_trade_closes.csv --out xau_may.png

  # from a pre-built OHLC bars CSV (columns: ts_utc/ts_unix, open, high, low, close)
  python3 x1_validate.py --bars xau_m1.csv --symbol XAUUSD \
      --trades omega_trade_closes.csv --out xau_may.png

Indicator-only (no overlay): omit --trades or pass a symbol with no fills.
"""

import argparse
import sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

# --------------------------------------------------------------------------- #
# Defaults (all overridable on the command line)
# --------------------------------------------------------------------------- #
DEFAULTS = dict(
    tf=1,                 # bar timeframe in minutes
    # WaveTrend (LazyBear): channel length, average length, signal SMA length
    wt_n1=10, wt_n2=21, wt_signal=4,
    wt_ob1=53.0, wt_ob2=60.0, wt_os1=-53.0, wt_os2=-60.0,
    # trend regime EMAs (on close)
    ema_fast=21, ema_slow=55,
    horizons="5,10,20",   # forward-return horizons in bars
    lookback=10,          # bars before a trade entry to look for a confirming tag
    chunk=2_000_000,      # tick read chunk size
)

LONG_SIDES = {"LONG", "BUY"}
SHORT_SIDES = {"SHORT", "SELL"}

C_UP = "#26a69a"
C_DOWN = "#ef5350"
C_WT1 = "#3fc1c9"
C_WT2 = "#ff9f1c"
C_MOMUP = "#00e676"
C_MOMDN = "#ff1744"
C_RETR = "#7c4dff"


# --------------------------------------------------------------------------- #
# Data loading
# --------------------------------------------------------------------------- #
def ticks_to_bars(path, tf_min, start_ms=None, end_ms=None, chunk=2_000_000):
    """Stream a Dukascopy-style tick CSV (ts_ms, askPrice, bidPrice) and build
    OHLC bars from the mid price. Optional [start_ms, end_ms] window keeps memory
    bounded on the multi-GB canonical file."""
    parts = []
    reader = pd.read_csv(
        path, header=0, names=["ts_ms", "ask", "bid"],
        dtype={"ts_ms": "int64", "ask": "float64", "bid": "float64"},
        chunksize=chunk,
    )
    for ch in reader:
        if start_ms is not None:
            ch = ch[ch["ts_ms"] >= start_ms]
        if end_ms is not None:
            ch = ch[ch["ts_ms"] <= end_ms]
        if ch.empty:
            continue
        mid = (ch["ask"] + ch["bid"]) / 2.0
        parts.append(pd.DataFrame({"ts_ms": ch["ts_ms"].values, "mid": mid.values}))
    if not parts:
        raise SystemExit("No ticks in the requested window — check --symbol/--from/--to.")
    df = pd.concat(parts, ignore_index=True)
    df["dt"] = pd.to_datetime(df["ts_ms"], unit="ms", utc=True)
    df = df.set_index("dt").sort_index()
    rule = f"{tf_min}min"
    bars = df["mid"].resample(rule, closed="left", label="left").ohlc()
    bars = bars.dropna(how="any")
    bars.columns = ["open", "high", "low", "close"]
    return bars


def load_bars(path):
    """Load a pre-built OHLC bars CSV. Accepts ts_utc (ISO) or ts_unix (sec)."""
    df = pd.read_csv(path)
    cols = {c.lower(): c for c in df.columns}
    if "ts_utc" in cols:
        idx = pd.to_datetime(df[cols["ts_utc"]], utc=True)
    elif "ts_unix" in cols:
        idx = pd.to_datetime(df[cols["ts_unix"]].astype("int64"), unit="s", utc=True)
    elif "dt" in cols:
        idx = pd.to_datetime(df[cols["dt"]], utc=True)
    else:
        raise SystemExit("bars CSV needs a ts_utc, ts_unix, or dt column.")
    out = pd.DataFrame({
        "open": df[cols["open"]].astype(float),
        "high": df[cols["high"]].astype(float),
        "low": df[cols["low"]].astype(float),
        "close": df[cols["close"]].astype(float),
    })
    out.index = idx
    return out.sort_index().dropna(how="any")


# --------------------------------------------------------------------------- #
# Indicators
# --------------------------------------------------------------------------- #
def ema(s, span):
    return s.ewm(span=span, adjust=False).mean()


def wavetrend(bars, n1, n2, signal):
    """LazyBear WaveTrend on hlc3."""
    ap = (bars["high"] + bars["low"] + bars["close"]) / 3.0
    esa = ema(ap, n1)
    d = ema((ap - esa).abs(), n1)
    d = d.where(d > 1e-12, 1e-12)            # guard division by zero
    ci = (ap - esa) / (0.015 * d)
    wt1 = ema(ci, n2)
    wt2 = wt1.rolling(signal).mean()
    return wt1, wt2


def compute_tags(bars, cfg):
    """Add WaveTrend, regime, and the four non-repainting tag columns."""
    b = bars.copy()
    b["wt1"], b["wt2"] = wavetrend(b, cfg["wt_n1"], cfg["wt_n2"], cfg["wt_signal"])
    b["ema_fast"] = ema(b["close"], cfg["ema_fast"])
    b["ema_slow"] = ema(b["close"], cfg["ema_slow"])
    regime_up = b["ema_fast"] > b["ema_slow"]

    wt1, wt2 = b["wt1"], b["wt2"]
    p1, p2 = wt1.shift(1), wt2.shift(1)        # previous closed bar
    cross_up = (wt1 > wt2) & (p1 <= p2)
    cross_dn = (wt1 < wt2) & (p1 >= p2)

    b["momentum_up"] = cross_up & regime_up
    b["momentum_down"] = cross_dn & (~regime_up)
    # retracement = first touch of OB/OS against the prevailing regime
    b["retr_down"] = regime_up & (wt1 >= cfg["wt_ob1"]) & (wt1.shift(1) < cfg["wt_ob1"])
    b["retr_up"] = (~regime_up) & (wt1 <= cfg["wt_os1"]) & (wt1.shift(1) > cfg["wt_os1"])
    b["regime_up"] = regime_up
    return b


# --------------------------------------------------------------------------- #
# Forward-return validation
# --------------------------------------------------------------------------- #
def forward_validation(b, horizons):
    rows = []
    for tag, expect_up in [("momentum_up", True), ("momentum_down", False),
                           ("retr_down", False), ("retr_up", True)]:
        mask = b[tag].fillna(False)
        n = int(mask.sum())
        for h in horizons:
            fwd = b["close"].shift(-h) / b["close"] - 1.0
            f = fwd[mask].dropna()
            if len(f) == 0:
                rows.append((tag, h, 0, np.nan, np.nan, np.nan))
                continue
            hit = (f > 0).mean() if expect_up else (f < 0).mean()
            rows.append((tag, h, len(f), f.mean() * 1e4, f.median() * 1e4, hit * 100))
    return pd.DataFrame(rows, columns=["tag", "horizon_bars", "n",
                                       "mean_bps", "median_bps", "hit_rate_pct"])


# --------------------------------------------------------------------------- #
# Trades
# --------------------------------------------------------------------------- #
def load_trades(path, symbol):
    df = pd.read_csv(path)
    df = df[df["symbol"] == symbol].copy()
    if df.empty:
        return df
    df["entry_dt"] = pd.to_datetime(df["entry_ts_unix"].astype("int64"), unit="s", utc=True)
    df["exit_dt"] = pd.to_datetime(df["exit_ts_unix"].astype("int64"), unit="s", utc=True)
    df["dir"] = df["side"].str.upper().map(
        lambda s: 1 if s in LONG_SIDES else (-1 if s in SHORT_SIDES else 0))
    df["net_pnl"] = pd.to_numeric(df["net_pnl"], errors="coerce").fillna(0.0)
    return df.sort_values("entry_dt")


def trade_context(b, trades, lookback):
    """For each trade in the bar window: WaveTrend at entry + whether a confirming
    momentum tag fired within `lookback` bars before entry."""
    idx = b.index
    recs = []
    for _, t in trades.iterrows():
        if t["entry_dt"] < idx[0] or t["entry_dt"] > idx[-1]:
            continue
        pos = idx.searchsorted(t["entry_dt"], side="right") - 1
        if pos < 0:
            continue
        lo = max(0, pos - lookback)
        win = b.iloc[lo:pos + 1]
        if t["dir"] == 1:
            confirm = bool(win["momentum_up"].any())
        elif t["dir"] == -1:
            confirm = bool(win["momentum_down"].any())
        else:
            confirm = False
        recs.append(dict(entry_dt=t["entry_dt"], dir=t["dir"], net_pnl=t["net_pnl"],
                         engine=t.get("engine", ""), exit_reason=t.get("exit_reason", ""),
                         wt1_at_entry=float(b["wt1"].iloc[pos]),
                         regime_up=bool(b["regime_up"].iloc[pos]),
                         confirming_tag=confirm))
    return pd.DataFrame(recs)


# --------------------------------------------------------------------------- #
# Plot
# --------------------------------------------------------------------------- #
def plot(b, trades, cfg, out_png, symbol):
    n = len(b)
    x = np.arange(n)
    fig, (ax, axo) = plt.subplots(
        2, 1, figsize=(min(34, max(12, n * 0.045)), 8), sharex=True,
        gridspec_kw={"height_ratios": [3, 1]})
    fig.patch.set_facecolor("#0b0e11")
    for a in (ax, axo):
        a.set_facecolor("#0b0e11")
        a.tick_params(colors="#aaa")
        for sp in a.spines.values():
            sp.set_color("#333")

    o, h, l, c = b["open"].values, b["high"].values, b["low"].values, b["close"].values
    w = 0.6
    for i in range(n):
        col = C_UP if c[i] >= o[i] else C_DOWN
        ax.vlines(x[i], l[i], h[i], color=col, linewidth=0.6)
        lo_, hi_ = min(o[i], c[i]), max(o[i], c[i])
        ax.add_patch(Rectangle((x[i] - w / 2, lo_), w, max(hi_ - lo_, 1e-9),
                               facecolor=col, edgecolor=col))

    rng = (np.nanmax(h) - np.nanmin(l)) or 1.0
    off = rng * 0.012
    def mark(mask, dy, marker, color, label):
        ix = np.where(b[mask].fillna(False).values)[0]
        if len(ix):
            yy = (l[ix] - off) if dy < 0 else (h[ix] + off)
            ax.scatter(ix, yy, marker=marker, c=color, s=42, zorder=5, label=label)
    mark("momentum_up", -1, "^", C_MOMUP, "Momentum Up")
    mark("momentum_down", +1, "v", C_MOMDN, "Momentum Down")
    mark("retr_down", +1, "x", C_RETR, "Possible Retr. Down")
    mark("retr_up", -1, "+", C_RETR, "Possible Retr. Up")

    # trades
    if trades is not None and not trades.empty:
        idx = b.index
        plotted = 0
        for _, t in trades.iterrows():
            if t["entry_dt"] < idx[0] or t["entry_dt"] > idx[-1]:
                continue
            ei = idx.searchsorted(t["entry_dt"], side="right") - 1
            xi = idx.searchsorted(min(t["exit_dt"], idx[-1]), side="right") - 1
            if ei < 0:
                continue
            em = "^" if t["dir"] == 1 else "v"
            ax.scatter(ei, c[ei], marker=em, s=120, facecolors="none",
                       edgecolors="#ffffff", linewidths=1.4, zorder=6)
            xcol = C_UP if t["net_pnl"] > 0 else (C_DOWN if t["net_pnl"] < 0 else "#888")
            if 0 <= xi < n:
                ax.scatter(xi, c[xi], marker="o", s=40, c=xcol, zorder=6)
                ax.plot([ei, xi], [c[ei], c[xi]], color=xcol, lw=0.7, alpha=0.6, zorder=4)
            plotted += 1
        ax.set_title(f"{symbol}  bars={n}  trades_in_window={plotted}",
                     color="#ddd", fontsize=11)
    else:
        ax.set_title(f"{symbol}  bars={n}  (indicator-only)", color="#ddd", fontsize=11)

    ax.legend(loc="upper left", facecolor="#15191e", edgecolor="#333",
              labelcolor="#ccc", fontsize=8)

    # WaveTrend sub-panel
    axo.plot(x, b["wt1"].values, color=C_WT1, lw=1.0, label="wt1")
    axo.plot(x, b["wt2"].values, color=C_WT2, lw=0.9, label="wt2")
    axo.fill_between(x, b["wt1"].values, b["wt2"].values,
                     where=(b["wt1"] >= b["wt2"]).values, color=C_UP, alpha=0.25)
    axo.fill_between(x, b["wt1"].values, b["wt2"].values,
                     where=(b["wt1"] < b["wt2"]).values, color=C_DOWN, alpha=0.25)
    for lvl in (cfg["wt_ob1"], cfg["wt_ob2"], cfg["wt_os1"], cfg["wt_os2"]):
        axo.axhline(lvl, color="#555", lw=0.6, ls="--")
    axo.axhline(0, color="#777", lw=0.5)
    axo.legend(loc="upper left", facecolor="#15191e", edgecolor="#333",
               labelcolor="#ccc", fontsize=8)
    axo.set_ylabel("WaveTrend", color="#aaa")

    # x tick labels as datetimes
    ticks = np.linspace(0, n - 1, min(10, n)).astype(int)
    axo.set_xticks(ticks)
    axo.set_xticklabels([b.index[i].strftime("%m-%d %H:%M") for i in ticks],
                        rotation=30, ha="right", fontsize=8)
    plt.tight_layout()
    fig.savefig(out_png, dpi=110, facecolor=fig.get_facecolor())
    plt.close(fig)


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description="X1-style overlay validation")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--ticks", help="tick CSV (ts_ms,askPrice,bidPrice)")
    src.add_argument("--bars", help="pre-built OHLC bars CSV")
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--trades", default=None, help="omega_trade_closes.csv (optional)")
    ap.add_argument("--out", default="x1_validation.png")
    ap.add_argument("--tf", type=int, default=DEFAULTS["tf"])
    ap.add_argument("--from", dest="dfrom", default=None, help="UTC start YYYY-MM-DD")
    ap.add_argument("--to", dest="dto", default=None, help="UTC end YYYY-MM-DD")
    ap.add_argument("--lookback", type=int, default=DEFAULTS["lookback"])
    ap.add_argument("--horizons", default=DEFAULTS["horizons"])
    ap.add_argument("--plot-bars", dest="plot_bars", type=int, default=750,
                    help="max bars rendered on the chart (stats always use all bars)")
    args = ap.parse_args()

    cfg = dict(DEFAULTS)
    cfg["tf"] = args.tf
    cfg["lookback"] = args.lookback
    horizons = [int(x) for x in args.horizons.split(",") if x.strip()]

    if args.ticks:
        sms = ems = None
        if args.dfrom:
            sms = int(pd.Timestamp(args.dfrom, tz="UTC").timestamp() * 1000)
        if args.dto:
            ems = int(pd.Timestamp(args.dto, tz="UTC").timestamp() * 1000) + 86_400_000
        print(f"Aggregating ticks -> M{cfg['tf']} bars ...", file=sys.stderr)
        bars = ticks_to_bars(args.ticks, cfg["tf"], sms, ems, DEFAULTS["chunk"])
    else:
        bars = load_bars(args.bars)
        if args.dfrom:
            bars = bars[bars.index >= pd.Timestamp(args.dfrom, tz="UTC")]
        if args.dto:
            bars = bars[bars.index <= pd.Timestamp(args.dto, tz="UTC") + pd.Timedelta(days=1)]

    if len(bars) < max(cfg["ema_slow"], cfg["wt_n2"]) + 5:
        raise SystemExit(f"Only {len(bars)} bars — not enough to compute indicators.")

    b = compute_tags(bars, cfg)

    print(f"\n=== {args.symbol}  bars={len(b)}  "
          f"{b.index[0]} .. {b.index[-1]} ===")
    counts = {t: int(b[t].fillna(False).sum())
              for t in ["momentum_up", "momentum_down", "retr_down", "retr_up"]}
    print("Tag counts:", counts)

    fv = forward_validation(b, horizons)
    print("\n--- Forward-return validation (bps) ---")
    print(fv.to_string(index=False))

    trades = None
    if args.trades:
        trades = load_trades(args.trades, args.symbol)
        print(f"\nTrades for {args.symbol}: {len(trades)}")
        if not trades.empty:
            ctx = trade_context(b, trades, cfg["lookback"])
            if not ctx.empty:
                win = ctx[ctx["net_pnl"] > 0]
                los = ctx[ctx["net_pnl"] < 0]
                def frac(d):
                    return (d["confirming_tag"].mean() * 100) if len(d) else float("nan")
                print(f"  In-window trades: {len(ctx)}  "
                      f"(winners {len(win)} / losers {len(los)})")
                print(f"  Confirming momentum tag in prior {cfg['lookback']} bars:")
                print(f"    winners: {frac(win):5.1f}%   losers: {frac(los):5.1f}%")
            else:
                print("  No trades fell inside the bar window (date mismatch).")

    # Choose a readable, renderable window for the chart (stats above use all bars).
    cap = args.plot_bars
    if len(b) <= cap:
        bplot = b
    else:
        center = None
        if trades is not None and not trades.empty:
            inwin = trades[(trades["entry_dt"] >= b.index[0]) &
                           (trades["entry_dt"] <= b.index[-1])]
            if not inwin.empty:
                lo = b.index.searchsorted(inwin["entry_dt"].min())
                hi = b.index.searchsorted(inwin["entry_dt"].max())
                center = (lo + hi) // 2
        if center is None:
            bplot = b.iloc[-cap:]
        else:
            half = cap // 2
            s = max(0, min(center - half, len(b) - cap))
            bplot = b.iloc[s:s + cap]
        print(f"(chart shows {len(bplot)} of {len(b)} bars: "
              f"{bplot.index[0]} .. {bplot.index[-1]}; widen with --plot-bars)")

    plot(bplot, trades, cfg, args.out, args.symbol)
    print(f"\nChart written: {args.out}")


if __name__ == "__main__":
    main()
