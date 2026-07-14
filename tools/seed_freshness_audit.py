#!/usr/bin/env python3
# SEED-FRESHNESS AUDIT (2026-06-23) — the safeguard that was missing.
#
# WHY: the gold_regime() bear-gate silently went BLIND for weeks because its warm-seed CSV
# (tsmom_warmup_H1.csv) was 83 days stale (April-1, gold 4692 vs live 4104) and NOTHING checked
# it. A stale warm-seed means an engine/gate boots with a price view detached from reality ->
# it mis-fires (here: never flipped bear -> long-only gold engines bought into the downtrend).
#
# WHAT: extract every ACTIVE warm-seed CSV referenced in the engine code (seed_from_*("...")),
# read each file's LAST bar timestamp, compute its age, and FLAG any that is older than its
# threshold. Run at deploy (called from VERIFY_STARTUP.ps1) AND on a daily schedule so a seed
# can never silently rot again. Exit code 1 if any active seed is stale.
#
# IMPORT-SAFE (S-2026-07-14, latent-class sweep item 12): the executable audit body lives in
# main() under the __main__ guard (same pattern as tools/seed_refresh.py), so sibling tools can
# IMPORT the shared tables (OVERRIDES / MAX_AGE_DAYS / threshold_for) instead of hand-copying
# them. tools/engine_state_audit.py does exactly that — its private thr() copy had drifted
# (missing the XAUUSD_M1 45d override). Derive-don't-copy: this file is the SINGLE copy.
#
# Usage:  python3 tools/seed_freshness_audit.py [--repo <dir>] [--max-age-days N]
import os, re, sys, glob, time, datetime

REPO = "."
MAX_AGE_DAYS = 14            # default; intraday/H1 seeds should be fresher than this
# per-pattern overrides (a daily/seasonal seed can be older than an H1 regime seed)
OVERRIDES = [
    # GoldCampaignD1Anch M1 seed: monthly-histdata sourced (prior month publishes early-next-
    # month -> 17-40d age is NORMAL) and the IBKR reseed has NO M1 recipe (MGC spread scale !=
    # spot XAUUSD; a wrong-scale spread seed would misfire the engine's 1.5x-slot-median gate).
    # Engine tolerates by design: days_seen>=5 + slot windows re-warm from live ticks in 6-72h
    # (GoldCampaignD1AnchEngine.hpp header). 14d default here fail-closed EVERY deploy for
    # weeks (2026-07-13 111840/112337/205120 aborts). KEEP IN SYNC with seed_refresh.py.
    (re.compile(r"warmup_XAUUSD_M1\.csv", re.I),                   45),
    (re.compile(r"tsmom_warmup_H1|regime|_H1\.csv|_h1\.csv", re.I), 5),   # H1 trend/regime seeds: tight
    (re.compile(r"_D1\.csv|daily|seasonal|warmup_.*_D1", re.I),    45),   # D1 seasonal/turtle: loose
    (re.compile(r"_H4\.csv|_h4\.csv|H4", re.I),                    14),
]
# NOT real read-at-boot seeds -- engine OUTPUT sinks + printf-template scan artifacts.
# (e.g. logs/shadow/tsmom_v2.csv is where tsmom_v2 WRITES trades; warmup_%s_D1.csv is a format string.)
EXCLUDE = re.compile(r"logs[/\\]shadow[/\\]|tsmom_v2|[%<>*]|\.\.\.", re.I)
# Seeds owned by DISABLED engines -- a stale seed they never read is harmless. Reported as
# [skipped-disabled], NOT counted as stale, does NOT fail the audit. KEEP IN SYNC with engine_init.hpp:
#   FX shadow book disabled 2026-06-23 ("we have no FX") -> FX warmups inert, EXCEPT
#   GBPUSD_H1 (carved out S-2026-07-14): FxLadder GBPUSD re-enabled S-2026-07-09c boot-seeds
#   warmup_GBPUSD_H1.csv -- this regex silently hid its staleness for 5 days. Other GBPUSD
#   TFs (H4/D1) stay skipped (disabled FX shadow book only).
#   SPXUSD (2026-07-02, rationale updated S-2026-07-14): warmup_SPXUSD_H4.csv's SOLE consumer
#   is SurvivorPortfolio's SpxOverlay. Survivor itself was RE-ENABLED S-2026-07-08c, but the
#   overlay only gates SPXVolGatedDonch-family cells and NONE are active (USDJPY_4h_SPXVG
#   tombstoned 2026-06-11) -> seed still unread-in-effect. Remove SPXUSD from this regex if
#   any SPXVolGatedDonch cell is re-added.
DISABLED = re.compile(r"(EURUSD|GBPUSD(?!_H1)|USDJPY|AUDUSD|NZDUSD|USDCAD|EURGBP|SPXUSD)", re.I)

