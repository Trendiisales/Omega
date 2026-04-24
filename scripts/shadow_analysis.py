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
  python shadow_analysis.py --no-ghost-filter   # disable S17 ghost guards (audit only)

S17 GHOST-RECORD GUARDS (2026-04-24)
------------------------------------
Defensive filters applied at CSV load time to reject corrupted trade records
caused by pre-mutex-fix stdout interleaving on 2026-04-14/16 (see
trade_lifecycle.hpp line 167 tombstone). The ghost pattern:

  - pnl_raw ~= -entryPrice * 0.01  (so USD pnl equals -entryPrice)
  - hold_sec in the billions (~56 years, Unix epoch arithmetic bug)
  - mfe == 0.000 always
  - side sometimes flipped from the real concurrent trade

Without these guards a single ghost can claim a fake -$4,767 or -$4,810 loss
and pollute every downstream statistic. Guards:

  G1 HOLD_SEC_MAX    : reject hold_sec > 604800 (7 days)
  G2 PNL_VS_PRICE    : reject abs(pnl) > entryPrice * PNL_PRICE_MAX_RATIO
  G3 DEDUP           : collapse (entryTs, symbol, side, exitReason) duplicates,
                       keep the record with the smallest |pnl| (the real one;
                       ghost amplifies pnl by 100-1000x, which is the bug's
                       fingerprint).
  G4 MFE_ZERO_WARN   : flag (do not reject) mfe == 0.0 with abs(pnl) > 10.0

