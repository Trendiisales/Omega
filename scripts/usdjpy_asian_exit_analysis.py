#!/usr/bin/env python3
"""
usdjpy_asian_exit_analysis.py
=============================
Diagnose exit behavior on a USDJPY Asian-Open trade ledger.

Inputs
------
  --ledger  build/trail_trades_<label>.csv   (default: WF_baseline_verify)

Outputs
-------
  Console report only. No new files written.

What it computes
----------------
1. Exit-reason mix   (TP_HIT / TRAIL_HIT / SL_HIT / BE_HIT)
2. MFE distribution  (winners only): mean, median, p75, p90, p95, max
3. MAE distribution  (losers only):  mean, median, p75, p90, p95, max
4. MFE clip          (MFE - realised exit pips, winners only): how much
                     each winner left on the table when it stopped out
                     on the trail
5. By-month breakdown of (trades, exit mix, win rate, net PnL)
6. By-trade-size breakdown
7. Top "clipped" winners — biggest MFE-vs-realised gaps
8. Adverse winners — trades that closed positive but only after a deep
                     MAE (dipped near SL before recovering)

Conventions
-----------
  - USDJPY pip = 0.01 of price.
  - All "pips" reported below are computed as price-distance / 0.01.
  - For LONG:  realised_pips = (exitPrice - entryPrice) / 0.01
               mfe_pips      = mfe / 0.01      (mfe is max favourable
                                                price-distance above
                                                entry, in price units)
               mae_pips      = -mae / 0.01     (mae stored negative for
                                                long; flip sign for
                                                "depth of drawdown" pips)
  - For SHORT: realised_pips = (entryPrice - exitPrice) / 0.01
               same MFE/MAE conventions in price-distance terms.

  This matches the C++ harness: MFE/MAE are tracked as max favourable
  / max adverse PRICE-DISTANCE (signed positive for favourable, negative
  for adverse) and stored as price units, not pips.
"""

import argparse
import csv
import statistics as stats
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

PIP = 0.01  # USDJPY