# Dynamic-path seeds the literal ".csv"-scan can NEVER see (S-2026-07-14 audit-blindness fix).
# SurvivorPortfolio::seed_all() BUILDS each cell's path at runtime:
#     base_dir + "/warmup_" + symbol + "_" + tf_tag + ".csv"      (SurvivorPortfolio.hpp:808)
# so no string literal exists for e.g. warmup_USTEC.F_H4.csv -> it rotted 94d unseen while
# seeding 2 ACTIVE cells (USTEC_4h_RSI_N7, USTEC_4h_ZMR). Parse the active (uncommented)
# add({...}) roster and synthesize the exact same paths. KEEP IN SYNC with tools/seed_refresh.py.
SURV_TF = {14400: "H4", 3600: "H1", 1800: "M30", 900: "M15", 300: "M5"}
SURV_ADD = re.compile(r'^\s*add\(\{.*?\.symbol="([^"]+)".*?\.tf_sec=(\d+)')
def survivor_seeds(repo):
    out = set()
    hpp = os.path.join(repo, "include", "SurvivorPortfolio.hpp")
    try:
        lines = open(hpp, errors="ignore").read().splitlines()
    except Exception:
        return out
    for ln in lines:
        m = SURV_ADD.match(ln)
        if not m: continue
        tag = SURV_TF.get(int(m.group(2)))
        if tag: out.add(f"phase1/signal_discovery/warmup_{m.group(1)}_{tag}.csv")
    return out
# REQUIRED-GENERATED (2026-07-02, operator decision): on-box generated, GITIGNORED files a
# SAFETY LAYER depends on. NOT bar-series seeds (no last-bar freshness applies); required =
# exists + >=1 data row. Missing => exit 2 so the OMEGA.ps1 [2c] fail-closed gate ABORTS the
# ship (override: --allow-missing-generated, which OMEGA.ps1 [2b] passes because a reseed
# cannot fix these). This REPLACES the earlier EXCLUDE-silencing of risk_monitor_thresholds:
# absence is NOT harmless -- load_thresholds() loads 0 rows => on_fire() early-returns for
# EVERY engine => the RiskMonitor auto-demote-to-shadow surveillance layer is silently OFF.
# "Tolerates absence" (no crash) is not the same as "safe to run without". Regenerate:
#   clang++ -std=c++17 -O3 -DNDEBUG -I include backtest/calibrate_risk_thresholds.cpp \
#           -o backtest/calibrate_risk_thresholds   (MSVC: cl /O2 /std:c++17 /Iinclude ...)
#   ./backtest/calibrate_risk_thresholds            (writes data/risk_monitor_thresholds.csv)
REQUIRED_GENERATED = ["data/risk_monitor_thresholds.csv"]

