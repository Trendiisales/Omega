#!/usr/bin/env python3
"""
shadow_analysis.py
==================
Analyses Omega shadow CSV to produce per-engine, per-symbol, and combined
performance statistics.

Metrics (all net after slippage + commission):
  n, win_rate, expectancy, sharpe, sortino, calmar, profit_factor,
  max_drawdown, avg_hold, autocorrelation(lag-1), p_value (binomial),
  edge_significant (p < 0.05)

Extra sections:
  - Statistical significance table per engine
  - Walk-forward OOS validation (--wfo flag)
  - Drawdown velocity audit
  - Daily session results

Shadow CSV columns:
  entryTs, symbol, side, engine, entryPrice, exitPrice,
  pnl, mfe, mae, hold_sec, exitReason, spreadAtEntry, latencyMs, regime

Usage:
  python shadow_analysis.py
  python shadow_analysis.py path/to/omega_shadow.csv
  python shadow_analysis.py --days 14
  python shadow_analysis.py --min-trades 5
  python shadow_analysis.py --csv
  python shadow_analysis.py --plot
  python shadow_analysis.py --wfo
"""

import sys
import os
import csv
import math
import argparse
import datetime
from collections import defaultdict
from typing import List, Dict, Tuple, Optional


COLUMNS = [
    "entryTs", "symbol", "side", "engine", "entryPrice", "exitPrice",
    "pnl", "mfe", "mae", "hold_sec", "exitReason", "spreadAtEntry",
    "latencyMs", "regime"
]

class Trade:
    __slots__ = COLUMNS
    def __init__(self, row: dict):
        self.entryTs       = int(row.get("entryTs", 0) or 0)
        self.symbol        = row.get("symbol", "UNKNOWN").strip()
        self.side          = row.get("side", "").strip().upper()
        self.engine        = row.get("engine", "UNKNOWN").strip()
        self.entryPrice    = float(row.get("entryPrice", 0) or 0)
        self.exitPrice     = float(row.get("exitPrice", 0) or 0)
        self.pnl           = float(row.get("pnl", 0) or 0)
        self.mfe           = float(row.get("mfe", 0) or 0)
        self.mae           = float(row.get("mae", 0) or 0)
        self.hold_sec      = float(row.get("hold_sec", 0) or 0)
        self.exitReason    = row.get("exitReason", "").strip()
        self.spreadAtEntry = float(row.get("spreadAtEntry", 0) or 0)
        self.latencyMs     = float(row.get("latencyMs", 0) or 0)
        self.regime        = row.get("regime", "").strip()


# ── Statistics ────────────────────────────────────────────────────────────────

def _avg_hold(trades):
    return max(sum(t.hold_sec for t in trades) / max(len(trades), 1), 1.0)

def _tpy(avg_hold_s):
    return (250.0 * 8 * 3600.0) / max(avg_hold_s, 1.0)

def sortino_ratio(pnls, avg_hold_s):
    if len(pnls) < 4: return 0.0
    mean = sum(pnls) / len(pnls)
    down_var = sum(v * v for v in pnls if v < 0) / len(pnls)
    down_std = math.sqrt(down_var)
    if down_std < 1e-9: return 99.0
    return (mean / down_std) * math.sqrt(_tpy(avg_hold_s))

def calmar_ratio(pnls, avg_hold_s):
    if len(pnls) < 4: return 0.0
    mean   = sum(pnls) / len(pnls)
    annual = mean * _tpy(avg_hold_s)
    cum = peak = max_dd = 0.0
    for v in pnls:
        cum += v; peak = max(peak, cum); max_dd = max(max_dd, peak - cum)
    return (annual / max_dd) if max_dd > 1e-9 else 99.0

def autocorr_lag1(pnls):
    if len(pnls) < 6: return 0.0
    mean = sum(pnls) / len(pnls)
    cov  = sum((pnls[i] - mean) * (pnls[i-1] - mean) for i in range(1, len(pnls)))
    var  = sum((v - mean) ** 2 for v in pnls[1:])
    return (cov / var) if var > 1e-9 else 0.0

