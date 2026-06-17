#!/usr/bin/env python3
"""
aurora_gate_eval.py -- did the AuroraGate's verdicts actually help?

Reads logs/aurora/aurora_gate_history.csv (appended each snapshot by
ibkr/aurora_snapshot.py) and scores every per-symbol verdict against what price
did NEXT, at several horizons. The question it answers: does the gate ALLOW the
directions that go on to win and BLOCK the ones that go on to lose?

Per symbol + horizon it reports, for each side (long/short):
  - blocked rows  : mean forward return in that direction (bps)  [want < 0]
  - allowed rows  : mean forward return in that direction (bps)  [want > 0]
  - DISCRIMINATION = allowed_mean - blocked_mean (bps)           [want > 0]

DISCRIMINATION > 0 (sustained, with enough n) = the gate separates winners from
losers -> it is adding value -> a candidate to justify a per-engine size bump.
DISCRIMINATION ~ 0 = the gate is just noise (no size bump warranted).

Forward return uses the futures (MGC/NQ) mid stored in the history -- the series
the gate is computed in; equals spot/index forward return (basis ~constant).

Usage:
  python tools/analytics/aurora_gate_eval.py [--hist PATH] [--horizons 60,240,1440]
    --hist       default logs/aurora/aurora_gate_history.csv
    --horizons   comma list of minutes (default 60,240,1440 = 1h/4h/1d)
"""
import argparse, csv, os, sys
from collections import defaultdict


def load(path):
    rows = defaultdict(list)   # sym -> list of dicts sorted by stamp
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        r = csv.DictReader(f)
        for d in r:
            try:
                rows[d["sym"]].append({
                    "ms":  int(d["stamp_ms"]),
                    "px":  float(d["price"]),
                    "al":  int(d["allow_long"]),
                    "as":  int(d["allow_short"]),
                })
            except (ValueError, KeyError):
                continue
    for s in rows:
        rows[s].sort(key=lambda x: x["ms"])
    return rows


def fwd_price(series, i, horizon_ms):
    """price of the first row at >= series[i].ms + horizon, within 1.5x window."""
    t0 = series[i]["ms"]
    target = t0 + horizon_ms
    for j in range(i + 1, len(series)):
        if series[j]["ms"] >= target:
            # accept only if not absurdly far past the target (data gap guard)
            if series[j]["ms"] <= target + horizon_ms // 2:
                return series[j]["px"]
            return None
    return None


def mean_bps(vals):
    return (sum(vals) / len(vals) * 1e4) if vals else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hist", default="logs/aurora/aurora_gate_history.csv")
    ap.add_argument("--horizons", default="60,240,1440")
    a = ap.parse_args()
    horizons = [int(x) for x in a.horizons.split(",") if x.strip()]

    rows = load(a.hist)
    if not rows:
        print(f"no history yet at {a.hist} -- gate just deployed; re-run once "
              f"aurora_snapshot has accumulated rows (one per snapshot interval).")
        return
    total = sum(len(v) for v in rows.values())
    print(f"# aurora_gate_eval  |  {a.hist}  |  {total} snapshot-rows  "
          f"|  horizons(min)={horizons}\n")

    for sym in sorted(rows):
        series = rows[sym]
        span_h = (series[-1]["ms"] - series[0]["ms"]) / 3.6e6 if len(series) > 1 else 0
        print(f"=== {sym}  ({len(series)} rows, {span_h:.1f}h span) ===")
        print(f"{'horizon':>8} {'side':>5} {'nBlk':>5} {'blk_bps':>8} "
              f"{'nAllow':>6} {'alw_bps':>8} {'DISCRIM':>8}  verdict")
        for H in horizons:
            hms = H * 60_000
            for side, flag in (("long", "al"), ("short", "as")):
                blk, alw = [], []
                for i in range(len(series)):
                    pf = fwd_price(series, i, hms)
                    if pf is None:
                        continue
                    ret = (pf - series[i]["px"]) / series[i]["px"]
                    dir_ret = ret if side == "long" else -ret   # return in the side's favor
                    (blk if series[i][flag] == 0 else alw).append(dir_ret)
                bb, ab = mean_bps(blk), mean_bps(alw)
                disc = (ab - bb) if (bb is not None and ab is not None) else None
                v = ("adds-value" if (disc is not None and disc > 5) else
                     "noise" if (disc is not None and abs(disc) <= 5) else
                     "neg!" if disc is not None else "n/a")
                print(f"{H:>8} {side:>5} {len(blk):>5} "
                      f"{('%+.1f'%bb) if bb is not None else '   --':>8} "
                      f"{len(alw):>6} {('%+.1f'%ab) if ab is not None else '   --':>8} "
                      f"{('%+.1f'%disc) if disc is not None else '   --':>8}  {v}")
        print()

    print("READ: blk_bps = mean forward return (bps) of BLOCKED verdicts in the\n"
          "      side's direction (want NEGATIVE -- blocked moves that would lose).\n"
          "      alw_bps = same for ALLOWED verdicts (want POSITIVE).\n"
          "      DISCRIM = alw - blk (want POSITIVE + growing). 'adds-value' >5bps.\n"
          "SIZE BUMP: justified only when DISCRIM is positive across horizons, both\n"
          "      sides where applicable, with nBlk+nAllow each in the hundreds --\n"
          "      and then via the adaptive-risk path, per-engine, gradually.")


if __name__ == "__main__":
    main()
