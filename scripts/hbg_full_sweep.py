#!/usr/bin/env python3
"""
HybridBracketGold full multi-day sweep.

Auto-discovers all available XAUUSD l2_ticks CSVs across known directories,
simulates the compression-breakout engine with multiple parameter combinations,
and reports aggregate PnL per combo.

Engine model (simplified from GoldHybridBracketEngine):
    - 600-tick rolling window
    - IDLE: range in [MIN_RANGE, MAX_RANGE]  ->  ARM
    - ARMED: tick > bracket_high + BUFFER    ->  LONG entry
             tick < bracket_low  - BUFFER    ->  SHORT entry
    - LIVE: SL at opposite range extreme; TP at RR * SL_dist
    - Trail: after MFE >= TRAIL_ARM, lock at BE; after MFE >= TRAIL_ARM*2,
             trail TRAIL_DIST behind peak
    - Cooldown: skip new ARM for COOLDOWN_SEC after SL_HIT

Stdlib only. Run from repo root:
    python3 scripts/hbg_full_sweep.py
"""

import csv
import os
import sys
from collections import defaultdict
from datetime import datetime, timezone
from glob import glob

# --- File discovery ---------------------------------------------------------
SEARCH_PATHS = [
    "/Users/jo/Tick/today/l2_ticks_XAUUSD_*.csv",
    "/Users/jo/Tick/l2_vps/l2_bundle/l2_ticks_XAUUSD_*.csv",
    "/Users/jo/Tick/l2_vps/l2_bundle/l2_ticks_2026-*.csv",  # legacy XAUUSD-only
    "/Users/jo/Tick/l2_data/l2_ticks_2026-*.csv",
    "/Users/jo/Tick/Omega/backtest/l2_ticks_XAUUSD_*.csv",
    "/Users/jo/Tick/Omega/backtest/l2_ticks_2026-*.csv",
    "/Users/jo/omega_repo/backtest/l2_ticks_2026-*.csv",
    "/Users/jo/omega_repo/data/l2_ticks_2026-*.csv",
]


def discover_csvs():
    seen = {}
    for pattern in SEARCH_PATHS:
        for path in glob(pattern):
            # Skip non-XAUUSD index/FX files
            if "l2_ticks_US500" in path or "l2_ticks_USTEC" in path or "l2_ticks_NAS100" in path:
                continue
            # Extract date from filename
            base = os.path.basename(path)
            date_part = ""
            for token in base.replace(".csv", "").split("_"):
                if len(token) == 10 and token[4] == "-" and token[7] == "-":
                    date_part = token
                    break
            if not date_part:
                continue
            # Prefer larger/newer file if duplicates per date
            existing = seen.get(date_part)
            if existing is None or os.path.getsize(path) > os.path.getsize(existing):
                seen[date_part] = path
    return [seen[d] for d in sorted(seen.keys())]


# --- Tick parser ------------------------------------------------------------
def load_ticks(path):
    """Return list of (ts_ms, mid, bid, ask). Tolerant to legacy / new headers."""
    out = []
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header:
            return []
        idx = {name: i for i, name in enumerate(header)}
        ts_i = idx.get("ts_ms", 0)
        mid_i = idx.get("mid", -1)
        bid_i = idx.get("bid", -1)
        ask_i = idx.get("ask", -1)
        for row in reader:
            if len(row) <= ts_i:
                continue
            try:
                ts_ms = int(row[ts_i])
                bid = float(row[bid_i]) if bid_i >= 0 and bid_i < len(row) else 0.0
                ask = float(row[ask_i]) if ask_i >= 0 and ask_i < len(row) else 0.0
                if mid_i >= 0 and mid_i < len(row):
                    mid = float(row[mid_i])
                else:
                    mid = (bid + ask) * 0.5
                if bid <= 0.0 and ask <= 0.0:
                    continue
                if mid <= 0.0:
                    mid = (bid + ask) * 0.5
                out.append((ts_ms, mid, bid, ask))
            except (ValueError, IndexError):
                continue
    return out