def binom_pvalue(wins, n):
    if n < 10: return 1.0
    z = (wins - n * 0.5) / math.sqrt(n * 0.25)
    return 0.5 * math.erfc(z / math.sqrt(2.0))

def stats(trades):
    if not trades: return {}
    pnls   = [t.pnl for t in trades]
    n      = len(pnls)
    wins   = [p for p in pnls if p > 0]
    losses = [p for p in pnls if p <= 0]
    wr     = len(wins) / n
    aw     = sum(wins) / len(wins)    if wins   else 0.0
    al     = abs(sum(losses) / len(losses)) if losses else 0.0
    exp    = wr * aw - (1 - wr) * al
    pf     = (sum(wins) / abs(sum(losses))) if losses and sum(losses) != 0 else float('inf')
    mean   = sum(pnls) / n
    var    = sum((p - mean)**2 for p in pnls) / n if n > 1 else 0.0
    std    = math.sqrt(var)
    ahs    = _avg_hold(trades)
    sharpe = (mean / std * math.sqrt(_tpy(ahs))) if std > 1e-9 else 0.0
    cum = peak = max_dd = 0.0
    for p in pnls:
        cum += p; peak = max(peak, cum); max_dd = max(max_dd, peak - cum)
    p_val = binom_pvalue(len(wins), n)
    return {
        "n": n, "wr": wr, "total_pnl": sum(pnls),
        "avg_win": aw, "avg_loss": al, "expectancy": exp,
        "profit_factor": pf, "sharpe": sharpe,
        "sortino": sortino_ratio(pnls, ahs),
        "calmar":  calmar_ratio(pnls, ahs),
        "max_dd": max_dd,
        "avg_hold": _fmt_hold(ahs),
        "n_wins": len(wins), "n_losses": len(losses),
        "autocorr": autocorr_lag1(pnls),
        "p_value": p_val,
        "significant": p_val < 0.05,
    }

def _fmt_hold(s):
    s = int(s)
    if s < 60:   return f"{s}s"
    if s < 3600: return f"{s//60}m{s%60:02d}s"
    return f"{s//3600}h{(s%3600)//60:02d}m"


# ── ANSI colours ──────────────────────────────────────────────────────────────

_UC = sys.stdout.isatty() and os.name != 'nt'
RS = "\033[0m"  if _UC else ""
GR = "\033[32m" if _UC else ""
AM = "\033[33m" if _UC else ""
RD = "\033[31m" if _UC else ""
BD = "\033[1m"  if _UC else ""
DM = "\033[2m"  if _UC else ""

def cwr(v):
    if not _UC: return ""
    return GR if v >= 0.55 else AM if v >= 0.45 else RD

def cval(v):
    if not _UC: return ""
    return GR if v > 0 else AM if v > -5 else RD

def cpval(p):
    if not _UC: return ""
    return GR if p < 0.05 else AM if p < 0.10 else RD

def cac(a):
    if not _UC: return ""
    return RD if a > 0.20 else AM if a > 0.10 else GR


# ── Report ────────────────────────────────────────────────────────────────────

HDR = (f"{'GROUP':<28} {'N':>5} {'WR':>6} {'EXP':>8} "
       f"{'SHP':>6} {'SRT':>6} {'CAL':>6} "
       f"{'PF':>6} {'TOTAL$':>9} {'MDD':>7} "
       f"{'AC':>5} {'p':>6} {'SIG':>4} {'HOLD':>8}")
SEP = "-" * len(HDR)

