#!/usr/bin/env python3
"""
x1_live.py — Mac-local live X1 overlay & interpreter (GOLD-FIRST)
=================================================================
Runs the gold-validated X1 momentum-confirm overlay on the Mac in REAL TIME,
driven by the Omega GUI's live feed.

Live feed (the key bit)
-----------------------
The Omega GUI telemetry (/api/telemetry, pushed ~250ms) carries top-level live
top-of-book for every symbol: gold_bid / gold_ask (also sp_bid/ask, nq_bid/ask,
eurusd_bid/ask, ...) plus xau_curh/curl/pdh/pdl. This is a CONTINUOUS live price
— available whether or not a trade is open. We poll it, take mid=(bid+ask)/2,
and aggregate the stream into M1 bars on the Mac in real time.

WaveTrend needs ~55 bars of warmup, so on startup we SEED history from a rolling
Dukascopy XAUUSD M1 pull (lags ~1h), then EXTEND it with live M1 bars built from
the telemetry mid as the app runs. The two meet as Dukascopy's recent hours close.

Scope: XAUUSD only. The confirm-filter edge is gold-specific
(X1_MULTISYMBOL_FINDINGS.md). Do NOT extend to indices.

Interpretation printed
----------------------
  - Current regime (EMA21 vs EMA55) + WaveTrend wt1/wt2 + last tag, on the live bar.
  - "Would a long/short entry RIGHT NOW be momentum-confirmed?" + the validated
    gold base rate (confirmed trend winners 71.9% vs losers 51.5%; +12pp
    within-trend — X1_STAGE1_FINDINGS.md).
  - For each OPEN gold trade (from telemetry live_trades): was a confirming
    momentum tag present in the `lookback` bars before entry? -> CONFIRMED /
    UNCONFIRMED, with live P&L, flagging non-trend engines.

Read-only. Never sends orders. Touches no core/engine code.

Usage
-----
  python3 x1_live.py --loop                      # real-time, GUI on localhost:7779
  python3 x1_live.py --gui-url http://localhost:7779 --loop --interval 5 --chart x1_live.png
  # remote VPS GUI via SSH tunnel:
  #   ssh -N -L 7779:localhost:7779 trader@185.167.119.59 -p 2222
  python3 x1_live.py --no-gui --bars XAUUSD_2026-05_m1.csv --hours 0   # offline demo
"""

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import time
import urllib.request
import pandas as pd

import x1_validate as X
from x1_stage1 import family_of

SYMBOL = "XAUUSD"
DUKAS_SCALE = 1000
BASE_RATE = ("gold confirmed-trend winners 71.9% vs losers 51.5% "
             "(+12pp within-trend, ~2 SE)")


