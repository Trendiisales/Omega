#!/usr/bin/env python3
"""seed_refresh.py — UNIFIED warm-seed pipeline (2026-06-29).

Folds the three former scripts into ONE, run in order, each phase independent + non-fatal:
  [1] rebuild — resample captured l2_ticks_*.csv -> warmup OHLC CSVs            (was scripts/rebuild_warmups.py)
  [2] ibkr    — pull fresh bars from IB Gateway (MGC gold + index futures)      (was tools/refresh_warmup_seeds.py)
                SKIPPED gracefully if ib_insync is missing OR the gateway is
                unreachable (connect timeout) -> no more hung deploys at [2b].
  [3] audit   — flag any ENABLED-engine warm-seed older than its threshold      (was tools/seed_freshness_audit.py)

Exit code = the AUDIT result (0 fresh / 1 stale) so the deploy seed-gate still works.
Phases 1+2 NEVER fail the run (a skip just keeps the prior / git-snapshot seed).

Usage:
  py tools/seed_refresh.py [--port 4002] [--days 30] [--repo .] [--seed-dir DIR]
                           [--logs DIR] [--max-age-days N] [--skip-ibkr]
                           [--only rebuild|ibkr|audit]
"""
from __future__ import annotations
import argparse, datetime, glob, os, re, sys, time

# --------------------------------------------------------------------------------------------------
# shared path resolution (VPS layout first, then local/Mac fallbacks) -- matches the former scripts
# --------------------------------------------------------------------------------------------------
def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def _default_dirs():
    win_logs = r"C:\Omega\logs"
    win_out  = r"C:\Omega\phase1\signal_discovery"
    if os.path.isdir(win_logs):
        return win_logs, win_out
    here = _repo_root()
    for logs in (os.path.expanduser("~/Tick/l2_xau_vps"), os.path.join(here, "logs")):
        if os.path.isdir(logs):
            return logs, os.path.join(here, "phase1", "signal_discovery")
    return win_logs, os.path.join(here, "phase1", "signal_discovery")

# ==================================================================================================
# PHASE 1 — rebuild warmup CSVs from captured l2_ticks (no network)
# ==================================================================================================
# (symbol, timeframe_minutes, output_filename, keep_last_bars)
_REBUILD_TARGETS = [
    ("XAUUSD", 15, "warmup_XAUUSD_M15.csv", 6000),
    ("XAUUSD", 30, "warmup_XAUUSD_M30.csv", 4000),
    ("ESTX50",  5, "warmup_ESTX50_M5.csv",  4000),
    ("GER40",  15, "warmup_GER40_M15.csv",  4000),
    ("GER40",  30, "warmup_GER40_M30.csv",  3500),
    ("NAS100", 15, "warmup_NAS100_M15.csv", 6000),
    ("NAS100", 30, "warmup_NAS100_M30.csv", 4000),
    ("UK100",  30, "warmup_UK100_M30.csv",  3500),
    ("UK100", 240, "warmup_UK100_M240.csv", 2000),
]

def _load_ticks(pd, logs_dir, symbol, days):
    paths = sorted(glob.glob(os.path.join(logs_dir, f"l2_ticks_{symbol}_*.csv")))
    if not paths:
        return None
    paths = paths[-days:] if days > 0 else paths
    frames = []
    for p in paths:
        try:
            df = pd.read_csv(p, usecols=["ts_ms", "mid"], dtype={"ts_ms": "int64", "mid": "float64"})
        except Exception as e:
            print(f"  [skip] {os.path.basename(p)}: {e}"); continue
        df = df[(df["mid"] > 0) & (df["ts_ms"] > 0)]
        if len(df):
            frames.append(df)
    if not frames:
        return None
    return pd.concat(frames, ignore_index=True)

