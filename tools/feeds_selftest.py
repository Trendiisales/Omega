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
import base64, csv, datetime as dt, json, os, subprocess, sys
from pathlib import Path

HOME = Path.home()
TICK = HOME / "Tick"
RDA  = HOME / "Omega" / "data" / "rdagent"
QLIB = HOME / ".qlib" / "qlib_data" / "omega_data"
STALL = HOME / "stall-accountant"
CRYPTO_BOOK = HOME / "Crypto" / "backtest" / "data" / "ibkrcrypto" / "state.json"
# S-2026-07-12: the ibkrcrypto book is RETIRED (folded onto Chimera/josgp1). The ONE
# crypto system's Mac-visible liveness = the chimera->desk relay staging file (rewritten
# every 120s by refresh_crypto_companion.sh). Used by crypto_book_heartbeat().
CHIMERA_RELAY = Path("/tmp/chimera_inbound.csv")

# VPS operational-dump manifest + the C:\Omega\logs root the live binary writes to.
LIVE_DUMP_MANIFEST = HOME / "Omega" / "tools" / "live_dump_manifest.tsv"
VPS_LOG_ROOT = r"C:\Omega\logs"
# Which box runs the LIVE binary (S-2026-07-07 cutover: omega-new is live; omega-vps is
# the retired shadow box pending decommission). All live-path checks poll THIS host.
VPS_HOST = "omega-new"


def _easter(year: int) -> dt.date:
    # Anonymous Gregorian computus -> Easter Sunday. Needed for Good Friday (NYSE closed).
    a = year % 19
    b, c = divmod(year, 100)
    d, e = divmod(b, 4)
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = divmod(c, 4)
    l = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l) // 451
    month = (h + l - 7 * m + 114) // 31
    day = ((h + l - 7 * m + 114) % 31) + 1
    return dt.date(year, month, day)


def _observed(d: dt.date) -> dt.date:
    # NYSE observed-holiday shift: Sat -> prior Fri, Sun -> next Mon.
    if d.weekday() == 5:
        return d - dt.timedelta(days=1)
    if d.weekday() == 6:
        return d + dt.timedelta(days=1)
    return d


def _nth_weekday(year: int, month: int, weekday: int, n: int) -> dt.date:
    # nth (1-based) `weekday` (Mon=0) of month; n<0 counts from the end.
    if n > 0:
        d = dt.date(year, month, 1)
        d += dt.timedelta(days=(weekday - d.weekday()) % 7)
        return d + dt.timedelta(weeks=n - 1)
    d = dt.date(year, month, 1) + dt.timedelta(days=31)  # into next month
    d = d.replace(day=1) - dt.timedelta(days=1)          # last day of month
    d -= dt.timedelta(days=(d.weekday() - weekday) % 7)
    return d


def us_market_holidays(year: int) -> set[dt.date]:
    """NYSE full-day closures for `year` (dates the US index/gold feeds legitimately
    do NOT advance). Computed, not hardcoded, so it stays correct in future years.
    Juneteenth included from 2022 (first NYSE observance)."""
    h = {
        _observed(dt.date(year, 1, 1)),                    # New Year's Day
        _nth_weekday(year, 1, 0, 3),                        # MLK Day (3rd Mon Jan)
        _nth_weekday(year, 2, 0, 3),                        # Washington's Birthday (3rd Mon Feb)
        _easter(year) - dt.timedelta(days=2),              # Good Friday
        _nth_weekday(year, 5, 0, -1),                       # Memorial Day (last Mon May)
        _nth_weekday(year, 9, 0, 1),                        # Labor Day (1st Mon Sep)
        _nth_weekday(year, 11, 3, 4),                       # Thanksgiving (4th Thu Nov)
        _observed(dt.date(year, 7, 4)),                    # Independence Day
        _observed(dt.date(year, 12, 25)),                  # Christmas
    }
    if year >= 2022:
        h.add(_observed(dt.date(year, 6, 19)))             # Juneteenth
    return h


def is_trading_day(d: dt.date) -> bool:
    return d.weekday() < 5 and d not in us_market_holidays(d.year)


def last_trading_day(today: dt.date) -> dt.date:
    # Back off weekends AND US market holidays so the freshness reference matches the
    # true last session (else the day after a holiday false-REDs every US feed).
    d = today
    while not is_trading_day(d):
        d -= dt.timedelta(days=1)
    return d