def row(label, s, indent=0):
    if not s: return ""
    pfs  = f"{s['profit_factor']:6.2f}" if s['profit_factor'] != float('inf') else "   inf"
    pad  = "  " * indent
    sig  = f"{GR}YES{RS}" if s['significant'] else f"{DM} no{RS}"
    return (
        f"{pad}{label:<{28-2*indent}} "
        f"{s['n']:>5} "
        f"{cwr(s['wr'])}{s['wr']*100:5.1f}%{RS} "
        f"{cval(s['expectancy'])}{s['expectancy']:>8.2f}{RS} "
        f"{cval(s['sharpe'])}{s['sharpe']:>6.2f}{RS} "
        f"{cval(s['sortino'])}{s['sortino']:>6.2f}{RS} "
        f"{cval(s['calmar'])}{s['calmar']:>6.2f}{RS} "
        f"{pfs} "
        f"{s['total_pnl']:>9.2f} "
        f"{s['max_dd']:>7.2f} "
        f"{cac(s['autocorr'])}{s['autocorr']:>5.2f}{RS} "
        f"{cpval(s['p_value'])}{s['p_value']:>6.3f}{RS} "
        f"{sig:>4} "
        f"{s['avg_hold']:>8}"
    )

def print_report(trades, min_trades):
    print()
    print(f"  {BD}OMEGA SHADOW ANALYSIS{RS}  ({len(trades)} total trades)")
    print(f"  Generated: {datetime.datetime.utcnow().strftime('%Y-%m-%d %H:%M UTC')}")
    print()

    print(HDR); print(SEP)
    print(row("ALL TRADES", stats(trades))); print(SEP); print()

    by_eng = defaultdict(list)
    for t in trades: by_eng[t.engine].append(t)
    print("  BY ENGINE"); print(HDR); print(SEP)
    for e in sorted(by_eng):
        if len(by_eng[e]) >= min_trades: print(row(e, stats(by_eng[e])))
    print(SEP); print()

    by_sym = defaultdict(list)
    for t in trades: by_sym[t.symbol].append(t)
    print("  BY SYMBOL"); print(HDR); print(SEP)
    for s in sorted(by_sym):
        if len(by_sym[s]) >= min_trades: print(row(s, stats(by_sym[s])))
    print(SEP); print()

    by_es = defaultdict(list)
    for t in trades: by_es[(t.engine, t.symbol)].append(t)
    print("  BY ENGINE × SYMBOL"); print(HDR); print(SEP)
    cur_eng = None
    for (e, s) in sorted(by_es):
        if len(by_es[(e,s)]) < min_trades: continue
        if e != cur_eng:
            if len(by_eng[e]) >= min_trades: print(row(f"[{e}]", stats(by_eng[e])))
            cur_eng = e
        print(row(s, stats(by_es[(e,s)]), indent=1))
    print(SEP); print()

    by_exit = defaultdict(list)
    for t in trades: by_exit[t.exitReason].append(t)
    print("  BY EXIT REASON"); print(HDR); print(SEP)
    for r in sorted(by_exit, key=lambda x: -len(by_exit[x])):
        if len(by_exit[r]) >= min_trades: print(row(r or "(unknown)", stats(by_exit[r])))
    print(SEP); print()

    by_reg = defaultdict(list)
    for t in trades: by_reg[t.regime or "UNKNOWN"].append(t)
    if len(by_reg) > 1:
        print("  BY MACRO REGIME"); print(HDR); print(SEP)
        for r in sorted(by_reg, key=lambda x: -len(by_reg[x])):
            if len(by_reg[r]) >= min_trades: print(row(r, stats(by_reg[r])))
        print(SEP); print()

    _sig_table(trades, by_eng, min_trades)
    _daily(trades)
    _velocity_audit(trades)


