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
ALLOW_MISSING_GENERATED = False
for i,a in enumerate(sys.argv):
    if a == "--repo" and i+1 < len(sys.argv): REPO = sys.argv[i+1]
    if a == "--max-age-days" and i+1 < len(sys.argv): MAX_AGE_DAYS = int(sys.argv[i+1])
    if a == "--allow-missing-generated": ALLOW_MISSING_GENERATED = True

def threshold_for(path):
    for rx, d in OVERRIDES:
        if rx.search(path): return d
    return MAX_AGE_DAYS

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
seed_paths |= survivor_seeds(REPO)   # dynamic Survivor paths -- invisible to the literal scan

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

now = time.time()
stale, ok, missing, skipped = [], [], [], []
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
    (stale if age_d > thr else ok).append(rec)

# REQUIRED-GENERATED check: exists + at least one data row beyond the header.
req_missing = []
for rel in REQUIRED_GENERATED:
    p = os.path.join(REPO, rel)
    rows = 0
    try:
        rows = sum(1 for ln in open(p, errors="ignore") if ln.strip())
    except Exception:
        pass
    if rows < 2: req_missing.append((rel, rows))

print("===== SEED-FRESHNESS AUDIT =====")
print(f"active seed CSVs found: {len(seed_paths)}  ok: {len(ok)}  STALE(enabled): {len(stale)}  skipped(disabled-engine): {len(skipped)}  missing/unreadable: {len(missing)}  missing-REQUIRED(generated): {len(req_missing)}\n")
for rel, age, thr, d in sorted(stale, key=lambda r:-r[1]):
    print(f"  [SEED-STALE] {rel}  last bar {d}  = {age:.0f}d old (> {thr}d)  -> ENGINE BOOTS BLIND, gate may misfire")
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

if stale:
    print(f"\n*** {len(stale)} STALE SEED(S) on ENABLED engines -- a gate is operating on a price view detached from reality. FIX. ***")
    sys.exit(1)
if req_missing and not ALLOW_MISSING_GENERATED:
    print(f"\n*** {len(req_missing)} REQUIRED generated file(s) MISSING -- a safety layer is silently disarmed. FIX. ***")
    sys.exit(2)
if req_missing:
    print(f"\n[override] {len(req_missing)} required generated file(s) missing but --allow-missing-generated set.")
print(f"\nAll enabled-engine seeds fresh. ({len(skipped)} disabled-engine seed(s) ignored.)")
sys.exit(0)
