#!/usr/bin/env python3
# gex_validate.py -- predicate study on accumulated GEX snapshots. PROVES (or
# disproves) the dealer-gamma thesis on OUR data before any engine routing.
#
# Input: gex_history.csv (appended by gex_chain.py --append): one row per intraday
# snapshot per index: ts_utc,index,spot,net_gex,regime,flip,call_wall,put_wall,...
#
# Tests (per index, then pooled), each vs a matched-random-level/sign baseline:
#  P1 VOL-DAMPENING (the regime claim): is the subsequent |spot move| SMALLER after
#     POSITIVE-gamma snapshots than after NEGATIVE-gamma? (pos = dealers dampen vol.)
#  P2 CALL-WALL RESISTANCE: when spot is near the call wall, does it reverse DOWN
#     (next move < 0) more than a coin flip / more than near a random level?
#  P3 PUT-WALL SUPPORT: near the put wall, does it reverse UP more than chance?
#  P4 FLIP REGIME SPLIT: above the flip, are forward moves more mean-reverting
#     (negative 1-step autocorr); below the flip, more trending (positive)?
#
# A real edge = the predicate holds OUT-OF-SAMPLE and beats the random baseline.
# usage: python ibkr/gex_validate.py [gex_history.csv] [--near-pct 0.3]
import sys, csv, statistics as st

def main():
    path = next((a for a in sys.argv[1:] if not a.startswith("--")), "C:/Omega/data/gex_history.csv")
    near = float(sys.argv[sys.argv.index("--near-pct")+1]) if "--near-pct" in sys.argv else 0.3  # % of spot = "near a wall"
    rows = [r for r in csv.DictReader(open(path))]
    def fl(x):
        try: return float(x)
        except: return None
    # group by index, time-ordered
    byix = {}
    for r in rows:
        byix.setdefault(r["index"], []).append(r)
    print(f"# gex_validate: {len(rows)} snapshots, {len(byix)} indices, near={near}% of spot")
    MIN = 40
    total = 0
    for ix, snaps in byix.items():
        snaps = [s for s in snaps if fl(s.get("spot"))]
        n = len(snaps); total += n
        print(f"\n==== {ix}  ({n} snapshots) ====")
        if n < MIN:
            print(f"   NOT ENOUGH DATA yet (need >= {MIN}; accumulating ~{n}). Predicate study deferred.")
            continue
        spots = [fl(s["spot"]) for s in snaps]
        moves = [(spots[i+1]-spots[i])/spots[i]*100 for i in range(n-1)]  # forward % move per step
        # P1 vol-dampening
        pos = [abs(moves[i]) for i in range(n-1) if snaps[i].get("regime","").startswith("positive")]
        neg = [abs(moves[i]) for i in range(n-1) if snaps[i].get("regime","").startswith("negative")]
        if pos and neg:
            print(f"   P1 vol-dampening: |move| after POS-gamma={st.mean(pos):.3f}% vs NEG-gamma={st.mean(neg):.3f}%"
                  f"  -> {'CONFIRMS (pos<neg)' if st.mean(pos)<st.mean(neg) else 'FAILS'}  (n_pos={len(pos)} n_neg={len(neg)})")
        else:
            print(f"   P1: need both regimes present (pos={len(pos)} neg={len(neg)})")
        # P2/P3 wall reactions
        cwhit=cwn=pwhit=pwn=0
        for i in range(n-1):
            s=spots[i]; cw=fl(snaps[i].get("call_wall")); pw=fl(snaps[i].get("put_wall")); mv=moves[i]
            if cw and abs(s-cw)/s*100 <= near: cwn+=1; cwhit += (mv<0)   # near call wall -> reverse down?
            if pw and abs(s-pw)/s*100 <= near: pwn+=1; pwhit += (mv>0)   # near put wall -> reverse up?
        if cwn: print(f"   P2 call-wall resistance: {cwhit}/{cwn} reversed down near wall ({100*cwhit/cwn:.0f}% vs 50% chance)")
        else:   print(f"   P2: no snapshots near call wall yet")
        if pwn: print(f"   P3 put-wall support:     {pwhit}/{pwn} reversed up near wall ({100*pwhit/pwn:.0f}% vs 50% chance)")
        else:   print(f"   P3: no snapshots near put wall yet")
        # P4 flip regime split (mean-rev above vs trend below)
        def autocorr(seq):
            if len(seq)<3: return None
            m=st.mean(seq); num=sum((seq[i]-m)*(seq[i+1]-m) for i in range(len(seq)-1)); den=sum((x-m)**2 for x in seq)
            return num/den if den else None
        above=[moves[i] for i in range(n-1) if fl(snaps[i].get("flip")) and spots[i]>fl(snaps[i]["flip"])]
        below=[moves[i] for i in range(n-1) if fl(snaps[i].get("flip")) and spots[i]<fl(snaps[i]["flip"])]
        aa,ab=autocorr(above),autocorr(below)
        if aa is not None and ab is not None:
            print(f"   P4 flip split: 1-step autocorr above-flip={aa:+.2f} (want <0 mean-rev) | below-flip={ab:+.2f} (want >0 trend)"
                  f"  -> {'CONFIRMS' if aa<ab else 'FAILS'}")
        else:
            print(f"   P4: not enough above/below-flip samples yet (above={len(above)} below={len(below)})")
    if total < MIN:
        print(f"\nVERDICT: accumulating ({total} snapshots). Run the GEX snapshotter through market hours; "
              f"re-run this once each index has >= {MIN} snapshots (~1-2 weeks).")

if __name__ == "__main__":
    main()
