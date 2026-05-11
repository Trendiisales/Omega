#!/usr/bin/env python3
"""
scripts/verify_extreme_asia_one_day.py

S29 Phase 5 — independent Python replay verification of the C++ harness
on the extreme_asia PRIMARY edge candidate, for ONE positive day:

  TP=30, SL=15, z=2.0, --session 0-7 (Asia 00-07 UTC), latency=1, w=200,
  cooldown=100, size=0.01, tick_mult=100, fill=honest, ungated

Day: 2025-04-10 (top winning day for this config in Phase 6 results,
                 ~$60 net contribution before commission).

Reimplements the harness signal + simulation in pure Python. Compares N
(trade count), WR (win rate %), and sum_usd (gross sum) to the C++
harness output. If they match to the cent, the C++ result for this
candidate is independently confirmed (modulo sample size — verifying
ONE day verifies the sim logic, not the multi-day aggregate).
"""
from __future__ import annotations

import csv
import datetime as dt
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DAY_FILE  = REPO_ROOT / "outputs" / "duka_xauusd_daily" / "2025-04-10.csv"
HARNESS   = REPO_ROOT / "backtest" / "honest_backtest_xauusd_v2_s29"

# Config
TP_PTS         = 30.0
SL_PTS         = 15.0
Z_THRESH       = 2.0
WINDOW         = 200
LATENCY_TICKS  = 1
COOLDOWN_TICKS = 100
SIZE_LOTS      = 0.01
TICK_MULT      = 100.0
SESSION_START  = 0    # UTC inclusive
SESSION_END    = 7    # UTC exclusive
COMMISSION_RT  = 0.0  # per-trade commission; we'll compute net separately


def utc_hour(ts_ms: int) -> int:
    return dt.datetime.fromtimestamp(ts_ms / 1000.0, tz=dt.timezone.utc).hour


def session_allows(ts_ms: int) -> bool:
    h = utc_hour(ts_ms)
    return SESSION_START <= h < SESSION_END


def python_replay(day_path: Path):
    ticks = []
    with day_path.open("r") as f:
        reader = csv.reader(f)
        header = next(reader)
        ix_ts  = header.index("ts_ms")
        ix_bid = header.index("bid")
        ix_ask = header.index("ask")
        for row in reader:
            try:
                ts = int(row[ix_ts])
                bid = float(row[ix_bid])
                ask = float(row[ix_ask])
            except (ValueError, IndexError):
                continue
            if ts > 0 and bid > 0 and ask > 0:
                ticks.append((ts, bid, ask))

    n_ticks = len(ticks)
    print(f"[python] loaded {n_ticks} ticks")

    # Z-score state (rolling window on midprice)
    ring  = [0.0] * WINDOW
    head  = 0
    s     = 0.0
    s2    = 0.0
    n     = 0
    last_z = 0.0
    warm  = False

    pos_open      = False
    pos_is_long   = False
    pos_entry_px  = 0.0
    pos_entry_ix  = 0
    pending_entry_at  = -1
    pending_entry_dir = 0
    pending_exit_at   = -1
    pending_exit_reason = "TP_HIT"
    pending_exit_trigger_px = 0.0

    next_allowed_entry = WINDOW

    n_trades = 0
    n_wins   = 0
    sum_gross = 0.0

    def book_trade(fill_px, exit_idx, reason):
        nonlocal n_trades, n_wins, sum_gross, pos_open, next_allowed_entry
        if pos_is_long:
            pnl_units = fill_px - pos_entry_px
        else:
            pnl_units = pos_entry_px - fill_px
        gross = pnl_units * SIZE_LOTS * TICK_MULT
        net = gross - COMMISSION_RT
        n_trades += 1
        if net > 0:
            n_wins += 1
        sum_gross += gross
        pos_open = False
        next_allowed_entry = exit_idx + COOLDOWN_TICKS

    for i, (ts, bid, ask) in enumerate(ticks):
        mid = 0.5 * (bid + ask)

        # Pending entry fires at index i
        if pending_entry_at == i:
            pos_is_long = (pending_entry_dir == 1)
            pos_entry_px = ask if pos_is_long else bid
            pos_entry_ix = i
            pos_open = True
            pending_entry_at  = -1
            pending_entry_dir = 0

        # Pending exit fires at index i
        if pos_open and pending_exit_at == i:
            # honest fill: worst side
            fpx = bid if pos_is_long else ask
            book_trade(fpx, i, pending_exit_reason)
            pending_exit_at = -1

        # Update z-score signal AT THIS tick (matches C++: sig.update(t.mid()))
        if n == WINDOW:
            old = ring[head]
            s  -= old
            s2 -= old * old
        ring[head] = mid
        s  += mid
        s2 += mid * mid
        head = (head + 1) % WINDOW
        if n < WINDOW:
            n += 1
            last_z = 0.0
            warm = False
            fire = 0
        else:
            warm = True
            m = s / WINDOW
            var = max(s2 / WINDOW - m * m, 1e-12)
            sd = var ** 0.5
            z = (mid - m) / sd
            last_z = z
            if z >  Z_THRESH:
                fire = -1
            elif z < -Z_THRESH:
                fire = +1
            else:
                fire = 0

        # session gate
        if fire != 0 and not session_allows(ts):
            fire = 0

        # TP/SL trigger checks for open position
        if pos_open and pending_exit_at == -1:
            if pos_is_long:
                if bid >= pos_entry_px + TP_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, n_ticks - 1)
                    pending_exit_reason = "TP_HIT"
                    pending_exit_trigger_px = pos_entry_px + TP_PTS
                elif bid <= pos_entry_px - SL_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, n_ticks - 1)
                    pending_exit_reason = "SL_HIT"
                    pending_exit_trigger_px = pos_entry_px - SL_PTS
            else:
                if ask <= pos_entry_px - TP_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, n_ticks - 1)
                    pending_exit_reason = "TP_HIT"
                    pending_exit_trigger_px = pos_entry_px - TP_PTS
                elif ask >= pos_entry_px + SL_PTS:
                    pending_exit_at = min(i + LATENCY_TICKS, n_ticks - 1)
                    pending_exit_reason = "SL_HIT"
                    pending_exit_trigger_px = pos_entry_px + SL_PTS

        # New entry consideration
        if not pos_open and pending_entry_at == -1 \
                and i >= next_allowed_entry and fire != 0:
            pending_entry_at = min(i + LATENCY_TICKS, n_ticks - 1)
            pending_entry_dir = fire

    # End-of-day forced close
    if pos_open:
        last_bid, last_ask = ticks[-1][1], ticks[-1][2]
        last_mid = 0.5 * (last_bid + last_ask)
        # honest fill on EOD: worst side
        fpx = last_bid if pos_is_long else last_ask
        book_trade(fpx, n_ticks - 1, "EOD_FORCE")

    wr_pct = (n_wins / n_trades * 100.0) if n_trades else 0.0
    return n_trades, wr_pct, sum_gross