# ===== SEED REGISTRY (structural NO-REFRESH-PATH gate, S-2026-07-14) =========================
# Operator mandate: "never have these seed / staleness issues again". Freshness checking alone
# can NOT deliver that -- USTEC.F_H4 and GBPUSD_H1 rotted 90+d because nothing REFRESHED them
# and (separately) nothing AUDITED them. This gate closes the first class STRUCTURALLY:
#   every ACTIVE seed must either (a) be refreshed by tools/seed_refresh.py -- derived by
#   IMPORTING its tables, never a hand-copied list -- or (b) carry an explicit entry below
#   naming WHO refreshes it instead. A seed with neither = exit 3, and the check runs in
#   scripts/mac_canary_engines.sh (--registry-only), so a new engine whose seed has no
#   refresh path cannot even be COMMITTED, let alone rot in production.
KNOWN_UNREFRESHED = {
    # basename -> who keeps it fresh instead of seed_refresh.py (or why age is by-design)
    "warmup_XAUUSD_M1.csv":   "monthly histdata manual pull; 45d override above; NO IBKR M1 recipe "
                              "by design (MGC spread scale != spot XAUUSD would misfire the slot gate)",
    "sp500_long_close.csv":   "owned by OmegaStockMoverFeed nightly VPS task; watched by "
                              "feeds_selftest.py (max 26h) -- a second refresher would fight it",
    "mgc_30m_live.csv":       "LIVE stream file appended by tools/mgc_live_bars.py (registered VPS "
                              "task); absent on Mac by design; freshness is the producer task's job",
    "mgc_15m_live.csv":       "LIVE stream file appended by tools/mgc_live_bars.py (S-2026-07-14 fine "
                              "grain for GoldDon15m); absent on Mac by design; producer task owns it",
    "mgc_10m_live.csv":       "LIVE stream file appended by tools/mgc_live_bars.py (S-2026-07-14 fine "
                              "grain for GoldDon10m); absent on Mac by design; producer task owns it",
}

def refreshed_filenames(repo):
    """Basenames seed_refresh.py refreshes nightly, derived from ITS OWN tables via import.
    Returns None if seed_refresh.py can't be loaded (callers must FAIL CLOSED on None --
    a broken refresher script is itself a seed emergency, not a skip)."""
    import importlib.util
    p = os.path.join(repo, "tools", "seed_refresh.py")
    try:
        spec = importlib.util.spec_from_file_location("_seed_refresh_tables", p)
        m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        out = set()
        for _sym, _tf, name, _keep in m._REBUILD_TARGETS: out.add(name)
        for tf in m._GOLD_TFS: out.add(f"warmup_XAUUSD_{tf}.csv")
        out |= {"warmup_XAUUSD_H4.csv", "tsmom_warmup_H1.csv",       # gold-section extras
                "mgc_h1_hist.csv", "mgc_30m_hist.csv", "mgc_h4_hist.csv",
                "mgc_15m_hist.csv", "mgc_10m_hist.csv"}              # S-2026-07-14 sub-30m DON seeds
        for sym, (_con, tfs) in m._INDEX.items():
            for tf in tfs: out.add(f"warmup_{sym}_{tf}.csv")
        for sym, tfs in m._FOREX.items():
            for tf in tfs: out.add(f"warmup_{sym}_{tf}.csv")
        for _src, dst in m._ALIASES: out.add(dst)
        return out
    except Exception as e:
        print(f"[P1] cannot derive refresh set from {p}: {e}")
        return None

ALLOW_MISSING_GENERATED = False
REGISTRY_ONLY = False
IS_VPS = os.path.isdir(r"C:\Omega\logs")   # on Mac, nightly-refreshed seeds lag as git snapshots by design

def threshold_for(path):
    for rx, d in OVERRIDES:
        if rx.search(path): return d
    return MAX_AGE_DAYS

