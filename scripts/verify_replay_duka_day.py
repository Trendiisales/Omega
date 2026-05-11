#!/usr/bin/env python3
# =============================================================================
# scripts/verify_replay_duka_day.py
# -----------------------------------------------------------------------------
# S28 — Independent Python reimplementation of honest_backtest_xauusd_v2 for
# ONE (Dukascopy day, ungated config). Used as a cross-check that the C++
# harness numbers we just relied on for the verdict are reproducible from
# scratch in a different language.
#
# Target: outputs/duka_xauusd_daily/2024-05-22.csv
#         TP=12 / SL=16 / z=2.0, ungated, latency=1, window=200
#         honest-fill (next-tick worst-side).
# Expected from the C++ wide-grid CSV (run today):
#         N=4, WR=25.0%, sum=-$26.47
#
# Exits 0 on (N, sum-to-cents) match, 1 otherwise.
# =============================================================================

import csv
import math
import sys
from pathlib import Path

REPO_ROOT       = Path(__file__).resolve().parents[1]
TICK_PATH       = REPO_ROOT / "outputs" / "duka_xauusd_daily" / "2024-05-22.csv"

WINDOW          = 200
TP_PTS          = 12.0
SL_PTS          = 16.0
Z_THRESH        = 2.0
LATENCY_TICKS   = 1
COOLDOWN_TICKS  = 100
SIZE_LOTS       = 0.01
TICK_MULT       = 100.0
COMMISSION_RT   = 0.0  # gross; matches C++ harness default
GATED           = False  # Dukascopy has no L2/regime/watchdog

EXPECTED_N      = 4
EXPECTED_WR     = 25.0
EXPECTED_SUM    = -26.47   # honest fill


def load(path):
    ticks = []
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            try:
                t = {
                    "ts_ms": int(row["ts_ms"]),
                    "bid":   float(row["bid"]),
                    "ask":   float(row["ask"]),
                }
                if t["ts_ms"] > 0 and t["bid"] > 0 and t["ask"] > 0:
                    ticks.append(t)
            except Exception:
                pass
    return ticks


def main():
    ticks = load(TICK_PATH)
    print(f"loaded {len(ticks)} ticks from {TICK_PATH.name}")
    N_TICKS = len(ticks)

    ring = [0.0] * WINDOW
    head = 0
    n = 0
    ssum = 0.0
    ssum2 = 0.0

    trades = []
    pos_open = False
    pos_is_long = False
    pos_entry_px = 0.0
    pos_entry_ix = 0
    pending_entry_at = -1
    pending_entry_dir = 0
    pending_exit_at = -1
    pending_exit_reason = None
    next_allowed_entry = WINDOW

    for i, t in enumerate(ticks):
        # Resolve pending entry
        if pending_entry_at == i:
            pos_is_long = (pending_entry_dir == +1)
            pos_entry_px = t["ask"] if pos_is_long else t["bid"]
            pos_entry_ix = i
            pos_open = True
            pending_entry_at = -1
            pending_entry_dir = 0

        # Resolve pending exit (honest fill: worst-side at exit tick)
        if pos_open and pending_exit_at == i:
            fpx = t["bid"] if pos_is_long else t["ask"]
            pnl_units = (fpx - pos_entry_px) if pos_is_long else (pos_entry_px - fpx)
            net = pnl_units * SIZE_LOTS * TICK_MULT - COMMISSION_RT
            trades.append({
                "entry_ix": pos_entry_ix,
                "exit_ix":  i,
                "is_long":  pos_is_long,
                "entry_px": pos_entry_px,
                "exit_px":  fpx,
                "reason":   pending_exit_reason,
                "net":      net,
            })
            pos_open = False
            next_allowed_entry = i + COOLDOWN_TICKS
            pending_exit_at = -1

        # Update z-score (mid-based)
        mid = 0.5 * (t["bid"] + t["ask"])
        if n == WINDOW:
            old = ring[head]
            ssum -= old
            ssum2 -= old * old
        ring[head] = mid
        ssum += mid
        ssum2 += mid * mid
        head = (head + 1) % WINDOW
        if n < WINDOW:
            n += 1
            fire = 0
        else:
            m = ssum / WINDOW
            var = max(ssum2 / WINDOW - m * m, 1e-12)
            sd = math.sqrt(var)
            z = (mid - m) / sd
            if z > Z_THRESH:    fire = -1
            elif z < -Z_THRESH: fire = +1
            else:               fire = 0

        # No gates (ungated) — fire passes through

        # TP/SL trigger
        if pos_open and pending_exit_at == -1:
            if pos_is_long:
                if t["bid"] >= pos_entry_px + TP_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, N_TICKS - 1)
                    pending_exit_reason = "TP"
                elif t["bid"] <= pos_entry_px - SL_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, N_TICKS - 1)
                    pending_exit_reason = "SL"
            else:
                if t["ask"] <= pos_entry_px - TP_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, N_TICKS - 1)
                    pending_exit_reason = "TP"
                elif t["ask"] >= pos_entry_px + SL_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, N_TICKS - 1)
                    pending_exit_reason = "SL"

        # Open new position
        if (not pos_open and pending_entry_at == -1
                and i >= next_allowed_entry and fire != 0):
            pending_entry_at = min(i + LATENCY_TICKS, N_TICKS - 1)
            pending_entry_dir = fire

    # EOD force-close (matches harness)
    if pos_open:
        last = ticks[-1]
        fpx = last["bid"] if pos_is_long else last["ask"]
        pnl_units = (fpx - pos_entry_px) if pos_is_long else (pos_entry_px - fpx)
        net = pnl_units * SIZE_LOTS * TICK_MULT - COMMISSION_RT
        trades.append({
            "entry_ix": pos_entry_ix, "exit_ix": N_TICKS - 1, "is_long": pos_is_long,
            "entry_px": pos_entry_px, "exit_px": fpx, "reason": "EOD", "net": net,
        })

    total = sum(tr["net"] for tr in trades)
    n_trades = len(trades)
    wins = sum(1 for tr in trades if tr["net"] > 0)
    wr = (wins / n_trades * 100) if n_trades else 0.0
    print(f"Python replay : N={n_trades}  WR={wr:.1f}%  sum=${total:+.2f}")
    print(f"C++ harness   : N={EXPECTED_N}  WR={EXPECTED_WR:.1f}%  sum=${EXPECTED_SUM:+.2f}")
    pass_n = (n_trades == EXPECTED_N)
    pass_sum = abs(total - EXPECTED_SUM) < 0.01
    print(f"Verdict       : N {'MATCH' if pass_n else 'DIFFER'}   "
          f"sum {'MATCH' if pass_sum else 'DIFFER'}")
    if not (pass_n and pass_sum):
        # Helpful detail dump
        for k, tr in enumerate(trades, 1):
            print(f"  trade {k}: {'LONG' if tr['is_long'] else 'SHORT'} "
                  f"entry@{tr['entry_ix']}={tr['entry_px']:.3f} "
                  f"exit@{tr['exit_ix']}={tr['exit_px']:.3f} "
                  f"reason={tr['reason']} net=${tr['net']:+.2f}")
    sys.exit(0 if (pass_n and pass_sum) else 1)


if __name__ == "__main__":
    main()
