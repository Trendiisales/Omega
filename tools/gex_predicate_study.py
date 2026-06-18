#!/usr/bin/env python3
# gex_predicate_study.py — does net-GEX regime predict subsequent index behavior?
#
# Reads data/gex_history.csv (built by run_gex_snapshot.ps1). Tests the dealer-gamma
# thesis that would justify wiring a GEX regime-gate into the index engines:
#   - net_gex POSITIVE (dealers long gamma)  -> MEAN-REVERT: price pins, small moves,
#       extensions fade, low realized vol; sign of move T->T+1 tends to REVERSE.
#   - net_gex NEGATIVE (dealers short gamma)  -> MOMENTUM: price trends, larger moves,
#       higher realized vol; sign of move T->T+1 tends to PERSIST.
#
# Self-contained: uses the `spot` column at each hourly snapshot to measure the
# realized move over the NEXT snapshot(s). No external price feed needed.
#
# Verdict logic (the predicate is REAL only if BOTH hold, with enough n):
#   (1) mean |next-step return| is materially LARGER in neg-gamma than pos-gamma
#       (vol-expansion signature), AND
#   (2) continuation rate (next-step sign persists) is >50% in neg-gamma and <50%
#       in pos-gamma (momentum vs mean-revert signature).
#
# usage: python3 tools/gex_predicate_study.py [data/gex_history.csv]
import sys, csv, statistics as st

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "data/gex_history.csv"
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            try:
                rows.append({"ts": r["ts_utc"], "ix": r["index"],
                             "spot": float(r["spot"]), "gex": float(r["net_gex"])})
            except (ValueError, KeyError):
                continue
    # per-index, time-ordered consecutive-step analysis
    by_ix = {}
    for r in rows:
        by_ix.setdefault(r["ix"], []).append(r)
    print(f"=== GEX predicate study ({path}) ===")
    GAP_MAX_H = 6  # skip overnight gaps: only score steps <= 6h apart (intraday)
    from datetime import datetime
    def parse(ts): return datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ")
    for ix, rs in by_ix.items():
        rs.sort(key=lambda x: x["ts"])
        # build (gex_sign_at_T, ret_T_to_T1, ret_T1_to_T2) triples for continuation
        steps = []  # (sign, ret_next, prev_ret)
        for i in range(1, len(rs)):
            dt_h = (parse(rs[i]["ts"]) - parse(rs[i-1]["ts"])).total_seconds() / 3600.0
            if dt_h > GAP_MAX_H or dt_h <= 0:
                continue
            ret = (rs[i]["spot"] / rs[i-1]["spot"] - 1.0) * 1e4  # bps
            prev_ret = None
            if i >= 2:
                dt0 = (parse(rs[i-1]["ts"]) - parse(rs[i-2]["ts"])).total_seconds() / 3600.0
                if 0 < dt0 <= GAP_MAX_H:
                    prev_ret = (rs[i-1]["spot"] / rs[i-2]["spot"] - 1.0) * 1e4
            steps.append((1 if rs[i-1]["gex"] > 0 else -1, ret, prev_ret))
        pos = [s for s in steps if s[0] > 0]
        neg = [s for s in steps if s[0] < 0]
        def absmean(g): return st.mean(abs(s[1]) for s in g) if g else float("nan")
        def cont(g):  # continuation rate: prev_ret and ret same sign
            c = [(1 if (s[2] is not None and s[1]*s[2] > 0) else 0) for s in g if s[2] is not None]
            return (100.0*sum(c)/len(c), len(c)) if c else (float("nan"), 0)
        pc, pn = cont(pos); nc, nn = cont(neg)
        print(f"\n[{ix}] steps={len(steps)} (pos-gamma={len(pos)} neg-gamma={len(neg)})")
        print(f"  mean |next-step move|:  pos-gamma={absmean(pos):.1f}bps  neg-gamma={absmean(neg):.1f}bps"
              f"   ({'NEG larger (thesis OK)' if (neg and pos and absmean(neg)>absmean(pos)) else 'thesis NOT seen'})")
        print(f"  continuation rate:      pos-gamma={pc:.0f}% (n={pn})  neg-gamma={nc:.0f}% (n={nn})"
              f"   (thesis: pos<50, neg>50)")
        n_min = min(len(pos), len(neg))
        if n_min < 30:
            print(f"  ** DATA-STARVED: {n_min} samples in smaller regime bucket (need >=30/bucket for a verdict). "
                  f"Snapshotter accumulates hourly during the US session — re-run as it grows. **")
    print()

if __name__ == "__main__":
    main()
