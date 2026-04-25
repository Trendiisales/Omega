#!/usr/bin/env python3
"""
Tick-level simulation of BracketEngine on USTEC.F / US500.F April 22-24 ticks.

Mirrors the BracketEngine arm->pending->fill->manage lifecycle at tick resolution.
Applies S23 gates:
  S22c MAX_SL_DIST_PTS: blocks arming when range > max_sl
  S21  CONFIRM gate:    aborts fills that fail to reach confirm_pts in confirm_s
  S20  MAX_HOLD_SEC:    force-close filled positions at hold cap
  S19  continuous trail: BE arms at 60% of tp_dist (vs 40%)

Uses engine_init.hpp configure() values verbatim for arm logic:
  g_bracket_sp.configure(buf=0.25, lookback=120, RR=2.5, cooldown_ms=180000,
                         MIN_RANGE=12.0, ...)
  g_bracket_nq.configure(buf=0.75, lookback=180, RR=2.5, cooldown_ms=180000,
                         MIN_RANGE=42.0, ...)
S23 gates on top:
  SP:   max_sl=12.0  cfm_pts=6.0  cfm_s=30  max_hold=1800
  NQ:   max_sl=45.0  cfm_pts=22.0 cfm_s=30  max_hold=1800
"""
import csv
import sys
from collections import deque
from pathlib import Path

# ---- Per-symbol config (from engine_init.hpp configure() + S23 gates) ----
CONFIGS = {
    "USTEC.F": dict(
        buffer=0.75, lookback=180, rr=2.5, cooldown_ms=180000,
        min_range=42.0, confirm_move=0.05, confirm_timeout_ms=4000,
        min_hold_ms=10000,
        # S23
        max_sl_dist_pts=45.0, confirm_pts=22.0, confirm_secs=30,
        max_hold_sec=1800, trail_activation_pts=18.0, trail_distance_pts=12.0,
        be_trigger_frac=0.60,  # BracketEngine base-class (not IndexHybrid)
        # ATR guard disabled for indices
        atr_period=20, atr_range_k=0.0,
        # Max range cap
        max_range=90.0,
        # Price scale (for reporting)
        usd_per_pt=20.0,
    ),
    "US500.F": dict(
        buffer=0.25, lookback=120, rr=2.5, cooldown_ms=180000,
        min_range=12.0, confirm_move=0.05, confirm_timeout_ms=4000,
        min_hold_ms=10000,
        max_sl_dist_pts=12.0, confirm_pts=6.0, confirm_secs=30,
        max_hold_sec=1800, trail_activation_pts=5.0, trail_distance_pts=3.0,
        be_trigger_frac=0.60,
        atr_period=20, atr_range_k=0.0,
        max_range=25.0,
        usd_per_pt=50.0,
    ),
}