def load_ledger(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def to_pips(d):
    """Convert price-distance to pips (USDJPY: 1 pip = 0.01)."""
    return d / PIP


def trade_pips(row):
    """Return (realised_pips, mfe_pips, mae_pips) where:
       - realised_pips: positive = winner, negative = loser
       - mfe_pips: best favourable distance reached during life (>= 0)
       - mae_pips: worst adverse distance reached during life (>= 0,
                   reported as POSITIVE depth)

    IMPORTANT: in the C++ harness (UsdjpyAsianOpenEngine.hpp lines 785-786)
    the trade record stores `tr.mfe = mfe_ * size_` and `tr.mae = mae_ * size_`,
    i.e. MFE/MAE are scaled by lot size before serialisation. To recover the
    raw price distance we divide by size before converting to pips.
    """
    side = row["side"]
    entry = float(row["entryPrice"])
    exit_ = float(row["exitPrice"])
    size  = float(row["size"]) or 1.0
    raw_mfe = float(row["mfe"]) / size
    raw_mae = float(row["mae"]) / size
    if side == "LONG":
        realised = (exit_ - entry)
        mfe = raw_mfe
        mae_depth = -raw_mae
    else:  # SHORT
        realised = (entry - exit_)
        # Engine convention: pos.mfe is a magnitude of favourable move
        # (always >= 0 once any favourable tick occurs); pos.mae is signed
        # against entry direction. For shorts the harness records the
        # adverse-against-entry depth as a negative number, same as longs.
        mfe = raw_mfe
        mae_depth = -raw_mae
    return to_pips(realised), to_pips(mfe), to_pips(mae_depth)


def percentile(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    k = (len(s) - 1) * p
    f = int(k)
    c = min(f + 1, len(s) - 1)
    if f == c:
        return s[f]
    return s[f] + (s[c] - s[f]) * (k - f)


def fmt_dist(values, label, units="pips"):
    if not values:
        print(f"  {label}: (empty)")
        return
    print(f"  {label} (n={len(values)}, {units}):")
    print(f"    mean   = {stats.fmean(values):7.2f}")
    print(f"    median = {stats.median(values):7.2f}")
    print(f"    p75    = {percentile(values, 0.75):7.2f}")
    print(f"    p90    = {percentile(values, 0.90):7.2f}")
    print(f"    p95    = {percentile(values, 0.95):7.2f}")
    print(f"    max    = {max(values):7.2f}")


def month_of(epoch_str):
    try:
        ts = int(float(epoch_str))
        return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m")
    except (TypeError, ValueError):
        return "?"


def report(rows):
    n = len(rows)
    print(f"\nLEDGER: {n} trades\n")

    # 1. Exit-reason mix
    print("=" * 64)
    print("1. EXIT-REASON MIX")
    print("=" * 64)
    reasons = Counter(r["exitReason"] for r in rows)
    for reason, c in reasons.most_common():
        print(f"  {reason:12s}  {c:4d}  ({c/n*100:5.1f}%)")

    # Pre-compute pip arrays
    realised, mfes, maes = [], [], []
    for r in rows:
        rp, mp, dp = trade_pips(r)
        realised.append(rp)
        mfes.append(mp)
        maes.append(dp)

    winners_idx = [i for i, p in enumerate(realised) if p > 0.05]
    losers_idx  = [i for i, p in enumerate(realised) if p < -0.05]
    bes_idx     = [i for i, p in enumerate(realised) if abs(p) <= 0.05]

    print(f"\n  decided W/L/BE: {len(winners_idx)} / {len(losers_idx)} "
          f"/ {len(bes_idx)}  "
          f"(WR among decided = {len(winners_idx)/max(1,len(winners_idx)+len(losers_idx))*100:.1f}%)")

    # 2. MFE distribution among winners
    print("\n" + "=" * 64)
    print("2. MFE DISTRIBUTION (winners only)")
    print("=" * 64)
    fmt_dist([mfes[i] for i in winners_idx], "MFE")

    # 3. MAE distribution among losers
    print("\n" + "=" * 64)
    print("3. MAE DISTRIBUTION (losers only, depth in pips)")
    print("=" * 64)
    fmt_dist([maes[i] for i in losers_idx], "MAE-depth")

    # 4. MFE clip = MFE - realised, for winners
    print("\n" + "=" * 64)
    print("4. MFE CLIP — pips left on the table per winner")
    print("=" * 64)
    print("  (clip = MFE_pips - realised_pips; positive = winner gave back)")
    clips = [mfes[i] - realised[i] for i in winners_idx]
    fmt_dist(clips, "Clip")
    if clips:
        avg_realised = stats.fmean(realised[i] for i in winners_idx)
        avg_mfe = stats.fmean(mfes[i] for i in winners_idx)
        print(f"  avg realised winner: {avg_realised:.2f} pips")
        print(f"  avg MFE on winner:   {avg_mfe:.2f} pips")
        print(f"  avg clip:            {stats.fmean(clips):.2f} pips "
              f"({stats.fmean(clips)/max(0.01,avg_mfe)*100:.1f}% of MFE)")
        # How many winners reached >X pips MFE
        for thresh in (5, 8, 10, 12, 15, 20):
            cnt = sum(1 for i in winners_idx if mfes[i] > thresh)
            print(f"  winners with MFE>{thresh:>2d} pips: {cnt}/{len(winners_idx)} "
                  f"({cnt/max(1,len(winners_idx))*100:.1f}%)")

    # 5. By-month breakdown
    print("\n" + "=" * 64)
    print("5. BY-MONTH BREAKDOWN")
    print("=" * 64)
    by_month = defaultdict(lambda: {"trades": 0, "wins": 0, "losses": 0,
                                    "bes": 0, "pnl": 0.0,
                                    "TP_HIT": 0, "TRAIL_HIT": 0,
                                    "SL_HIT": 0, "BE_HIT": 0,
                                    "mfes_winners": [], "clips": []})
    for i, r in enumerate(rows):
        m = month_of(r["entryTs"])
        b = by_month[m]
        b["trades"] += 1
        b[r["exitReason"]] = b.get(r["exitReason"], 0) + 1
        b["pnl"] += float(r["pnl_net_usd"])
        rp = realised[i]
        if rp > 0.05:
            b["wins"] += 1
            b["mfes_winners"].append(mfes[i])
            b["clips"].append(mfes[i] - realised[i])
        elif rp < -0.05:
            b["losses"] += 1
        else:
            b["bes"] += 1

    print(f"  {'month':7s}  {'n':>3s}  {'W':>3s} {'L':>3s} {'B':>3s}  "
          f"{'WR':>5s}  {'TP':>3s} {'TR':>3s} {'SL':>3s} {'BE':>3s}  "
          f"{'PnL':>8s}  {'avgMFE':>6s}  {'avgClip':>7s}")
    for m in sorted(by_month):
        b = by_month[m]
        wr = b["wins"] / max(1, b["wins"] + b["losses"]) * 100
        amfe = stats.fmean(b["mfes_winners"]) if b["mfes_winners"] else 0
        aclip = stats.fmean(b["clips"]) if b["clips"] else 0
        print(f"  {m:7s}  {b['trades']:>3d}  "
              f"{b['wins']:>3d} {b['losses']:>3d} {b['bes']:>3d}  "
              f"{wr:>4.1f}%  "
              f"{b.get('TP_HIT',0):>3d} {b.get('TRAIL_HIT',0):>3d} "
              f"{b.get('SL_HIT',0):>3d} {b.get('BE_HIT',0):>3d}  "
              f"${b['pnl']:>6.2f}  {amfe:>5.2f}p  {aclip:>5.2f}p")

    # 6. Lot-size distribution
    print("\n" + "=" * 64)
    print("6. LOT-SIZE DISTRIBUTION")
    print("=" * 64)
    sizes = [float(r["size"]) for r in rows]
    size_counter = Counter(round(s, 4) for s in sizes)
    print(f"  unique sizes: {len(size_counter)}")
    for s, c in sorted(size_counter.items()):
        print(f"    size={s:.4f}  count={c}  ({c/n*100:.1f}%)")
    print(f"  size mean   = {stats.fmean(sizes):.4f}")
    print(f"  size median = {stats.median(sizes):.4f}")
    print(f"  size max    = {max(sizes):.4f}")
    print(f"  size min    = {min(sizes):.4f}")

    # 7. Top clipped winners
    print("\n" + "=" * 64)
    print("7. TOP-10 MOST-CLIPPED WINNERS")
    print("=" * 64)
    clip_pairs = sorted(
        [(mfes[i] - realised[i], i) for i in winners_idx], reverse=True
    )[:10]
    print(f"  {'rank':>4s}  {'date':19s}  {'side':5s}  "
          f"{'real':>5s}  {'mfe':>5s}  {'clip':>5s}  {'reason':10s}")
    for rk, (clip, i) in enumerate(clip_pairs, 1):
        r = rows[i]
        ts = datetime.fromtimestamp(int(float(r["entryTs"])), tz=timezone.utc)
        print(f"  {rk:>4d}  {ts:%Y-%m-%d %H:%M:%S}  {r['side']:5s}  "
              f"{realised[i]:>4.1f}p  {mfes[i]:>4.1f}p  "
              f"{clip:>4.1f}p  {r['exitReason']:10s}")

    # 8. Adverse winners
    print("\n" + "=" * 64)
    print("8. WINNERS THAT DIPPED HARD BEFORE RECOVERING (top 10 by MAE)")
    print("=" * 64)
    adv = sorted(
        [(maes[i], i) for i in winners_idx], reverse=True
    )[:10]
    print(f"  {'rank':>4s}  {'date':19s}  {'side':5s}  "
          f"{'real':>5s}  {'mae':>5s}  {'reason':10s}")
    for rk, (mae_d, i) in enumerate(adv, 1):
        r = rows[i]
        ts = datetime.fromtimestamp(int(float(r["entryTs"])), tz=timezone.utc)
        print(f"  {rk:>4d}  {ts:%Y-%m-%d %H:%M:%S}  {r['side']:5s}  "
              f"{realised[i]:>4.1f}p  {mae_d:>4.1f}p  "
              f"{r['exitReason']:10s}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ledger",
                    default=str(REPO / "build" / "trail_trades_WF_baseline_verify.csv"),
                    help="path to trail_trades_*.csv ledger")
    args = ap.parse_args()

    path = Path(args.ledger)
    if not path.exists():
        print(f"FATAL: ledger not found: {path}")
        return 2
    rows = load_ledger(path)
    print(f"loaded {len(rows)} trades from {path}")
    report(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