def trading_days_between(d: dt.date, ltd: dt.date) -> int:
    """Number of trading sessions AFTER `d` up to and including `ltd` — i.e. how many
    sessions the feed is behind. 0 if the data is at/ahead of the reference. This is
    the true 'max_age_trading_days' the FEEDS table always meant: counting calendar
    days false-REDs every Monday (Fri->Mon = 3 calendar days) and every post-holiday
    day. Skips weekends + US market holidays so a normal or post-holiday Monday with
    Friday's (or the pre-holiday) bar reads age=1, not 3-4."""
    if d >= ltd:
        return 0
    n, cur = 0, d + dt.timedelta(days=1)
    while cur <= ltd:
        if is_trading_day(cur):
            n += 1
        cur += dt.timedelta(days=1)
    return n


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
    # S-2026-07-12 CONSOLIDATION: the Mac ibkrcrypto book was RETIRED (folded onto
    # the ONE Chimera system on josgp1). Track the SURVIVING system's liveness =
    # the chimera->desk relay staging file, which is rewritten every 120s by
    # refresh_crypto_companion.sh (a genuine relay, so mtime IS a valid liveness
    # signal here — unlike the old frozen-fields book state). If the relay stalls
    # or josgp1 is unreachable, this goes stale = the honest RED.
    d = mtime_date(CHIMERA_RELAY)
    if d:
        return d
    # fallback: old ibkrcrypto book fields (only if it were ever revived)
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
    """Feeds legitimately stop advancing across the weekend -> skip enforcement (else it
    false-REDs all weekend). The window is Fri 22:00 UTC -> the first post-reopen BAR CLOSE.

    CRITICAL Sunday-side subtlety (fixed 2026-07-06): the monitored live dumps here are
    BAR-driven, not tick-driven. Futures reopen ~22:00 UTC Sunday, but the first post-reopen
    bar does not CLOSE (and thus the CSV does not refresh) until 23:00 UTC (H1) / 00:00 UTC
    Monday (H4 = the slowest monitored feed, gold_d1_trend_h4.csv). Enforcing from 22:00 —
    the old boundary — false-REDs that reopen-bar lag EVERY Sunday (~2h for H4). So the guard
    now covers the WHOLE of Sunday UTC: enforcement resumes 00:00 UTC Monday, exactly when the
    slowest feed's first reopen bar has closed. This removes the recurring false alarm WITHOUT
    weakening the dangerous case (a feed dead for hours/days is still caught at 00:00 Mon and
    every session after). NOTE: the tick-driven companion (protection_selftest) refreshes at
    the first reopen tick ~22:00, so it correctly keeps the 22:00 boundary — do NOT blindly
    sync the two guards; they differ because bar-close != tick-arrival."""
    wd, hr = now_utc.weekday(), now_utc.hour   # Mon=0 .. Sun=6
    if wd == 5:                       # Saturday: fully closed
        return True
    if wd == 4 and hr >= 22:          # Friday from 22:00 UTC (close)
        return True
    if wd == 6:                       # Sunday: closed ALL day UTC — reopen is ~22:00 but the
                                      # first H1/H4 bars don't CLOSE until 23:00 / 00:00 Mon, so
                                      # the CSVs can't be fresh until then (see docstring).
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
            ["ssh", VPS_HOST, "powershell", "-NoProfile", "-Command", ps],
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
            ["ssh", VPS_HOST, "powershell", "-NoProfile", "-Command", ps],
            capture_output=True, text=True, timeout=45,
        )
    except (OSError, subprocess.SubprocessError):
        return ("RED", f"ssh {VPS_HOST} failed -- IBKR exec path UNVERIFIABLE")
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