class BracketSim:
    """Minimal BracketEngine simulator.
    Mirrors on_tick logic: IDLE -> ARMED -> PENDING -> LIVE/ABORT.
    Only tracks ONE position at a time (per-symbol engine instance)."""

    IDLE, ARMED, PENDING, LIVE, COOLDOWN = range(5)

    def __init__(self, sym, cfg):
        self.sym = sym
        self.cfg = cfg
        self.phase = self.IDLE
        self.window = deque(maxlen=cfg["lookback"])
        self.bracket_high = 0.0
        self.bracket_low = 0.0
        self.armed_ts_ms = 0
        self.cooldown_until_ms = 0
        self.pos = None
        self.stats = dict(
            armed=0, blocked_wide_sl=0, blocked_no_range=0,
            pending_filled=0, pending_expired=0,
            confirm_aborted=0, live_trades=0,
            exit_sl_hit=0, exit_be_hit=0, exit_trail_hit=0, exit_max_hold=0,
            gross_pnl=0.0,
        )

    def tick(self, ts_ms, bid, ask):
        mid = (bid + ask) * 0.5
        cfg = self.cfg

        if self.phase == self.COOLDOWN:
            if ts_ms >= self.cooldown_until_ms:
                self.phase = self.IDLE
            else:
                return

        if self.phase == self.LIVE:
            self._manage(ts_ms, bid, ask, mid)
            return

        if self.phase == self.PENDING:
            self._handle_pending(ts_ms, bid, ask, mid)
            return

        # Build rolling window for structure lookback
        self.window.append(mid)
        if len(self.window) < cfg["lookback"]:
            return

        w_hi = max(self.window)
        w_lo = min(self.window)
        rng = w_hi - w_lo

        if self.phase == self.IDLE:
            if rng < cfg["min_range"] or rng > cfg["max_range"]:
                return

            # S22c gate: block if sl distance would exceed cap.
            # sl_dist for bracket = range (SL sits at the opposite extreme)
            if cfg["max_sl_dist_pts"] > 0 and rng > cfg["max_sl_dist_pts"]:
                self.stats["blocked_wide_sl"] += 1
                # Don't arm. Skip this tick.
                return

            # Arm bracket
            self.bracket_high = w_hi
            self.bracket_low = w_lo
            self.armed_ts_ms = ts_ms
            self.phase = self.ARMED
            self.stats["armed"] += 1
            return

        if self.phase == self.ARMED:
            # Price inside bracket: update bracket (tighten)
            if self.bracket_low < mid < self.bracket_high:
                self.bracket_high = min(self.bracket_high, w_hi)
                self.bracket_low = max(self.bracket_low, w_lo)
                return
            # Price crossed: enter PENDING (would place stop orders live).
            # Shadow: simulate fill at bracket price on first cross.
            self.phase = self.PENDING
            # Armed timestamp reset for fill tracking
            self.armed_ts_ms = ts_ms

    def _handle_pending(self, ts_ms, bid, ask, mid):
        cfg = self.cfg
        # Confirmation timeout (separate from S21 CONFIRM gate)
        if (ts_ms - self.armed_ts_ms) > cfg["confirm_timeout_ms"]:
            self.phase = self.IDLE
            self.stats["pending_expired"] += 1
            return
        # Check fills
        if ask >= self.bracket_high:
            self._fill(True, self.bracket_high, ts_ms)
        elif bid <= self.bracket_low:
            self._fill(False, self.bracket_low, ts_ms)

    def _fill(self, is_long, fill_px, ts_ms):
        cfg = self.cfg
        sl_dist = self.bracket_high - self.bracket_low
        tp_dist = sl_dist * cfg["rr"]
        sl = (fill_px - sl_dist) if is_long else (fill_px + sl_dist)
        tp = (fill_px + tp_dist) if is_long else (fill_px - tp_dist)
        self.pos = dict(
            is_long=is_long, entry=fill_px, sl=sl, tp=tp,
            entry_ts_ms=ts_ms, mfe=0.0, be_locked=False,
            sl_dist=sl_dist, tp_dist=tp_dist,
            # S21 CONFIRM phase: must reach confirm_pts in confirm_secs
            confirm_pending=(cfg["confirm_pts"] > 0 and cfg["confirm_secs"] > 0),
        )
        self.phase = self.LIVE
        self.stats["pending_filled"] += 1

    def _manage(self, ts_ms, bid, ask, mid):
        cfg = self.cfg
        p = self.pos
        move = (mid - p["entry"]) if p["is_long"] else (p["entry"] - mid)
        if move > p["mfe"]:
            p["mfe"] = move

        held_s = (ts_ms - p["entry_ts_ms"]) / 1000

        # S21 CONFIRM gate
        if p["confirm_pending"]:
            if p["mfe"] >= cfg["confirm_pts"]:
                p["confirm_pending"] = False
            elif held_s >= cfg["confirm_secs"]:
                # Abort at market
                exit_px = bid if p["is_long"] else ask
                self._close(ts_ms, exit_px, "BREAKOUT_FAIL_CONFIRM")
                self.stats["confirm_aborted"] += 1
                return

        # Min-hold gate (same as original)
        if held_s * 1000 < cfg["min_hold_ms"]:
            return

        # S20 MAX_HOLD_SEC
        if cfg["max_hold_sec"] > 0 and held_s >= cfg["max_hold_sec"]:
            exit_px = bid if p["is_long"] else ask
            self._close(ts_ms, exit_px, "MAX_HOLD_TIMEOUT")
            self.stats["exit_max_hold"] += 1
            return

        # S19 continuous trail
        if cfg["trail_activation_pts"] > 0 and p["mfe"] >= cfg["trail_activation_pts"]:
            # Ratchet SL
            if p["is_long"]:
                new_sl = p["entry"] + p["mfe"] - cfg["trail_distance_pts"]
                if new_sl > p["sl"]:
                    p["sl"] = new_sl
            else:
                new_sl = p["entry"] - p["mfe"] + cfg["trail_distance_pts"]
                if new_sl < p["sl"]:
                    p["sl"] = new_sl

        # Legacy BE lock at be_trigger_frac of tp_dist (kept for symmetry with gold)
        if (not p["be_locked"] and p["mfe"] >= cfg["be_trigger_frac"] * p["tp_dist"]):
            p["be_locked"] = True
            if p["is_long"] and p["entry"] > p["sl"]:
                p["sl"] = max(p["sl"], p["entry"])
            elif not p["is_long"] and p["entry"] < p["sl"]:
                p["sl"] = min(p["sl"], p["entry"])

        # SL hit check
        sl_hit = (bid <= p["sl"]) if p["is_long"] else (ask >= p["sl"])
        if sl_hit:
            exit_px = p["sl"]  # fill at SL (optimistic)
            # Label: SL_HIT / BE_HIT / TRAIL_HIT
            if p["is_long"] and p["sl"] > p["entry"] + 0.01:
                label = "TRAIL_HIT"
            elif not p["is_long"] and p["sl"] < p["entry"] - 0.01:
                label = "TRAIL_HIT"
            elif p["be_locked"]:
                label = "BE_HIT"
            else:
                label = "SL_HIT"
            self._close(ts_ms, exit_px, label)
            if label == "SL_HIT":
                self.stats["exit_sl_hit"] += 1
            elif label == "BE_HIT":
                self.stats["exit_be_hit"] += 1
            else:
                self.stats["exit_trail_hit"] += 1
            return

        # TP hit check
        tp_hit = (ask >= p["tp"]) if p["is_long"] else (bid <= p["tp"])
        if tp_hit:
            self._close(ts_ms, p["tp"], "TP_HIT")
            self.stats["exit_trail_hit"] += 1

    def _close(self, ts_ms, exit_px, reason):
        p = self.pos
        pnl_pts = (exit_px - p["entry"]) if p["is_long"] else (p["entry"] - exit_px)
        pnl_usd = pnl_pts * self.cfg["usd_per_pt"] * 0.01  # 0.01 lot nominal
        self.stats["gross_pnl"] += pnl_usd
        self.stats["live_trades"] += 1
        self.pos = None
        self.phase = self.COOLDOWN
        self.cooldown_until_ms = ts_ms + self.cfg["cooldown_ms"]


