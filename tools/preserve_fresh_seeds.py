#!/usr/bin/env python3
# PRESERVE-FRESH-SEEDS (2026-06-29) — the deploy-speed cure for the perpetual reseed.
#
# WHY: the deploy's [2/12] `git reset --hard origin/main` REVERTS the warmup CSVs to the
# COMMITTED corpus, which is a static snapshot that ages by calendar -> [2b] then ALWAYS finds
# them stale -> ALWAYS pays the ~300s IBKR reseed, every single deploy. The skip never fires.
# Meanwhile the daily OmegaSeedRefresh task keeps the WORKING-TREE CSVs fresh -- but the reset
# throws that freshness away. This tool runs RIGHT AFTER the reset: for each seed that was
# backed up (pre-reset working tree), if the backup's LAST BAR is newer than the just-restored
# committed file, copy the backup back. Result: a freshly-refreshed working tree survives the
# reset -> [2b] audit clean -> SKIP the 300s reseed. A genuinely newer COMMITTED seed (a real
# seed update pushed to main) is left untouched (committed wins when it is the newer one).
#
# SAFETY: this only ever makes seeds FRESHER, never staler. The [2c] fail-closed gate re-audits
# after this + [2b] and ABORTS the deploy if anything is still stale -- so a bug here can never
# ship a blind binary, only (worst case) fall back to a reseed. Non-fatal: always exits 0.
#
# Usage:  python3 tools/preserve_fresh_seeds.py --repo <dir> --backup <backup_dir>
#   <backup_dir> mirrors repo structure (e.g. <backup>/phase1/signal_discovery/*.csv,
#   <backup>/data/mgc_*.csv) -- created by OMEGA.ps1 [2/12] before the git reset.
import os, sys, shutil

REPO = "."
BACKUP = None
args = sys.argv
for i, a in enumerate(args):
    if a == "--repo" and i + 1 < len(args): REPO = args[i + 1]
    if a == "--backup" and i + 1 < len(args): BACKUP = args[i + 1]

if not BACKUP or not os.path.isdir(BACKUP):
    print(f"[preserve] no backup dir ('{BACKUP}') -- nothing to preserve."); sys.exit(0)


def last_ts(path):
    """last data row's epoch (sec). handles sec or ms; first column = ts. (matches seed_freshness_audit)"""
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
    except Exception:
        return None
    return None


restored, kept, skipped = [], [], []
for root, _dirs, files in os.walk(BACKUP):
    for fn in files:
        if not fn.lower().endswith(".csv"): continue
        bpath = os.path.join(root, fn)
        rel = os.path.relpath(bpath, BACKUP)
        cpath = os.path.join(REPO, rel)
        b_ts = last_ts(bpath)
        if b_ts is None:
            skipped.append(rel); continue
        c_ts = last_ts(cpath) if os.path.exists(cpath) else None
        # restore backup when it is strictly newer than the committed file (or committed missing)
        if c_ts is None or b_ts > c_ts + 1.0:
            try:
                os.makedirs(os.path.dirname(cpath), exist_ok=True)
                shutil.copyfile(bpath, cpath)
                days = (b_ts - c_ts) / 86400.0 if c_ts else None
                restored.append((rel, days))
            except Exception as e:
                skipped.append(f"{rel} (copy {e})")
        else:
            kept.append(rel)

print("===== PRESERVE-FRESH-SEEDS =====")
print(f"restored(working-tree newer): {len(restored)}  kept(committed newer/equal): {len(kept)}  skipped: {len(skipped)}")
for rel, days in restored:
    extra = f"  (+{days:.0f}d fresher)" if days is not None else "  (committed missing)"
    print(f"  [restore] {rel}{extra}")
for s in skipped:
    print(f"  [skip] {s}")
sys.exit(0)
