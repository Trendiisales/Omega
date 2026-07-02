#!/usr/bin/env python3
"""FEEDS FRESHNESS SELF-TEST — RED when ANY data feed is stale.

Built 2026-06-30 after the rdagent qlib model silently froze at 06-25 for days
(IBKR tunnel down + no scheduler + no alarm). The operator's rule: never silently
trade/show stale data again. This is the alarm — like the protection self-test,
it runs every session (SessionStart hook) and screams RED on the first stale feed.

VPS operational-dump section (S-2026-07-02): the local getters below can only see
Mac-side files. The live VPS binary also writes load-bearing CSVs to C:\\Omega\\logs
(set_live_dump targets) that are NEVER pulled to the Mac — so a VPS writer could die
with every Mac banner GREEN. That blind spot is closed by ONE batched `ssh omega-vps`
poll that computes each tools/live_dump_manifest.tsv file's age ON the VPS (skew-free)
and REDs a stale LIVE dump. A weekend guard skips enforcement Fri22:00->Sun22:00 UTC
(markets closed). ssh failure => RED (an unverifiable live feed is not GREEN).

Exit codes:  0 = all fresh   1 = a LIVE feed stale (critical)   2 = only research stale
Run:  python3 tools/feeds_selftest.py   [--quiet]
"""
from __future__ import annotations
import csv, datetime as dt, json, os, subprocess, sys
from pathlib import Path

HOME = Path.home()
TICK = HOME / "Tick"
RDA  = HOME / "Omega" / "data" / "rdagent"
QLIB = HOME / ".qlib" / "qlib_data" / "omega_data"
STALL = HOME / "stall-accountant"
CRYPTO_BOOK = HOME / "Crypto" / "backtest" / "data" / "ibkrcrypto" / "state.json"

# VPS operational-dump manifest + the C:\Omega\logs root the live binary writes to.
LIVE_DUMP_MANIFEST = HOME / "Omega" / "tools" / "live_dump_manifest.tsv"
VPS_LOG_ROOT = r"C:\Omega\logs"


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


def crypto_book_heartbeat() -> dt.date | None:
    # Chimera/Crypto book refreshes every 5min; track the FRESHER of updated /
    # live_mark_ts. NEVER file mtime — the file is rewritten by other writers while
    # the heartbeat fields stay frozen (the exact silent-stale trap that let the book
    # go 1.5d stale after the 07-01 IBKRCrypto->Crypto move with every banner green).
    ds = [x for x in (json_field_date(CRYPTO_BOOK, "updated"),
                      json_field_date(CRYPTO_BOOK, "live_mark_ts")) if x]
    return max(ds) if ds else None


def mtime_date(path: Path) -> dt.date | None:
    try:
        return dt.datetime.fromtimestamp(path.stat().st_mtime).date()
    except OSError:
        return None


# ── VPS operational-CSV staleness (the Mac-invisible live dumps) ───────────────
# The live VPS binary writes load-bearing CSVs to C:\Omega\logs via set_live_dump().
# None are pulled to the Mac, so the getters above CANNOT see them — a VPS writer
# can die and every Mac banner stays GREEN. This section polls the VPS directly:
# ONE batched `ssh omega-vps` call runs PowerShell that computes each manifest
# file's age-in-minutes ON THE VPS (skew-free — no Mac/VPS clock comparison), and
# each age is checked against its manifest max_age_min. A stale LIVE dump => exit 1.
# mtime is safe here: these dumps are append-only and only their writer touches them.

def load_live_dump_manifest() -> list[tuple[str, int, str]]:
    """Parse live_dump_manifest.tsv -> [(path_under_logs, max_age_min, description)]."""
    out: list[tuple[str, int, str]] = []
    try:
        for line in LIVE_DUMP_MANIFEST.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            path, max_age = parts[0].strip(), parts[1].strip()
            desc = parts[2].strip() if len(parts) > 2 else ""
            try:
                out.append((path, int(max_age), desc))
            except ValueError:
                continue
    except OSError:
        pass
    return out


def market_closed_weekend(now_utc: dt.datetime) -> bool:
    """Gold/index markets closed roughly Fri 22:00 UTC -> Sun 22:00 UTC. Inside that
    window the live dumps legitimately stop advancing, so skip enforcement (else it
    false-REDs all weekend)."""
    wd, hr = now_utc.weekday(), now_utc.hour   # Mon=0 .. Sun=6
    if wd == 5:                       # Saturday: fully closed
        return True
    if wd == 4 and hr >= 22:          # Friday from 22:00 UTC
        return True
    if wd == 6 and hr < 22:           # Sunday before 22:00 UTC
        return True
    return False