def vps_stock_book_health() -> tuple[str, str]:
    """ONE `ssh omega-new` call. CONTENT-based (not mtime) integrity of the stock
    daily-close book. Catches the two silent failures the mtime manifest CANNOT and
    that burned us on 2026-07-10:
      (1) a feed whose file mtime is fresh but whose last DATA ROW is days old
          (the writer touches/rewrites without appending a new dated close);
      (2) ladder names registered in the engine but ABSENT from the feed columns
          -> they seed with bars=0 and can never trade (the 39/45 mismatch).
    RED if the feed's last data row is >1 trading day behind, or ANY ladder name
    has bars=0. Returns (status, detail)."""
    ps = (
        r"$f='C:\Omega\data\rdagent\sp500_long_close.csv';"
        r"if(Test-Path $f){$l=Get-Content $f;$last=$l[-1].Split(',')[0];$cols=($l[0].Split(',').Count-1)}else{$last='MISSING';$cols=0};"
        r"$s='C:\Omega\stockladder_companion_state.json';"
        r"if(Test-Path $s){$j=Get-Content $s -Raw|ConvertFrom-Json;$zero=@($j.names|Where-Object{[int]$_.bars -lt 1}).Count;$tot=@($j.names).Count;$znames=(@($j.names|Where-Object{[int]$_.bars -lt 1}|ForEach-Object{$_.sym}) -join ' ');"
        # per-name FRESHNESS: newest bar-ts across names, and any name lagging it by >2 days is
        # frozen (has bars but stopped updating) -- the false-green that bars>0 alone misses.
        r"$mx=(@($j.names|ForEach-Object{[long]$_.ts})|Measure-Object -Maximum).Maximum;"
        r"$lag=@($j.names|Where-Object{ ($mx-[long]$_.ts) -gt 129600 });$lagc=$lag.Count;$lagn=(@($lag|ForEach-Object{$_.sym}) -join ' ')"
        r"}else{$zero=-1;$tot=0;$znames='NOSTATE';$lagc=-1;$lagn='NOSTATE'};"
        r"Write-Output ($last+'|'+$cols+'|'+$zero+'|'+$tot+'|'+$znames+'|'+$lagc+'|'+$lagn)"
    )
    # -EncodedCommand (UTF-16LE base64): the probe uses {} | $_ pipes that ssh->cmd.exe
    # ->powershell -Command mangles (that mangling was the rc=255 false-RED on first run).
    # Base64 is immune to every quoting layer. Prefix stays `ssh <VPS_HOST> powershell`.
    enc = base64.b64encode(ps.encode("utf-16-le")).decode()
    try:
        r = subprocess.run(["ssh", VPS_HOST, "powershell", "-NoProfile", "-EncodedCommand", enc],
                           capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.SubprocessError):
        return ("RED", f"ssh {VPS_HOST} failed -- stock feed/book UNVERIFIABLE")
    if r.returncode != 0:
        return ("RED", f"ssh rc={r.returncode} -- stock feed/book UNVERIFIABLE")
    line = next((x for x in r.stdout.splitlines() if "|" in x), "")
    parts = line.split("|")
    if len(parts) != 7:
        return ("RED", f"unparseable stock-book probe: {line!r}")
    last, cols, zero, tot, znames, lagc, lagn = parts
    try:
        z, t, lag = int(zero), int(tot), int(lagc)
    except ValueError:
        z, t, lag = -1, 0, -1
    # (1) per-name coverage — ANY bars=0 name is a dead name (feed missing its column)
    if z < 0:
        return ("RED", "stockladder_companion_state.json missing/unreadable -- book UNVERIFIABLE")
    if z > 0:
        return ("RED", f"{z}/{t} ladder names have bars=0 (feed missing their columns; can't trade): {znames.strip()}")
    # (1b) per-name FRESHNESS — a name with bars>0 but a stale latest-ts is FROZEN (bars>0 alone
    # false-greens it). Caught the 6 backfilled names (WDC/STX/DD/TPR/BMY/SWKS) sitting 2 td behind.
    if lag > 0:
        return ("RED", f"{lag}/{t} ladder names FROZEN (bars>0 but latest bar >2d behind the rest; producer not feeding them): {lagn.strip()}")
    # (2) content-date staleness — last DATA ROW vs last trading day
    try:
        d = dt.date.fromisoformat(last.strip())
    except ValueError:
        return ("RED", f"feed last-row date unparseable ('{last}') -- feed corrupt/empty")
    ltd = last_trading_day(dt.datetime.now(dt.timezone.utc).date())
    behind = trading_days_between(d, ltd)
    if behind > 1:
        return ("RED", f"feed last DATA ROW {d} is {behind} trading days behind {ltd} -- daily-close writer STALLED (mtime can lie; this reads content)")
    return ("PASS", f"feed row {d} (<=1td behind {ltd}), {cols} name-cols, {t}/{t} names have bars>0")


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
            age = trading_days_between(d, ltd)
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
                    vps_rows.append((path, "RED", f"VPS UNREACHABLE (ssh {VPS_HOST} failed) -- feed unverifiable (max {max_age_min}min)"))
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

    # ── Stock daily-close book: CONTENT-date staleness + per-name bars=0 ──────────
    # Runs every day (not weekend-gated): a bars=0 name is a config bug regardless of
    # market hours, and the content-date check is trading-day aware. This is the guard
    # that would have caught the 2026-07-10 "39/45 names, feed frozen" silent failure
    # that the mtime manifest false-greened.
    stock_row = vps_stock_book_health()
    if stock_row[0] != "PASS":
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
            print(f"  -- VPS operational dumps (C:\\Omega\\logs, polled skew-free via ssh {VPS_HOST}) --")
            for path, status, detail in vps_rows:
                print(f"  {status:7} [vps-live] {path:24} {detail}")
        if exec_row:
            print(f"  {exec_row[0]:7} [vps-exec] {'ibkr_exec_status':24} {exec_row[1]}")
        print(f"  {stock_row[0]:7} [vps-stock] {'stock_daily_book':24} {stock_row[1]}")
        if live_red:
            print("  -> A LIVE feed is stale: fix the feeder BEFORE trusting any signal/telemetry.")
        elif res_red:
            print("  -> Research/paper feed stale (rdagent panel). Run: bash tools/rdagent/qlib_refresh.sh")
    return 1 if live_red else (2 if res_red else 0)


if __name__ == "__main__":
    sys.exit(main())
