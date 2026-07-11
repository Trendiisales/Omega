#!/usr/bin/env python3
# fail_verdict_guard.py (S-2026-07-12, never-again audit) -- FAIL-verdict enforcement.
#
# WHY: the tombstone guard (tools/tombstone_guard.py) blocks TOMBSTONED engines, but on
# 2026-07-12 three engines with recorded true-cost FAIL backtests (GOLD_PHASE1B) were
# still enabled=true and live until the operator ordered a cull. A FAIL verdict carried
# no enforcement -- an engine could fail a true-cost backtest and keep trading.
#
# WHAT: read backtest/FAIL_VERDICTS.tsv (the FAIL disable-list). Scan engine_init.hpp +
# omega_main.hpp for every `g_X.enabled = true`. If any listed global is enabled -> FAIL
# (exit 1), LOUD, with the verdict ref. Re-enabling requires removing the row WITH a new
# passing backtest ref in the same commit (see the tsv header).
#
# Wired into scripts/mac_canary_engines.sh (after the tombstone guard) so a FAIL-verdict
# engine cannot be re-enabled and committed silently.
# Run: python3 tools/fail_verdict_guard.py [--repo <dir>]
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

# 1. FAIL-verdict list
FAILS = {}  # global -> dict(engine, date, ref, reason)
for ln in rd("backtest/FAIL_VERDICTS.tsv").splitlines():
    if ln.startswith("#") or not ln.strip():
        continue
    c = ln.split("\t")
    if len(c) >= 5 and c[0].startswith("g_"):
        FAILS[c[0].strip()] = dict(engine=c[1].strip(), date=c[2].strip(),
                                   ref=c[3].strip(), reason=c[4].strip())

# 2. enabled globals
enabled = set()
for src in ("include/engine_init.hpp", "include/omega_main.hpp"):
    for m in re.finditer(r'(g_[A-Za-z0-9_]+)\.enabled\s*=\s*true', rd(src)):
        enabled.add(m.group(1))

# 3. violations
violations = [(g, FAILS[g]) for g in sorted(enabled) if g in FAILS]

print(f"[fail-verdict-guard] {len(FAILS)} FAIL-verdict globals | {len(enabled)} enabled | "
      f"{len(violations)} violation(s)")

if violations:
    print("\n  *** FAIL-VERDICT ENGINE ENABLED -- BLOCKED ***")
    for g, f in violations:
        print(f"  {g} ({f['engine']}) -- true-cost FAIL {f['date']} ({f['ref']})")
        print(f"      {f['reason']}")
    print("\n  An engine with a recorded true-cost FAIL may NOT be enabled. To re-enable:")
    print("  remove its FAIL_VERDICTS.tsv row IN THE SAME COMMIT as a NEW passing faithful")
    print("  true-cost backtest ref (both WF halves + both regimes), named in the commit msg.")
else:
    print("  CLEAN -- no FAIL-verdict engine is enabled.")

sys.exit(1 if violations else 0)
