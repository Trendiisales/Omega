#!/usr/bin/env python3
"""FEEDS FRESHNESS SELF-TEST — RED when ANY data feed is stale.

Built 2026-06-30 after the rdagent qlib model silently froze at 06-25 for days
(IBKR tunnel down + no scheduler + no alarm). The operator's rule: never silently
trade/show stale data again. This is the alarm — like the protection self-test,
it runs every session (SessionStart hook) and screams RED on the first stale feed.

Exit codes:  0 = all fresh   1 = a LIVE feed stale (critical)   2 = only research stale
Run:  python3 tools/feeds_selftest.py   [--quiet]
"""
from __future__ import annotations
import csv, datetime as dt, json, os, sys
from pathlib import Path

HOME = Path.home()
TICK = HOME / "Tick"
RDA  = HOME / "Omega" / "data" / "rdagent"
QLIB = HOME / ".qlib" / "qlib_data" / "omega_data"
STALL = HOME / "stall-accountant"


def last_trading_day(today: dt.date) -> dt.date:
    d = today
    while d.weekday() >= 5:  # Sat=5 Sun=6 -> back to Fri
        d -= dt.timedelta(days=1)
    return d


def epoch_or_iso(s: str) -> dt.date | None:
    s = s.strip().strip('"')
    if not s:
        return None
    try:
        if s.isdigit():  # epoch seconds (tick files)
            return dt.datetime.fromtimestamp(int(s), dt.timezone.utc).date()
        return dt.date.fromisoformat(s[:10])
    except (ValueError, OverflowError, OSError):
        return None


def csv_last_date(path: Path) -> dt.date | None:
    try:
        with open(path) as fh:
            last = None
            for row in csv.reader(fh):
                if row and row[0] and row[0][0].isdigit():
                    last = row[0]
        return epoch_or_iso(last) if last else None
    except OSError:
        return None


def qlib_cal_last() -> dt.date | None:
    try:
        return epoch_or_iso(QLIB.joinpath("calendars/day.txt").read_text().strip().splitlines()[-1])
    except (OSError, IndexError):
        return None


def json_field_date(path: Path, *keys) -> dt.date | None:
    try:
        d = json.loads(path.read_text())
        for k in keys:
            d = d[k]
        return epoch_or_iso(str(d))
    except (OSError, KeyError, TypeError, json.JSONDecodeError):
        return None


def mtime_date(path: Path) -> dt.date | None:
    try:
        return dt.datetime.fromtimestamp(path.stat().st_mtime).date()
    except OSError:
        return None


# (label, kind, max_age_trading_days, getter)  kind: "live" critical | "research" paper
FEEDS = [
    ("tick XAUUSD daily",   "live", 2, lambda: csv_last_date(TICK / "2yr_XAUUSD_daily.csv")),
    ("tick DJ30 daily",     "live", 2, lambda: csv_last_date(TICK / "DJ30_daily_2016_2026.csv")),
    ("tick SPX daily",      "live", 2, lambda: csv_last_date(TICK / "SPX_daily_2016_2026.csv")),
    ("tick NDX daily",      "live", 2, lambda: csv_last_date(TICK / "NDX_daily_2016_2026.csv")),
    ("tick GER40 daily",    "live", 4, lambda: csv_last_date(TICK / "GER40_daily_2016_2026.csv")),
    ("companion telemetry", "live", 1, lambda: mtime_date(STALL / "companion_state.json")),
    ("qlib omega_data",     "research", 2, qlib_cal_last),
    ("rdagent basket",      "research", 2, lambda: json_field_date(RDA / "latest.json", "signal", "date")),
    ("sp500_long_close",    "research", 4, lambda: csv_last_date(RDA / "sp500_long_close.csv")),
    ("sp500_close",         "research", 4, lambda: csv_last_date(RDA / "sp500_close.csv")),
]


def main() -> int:
    quiet = "--quiet" in sys.argv
    today = dt.date.today()
    ltd = last_trading_day(today)
    rows, live_red, res_red = [], 0, 0
    for label, kind, max_age, getter in FEEDS:
        d = getter()
        if d is None:
            status, age = "MISSING", None
        else:
            age = (ltd - d).days
            status = "FRESH" if age <= max_age else "STALE"
        if status != "FRESH":
            if kind == "live":
                live_red += 1
            else:
                res_red += 1
        rows.append((label, kind, d, age, max_age, status))

    if not quiet:
        verdict = "RED — LIVE FEED STALE" if live_red else ("AMBER — research stale" if res_red else "GREEN — all feeds fresh")
        mark = {"GREEN": "GREEN", "AMBER": "AMBER", "RED": "RED"}
        head = "RED" if live_red else ("AMBER" if res_red else "GREEN")
        print(f"FEEDS SELF-TEST {mark[head]}  (ref last-trading-day {ltd})  -- {verdict}")
        for label, kind, d, age, max_age, status in rows:
            flag = "PASS" if status == "FRESH" else status
            ds = d.isoformat() if d else "—"
            print(f"  {flag:7} [{kind:8}] {label:22} last={ds} age={age if age is not None else '?'}d (max {max_age}d)")
        if live_red:
            print("  -> A LIVE feed is stale: fix the feeder BEFORE trusting any signal/telemetry.")
        elif res_red:
            print("  -> Research/paper feed stale (rdagent panel). Run: bash tools/rdagent/qlib_refresh.sh")
    return 1 if live_red else (2 if res_red else 0)


if __name__ == "__main__":
    sys.exit(main())
