#!/usr/bin/env python3
# CRYPTO-DESK COMPANION-STATE PILOT-SYM GATE (S-2026-07-19) — structural, self-healing.
#
# OPERATOR RULE (hard): ZERO SHADOW on the desk (project-crypto-shadow-ledger-isolation:
# "operator wants zero shadow 2026-07-19").
#
# crypto_companion_state.json is the josgp1 companion mirror the desk reads via
# GET /api/crypto_companion -> window._cc. Its legs[] array holds EVERY coin the SHADOW
# book clips (all coins), but LIVE cash is gated to the pilot syms only. The desk JS loops
# every leg and rings a toast ("companion open: crypto AAVE ...", "companion clip banked:
# crypto AAVE +N clip(s)") + shows a MIMIC BOOKS row for each — so every non-pilot leg is a
# SHADOW row on the operator's desk. Drop every non-pilot-sym leg here, in the 120s relay
# (refresh_crypto_companion.sh HOP2) BEFORE the push to omega-new, so the desk shows only the
# live pilot regardless of what the append-only source keeps emitting. Self-healing: even if
# josgp1 re-arms AAVE next bar, the next relay drops it again.
#
# live_mirror (the REAL LTC Binance cash mirror) + ts are preserved untouched. Every drop is
# LOGGED (not silent) so the shadow record stays auditable. Override the pilot set with
# CRYPTO_DESK_PILOT_SYMS="LTC,ETH,..." if the pilot widens.
#
# Twin of filter_chimera_inbound.py (last-15 CSV panel); this covers the companion-state
# toasts + MIMIC BOOKS panel. Both gate the same desk to the same pilot set.
import json
import os
import sys

PILOT_SYMS = {
    s.strip().upper()
    for s in os.environ.get("CRYPTO_DESK_PILOT_SYMS", "LTC,LTCUSDT").split(",")
    if s.strip()
}


def main() -> int:
    if len(sys.argv) < 2:
        return 0
    path = sys.argv[1]
    try:
        with open(path) as fh:
            d = json.load(fh)
    except (OSError, ValueError):
        return 0  # unreadable/invalid -> pass through untouched (fail-open, never blank the desk blind)

    legs = d.get("legs")
    if not isinstance(legs, list):
        return 0  # unknown schema -> leave alone

    kept, dropped = [], []
    for leg in legs:
        sym = str((leg or {}).get("sym", "")).strip().upper()
        if sym in PILOT_SYMS:
            kept.append(leg)
        else:
            dropped.append((sym or "?", (leg or {}).get("cell") or (leg or {}).get("tag") or "?"))

    if dropped:
        d["legs"] = kept
        with open(path, "w") as out:
            json.dump(d, out)
        for sym, cell in dropped:
            print(f"[COMPANION-STATE-GUARD] DROPPED SHADOW leg (non-pilot sym {sym}): {cell} "
                  f"(operator rule: zero shadow on the desk; pilot={sorted(PILOT_SYMS)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
