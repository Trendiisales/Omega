#!/usr/bin/env python3
"""ext_retire_thresholds.py — retire-threshold basis for the S-2026-07-17k roster
extensions (turtle ext-14 + dip ext-11). For each PASS name: banked-curve worst
DD episode (cumsum of per-trade net% on $10k notional, 16bp RT — the scan cost).
Family retire bar = ~2x the worst per-name DD across the ext set (the shipped
StockDipTurtle convention: DIP -$9.5k = 2x MU -$4,756; TURTLE -$8.5k = 2x TPR).
"""
import importlib.util, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("scan", os.path.join(HERE, "bigcap45_engine_scan_bt.py"))
scan = importlib.util.module_from_spec(spec)
# suppress the module's load-time prints? module top-level only loads data silently.
spec.loader.exec_module(scan)

EXT_TUR = "MU MRVL PLTR TSLA META NFLX CRWD PANW DELL AMZN GOOGL ASTS RKLB WDC".split()
EXT_DIP = "MRVL PLTR META NFLX SHOP MSTR NOW AMZN GOOGL WDC DD".split()
COST_RT = 0.16  # % — 16bp RT, the scan's per-name cost

def banked_dd(trades):
    """trades: list of (exit_date, pct_net). Returns worst banked-curve DD in % of notional."""
    trades = sorted(trades)
    cum = pk = 0.0; dd = 0.0
    for _, p in trades:
        cum += p
        if cum > pk: pk = cum
        dd = min(dd, cum - pk)
    return dd

def report(tag, names, fn):
    print(f"\n== {tag} (16bp RT, banked-curve DD % of $10k notional) ==")
    worst = 0.0; wname = ""
    for s in names:
        tr = fn(s, COST_RT)
        dd = banked_dd(tr)
        usd = dd / 100.0 * 10000.0
        print(f"  {s:6s} n={len(tr):4d} net={sum(p for _,p in tr):+8.1f}% ddworst={dd:+7.1f}% (${usd:+8.0f})")
        if dd < worst: worst, wname = dd, s
    usd2 = worst / 100.0 * 10000.0 * 2.0
    print(f"  -> worst {wname} {worst:+.1f}% (${worst/100.0*10000.0:+.0f}); 2x retire bar = ${usd2:+.0f}")
    return usd2

if __name__ == "__main__":
    t = report("TURTLE ext-14", EXT_TUR, scan.turtle_trades)
    d = report("DIP ext-11", EXT_DIP, scan.dip_trades)
    print(f"\nRETIRE BARS: turtle-ext {t:+.0f} USD, dip-ext {d:+.0f} USD (round to clean -$X,500/-$X,000)")
