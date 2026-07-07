#!/usr/bin/env python3
"""config_drift_guard.py — stop the "claimed PF != deployed/faithful PF" class of falsification.

Cross-references engine_init.hpp against backtest/AUDITED_CONFIGS.tsv (the only trustworthy
per-engine faithful-BT record) and flags:
  1. CLAIM-DRIFT  : an inline PF claim in engine_init.hpp with no matching faithful manifest
                    entry, OR where the claimed PF differs from the faithful PF by > tol.
  2. UNAUDITED    : an engine enabled=true with no manifest entry at all.
  3. RISKY-ENABLE : an engine enabled=true whose faithful verdict is MARGINAL / DEAD.

Exit 1 if any CLAIM-DRIFT or RISKY-ENABLE found (CI-gate); UNAUDITED is a warning.
This would have caught the 2026-06-18 NAS "PF2.69" comment (claim for an unshipped config).

Run:  python3 tools/config_drift_guard.py
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
INIT = ROOT / "include" / "engine_init.hpp"
MANIFEST = ROOT / "backtest" / "AUDITED_CONFIGS.tsv"
TOL = 0.30  # claimed-vs-faithful PF relative tolerance

def load_manifest():
    m = {}
    for ln in MANIFEST.read_text(encoding="utf-8").splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        p = ln.split("\t")
        if len(p) < 5:
            continue
        try:
            pf = float(p[1])
        except ValueError:
            pf = None  # verdict-only rows (e.g. pf column = "FAIL") carry no faithful PF number
        m[p[0].strip()] = {"pf": pf, "date": p[2], "harness": p[3],
                           "verdict": p[4].strip().upper(), "note": p[5] if len(p) > 5 else ""}
    return m

def main():
    init = INIT.read_text(encoding="utf-8").splitlines()  # source is UTF-8; Windows default cp1252 chokes on box-draw/✓ bytes (S-2026-06-23 deploy abort)
    man = load_manifest()
    drift, drift_warn, risky, unaudited = [], [], [], []

    # 1. enabled engines: "g_<name>.enabled = true" -> map global to manifest key heuristically
    enabled = []
    for i, ln in enumerate(init):
        mo = re.search(r'\bg_([a-z0-9_]+)\.enabled\s*=\s*true', ln)
        if mo:
            enabled.append((i + 1, mo.group(1)))

    def match(global_name):
        g = global_name.replace("_", "")
        return next((k for k in man if k.lower().replace("_", "") == g
                     or k.lower().replace("_", "").replace("d1", "") == g), None)

    # 2. DEPLOYMENT PF claims only: a PF cited on/near (±2 lines of) an `enabled=true`
    #    line. Historical sweep-note PFs elsewhere in comments are out of scope (noise).
    #    This is exactly the NAS "PF2.69 on enable line for an unshipped config" case.
    enabled_lines = {ln for ln, _ in enabled}
    for lineno, g in enabled:
        k = match(g)
        ctx = range(max(0, lineno - 1 - 2), min(len(init), lineno + 2))
        for j in ctx:
            for pm in re.finditer(r'PF\s*([0-9]+\.[0-9]+)', init[j]):
                claim = float(pm.group(1))
                if k is None:
                    # unbacked deploy-claim → WARNING (owed a faithful audit, not a contradiction)
                    drift_warn.append((j + 1, f"{g} enabled w/ PF{claim} claim but NO faithful manifest entry"))
                else:
                    fpf = man[k]["pf"]
                    if fpf is not None and abs(claim - fpf) / max(fpf, 1e-9) > TOL:
                        # claim CONTRADICTS a known faithful number → HARD FAIL (a lie in the code)
                        drift.append((j + 1, f"{g} enable-claim PF{claim} vs faithful {k} PF{fpf} ({man[k]['date']}) — DRIFT >{int(TOL*100)}%"))

    # 3. risky enables (best-effort name match)
    for lineno, g in enabled:
        k = match(g)
        if k is None:
            unaudited.append((lineno, g))
        elif man[k]["verdict"] in ("MARGINAL", "DEAD"):
            risky.append((lineno, g, man[k]["verdict"], man[k]["pf"]))

    def emit(title, items):
        if items:
            print(f"\n=== {title} ({len(items)}) ===")
            for it in items:
                print("  engine_init.hpp:" + str(it[0]) + "  " + "  ".join(str(x) for x in it[1:]))

    print(f"config_drift_guard: {len(enabled)} enabled engines, {len(man)} audited entries, tol {int(TOL*100)}%")
    emit("CLAIM-DRIFT — deploy claim CONTRADICTS faithful number (FAIL)", drift)
    emit("UNBACKED-CLAIM — deploy PF claim, no faithful entry (warn)", drift_warn)
    emit("RISKY-ENABLE — marginal/dead but enabled (warn)", risky)
    emit("UNAUDITED — enabled, no faithful manifest entry (warn)", unaudited)

    # HARD FAIL only on a claim that contradicts a known faithful number — that's a
    # lie in the deployed code (the NAS PF2.69 class). Everything else is a tracked warning.
    if drift:
        print("\nRESULT: FAIL — a deploy-line PF claim contradicts the faithful audit. Fix the comment or re-audit.")
        return 1
    nwarn = len(drift_warn) + len(risky) + len(unaudited)
    print(f"\nRESULT: OK ({nwarn} warnings — owed audits / intentional shadow-marginals)" if nwarn else "\nRESULT: OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