def last_ts(path):
    """last data row's epoch (sec). handles sec or ms; first column = ts."""
    try:
        with open(path, "rb") as fh:
            fh.seek(0, os.SEEK_END); size = fh.tell()
            # 64KB: wide close CSVs (500+ cols) run ~10KB/row, so 4KB was < one row ->
            # the window held one mid-row fragment and col0 was a stray price -> epoch~0.
            back = min(size, 65536); fh.seek(size - back); tail = fh.read().decode("latin-1")
        lines = tail.splitlines()
        # dropped: partial leading row fragment when not reading from byte 0
        if size > back and len(lines) > 1: lines = lines[1:]
        for line in reversed(lines):
            a = line.split(",")
            if not a or not a[0]: continue
            c0 = a[0].strip()
            try:                                 # numeric epoch first column (tick/bar CSVs)
                ts = float(c0)
                if ts <= 0: continue
                if ts > 1e11: ts /= 1000.0       # ms -> sec
                return ts
            except ValueError:
                pass
            m = re.match(r"^(\d{4})-(\d{2})-(\d{2})", c0)   # ISO date-index (wide close CSVs)
            if m:
                try:
                    return datetime.datetime(int(m.group(1)), int(m.group(2)), int(m.group(3)),
                                             tzinfo=datetime.timezone.utc).timestamp()
                except ValueError:
                    continue
    except Exception: return None
    return None

def resolve(rel):
    for cand in (os.path.join(REPO, rel), rel):
        if os.path.exists(cand): return cand
    # Concatenation fragment: code does seed_from_csv(base_dir + "/warmup_X.csv"), so the
    # capture is the leading-slash literal "/warmup_X.csv". os.path.join(REPO, "/...") drops
    # REPO (2nd arg absolute) -> false MISSING. Fall back to basename under the seed dirs.
    base = os.path.basename(rel)
    for d in ("phase1/signal_discovery", "data"):
        hits = glob.glob(os.path.join(REPO, d, base))
        if hits: return hits[0]
    return None