def _sig_table(trades, by_eng, min_trades):
    print("  STATISTICAL SIGNIFICANCE  (binomial WR > 50%, Bayesian shrinkage prior_n=20)")
    print(f"  {'ENGINE':<28} {'N':>5}  {'RAW_WR':>7}  {'SHRUNK':>7}  {'p-val':>7}  STATUS             ACTION")
    print(f"  {'-'*88}")
    for e in sorted(by_eng):
        ts = by_eng[e]
        if len(ts) < min_trades: continue
        n    = len(ts)
        wins = sum(1 for t in ts if t.pnl > 0)
        raw  = wins / n
        shr  = (n * raw + 20 * 0.50) / (n + 20)
        p    = binom_pvalue(wins, n)
        if p < 0.05:   st, act = f"{GR}SIGNIFICANT{RS}", "OK to scale"
        elif p < 0.10: st, act = f"{AM}MARGINAL{RS}   ", "Accumulate more trades"
        else:          st, act = f"{RD}NOT SIG{RS}    ", "Do NOT increase size"
        print(f"  {e:<28} {n:>5}  {raw*100:>6.1f}%  {shr*100:>6.1f}%  {cpval(p)}{p:>7.3f}{RS}  {st}  {act}")
    print()


def _daily(trades):
    by_day = defaultdict(float)
    for t in trades:
        by_day[datetime.datetime.utcfromtimestamp(t.entryTs).strftime("%Y-%m-%d")] += t.pnl
    if not by_day: return
    print("  DAILY SESSION RESULTS")
    print(f"  {'DATE':<12} {'PNL':>10}  RESULT")
    print(f"  {'-'*38}")
    streak = 0
    for day in sorted(by_day):
        p = by_day[day]
        if p <= 0: streak += 1
        else:      streak = 0
        sw = f"  ⚠ {streak} consec loss" if streak >= 2 else ""
        col = GR if p > 0 else RD
        print(f"  {day:<12} {col}{p:>10.2f}{RS}  {'WIN' if p>0 else 'LOSS'}{sw}")
    print()


def _velocity_audit(trades):
    if not trades: return
    st = sorted(trades, key=lambda t: t.entryTs)
    W  = 1800
    events = []
    for t in st:
        close = t.entryTs + int(t.hold_sec)
        wpnl  = sum(x.pnl for x in st
                    if x.entryTs + int(x.hold_sec) <= close
                    and x.entryTs + int(x.hold_sec) >= close - W)
        if wpnl < -50:
            events.append((close, wpnl))
    if not events:
        print("  DRAWDOWN VELOCITY AUDIT  — no 30-min windows with >$50 loss\n")
        return
    seen = set(); flagged = []
    for ts, p in events:
        d = datetime.datetime.utcfromtimestamp(ts).strftime("%Y-%m-%d")
        if d not in seen:
            seen.add(d)
            flagged.append((datetime.datetime.utcfromtimestamp(ts).strftime("%Y-%m-%d %H:%M"), p))
    print(f"  DRAWDOWN VELOCITY AUDIT  — {len(flagged)} session(s) with rapid loss")
    print(f"  {'DATETIME (UTC)':<20} {'30MIN_LOSS':>12}  NOTE")
    print(f"  {'-'*56}")
    for day, p in flagged[:15]:
        note = "would trigger 15-min halt" if p < -100 else "approaching threshold"
        print(f"  {day:<20} {RD}{p:>12.2f}{RS}  {note}")
    print()


