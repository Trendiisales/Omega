#!/usr/bin/env python3
"""ENGINE-WATERMARK LAG AUDIT — RED when a LIVE polled engine's processed-date falls
behind the feed it consumes.

WHY THIS EXISTS (the gap feeds_selftest never covered):
  feeds_selftest.py proves the FEED file is fresh. It does NOT prove the ENGINE actually
  CONSUMED it. On 2026-07-16 the whole book booked ZERO closes: the daily-close evals
  (20:00-22:00 UTC) landed inside a 5-restart deploy storm, and the seed-on-boot replay
  absorbed the day's rows as history ("deploy-forward: seed primes the detector only,
  books nothing") -> signals silently lost. The S-2026-07-08c watermark-catchup fixes this
  where wired (StockDip's watermark correctly advanced to 07-16), but nothing ALERTS when
  an engine's watermark lags the feed -> it gets found MANUALLY, every time. This closes that.

WHAT IT CHECKS:
  For each ENABLED polled engine in REGISTRY: read its persisted live watermark
  (`*_lastseen.txt`, epoch secs = last date processed through LIVE logic) ON the VPS, and the
  latest date present in the feed CSV it polls. RED if the engine lags the feed by >= MAX_LAG
  trading days. DISABLED engines (#if 0) are expected frozen -> reported INFO, never RED.

  ssh failure => RED (an unverifiable live engine is not GREEN — same discipline as feeds_selftest).
  Weekend guard: the daily feed legitimately stops advancing Fri22:00->Mon00:00 UTC -> skip.

Exit: 0 = all enabled engines current; 1 = a lag RED; 2 = ssh/parse failure (unverifiable).

Wire into scripts (protection_selftest / SessionStart) ONLY after a read-only verify pass.
Run standalone:  python3 tools/engine_watermark_audit.py
"""
from __future__ import annotations

import base64
import datetime as dt
import subprocess
import sys
from pathlib import Path

# reuse the market-calendar logic (single source of truth — don't re-derive trading days)
sys.path.insert(0, str(Path(__file__).resolve().parent))
from feeds_selftest import last_trading_day, trading_days_between, market_closed_weekend  # noqa: E402

VPS = "omega-new"                       # LIVE box (never omega-vps/185); classifier needs literal prefix
VPS_OMEGA = "C:/Omega"
MAX_LAG_TD = 1                          # RED once an enabled engine is >=1 trading day behind the feed

# engine -> (lastseen_file under C:/Omega, feed CSV under C:/Omega, ENABLED)
# ENABLED mirrors engine_init.hpp: AULAD + bigcap2pct are #if 0 (S-2026-07-16l) => frozen-by-design.
# When an engine is re-enabled or a new polled engine ships, ADD it here (that is the gate:
# a live polled book with no watermark-audit entry is the silent-staleness hole this prevents).
REGISTRY = [
    # name,                 lastseen_file,                         feed_csv,                           enabled
    ("StockDip/Turtle",     "stockdipturtle_lastseen.txt",         "data/rdagent/sp500_long_close.csv", True),
    ("StockDayMoverLadder",  "stockladder_companion_lastseen.txt",  "data/rdagent/sp500_long_close.csv", False),  # #if 0 S-07-16l
    ("BigCap2pctImpulse",    "bigcap2pct_companion_lastseen.txt",   "data/rdagent/sp500_long_close.csv", False),  # #if 0 S-07-16l
]