def vps_live_dump_ages(manifest: list[tuple[str, int, str]]) -> dict[str, float] | None:
    """ONE batched `ssh omega-vps` call. Returns {path_under_logs: age_min} computed
    ON the VPS (skew-free). None if the ssh call fails entirely (=> RED). A file that
    is missing on the VPS is reported as a very large age so it REDs against its cap.
    The ssh command MUST start literally `ssh omega-vps` (classifier rule)."""
    # PowerShell one-liner: for each relative path, print "<path>\t<age_min>" (or a
    # huge age if absent). LastWriteTime age keeps us clock-skew-free vs the Mac.
    files = ";".join(f"'{p}'" for p, _, _ in manifest)
    ps = (
        f"$r='{VPS_LOG_ROOT}';"
        f"foreach($f in @({files})){{"
        f"$p=Join-Path $r $f;"
        f"if(Test-Path $p){{"
        f"$a=((Get-Date)-(Get-Item $p).LastWriteTime).TotalMinutes;"
        # [char]9 (tab), NOT "`t": ssh->remote cmd.exe strips embedded double-quotes,
        # leaving a bare `t that PowerShell can't parse. Single-quotes/[char]9 survive.
        f"Write-Output ($f+[char]9+[math]::Round($a,1))}}"
        f"else{{Write-Output ($f+[char]9+999999)}}}}"
    )
    try:
        r = subprocess.run(
            ["ssh", "omega-vps", "powershell", "-NoProfile", "-Command", ps],
            capture_output=True, text=True, timeout=45,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    ages: dict[str, float] = {}
    for ln in r.stdout.splitlines():
        parts = ln.strip().split("\t")
        if len(parts) != 2:
            continue
        try:
            ages[parts[0].strip()] = float(parts[1].strip())
        except ValueError:
            continue
    return ages if ages else None


def vps_ibkr_exec_status() -> tuple[str, str]:
    """ONE `ssh omega-vps` call. Confirms the live binary's IBKR EXECUTION path is
    ACTIVE. Gold now routes to IBKR GC futures (execution_broker==IBKR) and its
    MIN_TRADE_GATE cost basis is rebased onto IBKR (0.5x BlackBull-spot spread).
    If the exec path silently reverts to BLACKBULL_FIX or hits CONNECT FAILED,
    gold orders are rejected AND the cheaper cost gate is mispriced against the
    wrong venue -- a double failure that looks like the strategy died. Scans the
    3 most-recent runtime logs (robust across the UTC date-roll, since the
    [IBKR-EXEC] status prints only at boot) for the latest status line.
    Returns (status, detail). ssh command MUST start literally `ssh omega-vps`."""
    # No `|` pipes: ssh->remote cmd.exe would intercept them before PowerShell sees
    # the string (same trap the vps_live_dump_ages helper avoids). Emit each match as
    # "<mtime_ticks>[char]9<line>" and pick the newest file's match in Python -- avoids
    # a pipe-based Sort-Object entirely.
    ps = (
        r"$ls=Get-ChildItem 'C:\Omega\logs\omega_*.log';"
        r"foreach($f in $ls){"
        r"$m=Select-String -Path $f.FullName -Pattern '\[IBKR-EXEC\] execution_broker';"
        r"if($m){Write-Output ($f.LastWriteTime.Ticks.ToString()+[char]9+$m[-1].Line.Trim())}}"
    )
    try:
        r = subprocess.run(
            ["ssh", "omega-vps", "powershell", "-NoProfile", "-Command", ps],
            capture_output=True, text=True, timeout=45,
        )
    except (OSError, subprocess.SubprocessError):
        return ("RED", "ssh omega-vps failed -- IBKR exec path UNVERIFIABLE")
    if r.returncode != 0:
        return ("RED", f"ssh rc={r.returncode} -- IBKR exec path UNVERIFIABLE")
    # Pick the [IBKR-EXEC] line from the newest-modified log (max mtime ticks).
    best_ticks, line = -1, ""
    for ln in r.stdout.splitlines():
        parts = ln.strip().split("\t", 1)
        if len(parts) != 2:
            continue
        try:
            ticks = int(parts[0])
        except ValueError:
            continue
        if ticks > best_ticks:
            best_ticks, line = ticks, parts[1].strip()
    if not line:
        return ("RED", "no [IBKR-EXEC] line in any C:\\Omega\\logs\\omega_*.log")
    if "ACTIVE" in line:
        return ("PASS", line)
    if "CONNECT FAILED" in line:
        return ("RED", f"IBKR exec CONNECT FAILED -- gold orders rejected: {line}")
    if "BLACKBULL_FIX" in line:
        return ("RED", f"execution_broker=BLACKBULL_FIX -- gold NOT on IBKR, "
                       f"cost gate mispriced vs venue: {line}")
    return ("RED", f"no [IBKR-EXEC] ACTIVE line in last 3 logs: {line or 'empty'}")


# (label, kind, max_age_trading_days, getter)  kind: "live" critical | "research" paper
FEEDS = [
    ("tick XAUUSD daily",   "live", 2, lambda: csv_last_date(TICK / "2yr_XAUUSD_daily.csv")),
    ("tick DJ30 daily",     "live", 2, lambda: csv_last_date(TICK / "DJ30_daily_2016_2026.csv")),
    ("tick SPX daily",      "live", 2, lambda: csv_last_date(TICK / "SPX_daily_2016_2026.csv")),
    ("tick NDX daily",      "live", 2, lambda: csv_last_date(TICK / "NDX_daily_2016_2026.csv")),
    ("tick GER40 daily",    "live", 4, lambda: csv_last_date(TICK / "GER40_daily_2016_2026.csv")),
    ("companion telemetry", "live", 1, lambda: mtime_date(STALL / "companion_state.json")),
    ("crypto book heartbeat","live", 1, crypto_book_heartbeat),
    ("qlib omega_data",     "research", 2, qlib_cal_last),
    ("rdagent basket",      "research", 2, lambda: json_field_date(RDA / "latest.json", "signal", "date")),
    ("sp500_long_close",    "research", 4, lambda: csv_last_date(RDA / "sp500_long_close.csv")),
    # NB: sp500_close.csv is a dead fallback — serve.py:26 reads sp500_long_close first and never
    # reaches it. Not fed by any refresher, so tracking it = false AMBER. Excluded deliberately.
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

    # ── VPS operational-CSV staleness (Mac-invisible live dumps) ──────────────
    # Reported in a distinct kind ("vps-live") so the age is minutes, not days.
    vps_rows: list[tuple[str, str, str]] = []   # (label, status, detail)
    manifest = load_live_dump_manifest()
    now_utc = dt.datetime.now(dt.timezone.utc)
    if manifest:
        if market_closed_weekend(now_utc):
            for path, max_age_min, desc in manifest:
                vps_rows.append((path, "SKIP", f"weekend (market closed) -- age not enforced (max {max_age_min}min)"))
        else:
            ages = vps_live_dump_ages(manifest)
            if ages is None:
                # ssh unreachable / failed entirely -> cannot verify a load-bearing
                # live feed -> treat as RED (each manifested LIVE dump is unverifiable).
                live_red += len(manifest)
                for path, max_age_min, desc in manifest:
                    vps_rows.append((path, "RED", f"VPS UNREACHABLE (ssh omega-vps failed) -- feed unverifiable (max {max_age_min}min)"))
            else:
                for path, max_age_min, desc in manifest:
                    age_min = ages.get(path)
                    if age_min is None:
                        live_red += 1
                        vps_rows.append((path, "RED", f"no age returned for {path} -- writer/path missing (max {max_age_min}min)"))
                    elif age_min > max_age_min:
                        live_red += 1
                        vps_rows.append((path, "RED", f"age={age_min:.0f}min > max {max_age_min}min -- VPS writer stalled ({desc})"))
                    else:
                        vps_rows.append((path, "PASS", f"age={age_min:.0f}min (max {max_age_min}min) {desc}"))

    # ── VPS IBKR execution-path status (gold routes to IBKR GC; cost gate rebased) ──
    # Skip on weekend (market closed -> gold not trading, exec path irrelevant).
    exec_row: tuple[str, str] | None = None
    if not market_closed_weekend(now_utc):
        ex_status, ex_detail = vps_ibkr_exec_status()
        exec_row = (ex_status, ex_detail)
        if ex_status != "PASS":
            live_red += 1

    if not quiet:
        verdict = "RED — LIVE FEED STALE" if live_red else ("AMBER — research stale" if res_red else "GREEN — all feeds fresh")
        mark = {"GREEN": "GREEN", "AMBER": "AMBER", "RED": "RED"}
        head = "RED" if live_red else ("AMBER" if res_red else "GREEN")
        print(f"FEEDS SELF-TEST {mark[head]}  (ref last-trading-day {ltd})  -- {verdict}")
        for label, kind, d, age, max_age, status in rows:
            flag = "PASS" if status == "FRESH" else status
            ds = d.isoformat() if d else "—"
            print(f"  {flag:7} [{kind:8}] {label:22} last={ds} age={age if age is not None else '?'}d (max {max_age}d)")
        if vps_rows:
            print("  -- VPS operational dumps (C:\\Omega\\logs, polled skew-free via ssh omega-vps) --")
            for path, status, detail in vps_rows:
                print(f"  {status:7} [vps-live] {path:24} {detail}")
        if exec_row:
            print(f"  {exec_row[0]:7} [vps-exec] {'ibkr_exec_status':24} {exec_row[1]}")
        if live_red:
            print("  -> A LIVE feed is stale: fix the feeder BEFORE trusting any signal/telemetry.")
        elif res_red:
            print("  -> Research/paper feed stale (rdagent panel). Run: bash tools/rdagent/qlib_refresh.sh")
    return 1 if live_red else (2 if res_red else 0)


if __name__ == "__main__":
    sys.exit(main())
