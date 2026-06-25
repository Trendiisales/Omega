#!/usr/bin/env python3
"""data_health_monitor.py — REGISTRY-DRIVEN cross-system stale-feed guard (Mac).

The "never again" guard. Every feed that drives a LIVE decision is enrolled in FEEDS below.
Each is checked for staleness — FILE-level AND, for wide CSVs, PER-COLUMN (the gap that let
230 of 536 sp500_long_close columns rot to 2yr-stale while the file's last row looked fresh).
On any stale feed: optional AUTO-REFRESH (--fix), always a loud table + DATA_HEALTH.json +
DATA_HEALTH.flag + beep + exit 1. Wire to launchd (every 10-15 min) so drift cannot hide.

  python3 data_health_monitor.py            # human table + exit code
  python3 data_health_monitor.py --quiet    # only print on FAIL (cron)
  python3 data_health_monitor.py --fix      # attempt registered auto-refresh on stale feeds, then re-check

ENROLLING A NEW FEED IS MANDATORY: any new CSV/series that feeds a live engine/gate/GUI/model
MUST be added to FEEDS here in the same change. That is the systemic guarantee.
"""
import os, sys, json, time, csv, subprocess, datetime as dt

HOME = os.path.expanduser("~")
NOW = time.time()
TODAY = dt.date.today()
QUIET = "--quiet" in sys.argv
FIX = "--fix" in sys.argv

# ── helpers ───────────────────────────────────────────────────────────────────
def _file_newest_date_age(path, ts_col0=False):
    """Age (days) of the newest DATE found scanning a CSV's first column."""
    try:
        newest = None
        with open(path, newline="") as fh:
            r = csv.reader(fh); next(r, None)
            for row in r:
                if not row: continue
                c0 = row[0].strip()
                d = None
                if ts_col0 or (c0.isdigit() and len(c0) >= 9):
                    try:
                        v = float(c0);  v = v/1000.0 if v > 1e12 else v
                        d = dt.datetime.utcfromtimestamp(v).date()
                    except Exception: d = None
                else:
                    try: d = dt.date.fromisoformat(c0[:10])
                    except Exception: d = None
                if d and (newest is None or d > newest): newest = d
        return None if newest is None else (TODAY - newest).days
    except Exception:
        return None

def _percolumn_stale(path, max_age_d):
    """For a wide Date-indexed CSV: (n_total, n_stale, frac_stale, examples). A column is stale
    if its last non-empty cell's date is > max_age_d behind the file's newest date."""
    try:
        with open(path, newline="") as fh:
            r = csv.reader(fh); syms = next(r)[1:]
            last_date = {}; newest = None
            for row in r:
                if not row: continue
                try: d = dt.date.fromisoformat(row[0][:10])
                except Exception: continue
                if newest is None or d > newest: newest = d
                for i, s in enumerate(syms, 1):
                    if i < len(row) and row[i] not in ("", "nan", "NaN"):
                        last_date[s] = d
        if newest is None: return (len(syms), len(syms), 1.0, ["(no dates)"])
        stale = [s for s in syms if s not in last_date or (newest - last_date[s]).days > max_age_d]
        return (len(syms), len(stale), (len(stale)/len(syms) if syms else 1.0), stale[:8])
    except Exception as e:
        return (0, 0, 1.0, [f"err:{e}"])

# ── THE REGISTRY: every live-decision feed. kind: file | percol | json_book ────
FEEDS = [
    # name, path, max_age_d, kind, severity, consumer, refresh_cmd (or None), extra
    ("rdagent.sp500_long_close", f"{HOME}/Omega/data/rdagent/sp500_long_close.csv", 5, "percol", "HIGH",
     "rdagent GUI + paper basket fills",
     ["python3", f"{HOME}/Omega/tools/rdagent/refresh_close_ibkr.py", "--tickers", "bigcap"], {"max_stale_frac": 0.15}),
    ("rdagent.qlib_calendar", f"{HOME}/.qlib/qlib_data/omega_data/calendars/day.txt", 4, "file", "HIGH",
     "rdagent model rankings", None, {}),
    ("tick.NDX_daily", f"{HOME}/Tick/NDX_daily_2016_2026.csv", 4, "file", "MED",
     "index research", None, {}),
    # the LIVE C++ MacroGoldGate reads logs/macro/macro_gold_gate.tsv (last field = stamp_ms);
    # macro_gold_gate.py is the producer (fetches real-yield + dollar from source). Monitor the
    # ACTUAL gate input, auto-refresh by re-running the producer.
    ("macrogoldgate.tsv", f"{HOME}/Omega/logs/macro/macro_gold_gate.tsv", 2, "stamp_ms", "HIGH",
     "MacroGoldGate (LIVE gold gate input)",
     ["python3", f"{HOME}/Omega/tools/macro_gold_gate.py"], {}),
    ("gold.mgc_h1_seed", f"{HOME}/Omega/data/mgc_h1_hist.csv", 7, "file", "MED",
     "gold engine warm-seed",
     ["python3", f"{HOME}/Omega/tools/mgc_pull_history.py"], {}),
    ("gold.mgc_30m_seed", f"{HOME}/Omega/data/mgc_30m_hist.csv", 7, "file", "MED",
     "gold engine warm-seed",
     ["python3", f"{HOME}/Omega/tools/mgc_pull_history.py"], {}),
]

