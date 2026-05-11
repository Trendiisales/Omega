#!/usr/bin/env python3
# =============================================================================
# scripts/verify_replay_one_day.py
# -----------------------------------------------------------------------------
# Independent Python reimplementation of honest_backtest_xauusd_v2 for ONE
# (day, config) -- used purely as a cross-check that the C++ harness is
# implementing the algorithm we think it is.
#
# Target: 2026-04-22 (XAUUSD), TP=5 / SL=16 / z=2.0, gated, latency=1,
# honest-fill model. Expected per Part 3 §3 and our re-run:
#     N=26, WR=84.6%, sum=+$65.59
#
# This script reads the raw L2 tick CSV, runs the same logic in Python,
# and prints PASS/FAIL on the sum to two decimals.
# =============================================================================

import csv
import math
import sys

PATH    = "/sessions/admiring-clever-franklin/mnt/omega_repo/data/l2_ticks_XAUUSD_2026-04-22.csv"
WINDOW          = 200
TP_PTS          = 5.0
SL_PTS          = 16.0
Z_THRESH        = 2.0
LATENCY_TICKS   = 1
COOLDOWN_TICKS  = 100
SIZE_LOTS       = 0.01
TICK_MULT       = 100.0
COMMISSION_RT   = 0.0
GATED           = True
MAX_SPREAD_PT   = 1.0
IMB_LONG        = 0.502
IMB_SHORT       = 0.498

EXPECTED_N      = 26
EXPECTED_SUM    = 65.59
EXPECTED_WR     = 0.846


def load(path):
    ticks = []
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            try:
                t = {
                    "ts_ms":  int(row["ts_ms"]),
                    "bid":    float(row["bid"]),
                    "ask":    float(row["ask"]),
                    "l2_imb": float(row.get("l2_imb", 0.5) or 0.5),
                    "regime": int(row.get("regime", 0) or 0),
                    "wd":     int(row.get("watchdog_dead", 0) or 0),
                }
                if t["ts_ms"] > 0 and t["bid"] > 0 and t["ask"] > 0:
                    ticks.append(t)
            except Exception:
                pass
    return ticks


def gated_filter(raw, t):
    if raw == 0: return 0
    if t["wd"] != 0: return 0
    if (t["ask"] - t["bid"]) > MAX_SPREAD_PT: return 0
    if t["regime"] not in (0, 2): return 0
    if raw == +1 and t["l2_imb"] < IMB_LONG: return 0
    if raw == -1 and t["l2_imb"] > IMB_SHORT: return 0
    return raw


def main():
    ticks = load(PATH)
    print(f"loaded {len(ticks)} ticks")
    N = len(ticks)
    # Rolling z-score state
    ring = [0.0] * WINDOW
    head = 0; n = 0; ssum = 0.0; ssum2 = 0.0

    trades = []
    pos_open = False
    pos_is_long = False
    pos_entry_px = 0.0
    pos_entry_ix = 0
    pending_entry_at = -1
    pending_entry_dir = 0
    pending_exit_at = -1
    pending_exit_reason = None
    pending_exit_trigger_px = 0.0
    next_allowed_entry = WINDOW

    for i, t in enumerate(ticks):
        # Resolve pending entry
        if pending_entry_at == i:
            pos_is_long = (pending_entry_dir == +1)
            # entry fill: honest = ask (long) / bid (short)
            pos_entry_px = t["ask"] if pos_is_long else t["bid"]
            pos_entry_ix = i
            pos_open = True
            pending_entry_at = -1
            pending_entry_dir = 0

        # Resolve pending exit
        if pos_open and pending_exit_at == i:
            # honest exit fill: always worst-side
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
        if GATED:
            fire = gated_filter(fire, t)

        # TP/SL trigger
        if pos_open and pending_exit_at == -1:
            if pos_is_long:
                if t["bid"] >= pos_entry_px + TP_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, N - 1)
                    pending_exit_reason = "TP"
                    pending_exit_trigger_px = pos_entry_px + TP_PTS
                elif t["bid"] <= pos_entry_px - SL_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, N - 1)
                    pending_exit_reason = "SL"
                    pending_exit_trigger_px = pos_entry_px - SL_PTS
            else:
                if t["ask"] <= pos_entry_px - TP_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, N - 1)
                    pending_exit_reason = "TP"
                    pending_exit_trigger_px = pos_entry_px - TP_PTS
                elif t["ask"] >= pos_entry_px + SL_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, N - 1)
                    pending_exit_reason = "SL"
                    pending_exit_trigger_px = pos_entry_px + SL_PTS

        # Open new position
        if (not pos_open and pending_entry_at == -1
                and i >= next_allowed_entry and fire != 0):
            pending_entry_at = min(i + LATENCY_TICKS, N - 1)
            pending_entry_dir = fire

    # EOD force-close if needed
    if pos_open:
        last = ticks[-1]
        fpx = last["bid"] if pos_is_long else last["ask"]
        pnl_units = (fpx - pos_entry_px) if pos_is_long else (pos_entry_px - fpx)
        net = pnl_units * SIZE_LOTS * TICK_MULT - COMMISSION_RT
        trades.append({
            "entry_ix": pos_entry_ix, "exit_ix": N - 1, "is_long": pos_is_long,
            "entry_px": pos_entry_px, "exit_px": fpx, "reason": "EOD", "net": net,
        })

    total = sum(tr["net"] for tr in trades)
    n_trades = len(trades)
    wins = sum(1 for tr in trades if tr["net"] > 0)
    wr = wins / n_trades if n_trades else 0.0
    print(f"Python replay: N={n_trades}  WR={wr*100:.1f}%  sum=${total:+.2f}")
    print(f"C++ harness  : N={EXPECTED_N}  WR={EXPECTED_WR*100:.1f}%  sum=${EXPECTED_SUM:+.2f}")
    pass_n   = n_trades == EXPECTED_N
    pass_sum = abs(total - EXPECTED_SUM) < 0.01
    print(f"Verdict: N {'MATCH' if pass_n else 'DIFFER'}   sum {'MATCH' if pass_sum else 'DIFFER'}")
    sys.exit(0 if (pass_n and pass_sum) else 1)


if __name__ == "__main__":
    main()