# --------------------------------------------------------------------------- #
# GUI telemetry
# --------------------------------------------------------------------------- #
def fetch_gui(gui_url, timeout=4):
    try:
        req = urllib.request.Request(gui_url.rstrip("/") + "/api/telemetry",
                                     headers={"User-Agent": "x1_live"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception as e:
        print(f"[gui] unreachable ({e})", file=sys.stderr)
        return None


def gold_mid(tele):
    """Live gold mid from top-level gold_bid/gold_ask. None if absent."""
    if not tele:
        return None
    b, a = tele.get("gold_bid"), tele.get("gold_ask")
    try:
        b, a = float(b), float(a)
    except (TypeError, ValueError):
        return None
    if b <= 0 or a <= 0:
        return None
    return (b + a) / 2.0


def gold_open_trades(tele):
    if not tele:
        return []
    return [t for t in tele.get("live_trades", [])
            if SYMBOL in str(t.get("symbol", ""))]


# --------------------------------------------------------------------------- #
# Live M1 bar builder — aggregates polled mids into minute OHLC.
# Persists across loop iterations.
# --------------------------------------------------------------------------- #
class LiveBars:
    def __init__(self):
        self.bars = {}   # minute_floor_ts(UTC, sec) -> [o,h,l,c]

    def push(self, mid, now_utc):
        m = int(now_utc.timestamp() // 60 * 60)
        b = self.bars.get(m)
        if b is None:
            self.bars[m] = [mid, mid, mid, mid]
        else:
            b[1] = max(b[1], mid)
            b[2] = min(b[2], mid)
            b[3] = mid

    def frame(self):
        if not self.bars:
            return pd.DataFrame(columns=["open", "high", "low", "close"])
        idx = pd.to_datetime(sorted(self.bars), unit="s", utc=True)
        rows = [self.bars[int(t.timestamp())] for t in idx]
        df = pd.DataFrame(rows, columns=["open", "high", "low", "close"], index=idx)
        return df


# --------------------------------------------------------------------------- #
# Seed history (Dukascopy) + merge with live bars
# --------------------------------------------------------------------------- #
def seed_bars(hours_back, bars_csv, refresh, here):
    path = bars_csv or os.path.join(here, "_x1_seed_XAUUSD_m1.csv")
    need = refresh and (not os.path.exists(path) or
                        (time.time() - os.path.getmtime(path)) > 600)
    if need:
        to = dt.datetime.now(dt.timezone.utc).date()
        frm = to - dt.timedelta(days=max(2, hours_back // 24 + 1))
        pull = os.path.join(here, "pull_dukascopy.py")
        cmd = [sys.executable, pull, "--symbol", "XAUUSD",
               "--from", frm.isoformat(), "--to", to.isoformat(),
               "--scale", str(DUKAS_SCALE), "--out", path, "--workers", "16"]
        print(f"[seed] Dukascopy M1 {frm}..{to} -> {path}", file=sys.stderr)
        subprocess.run(cmd, cwd=here, check=False)
    if not os.path.exists(path):
        return pd.DataFrame(columns=["open", "high", "low", "close"])
    b = X.load_bars(path)
    return b.iloc[-(hours_back * 60):] if hours_back else b


def merge(seed, live):
    """Concatenate seed history with live-built bars; live overrides on overlap."""
    if live.empty:
        return seed
    if seed.empty:
        return live
    cut = live.index[0]
    return pd.concat([seed[seed.index < cut], live]).sort_index()


# --------------------------------------------------------------------------- #
# Interpretation
# --------------------------------------------------------------------------- #
def confirm_at(b, entry_dt, lookback, side):
    idx = b.index
    if entry_dt < idx[0] or entry_dt > idx[-1]:
        return None
    pos = idx.searchsorted(entry_dt, side="right") - 1
    if pos < 0:
        return None
    win = b.iloc[max(0, pos - lookback):pos + 1]
    if side > 0:
        return bool(win["momentum_up"].any())
    if side < 0:
        return bool(win["momentum_down"].any())
    return None


def interpret(b, lookback):
    last = b.iloc[-1]
    tag = next((t for t in ("momentum_up", "momentum_down", "retr_down", "retr_up")
                if bool(last.get(t, False))), None)
    lb = b.iloc[-lookback:]
    return dict(regime_up=bool(last["regime_up"]),
                wt1=float(last["wt1"]), wt2=float(last["wt2"]),
                last_tag=tag, price=float(last["close"]), ts=b.index[-1],
                long_confirmed=bool(lb["momentum_up"].any()),
                short_confirmed=bool(lb["momentum_down"].any()))


def render(state, trades, b, lookback, src):
    print("=" * 72)
    print(f"X1 LIVE — XAUUSD  {state['ts']:%Y-%m-%d %H:%M}Z   "
          f"price={state['price']:.2f}   [{src}]")
    print("=" * 72)
    print(f"  Regime : {'UP (EMA21>EMA55)' if state['regime_up'] else 'DOWN (EMA21<EMA55)'}")
    print(f"  WaveTrend: wt1={state['wt1']:+6.1f} wt2={state['wt2']:+6.1f} "
          f"({'bull' if state['wt1']>=state['wt2'] else 'bear'})")
    print(f"  Last tag : {state['last_tag'] or '-'}")
    print("  --- confirm-filter read (gold-validated) ---")
    print(f"  LONG  entry now: {'CONFIRMED' if state['long_confirmed'] else 'unconfirmed'} "
          f"(momentum_up in last {lookback} bars)")
    print(f"  SHORT entry now: {'CONFIRMED' if state['short_confirmed'] else 'unconfirmed'} "
          f"(momentum_down in last {lookback} bars)")
    print(f"  base rate: {BASE_RATE}")
    print("  --- open gold trades (live_trades) ---")
    if not trades:
        print("    (none open)")
    for t in trades:
        side_s = str(t.get("side", "")).upper()
        side = 1 if side_s in ("LONG", "BUY") else (-1 if side_s in ("SHORT", "SELL") else 0)
        eng = t.get("engine", "?")
        fam = family_of(eng)
        held = float(t.get("held_sec", 0) or 0)
        pnl = float(t.get("live_pnl", 0) or 0)
        entry_dt = state["ts"] - pd.Timedelta(seconds=held)
        conf = confirm_at(b, entry_dt, lookback, side)
        verdict = ("CONFIRMED" if conf else "UNCONFIRMED") if conf is not None \
                  else "n/a (entry pre-window)"
        flag = "" if fam == "trend" else f"  [filter validated for TREND; engine is '{fam}']"
        print(f"    {eng:24} {side_s:5} held={held/60:5.0f}m pnl={pnl:+8.2f}  "
              f"entry-confirm: {verdict}{flag}")


# --------------------------------------------------------------------------- #
def step(args, here, live, seed_cache):
    tele = None if args.no_gui else fetch_gui(args.gui_url)
    now = dt.datetime.now(dt.timezone.utc)
    src = "offline"
    mid = gold_mid(tele)
    if mid is not None:
        live.push(mid, now)
        src = "GUI-live"

    seed = seed_cache.get("seed")
    if seed is None:
        seed = seed_bars(args.hours, args.bars, not args.no_refresh, here)
        seed_cache["seed"] = seed
    b = merge(seed, live.frame())
    if len(b) < max(X.DEFAULTS["ema_slow"], X.DEFAULTS["wt_n2"]) + 5:
        print(f"[warmup] only {len(b)} bars — waiting for more "
              f"(seed empty? live accumulating)", file=sys.stderr)
        return
    b = X.compute_tags(b, dict(X.DEFAULTS))

    trades = gold_open_trades(tele)
    if not trades and args.trades and os.path.exists(args.trades) and args.no_gui:
        tr = X.load_trades(args.trades, SYMBOL)
        tr = tr[(tr["net_pnl"] != 0) & (tr["entry_dt"] >= b.index[0])]
        trades = [dict(symbol=SYMBOL, engine=r["engine"], side=r["side"],
                       held_sec=(r["exit_dt"] - r["entry_dt"]).total_seconds(),
                       live_pnl=r["net_pnl"]) for _, r in tr.tail(8).iterrows()]

    state = interpret(b, args.lookback)
    render(state, trades, b, args.lookback, src if src != "offline" else "Dukascopy")
    if args.chart:
        X.plot(b.iloc[-args.plot_bars:], None, dict(X.DEFAULTS), args.chart, SYMBOL)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Mac-local live X1 gold overlay")
    ap.add_argument("--gui-url", default="http://localhost:7779")
    ap.add_argument("--no-gui", action="store_true")
    ap.add_argument("--bars", default=None, help="seed bars CSV (default rolling Dukascopy)")
    ap.add_argument("--no-refresh", action="store_true")
    ap.add_argument("--hours", type=int, default=48, help="seed window hours (0=all)")
    ap.add_argument("--lookback", type=int, default=X.DEFAULTS["lookback"])
    ap.add_argument("--trades", default=os.path.expanduser("~/Downloads/omega_trade_closes.csv"))
    ap.add_argument("--chart", default=None)
    ap.add_argument("--plot-bars", dest="plot_bars", type=int, default=400)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--interval", type=int, default=5, help="poll seconds (GUI pushes ~250ms)")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(line_buffering=True)  # stream cleanly when piped
    except Exception:
        pass
    live, seed_cache = LiveBars(), {}
    if not args.loop:
        step(args, here, live, seed_cache)
        return
    print(f"[x1_live] real-time loop every {args.interval}s, GUI={args.gui_url}",
          file=sys.stderr)
    while True:
        try:
            step(args, here, live, seed_cache)
        except SystemExit:
            raise
        except Exception as e:
            print(f"[loop] error: {e}", file=sys.stderr)
        time.sleep(max(1, args.interval))


if __name__ == "__main__":
    main()
