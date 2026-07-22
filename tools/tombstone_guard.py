#!/usr/bin/env python3
# tombstone_guard.py (S-2026-06-24L) -- the ENFORCED anti-resurrection gate.
#
# WHY: tombstoned engines kept coming back. GoldOrbRetrace was un-tombstoned THREE
# times -- each time a fresh "EDGE" row in AUDITED_CONFIGS silenced engine_registry's
# DEAD/UNAUDITED check, and a live-falsified engine traded again. cull_audit.py logs
# culls but never blocks (exit 0 always). Nothing made a tombstone STICK.
#
# WHAT: read backtest/TOMBSTONES.tsv (the hard blocklist). Scan engine_init.hpp for
# every `g_X.enabled = true`. If any tombstoned global is enabled -> FAIL (exit 1),
# LOUD, with the tombstone reason + date + basis. This OVERRIDES AUDITED_CONFIGS:
# adding a fresh EDGE row CANNOT resurrect a FORBIDDEN engine. The only unblock path
# is to edit TOMBSTONES.tsv (a deliberate, reviewed, git-logged act).
#
# Also cross-checks: any AUDITED_CONFIGS verdict=DEAD engine that has a g_ enable line
# in engine_init but is NOT covered by TOMBSTONES.tsv -> WARN (keep the list complete).
#
# Wired into scripts/mac_canary_engines.sh so a resurrection can't be committed.
# Run: python3 tools/tombstone_guard.py [--repo <dir>]
import os, re, sys

REPO = "."
for i, a in enumerate(sys.argv):
    if a == "--repo" and i + 1 < len(sys.argv):
        REPO = sys.argv[i + 1]

def rd(p):
    try:
        return open(os.path.join(REPO, p), errors="ignore").read()
    except Exception:
        return ""

# 1. tombstone blocklist
TOMB = {}  # global -> dict(engine, date, basis, policy, reason)
for ln in rd("backtest/TOMBSTONES.tsv").splitlines():
    if ln.startswith("#") or not ln.strip():
        continue
    c = ln.split("\t")
    if len(c) >= 6 and c[0].startswith("g_"):
        TOMB[c[0].strip()] = dict(engine=c[1].strip(), date=c[2].strip(),
                                  basis=c[3].strip(), policy=c[4].strip(),
                                  reason=c[5].strip())

# 2. enabled globals in engine_init.hpp + omega_main.hpp
enabled = set()
for src in ("include/engine_init.hpp", "include/omega_main.hpp"):
    for m in re.finditer(r'(g_[A-Za-z0-9_]+)\.enabled\s*=\s*true', rd(src)):
        enabled.add(m.group(1))

# 3. violations = tombstoned globals that are enabled
violations = [(g, TOMB[g]) for g in sorted(enabled) if g in TOMB]

# 4. completeness cross-check: AUDITED_CONFIGS DEAD rows whose engine has a g_ enable
#    line anywhere but isn't in TOMBSTONES -> warn (the blocklist is missing one).
def norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())
tomb_norm = {norm(v["engine"]) for v in TOMB.values()} | {norm(g[2:]) for g in TOMB}
dead_uncovered = []
for ln in rd("backtest/AUDITED_CONFIGS.tsv").splitlines():
    if ln.startswith("#") or not ln.strip():
        continue
    c = ln.split("\t")
    if len(c) >= 5 and c[4].strip() == "DEAD":
        if norm(c[0]) not in tomb_norm:
            dead_uncovered.append(c[0].strip())

print(f"[tombstone-guard] {len(TOMB)} tombstoned globals | {len(enabled)} enabled | "
      f"{len(violations)} RESURRECTION(s)")

if violations:
    print("\n  *** TOMBSTONE RESURRECTION BLOCKED ***")
    for g, t in violations:
        print(f"  [{t['policy']}] {g} ({t['engine']}) -- tombstoned {t['date']} ({t['basis']})")
        print(f"      {t['reason']}")
        if t["policy"] == "FORBIDDEN":
            print("      -> dead at source / live-falsified. Do NOT re-enable. To revisit: remove")
            print("         its row in backtest/TOMBSTONES.tsv (logged) + attach a FRESH faithful-TICK PF.")
        else:
            print("      -> re-enable needs a faithful-tick BT row in AUDITED_CONFIGS + downgrade")
            print("         its TOMBSTONES.tsv row in the SAME commit.")
    print("\n  A tombstone overrides any AUDITED_CONFIGS row -- a fresh EDGE row does NOT unblock it.")

if dead_uncovered:
    # Operator hard rule (2026-07-22): dead-engine NAMES are never printed in routine
    # output — counts only. Names available only behind an explicit opt-in flag.
    if "--list-dead" in sys.argv:
        print(f"\n  [warn] AUDITED_CONFIGS verdict=DEAD not in TOMBSTONES.tsv (add if it has a live global): "
              + ", ".join(dead_uncovered))
    else:
        print(f"\n  [warn] {len(dead_uncovered)} AUDITED_CONFIGS verdict=DEAD row(s) not in TOMBSTONES.tsv "
              f"(bookkeeping only; names suppressed -- rerun with --list-dead if acting on it)")

if not violations:
    print("  CLEAN -- no tombstoned engine is enabled.")

sys.exit(1 if violations else 0)