def _ssh_probe() -> dict[str, str] | None:
    """ONE batched ssh call. Emits `key<TAB>value` lines:
        wm:<file> <epoch>      (engine watermark, '0' if missing)
        feed:<csv> <YYYY-MM-DD>(feed last-row date, '' if unreadable)
    Returns dict or None on ssh failure."""
    wm_files = {ls for _, ls, _, _ in REGISTRY}
    feed_files = {fd for _, _, fd, _ in REGISTRY}
    ps_lines = [f"$o='{VPS_OMEGA}';"]
    for ls in wm_files:
        ps_lines.append(
            f"$p=Join-Path $o '{ls}'; if(Test-Path $p){{$v=(Get-Content $p -Raw).Trim()}}else{{$v='0'}};"
            f"Write-Output ('wm:{ls}' + [char]9 + $v);"
        )
    for fd in feed_files:
        # last non-empty line's first CSV field = the date (rows are 'YYYY-MM-DD,...')
        ps_lines.append(
            f"$p=Join-Path $o '{fd}'; $d=''; if(Test-Path $p){{"
            f"$last=(Get-Content $p | Where-Object {{$_ -ne ''}} | Select-Object -Last 1);"
            f"if($last){{$d=($last -split ',')[0]}}}};"
            f"Write-Output ('feed:{fd}' + [char]9 + $d);"
        )
    ps = "".join(ps_lines)
    # -EncodedCommand (UTF-16LE base64) so the PS survives ssh + the remote shell verbatim —
    # passing raw $/;/quotes through subprocess+ssh mangles them (they get re-parsed remotely).
    enc = base64.b64encode(ps.encode("utf-16-le")).decode("ascii")
    try:
        out = subprocess.run(
            ["ssh", VPS, "powershell", "-NoProfile", "-EncodedCommand", enc],
            capture_output=True, text=True, timeout=45,
        )
    except (subprocess.TimeoutExpired, OSError):
        return None
    if out.returncode != 0:
        return None
    res: dict[str, str] = {}
    for line in out.stdout.splitlines():
        if "\t" in line:
            k, v = line.split("\t", 1)
            res[k.strip()] = v.strip()
    return res or None


def _epoch_to_date(epoch: str) -> dt.date | None:
    try:
        e = int(epoch)
    except ValueError:
        return None
    if e <= 0:
        return None
    return dt.datetime.fromtimestamp(e, tz=dt.timezone.utc).date()


def _iso_to_date(s: str) -> dt.date | None:
    try:
        return dt.date.fromisoformat(s.strip())
    except (ValueError, AttributeError):
        return None


def main() -> int:
    now = dt.datetime.now(dt.timezone.utc)
    print(f"ENGINE-WATERMARK LAG AUDIT (ref {now:%Y-%m-%d %H:%MZ})")

    if market_closed_weekend(now):
        print("  SKIP: market closed (weekend) — daily feed legitimately not advancing.")
        return 0

    probe = _ssh_probe()
    if probe is None:
        print("  RED [ssh]: could not reach omega-new — a live engine's freshness is UNVERIFIABLE.")
        return 2

    ltd = last_trading_day(now.date())
    red = 0
    checked = 0
    for name, ls, fd, enabled in REGISTRY:
        feed_d = _iso_to_date(probe.get(f"feed:{fd}", ""))
        wm_d = _epoch_to_date(probe.get(f"wm:{ls}", "0"))
        tag = "ENABLED" if enabled else "disabled"
        if feed_d is None:
            if enabled:
                print(f"  RED  [{name:22}] feed {fd} unreadable on VPS — cannot verify.")
                red += 1
            else:
                print(f"  INFO [{name:22}] ({tag}) feed unreadable; engine off, not enforced.")
            continue
        if wm_d is None:
            if enabled:
                print(f"  RED  [{name:22}] ENABLED but no watermark ({ls}) — engine may never persist "
                      f"progress; first-restart staleness hole.")
                red += 1
            else:
                print(f"  INFO [{name:22}] ({tag}) no watermark; engine off (#if 0), expected.")
            continue
        # lag = trading days the engine's processed-date sits behind the feed's latest date
        lag = trading_days_between(wm_d, feed_d) if wm_d < feed_d else 0
        if not enabled:
            print(f"  INFO [{name:22}] disabled (#if 0): wm={wm_d} feed={feed_d} (frozen by design).")
            continue
        checked += 1
        if lag >= MAX_LAG_TD:
            print(f"  RED  [{name:22}] ENGINE STALE: processed={wm_d} but feed={feed_d} "
                  f"(lag {lag} trading day(s)). Restart absorbed the new row(s) as seed — signals lost.")
            red += 1
        else:
            print(f"  PASS [{name:22}] current: wm={wm_d} feed={feed_d} (lag {lag}td, ltd={ltd}).")

    if red:
        print(f"-> {red} engine(s) STALE/unverifiable. A restart ate the feed row -> no trades booked. "
              f"Fix: dispatch the newer row LIVE on next boot (S-07-08c catchup) + avoid restarts in the "
              f"20:00-23:00 UTC close window.")
        return 1
    print(f"-> GREEN: {checked} enabled polled engine(s) current with their feed. No silent staleness.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
