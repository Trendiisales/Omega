#!/usr/bin/env python3
"""
live_truth_guard.py  (S-2026-07-16) — kill the recurring "stale Mac-local data poisoned a live
reconstruction" class. Operator rule: force every session to the VPS live file / ledger as the
SINGLE SOURCE OF TRUTH for live equity state (fills, marks, "yesterday's close").

THE CLASS (feedback-silent-fallback-stale-feed-class + project-rdagent-basket-stale-mark):
a session reaches for a Mac-local file to reconstruct LIVE state; the Mac copy is a RESEARCH
artifact that can lag (bulk yfinance chokes, no watcher) -> wrong "live" numbers. The live feed
was fine the whole time.

AUTHORITATIVE LIVE TRUTH (use THESE, never a Mac-local copy):
  - Stockmover / daily equity closes : VPS  C:\\Omega\\data\\rdagent\\sp500_long_close.csv
                                       (OmegaStockMoverFeed task, in-binary daily-close writer)
  - Realized fills / closed trades   : ledger  ~/Omega-vps-mirror/logs/trades/omega_trade_closes.csv
                                       (+ daily CSV; VPS C:\\Omega\\logs\\trades\\)

RESEARCH-ONLY Mac-local copies (NEVER a live-truth source):
  - data/rdagent/*                        (Mac rdagent research + paper basket book — SEPARATE book;
                                           it is the SOURCE of the desk STOCK BASKET panel, not live fills)
  - backtest/data/bigcap_daily_ohlc/*     (Yahoo historical OHLC for backtests — deleted S-2026-07-16;
                                           re-fetch with backtest/fetch_bigcap_daily_ohlc.py when a BT needs it)

Run this before any live-state reconstruction. Exit 0 = safe; exit 3 = a stale Mac-local artifact
exists that a session could mistake for live truth -> STOP, use the VPS sources above.
"""
import os, sys, time, datetime as dt

HOME = os.path.expanduser("~")
RDA_LOCAL   = os.path.join(HOME, "Omega", "data", "rdagent", "sp500_long_close.csv")
OHLC_LOCAL  = os.path.join(HOME, "Omega", "backtest", "data", "bigcap_daily_ohlc")
LEDGER      = os.path.join(HOME, "Omega-vps-mirror", "logs", "trades", "omega_trade_closes.csv")
STALE_DAYS  = 2   # a Mac research close copy older than this must NOT be trusted for live

def _last_row_date(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2); size = f.tell(); back = min(size, 4096); f.seek(size - back)
            tail = f.read().decode("utf-8", "replace").strip().splitlines()
        for line in reversed(tail):
            tok = line.split(",")[0].strip()
            for fmt in ("%Y-%m-%d", "%Y%m%d"):
                try: return dt.datetime.strptime(tok[:10] if fmt == "%Y-%m-%d" else tok[:8], fmt).date()
                except ValueError: continue
    except Exception:
        return None
    return None

def main():
    print("=== LIVE-TRUTH GUARD (single source of truth = VPS live file / ledger) ===")
    print("  LIVE closes : VPS C:\\Omega\\data\\rdagent\\sp500_long_close.csv (OmegaStockMoverFeed)")
    print(f"  LIVE fills  : {LEDGER}")
    print("  RESEARCH    : data/rdagent/* + backtest/data/bigcap_daily_ohlc/* -> NEVER for live state")
    rc = 0
    today = dt.date.today()

    # 1. backtest OHLC footgun must be gone (research data mistaken for live reconstruction source)
    if os.path.isdir(OHLC_LOCAL) and any(n.endswith(".csv") for n in os.listdir(OHLC_LOCAL)):
        print(f"  [WARN] {OHLC_LOCAL} present — research OHLC. NEVER reconstruct live fills from it.")
    else:
        print("  [OK]   backtest/data/bigcap_daily_ohlc absent (footgun removed; re-fetch on demand).")

    # 2. Mac-local rdagent close copy: allowed to exist (Mac basket needs it) but must NOT be
    #    trusted as live truth when stale.
    if os.path.exists(RDA_LOCAL):
        d = _last_row_date(RDA_LOCAL)
        age = (today - d).days if d else None
        if d is None:
            print(f"  [WARN] {RDA_LOCAL} unreadable last-row date — do NOT use for live; use VPS.")
        elif age is not None and age > STALE_DAYS:
            print(f"  [STALE] Mac-local sp500_long_close last row {d} (age {age}d > {STALE_DAYS}d).")
            print("          DO NOT use this for live state. Live truth = VPS file / ledger above.")
            rc = 3
        else:
            print(f"  [OK]   Mac-local sp500_long_close last row {d} (research copy; live truth still = VPS).")

    # 3. ledger reachable (the real live-fill record)
    if os.path.exists(LEDGER):
        age_min = (time.time() - os.path.getmtime(LEDGER)) / 60.0
        print(f"  [OK]   live-fill ledger present, mtime {age_min:.0f}min ago.")
    else:
        print(f"  [NOTE] {LEDGER} not mounted this box — pull from VPS C:\\Omega\\logs\\trades\\ for live fills.")

    print(f"=== guard {'CLEAN' if rc == 0 else 'STALE — use VPS sources, not Mac-local'} (rc={rc}) ===")
    return rc

if __name__ == "__main__":
    sys.exit(main())