def walk_forward_analysis(trades, n_splits=5):
    if len(trades) < 40:
        print("  WALK-FORWARD OOS  — need >= 40 trades\n"); return
    st = sorted(trades, key=lambda t: t.entryTs)
    n  = len(st)
    fs = n // n_splits
    print(f"  {BD}WALK-FORWARD OOS  ({n_splits}-fold, {n} trades){RS}")
    print(f"  Two Sigma rule: OOS Sharpe >= 0.8  AND  OOS/IS >= 0.40")
    print(f"  {'FOLD':<6} {'IS_N':>6} {'IS_SHP':>8} {'OOS_N':>6} {'OOS_SHP':>9} {'RATIO':>7}  STATUS")
    print(f"  {'-'*65}")
    all_pass = True; oos_shs = []; ratios = []
    for fold in range(n_splits):
        os_ = fold * fs
        oe  = os_ + fs if fold < n_splits - 1 else n
        oos = st[os_:oe]; isd = st[:os_] + st[oe:]
        if len(isd) < 10 or len(oos) < 5: continue
        iss = stats(isd)['sharpe']; ooss = stats(oos)['sharpe']
        ratio = (ooss / iss) if abs(iss) > 0.1 else 0.0
        oos_shs.append(ooss); ratios.append(ratio)
        ok = ooss >= 0.8 and ratio >= 0.40
        if not ok: all_pass = False
        col = GR if ok else RD
        print(f"  {fold+1:<6} {len(isd):>6} {iss:>8.2f} {len(oos):>6} {ooss:>9.2f} {ratio:>7.2f}  {col}{'PASS' if ok else 'FAIL'}{RS}")
    if oos_shs:
        avg_o = sum(oos_shs)/len(oos_shs); avg_r = sum(ratios)/len(ratios)
        col   = GR if all_pass else RD
        verd  = "DEPLOY READY" if all_pass else "DO NOT SCALE — edge not OOS validated"
        print(f"  {'-'*65}")
        print(f"  Avg OOS Sharpe: {avg_o:.2f}   Avg OOS/IS: {avg_r:.2f}")
        print(f"  Verdict: {col}{BD}{verd}{RS}")
    print()


# ── CSV export ────────────────────────────────────────────────────────────────

def write_csv_report(trades, path, min_trades):
    rows = []
    def add(gt, gn, ts):
        if len(ts) < min_trades: return
        s = stats(ts)
        rows.append({
            "group_type": gt, "group_name": gn,
            "n": s["n"], "win_rate": round(s["wr"],4),
            "expectancy": round(s["expectancy"],2),
            "sharpe": round(s["sharpe"],3),
            "sortino": round(s["sortino"],3),
            "calmar": round(s["calmar"],3),
            "profit_factor": round(s["profit_factor"],2) if s["profit_factor"] != float('inf') else 9999,
            "total_pnl": round(s["total_pnl"],2),
            "max_dd": round(s["max_dd"],2),
            "autocorr": round(s["autocorr"],3),
            "p_value": round(s["p_value"],4),
            "significant": int(s["significant"]),
            "avg_hold": s["avg_hold"],
        })
    add("ALL","ALL",trades)
    by_eng = defaultdict(list)
    for t in trades: by_eng[t.engine].append(t)
    for k,v in by_eng.items(): add("ENGINE",k,v)
    by_sym = defaultdict(list)
    for t in trades: by_sym[t.symbol].append(t)
    for k,v in by_sym.items(): add("SYMBOL",k,v)
    by_es = defaultdict(list)
    for t in trades: by_es[(t.engine,t.symbol)].append(t)
    for (e,s),v in sorted(by_es.items()): add("ENGINE_SYMBOL",f"{e}:{s}",v)
    if not rows: print("  No rows to write."); return
    with open(path,"w",newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)
    print(f"  CSV report written to: {path}")


# ── Equity plot ───────────────────────────────────────────────────────────────

