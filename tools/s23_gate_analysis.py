#!/usr/bin/env python3
"""
S23 gate validation against daily shadow trade CSVs with FULL schema.

Daily CSV has: entry_px, exit_px, tp, sl (at close), size, gross_pnl, mfe,
mae, hold_sec, exit_reason, spread_at_entry. This is enough to precisely
determine:

  1. tp_dist at entry = |entry - initial_tp|. For HybridBracketIndex,
     initial_tp = entry + sl_dist * tp_rr. We don't have initial_tp directly,
     but we can derive sl_dist from the entry/sl at fill (sl column shows
     the TRAIL-moved sl at close, not original). For HybridBracketIndex with
     cfg.tp_rr=2.0, tp_dist ~ 2 * sl_dist_at_fill. But sl_at_fill isn't in
     CSV -- we only see sl_at_close.
     Fall back to: infer tp_dist from `mfe` of TRAIL_HIT trades (they
     reached >= tp_dist * some_fraction), or from the NAS-specific
     config.max_range/min_range (bracket range at arm drives sl_dist).

  2. MFE exceeded threshold: direct check against `mfe` column.

  3. Hold exceeded cap: direct check against `hold_sec` column.

What's reportable WITHOUT tick data:
  - How many trades would be blocked/aborted/changed by each gate
  - For S20 MAX_HOLD: whether mfe at time of close was positive (since mfe
    is peak, not final, a winner might have held AT cap but tick data needed
    to know its mid price at t=cap)

Output: per-trade analysis per-gate.
"""
import csv
import glob
from collections import defaultdict

# S23 params per symbol
P = {
    "NAS100":  dict(max_hold=1500, be_old=0.40, be_new=0.70, mta_pts=12.0, mta_s=20,
                    max_sl=40.0,   cfm_pts=24.0, cfm_s=45, usd_per_pt=1.0),
    "US500.F": dict(max_hold=1800, be_old=0.40, be_new=0.60, mta_pts=3.0,  mta_s=15,
                    max_sl=12.0,   cfm_pts=6.0,  cfm_s=30, usd_per_pt=50.0),
    "USTEC.F": dict(max_hold=1800, be_old=0.40, be_new=0.60, mta_pts=8.0,  mta_s=15,
                    max_sl=45.0,   cfm_pts=22.0, cfm_s=30, usd_per_pt=20.0),
    "DJ30.F":  dict(max_hold=1800, be_old=0.40, be_new=0.60, mta_pts=15.0, mta_s=15,
                    max_sl=90.0,   cfm_pts=45.0, cfm_s=30, usd_per_pt=5.0),
}


def load_trades():
    rows = []
    for fp in sorted(glob.glob("/home/claude/s23/omega_shadow_trades_*.csv")):
        with open(fp) as f:
            rdr = csv.DictReader(f)
            for r in rdr:
                # Strip quotes from string columns
                for k in ("symbol", "engine", "side", "regime", "exit_reason"):
                    if k in r and r[k]:
                        r[k] = r[k].strip('"').strip()
                rows.append(r)
    return rows


def num(x):
    try: return float(x)
    except: return 0.0


