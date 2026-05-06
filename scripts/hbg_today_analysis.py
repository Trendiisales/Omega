#!/usr/bin/env python3
"""
HybridBracketGold filter analysis on today's live tick data.

Reads:  /Users/jo/Tick/today/l2_ticks_XAUUSD_2026-05-06.csv
Output: comparison table -- which MIN_RANGE / ATR / cooldown thresholds
        would have blocked each of today's SL fires while preserving the
        trail wins.

Usage:
    python3 scripts/hbg_today_analysis.py
    python3 scripts/hbg_today_analysis.py --csv /path/to/l2_ticks.csv

Trade fire times are baked from the 2026-05-06 trade journal posted by
the user. Adjust the FIRES list if running on a different day.
"""

import csv
import argparse
import sys
from datetime import datetime, timezone
from collections import deque

# --- Today's HybridBracketGold trades (from 2026-05-06 journal) -------------
# (entry_time_utc, side, outcome_tag, net_pnl)
FIRES = [
    ("09:00:12", "LONG",  "TRAIL", +1.79),
    ("10:30:39", "LONG",  "TRAIL", +0.25),
    ("10:54:06", "LONG",  "SL",    -7.08),
    ("11:41:48", "SHORT", "TRAIL", +2.22),
    ("13:00:30", "SHORT", "SL",    -6.29),
    ("13:02:00", "LONG",  "SL",    -9.08),
    ("13:05:52", "LONG",  "SL",    -6.71),
    ("13:08:34", "SHORT", "SL",    -7.53),
    ("13:16:23", "SHORT", "TRAIL", +2.10),
    ("13:51:01", "SHORT", "SL",    -6.97),
    ("14:07:01", "SHORT", "SL",    -6.87),
]

LOOKBACK_TICKS = 600  # HybridBracketGold compression window per S22c
ATR_LOOKBACK_TICKS = 14 * 60  # rough ATR sample, ~14 minutes at 60 ticks/min

# --- Helpers ----------------------------------------------------------------
def parse_csv(path):
    """Yield (ts_ms, mid, bid, ask) tuples from omega l2_ticks CSV."""
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                yield (
                    int(row["ts_ms"]),
                    float(row["mid"]),
                    float(row["bid"]),
                    float(row["ask"]),
                )
            except (KeyError, ValueError):
                continue


def utc_hms_to_ms(hms, ref_date):
    """Convert "HH:MM:SS" to ms-since-epoch on the same UTC date as ref_date."""
    h, m, s = (int(x) for x in hms.split(":"))
    dt = datetime(
        ref_date.year, ref_date.month, ref_date.day,
        h, m, s, tzinfo=timezone.utc,
    )
    return int(dt.timestamp() * 1000)


def fmt_dt(ts_ms):
    return datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime("%H:%M:%S")


