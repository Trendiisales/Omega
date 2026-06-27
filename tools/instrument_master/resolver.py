#!/usr/bin/env python3
# resolver.py — Omega Instrument Master resolver (Build 1 from the 'Being a Quant' video).
# Maps ANY broker/vendor/data alias -> ONE canonical instrument + its specs/drivers/route.
# Fixes the recurring "NSXUSD=NASDAQ / no data" failure: resolve() BEFORE concluding a feed is dead.
#
# Usage:
#   from resolver import resolve, canonical, aliases_of
#   resolve("NSXUSD")     -> {"canonical":"NAS100", ...full spec...}
#   canonical("US100.f")  -> "NAS100"
#   resolve("XAU/USD")["futures_proxy"] -> "MGC"
#   python resolver.py NSXUSD            # CLI lookup
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
_REG = json.load(open(os.path.join(HERE, "instruments.json")))["instruments"]

# build a lowercase alias -> canonical index once
_IDX = {}
for canon, spec in _REG.items():
    _IDX[canon.lower()] = canon
    for a in spec.get("aliases", []):
        _IDX[a.lower().strip()] = canon

def canonical(symbol):
    """Return the canonical instrument key for any alias, or None if unknown."""
    if symbol is None: return None
    return _IDX.get(str(symbol).lower().strip())

def resolve(symbol):
    """Return the full spec dict (with 'canonical' added) for any alias, or None."""
    c = canonical(symbol)
    if not c: return None
    out = dict(_REG[c]); out["canonical"] = c; out["queried"] = symbol
    return out

def aliases_of(symbol):
    c = canonical(symbol)
    return _REG[c].get("aliases", []) if c else []

def driver_set(symbol):
    """Drivers/correlated instruments for the multifactor model (Build 2)."""
    c = canonical(symbol)
    return _REG[c].get("drivers", []) if c else []

if __name__ == "__main__":
    if len(sys.argv) > 1:
        for q in sys.argv[1:]:
            r = resolve(q)
            if r: print(f"{q!r:24} -> {r['canonical']:8} ({r['name']}) drivers={r.get('drivers')} route={r.get('exec_route')}")
            else: print(f"{q!r:24} -> UNKNOWN (not in instrument master — add it to instruments.json)")
    else:
        # self-test: the operator's known pain aliases must all resolve
        tests = ["NSXUSD", "US100.f", "USTEC", "NQ", "XAU/USD", "GOLD", "GC=F", "SPX500", "ES", "DAX", "ETHUSDT", "SOL (fut, SQF pending)"]
        print("=== Instrument Master self-test ===")
        ok = 0
        for t in tests:
            c = canonical(t)
            status = "OK" if c else "FAIL"
            if c: ok += 1
            print(f"  [{status}] {t!r:28} -> {c}")
        print(f"{ok}/{len(tests)} aliases resolved")