def main():
    rows = load_trades()
    # Scope: only HybridBracketIndex (NAS) and XAUUSD_BRACKET / HybridBracketGold
    # (not in S23 index port scope but useful as baseline), and any index BracketEngine
    # trades (none expected).
    in_scope = [r for r in rows
                if r["symbol"] in P
                and r["engine"] in ("HybridBracketIndex",)]

    print("=" * 78)
    print("S23 GATE ANALYSIS - daily shadow_trades CSVs, full schema")
    print("=" * 78)
    print(f"Total HybridBracketIndex trades: {len(in_scope)}")

    # Group by symbol
    by_sym = defaultdict(list)
    for r in in_scope:
        by_sym[r["symbol"]].append(r)

    for sym, trs in sorted(by_sym.items()):
        p = P[sym]
        print()
        print("=" * 78)
        print(f"{sym}  (n={len(trs)})")
        print("=" * 78)
        orig_total = sum(num(r["net_pnl"]) for r in trs)
        print(f"Original net PnL: ${orig_total:+.2f}")

        # Exit reason breakdown
        er = defaultdict(lambda: [0, 0.0])
        for r in trs:
            er[r["exit_reason"]][0] += 1
            er[r["exit_reason"]][1] += num(r["net_pnl"])
        print(f"Exit reason breakdown:")
        for reason, (n, pnl) in sorted(er.items()):
            print(f"  {reason:30s}  n={n:3d}  pnl=${pnl:+.2f}")

        # --- Gate: S20 MAX_HOLD_SEC ---
        print()
        print(f"--- S20 MAX_HOLD_SEC = {p['max_hold']}s ---")
        over = [r for r in trs if num(r["hold_sec"]) > p["max_hold"]]
        print(f"Trades over cap: {len(over)}")
        over_win = [r for r in over if num(r["net_pnl"]) > 0]
        over_lose = [r for r in over if num(r["net_pnl"]) <= 0]
        print(f"  Winners over cap: {len(over_win)} pnl=${sum(num(r['net_pnl']) for r in over_win):+.2f}")
        print(f"  Losers over cap:  {len(over_lose)} pnl=${sum(num(r['net_pnl']) for r in over_lose):+.2f}")
        if over:
            print(f"  Details:")
            for r in over:
                pnl = num(r["net_pnl"])
                mfe = num(r["mfe"])
                hold = int(num(r["hold_sec"]))
                print(f"    entry={r['entry_ts_utc']:22s} pnl=${pnl:+7.2f} mfe={mfe:6.2f}"
                      f" hold={hold:5d}s reason={r['exit_reason']} size={r['size']}")

        # --- Gate: S23 trail-arm guards ---
        print()
        print(f"--- S23 trail-arm guards (min_trail_arm_pts={p['mta_pts']}, min_trail_arm_secs={p['mta_s']}) ---")
        print(f"Identifies trades where BE lock would NOT have engaged under new rules.")
        be_trades = [r for r in trs if r["exit_reason"] == "BE_HIT"]
        trail_trades = [r for r in trs if r["exit_reason"] == "TRAIL_HIT"]
        # Original BE lock happens at be_old*tp_dist. tp_dist unknown per-trade.
        # But we KNOW these BE_HIT trades locked under original rules.
        # Under S23: BE also requires mfe >= mta_pts AND held >= mta_s.
        blocked_by_guard = [r for r in be_trades
                            if num(r["mfe"]) < p["mta_pts"] or num(r["hold_sec"]) < p["mta_s"]]
        print(f"  BE_HIT trades that FAIL the guard (would stay open):")
        print(f"    {len(blocked_by_guard)} / {len(be_trades)}")
        if blocked_by_guard:
            for r in blocked_by_guard[:10]:
                pnl = num(r["net_pnl"])
                mfe = num(r["mfe"])
                hold = int(num(r["hold_sec"]))
                reasons = []
                if num(r["mfe"]) < p["mta_pts"]: reasons.append(f"mfe<{p['mta_pts']}")
                if num(r["hold_sec"]) < p["mta_s"]: reasons.append(f"hold<{p['mta_s']}s")
                print(f"    pnl=${pnl:+7.2f} mfe={mfe:6.2f} hold={hold:4d}s  {' '.join(reasons)}")

        # --- Gate: S19 BE widening (40% -> 60%/70%) ---
        print()
        print(f"--- S19 BE widening (be_frac {p['be_old']*100:.0f}% -> {p['be_new']*100:.0f}%) ---")
        # TRAIL_HIT trades had mfe >= be_old*tp_dist. Infer tp_dist lower
        # bound from: max of all TRAIL_HIT MFEs (that's the best-case
        # trail-progressed MFE; tp_dist must be <= max_mfe in most cases).
        # Better heuristic: tp_dist ~ median TRAIL_HIT mfe * 1.3 (trails
        # captured ~75% of TP).
        if trail_trades:
            trail_mfes = sorted(num(r["mfe"]) for r in trail_trades)
            median_trail_mfe = trail_mfes[len(trail_mfes)//2]
            tp_dist_est = median_trail_mfe / 0.75
            print(f"  tp_dist estimated ~{tp_dist_est:.2f}pt (median TRAIL MFE {median_trail_mfe:.2f} / 0.75)")
            old_thr = p["be_old"] * tp_dist_est
            new_thr = p["be_new"] * tp_dist_est
            print(f"  Old BE trigger: mfe >= {old_thr:.2f}pt   New: {new_thr:.2f}pt")
            # BE_HIT trades with mfe in [old_thr, new_thr) would NOT BE-lock
            # under S23. They'd keep running -- either TRAIL or SL.
            in_gap = [r for r in be_trades
                      if num(r["mfe"]) >= old_thr and num(r["mfe"]) < new_thr]
            print(f"  BE_HIT trades in [old, new) MFE range: {len(in_gap)} / {len(be_trades)}")
            if in_gap:
                for r in in_gap[:10]:
                    pnl = num(r["net_pnl"])
                    mfe = num(r["mfe"])
                    hold = int(num(r["hold_sec"]))
                    print(f"    pnl=${pnl:+7.2f} mfe={mfe:6.2f} hold={hold:4d}s"
                          f" (would skip BE-lock under S23, likely -> SL)")
            # Also trades with mfe >= new_thr would still BE-lock (unchanged).
            above_new = [r for r in be_trades if num(r["mfe"]) >= new_thr]
            print(f"  BE_HIT trades already above new threshold: {len(above_new)} (unchanged)")
            below_old = [r for r in be_trades if num(r["mfe"]) < old_thr]
            print(f"  BE_HIT trades below old threshold: {len(below_old)} (shouldn't be BE_HIT in original logic; checking...)")
            if below_old:
                print("    (These have mfe < old_thr so would not have BE-locked even under original")
                print("     rules if tp_dist estimate is right -- engine must be using different")
                print("     tp_dist per trade. Conservative: trust actual BE_HIT classification.)")

        # --- S22c MAX_SL_DIST_PTS (note: not wired on HybridBracket) ---
        print()
        print("--- S22c MAX_SL_DIST_PTS ---")
        print("  NOT applied to HybridBracketIndex (uses config.max_range instead).")
        print("  S22c gate is on BracketEngine (g_bracket_*) which has 0 trades here.")

        # --- S21 CONFIRM gate ---
        print()
        print("--- S21 CONFIRM gate ---")
        print("  NOT applied to HybridBracketIndex (separate engine class).")
        print("  S21 gate is on BracketEngine (g_bracket_*) which has 0 trades here.")

    # == Baseline XAUUSD BRACKET engine trades for calibration ==
    print()
    print("=" * 78)
    print("BASELINE: XAUUSD_BRACKET trades (what S19/S20/S21/S22c was CALIBRATED on)")
    print("=" * 78)
    gold_brack = [r for r in rows if r["engine"] == "XAUUSD_BRACKET"]
    print(f"n={len(gold_brack)}")
    if gold_brack:
        gold_pnl = sum(num(r["net_pnl"]) for r in gold_brack)
        print(f"Gold total PnL: ${gold_pnl:+.2f}")
        # Wide SL trades
        wide = [r for r in gold_brack
                if abs(num(r["entry_px"]) - num(r["sl"])) > 6.0]
        print(f"Trades with |entry-sl| > 6pt (S22c would block): {len(wide)}")
        print(f"  sum pnl: ${sum(num(r['net_pnl']) for r in wide):+.2f}")

    # Totals
    print()
    print("=" * 78)


if __name__ == "__main__":
    main()