def check_feed(f):
    name, path, max_age, kind, sev, consumer, refresh, extra = f
    if not os.path.exists(path):
        return dict(name=name, ok=False, age=None, limit=max_age, sev=sev, detail=f"MISSING ({consumer})")
    if kind == "percol":
        ntot, nstale, frac, ex = _percolumn_stale(path, max_age)
        ok = frac <= extra.get("max_stale_frac", 0.10)
        return dict(name=name, ok=ok, age=None, limit=max_age, sev=sev,
                    detail=f"{nstale}/{ntot} cols stale ({frac*100:.0f}%) e.g. {ex[:4]} [{consumer}]")
    elif kind == "stamp_ms":
        try:
            last = [l for l in open(path).read().strip().splitlines() if l and not l.startswith("#")][-1]
            stamp = float(last.split("\t")[-1].split(",")[-1])
            age = (NOW - (stamp/1000.0)) / 86400.0
            ok = age <= max_age
            return dict(name=name, ok=ok, age=round(age, 1), limit=max_age, sev=sev,
                        detail=f"stamp {age:.1f}d old [{consumer}]")
        except Exception as e:
            return dict(name=name, ok=False, age=None, limit=max_age, sev=sev, detail=f"unparseable: {e} [{consumer}]")
    else:
        age = _file_newest_date_age(path, ts_col0=(kind == "file" and path.endswith(("NDX_daily_2016_2026.csv",))))
        ok = age is not None and age <= max_age
        return dict(name=name, ok=ok, age=age, limit=max_age, sev=sev,
                    detail=(f"newest {age}d old [{consumer}]" if age is not None else f"unreadable [{consumer}]"))

# ── ibkrcrypto book (its own data_health block) ───────────────────────────────
def check_ibkrcrypto():
    sp = f"{HOME}/IBKRCrypto/backtest/data/ibkrcrypto/state.json"
    try:
        d = json.load(open(sp)); dh = d.get("data_health", {})
        # S-2026-06-26: check the BOOK's 'updated' AGE, not just its self-reported all_fresh flag
        # (the flag froze stale-True while the book hadn't refreshed in 23h -> NDX/QNDX price stale).
        upd = d.get("updated", "")  # e.g. "2026-06-25 06:27 UTC"
        age_h = None
        try:
            u = dt.datetime.strptime(upd.replace(" UTC", ""), "%Y-%m-%d %H:%M").replace(tzinfo=dt.timezone.utc)
            age_h = (NOW - u.timestamp()) / 3600.0
        except Exception:
            pass
        fresh_age = age_h is None or age_h <= 2.0   # book must have refreshed within 2h
        ok = dh.get("all_fresh", False) and fresh_age
        det = f"book updated {age_h:.1f}h ago" if age_h is not None else "no 'updated' ts"
        return dict(name="ibkrcrypto.book", ok=ok, age=round(age_h,1) if age_h is not None else None,
                    limit=2, sev="HIGH" if not fresh_age else "MED",
                    detail=f"{det}, all_fresh={dh.get('all_fresh')}, stale={dh.get('stale_sources', [])} [Chimera crypto book/NDX]")
    except Exception as e:
        return dict(name="ibkrcrypto.book", ok=False, age=None, limit=2, sev="HIGH", detail=f"state.json: {e}")

# ── run ───────────────────────────────────────────────────────────────────────
checks = [check_feed(f) for f in FEEDS] + [check_ibkrcrypto()]

# AUTO-REFRESH (--fix): for each stale feed with a refresh command, run it, then re-check that feed
if FIX:
    bymap = {f[0]: f for f in FEEDS}
    for c in list(checks):
        if not c["ok"] and c["name"] in bymap and bymap[c["name"]][6]:
            cmd = bymap[c["name"]][6]
            print(f"[auto-fix] {c['name']} stale -> running {' '.join(cmd[:3])} ...", flush=True)
            try:
                subprocess.run(cmd, timeout=2400, capture_output=True)
                idx = next(i for i, x in enumerate(checks) if x["name"] == c["name"])
                checks[idx] = check_feed(bymap[c["name"]])
            except Exception as e:
                print(f"[auto-fix] {c['name']} refresh failed: {e}", flush=True)

all_ok = all(c["ok"] for c in checks)
out = {"ts": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC"), "all_ok": all_ok, "checks": checks}
try:
    os.makedirs(f"{HOME}/Omega/data/rdagent", exist_ok=True)
    json.dump(out, open(f"{HOME}/Omega/data/rdagent/DATA_HEALTH.json", "w"), indent=1)
    fpath = f"{HOME}/Omega/data/rdagent/DATA_HEALTH.flag"
    bad = [f"{c['name']}({c['sev']})" for c in checks if not c["ok"]]
    if bad: open(fpath, "w").write(out["ts"] + " STALE: " + ", ".join(bad) + "\n")
    elif os.path.exists(fpath): os.remove(fpath)
except Exception:
    pass

if not (QUIET and all_ok):
    print(f"=== DATA HEALTH {out['ts']} — {'GO (all fresh)' if all_ok else 'NO-GO (STALE FEED!)'} ===")
    print(f"{'feed':28} {'ok':5} {'sev':5} {'age':>6} {'lim':>5}  detail")
    for c in sorted(checks, key=lambda x: (x["ok"], x["sev"] != "HIGH")):
        age = f"{c['age']}d" if c.get("age") is not None else "-"
        lim = f"{c['limit']}d" if c.get("limit") is not None else "-"
        print(f"{c['name']:28} {('OK' if c['ok'] else 'FAIL'):5} {c['sev']:5} {age:>6} {lim:>5}  {c['detail']}")

if not all_ok:
    try: print("\a", end="")
    except Exception: pass
    sys.exit(1)
sys.exit(0)
