#!/usr/bin/env python3
# livebook_pnl.py — split the trade ledger into the VALIDATED LIVE BOOK vs RESEARCH.
#
# Why: omega_trade_closes.csv dumps every engine (80+ shadow candidates) into one
# file, so the handful of validated both-regime edges are invisible under marginal
# noise + phantom fat-tails. This gives the legible view WITHOUT touching the
# production journal pipeline (GUI/retention read it). Read-only.
#
# LIVE BOOK = engines whose AUDITED_CONFIGS.tsv verdict is EDGE *and* both-regime
# (bull-only EDGEs and live-falsified ones are NOT in the live book). Edit
# LIVE_BOOK below as the manifest changes — keep it tied to AUDITED_CONFIGS verdicts.
#
# usage: python3 tools/livebook_pnl.py [omega_trade_closes.csv]
import sys, csv
from collections import defaultdict

# Validated both-regime EDGE engines (AUDITED_CONFIGS.tsv verdict=EDGE, cross-regime).
# Prefix-matched against TradeRecord.engine (covers per-cell tag suffixes).
LIVE_BOOK = [
    "SpxTurtleD1",      # EDGE 10yr daily, 2022 bear +92, both halves+
    "Dj30TurtleD1",     # EDGE 10yr daily, 2022 bear +63, both halves+
    "FxXrevEurgbp",     # EDGE strongest, both-regime+both-halves, survives 2x spread
    "XauTf4h",          # EDGE bull1.58/bear1.13 cross-regime, NOT bull-beta
    "CalendarTom",      # EDGE turn-of-month, STRONGER in 2022 bear (flows not beta)
]
# NOTE: "XAU_4h_DonchN20/N100" tags may be XauTf4h cells (manifest EDGE) OR a separate
# Donchian engine — UNVERIFIED. N100 is a phantom (+4060/n3) -> SUSPECT. N20 is lifetime
# -142 -> left in RESEARCH until the tag<->engine mapping is confirmed against the code.
# Phantom/suspect stats that mask the real book (single fat-tails / port artifacts).
SUSPECT = ["XAU_4h_DonchN100", "USDJPY_4h_SPXVG", "NasOrbRetrace"]

def F(x):
    try: return float(x)
    except: return 0.0

def in_book(eng, book):
    return any(eng == b or eng.startswith(b) for b in book)

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "logs/trades/omega_trade_closes.csv"
    rows = list(csv.DictReader(open(path)))
    agg = defaultdict(lambda: [0, 0.0, 0])  # n, net, wins
    for r in rows:
        e = r.get("engine", ""); n = F(r.get("net_pnl"))
        agg[e][0] += 1; agg[e][1] += n; agg[e][2] += (1 if n > 0 else 0)

    def report(title, names):
        print(f"\n=== {title} ===")
        tot = 0.0; tn = 0
        for e in sorted(names, key=lambda x: agg[x][1]):
            n, net, w = agg[e]
            print(f"  {e:34} n={n:3} net={net:+9.2f} WR={100*w/n if n else 0:3.0f}%")
            tot += net; tn += n
        print(f"  {'-'*34} n={tn:3} net={tot:+9.2f}")
        return tot

    live = [e for e in agg if in_book(e, LIVE_BOOK) and e not in SUSPECT]
    susp = [e for e in agg if e in SUSPECT]
    rsch = [e for e in agg if e not in live and e not in susp]

    print(f"ledger: {path}  ({len(rows)} closed trades)")
    lt = report("LIVE BOOK (validated both-regime EDGE)", live)
    st = report("SUSPECT / PHANTOM (excluded — investigate)", susp)
    rt = report("RESEARCH (shadow candidates / marginal / dead)", rsch)

    print(f"\n>>> LIVE BOOK net = {lt:+.2f}   (this is the real number)")
    print(f">>> research net = {rt:+.2f}   |   phantom net = {st:+.2f} (masks the book)")
    missing = [b for b in LIVE_BOOK if not any(in_book(e, [b]) for e in agg)]
    if missing:
        print(f"\n!! validated engines with ZERO live trades this ledger: {', '.join(missing)}")
        print("   (the edges that should carry the book are barely firing)")

if __name__ == "__main__":
    main()