# --- Engine simulator -------------------------------------------------------
class HBGSim:
    IDLE, ARMED, LIVE = range(3)

    def __init__(self, *, min_range, max_range, buffer, lookback,
                 rr, trail_arm, trail_dist, cooldown_sec):
        self.cfg = dict(
            min_range=min_range, max_range=max_range, buffer=buffer,
            lookback=lookback, rr=rr,
            trail_arm=trail_arm, trail_dist=trail_dist,
            cooldown_sec=cooldown_sec,
        )
        self.phase = self.IDLE
        self.window = []           # list of mids
        self.b_hi = self.b_lo = 0.0
        self.cooldown_until_ms = 0
        self.pos = None
        self.trades = []           # list of dicts

    def _arm(self):
        cfg = self.cfg
        rng = max(self.window) - min(self.window)
        if cfg["min_range"] <= rng <= cfg["max_range"]:
            self.b_hi = max(self.window)
            self.b_lo = min(self.window)
            self.phase = self.ARMED

    def _enter_long(self, ts_ms, mid):
        cfg = self.cfg
        sl_dist = self.b_hi - self.b_lo + cfg["buffer"]
        self.pos = dict(
            side="LONG", entry=mid, sl=mid - sl_dist,
            tp=mid + cfg["rr"] * sl_dist,
            mfe=0.0, peak=mid, entry_ms=ts_ms,
            be_armed=False, trail_armed=False,
        )
        self.phase = self.LIVE

    def _enter_short(self, ts_ms, mid):
        cfg = self.cfg
        sl_dist = self.b_hi - self.b_lo + cfg["buffer"]
        self.pos = dict(
            side="SHORT", entry=mid, sl=mid + sl_dist,
            tp=mid - cfg["rr"] * sl_dist,
            mfe=0.0, peak=mid, entry_ms=ts_ms,
            be_armed=False, trail_armed=False,
        )
        self.phase = self.LIVE

    def _close(self, ts_ms, mid, reason):
        p = self.pos
        if p["side"] == "LONG":
            pnl_pts = mid - p["entry"]
        else:
            pnl_pts = p["entry"] - mid
        # Apply cost: ~0.22 USD round-trip at 0.01 lot for gold
        pnl_usd = pnl_pts * 1.0 - 0.22  # 1 USD/pt at 0.01 lot
        self.trades.append(dict(
            entry_ms=p["entry_ms"], exit_ms=ts_ms, side=p["side"],
            entry=p["entry"], exit=mid, reason=reason,
            pnl_pts=pnl_pts, pnl_usd=pnl_usd,
        ))
        if reason == "SL":
            self.cooldown_until_ms = ts_ms + self.cfg["cooldown_sec"] * 1000
        self.pos = None
        self.phase = self.IDLE

    def on_tick(self, ts_ms, mid, bid, ask):
        cfg = self.cfg
        self.window.append(mid)
        if len(self.window) > cfg["lookback"]:
            self.window.pop(0)

        if self.phase == self.LIVE:
            p = self.pos
            # Update MFE / peak
            if p["side"] == "LONG":
                if mid > p["peak"]:
                    p["peak"] = mid
                p["mfe"] = max(p["mfe"], p["peak"] - p["entry"])
            else:
                if mid < p["peak"]:
                    p["peak"] = mid
                p["mfe"] = max(p["mfe"], p["entry"] - p["peak"])

            # Stage 1: BE arm
            if not p["be_armed"] and p["mfe"] >= cfg["trail_arm"]:
                if p["side"] == "LONG":
                    p["sl"] = max(p["sl"], p["entry"])
                else:
                    p["sl"] = min(p["sl"], p["entry"])
                p["be_armed"] = True

            # Stage 2: trail
            if not p["trail_armed"] and p["mfe"] >= cfg["trail_arm"] * 2:
                p["trail_armed"] = True

            if p["trail_armed"]:
                if p["side"] == "LONG":
                    p["sl"] = max(p["sl"], p["peak"] - cfg["trail_dist"])
                else:
                    p["sl"] = min(p["sl"], p["peak"] + cfg["trail_dist"])

            # Exits
            if p["side"] == "LONG":
                if bid <= p["sl"]:
                    reason = "TRAIL" if p["trail_armed"] else ("BE" if p["be_armed"] else "SL")
                    self._close(ts_ms, p["sl"], reason)
                elif ask >= p["tp"]:
                    self._close(ts_ms, p["tp"], "TP")
            else:
                if ask >= p["sl"]:
                    reason = "TRAIL" if p["trail_armed"] else ("BE" if p["be_armed"] else "SL")
                    self._close(ts_ms, p["sl"], reason)
                elif bid <= p["tp"]:
                    self._close(ts_ms, p["tp"], "TP")
            return

        if ts_ms < self.cooldown_until_ms:
            return

        if self.phase == self.IDLE:
            if len(self.window) >= cfg["lookback"]:
                self._arm()
            return

        if self.phase == self.ARMED:
            # Re-evaluate range; if it widens beyond max, abandon arm
            rng = max(self.window) - min(self.window)
            if rng > cfg["max_range"] or rng < cfg["min_range"] * 0.5:
                self.phase = self.IDLE
                return
            # Update bracket if range tightened/shifted
            self.b_hi = max(self.window)
            self.b_lo = min(self.window)
            # Check breakout
            if mid > self.b_hi + cfg["buffer"]:
                self._enter_long(ts_ms, ask)
            elif mid < self.b_lo - cfg["buffer"]:
                self._enter_short(ts_ms, bid)


