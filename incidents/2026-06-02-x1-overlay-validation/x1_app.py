#!/usr/bin/env python3
"""
x1_app.py — multi-symbol X1 overlay validator / monitor
=======================================================
The buildable app. Runs the X1 momentum-confirm overlay across EVERY symbol we
have bars + real fills for, pools the confirm-gap to beat the single-symbol n
limit, and reports per-symbol / per-family / pooled.

Reuses the fidelity-checked machinery:
  - x1_validate.py  : load_bars, compute_tags, load_trades, trade_context, forward_validation
  - x1_stage1.py    : FAMILY map, family_of, wilson_lo_hi, MAX_SANE_HOLD_SEC

Symbols are declared in a manifest (--manifest CSV: symbol,bars_csv  per line)
or via the built-in DEFAULT_MANIFEST. Each `symbol` must match the value in the
trades file; each `bars_csv` is an OHLC bars file (from pull_dukascopy.py).

Usage
-----
  python3 x1_app.py --trades ~/Downloads/omega_trade_closes.csv
  python3 x1_app.py --trades closes.csv --manifest my_symbols.csv --charts
  python3 x1_app.py --trades closes.csv --only XAUUSD,US500.F

Read-only research tooling. Touches no core/engine code.
"""

import argparse
import os
import sys
import numpy as np
import pandas as pd

import x1_validate as X
from x1_stage1 import family_of, wilson_lo_hi, MAX_SANE_HOLD_SEC

# symbol -> bars CSV (relative to this dir). Broker symbols match the trades file.
DEFAULT_MANIFEST = {
    "XAUUSD":  "XAUUSD_2026-05_m1.csv",
    "US500.F": "US500F_m1.csv",
    "USTEC.F": "USTECF_m1.csv",
    "GER40":   "GER40_m1.csv",
    "GBPUSD":  "GBPUSD_m1.csv",
    "EURUSD":  "EURUSD_m1.csv",
}


def confirm_stats(df):
    """win/los confirm rates + gap for one slice of trade-context rows."""
    w = df[df["net_pnl"] > 0]
    l = df[df["net_pnl"] < 0]
    nw, nl = len(w), len(l)
    cw, cl = int(w["confirming_tag"].sum()), int(l["confirming_tag"].sum())
    fw = (cw / nw * 100) if nw else float("nan")
    fl = (cl / nl * 100) if nl else float("nan")
    gap = (fw - fl) if (nw and nl) else float("nan")
    wlo, whi = wilson_lo_hi(cw, nw)
    llo, lhi = wilson_lo_hi(cl, nl)
    return dict(n=len(df), win=nw, los=nl, win_conf=fw, los_conf=fl, gap=gap,
                win_ci=f"[{wlo*100:3.0f},{whi*100:3.0f}]" if nw else "",
                los_ci=f"[{llo*100:3.0f},{lhi*100:3.0f}]" if nl else "")


def run_symbol(symbol, bars_csv, trades_path, cfg, want_chart, outdir):
    if not os.path.exists(bars_csv):
        print(f"  [skip] {symbol}: bars file not found ({bars_csv})", file=sys.stderr)
        return None
    b = X.load_bars(bars_csv)
    if len(b) < max(cfg["ema_slow"], cfg["wt_n2"]) + 5:
        print(f"  [skip] {symbol}: only {len(b)} bars", file=sys.stderr)
        return None
    b = X.compute_tags(b, cfg)
    trades = X.load_trades(trades_path, symbol)
    trades = trades[trades["net_pnl"] != 0]      # drop paper/zero-pnl rows
    if trades.empty:
        print(f"  [skip] {symbol}: no nonzero fills", file=sys.stderr)
        return None
    ctx = X.trade_context(b, trades, cfg["lookback"])
    ctx = ctx[ctx["dir"] != 0].copy()
    if ctx.empty:
        print(f"  [skip] {symbol}: no fills inside bar window", file=sys.stderr)
        return None
    ctx["symbol"] = symbol
    ctx["family"] = ctx["engine"].map(family_of)
    if want_chart:
        png = os.path.join(outdir, f"x1_{symbol.replace('.', '')}.png")
        X.plot(b.iloc[-cfg.get("plot_bars", 750):], trades, cfg, png, symbol)
    s = confirm_stats(ctx)
    print(f"  {symbol:8} bars={len(b):6}  in-window fills={s['n']:3} "
          f"(W{s['win']}/L{s['los']})  trend-gap pooled below")
    return ctx


