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
    (re.compile(r"tsmom_warmup_H1|regime|_H1\.csv|_h1\.csv", re.I), 5),   # H1 trend/regime seeds: tight
    (re.compile(r"_D1\.csv|daily|seasonal|warmup_.*_D1", re.I),    45),   # D1 seasonal/turtle: loose
    (re.compile(r"_H4\.csv|_h4\.csv|H4", re.I),                    14),
]
# NOT real read-at-boot seeds -- engine OUTPUT sinks + printf-template scan artifacts.
# (e.g. logs/shadow/tsmom_v2.csv is where tsmom_v2 WRITES trades; warmup_%s_D1.csv is a format string.)
EXCLUDE = re.compile(r"logs[/\\]shadow[/\\]|tsmom_v2|[%<>*]|\.\.\.", re.I)
# Seeds owned by DISABLED engines -- a stale seed they never read is harmless. Reported as
# [skipped-disabled], NOT counted as stale, does NOT fail the audit. KEEP IN SYNC with engine_init.hpp:
#   FX shadow book disabled 2026-06-23 ("we have no FX") -> all FX warmups inert.
DISABLED = re.compile(r"(EURUSD|GBPUSD|USDJPY|AUDUSD|NZDUSD|USDCAD|EURGBP)", re.I)
for i,a in enumerate(sys.argv):
    if a == "--repo" and i+1 < len(sys.argv): REPO = sys.argv[i+1]
    if a == "--max-age-days" and i+1 < len(sys.argv): MAX_AGE_DAYS = int(sys.argv[i+1])

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

def last_ts(path):
    """last data row's epoch (sec). handles sec or ms; first column = ts."""
    try:
        with open(path, "rb") as fh:
            fh.seek(0, os.SEEK_END); size = fh.tell()
            back = min(size, 4096); fh.seek(size - back); tail = fh.read().decode("latin-1")
        for line in reversed(tail.splitlines()):
            a = line.split(",")
            if not a or not a[0]: continue
            try: ts = float(a[0])
            except: continue
            if ts <= 0: continue
            if ts > 1e11: ts /= 1000.0       # ms -> sec
            return ts
    except Exception: return None
    return None

now = time.time()
stale, ok, missing, skipped = [], [], [], []
for rel in sorted(seed_paths):
    cand = os.path.join(REPO, rel)
    if not os.path.exists(cand): cand = rel
    if not os.path.exists(cand): missing.append(rel); continue
    ts = last_ts(cand)
    if ts is None: missing.append(rel); continue
    age_d = (now - ts) / 86400.0
    thr = threshold_for(rel)
    rec = (rel, age_d, thr, datetime.datetime.utcfromtimestamp(ts).strftime("%Y-%m-%d"))
    if age_d > thr and DISABLED.search(rel):        # stale but owned by a disabled engine -> harmless
        skipped.append(rec); continue
    (stale if age_d > thr else ok).append(rec)

print("===== SEED-FRESHNESS AUDIT =====")
print(f"active seed CSVs found: {len(seed_paths)}  ok: {len(ok)}  STALE(enabled): {len(stale)}  skipped(disabled-engine): {len(skipped)}  missing/unreadable: {len(missing)}\n")
for rel, age, thr, d in sorted(stale, key=lambda r:-r[1]):
    print(f"  [SEED-STALE] {rel}  last bar {d}  = {age:.0f}d old (> {thr}d)  -> ENGINE BOOTS BLIND, gate may misfire")
for rel, age, thr, d in sorted(skipped, key=lambda r:-r[1]):
    print(f"  [skipped-disabled] {rel}  {age:.0f}d old -- engine disabled (FX), seed unread, harmless")
for rel in missing:
    print(f"  [SEED-MISSING] {rel}  (referenced in code, not found / unreadable)")
for rel, age, thr, d in sorted(ok, key=lambda r:-r[1])[:8]:
    print(f"  [ok] {rel}  {age:.0f}d (<= {thr}d)")
if len(ok) > 8: print(f"  ... +{len(ok)-8} more fresh")

if stale:
    print(f"\n*** {len(stale)} STALE SEED(S) on ENABLED engines -- a gate is operating on a price view detached from reality. FIX. ***")
    sys.exit(1)
print(f"\nAll enabled-engine seeds fresh. ({len(skipped)} disabled-engine seed(s) ignored.)")
sys.exit(0)
