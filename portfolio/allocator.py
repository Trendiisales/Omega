#!/usr/bin/env python3
"""
Omega portfolio allocator
=========================
Combines per-engine trade streams into one vol-targeted, correlation-aware book
and reports the diversification lift.

WHY: individual engines are decent-but-lumpy (Sharpe ~1, regime-dependent). Real
money comes from blending UNCORRELATED edges so the bumps cancel -> smoother book
-> higher Sharpe -> safely leverageable. This tool does that blend honestly.

INPUT  : streams/<Name>_trades.txt  -- one line per closed trade: "epoch_seconds,pnl"
         (the "standard runner" format: any backtest harness emits it by adding a
          PORT_DUMP env dump near where it finalises trades.)
CONFIG : book.json  -- {"book": ["Name1","Name2",...]}  the engines to combine.
USAGE  : python3 allocator.py [book.json]

METHOD:
  1. weekly-bucket each engine's pnl (units don't matter -- step 2 normalises).
  2. vol-target: divide each weekly series by its own stdev -> equal risk per engine
     (keep the mean, so drift is preserved -- do NOT z-score).
  3. correlation matrix on overlapping weeks; flag clusters (|rho|>=0.4) -- engines
     in a cluster are ~one bet, so they get down-weighted (1/cluster_size).
  4. combined = correlation-capped weighted mean of the normalised streams.
  5. report per-engine + combined Sharpe (ann = weekly mean/std * sqrt(52)),
     full-span and overlap-only, plus the equity curve.
"""
import sys, json, math, os, datetime as dt

ROOT = os.path.dirname(os.path.abspath(__file__))

def wk(ts):
    return dt.datetime.fromtimestamp(int(ts), dt.timezone.utc).date().isocalendar()[:2]

def load(path):
    d = {}
    for ln in open(path):
        ln = ln.strip()
        if not ln or ln[0] not in "0123456789":
            continue
        a = ln.split(",")
        try:
            d[wk(a[0])] = d.get(wk(a[0]), 0.0) + float(a[1])
        except ValueError:
            pass
    return d

def ann_sharpe(series):
    v = list(series.values()); n = len(v)
    if n < 3:
        return 0.0
    mu = sum(v) / n
    sd = (sum((x - mu) ** 2 for x in v) / n) ** 0.5
    return mu / sd * math.sqrt(52) if sd > 0 else 0.0

def corr(a, b):
    cw = sorted(set(a) & set(b)); k = len(cw)
    if k < 8:
        return None
    va = [a[w] for w in cw]; vb = [b[w] for w in cw]
    m1, m2 = sum(va) / k, sum(vb) / k
    cov = sum((va[i] - m1) * (vb[i] - m2) for i in range(k)) / k
    s1 = (sum((x - m1) ** 2 for x in va) / k) ** 0.5
    s2 = (sum((x - m2) ** 2 for x in vb) / k) ** 0.5
    return cov / (s1 * s2) if s1 * s2 > 0 else 0.0

def main():
    cfg = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "book.json")
    book = json.load(open(cfg))["book"]

    raw = {}
    for name in book:
        p = os.path.join(ROOT, "streams", f"{name}_trades.txt")
        if not os.path.exists(p):
            print(f"  ! missing stream: {name} ({p})"); continue
        raw[name] = load(p)
    names = [n for n in book if n in raw and raw[n]]

    # vol-target each over its active span
    norm = {}
    for n in names:
        d = raw[n]; lo, hi = min(d), max(d)
        span = [d.get(w, 0.0) for w in _weeks(lo, hi)]
        mu = sum(span) / len(span)
        sd = (sum((x - mu) ** 2 for x in span) / len(span)) ** 0.5
        norm[n] = {w: d.get(w, 0.0) / sd for w in _weeks(lo, hi)} if sd > 0 else {}

    print("=== per-engine (weekly, annualised) ===")
    for n in names:
        print(f"  {n:18s} Sharpe={ann_sharpe(raw[n]):+.2f}  trades={sum(1 for _ in open(os.path.join(ROOT,'streams',n+'_trades.txt')))}")

    # correlation + cluster detection (|rho|>=0.4 links engines)
    print("\n=== correlation (overlapping weeks, blank if <8) ===")
    print("            " + " ".join(f"{n[:9]:>9s}" for n in names))
    C = {}
    for a in names:
        row = []
        for b in names:
            if a == b:
                C[(a, b)] = 1.0; row.append("  1.00"); continue
            c = corr(norm[a], norm[b]); C[(a, b)] = c
            row.append(f"{c:+.2f}" if c is not None else "    .")
        print(f"{a:11s} " + " ".join(f"{x:>9s}" for x in row))

    # cluster -> correlation-cap weights (engine weight = 1/its cluster size)
    parent = {n: n for n in names}
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]; x = parent[x]
        return x
    for a in names:
        for b in names:
            if a < b and (C[(a, b)] or 0) >= 0.4:
                parent[find(a)] = find(b)
    clusters = {}
    for n in names:
        clusters.setdefault(find(n), []).append(n)
    weight = {}
    for members in clusters.values():
        for n in members:
            weight[n] = 1.0 / len(members)   # correlation cap: a cluster = one bet
    if any(len(m) > 1 for m in clusters.values()):
        print("\n=== correlation-capped clusters (each cluster counts as ~1 bet) ===")
        for m in clusters.values():
            if len(m) > 1:
                print(f"  {' + '.join(m)}  -> each weighted {1.0/len(m):.2f}")

    # combined = correlation-capped weighted mean of normalised streams
    allw = sorted(set().union(*[set(norm[n]) for n in names]))
    combo = {}
    for w in allw:
        num = sum(weight[n] * norm[n][w] for n in names if w in norm[n])
        den = sum(weight[n] for n in names if w in norm[n])
        if den > 0:
            combo[w] = num / den
    ov = sorted(set.intersection(*[set(norm[n]) for n in names]))
    combo_ov = {w: sum(weight[n] * norm[n][w] for n in names) / sum(weight.values()) for w in ov} if ov else {}

    best = max(ann_sharpe(raw[n]) for n in names)
    print("\n=== COMBINED (vol-targeted, correlation-capped) ===")
    print(f"  best single engine Sharpe = {best:+.2f}")
    print(f"  combined Sharpe (full span) = {ann_sharpe(combo):+.2f}")
    if ov:
        print(f"  combined Sharpe (overlap, all active, {len(ov)} wks) = {ann_sharpe(combo_ov):+.2f}")

    # equity curve export
    tot = 0; curve = []
    for w in allw:
        tot += combo.get(w, 0.0); curve.append([f"{w[0]}-{w[1]:02d}", round(tot, 3)])
    json.dump({"book": names, "weights": weight,
               "sharpe_full": round(ann_sharpe(combo), 2),
               "sharpe_overlap": round(ann_sharpe(combo_ov), 2) if ov else None,
               "curve": curve},
              open(os.path.join(ROOT, "book_result.json"), "w"), indent=1)
    print("\nwrote book_result.json (weights + equity curve)")

def _weeks(lo, hi):
    out = []; y, w = lo
    while (y, w) <= hi:
        out.append((y, w)); w += 1
        if w > 52:
            w = 1; y += 1
    return out

if __name__ == "__main__":
    main()
