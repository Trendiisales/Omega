#!/usr/bin/env python3
"""
x1_live.py — Mac-local live X1 overlay & interpreter (GOLD-FIRST)
=================================================================
Runs the gold-validated X1 momentum-confirm overlay on the Mac, reading live
state from the Omega GUI telemetry (localhost:7779) and rendering the correct
interpretation per the validation findings.

Design (per operator decisions 2026-06-02):
  - Data source : the Omega GUI on this Mac (http://localhost:7779/api/telemetry).
                  Supplies open gold positions (entry/side/engine/held_sec/pnl)
                  and the live gold price (live_trades[].current).
  - Price series: WaveTrend needs continuous bars, which the GUI does NOT serve.
                  Base = rolling Dukascopy XAUUSD M1 history; the latest bar's
                  close is ANCHORED to the GUI live price when a gold trade is
                  open (operator choice: "GUI current price anchor").
  - Scope       : XAUUSD only. The confirm-filter edge is gold-specific
                  (X1_MULTISYMBOL_FINDINGS.md). Do NOT extend to indices.

Graceful degradation: if the GUI is unreachable, the oscillator still runs from
Dukascopy, and trade interpretation falls back to the ledger CSV (--trades).

Interpretation it prints (the "correct interpretation"):
  - Current regime (EMA21 vs EMA55) + WaveTrend wt1/wt2 + last tag.
  - "Would a long/short entry RIGHT NOW be momentum-confirmed?" + the validated
    gold base rate (confirmed trend winners 71.9% vs losers 51.5%; +12pp
    within-trend per-trade edge — see X1_STAGE1_FINDINGS.md).
  - For each OPEN gold trade: was a confirming momentum tag present in the
    `lookback` bars before entry? -> CONFIRMED / UNCONFIRMED, with live P&L.

Read-only. Touches no core/engine code; never sends orders.

Usage
-----
  python3 x1_live.py                       # one snapshot (GUI + Dukascopy)
  python3 x1_live.py --loop --interval 60  # refresh every 60s
  python3 x1_live.py --gui-url http://localhost:7779 --chart x1_live.png
  python3 x1_live.py --no-gui --trades ~/Downloads/omega_trade_closes.csv  # offline
"""

import argparse
import json
import os
import sys
import time
import subprocess
import urllib.request
import pandas as pd

import x1_validate as X
from x1_stage1 import family_of

SYMBOL = "XAUUSD"
DUKAS_SCALE = 1000
# Validated gold base rates (X1_STAGE1_FINDINGS.md). Shown with every verdict so
# the interpretation is honest about strength, not just direction.
BASE_RATE = "gold confirmed-trend winners 71.9% vs losers 51.5% (+12pp within-trend, ~2 SE)"


