#!/usr/bin/env python3
"""engine_exceptional_watch.py -- ML-side live-trading monitor (S-2026-07-23a).

Operator order: "monitor our trading and if it sees an engine that is making
exceptional trades we need to empower it to allow that engine to open mimic
trades x2".

WHAT IT DOES (honest scope):
  1. Reads the LIVE ledger (omega_trade_closes.csv -- the primary record of
     engine performance per OMEGA.md) and computes per-engine rolling stats
     over the last WINDOW_D days: n, net, PF, win%, and net split across the
     two window halves.
  2. Flags EXCEPTIONAL engines: n >= MIN_N, PF >= MIN_PF, net > 0 in BOTH
     halves (kills one-lucky-trade flags).
  3. EMPOWERMENT CONTRACT: writes data/exceptional_engines.json with, per
     flagged engine, its mimic status:
       - "mimic_live"      : a certified x2 companion already runs (nothing to arm)
       - "MIMIC-X2-OWED"   : NO certified mimic exists -> this engine is a
                             build+certify candidate. THE FLAG IS THE ORDER for
                             the next AI session: build its x2 BE-floored mimic
                             (BeFloorOnOpenFoundation, confirm>=2xRT, anchored),
                             certify standalone, then wire live.
  HARD-RULE GUARD (why this does NOT auto-open mimic trades): every new
  companion MUST be BE-floor-on-open FOUNDATION + certified standalone before
  live (operator mandates 2026-07-17c + live-only-cull). Auto-opening an
  uncertified mimic book from a monitor would violate both. The monitor
  EMPOWERS by flagging + queueing the cert; arming stays a certified-code path.

Run cadence: daily post-close (cron/task). Fail-soft.
"""
import csv, json, os, sys, datetime as dt
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGERS = [
    os.path.expanduser("~/Omega-vps-mirror/logs/trades/omega_trade_closes.csv"),  # Mac mirror
    r"C:\Omega\logs\trades\omega_trade_closes.csv",                               # VPS-local
]
OUT = os.path.join(ROOT, "data", "exceptional_engines.json")

WINDOW_D = 30
MIN_N    = 8
MIN_PF   = 2.0

# engines that ALREADY have a certified live mimic/companion book (x2 pattern
# where noted). Keep in sync with engine_init wiring.
MIMIC_LIVE = {
    "GoldDailyCbe":  "GoldDailyCbeMimic x2 (T+W) LIVE S-22i",
    "GoldTrend":     "GoldTrendMimicLadder LIVE",
    "StockDip":      "StockDipTrendMimic LIVE",
    "StockDipTurtle":"StockDipTrendMimic LIVE",
}

def main():
    src = next((p for p in LEDGERS if os.path.exists(p)), None)
    if not src:
        print("[exwatch] no ledger found"); return 1
    cut = (dt.date.today() - dt.timedelta(days=WINDOW_D)).isoformat()
    mid = (dt.date.today() - dt.timedelta(days=WINDOW_D // 2)).isoformat()
    st = defaultdict(lambda: dict(n=0, net=0.0, gw=0.0, gl=0.0, wins=0, h1=0.0, h2=0.0))
    with open(src, newline="") as f:
        for r in csv.DictReader(f):
            eng = (r.get("engine") or r.get("Engine") or "").strip()
            if not eng: continue
            ts = (r.get("exit_ts_utc") or r.get("close_time") or r.get("exitTs") or "")[:10]
            if not ts or ts < cut: continue
            try: pnl = float(r.get("net_pnl") or r.get("pnl") or r.get("PnL") or 0)
            except ValueError: continue
            s = st[eng]
            s["n"] += 1; s["net"] += pnl
            if pnl >= 0: s["gw"] += pnl; s["wins"] += 1
            else: s["gl"] += -pnl
            if ts < mid: s["h1"] += pnl
            else: s["h2"] += pnl
    flagged, watch = [], []
    for eng, s in sorted(st.items(), key=lambda kv: -kv[1]["net"]):
        pf = (s["gw"] / s["gl"]) if s["gl"] > 0 else (999.0 if s["gw"] > 0 else 0.0)
        row = dict(engine=eng, n=s["n"], net=round(s["net"], 2), pf=round(pf, 2),
                   win_pct=round(100.0 * s["wins"] / s["n"], 1) if s["n"] else 0,
                   h1=round(s["h1"], 2), h2=round(s["h2"], 2))
        if s["n"] >= MIN_N and pf >= MIN_PF and s["h1"] > 0 and s["h2"] > 0:
            row["mimic"] = MIMIC_LIVE.get(eng, "MIMIC-X2-OWED")
            flagged.append(row)
        else:
            watch.append(row)
    out = dict(asof=str(dt.date.today()), ledger=src, window_days=WINDOW_D,
               criteria=f"n>={MIN_N} & PF>={MIN_PF} & both-half net>0",
               exceptional=flagged, all_engines=watch[:40],
               note=("MIMIC-X2-OWED = empowerment queue: build + certify a BE-floored "
                     "x2 mimic for this engine (BeFloorOnOpenFoundation), then wire live. "
                     "Auto-arming uncertified mimics is forbidden by operator hard rule."))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f: json.dump(out, f, indent=1)
    print(f"[exwatch] {len(st)} engines in {WINDOW_D}d window; exceptional={len(flagged)}: "
          + (", ".join(f"{r['engine']}(PF{r['pf']},n{r['n']},{r['mimic']})" for r in flagged) or "none"))
    return 0

if __name__ == "__main__":
    sys.exit(main())
