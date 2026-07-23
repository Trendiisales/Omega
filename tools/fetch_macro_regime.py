#!/usr/bin/env python3
"""Fetch the index macro-regime signals and write them where the live engines'
portfolio risk-gate reads them: data/index_regime.txt

    one line:  epoch_sec,vix_ratio,credit_mom,dxy_mom

      vix_ratio  = ^VIX / ^VIX3M                       (>=1.05 => backwardation)
      credit_mom = (HYG/HYG[-20d]-1) + (LQD/LQD[-20d]-1)   (<0 => credit deteriorating)
      dxy_mom    = DX-Y.NYB / DX-Y.NYB[-20d] - 1           (>0 => USD strengthening)

WHY: include/IndexRiskGate.hpp suppresses NEW index-engine entries when the macro
backdrop is risk-off (VIX backwardation OR credit deteriorating OR dollar rising).
Validated (index_macro_regime.cpp): 6-index EW basket Sharpe 0.63->1.02 maxDD -61%;
seasonal sleeve 1.13->1.27 maxDD -67%. Broker FIX lacks VIX3M + credit ETFs, so
this external fetch supplies them.

Also writes data/vix_term_ratio.txt ("epoch_sec,ratio") for IndexSeasonalEngine's
own per-instance VIX gate (back-compat).

DEPLOY (VPS, operator): schedule daily after the US close (e.g. 21:30 UTC) via
Windows Task Scheduler:  python C:\\Omega\\tools\\fetch_macro_regime.py
Engines apply a 4-day staleness guard -> a missed run / dead network degrades
gracefully to risk-on (no halt). Requires: pip install yfinance pandas.
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")

def mom(series, lookback=20):
    if len(series) <= lookback: return 0.0
    return float(series.iloc[-1]) / float(series.iloc[-1-lookback]) - 1.0

def main():
    try:
        import yfinance as yf
    except Exception as e:
        print(f"[fetch_macro_regime] yfinance missing: {e}", file=sys.stderr); sys.exit(1)
    syms = ["^VIX", "^VIX3M", "HYG", "LQD", "DX-Y.NYB"]
    d = yf.download(syms, period="60d", interval="1d", progress=False, auto_adjust=False)["Close"].dropna()
    if d.empty or len(d) < 22:
        print("[fetch_macro_regime] insufficient data", file=sys.stderr); sys.exit(1)
    last = d.iloc[-1]
    vix, vix3m = float(last["^VIX"]), float(last["^VIX3M"])
    if vix <= 0 or vix3m <= 0:
        print("[fetch_macro_regime] bad VIX", file=sys.stderr); sys.exit(1)
    vix_ratio  = vix / vix3m
    credit_mom = mom(d["HYG"]) + mom(d["LQD"])
    dxy_mom    = mom(d["DX-Y.NYB"])
    ts = int(d.index[-1].timestamp())

    os.makedirs(DATA, exist_ok=True)
    with open(os.path.join(DATA, "index_regime.txt"), "w") as f:
        f.write(f"{ts},{vix_ratio:.5f},{credit_mom:+.5f},{dxy_mom:+.5f}\n")
    with open(os.path.join(DATA, "vix_term_ratio.txt"), "w") as f:
        f.write(f"{ts},{vix_ratio:.5f}\n")
    print(f"[fetch_macro_regime] ts={ts} vix_ratio={vix_ratio:.4f} credit_mom={credit_mom:+.4f} dxy_mom={dxy_mom:+.4f}"
          f"  -> risk_off={'YES' if (vix_ratio>=1.05 or credit_mom<0 or dxy_mom>0) else 'no'}")

    # S-2026-07-17k: SPY close history for the BigCapHi52Engine SPY-200DMA gate
    # (data/spy_close_hist.csv, "ts_sec,close" rows, full rewrite nightly; the
    # engine FREEZES — holds members, blocks rebals — if this goes stale >6d,
    # so a failed pull degrades gracefully, never liquidates the book).
    # S-2026-07-23: + NDX/SPX INDEX close histories for the index-ladder 200DMA
    # bull gates (gate-source robustness fix: ONE consistent nightly-refreshed
    # single-source series per index, same contract as spy_close_hist — full
    # rewrite so there is never a scale seam; gates fail-CLOSED on stale/short).
    for tick, fname in (("SPY", "spy_close_hist.csv"),
                        ("^NDX", "ndx_close_hist.csv"),
                        ("^GSPC", "spx_close_hist.csv")):
        try:
            s = yf.download(tick, period="400d", interval="1d", progress=False, auto_adjust=False)["Close"].dropna()
            if hasattr(s, "squeeze"):
                s = s.squeeze()
            if len(s) >= 250:
                with open(os.path.join(DATA, fname), "w") as f:
                    for ts_i, px in s.items():
                        f.write(f"{int(ts_i.timestamp())},{float(px):.4f}\n")
                print(f"[fetch_macro_regime] {fname}: {len(s)} rows, last={float(s.iloc[-1]):.2f}")
            else:
                print(f"[fetch_macro_regime] {tick} pull too short ({len(s)}) — {fname} NOT rewritten", file=sys.stderr)
        except Exception as e:
            print(f"[fetch_macro_regime] {tick} pull failed ({e}) — {fname} NOT rewritten", file=sys.stderr)

if __name__ == "__main__":
    main()
