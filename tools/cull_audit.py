#!/usr/bin/env python3
"""cull_audit.py — standing audit of every performance cull. Surfaces which tombstoned
engines are still OWED a faithful re-check (the suspect bucket the operator distrusts).

Reads backtest/CULL_LEDGER.tsv. A cull is SUSPECT (owed=YES) when its kill-basis is
POLLUTED / BAR-REPLAY / UNVERIFIED / UNEVALUABLE — i.e. it was never confirmed dead on a
faithful tick BT or a clean live ledger. The cure: faithful re-check -> it resurrects or
is confirmed dead, and its row flips to FAITHFUL with owed=NO.

Trustworthy buckets (FAITHFUL / LEDGER / OPERATOR / RESEARCH) are reported as a count only.

Run: python3 tools/cull_audit.py
Exit 0 always (informational); prints the owed queue so it can't hide.
"""
import sys, pathlib
LED = pathlib.Path(__file__).resolve().parents[1] / "backtest" / "CULL_LEDGER.tsv"

def main():
    rows = []
    for ln in LED.read_text().splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        p = ln.split("\t")
        if len(p) < 6:
            continue
        rows.append(dict(engine=p[0], date=p[1], basis=p[2].strip(),
                         owed=p[3].strip().upper(), verdict=p[4].strip(), note=p[5]))
    owed = [r for r in rows if r["owed"] == "YES"]
    trust = [r for r in rows if r["owed"] == "NO"]
    from collections import Counter
    bc = Counter(r["basis"] for r in trust)

    print(f"cull_audit: {len(rows)} culls logged | {len(trust)} trustworthy | {len(owed)} OWED faithful re-check\n")
    print(f"  trustworthy by basis: {dict(bc)}")
    print(f"\n=== SUSPECT — owed a faithful re-check ({len(owed)}) ===")
    print(f"  {'engine':40} {'basis':12} {'culled':11} note")
    for r in sorted(owed, key=lambda x: x["basis"]):
        print(f"  {r['engine'][:39]:40} {r['basis']:12} {r['date']:11} {r['note'][:70]}")
    if owed:
        print("\n  -> faithful tick BT each (drives the real engine class). It resurrects (e.g.")
        print("     GoldOrb PF2.38) or confirms dead (e.g. Squeeze) -> flip its row to FAITHFUL.")
    else:
        print("\n  CLEAN — every cull has a faithful/ledger/operator verdict.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