def report(all_ctx):
    pd.set_option("display.width", 200)
    fmt = lambda v: f"{v:5.1f}"

    print("\n" + "=" * 80)
    print("POOLED confirm-gap across all symbols  (the n-boosted result)")
    print("=" * 80)
    rows = []
    for fam in ["trend", "meanrev", "scalp", "straddle", "other"]:
        d = all_ctx[all_ctx["family"] == fam]
        if len(d):
            r = confirm_stats(d); r["group"] = fam; rows.append(r)
    r = confirm_stats(all_ctx); r["group"] = "ALL"; rows.append(r)
    t = pd.DataFrame(rows)
    with pd.option_context("display.float_format", fmt):
        print(t[["group", "n", "win", "los", "win_conf", "win_ci",
                 "los_conf", "los_ci", "gap"]].to_string(index=False))

    print("\n" + "-" * 80)
    print("Per-symbol trend-family confirm-gap (the buildable signal, by symbol)")
    print("-" * 80)
    rows = []
    for sym, d in all_ctx[all_ctx["family"] == "trend"].groupby("symbol"):
        r = confirm_stats(d); r["symbol"] = sym; rows.append(r)
    if rows:
        with pd.option_context("display.float_format", fmt):
            print(pd.DataFrame(rows)[["symbol", "n", "win", "los",
                  "win_conf", "los_conf", "gap"]].to_string(index=False))
    else:
        print("  (no trend-family fills)")

    print("\n" + "-" * 80)
    print("Per-symbol ALL-family confirm-gap")
    print("-" * 80)
    rows = []
    for sym, d in all_ctx.groupby("symbol"):
        r = confirm_stats(d); r["symbol"] = sym; rows.append(r)
    with pd.option_context("display.float_format", fmt):
        print(pd.DataFrame(rows)[["symbol", "n", "win", "los",
              "win_conf", "los_conf", "gap"]].to_string(index=False))


def main():
    ap = argparse.ArgumentParser(description="Multi-symbol X1 overlay validator")
    ap.add_argument("--trades", required=True)
    ap.add_argument("--manifest", default=None,
                    help="CSV with 'symbol,bars_csv' lines; default = built-in")
    ap.add_argument("--only", default=None, help="comma-list of symbols to include")
    ap.add_argument("--lookback", type=int, default=X.DEFAULTS["lookback"])
    ap.add_argument("--tf", type=int, default=X.DEFAULTS["tf"])
    ap.add_argument("--charts", action="store_true")
    ap.add_argument("--outdir", default=".")
    args = ap.parse_args()

    cfg = dict(X.DEFAULTS); cfg["lookback"] = args.lookback; cfg["tf"] = args.tf

    if args.manifest:
        m = {}
        for line in open(args.manifest):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            sym, _, path = line.partition(",")
            m[sym.strip()] = path.strip()
    else:
        m = dict(DEFAULT_MANIFEST)
    if args.only:
        keep = {s.strip() for s in args.only.split(",")}
        m = {k: v for k, v in m.items() if k in keep}

    print(f"=== X1 multi-symbol overlay ===  trades={args.trades}  "
          f"lookback={cfg['lookback']} tf=M{cfg['tf']}  symbols={list(m)}")
    parts = []
    for sym, path in m.items():
        ctx = run_symbol(sym, path, args.trades, cfg, args.charts, args.outdir)
        if ctx is not None:
            parts.append(ctx)
    if not parts:
        raise SystemExit("No symbols produced in-window fills — check bars files.")
    all_ctx = pd.concat(parts, ignore_index=True)
    report(all_ctx)
    print(f"\nTotal pooled in-window fills: {len(all_ctx)} across "
          f"{all_ctx['symbol'].nunique()} symbols.")


if __name__ == "__main__":
    main()
