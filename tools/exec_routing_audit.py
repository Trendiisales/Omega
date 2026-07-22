#!/usr/bin/env python3
"""exec_routing_audit.py -- S-2026-07-22j structural gate (operator: "how do you
stop this from ever occurring again").

THE FAILURE CLASS THIS KILLS: S-22c flipped `enabled=true` flags and called it a
live-only conversion, but most engine CLASSES contain no order-send code -- the
"33 live" engines were structurally paper for days and nobody's gate objected.

RULE: every `<global>.enabled = true` assignment in engine_init.hpp must resolve
to a class whose header contains a REAL exec marker
    (send_live_order | set_exec | exec_open | OpenFn)
OR the global must be named in the boot [PAPER-PURGE] block (force-disabled
before it can run) OR carry an explicit allowlist entry below with a reason.
Any other enabled engine = EXIT 1 = the mac canary FAILS = it cannot be
committed. Flags without routing are now a build error, not a discovery.

Wired into scripts/mac_canary_engines.sh.
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
EI   = ROOT / "include" / "engine_init.hpp"
GL   = ROOT / "include" / "globals.hpp"
INC  = ROOT / "include"

EXEC_MARKER = re.compile(r"send_live_order|set_exec|exec_open|OpenFn")

# global -> documented exception (NOT engines: infra/gates/managers that hold the
# enabled field but open no broker positions themselves).
ALLOW = {
    "g_survivor":       "portfolio container; its CELLS are swept by the PAPER-PURGE loop",
    "g_gold_stack":     "shared regime-signal infra (LIVE-ONLY gate carve-out, certify-or-cull owed)",
    "g_regime_adaptor": "regime-adaptive weight/vol infra (omega_types.hpp decl); opens no positions",
}

def main() -> int:
    ei = EI.read_text(errors="replace")
    gl = GL.read_text(errors="replace")

    # global -> class from globals.hpp declarations
    gclass = {}
    for m in re.finditer(r"static\s+omega::(\w+)\s+(g_\w+)", gl):
        gclass[m.group(2)] = m.group(1)
    for m in re.finditer(r"static\s+(\w+)\s+(g_\w+)\s*;", gl):
        gclass.setdefault(m.group(2), m.group(1))

    # purge-covered globals (the [PAPER-PURGE] block)
    purged = set(re.findall(r"OMEGA_PP\((g_\w+)\)", ei))

    # class -> has exec marker (scan the class's header once)
    cls_ok = {}
    def class_routed(cls: str) -> bool:
        if cls in cls_ok:
            return cls_ok[cls]
        ok = False
        for h in INC.glob("*.hpp"):
            t = h.read_text(errors="replace")
            if re.search(r"\b(class|struct)\s+" + re.escape(cls) + r"\b", t):
                if EXEC_MARKER.search(t):
                    ok = True
                    break
        cls_ok[cls] = ok
        return ok

    viol = []
    enabled = sorted(set(re.findall(r"(g_\w+)\.enabled\s*=\s*true", ei)))
    checked = 0
    for g in enabled:
        checked += 1
        if g in ALLOW or g in purged:
            continue
        cls = gclass.get(g)
        if cls is None:
            viol.append(f"{g}: enabled=true but no declaration found in globals.hpp -- parser gap, fix in same commit")
            continue
        if not class_routed(cls):
            viol.append(f"{g} (class {cls}): enabled=true but the class has NO order-send path "
                        f"(no send_live_order/set_exec/OpenFn) and is NOT in the PAPER-PURGE block")

    print(f"[EXEC-ROUTING] {checked} enabled globals checked | {len(purged)} purge-covered | "
          f"{len(ALLOW)} allowlisted | {len(viol)} VIOLATION(s)")
    for v in viol:
        print(f"[EXEC-ROUTING] VIOLATION {v}")
    if viol:
        print("[EXEC-ROUTING] FAIL: a flag-only 'live' engine would boot. Wire real exec "
              "(set_exec/send_live_order in the class + call-site activation) in the SAME "
              "commit, or add it to the [PAPER-PURGE] block.")
        return 1
    print("[EXEC-ROUTING] PASS: every enabled engine has a real order path, is purge-covered, or is a documented non-engine.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