def _resample(pd, df, tf_min):
    df = df.drop_duplicates(subset="ts_ms").sort_values("ts_ms")
    idx = pd.to_datetime(df["ts_ms"], unit="ms", utc=True)
    s = pd.Series(df["mid"].values, index=idx)
    bars = s.resample(f"{tf_min}min").agg(["first", "max", "min", "last"]).dropna()
    bars.columns = ["open", "high", "low", "close"]
    # Epoch-ms extraction, unit- AND tz-safe. History (2026-07-08, two failed forms):
    #   .astype(int64)//1e6            -- assumes [ns]; pandas 2.x resample yields [ms]
    #                                     here -> emitted epoch-KILOSECONDS (ts=1783467)
    #   .astype('datetime64[ns]')      -- raises on this tz-AWARE index (utc=True above)
    # as_unit('ms').asi8 = int64 epoch-ms regardless of index unit; asi8 is UTC-based
    # so tz-awareness is fine. pandas <2 fallback: index is always [ns].
    try:
        bars["bar_start_ms"] = bars.index.as_unit("ms").asi8
    except AttributeError:
        bars["bar_start_ms"] = bars.index.asi8 // 1_000_000
    return bars[["bar_start_ms", "open", "high", "low", "close"]]

def phase_rebuild(logs, out, days):
    print("\n========== [1/3] REBUILD warmups from l2_ticks ==========")
    try:
        import pandas as pd
    except Exception as e:
        print(f"  [skip-phase] pandas unavailable: {e} -- keeping git snapshots"); return
    print(f"  logs={logs} out={out} days={days}")
    os.makedirs(out, exist_ok=True)
    rebuilt = skipped = 0
    for symbol, tf_min, out_name, keep in _REBUILD_TARGETS:
        ticks = _load_ticks(pd, logs, symbol, days)
        if ticks is None or len(ticks) < 100:
            print(f"  [skip] {out_name}: no l2_ticks for {symbol} (keeps git snapshot)"); skipped += 1; continue
        bars = _resample(pd, ticks, tf_min)
        if len(bars) < 50:
            print(f"  [skip] {out_name}: only {len(bars)} bars resampled"); skipped += 1; continue
        if keep > 0:
            bars = bars.tail(keep)
        out_path = os.path.join(out, out_name); tmp = out_path + ".tmp"
        bars.to_csv(tmp, index=False); os.replace(tmp, out_path)   # atomic
        print(f"  [ok]   {out_name}: {len(bars)} M{tf_min} bars, last close={float(bars['close'].iloc[-1]):.2f}")
        rebuilt += 1
    print(f"  [1/3] done: {rebuilt} rebuilt, {skipped} skipped")

# ==================================================================================================
# PHASE 2 — refresh warmup seeds from IB Gateway (MGC gold + index futures). NON-FATAL.
# ==================================================================================================
_TF = {
    "M5": ("5 mins","10 D"), "M15": ("15 mins","20 D"), "M30": ("30 mins","40 D"),
    "H1": ("1 hour","90 D"), "M240": ("4 hours","300 D"), "H4": ("4 hours","300 D"), "D1": ("1 day","5 Y"),
}
_GOLD_TFS = ["M5", "M15", "M30", "H1", "H4", "D1"]
_INDEX = {
    "NAS100": (("CME","NQ"),      ["H1","M30","M15","M5","D1"]),  # D1 added S-2026-07-12c (was aging, no generator)
    "USTEC":  (("CME","NQ"),      ["D1"]),                        # S-2026-07-12c: warmup_USTEC_D1 had NO refresh path (43d old, 8+ consumers)
    "US500":  (("CME","ES"),      ["H1","D1"]),
    "GER40":  (("EUREX","DAX"),   ["H1","H4","M30","M15","D1"]),
    "UK100":  (("ICEEU","Z"),     ["M30","M240","D1"]),
    "DJ30":   (("CBOT","YM"),     ["H1","D1"]),
    "ESTX50": (("EUREX","ESTX50"),["D1","M5"]),
}
# consumer filenames that must MIRROR a refreshed CSV. A warmup whose exact filename is
# outside this module never refreshes and rots until the deploy seed gate aborts (the
# S-2026-07-12c XAUUSD_H1_BRC abort). Keep every alias here, not as a hand-copied file.
_ALIASES = [
    ("warmup_GER40_D1.csv", "warmup_GER40_D1_idx.csv"),   # IndexSeasonal/ToM GER40 variant (was 43d old)
]