A summary of rejected and flagged rows is printed before the report so no
ghost is ever silently discarded.
"""

import sys
import os
import csv
import math
import argparse
import datetime
import warnings
from collections import defaultdict
from typing import List, Dict, Tuple, Optional

# S17: suppress the Python 3.12+ DeprecationWarnings from datetime.utcnow()
# and datetime.utcfromtimestamp().  Migrating every call site to timezone-aware
# objects changes printed strings ("+00:00" suffix) and has subtle behaviour
# differences; that's a separate refactor.  The functions still work correctly
# on all currently-supported Python versions.
warnings.filterwarnings(
    "ignore",
    message="datetime.datetime.utc.*",
    category=DeprecationWarning,
)


COLUMNS = [
    "entryTs", "symbol", "side", "engine", "entryPrice", "exitPrice",
    "pnl", "mfe", "mae", "hold_sec", "exitReason", "spreadAtEntry",
    "latencyMs", "regime"
]


# ── S17 ghost-record guard thresholds ─────────────────────────────────────────
#
# These constants are the only tunable surface for the ghost filter. If the
# legitimate engine universe ever produces a hold_sec > 7 days or a pnl
# magnitude near the entry price, revisit here BEFORE widening the guard --
# loosening these filters silently re-exposes the $4,767 ghost-loss bug.
GHOST_HOLD_SEC_MAX       = 604800      # 7 days. Live XAUUSD/index trades never.
GHOST_PNL_PRICE_RATIO    = 0.99        # abs(pnl) > entryPrice * 0.99 == ghost.
GHOST_MFE_ZERO_PNL_WARN  = 10.0        # mfe==0 with |pnl|>10 is suspicious.


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


# ── S17 Ghost-record guards ───────────────────────────────────────────────────

class GhostStats:
    """Running tally of guard trips for the load-time report."""
    __slots__ = ("raw_rows", "kept", "g1_hold", "g2_pnl_price",
                 "g3_dedup", "g4_mfe_warn", "samples")
    def __init__(self):
        self.raw_rows     = 0
        self.kept         = 0
        self.g1_hold      = []   # list of (entryTs, symbol, hold_sec, pnl)
        self.g2_pnl_price = []   # list of (entryTs, symbol, entryPrice, pnl)
        self.g3_dedup     = []   # list of (entryTs, symbol, side, exitReason, pnl_kept, pnl_rejected)
        self.g4_mfe_warn  = []   # list of (entryTs, symbol, engine, pnl)

    @property
    def rejected(self):
        return len(self.g1_hold) + len(self.g2_pnl_price) + len(self.g3_dedup)


def _ghost_dedup_key(t: Trade) -> Tuple[int, str, str, str]:
    """Same-second duplicate close on same symbol+side+reason is always
    corruption. Uses floor-to-second because the pre-mutex-fix bug produced
    two EXIT lines in the same log millisecond."""
    return (int(t.entryTs), t.symbol, t.side, t.exitReason)


def apply_ghost_filters(raw: List[Trade], gs: GhostStats,
                        enable: bool = True) -> List[Trade]:
    """Apply S17 ghost-record guards. See module docstring for full rationale.

    Returns the filtered trade list. Populates `gs` so the caller can print
    a summary of what was rejected.
    """
    gs.raw_rows = len(raw)

    if not enable:
        # Audit path: keep every row but still run the MFE warning pass so the
        # user can see what WOULD have been flagged.
        for t in raw:
            if t.mfe == 0.0 and abs(t.pnl) > GHOST_MFE_ZERO_PNL_WARN:
                gs.g4_mfe_warn.append((t.entryTs, t.symbol, t.engine, t.pnl))
        gs.kept = len(raw)
        return list(raw)

    # G1 + G2 — per-row rejection pass.
    after_g1g2: List[Trade] = []
    for t in raw:
        # G1 HOLD_SEC_MAX
        if t.hold_sec > GHOST_HOLD_SEC_MAX:
            gs.g1_hold.append((t.entryTs, t.symbol, t.hold_sec, t.pnl))
            continue
        # G2 PNL_VS_PRICE — the smoking gun: pnl magnitude within 1% of the
        # entry price means the CSV has captured the price as pnl (the
        # streamed-cout interleaving bug).  Only applies when entryPrice > 0
        # to avoid false positives on rows missing the field.
        if t.entryPrice > 0.0 and abs(t.pnl) > t.entryPrice * GHOST_PNL_PRICE_RATIO:
            gs.g2_pnl_price.append((t.entryTs, t.symbol, t.entryPrice, t.pnl))
            continue
        after_g1g2.append(t)

    # G3 — dedup same-second (symbol, side, exitReason) collisions.  Keep the
    # record with the smallest |pnl|; the ghost inflates pnl by 100-1000x so
    # the real trade always has the lower magnitude.
    groups: Dict[Tuple[int, str, str, str], List[Trade]] = defaultdict(list)
    for t in after_g1g2:
        groups[_ghost_dedup_key(t)].append(t)

    kept: List[Trade] = []
    for key, bucket in groups.items():
        if len(bucket) == 1:
            kept.append(bucket[0])
            continue
        # Sort ascending by |pnl|; first element is the real trade.
        bucket_sorted = sorted(bucket, key=lambda x: abs(x.pnl))
        real = bucket_sorted[0]
        kept.append(real)
        for ghost in bucket_sorted[1:]:
            gs.g3_dedup.append(
                (key[0], key[1], key[2], key[3], real.pnl, ghost.pnl)
            )

    # G4 — non-rejecting warn pass for MFE=0 with material loss.
    for t in kept:
        if t.mfe == 0.0 and abs(t.pnl) > GHOST_MFE_ZERO_PNL_WARN:
            gs.g4_mfe_warn.append((t.entryTs, t.symbol, t.engine, t.pnl))

    gs.kept = len(kept)
    return kept


def _fmt_ts(ts: int) -> str:
    try:
        return datetime.datetime.utcfromtimestamp(int(ts)).strftime("%Y-%m-%d %H:%M:%S")
    except (ValueError, OSError, OverflowError):
        return f"ts={ts}"


def print_ghost_report(gs: GhostStats, enable: bool) -> None:
    """Always print a summary. Silence is dangerous — a user needs to see
    when the guards tripped so they can cross-check against live logs."""
    print()
    hdr = f"  {BD}S17 GHOST-RECORD GUARDS{RS}"
    if not enable:
        hdr += f"  {AM}(DISABLED via --no-ghost-filter){RS}"
    print(hdr)
    print(f"  {'-'*80}")
    print(f"  Rows read       : {gs.raw_rows}")
    print(f"  Rows kept       : {gs.kept}")
    if enable:
        col = GR if gs.rejected == 0 else RD
        print(f"  Rows rejected   : {col}{gs.rejected}{RS}"
              f"   (G1_hold={len(gs.g1_hold)} "
              f"G2_pnl_price={len(gs.g2_pnl_price)} "
              f"G3_dedup={len(gs.g3_dedup)})")
    print(f"  MFE=0 warnings  : {len(gs.g4_mfe_warn)}")

    # Detail blocks -- only print if there were hits, cap at 10 examples each.
    if gs.g1_hold:
        print()
        print(f"  {RD}G1  hold_sec > {GHOST_HOLD_SEC_MAX}s ({GHOST_HOLD_SEC_MAX//86400} days){RS}")
        print(f"  {'TIMESTAMP (UTC)':<22} {'SYMBOL':<10} {'HOLD_SEC':>16} {'PNL':>10}")
        for ts, sym, hold, pnl in gs.g1_hold[:10]:
            print(f"  {_fmt_ts(ts):<22} {sym:<10} {hold:>16.0f} {pnl:>10.2f}")
        if len(gs.g1_hold) > 10:
            print(f"  ... and {len(gs.g1_hold)-10} more")

    if gs.g2_pnl_price:
        print()
        print(f"  {RD}G2  |pnl| > entryPrice * {GHOST_PNL_PRICE_RATIO:.2f}  "
              f"(ghost signature: pnl==-entryPrice){RS}")
        print(f"  {'TIMESTAMP (UTC)':<22} {'SYMBOL':<10} {'ENTRY_PX':>10} {'PNL':>12}")
        for ts, sym, px, pnl in gs.g2_pnl_price[:10]:
            print(f"  {_fmt_ts(ts):<22} {sym:<10} {px:>10.2f} {pnl:>12.2f}")
        if len(gs.g2_pnl_price) > 10:
            print(f"  ... and {len(gs.g2_pnl_price)-10} more")

    if gs.g3_dedup:
        print()
        print(f"  {RD}G3  same-second duplicate (entryTs, symbol, side, exitReason){RS}")
        print(f"  {'TIMESTAMP (UTC)':<22} {'SYMBOL':<8} {'SIDE':<6} {'EXIT':<16} "
              f"{'KEPT_PNL':>10} {'DROPPED_PNL':>13}")
        for ts, sym, side, reason, kept_pnl, drop_pnl in gs.g3_dedup[:10]:
            print(f"  {_fmt_ts(ts):<22} {sym:<8} {side:<6} {reason:<16} "
                  f"{kept_pnl:>10.2f} {drop_pnl:>13.2f}")
        if len(gs.g3_dedup) > 10:
            print(f"  ... and {len(gs.g3_dedup)-10} more")

    if gs.g4_mfe_warn:
        print()
        print(f"  {AM}G4  mfe==0 with |pnl| > ${GHOST_MFE_ZERO_PNL_WARN:.0f}  "
              f"(suspicious — not rejected){RS}")
        print(f"  {'TIMESTAMP (UTC)':<22} {'SYMBOL':<10} {'ENGINE':<20} {'PNL':>10}")
        for ts, sym, eng, pnl in gs.g4_mfe_warn[:10]:
            print(f"  {_fmt_ts(ts):<22} {sym:<10} {eng:<20} {pnl:>10.2f}")
        if len(gs.g4_mfe_warn) > 10:
            print(f"  ... and {len(gs.g4_mfe_warn)-10} more")

    if gs.rejected == 0 and not gs.g4_mfe_warn:
        print(f"  {GR}No ghost-record symptoms detected.{RS}")
    print()


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
    """Load raw CSV rows into Trade objects. Ghost filtering is applied
    separately by apply_ghost_filters() so the pre-filter row count is
    available for the load-time report."""
    trades: List[Trade] = []
    cutoff = 0
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
    ap.add_argument("--no-ghost-filter", action="store_true",
                    help="Disable S17 ghost-record guards (audit mode; "
                         "rows still scanned but not rejected).")
    args = ap.parse_args()

    path = args.csv_path or find_default_csv()
    if not path or not os.path.isfile(path):
        print("ERROR: shadow CSV not found."); sys.exit(1)

    print(f"\n  Loading: {path}")
    raw_trades = load_csv(path, args.days)
    if not raw_trades:
        print("  No trades found."); sys.exit(0)

    gs = GhostStats()
    trades = apply_ghost_filters(raw_trades, gs,
                                 enable=not args.no_ghost_filter)
    print_ghost_report(gs, enable=not args.no_ghost_filter)

    if not trades:
        print("  No trades remaining after ghost filter."); sys.exit(0)

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