def main():
    global REPO, MAX_AGE_DAYS, ALLOW_MISSING_GENERATED, REGISTRY_ONLY
    for i,a in enumerate(sys.argv):
        if a == "--repo" and i+1 < len(sys.argv): REPO = sys.argv[i+1]
        if a == "--max-age-days" and i+1 < len(sys.argv): MAX_AGE_DAYS = int(sys.argv[i+1])
        if a == "--allow-missing-generated": ALLOW_MISSING_GENERATED = True
        if a == "--registry-only": REGISTRY_ONLY = True   # structural checks only (mac canary: deterministic, no time-rot)

    # 1. extract active seed paths from the code: seed_from_*("...csv") / warmup="...csv"
    seed_paths = set()
    pat = re.compile(r'(?:seed_from[a-z0-9_]*\(\s*|warmup[a-z0-9_]*\s*=\s*|"\s*)"?([A-Za-z0-9_./\\-]+\.csv)"')
    src = glob.glob(os.path.join(REPO, "include", "*.hpp")) + glob.glob(os.path.join(REPO, "include", "*.cpp"))
    for f in src:
        try: txt = open(f, errors="ignore").read()
        except Exception: continue
        for m in re.finditer(r'\.csv', txt): pass
        for m in re.finditer(r'"([^"]+\.csv)"', txt):
            p = m.group(1)
            if "signal_discovery" in p or "warmup" in p.lower() or "tsmom" in p.lower() or "/data/" in p or p.startswith("data/"):
                if EXCLUDE.search(p): continue          # drop output sinks + template artifacts
                seed_paths.add(p)
    # Dynamic Survivor paths -- invisible to the literal scan. PARSER-BLINDNESS GUARD
    # (S-2026-07-14, latent-class sweep item 8c): if SurvivorPortfolio.hpp still exists but the
    # SURV_ADD regex parses ZERO active add() entries, the roster format has drifted out from
    # under the parser and the audit is BLIND to every dynamic Survivor seed -- the exact failure
    # class this parser was built to fix (warmup_USTEC.F_H4 rotted 94d unseen). Fail LOUDLY
    # (exit 3, structural) instead of silently passing with zero coverage.
    _surv_paths = survivor_seeds(REPO)
    _surv_hpp = os.path.join(REPO, "include", "SurvivorPortfolio.hpp")
    if not _surv_paths and os.path.exists(_surv_hpp):
        print(f"[P1-PARSER-BLIND] SURV_ADD parser matched ZERO active add() entries in {_surv_hpp}.")
        print( "                  Either the add({...}) roster format changed (fix the SURV_ADD regex in")
        print( "                  tools/seed_freshness_audit.py) or the roster was intentionally emptied")
        print( "                  (then update this guard). Until fixed the")
        print( "                  audit is blind to ALL dynamic Survivor seeds -- refusing to pass silently.")
        sys.exit(3)
    seed_paths |= _surv_paths

    now = time.time()
    stale, ok, missing, skipped, snapshot_lag = [], [], [], [], []

    # ---- STRUCTURAL registry gate (runs FIRST; exit 3 beats every freshness verdict) ----
    refreshed = refreshed_filenames(REPO)
    no_refresh_path, orphan_refresh = [], []
    if refreshed is None:
        no_refresh_path.append(("<tools/seed_refresh.py unloadable>", "refresh set underivable -- fail closed"))
    else:
        active_basenames = set()
        for rel in seed_paths:
            r = rel.replace("\\", "/")
            if r in REQUIRED_GENERATED or DISABLED.search(r): continue
            base = os.path.basename(r)
            active_basenames.add(base)
            if base not in refreshed and base not in KNOWN_UNREFRESHED:
                no_refresh_path.append((rel, "no seed_refresh.py entry and no KNOWN_UNREFRESHED owner"))
        # refreshed-but-consumed-by-nobody: waste/config-rot signal, warn only (mirror files like
        # GER40_D1_idx are alias targets whose consumer names the alias, so check aliases too)
        for base in sorted(refreshed - active_basenames):
            if base not in {os.path.basename(p) for p in seed_paths}:
                orphan_refresh.append(base)

    if not REGISTRY_ONLY:
        for rel in sorted(seed_paths):
            if rel.replace("\\", "/") in REQUIRED_GENERATED:  # not a bar-series seed; checked separately below
                continue
            cand = resolve(rel)
            if cand is None: missing.append(rel); continue
            ts = last_ts(cand)
            if ts is None: missing.append(rel); continue
            age_d = (now - ts) / 86400.0
            thr = threshold_for(rel)
            rec = (rel, age_d, thr, datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y-%m-%d"))
            if age_d > thr and DISABLED.search(rel):        # stale but owned by a disabled engine -> harmless
                skipped.append(rec); continue
            # Mac working copies of NIGHTLY-VPS-REFRESHED seeds lag as git snapshots by design --
            # that lag is bounded (any cold deploy is followed by the next 23:30 refresh) and used
            # to bury the REAL signals under ~20 false "STALE" lines. Only the VPS copy's age is
            # load-bearing. Seeds with NO refresh path stay hard-stale everywhere.
            if age_d > thr and not IS_VPS and refreshed is not None and os.path.basename(rel) in refreshed:
                snapshot_lag.append(rec); continue
            (stale if age_d > thr else ok).append(rec)

    # REQUIRED-GENERATED check: exists + at least one data row beyond the header.
    req_missing = []
    if not REGISTRY_ONLY:
        for rel in REQUIRED_GENERATED:
            p = os.path.join(REPO, rel)
            rows = 0
            try:
                rows = sum(1 for ln in open(p, errors="ignore") if ln.strip())
            except Exception:
                pass
            if rows < 2: req_missing.append((rel, rows))

    print("===== SEED-FRESHNESS AUDIT =====" if not REGISTRY_ONLY else "===== SEED-REGISTRY STRUCTURAL AUDIT (--registry-only) =====")
    if REGISTRY_ONLY:
        print(f"active seed CSVs found: {len(seed_paths)}  refreshed-nightly: {len(refreshed or [])}  allowlisted-unrefreshed: {len(KNOWN_UNREFRESHED)}  NO-REFRESH-PATH: {len(no_refresh_path)}\n")
    else:
        print(f"active seed CSVs found: {len(seed_paths)}  ok: {len(ok)}  STALE(enabled): {len(stale)}  snapshot-lag(Mac,VPS-refreshed): {len(snapshot_lag)}  skipped(disabled-engine): {len(skipped)}  missing/unreadable: {len(missing)}  missing-REQUIRED(generated): {len(req_missing)}  NO-REFRESH-PATH: {len(no_refresh_path)}\n")
    for rel, why in no_refresh_path:
        print(f"  [NO-REFRESH-PATH] {rel}  -- {why}.")
        print( "                    Every active seed MUST be refreshed by tools/seed_refresh.py or carry a")
        print( "                    KNOWN_UNREFRESHED entry naming its owner. This is how USTEC.F_H4/GBPUSD_H1")
        print( "                    rotted 90+d unseen. Add the refresh recipe (preferred) or the entry.")
    for base in orphan_refresh:
        print(f"  [orphan-refresh] {base}  -- refreshed nightly but NO active engine reads it (waste / roster drift; consider dropping the recipe)")
    for rel, age, thr, d in sorted(stale, key=lambda r:-r[1]):
        print(f"  [SEED-STALE] {rel}  last bar {d}  = {age:.0f}d old (> {thr}d)  -> ENGINE BOOTS BLIND, gate may misfire")
    for rel, age, thr, d in sorted(snapshot_lag, key=lambda r:-r[1]):
        print(f"  [snapshot-lag] {rel}  {age:.0f}d (> {thr}d) -- Mac git snapshot of a nightly-VPS-refreshed seed; VPS copy is the load-bearing one. Re-collate: scp from omega-new + commit.")
    for rel, age, thr, d in sorted(skipped, key=lambda r:-r[1]):
        print(f"  [skipped-disabled] {rel}  {age:.0f}d old -- engine disabled, seed unread, harmless")
    for rel in missing:
        print(f"  [SEED-MISSING] {rel}  (referenced in code, not found / unreadable)")
    for rel, rows in req_missing:
        print(f"  [P1-MISSING-REQUIRED] {rel}  (generated file, {rows} row(s) on disk) -- RiskMonitor surveillance layer is OFF:")
        print( "                        load_thresholds()=0 rows -> on_fire() early-returns for EVERY engine ->")
        print( "                        no auto-demote-to-shadow protection. Regenerate (see header of this script")
        print( "                        or backtest/calibrate_risk_thresholds.cpp), then re-run this audit.")
    for rel, age, thr, d in sorted(ok, key=lambda r:-r[1])[:8]:
        print(f"  [ok] {rel}  {age:.0f}d (<= {thr}d)")
    if len(ok) > 8: print(f"  ... +{len(ok)-8} more fresh")

    if no_refresh_path:
        print(f"\n*** {len(no_refresh_path)} ACTIVE SEED(S) WITH NO REFRESH PATH -- structurally guaranteed to rot. FIX (recipe or allowlist entry). ***")
        sys.exit(3)
    if REGISTRY_ONLY:
        print(f"\nSeed registry structurally sound: every active seed has a refresh path or a named owner. ({len(orphan_refresh)} orphan-refresh warning(s).)")
        sys.exit(0)
    if stale:
        print(f"\n*** {len(stale)} STALE SEED(S) on ENABLED engines -- a gate is operating on a price view detached from reality. FIX. ***")
        sys.exit(1)
    if req_missing and not ALLOW_MISSING_GENERATED:
        print(f"\n*** {len(req_missing)} REQUIRED generated file(s) MISSING -- a safety layer is silently disarmed. FIX. ***")
        sys.exit(2)
    if req_missing:
        print(f"\n[override] {len(req_missing)} required generated file(s) missing but --allow-missing-generated set.")
    print(f"\nAll enabled-engine seeds fresh. ({len(skipped)} disabled-engine seed(s) ignored; {len(snapshot_lag)} Mac snapshot-lag warning(s).)")
    sys.exit(0)

if __name__ == "__main__":
    main()