# --- Analysis ---------------------------------------------------------------
def analyze(csv_path):
    print(f"[ANALYSIS] Reading {csv_path}")
    print(f"[ANALYSIS] Examining {len(FIRES)} HybridBracketGold fires from 2026-05-06\n")

    # Load all ticks into a list -- 31MB CSV fits comfortably in RAM.
    ticks = list(parse_csv(csv_path))
    if not ticks:
        print("[ERROR] No ticks parsed from CSV", file=sys.stderr)
        sys.exit(1)
    print(f"[ANALYSIS] Loaded {len(ticks):,} ticks from "
          f"{fmt_dt(ticks[0][0])} to {fmt_dt(ticks[-1][0])} UTC\n")

    ref_date = datetime.fromtimestamp(ticks[0][0] / 1000, tz=timezone.utc).date()

    # Find the index of each fire in the tick stream.
    fire_data = []
    for hms, side, outcome, pnl in FIRES:
        target_ms = utc_hms_to_ms(hms, ref_date)
        # Binary search would be cleaner; linear is fine for a few fires.
        idx = next(
            (i for i, t in enumerate(ticks) if t[0] >= target_ms),
            None,
        )
        if idx is None:
            print(f"[WARN] No tick found at/after {hms} -- skipping")
            continue

        # Compute compression range over prior LOOKBACK_TICKS
        start = max(0, idx - LOOKBACK_TICKS)
        window_mids = [t[1] for t in ticks[start:idx]]
        if len(window_mids) < 50:
            range_pt = 0.0
        else:
            range_pt = max(window_mids) - min(window_mids)

        # Compute "ATR" proxy: avg absolute tick-to-tick move over wider window
        atr_start = max(0, idx - ATR_LOOKBACK_TICKS)
        atr_mids = [t[1] for t in ticks[atr_start:idx]]
        if len(atr_mids) < 50:
            atr_pt = 0.0
        else:
            moves = [abs(atr_mids[i] - atr_mids[i - 1]) for i in range(1, len(atr_mids))]
            atr_pt = sum(moves) / len(moves) * 60  # scale to per-minute approx

        spread = ticks[idx][3] - ticks[idx][2]

        fire_data.append({
            "hms": hms,
            "side": side,
            "outcome": outcome,
            "pnl": pnl,
            "range_pt": range_pt,
            "atr_pt": atr_pt,
            "spread": spread,
            "idx": idx,
        })

    # Print per-fire detail
    print("FIRE DETAIL:")
    print(f"{'time':<10} {'side':<6} {'outcome':<8} {'net':>7}  "
          f"{'range_600':>10} {'atr_proxy':>10} {'spread':>8}")
    print("-" * 70)
    for f in fire_data:
        print(f"{f['hms']:<10} {f['side']:<6} {f['outcome']:<8} "
              f"{f['pnl']:>+7.2f}  "
              f"{f['range_pt']:>10.2f} {f['atr_pt']:>10.3f} "
              f"{f['spread']:>8.3f}")

    # --- Sweep ---
    # For each MIN_RANGE threshold, determine which fires it would block.
    print("\n\nMIN_RANGE FILTER SWEEP")
    print("=" * 80)
    print(f"{'min_range':>10} {'kept_SLs':>9} {'kept_TRAILs':>12} "
          f"{'blocked_SL':>11} {'blocked_TR':>11} {'kept_net':>10}")
    print("-" * 80)

    for min_r in [1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0]:
        kept = [f for f in fire_data if f["range_pt"] >= min_r]
        blocked = [f for f in fire_data if f["range_pt"] < min_r]
        kept_sl = sum(1 for f in kept if f["outcome"] == "SL")
        kept_tr = sum(1 for f in kept if f["outcome"] == "TRAIL")
        blocked_sl = sum(1 for f in blocked if f["outcome"] == "SL")
        blocked_tr = sum(1 for f in blocked if f["outcome"] == "TRAIL")
        kept_net = sum(f["pnl"] for f in kept)
        print(f"{min_r:>10.1f} {kept_sl:>9d} {kept_tr:>12d} "
              f"{blocked_sl:>11d} {blocked_tr:>11d} {kept_net:>+10.2f}")

    # --- Post-SL cooldown sweep ---
    print("\n\nPOST-SL COOLDOWN SWEEP")
    print("=" * 80)
    print(f"{'cooldown_s':>11} {'kept_fires':>11} {'kept_SLs':>9} "
          f"{'kept_TRAILs':>12} {'kept_net':>10}")
    print("-" * 80)

    sorted_fires = sorted(fire_data, key=lambda f: f["hms"])
    for cooldown_sec in [0, 60, 180, 300, 600, 900, 1200, 1800]:
        kept = []
        last_sl_ms = None
        for f in sorted_fires:
            target_ms = utc_hms_to_ms(f["hms"], ref_date)
            if last_sl_ms is None or (target_ms - last_sl_ms) / 1000 >= cooldown_sec:
                kept.append(f)
                if f["outcome"] == "SL":
                    last_sl_ms = target_ms
        kept_sl = sum(1 for f in kept if f["outcome"] == "SL")
        kept_tr = sum(1 for f in kept if f["outcome"] == "TRAIL")
        kept_net = sum(f["pnl"] for f in kept)
        print(f"{cooldown_sec:>11d} {len(kept):>11d} {kept_sl:>9d} "
              f"{kept_tr:>12d} {kept_net:>+10.2f}")

    # --- Combined sweep: MIN_RANGE x COOLDOWN ---
    print("\n\nCOMBINED MIN_RANGE x COOLDOWN SWEEP (best 10 by net)")
    print("=" * 80)
    print(f"{'min_range':>10} {'cooldown':>10} {'fires':>7} {'SLs':>5} "
          f"{'TRs':>5} {'net':>10}")
    print("-" * 80)

    combos = []
    for min_r in [1.5, 2.5, 3.0, 4.0, 5.0]:
        for cooldown_sec in [0, 300, 600, 900, 1200]:
            kept = []
            last_sl_ms = None
            for f in sorted_fires:
                if f["range_pt"] < min_r:
                    continue
                target_ms = utc_hms_to_ms(f["hms"], ref_date)
                if last_sl_ms is None or (target_ms - last_sl_ms) / 1000 >= cooldown_sec:
                    kept.append(f)
                    if f["outcome"] == "SL":
                        last_sl_ms = target_ms
            kept_sl = sum(1 for f in kept if f["outcome"] == "SL")
            kept_tr = sum(1 for f in kept if f["outcome"] == "TRAIL")
            net = sum(f["pnl"] for f in kept)
            combos.append((min_r, cooldown_sec, len(kept), kept_sl, kept_tr, net))

    combos.sort(key=lambda c: -c[5])  # sort by net desc
    for c in combos[:10]:
        print(f"{c[0]:>10.1f} {c[1]:>10d} {c[2]:>7d} {c[3]:>5d} "
              f"{c[4]:>5d} {c[5]:>+10.2f}")

    print(f"\n{'BASELINE (current params, no filter)':>50}: "
          f"{sum(f['pnl'] for f in fire_data):>+8.2f}")
    print(f"{'BEST COMBO net':>50}: {combos[0][5]:>+8.2f} "
          f"(min_range={combos[0][0]} cooldown={combos[0][1]}s)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--csv",
        default="/Users/jo/Tick/today/l2_ticks_XAUUSD_2026-05-06.csv",
    )
    args = parser.parse_args()
    analyze(args.csv)


if __name__ == "__main__":
    main()