def plot_equity(trades):
    try:
        import matplotlib.pyplot as plt
        import matplotlib.dates as mdates
    except ImportError:
        print("  matplotlib not available"); return
    trades = sorted(trades, key=lambda t: t.entryTs)
    xs  = [datetime.datetime.utcfromtimestamp(t.entryTs) for t in trades]
    cum = []; c = 0.0
    for t in trades: c += t.pnl; cum.append(c)
    engs = sorted(set(t.engine for t in trades))
    ep   = defaultdict(list); ec = defaultdict(float)
    for t in trades:
        ec[t.engine] += t.pnl
        ep[t.engine].append((datetime.datetime.utcfromtimestamp(t.entryTs), ec[t.engine]))
    fig, axes = plt.subplots(2,1,figsize=(14,8))
    fig.suptitle("Omega Shadow — Equity Analysis", fontsize=13, fontweight='bold')
    ax0 = axes[0]
    ax0.plot(xs, cum, color='#00d4aa', linewidth=1.5)
    ax0.axhline(0, color='grey', linewidth=0.5, linestyle='--')
    ax0.fill_between(xs, cum, 0, where=[v>=0 for v in cum], alpha=0.15, color='#00d4aa')
    ax0.fill_between(xs, cum, 0, where=[v<0  for v in cum], alpha=0.15, color='#ff4444')
    ax0.set_title("Combined Equity"); ax0.set_ylabel("USD PnL")
    ax0.xaxis.set_major_formatter(mdates.DateFormatter('%m/%d')); ax0.grid(alpha=0.15)
    ax1 = axes[1]
    cmap = plt.cm.get_cmap('tab10', len(engs))
    for i,e in enumerate(engs):
        pts = ep[e]
        if len(pts) >= 2:
            xs2,ys = zip(*pts); ax1.plot(xs2,ys,linewidth=1.2,label=e,color=cmap(i))
    ax1.axhline(0, color='grey', linewidth=0.5, linestyle='--')
    ax1.set_title("Per-Engine Equity"); ax1.set_ylabel("USD PnL")
    ax1.xaxis.set_major_formatter(mdates.DateFormatter('%m/%d'))
    ax1.grid(alpha=0.15); ax1.legend(fontsize=7, ncol=3)
    plt.tight_layout(); plt.show()


# ── CSV loader ────────────────────────────────────────────────────────────────

def load_csv(path, days_filter):
    trades = []; cutoff = 0
    if days_filter:
        cutoff = int((datetime.datetime.utcnow()-datetime.timedelta(days=days_filter)).timestamp())
    with open(path, newline="", encoding="utf-8") as f:
        sample = f.read(512); f.seek(0)
        has_hdr = sample.startswith("entryTs") or sample.startswith('"entryTs')
        reader  = csv.DictReader(f) if has_hdr else csv.DictReader(f, fieldnames=COLUMNS)
        for r in reader:
            try:
                t = Trade(r)
                if t.symbol == "UNKNOWN" or t.entryTs == 0: continue
                if cutoff and t.entryTs < cutoff: continue
                trades.append(t)
            except (ValueError, KeyError): continue
    return trades

def find_default_csv():
    for p in ["logs/shadow/omega_shadow.csv",
              "C:/Omega/logs/shadow/omega_shadow.csv",
              os.path.join(os.path.dirname(__file__),"..","logs","shadow","omega_shadow.csv")]:
        if os.path.isfile(p): return os.path.abspath(p)
    return None


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Omega shadow mode performance analyser")
    ap.add_argument("csv_path", nargs="?", default=None)
    ap.add_argument("--days",       type=int,  default=None)
    ap.add_argument("--min-trades", type=int,  default=5)
    ap.add_argument("--csv",        action="store_true")
    ap.add_argument("--plot",       action="store_true")
    ap.add_argument("--wfo",        action="store_true")
    ap.add_argument("--wfo-splits", type=int, default=5)
    args = ap.parse_args()

    path = args.csv_path or find_default_csv()
    if not path or not os.path.isfile(path):
        print("ERROR: shadow CSV not found."); sys.exit(1)

    print(f"\n  Loading: {path}")
    trades = load_csv(path, args.days)
    if not trades:
        print("  No trades found."); sys.exit(0)
    if args.days:
        print(f"  Filter: last {args.days} days  ({len(trades)} trades)")

    print_report(trades, args.min_trades)

    if args.wfo:
        walk_forward_analysis(trades, n_splits=args.wfo_splits)
    if args.csv:
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shadow_report.csv")
        write_csv_report(trades, out, args.min_trades)
    if args.plot:
        plot_equity(trades)

if __name__ == "__main__":
    main()