# --------------------------------------------------------------------------- #
# GUI telemetry
# --------------------------------------------------------------------------- #
def fetch_gui(gui_url, timeout=4):
    """Return parsed /api/telemetry JSON, or None if unreachable."""
    try:
        req = urllib.request.Request(gui_url.rstrip("/") + "/api/telemetry",
                                     headers={"User-Agent": "x1_live"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception as e:
        print(f"[gui] unreachable ({e}); degrading to Dukascopy+ledger",
              file=sys.stderr)
        return None


def gui_gold_trades(tele):
    """Open XAUUSD trades from the telemetry live_trades array."""
    if not tele:
        return [], None
    out, live_px = [], None
    for t in tele.get("live_trades", []):
        if SYMBOL not in str(t.get("symbol", "")):
            continue
        cur = t.get("current")
        if cur:
            live_px = float(cur)
        out.append(t)
    return out, live_px


# --------------------------------------------------------------------------- #
# Price series: rolling Dukascopy base + GUI live anchor
# --------------------------------------------------------------------------- #
def rolling_bars(hours_back, bars_csv, refresh, here):
    """Load (and optionally refresh) a rolling XAUUSD M1 bars file.

    To keep this dependency-light we reuse pull_dukascopy.py. If `refresh` and
    the file is missing or older than ~10 min, re-pull the last `hours_back`h.
    Dukascopy lags real time by a few minutes — acceptable for M1 gold context;
    the GUI anchor closes that last-mile gap on the latest bar.
    """
    path = bars_csv or os.path.join(here, "XAUUSD_2026-05_m1.csv")
    need = refresh and (not os.path.exists(path) or
                        (time.time() - os.path.getmtime(path)) > 600)
    if need:
        # window: today-ish. We pull a generous span and let load_bars tail it.
        # (date math without Date.now is fine here — this is the Mac, not a
        # workflow script.)
        import datetime as dt
        to = dt.datetime.utcnow().date()
        frm = to - dt.timedelta(days=max(2, hours_back // 24 + 1))
        pull = os.path.join(here, "pull_dukascopy.py")
        cmd = [sys.executable, pull, "--symbol", "XAUUSD",
               "--from", frm.isoformat(), "--to", to.isoformat(),
               "--scale", str(DUKAS_SCALE), "--out", path, "--workers", "16"]
        print(f"[price] refreshing Dukascopy M1 {frm}..{to} -> {path}",
              file=sys.stderr)
        subprocess.run(cmd, cwd=here, check=False)
    if not os.path.exists(path):
        raise SystemExit(f"No bars file ({path}) and refresh failed/disabled.")
    b = X.load_bars(path)
    return b.iloc[-(hours_back * 60):] if hours_back else b


def anchor_live(bars, live_px):
    """Overwrite the latest bar's close with the GUI live price (and stretch
    high/low to contain it) so the freshest WaveTrend value reflects now."""
    if live_px is None or bars.empty:
        return bars
    b = bars.copy()
    i = b.index[-1]
    b.loc[i, "close"] = live_px
    b.loc[i, "high"] = max(b.loc[i, "high"], live_px)
    b.loc[i, "low"] = min(b.loc[i, "low"], live_px)
    return b


# --------------------------------------------------------------------------- #
# Interpretation
# --------------------------------------------------------------------------- #
def confirm_at(b, entry_dt, lookback, side):
    """Was a confirming momentum tag present in `lookback` bars before entry?"""
    idx = b.index
    if entry_dt < idx[0] or entry_dt > idx[-1]:
        return None
    pos = idx.searchsorted(entry_dt, side="right") - 1
    if pos < 0:
        return None
    lo = max(0, pos - lookback)
    win = b.iloc[lo:pos + 1]
    if side > 0:
        return bool(win["momentum_up"].any())
    if side < 0:
        return bool(win["momentum_down"].any())
    return None


def interpret(b, lookback):
    """Current overlay state + the 'would an entry now be confirmed' read."""
    last = b.iloc[-1]
    regime_up = bool(last["regime_up"])
    wt1, wt2 = float(last["wt1"]), float(last["wt2"])
    tag = None
    for t in ("momentum_up", "momentum_down", "retr_down", "retr_up"):
        if bool(last.get(t, False)):
            tag = t
            break
    lb = b.iloc[-lookback:]
    long_conf = bool(lb["momentum_up"].any())
    short_conf = bool(lb["momentum_down"].any())
    return dict(regime_up=regime_up, wt1=wt1, wt2=wt2, last_tag=tag,
                price=float(last["close"]), ts=b.index[-1],
                long_confirmed=long_conf, short_confirmed=short_conf)


def render(state, gold_trades, b, lookback, gui_live):
    print("=" * 72)
    print(f"X1 LIVE — XAUUSD  {state['ts']:%Y-%m-%d %H:%M}Z   "
          f"price={state['price']:.2f}" + ("  [GUI-anchored]" if gui_live else "  [Dukascopy]"))
    print("=" * 72)
    reg = "UP (EMA21>EMA55)" if state["regime_up"] else "DOWN (EMA21<EMA55)"
    print(f"  Regime : {reg}")
    print(f"  WaveTrend: wt1={state['wt1']:+6.1f}  wt2={state['wt2']:+6.1f}  "
          f"({'wt1>wt2 bull' if state['wt1']>=state['wt2'] else 'wt1<wt2 bear'})")
    print(f"  Last tag : {state['last_tag'] or '-'}")
    print("  --- confirm-filter read (gold-validated) ---")
    print(f"  A LONG  entry now: {'CONFIRMED' if state['long_confirmed'] else 'unconfirmed'} "
          f"(momentum_up in last {lookback} bars)")
    print(f"  A SHORT entry now: {'CONFIRMED' if state['short_confirmed'] else 'unconfirmed'} "
          f"(momentum_down in last {lookback} bars)")
    print(f"  base rate: {BASE_RATE}")

    print("  --- open gold trades (from GUI) ---")
    if not gold_trades:
        print("    (none open, or GUI offline)")
    for t in gold_trades:
        side_s = str(t.get("side", "")).upper()
        side = 1 if side_s in ("LONG", "BUY") else (-1 if side_s in ("SHORT", "SELL") else 0)
        eng = t.get("engine", "?")
        fam = family_of(eng)
        held = t.get("held_sec", 0)
        pnl = t.get("live_pnl", 0)
        # entry time: GUI gives held_sec; derive entry bar from now-held
        entry_dt = state["ts"] - pd.Timedelta(seconds=float(held or 0))
        conf = confirm_at(b, entry_dt, lookback, side)
        verdict = ("CONFIRMED" if conf else "UNCONFIRMED") if conf is not None \
                  else "n/a (entry outside bar window)"
        applies = "" if fam == "trend" else f"  [filter validated for TREND family; this is '{fam}']"
        print(f"    {eng:24} {side_s:5} held={float(held or 0)/60:5.0f}m "
              f"pnl={float(pnl or 0):+8.2f}  entry-confirm: {verdict}{applies}")


# --------------------------------------------------------------------------- #
def snapshot(args, here):
    tele = None if args.no_gui else fetch_gui(args.gui_url)
    gold_trades, live_px = gui_gold_trades(tele)
    b = rolling_bars(args.hours, args.bars, not args.no_refresh, here)
    if live_px is not None:
        b = anchor_live(b, live_px)
    b = X.compute_tags(b, dict(X.DEFAULTS))

    # offline fallback: if no GUI trades, interpret recent ledger gold trades
    if not gold_trades and args.trades and os.path.exists(args.trades):
        tr = X.load_trades(args.trades, SYMBOL)
        tr = tr[(tr["net_pnl"] != 0) & (tr["entry_dt"] >= b.index[0])]
        gold_trades = [dict(symbol=SYMBOL, engine=r["engine"], side=r["side"],
                            held_sec=(r["exit_dt"] - r["entry_dt"]).total_seconds(),
                            live_pnl=r["net_pnl"]) for _, r in tr.tail(8).iterrows()]
        if gold_trades:
            print("[interp] GUI offline — interpreting recent ledger gold trades",
                  file=sys.stderr)

    state = interpret(b, args.lookback)
    render(state, gold_trades, b, args.lookback, gui_live=(live_px is not None))
    if args.chart:
        trades_df = None
        X.plot(b.iloc[-args.plot_bars:], trades_df, dict(X.DEFAULTS), args.chart, SYMBOL)
        print(f"\n[chart] {args.chart}")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Mac-local live X1 gold overlay")
    ap.add_argument("--gui-url", default="http://localhost:7779")
    ap.add_argument("--no-gui", action="store_true", help="skip GUI; Dukascopy+ledger only")
    ap.add_argument("--bars", default=None, help="bars CSV (default rolling XAUUSD M1)")
    ap.add_argument("--no-refresh", action="store_true", help="don't re-pull Dukascopy")
    ap.add_argument("--hours", type=int, default=48, help="rolling bar window (hours)")
    ap.add_argument("--lookback", type=int, default=X.DEFAULTS["lookback"])
    ap.add_argument("--trades", default=os.path.expanduser("~/Downloads/omega_trade_closes.csv"))
    ap.add_argument("--chart", default=None, help="write PNG snapshot")
    ap.add_argument("--plot-bars", dest="plot_bars", type=int, default=400)
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--interval", type=int, default=60)
    args = ap.parse_args()

    if not args.loop:
        snapshot(args, here)
        return
    while True:
        try:
            snapshot(args, here)
        except SystemExit:
            raise
        except Exception as e:
            print(f"[loop] error: {e}", file=sys.stderr)
        time.sleep(max(5, args.interval))


if __name__ == "__main__":
    main()
