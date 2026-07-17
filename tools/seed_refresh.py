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
    # ESTX50 M5 recipe DROPPED S-2026-07-14 (latent-class sweep item 14d): warmup_ESTX50_M5.csv
    # has NO consumer engine (grep include/ + Survivor dynamic roster = zero readers; the M5
    # straddle cells that read it are gone). Orphan recipes hide real roster drift -- re-add
    # only together with a consumer.
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
    "M5": ("5 mins","10 D"), "M10": ("10 mins","15 D"), "M15": ("15 mins","20 D"), "M30": ("30 mins","40 D"),
    "H1": ("1 hour","90 D"), "M240": ("4 hours","300 D"), "H4": ("4 hours","300 D"), "D1": ("1 day","5 Y"),
}
_GOLD_TFS = ["M5", "M15", "M30", "H1", "H4", "D1"]   # M10 dropped S-2026-07-13 (only consumer XauBracketCascade_M10 culled)
_INDEX = {
    # ORPHAN-RECIPE CULL S-2026-07-14 (latent-class sweep item 14d): NAS100 M5, GER40 H4,
    # DJ30 H1, ESTX50 M5 dropped -- their warmup CSVs have NO consumer engine anywhere
    # (grep include/ literals + SurvivorPortfolio dynamic seed_all roster = zero readers;
    # seed_freshness_audit --registry-only flagged all four [orphan-refresh]). A recipe
    # with no consumer is roster drift waiting to hide a real one. Re-add WITH a consumer.
    "NAS100": (("CME","NQ"),      ["H1","M30","M15","D1"]),       # D1 S-2026-07-12c; H4 dropped S-2026-07-13 (BrkCascade culled); M5 dropped S-2026-07-14 (orphan)
    "USTEC":  (("CME","NQ"),      ["D1"]),                        # S-2026-07-12c: warmup_USTEC_D1 had NO refresh path (43d old, 8+ consumers)
    "USTEC.F":(("CME","NQ"),      ["H4"]),                        # S-2026-07-14: SurvivorPortfolio USTEC cells (RSI_N7 + ZMR, ACTIVE) seed
                                                                  # warmup_USTEC.F_H4.csv via the DYNAMIC seed_all() path (SurvivorPortfolio.hpp:808);
                                                                  # it had NO refresh path AND was audit-blind -> rotted 94d unseen.
    "US500":  (("CME","ES"),      ["H1","D1"]),                   # H4 dropped S-2026-07-13 (BrkCascade culled)
    "M2K":    (("CME","M2K"),     ["H1"]),                        # S-2026-07-13: warmup_M2K_H1 refresh kept (FxLadder M2K seeds from it; cascade cells culled)
    "GER40":  (("EUREX","DAX"),   ["H1","M30","M15","D1"]),       # H4 dropped S-2026-07-14 (orphan; its consumer's seed call is long gone)
    "UK100":  (("ICEEU","Z"),     ["M30","M240","D1"]),
    "DJ30":   (("CBOT","YM"),     ["D1"]),                        # H1 dropped S-2026-07-14 (orphan)
    "ESTX50": (("EUREX","ESTX50"),["D1"]),                        # M5 dropped S-2026-07-14 (orphan)
}
# Seeds whose consumer does NOT normalize ms->sec: SurvivorPortfolio Cell::seed_from_csv
# reads column 0 as SECONDS verbatim (b.ts_sec = atoll(tok[0])) -- an ms-format file would
# feed it year-58000 timestamps. Write these with ms=False. (FxLadder norm_ts_ tolerates both.)
_SEC_TS = {"USTEC.F"}
# FX spot seeds (IDEALPRO, MIDPOINT -- forex has no TRADES prints). S-2026-07-14: FxLadder
# GBPUSD (re-enabled S-2026-07-09c, engine_init.hpp FL[]) boot-seeds warmup_GBPUSD_H1.csv,
# which had NO refresh path (data ended 2026-04-11) and sat behind the audit's FX skip-regex.
# ts written in SECONDS (repo H1 convention; FxLadderPair::norm_ts_ tolerates both).
_FOREX = {
    "GBPUSD": ["H1"],
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
        from ib_insync import IB, ContFuture, Forex
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
            h1_gold = m30_gold = h4_gold = m15_gold = None
            for tf in _GOLD_TFS:
                try:
                    bars = pull(mgc, tf)
                    if bars and len(bars) > 10:
                        n = write_csv(f"{seed_dir}/warmup_XAUUSD_{tf}.csv", bars, ms=True); ok.append(f"XAUUSD_{tf}({n})")
                        if tf == "H1":  h1_gold = bars
                        if tf == "M30": m30_gold = bars
                        if tf == "H4":  h4_gold = bars
                        if tf == "M15": m15_gold = bars
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
            # S-2026-07-14: boot warm-seeds for the sub-30m DON cells
            # (GoldDon10m, omega_main.hpp; GoldDon15m CULLED S-2026-07-17s --
            # mgc_15m_hist regen kept harmless, no engine consumes it). M15 reuses the warmup
            # pull above; M10 is a dedicated pull -- it is NOT in _GOLD_TFS so
            # no warmup_XAUUSD_M10.csv orphan is recreated (S-2026-07-13 cull).
            if m15_gold:
                try: ok.append(f"mgc_15m_hist({write_mgc_hist('data/mgc_15m_hist.csv', m15_gold)})")
                except Exception as e: fail.append(f"mgc_15m_hist({e})")
            try:
                m10_gold = pull(mgc, "M10")
                if m10_gold and len(m10_gold) > 10:
                    ok.append(f"mgc_10m_hist({write_mgc_hist('data/mgc_10m_hist.csv', m10_gold)})")
                else:
                    fail.append("mgc_10m_hist(no bars)")
            except Exception as e:
                fail.append(f"mgc_10m_hist({e})")
            # data/mgc_h4_hist.csv: boot warm-seed of g_mgc_tf_4h (ENABLED,
            # omega_main.hpp warmup_or_die). MgcFastDonchianFeed.hpp says it is
            # "regenerated at deploy" but S-2026-07-14 found NO regenerator existed
            # anywhere -- the file was static since 2026-07-07 and would cross its
            # 14d threshold unwatched. Regenerate nightly here from the MGC H4 pull
            # (fallback: aggregate from H1). Loader norms sec/ms; keep the file's
            # existing bar_start_ms header + ms values.
            if h4_gold or h1_gold:
                try:
                    if h4_gold:
                        with open("data/mgc_h4_hist.csv", "w") as f:
                            f.write("bar_start_ms,open,high,low,close\n")
                            for b in h4_gold:
                                f.write(f"{int(b.date.timestamp())*1000},{b.open},{b.high},{b.low},{b.close}\n")
                            n = len(h4_gold)
                    else:
                        n = agg_h4_from_h1(h1_gold, "data/mgc_h4_hist.csv")
                    ok.append(f"mgc_h4_hist({n})")
                except Exception as e:
                    fail.append(f"mgc_h4_hist({e})")
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
                        ok.append(f"{sym}_{tf}({write_csv(f'{seed_dir}/warmup_{sym}_{tf}.csv', bars, ms=sym not in _SEC_TS)})")
                    else:
                        fail.append(f"{sym}_{tf}(no bars)")
                except Exception as e:
                    fail.append(f"{sym}_{tf}({e})")

        # ---- FX SPOT (IDEALPRO, MIDPOINT) -- see _FOREX comment above ----
        for sym, tfs in _FOREX.items():
            fx = None
            try:
                c = Forex(sym); ib.qualifyContracts(c)
                if getattr(c, "conId", 0): fx = c
            except Exception as e:
                fail.append(f"{sym}(qualify {e})"); continue
            if fx is None:
                fail.append(f"{sym}(no IDEALPRO pair)"); continue
            for tf in tfs:
                try:
                    bs, dur = _TF[tf]
                    bars = ib.reqHistoricalData(fx, "", durationStr=dur, barSizeSetting=bs,
                                                whatToShow="MIDPOINT", useRTH=False, timeout=45)
                    if bars and len(bars) > 10:
                        ok.append(f"{sym}_{tf}({write_csv(f'{seed_dir}/warmup_{sym}_{tf}.csv', bars, ms=False)})")
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
# SINGLE SOURCE OF TRUTH (S-2026-07-14): this phase previously carried its OWN copy of the
# audit (thresholds/skip-regex/extraction) "kept in sync" with tools/seed_freshness_audit.py
# by hand -- and it drifted (SPXUSD skip existed in one copy only). The audit now lives in
# EXACTLY ONE file, tools/seed_freshness_audit.py (freshness + dynamic-Survivor paths +
# structural NO-REFRESH-PATH registry gate); this phase just runs it and passes the exit
# code through, so the nightly task and the deploy gate can never diverge again.
def phase_audit(repo, max_age_days):
    print("\n========== [3/3] SEED-FRESHNESS AUDIT (tools/seed_freshness_audit.py) ==========")
    import subprocess
    script = os.path.join(repo, "tools", "seed_freshness_audit.py")
    if not os.path.exists(script):
        print(f"  [P1] {script} MISSING -- audit cannot run; failing closed."); return 1
    try:
        return subprocess.call([sys.executable, script, "--repo", repo,
                                "--max-age-days", str(max_age_days)])
    except Exception as e:
        print(f"  [P1] audit subprocess failed: {e} -- failing closed."); return 1


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