def phase_ibkr(port, seed_dir):
    print("\n========== [2/3] REFRESH seeds from IB Gateway ==========")
    try:
        from ib_insync import IB, ContFuture
    except Exception as e:
        print(f"  [skip-phase] ib_insync unavailable: {e} -- keeping prior seeds"); return
    os.makedirs(seed_dir, exist_ok=True)

    def write_csv(path, bars, ms=True):
        with open(path, "w") as f:
            f.write("ts,o,h,l,c\n")
            for b in bars:
                t = b.date
                ep = int(t.timestamp()) if hasattr(t, "timestamp") else int(datetime.datetime(t.year,t.month,t.day).timestamp())
                if ms: ep *= 1000
                f.write(f"{ep},{b.open},{b.high},{b.low},{b.close}\n")
        return len(bars)

    def write_mgc_hist(path, bars):
        with open(path, "w") as f:
            f.write("ts,o,h,l,c,v\n")
            for b in bars:
                f.write(f"{int(b.date.timestamp())},{b.open},{b.high},{b.low},{b.close},{getattr(b,'volume',0) or 0}\n")
        return len(bars)

    def agg_h4_from_h1(h1_bars, out_path):
        buckets = {}
        for b in h1_bars:
            ep = int(b.date.timestamp()); k = (ep // 14400) * 14400
            if k not in buckets: buckets[k] = [b.open, b.high, b.low, b.close]
            else:
                buckets[k][1] = max(buckets[k][1], b.high); buckets[k][2] = min(buckets[k][2], b.low); buckets[k][3] = b.close
        rows = sorted(buckets.items())
        with open(out_path, "w") as f:
            f.write("ts,o,h,l,c\n")
            for k, (o,h,l,c) in rows: f.write(f"{k*1000},{o},{h},{l},{c}\n")
        return len(rows)

    ib = IB()
    try:
        ib.connect("127.0.0.1", port, clientId=91, timeout=20)   # connect timeout: fail fast if gateway down
    except Exception as e:
        print(f"  [skip-phase] cannot connect IB @ {port}: {e} -- keeping prior seeds"); return

    def pull(con, tf):
        bs, dur = _TF[tf]
        return ib.reqHistoricalData(con, "", durationStr=dur, barSizeSetting=bs,
                                    whatToShow="TRADES", useRTH=False, timeout=45)  # per-pull timeout
    ok, fail = [], []
    try:
        # ---- GOLD (XAUUSD_*) from MGC COMEX future ----
        try:
            mgc = ContFuture("MGC", "COMEX", "USD"); ib.qualifyContracts(mgc)
            h1_gold = m30_gold = None
            for tf in _GOLD_TFS:
                try:
                    bars = pull(mgc, tf)
                    if bars and len(bars) > 10:
                        n = write_csv(f"{seed_dir}/warmup_XAUUSD_{tf}.csv", bars, ms=True); ok.append(f"XAUUSD_{tf}({n})")
                        if tf == "H1":  h1_gold = bars
                        if tf == "M30": m30_gold = bars
                    elif tf == "H4" and h1_gold:
                        ok.append(f"XAUUSD_H4(agg<-H1,{agg_h4_from_h1(h1_gold, seed_dir + '/warmup_XAUUSD_H4.csv')})")
                    else:
                        fail.append(f"XAUUSD_{tf}(no bars)")
                except Exception as e:
                    fail.append(f"XAUUSD_{tf}({e})")
            if h1_gold and not any(s.startswith("XAUUSD_H4") for s in ok):
                try: ok.append(f"XAUUSD_H4(agg<-H1,{agg_h4_from_h1(h1_gold, seed_dir + '/warmup_XAUUSD_H4.csv')})")
                except Exception as e: fail.append(f"XAUUSD_H4-agg({e})")
            if h1_gold:
                try: ok.append(f"mgc_h1_hist({write_mgc_hist('data/mgc_h1_hist.csv', h1_gold)})")
                except Exception as e: fail.append(f"mgc_h1_hist({e})")
            if m30_gold:
                try: ok.append(f"mgc_30m_hist({write_mgc_hist('data/mgc_30m_hist.csv', m30_gold)})")
                except Exception as e: fail.append(f"mgc_30m_hist({e})")
        except Exception as e:
            fail.append(f"MGC-qualify({e})")

        # ---- tsmom regime seed (gold_regime) is H1 -> mirror warmup_XAUUSD_H1 ----
        try:
            src = f"{seed_dir}/warmup_XAUUSD_H1.csv"
            if os.path.exists(src):
                import shutil; shutil.copyfile(src, f"{seed_dir}/tsmom_warmup_H1.csv"); ok.append("tsmom_warmup_H1(<-XAUUSD_H1)")
        except Exception as e:
            fail.append(f"tsmom_copy({e})")

        # ---- INDICES (index futures) ----
        for sym, ((exch, ibsym), tfs) in _INDEX.items():
            con = None
            try:
                c = ContFuture(ibsym, exch); ib.qualifyContracts(c)
                if getattr(c, "conId", 0): con = c
            except Exception as e:
                fail.append(f"{sym}(qualify {e})"); continue
            if con is None:
                fail.append(f"{sym}(no future {exch}:{ibsym})"); continue
            for tf in tfs:
                try:
                    bars = pull(con, tf)
                    if bars and len(bars) > 10:
                        ok.append(f"{sym}_{tf}({write_csv(f'{seed_dir}/warmup_{sym}_{tf}.csv', bars, ms=True)})")
                    else:
                        fail.append(f"{sym}_{tf}(no bars)")
                except Exception as e:
                    fail.append(f"{sym}_{tf}({e})")
    finally:
        try: ib.disconnect()
        except Exception: pass
    # ---- alias mirrors (same-name-drift guard) ----
    for src_name, dst_name in _ALIASES:
        try:
            src = f"{seed_dir}/{src_name}"
            if os.path.exists(src):
                import shutil; shutil.copyfile(src, f"{seed_dir}/{dst_name}"); ok.append(f"{dst_name}(<-{src_name})")
        except Exception as e:
            fail.append(f"{dst_name}({e})")
    print(f"  refreshed ({len(ok)}): " + ", ".join(ok))
    print(f"  skipped/failed ({len(fail)}): " + ", ".join(fail))

# ==================================================================================================
# PHASE 3 — freshness audit (sets the exit code)
# ==================================================================================================
_OVERRIDES = [
    (re.compile(r"tsmom_warmup_H1|regime|_H1\.csv|_h1\.csv", re.I), 5),
    (re.compile(r"_D1\.csv|daily|seasonal|warmup_.*_D1", re.I),    45),
    (re.compile(r"_H4\.csv|_h4\.csv|H4", re.I),                    14),
]
_EXCLUDE  = re.compile(r"logs[/\\]shadow[/\\]|tsmom_v2|[%<>*]|\.\.\.", re.I)
_DISABLED = re.compile(r"(EURUSD|GBPUSD|USDJPY|AUDUSD|NZDUSD|USDCAD|EURGBP)", re.I)

def _last_ts(path):
    try:
        with open(path, "rb") as fh:
            fh.seek(0, os.SEEK_END); size = fh.tell()
            # 64KB tail: wide close CSVs (500+ cols) run ~10KB/row, so 4KB was < one
            # row -> the window held a single mid-row fragment and c0 was a stray price.
            back = min(size, 65536); fh.seek(size - back); tail = fh.read().decode("latin-1")
        lines = tail.splitlines()
        # if we didn't read from byte 0, the first line is a partial row fragment -> drop it
        if size > back and len(lines) > 1: lines = lines[1:]
        for line in reversed(lines):
            a = line.split(",")
            if not a or not a[0]: continue
            c0 = a[0].strip()
            # numeric epoch first column (tick/bar CSVs)
            try:
                ts = float(c0)
                if ts <= 0: continue
                if ts > 1e11: ts /= 1000.0
                return ts
            except ValueError:
                pass
            # ISO date-string index (wide close CSVs: "Date,NVDA,..." / "2026-07-02,...")
            # float() fails on these -> without this branch _last_ts falls through to a
            # stray price fragment in the partial tail window and reports epoch~0 (1970),
            # a false STALE that aborts the deploy on a perfectly fresh file.
            m = re.match(r"^(\d{4})-(\d{2})-(\d{2})", c0)
            if m:
                try:
                    return datetime.datetime(int(m.group(1)), int(m.group(2)), int(m.group(3)),
                                             tzinfo=datetime.timezone.utc).timestamp()
                except ValueError:
                    continue
    except Exception:
        return None
    return None

def phase_audit(repo, max_age_days):
    print("\n========== [3/3] SEED-FRESHNESS AUDIT ==========")
    def threshold_for(p):
        for rx, d in _OVERRIDES:
            if rx.search(p): return d
        return max_age_days
    seed_paths = set()
    src = glob.glob(os.path.join(repo, "include", "*.hpp")) + glob.glob(os.path.join(repo, "include", "*.cpp"))
    for f in src:
        try: txt = open(f, errors="ignore").read()
        except Exception: continue
        for m in re.finditer(r'"([^"]+\.csv)"', txt):
            p = m.group(1)
            if "signal_discovery" in p or "warmup" in p.lower() or "tsmom" in p.lower() or "/data/" in p or p.startswith("data/"):
                if _EXCLUDE.search(p): continue
                seed_paths.add(p)
    now = time.time(); stale, ok, missing, skipped = [], [], [], []
    for rel in sorted(seed_paths):
        cand = os.path.join(repo, rel)
        if not os.path.exists(cand): cand = rel
        if not os.path.exists(cand): missing.append(rel); continue
        ts = _last_ts(cand)
        if ts is None: missing.append(rel); continue
        age_d = (now - ts) / 86400.0; thr = threshold_for(rel)
        rec = (rel, age_d, thr, datetime.datetime.utcfromtimestamp(ts).strftime("%Y-%m-%d"))
        if age_d > thr and _DISABLED.search(rel): skipped.append(rec); continue
        (stale if age_d > thr else ok).append(rec)
    print(f"  active seed CSVs: {len(seed_paths)}  ok: {len(ok)}  STALE(enabled): {len(stale)}  skipped(disabled): {len(skipped)}  missing: {len(missing)}")
    for rel, age, thr, d in sorted(stale, key=lambda r:-r[1]):
        print(f"  [SEED-STALE] {rel}  last {d} = {age:.0f}d (> {thr}d) -> ENGINE BOOTS BLIND")
    for rel in missing:
        print(f"  [SEED-MISSING] {rel}")
    if stale:
        print(f"  *** {len(stale)} STALE SEED(S) on ENABLED engines -- FIX. ***")
        return 1
    print(f"  All enabled-engine seeds fresh. ({len(skipped)} disabled-engine ignored.)")
    return 0

# ==================================================================================================
def main():
    dl, do = _default_dirs()
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4002)
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--seed-dir", default=do)
    ap.add_argument("--logs", default=dl)
    ap.add_argument("--max-age-days", type=int, default=14)
    ap.add_argument("--skip-ibkr", action="store_true")
    ap.add_argument("--only", choices=["rebuild", "ibkr", "audit"], default=None)
    a = ap.parse_args()

    if a.only in (None, "rebuild"): phase_rebuild(a.logs, a.seed_dir, a.days)
    if a.only in (None, "ibkr") and not a.skip_ibkr: phase_ibkr(a.port, a.seed_dir)
    elif a.only is None and a.skip_ibkr: print("\n========== [2/3] IBKR refresh SKIPPED (--skip-ibkr) ==========")
    rc = 0
    if a.only in (None, "audit"): rc = phase_audit(a.repo, a.max_age_days)
    sys.exit(rc)

if __name__ == "__main__":
    main()
