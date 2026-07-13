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

# auto-heal command for the daily index+gold feeds (yfinance, Gateway-independent)
DAILY_YF = ["/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python",
            f"{HOME}/Omega/tools/refresh_daily_feeds.py"]

# ── THE REGISTRY: every live-decision feed. kind: file | percol | json_book ────
FEEDS = [
    # name, path, max_age_d, kind, severity, consumer, refresh_cmd (or None), extra
    ("rdagent.sp500_long_close", f"{HOME}/Omega/data/rdagent/sp500_long_close.csv", 5, "percol", "HIGH",
     "rdagent GUI + paper basket fills",
     # FIX 2026-06-29: was refresh_close_ibkr.py (BROKEN — IBKR Gateway clientId/port fails) -> auto-heal
     # never worked. Now the yfinance refresher (Gateway-independent, proven).
     ["/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python", f"{HOME}/Omega/tools/rdagent/refresh_close_yf.py"], {"max_stale_frac": 0.15}),
    ("rdagent.qlib_calendar", f"{HOME}/.qlib/qlib_data/omega_data/calendars/day.txt", 4, "file", "HIGH",
     "rdagent model rankings", None, {}),
    # daily index+gold feeds -> auto-heal via the unified yfinance refresher (Gateway-independent).
    ("tick.NDX_daily", f"{HOME}/Tick/NDX_daily_2016_2026.csv", 4, "file", "MED",
     "index research", DAILY_YF, {}),
    ("tick.SPX_daily", f"{HOME}/Tick/SPX_daily_2016_2026.csv", 4, "file", "MED",
     "index turtle + freq_dd_frontier backtest", DAILY_YF, {}),
    ("tick.DJ30_daily", f"{HOME}/Tick/DJ30_daily_2016_2026.csv", 4, "file", "MED",
     "index turtle + backtest", DAILY_YF, {}),
    ("tick.GER40_daily", f"{HOME}/Tick/GER40_daily_2016_2026.csv", 4, "file", "MED",
     "index backtest", DAILY_YF, {}),
    ("tick.XAU_daily", f"{HOME}/Tick/2yr_XAUUSD_daily.csv", 4, "file", "MED",
     "gold D1 trend seed + backtest", DAILY_YF, {}),
    # the LIVE C++ MacroGoldGate reads logs/macro/macro_gold_gate.tsv (last field = stamp_ms);
    # macro_gold_gate.py is the producer (fetches real-yield + dollar from source). Monitor the
    # ACTUAL gate input, auto-refresh by re-running the producer.
    ("macrogoldgate.tsv", f"{HOME}/Omega/logs/macro/macro_gold_gate.tsv", 2, "stamp_ms", "HIGH",
     "MacroGoldGate (LIVE gold gate input)",
     ["python3", f"{HOME}/Omega/tools/macro_gold_gate.py"], {}),
    # 2026-07-12 (audit A3): watch the MIRRORED BOX seed, not the Mac repo copy.
    # OmegaSeedRefresh refreshes the box seed nightly (that's what the engine
    # loads); the Mac repo copy never updates -> false 17d-stale alarms.
    # pull_vps_display.sh mirrors the box files every 5min.
    ("gold.mgc_h1_seed", f"{HOME}/Omega-vps-mirror/data/mgc_h1_hist.csv", 7, "file", "MED",
     "gold engine warm-seed (box copy via mirror)",
     ["python3", f"{HOME}/Omega/tools/mgc_pull_history.py"], {}),
    ("gold.mgc_30m_seed", f"{HOME}/Omega-vps-mirror/data/mgc_30m_hist.csv", 7, "file", "MED",
     "gold engine warm-seed (box copy via mirror)",
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
    # S-2026-07-02: live crypto state moved to ~/Crypto on the 2026-07-01
    # IBKRCrypto->Crypto consolidation; this monitor kept reading the OLD path's
    # frozen state.json -> stale warnings against a book that had moved.
    sp = f"{HOME}/Crypto/backtest/data/ibkrcrypto/state.json"
    if not os.path.exists(sp):
        sp = f"{HOME}/IBKRCrypto/backtest/data/ibkrcrypto/state.json"  # pre-cutover fallback
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
        # NDX mark: S-2026-07-12 corrected check. shadow_refresh.cpp was REDESIGNED to
        # daily-close-ONLY (the old live ssh-NQ mark was removed -> src is NEVER "IBKR-NQ"
        # anymore). The prior check required IBKR-NQ -> it false-FAILed HIGH on EVERY run =
        # permanent alarm-fatigue that hid real failures. Correct semantics: daily-close is
        # the DESIGN; only stale if the NDX DAILY FEED itself is stale. So check the daily
        # CSV's last-bar age (weekend-aware: NDX trades weekdays, allow 4d).
        ndx_src = d.get("ndx_mark_src", "")
        ndx_ok, ndx_det = True, ndx_src or "NONE"
        if ndx_src != "IBKR-NQ":  # daily-close mark -> validate the daily feed's freshness
            try:
                ncsv = f"{HOME}/Tick/NDX_daily_2016_2026.csv"
                last_ts = int(open(ncsv).read().rstrip().rsplit("\n",1)[-1].split(",")[0])
                nage_d = (NOW - last_ts) / 86400.0
                ndx_ok = nage_d <= 4.0   # 4d covers a Fri->Mon weekend gap
                ndx_det = f"daily-close feed {nage_d:.1f}d old" + ("" if ndx_ok else " STALE!")
            except Exception as e:
                ndx_ok, ndx_det = False, f"daily-close feed unreadable: {e}"
        ok = dh.get("all_fresh", False) and fresh_age and ndx_ok
        det = f"book updated {age_h:.1f}h ago" if age_h is not None else "no 'updated' ts"
        return dict(name="ibkrcrypto.book", ok=ok, age=round(age_h,1) if age_h is not None else None,
                    limit=2, sev="HIGH" if (not fresh_age or not ndx_ok) else "MED",
                    detail=f"{det}, ndx_mark={ndx_det}, all_fresh={dh.get('all_fresh')}, stale={dh.get('stale_sources', [])} [Chimera crypto book/NDX]")
    except Exception as e:
        return dict(name="ibkrcrypto.book", ok=False, age=None, limit=2, sev="HIGH", detail=f"state.json: {e}")

# ── VIX term-structure engine (deployed paper engine; launchd daily 09:00) ────
# S-2026-06-26: deployed engine -> enrolled so a DEAD launchd job can't silently leave it dormant
# (operator rule: never let a built+deployed engine go un-run/forgotten). last_run_date should be a
# recent weekday; >4d stale (weekday + holiday slack) = the daily job stopped firing -> FAIL.
def check_vix_engine():
    sp = f"{HOME}/vix-engine/.state/state.json"
    try:
        d = json.load(open(sp))
        lr = d.get("last_run_date")
        age = (TODAY - dt.date.fromisoformat(lr)).days if lr else None
        ok = age is not None and age <= 4
        return dict(name="vix-term.engine", ok=ok, age=age, limit=4, sev="MED",
                    detail=f"last_run {lr} ({age}d ago), open={len(d.get('open',[]))} closed={len(d.get('closed',[]))} [VIX-term paper engine, launchd daily]")
    except Exception as e:
        return dict(name="vix-term.engine", ok=False, age=None, limit=4, sev="MED", detail=f"state.json: {e} [VIX-term engine — launchd dead?]")

# ── RD-Agent paper basket (auto-executed daily; operator: nothing manual, everything tracked) ──
def check_rdagent_basket():
    sp = f"{HOME}/Omega/data/rdagent/factor_basket_result.json"
    try:
        d = json.load(open(sp)); ts = d.get("ts","")
        age = (NOW - dt.datetime.fromisoformat(ts).timestamp())/86400.0 if ts else None
        # MODEL AGE: the basket EXECUTES daily, but the underlying model (as_of) can silently go
        # stale -- a fresh exec on an 8-day-old model passed before (2026-06-26). Check BOTH now.
        mdl = d.get("as_of","")
        mage = (NOW - dt.datetime.strptime(mdl,"%Y-%m-%d").replace(tzinfo=dt.timezone.utc).timestamp())/86400.0 if mdl else None
        exec_ok = age is not None and age <= 2
        model_ok = mage is not None and mage <= 3
        ok = exec_ok and model_ok
        warn = "" if model_ok else f" *** MODEL STALE {mage:.0f}d -- qlib retrain owed ***"
        return dict(name="rdagent.basket", ok=ok, age=round(age,1) if age is not None else None, limit=2, sev="MED",
                    detail=f"exec {age:.1f}d ago, model {('%.0fd old'%mage) if mage is not None else '?'}{warn}, book={list((d.get('book') or {}).keys())} [RD-Agent paper basket]")
    except Exception as e:
        return dict(name="rdagent.basket", ok=False, age=None, limit=2, sev="MED", detail=f"result.json: {e} [basket executor not run?]")

# ── Jo engine (tight-trail Luke companion; launchd daily; operator: everything tracked) ──
def check_jo_engine():
    sp = f"{HOME}/jo-engine/jo_state.json"
    try:
        d = json.load(open(sp)); upd = d.get("updated","")
        age = (NOW - dt.datetime.strptime(upd.replace(" UTC",""), "%Y-%m-%d %H:%M").replace(tzinfo=dt.timezone.utc).timestamp())/86400.0 if upd else None
        ok = age is not None and age <= 2
        return dict(name="jo.engine", ok=ok, age=round(age,1) if age is not None else None, limit=2, sev="MED",
                    detail=f"ran {age:.1f}d ago, open={d.get('n_open')} closed={d.get('n_closed')} WR={d.get('wr')}% pnl=${d.get('total_pnl')} [Jo tight-trail Luke companion, launchd daily]")
    except Exception as e:
        return dict(name="jo.engine", ok=False, age=None, limit=2, sev="MED", detail=f"jo_state.json: {e} [Jo engine not run?]")

# ── run ───────────────────────────────────────────────────────────────────────
# S-2026-07-13: check_ibkrcrypto() RETIRED from the scan. The ibkrcrypto book was
# consolidated to ~/Crypto (01-07) then the whole crypto system moved to josgp1; the
# Mac state.json it read is frozen/retired, so this check FAILed on age (>2h) on EVERY
# run -> a permanent data-health RED that was pure noise (alarm fatigue hiding real
# stale feeds). The LIVE crypto book is monitored separately via the Chimera crypto
# heartbeat (feeds_selftest 'crypto book heartbeat') + NDX daily freshness (tick NDX
# daily). Function kept for reference; no longer enrolled.
checks = [check_feed(f) for f in FEEDS] + [check_vix_engine(), check_rdagent_basket()]

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