def run_sim(ticks, params):
    """Run engine over ticks. Return summary dict."""
    sim = HBGSim(**params)
    for ts, mid, bid, ask in ticks:
        sim.on_tick(ts, mid, bid, ask)
    trades = sim.trades
    if not trades:
        return dict(n=0, wr=0.0, net=0.0, by_reason={})
    wins = [t for t in trades if t["pnl_usd"] > 0]
    by_reason = defaultdict(int)
    for t in trades:
        by_reason[t["reason"]] += 1
    return dict(
        n=len(trades),
        wr=100.0 * len(wins) / len(trades),
        net=sum(t["pnl_usd"] for t in trades),
        gross=sum(t["pnl_pts"] for t in trades),
        by_reason=dict(by_reason),
    )


# --- Sweep -----------------------------------------------------------------
BASE = dict(
    min_range=1.5, max_range=12.0, buffer=0.8, lookback=600,
    rr=4.0, trail_arm=2.5, trail_dist=0.8, cooldown_sec=90,
)


def sweep(files):
    combos = []
    for min_r in [1.5, 2.5, 3.0, 4.0, 5.0]:
        for cd in [90, 300, 600, 900, 1200, 1800]:
            combos.append(dict(BASE, min_range=min_r, cooldown_sec=cd))

    print(f"Loading {len(files)} files...", file=sys.stderr)
    ticks_by_day = []
    for path in files:
        ticks = load_ticks(path)
        if ticks:
            day = os.path.basename(path).split("_")[-1].replace(".csv", "")
            ticks_by_day.append((day, path, ticks))
            print(f"  {day}: {len(ticks):,} ticks", file=sys.stderr)
        else:
            print(f"  [WARN] {path}: no ticks parsed", file=sys.stderr)

    print(f"\n{'='*92}", file=sys.stderr)
    print(f"Running {len(combos)} param combos x {len(ticks_by_day)} days = "
          f"{len(combos)*len(ticks_by_day)} sims...", file=sys.stderr)
    print(f"{'='*92}\n", file=sys.stderr)

    # Run baseline + sweep, aggregate per combo
    rows = []
    for combo in combos:
        agg_n = 0
        agg_net = 0.0
        agg_wr_w = 0
        agg_reasons = defaultdict(int)
        per_day = []
        for day, _path, ticks in ticks_by_day:
            r = run_sim(ticks, combo)
            agg_n += r["n"]
            agg_net += r["net"]
            agg_wr_w += int(r["wr"] / 100.0 * r["n"])
            for k, v in r["by_reason"].items():
                agg_reasons[k] += v
            per_day.append((day, r["n"], r["net"]))
        wr = 100.0 * agg_wr_w / agg_n if agg_n else 0.0
        rows.append(dict(
            min_range=combo["min_range"], cooldown_sec=combo["cooldown_sec"],
            n=agg_n, wr=wr, net=agg_net,
            reasons=dict(agg_reasons),
            per_day=per_day,
        ))

    # Print aggregate table
    rows.sort(key=lambda r: -r["net"])
    print("AGGREGATE SWEEP (all days)")
    print("=" * 92)
    print(f"{'min_range':>10} {'cooldown_s':>11} {'trades':>8} {'WR%':>6} "
          f"{'net_USD':>10}  {'reasons'}")
    print("-" * 92)
    for r in rows:
        rstr = " ".join(f"{k}={v}" for k, v in sorted(r["reasons"].items()))
        print(f"{r['min_range']:>10.1f} {r['cooldown_sec']:>11d} {r['n']:>8d} "
              f"{r['wr']:>6.1f} {r['net']:>+10.2f}  {rstr}")

    # Print baseline detail
    base_row = next(r for r in rows
                    if r["min_range"] == 1.5 and r["cooldown_sec"] == 90)
    print("\nBASELINE (min_range=1.5 cooldown=90s) per-day breakdown:")
    print(f"{'date':>12} {'trades':>8} {'net':>10}")
    print("-" * 32)
    for day, n, net in base_row["per_day"]:
        print(f"{day:>12} {n:>8d} {net:>+10.2f}")
    print(f"{'TOTAL':>12} {base_row['n']:>8d} {base_row['net']:>+10.2f}")

    # Print best combo detail
    best = rows[0]
    print(f"\nBEST COMBO (min_range={best['min_range']} cooldown={best['cooldown_sec']}s) per-day:")
    print(f"{'date':>12} {'trades':>8} {'net':>10}")
    print("-" * 32)
    for day, n, net in best["per_day"]:
        print(f"{day:>12} {n:>8d} {net:>+10.2f}")
    print(f"{'TOTAL':>12} {best['n']:>8d} {best['net']:>+10.2f}")

    # Improvement
    print(f"\nIMPROVEMENT over baseline: {best['net'] - base_row['net']:+.2f} USD "
          f"({best['n']} vs {base_row['n']} trades)")


def main():
    files = discover_csvs()
    if not files:
        print("No XAUUSD CSV files found in known paths.", file=sys.stderr)
        sys.exit(1)
    print(f"Discovered {len(files)} XAUUSD CSV files", file=sys.stderr)
    sweep(files)


if __name__ == "__main__":
    main()