def run_cpp_single(day_path: Path):
    csv_out = "/tmp/verify_extreme_asia_one_day.csv"
    if os.path.exists(csv_out):
        os.remove(csv_out)
    cmd = [
        str(HARNESS),
        "--single", f"{TP_PTS},{SL_PTS},{Z_THRESH}",
        "--session", f"{SESSION_START}-{SESSION_END}",
        "--latency", str(LATENCY_TICKS),
        "--csv-out", csv_out,
        "--label", "verify_replay",
        str(day_path),
    ]
    print(f"[cpp]   {' '.join(cmd)}")
    subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    # Find the honest row
    with open(csv_out, "r") as f:
        reader = csv.DictReader(f)
        for r in reader:
            if r["fill_model"] == "honest":
                return int(r["n_trades"]), float(r["wr_pct"]), float(r["sum_usd"])
    raise RuntimeError("no honest row from harness")


def main():
    if not HARNESS.exists():
        sys.exit(f"[FATAL] missing harness {HARNESS}")
    if not DAY_FILE.exists():
        sys.exit(f"[FATAL] missing day {DAY_FILE}")

    print(f"\nDay: {DAY_FILE.name}")
    print(f"Config: TP={TP_PTS} SL={SL_PTS} z={Z_THRESH} session=[{SESSION_START},{SESSION_END}) "
          f"lat={LATENCY_TICKS} w={WINDOW} cooldown={COOLDOWN_TICKS}\n")

    py_n, py_wr, py_sum = python_replay(DAY_FILE)
    cpp_n, cpp_wr, cpp_sum = run_cpp_single(DAY_FILE)

    print(f"\n{'metric':<10s} {'python':>15s} {'C++ harness':>18s} {'match?':>10s}")
    print("-" * 60)
    n_match  = py_n == cpp_n
    wr_match = abs(py_wr - cpp_wr) < 0.01
    sum_match = abs(py_sum - cpp_sum) < 0.005
    print(f"{'N':<10s} {py_n:>15d} {cpp_n:>18d} {('MATCH' if n_match else 'MISMATCH'):>10s}")
    print(f"{'WR%':<10s} {py_wr:>15.2f} {cpp_wr:>18.2f} {('MATCH' if wr_match else 'MISMATCH'):>10s}")
    print(f"{'sum gross':<10s} {py_sum:>15.4f} {cpp_sum:>18.4f} {('MATCH' if sum_match else 'MISMATCH'):>10s}")
    if n_match and wr_match and sum_match:
        print("\nVerdict       : ALL MATCH — C++ harness verified for extreme_asia primary candidate.")
        return 0
    print("\nVerdict       : MISMATCH — investigate.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
