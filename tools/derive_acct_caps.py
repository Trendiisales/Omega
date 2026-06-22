#!/usr/bin/env python3
# =============================================================================
# derive_acct_caps.py -- regenerate AccountingGuard per-engine runaway caps from
# the CLEANED cumulative close ledger (operator mandate 2026-06-22: "ensure the
# cap table can never go stale, always viable + available").
#
# This is the AUDIT / VERIFICATION twin of the in-process C++ rebuild in
# include/AccountingGuard.hpp. Both must produce identical caps on identical
# input -- this script is the faithful Python reference + the diff-vs-seed tool.
#
# CAP RULE (matches AccountingGuard seed-derivation 2026-06-13):
#   cap[engine] = clamp( 3 * median(|net_loss|), FLOOR=$25, CEIL=$800 )
#   only when n_losses >= MIN_N (else the engine keeps its compiled seed / DEFAULT).
#
# ARTIFACT FILTER (reused VERBATIM from tools/analytics/ledger_analytics.py:73-89,
#   the 2026-06-16 cleaning that fixed the tombstone-ledger-pollution cull bug):
#   - drop hold_sec > 7d           (multi-day PHANTOM holds)
#   - drop XAU/XAG with size > 0.05 (pre-2026-06-09 lot=1.0 100x metal bug)
#   - optional --since YYYY-MM-DD   (date cutoff, e.g. post the lot fix)
#
# USAGE:
#   ./derive_acct_caps.py "logs/trades/omega_trade_closes*.csv" [--out state/acct_caps.tsv] [--since 2026-06-09] [--min-n 20]
# =============================================================================
import sys, csv, glob, collections, statistics, time

FLOOR, CEIL, MULT = 25.0, 800.0, 3.0
DEFAULT_MIN_N = 20

def f(x):
    try: return float(str(x).strip().strip('"'))
    except: return 0.0

def main():
    # value-consuming flags must not be mistaken for the positional glob
    VALUE_FLAGS = {"--out","--since","--min-n"}
    pos = []; skip = False
    for a in sys.argv[1:]:
        if skip: skip = False; continue
        if a in VALUE_FLAGS: skip = True; continue
        if a.startswith("--"): continue
        pos.append(a)
    pattern = pos[0] if pos else "logs/trades/omega_trade_closes*.csv"
    out     = sys.argv[sys.argv.index("--out")+1]   if "--out"   in sys.argv else "state/acct_caps.tsv"
    since   = sys.argv[sys.argv.index("--since")+1] if "--since" in sys.argv else None
    min_n   = int(sys.argv[sys.argv.index("--min-n")+1]) if "--min-n" in sys.argv else DEFAULT_MIN_N

    files = [p for p in glob.glob(pattern) if ".bak" not in p and ".cleared" not in p]
    if not files:
        print(f"# no ledger files matched {pattern}", file=sys.stderr); sys.exit(1)

    losses = collections.defaultdict(list)   # engine -> [ |net_loss|, ... ]
    EX = collections.Counter(); n_rows = 0
    for path in files:
        with open(path, newline="") as fh:
            for r in csv.DictReader(fh):
                n_rows += 1
                eng = (r.get("engine") or "?").strip('"')
                sym = (r.get("symbol") or "?").strip('"')
                # --- artifact filter (verbatim from ledger_analytics.py) ---
                if f(r.get("hold_sec")) > 7*86400: EX["phantom_hold>7d"] += 1; continue
                if sym in ("XAUUSD","XAGUSD") and (f(r.get("size")) or 0) > 0.05:
                    EX["oversized_metal_lot"] += 1; continue
                if since and (r.get("exit_ts_utc") or "").strip('"')[:10] < since:
                    EX["pre_since"] += 1; continue
                net = f(r.get("net_pnl"))
                if net < 0: losses[eng].append(abs(net))

    if EX:
        print("# EXCLUDED artifacts: " + "  ".join(f"{k}={v}" for k,v in EX.items()))
    print(f"# scanned {n_rows} closes across {len(files)} file(s); {len(losses)} engines with losses\n")

    derived_unix = int(time.time())
    rows_out = []
    print(f"{'engine':<26} {'n_loss':>6} {'median$':>8} {'cap$':>7}  note")
    for eng in sorted(losses):
        ln = losses[eng]
        if len(ln) < min_n:
            print(f"{eng:<26} {len(ln):>6} {statistics.median(ln):>8.2f} {'--':>7}  n<{min_n} -> keeps seed")
            continue
        med = statistics.median(ln)
        cap = min(CEIL, max(FLOOR, MULT*med))
        rows_out.append((eng, cap, len(ln), med))
        print(f"{eng:<26} {len(ln):>6} {med:>8.2f} {cap:>7.2f}")

    with open(out, "w") as o:
        o.write("# acct_caps.tsv -- engine\\tcap_usd\\tn_losses\\tmedian_loss\\tderived_unix\n")
        o.write(f"# generated {derived_unix} ; rule cap=clamp(3*median(|net_loss|),{FLOOR},{CEIL}) ; min_n={min_n}\n")
        for eng, cap, n, med in rows_out:
            o.write(f"{eng}\t{cap:.2f}\t{n}\t{med:.2f}\t{derived_unix}\n")
    print(f"\n# wrote {len(rows_out)} dynamic caps -> {out}")

if __name__ == "__main__":
    main()