def run_file(sym, cfg, path):
    sim = BracketSim(sym, cfg)
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            sim.tick(int(row["ts_ms"]), float(row["bid"]), float(row["ask"]))
    return sim.stats


def main():
    import glob
    for sym in ("USTEC.F", "US500.F"):
        cfg = CONFIGS[sym]
        sym_short = "USTEC" if "USTEC" in sym else "US500"
        files = sorted(glob.glob(f"/home/claude/s23/l2_ticks_{sym_short}_*.csv"))
        print("="*78)
        print(f"{sym}  -- S23 gates applied to {len(files)} days of real tick data")
        print(f"  max_sl={cfg['max_sl_dist_pts']}  cfm={cfg['confirm_pts']}/{cfg['confirm_secs']}s"
              f"  max_hold={cfg['max_hold_sec']}s  trail={cfg['trail_activation_pts']}/{cfg['trail_distance_pts']}")
        print("="*78)
        total = dict(armed=0, blocked_wide_sl=0, pending_filled=0, pending_expired=0,
                     confirm_aborted=0, live_trades=0, exit_sl_hit=0, exit_be_hit=0,
                     exit_trail_hit=0, exit_max_hold=0, gross_pnl=0.0)
        for fp in files:
            stats = run_file(sym, cfg, fp)
            day = Path(fp).stem.split("_")[-1]
            print(f"  {day}: armed={stats['armed']:3d} blocked_wide={stats['blocked_wide_sl']:3d}"
                  f"  filled={stats['pending_filled']:2d} aborted={stats['confirm_aborted']:2d}"
                  f"  trades={stats['live_trades']:2d} (SL={stats['exit_sl_hit']} BE={stats['exit_be_hit']}"
                  f" TRAIL={stats['exit_trail_hit']} HOLD={stats['exit_max_hold']})"
                  f"  pnl=${stats['gross_pnl']:+.2f}")
            for k in total: total[k] += stats[k]
        print(f"  TOTAL: armed={total['armed']} blocked_by_S22c={total['blocked_wide_sl']}"
              f" aborted_by_S21={total['confirm_aborted']} closed_by_S20={total['exit_max_hold']}")
        print(f"         live_trades={total['live_trades']} gross_pnl=${total['gross_pnl']:+.2f}")
        print()


if __name__ == "__main__":
    main()
