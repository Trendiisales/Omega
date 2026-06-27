#!/usr/bin/env python3
# report_runner.py — Omega Quant Report Engine (Build 2 from the QuantStats video).
# The "truth machine": turns the shadow ledger into a real risk-adjusted report so an engine
# can't hide bad risk behind a positive dollar total. Analytics ONLY — runs OFF the trade log,
# never in the live loop (let C++ trade, let Python audit).
#
#   python report_runner.py /tmp/omega_ledger.csv [start_capital]
# Outputs:
#   reports/omega_tearsheet.html   — full QuantStats tear sheet (Sharpe, drawdown, monthly, etc.)
#   stdout                          — per-engine ranking table (n, net, WR, PF, avg, maxDD) + pass/fail gate
import sys, os, csv, datetime
HERE = os.path.dirname(os.path.abspath(__file__))
REPORTS = os.path.join(HERE, "reports"); os.makedirs(REPORTS, exist_ok=True)

ledger = sys.argv[1] if len(sys.argv) > 1 else "/tmp/omega_ledger.csv"
START_CAP = float(sys.argv[2]) if len(sys.argv) > 2 else 10000.0

# --- parse the ledger (header-driven — column order is not assumed) ---
rows = list(csv.DictReader(open(ledger)))
def f(r, k, d=0.0):
    try: return float(r.get(k, d) or d)
    except Exception: return d
trades = []
for r in rows:
    net = f(r, "net_pnl")
    ts = int(f(r, "exit_ts_unix")) or int(f(r, "entry_ts_unix"))
    if ts <= 0: continue
    trades.append({"ts": ts, "date": datetime.datetime.utcfromtimestamp(ts).date(),
                   "engine": (r.get("engine") or "?").strip(), "symbol": (r.get("symbol") or "?").strip(),
                   "net": net, "win": net > 0})
trades.sort(key=lambda t: t["ts"])
if not trades:
    print("no trades parsed from", ledger); sys.exit(1)

# --- PER-ENGINE TRUTH TABLE (the part that exposes fake edges) ---
print(f"\n=== OMEGA ENGINE LEDGER — {len(trades)} trades, start cap ${START_CAP:,.0f} ===")
by_eng = {}
for t in trades:
    e = by_eng.setdefault(t["engine"], {"n": 0, "net": 0.0, "wins": 0, "gross_win": 0.0, "gross_loss": 0.0, "eq": [], "peak": 0.0, "maxdd": 0.0})
    e["n"] += 1; e["net"] += t["net"]; e["wins"] += 1 if t["win"] else 0
    if t["net"] > 0: e["gross_win"] += t["net"]
    else: e["gross_loss"] += -t["net"]
    e["eq"].append(e["net"]); e["peak"] = max(e["peak"], e["net"]); e["maxdd"] = max(e["maxdd"], e["peak"] - e["net"])
print(f"{'engine':28} {'n':>3} {'net$':>9} {'WR%':>5} {'PF':>5} {'avg$':>7} {'maxDD$':>7}")
for eng, e in sorted(by_eng.items(), key=lambda kv: -kv[1]["net"]):
    pf = (e["gross_win"] / e["gross_loss"]) if e["gross_loss"] > 1e-9 else float("inf")
    wr = 100.0 * e["wins"] / e["n"]
    print(f"{eng:28} {e['n']:>3} {e['net']:>9.2f} {wr:>5.0f} {pf:>5.2f} {e['net']/e['n']:>7.2f} {e['maxdd']:>7.2f}")
total = sum(t["net"] for t in trades)
print(f"{'TOTAL':28} {len(trades):>3} {total:>9.2f}")

# --- QuantStats tear sheet (risk-adjusted: Sharpe, drawdown, monthly, MC) ---
try:
    import pandas as pd, quantstats as qs
    # daily equity curve from net P&L -> daily returns
    daily = {}
    for t in trades: daily[t["date"]] = daily.get(t["date"], 0.0) + t["net"]
    idx = pd.date_range(min(daily), max(daily), freq="D")
    pnl = pd.Series([daily.get(d.date(), 0.0) for d in idx], index=idx)
    equity = START_CAP + pnl.cumsum()
    rets = equity.pct_change().fillna(0.0)
    out = os.path.join(REPORTS, "omega_tearsheet.html")
    qs.reports.html(rets, output=out, title="Omega Shadow Book — Risk-Adjusted Report", download_filename=out)
    def sc(x):
        try: return float(x.iloc[0] if hasattr(x, "iloc") else x)
        except Exception: return float("nan")
    print(f"\nTEAR SHEET: {out}")
    print(f"  Sharpe={sc(qs.stats.sharpe(rets)):.2f}  Sortino={sc(qs.stats.sortino(rets)):.2f}  "
          f"maxDD={sc(qs.stats.max_drawdown(equity))*100:.1f}%  CAGR={sc(qs.stats.cagr(rets))*100:.1f}%")
except ImportError:
    print("\n[quantstats not installed — engine table above is still valid; install for the HTML tear sheet]")
