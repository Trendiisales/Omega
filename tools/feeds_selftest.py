#!/usr/bin/env python3
"""FEEDS FRESHNESS SELF-TEST — RED when ANY data feed is stale.

Built 2026-06-30 after the rdagent qlib model silently froze at 06-25 for days
(IBKR tunnel down + no scheduler + no alarm). The operator's rule: never silently
trade/show stale data again. This is the alarm — like the protection self-test,
it runs every session (SessionStart hook) and screams RED on the first stale feed.

VPS operational-dump section (S-2026-07-02): the local getters below can only see
Mac-side files. The live VPS binary also writes load-bearing CSVs to C:\\Omega\\logs
(set_live_dump targets) that are NEVER pulled to the Mac — so a VPS writer could die
with every Mac banner GREEN. That blind spot is closed by ONE batched `ssh omega-new`
poll that computes each tools/live_dump_manifest.tsv file's age ON the VPS (skew-free)
and REDs a stale LIVE dump. A weekend guard skips enforcement Fri22:00->Sun22:00 UTC
(markets closed). ssh failure => RED (an unverifiable live feed is not GREEN).

Exit codes:  0 = all fresh   1 = a LIVE feed stale (critical)   2 = only research stale
Run:  python3 tools/feeds_selftest.py   [--quiet]
"""
from __future__ import annotations
import base64, csv, datetime as dt, json, os, re, subprocess, sys
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

# Producer scheduled-tasks whose SILENT DISABLE freezes a data feed. The 2026-07-13
# incident: OmegaStockMoverFeed was left Disabled during the 07-07/07-10 box migration,
# so the bigcap daily-close feed froze at 07-09 and NOTHING caught it until the DATA
# aged past its cap 3 days later. This guard catches the disable at the TASK level the
# moment it happens (RED), not 3 days downstream. State==Disabled => RED; also flags a
# task that never ran (last=1999). All are re-armed with StartWhenAvailable so a missed
# fire (reboot/migration) auto-catches-up instead of waiting for the next weekday.
CRITICAL_FEED_TASKS = ["OmegaStockMoverFeed", "OmegaSeedRefresh", "OmegaMacroGoldGate",
                       # S-2026-07-14 sweep P1-1: vix_term_ratio.txt + index_regime.txt
                       # producer (fetch_macro_regime.py) — was never scheduled anywhere.
                       "OmegaMacroRegime"]

# S-2026-07-16: a scheduled task freezes a feed in TWO further ways the old State-only
# check was blind to, BOTH while State stays "Ready" (so State==Ready is NOT proof of a
# live feed): (a) the last run FAILED — LastTaskResult is a nonzero error code — yet a
# stale prior file lingers; (b) the SCHEDULER STOPPED ADVANCING — NextRunTime has slid
# into the past and no run fired (missed trigger not caught up, corrupt trigger, service
# degraded). Both are the exact "producer wired-but-frozen, invisible for days" class the
# operator has been repeatedly burned by. We now RED on either. All time math is computed
# VPS-SIDE (Get-Date on the box) so a Mac↔VPS clock/timezone skew — the very thing that
# masqueraded as a "stale feed" on 2026-07-16 (UTC feed read as NZ-missed-refresh) — can
# never produce a false RED or false GREEN here.
# PASS result codes: 0 = success; 267009 (0x41301) = task currently running.
TASK_RESULT_PASS_CODES = {0, 267009}
# NextRunTime may legitimately sit slightly in the past between the trigger firing and the
# scheduler recomputing it, or while a run is in flight — allow this much slack before RED.
TASK_OVERDUE_SLACK_MIN = 120

# S-2026-07-18ah: WATCHDOG RELAUNCH-LOOP detector (operator ask after the 07-18 incident:
# gateway_watchdog probed paper port 4002 while the gateway ran LIVE on 4001, so every
# 5-min tick relaunched IBC against a healthy session for ~20min with NO alarm — the
# operator saw it via the RDP window, not a banner). Class rule (chimera restart-loop
# S-18ad precedent): any watchdog/task that retries N× consecutively must ALARM, not just
# log. Healthy watchdog ticks log NOTHING, so a dead loop's WARN burst sits at the log
# tail forever — detection therefore needs BOTH density AND recency: >= loop_n alarm
# lines inside window_min AND the newest one <= active_min old (2-3 tick intervals)
# => loop STILL RUNNING => RED. An old burst (e.g. the fixed 06:1x-06:3xZ spam) stays
# PASS with a note. The watchdog task's own LastTaskResult is checked on the same row
# (during a loop every tick exits 1; MISSING task = watchdog not installed = RED).
# All time math vs the VPS's OWN UtcNow fetched in the same ssh call (skew-free).
# Extend this manifest for any future *_watchdog that logs alarm lines with a leading
# ISO-8601 UTC stamp (yyyy-MM-ddTHH:mm:ssZ).
WATCHDOG_LOOP_CHECKS = [
    {
        "task": "IbkrGateway",
        "log": r"C:\Omega\bracket-bot\logs\gateway_watchdog.log",
        # WARN: relaunched but port never came up; ERROR: IBC missing / launch failed.
        "alarm_re": r"\b(?:WARN|ERROR):",
        "loop_n": 3,        # >= this many alarm ticks ...
        "window_min": 30,   # ... inside this window = loop density
        "active_min": 12,   # newest alarm at most this old = loop STILL RUNNING
    },
]


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
        return dt.datetime.fromtimestamp(path.stat().st_mtime, dt.timezone.utc).date()
    except OSError:
        return None


# ── VPS operational-CSV staleness (the Mac-invisible live dumps) ───────────────
# The live VPS binary writes load-bearing CSVs to C:\Omega\logs via set_live_dump().
# None are pulled to the Mac, so the getters above CANNOT see them — a VPS writer
# can die and every Mac banner stays GREEN. This section polls the VPS directly:
# ONE batched `ssh omega-new` call runs PowerShell that computes each manifest
# file's age-in-minutes ON THE VPS (skew-free — no Mac/VPS clock comparison), and
# each age is checked against its manifest max_age_min. A stale LIVE dump => exit 1.
# mtime is safe here: these dumps are append-only and only their writer touches them.

def load_live_dump_manifest() -> list[tuple[str, int, str, str]]:
    """Parse live_dump_manifest.tsv -> [(path_under_logs, max_age_min, description, flags)].
    flags (optional 4th tab column): 'wkday-daily' = the producer only runs on weekdays
    (e.g. OmegaStockMoverFeed 22:35 UTC Mon-Fri), so on Monday — enforcement resumes at
    00:00 UTC but the last write was FRIDAY — the age cap gets +48h weekend grace.
    Without it the row false-REDs every Monday 00:00->22:35 UTC. Tuesday reverts to the
    base cap, so a producer that failed on Monday is still caught within a day."""
    out: list[tuple[str, int, str, str]] = []
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
            flags = parts[3].strip() if len(parts) > 3 else ""
            try:
                out.append((path, int(max_age), desc, flags))
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


def vps_live_dump_ages(manifest: list[tuple[str, int, str, str]]) -> dict[str, float] | None:
    """ONE batched `ssh omega-new` call. Returns {path_under_logs: age_min} computed
    ON the VPS (skew-free). None if the ssh call fails entirely (=> RED). A file that
    is missing on the VPS is reported as a very large age so it REDs against its cap.
    The ssh command MUST start literally `ssh omega-new` (classifier rule; omega-vps alias DISABLED 2026-07-14)."""
    # PowerShell one-liner: for each relative path, print "<path>\t<age_min>" (or a
    # huge age if absent). LastWriteTime age keeps us clock-skew-free vs the Mac.
    files = ";".join(f"'{p}'" for p, *_ in manifest)
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
    """ONE `ssh omega-new` call. Confirms the live binary's IBKR EXECUTION path is
    ACTIVE. Gold now routes to IBKR GC futures (execution_broker==IBKR) and its
    MIN_TRADE_GATE cost basis is rebased onto IBKR (0.5x BlackBull-spot spread).
    If the exec path silently reverts to BLACKBULL_FIX or hits CONNECT FAILED,
    gold orders are rejected AND the cheaper cost gate is mispriced against the
    wrong venue -- a double failure that looks like the strategy died. Scans the
    3 most-recent runtime logs (robust across the UTC date-roll, since the
    [IBKR-EXEC] status prints only at boot) for the latest status line.
    Returns (status, detail). ssh command MUST start literally `ssh omega-new`."""
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


def vps_hash_drift() -> tuple[str, str]:
    """ONE `ssh omega-new` call + one local `git ls-remote`. Three-way deploy-drift
    gate: origin/main == VPS working-tree HEAD == RUNNING binary's stamped hash.
    Built S-2026-07-23 after a detached deploy DIED SILENTLY at launch (no log ever
    created), the launching session hit its context limit, and the VPS ran 6 commits
    behind origin overnight with NOTHING flagging it. This is the alarm that makes
    "deploy never ran" / "built but not restarted" visible the next session start.
    Grace: a push younger than DRIFT_GRACE_SEC (3h) is a deploy-in-flight, not drift."""
    DRIFT_GRACE_SEC = 3 * 3600
    repo = str(HOME / "Omega")
    try:
        lr = subprocess.run(["git", "-C", repo, "ls-remote", "origin", "main"],
                            capture_output=True, text=True, timeout=30)
        origin = lr.stdout.split()[0][:8] if lr.returncode == 0 and lr.stdout.strip() else ""
    except (OSError, subprocess.SubprocessError, IndexError):
        origin = ""
    if not origin:
        return ("RED", "git ls-remote origin main failed -- drift UNVERIFIABLE")
    # VPS side in one call: HEAD + newest stderr 'Git hash:' line (skew-free, box-side).
    ps = (
        r"$h=(git -C C:\Omega rev-parse --short=8 HEAD);"
        r"$g='';$ls=Get-ChildItem 'C:\Omega\logs\omega_service_stderr*.log' -ErrorAction SilentlyContinue;"
        r"$best=0;foreach($f in $ls){$m=Select-String -Path $f.FullName -Pattern 'Git hash: *([0-9a-f]+)';"
        r"if($m -and $f.LastWriteTime.Ticks -gt $best){$best=$f.LastWriteTime.Ticks;$g=$m[-1].Matches[0].Groups[1].Value}}"
        r"Write-Output ($h+[char]9+$g)"
    )
    try:
        r = subprocess.run(["ssh", VPS_HOST, "powershell", "-NoProfile", "-Command", ps],
                           capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.SubprocessError):
        return ("RED", f"ssh {VPS_HOST} failed -- deploy drift UNVERIFIABLE")
    parts = (r.stdout.strip().splitlines() or [""])[-1].split("\t")
    vps_head = parts[0].strip()[:8] if parts else ""
    run_hash = parts[1].strip() if len(parts) > 1 else ""
    if not vps_head:
        return ("RED", "VPS rev-parse HEAD empty -- deploy drift UNVERIFIABLE")
    if not origin.startswith(vps_head) and not vps_head.startswith(origin):
        # how old is the origin tip? young push == deploy legitimately in flight
        try:
            ct = subprocess.run(["git", "-C", repo, "show", "-s", "--format=%ct", origin],
                                capture_output=True, text=True, timeout=15)
            age = int(dt.datetime.now(dt.timezone.utc).timestamp()) - int(ct.stdout.strip())
        except (OSError, subprocess.SubprocessError, ValueError):
            age = DRIFT_GRACE_SEC + 1  # unknown-object => not a fresh local push => no grace
        if age > DRIFT_GRACE_SEC:
            return ("RED", f"VPS HEAD {vps_head} != origin/main {origin} for {age//3600}h "
                           f"-- DEPLOY NEVER RAN (or died silently); redeploy via tools/omega_deploy.sh")
        return ("PASS", f"VPS {vps_head} behind origin {origin} but push is {age//60}min old (deploy window)")
    if run_hash and not (vps_head.startswith(run_hash) or run_hash.startswith(vps_head)):
        return ("RED", f"RUNNING binary {run_hash} != VPS HEAD {vps_head} "
                       f"-- built-not-restarted or restart-from-old-binary; verify per CLAUDE.md Deploy Hygiene")
    return ("PASS", f"origin==VPS-HEAD==running ({origin}{'' if run_hash else '; no Git-hash line found -- tree match only'})")


def vps_gapshort_ledger_health() -> tuple[str, str]:
    """ONE `ssh omega-new` call. GapShortDaily forward-ledger LIVENESS. Built
    S-2026-07-23 after the first live --orders session recorded NOTHING: POSIX
    system("mkdir -p") no-opped on Windows, the ledger ofstream open failed and
    `if(!led_) return;` swallowed every row -- zero errors anywhere (fix 42d7ec67:
    std::filesystem + [DAILY][FATAL-LEDGER] + a SESSION heartbeat row each boot).
    REDs on: (1) ledger file missing while the task has fired; (2) ledger mtime not
    updated by the most recent run (SESSION row makes every run write); (3) a stale
    Running instance >20h old -- it BLOCKS the next scheduled fire (today's 13:30
    entry silently never ran because yesterday's instance was still alive)."""
    ps = (
        r"$ti=Get-ScheduledTaskInfo -TaskName OmegaGapShortDaily -ErrorAction SilentlyContinue;"
        r"$t=Get-ScheduledTask -TaskName OmegaGapShortDaily -ErrorAction SilentlyContinue;"
        r"$now=Get-Date;"
        r"$lastMin=-1;if($ti -and $ti.LastRunTime -gt (Get-Date '2001-01-01')){$lastMin=[int](($now-$ti.LastRunTime).TotalMinutes)};"
        r"$state='ABSENT';if($t){$state=$t.State.ToString()};"
        r"$ledMin=-1;$led=Get-Item 'C:\Omega\data\gapshort\daily_ledger.csv' -ErrorAction SilentlyContinue;"
        r"if($led){$ledMin=[int](($now-$led.LastWriteTime).TotalMinutes)};"
        r"Write-Output ($state+[char]9+$lastMin+[char]9+$ledMin)"
    )
    try:
        r = subprocess.run(["ssh", VPS_HOST, "powershell", "-NoProfile", "-Command", ps],
                           capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.SubprocessError):
        return ("RED", f"ssh {VPS_HOST} failed -- gapshort ledger UNVERIFIABLE")
    parts = (r.stdout.strip().splitlines() or [""])[-1].split("\t")
    if len(parts) != 3:
        return ("RED", f"unparseable gapshort probe: {r.stdout.strip()[:120]!r}")
    state, last_min, led_min = parts[0].strip(), int(parts[1]), int(parts[2])
    if state == "ABSENT":
        return ("RED", "task OmegaGapShortDaily ABSENT -- engine unscheduled")
    if state == "Disabled":
        return ("RED", "task OmegaGapShortDaily Disabled -- engine silently off")
    if state == "Running" and last_min > 20 * 60:
        return ("RED", f"stale Running instance {last_min/60:.0f}h old -- BLOCKS next 13:30 fire; "
                       f"schtasks /end + let tomorrow's trigger run the current exe")
    if last_min < 0:
        return ("PASS", f"task {state}, never fired yet (ledger check idle)")
    if led_min < 0:
        return ("RED", f"task fired {last_min/60:.1f}h ago but data\\gapshort\\daily_ledger.csv MISSING "
                       f"-- forward record silently lost (mkdir/open failure class)")
    if led_min > last_min + 30:
        return ("RED", f"task fired {last_min/60:.1f}h ago but ledger last write {led_min/60:.1f}h ago "
                       f"-- ran WITHOUT writing (SESSION heartbeat absent = old exe or open failure)")
    return ("PASS", f"task {state}, last fire {last_min/60:.1f}h ago, ledger write {led_min/60:.1f}h ago")


def vps_position_parity() -> tuple[str, str]:
    """ONE `ssh omega-new` call. ENGINE-STATE <-> BROKER position parity for the
    stock books. Built S-2026-07-23 (operator: 'why did you not find this
    automatically') after a session had to hand-reconcile the BMY StockTurtle
    position across live.txt / state.json / ibkr_fills.csv -- a human (or AI)
    eyeball was the reconciliation layer, the exact class banned by
    feedback-content-parity-not-just-plumbing. Compares:
      engine-claimed opens: stockdipturtle_*_live.txt (act==1) + dualmom_live.txt
      broker net shares:    logs/trades/ibkr_fills.csv (BOT-SLD per symbol, STK only)
    RED on either direction: engine manages a position the broker doesn't hold
    (phantom -- exit will 'fire' on nothing) or the broker holds shares no engine
    manages (orphan -- nothing will ever exit it). Caveat: fills history begins
    2026-07-22 (first real fill); symbols with '.' (CMDTY/FX aliases) excluded."""
    ps = (
        r"foreach($f in Get-ChildItem 'C:\Omega\stockdipturtle_*_live.txt' -ErrorAction SilentlyContinue){"
        r"$c=(Get-Content $f.FullName -Raw -ErrorAction SilentlyContinue);"
        r"if($c){Write-Output ('SDT'+[char]9+$f.Name+[char]9+$c.Trim())}};"
        r"if(Test-Path 'C:\Omega\dualmom_live.txt'){foreach($l in Get-Content 'C:\Omega\dualmom_live.txt'){"
        r"Write-Output ('DM'+[char]9+$l)}};"
        r"if(Test-Path 'C:\Omega\logs\trades\ibkr_fills.csv'){foreach($l in Get-Content 'C:\Omega\logs\trades\ibkr_fills.csv'){"
        r"Write-Output ('FILL'+[char]9+$l)}}"
    )
    try:
        r = subprocess.run(["ssh", VPS_HOST, "powershell", "-NoProfile", "-Command", ps],
                           capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.SubprocessError):
        return ("RED", f"ssh {VPS_HOST} failed -- position parity UNVERIFIABLE")
    engine_open: dict[str, str] = {}      # SYM -> which engine claims it
    fills_net: dict[str, int] = {}        # SYM -> net shares from fills
    saw_fills_file = False
    for ln in r.stdout.splitlines():
        parts = ln.split("\t")
        if parts[0] == "SDT" and len(parts) >= 3:
            m = re.match(r"stockdipturtle_(dip|turtle)_([a-z0-9]+)_live\.txt", parts[1].strip(), re.I)
            toks = parts[2].split()
            if m and toks and toks[0] == "1":
                engine_open[m.group(2).upper()] = f"Stock{'Dip' if m.group(1).lower()=='dip' else 'Turtle'}"
        elif parts[0] == "DM" and len(parts) >= 2:
            toks = parts[1].split()
            # DualMom rows: OLD "SYM entry ts token" (4) OR NEW "SYM entry ts peak token"
            # (5, S-2026-07-23u profit-lock peak column). RETRY/STAYOUT rows (2 toks) and the
            # leading day_no line (1 numeric tok) are skipped by the len + isdigit guards.
            if len(toks) in (4, 5) and not toks[0].isdigit() and toks[0] not in ("RETRY", "STAYOUT"):
                engine_open[toks[0].upper()] = "DualMom"
        elif parts[0] == "FILL" and len(parts) >= 2:
            saw_fills_file = True
            cols = parts[1].split(",")
            if len(cols) >= 9 and cols[0] != "ts_unix":
                sym, side, qty = cols[4].strip().upper(), cols[6].strip().upper(), cols[7].strip()
                if "." in sym or not qty.isdigit():
                    continue
                fills_net[sym] = fills_net.get(sym, 0) + (int(qty) if side == "BOT" else -int(qty))
    if not saw_fills_file:
        return ("RED", "ibkr_fills.csv missing on VPS -- broker side UNVERIFIABLE")
    phantom = [f"{s}({e})" for s, e in sorted(engine_open.items()) if fills_net.get(s, 0) <= 0]
    orphan = [s for s, n in sorted(fills_net.items()) if n > 0 and s not in engine_open]
    if phantom or orphan:
        bits = []
        if phantom:
            bits.append(f"engine-open but broker-flat (PHANTOM, exit fires on nothing): {','.join(phantom)}")
        if orphan:
            bits.append(f"broker-held but NO engine manages (ORPHAN, nothing exits it): {','.join(orphan)}")
        return ("RED", "; ".join(bits))
    n_open = len(engine_open)
    return ("PASS", f"{n_open} engine-open position(s) == broker fills net "
                    f"({', '.join(f'{s}:{engine_open[s]}' for s in sorted(engine_open)) or 'none open'})")


def vps_stock_book_health() -> tuple[str, str]:
    """ONE `ssh omega-new` call. CONTENT-based (not mtime) integrity of the stock
    daily-close feed (sp500_long_close.csv — consumed by the live DualMom poller +
    daily stock books). Catches the silent failure the mtime manifest CANNOT and
    that burned us on 2026-07-10: a feed whose file mtime is fresh but whose last
    DATA ROW is days old (the writer touches/rewrites without appending a new
    dated close). RED if the feed's last data row is >1 trading day behind.
    History S-2026-07-23g: this probe also read a per-name ladder state json, but
    that book was retired S-16l — the leftover empty file made the check a
    permanent false RED (PowerShell null-pipe artifact reported '1/1 bars=0' with
    an empty name list). Per-name coverage now belongs to the consumers' own boot
    lines (e.g. [DUALMOM] seed done: N names with history)."""
    ps = (
        r"$f='C:\Omega\data\rdagent\sp500_long_close.csv';"
        r"if(Test-Path $f){$l=Get-Content $f;$last=$l[-1].Split(',')[0];$cols=($l[0].Split(',').Count-1)}else{$last='MISSING';$cols=0};"
        r"Write-Output ($last+'|'+$cols)"
    )
    # -EncodedCommand (UTF-16LE base64): immune to every ssh->cmd.exe quoting layer.
    # Prefix stays `ssh <VPS_HOST> powershell`.
    enc = base64.b64encode(ps.encode("utf-16-le")).decode()
    try:
        r = subprocess.run(["ssh", VPS_HOST, "powershell", "-NoProfile", "-EncodedCommand", enc],
                           capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.SubprocessError):
        return ("RED", f"ssh {VPS_HOST} failed -- stock feed UNVERIFIABLE")
    if r.returncode != 0:
        return ("RED", f"ssh rc={r.returncode} -- stock feed UNVERIFIABLE")
    line = next((x for x in r.stdout.splitlines() if "|" in x), "")
    parts = line.split("|")
    if len(parts) != 2:
        return ("RED", f"unparseable stock-feed probe: {line!r}")
    last, cols = parts
    # content-date staleness — last DATA ROW vs last trading day
    try:
        d = dt.date.fromisoformat(last.strip())
    except ValueError:
        return ("RED", f"feed last-row date unparseable ('{last}') -- feed corrupt/empty")
    ltd = last_trading_day(dt.datetime.now(dt.timezone.utc).date())
    behind = trading_days_between(d, ltd)
    if behind > 1:
        return ("RED", f"feed last DATA ROW {d} is {behind} trading days behind {ltd} -- daily-close writer STALLED (mtime can lie; this reads content)")
    return ("PASS", f"feed row {d} (<=1td behind {ltd}), {cols} name-cols")


# ── SHARED FEED THRESHOLD REGISTRY — SINGLE SOURCE OF TRUTH (S-2026-07-14 sweep item 9) ──
# tools/data_health_monitor.py monitors the SAME files as the FEEDS table below. The two
# tables were hand-synced duplicates and drifted CONTRADICTORY (this file: 2 trading-days;
# data_health: 4 calendar-days — a feed could be RED here and GO there). Derive-don't-copy:
# every shared file's threshold is defined ONCE here, in TRADING DAYS (calendar days
# false-RED every Monday/post-holiday — see trading_days_between docstring), and
# data_health_monitor IMPORTS this dict + the trading-day helpers above. Do NOT re-type
# these numbers anywhere else; add new shared feeds HERE and reference the key.
#
# Threshold decisions at unification (stricter/correct value kept per file):
#   XAU/DJ30/SPX/NDX daily + qlib calendar: 2 td (this file's value; STRICTER than
#     data_health's old 4 calendar — data_health tightened to match).
#   GER40 daily: 4 td kept (deliberate since birth: trading_days_between uses the US
#     holiday calendar, so DE-only holidays inflate GER40's apparent td-age; 2 td would
#     false-RED around German holidays). data_health moves 4 cal -> 4 td, marginally
#     later alarm across a weekend, in exchange for killing that false-RED class.
#   sp500_long_close: file-level 4 td (this file's value). data_health's per-column
#     check keeps its own two knobs below (intra-file column lag — calendar days are
#     correct there because all columns share the same date index).
#   companion telemetry / crypto heartbeat: 1 td (feeds_selftest values; newly ALSO
#     enrolled in data_health for 10-15min launchd cadence, not just session start).
SHARED_FEED_MAX_AGE_TD = {
    "tick.XAU_daily":           2,
    "tick.DJ30_daily":          2,
    "tick.SPX_daily":           2,
    "tick.NDX_daily":           2,
    "tick.GER40_daily":         4,
    "rdagent.qlib_calendar":    2,
    "rdagent.sp500_long_close": 4,   # file-level last-row age
    "companion.telemetry":      1,
    "crypto.heartbeat":         1,
}
# sp500_long_close per-column knobs (data_health's percol check): a column is stale if its
# last non-empty cell lags the file's newest date by > LAG_MAX_D; alarm when the stale
# fraction exceeds MAX_STALE_FRAC. Single definition — data_health imports these too.
SP500_PERCOL_LAG_MAX_D = 5
SP500_PERCOL_MAX_STALE_FRAC = 0.15

# Coverage division vs data_health_monitor (deliberate, do not blindly mirror):
#   - macro_gold_gate.tsv: the LIVE gate input is the VPS-side copy, already polled here
#     via live_dump_manifest.tsv (26h row) + the OmegaMacroGoldGate task check; data_health
#     watches the Mac-side producer copy. No duplicate Mac-side row here.
#   - mgc_h1/mgc_30m mirror seeds: owned by data_health (7d) + seed_freshness_audit.py +
#     the OmegaSeedRefresh task check here. Not duplicated.
#   - VPS ssh checks (live dumps / exec path / tasks) live ONLY here: data_health runs on a
#     10-15min launchd cadence and must stay ssh-free (operator rule: minimize VPS ssh).

# (label, kind, max_age_trading_days, getter)  kind: "live" critical | "research" paper
# max_age values for shared files come from SHARED_FEED_MAX_AGE_TD — never literals.
FEEDS = [
    ("tick XAUUSD daily",   "live", SHARED_FEED_MAX_AGE_TD["tick.XAU_daily"],   lambda: csv_last_date(TICK / "2yr_XAUUSD_daily.csv")),
    ("tick DJ30 daily",     "live", SHARED_FEED_MAX_AGE_TD["tick.DJ30_daily"],  lambda: csv_last_date(TICK / "DJ30_daily_2016_2026.csv")),
    ("tick SPX daily",      "live", SHARED_FEED_MAX_AGE_TD["tick.SPX_daily"],   lambda: csv_last_date(TICK / "SPX_daily_2016_2026.csv")),
    ("tick NDX daily",      "live", SHARED_FEED_MAX_AGE_TD["tick.NDX_daily"],   lambda: csv_last_date(TICK / "NDX_daily_2016_2026.csv")),
    ("tick GER40 daily",    "live", SHARED_FEED_MAX_AGE_TD["tick.GER40_daily"], lambda: csv_last_date(TICK / "GER40_daily_2016_2026.csv")),
    ("companion telemetry", "live", SHARED_FEED_MAX_AGE_TD["companion.telemetry"], lambda: mtime_date(STALL / "companion_state.json")),
    ("crypto book heartbeat","live", SHARED_FEED_MAX_AGE_TD["crypto.heartbeat"], crypto_book_heartbeat),
    ("qlib omega_data",     "research", SHARED_FEED_MAX_AGE_TD["rdagent.qlib_calendar"], qlib_cal_last),
    ("rdagent basket",      "research", 2, lambda: json_field_date(RDA / "latest.json", "signal", "date")),
    ("sp500_long_close",    "research", SHARED_FEED_MAX_AGE_TD["rdagent.sp500_long_close"], lambda: csv_last_date(RDA / "sp500_long_close.csv")),
    # NB: sp500_close.csv is a dead fallback — serve.py:26 reads sp500_long_close first and never
    # reaches it. Not fed by any refresher, so tracking it = false AMBER. Excluded deliberately.
]


def vps_feed_task_health() -> list[tuple[str, str, str]]:
    """ONE `ssh omega-new` call. For each CRITICAL_FEED_TASKS producer, returns
    (task, status, detail). REDs on ALL FOUR silent-freeze modes — three of which
    leave State=="Ready", so State alone is NOT proof of a live feed:
      1. State==Disabled  — producer disabled (the 2026-07-13 migration failure: a
         disabled task froze the bigcap feed for days, invisible until data aged out).
      2. MISSING / never-run (last=1999) — producer wired but idle.
      3. LastTaskResult != 0 (and != running) — the last run ERRORED; a stale prior
         file lingers while State stays Ready.
      4. NextRunTime slid > TASK_OVERDUE_SLACK_MIN into the PAST — the scheduler stopped
         advancing (missed trigger not caught up / corrupt trigger), so no run fires
         though State stays Ready.
    Modes 3 & 4 are the early-warning the old State-only check was blind to. ALL time
    math is computed VPS-side (Get-Date on the box) so a Mac↔VPS clock/tz skew cannot
    yield a false RED/GREEN. ssh MUST start literally `ssh omega-new`. Single RED on ssh
    failure."""
    names = ",".join(f"'{t}'" for t in CRITICAL_FEED_TASKS)
    ps = (
        f"$now=Get-Date;"
        f"foreach($n in @({names})){{"
        f"$t=Get-ScheduledTask -TaskName $n -ErrorAction SilentlyContinue;"
        f"if($null-eq$t){{Write-Output ($n+[char]9+'MISSING'+[char]9+'-'+[char]9+'-'+[char]9+'NA')}}"
        f"else{{$i=Get-ScheduledTaskInfo -TaskName $n -ErrorAction SilentlyContinue;"
        f"$od=if($i.NextRunTime){{[string][math]::Round((($now-$i.NextRunTime).TotalMinutes))}}else{{'NA'}};"
        f"Write-Output ($n+[char]9+$t.State+[char]9+$i.LastRunTime+[char]9+[string]$i.LastTaskResult+[char]9+$od)}}}}"
    )
    try:
        r = subprocess.run(
            ["ssh", VPS_HOST, "powershell", "-NoProfile", "-Command", ps],
            capture_output=True, text=True, timeout=45,
        )
    except (OSError, subprocess.SubprocessError):
        return [("feed-producer-tasks", "RED", f"ssh {VPS_HOST} failed -- producer states unverifiable")]
    if r.returncode != 0:
        return [("feed-producer-tasks", "RED", f"ssh {VPS_HOST} rc={r.returncode} -- producer states unverifiable")]
    out: list[tuple[str, str, str]] = []
    seen = set()
    for ln in r.stdout.splitlines():
        parts = ln.strip().split("\t")
        if len(parts) < 2:
            continue
        name, state = parts[0].strip(), parts[1].strip()
        last = parts[2].strip() if len(parts) > 2 else "-"
        result = parts[3].strip() if len(parts) > 3 else ""
        overdue = parts[4].strip() if len(parts) > 4 else "NA"
        seen.add(name)
        if state == "Disabled":
            out.append((name, "RED", f"scheduled task DISABLED -- feed will freeze silently (re-enable + StartWhenAvailable)"))
            continue
        if state == "MISSING":
            out.append((name, "RED", f"scheduled task MISSING on {VPS_HOST} -- producer never installed"))
            continue
        if last.startswith("11/30/1999") or last in ("", "-"):
            out.append((name, "RED", f"scheduled task never ran (last={last}) -- producer wired but idle"))
            continue
        # mode 3: last run errored (result code nonzero and not "currently running")
        try:
            rc = int(result)
        except ValueError:
            rc = 0  # unparseable -> don't manufacture a RED on the result axis
        if rc not in TASK_RESULT_PASS_CODES:
            out.append((name, "RED", f"last run FAILED (LastTaskResult={result}/0x{rc & 0xFFFFFFFF:X}) -- producer errored though State={state}; feed frozen at last-good until fixed"))
            continue
        # mode 4: scheduler stopped advancing (NextRunTime slid into the past)
        try:
            od = float(overdue)
        except ValueError:
            od = None
        if od is not None and od > TASK_OVERDUE_SLACK_MIN:
            out.append((name, "RED", f"scheduler OVERDUE by {od/60:.1f}h -- NextRunTime already passed with no run (trigger not advancing / silent freeze; last ran {last})"))
            continue
        if od is None:
            nextinfo = "next=?"
        elif od <= 0:
            nextinfo = f"next in {(-od)/60:.1f}h"
        else:
            nextinfo = f"next {od:.0f}m overdue (within {TASK_OVERDUE_SLACK_MIN}m slack)"
        out.append((name, "PASS", f"task {state}, last ran {last} (rc={result}), {nextinfo}"))
    for t in CRITICAL_FEED_TASKS:
        if t not in seen:
            out.append((t, "RED", f"no state returned -- producer unverifiable"))
    return out


def vps_watchdog_loop_health() -> list[tuple[str, str, str]]:
    """ONE `ssh omega-new` call. For each WATCHDOG_LOOP_CHECKS entry, returns
    (task, status, detail). REDs on:
      1. watchdog task MISSING — watchdog not installed;
      2. LastTaskResult nonzero (and not running) — last watchdog tick errored
         (during a relaunch loop every tick exits 1, so this fires too);
      3. RELAUNCH LOOP ACTIVE — >= loop_n WARN:/ERROR: lines within window_min
         AND newest <= active_min old. Density alone is NOT enough: healthy ticks
         log nothing, so a long-dead loop's burst sits at the tail forever — the
         recency arm keeps that a PASS (with history note) instead of a stuck RED.
    Ages computed against the VPS's own UtcNow from the same ssh call."""
    ps = "$u=[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ');Write-Output ('NOW'+[char]9+$u);"
    for c in WATCHDOG_LOOP_CHECKS:
        t = c["task"]
        ps += (
            f"$t=Get-ScheduledTask -TaskName '{t}' -ErrorAction SilentlyContinue;"
            f"if($null-eq$t){{Write-Output ('TASK'+[char]9+'{t}'+[char]9+'MISSING')}}"
            f"else{{$i=Get-ScheduledTaskInfo -TaskName '{t}' -ErrorAction SilentlyContinue;"
            f"Write-Output ('TASK'+[char]9+'{t}'+[char]9+[string]$i.LastTaskResult)}};"
            f"Get-Content '{c['log']}' -Tail 40 -ErrorAction SilentlyContinue|"
            f"ForEach-Object {{Write-Output ('LOG'+[char]9+'{t}'+[char]9+$_)}};"
        )
    # rc must reflect ssh transport only -- a suppressed cmdlet error must not mask
    # the parsed output (missing task/log verdicts come from the TASK/LOG lines).
    ps += "exit 0;"
    # -EncodedCommand (UTF-16LE base64): the log-relay ForEach uses | pipes that
    # ssh->cmd.exe would otherwise split out of the powershell string (rc=255).
    enc = base64.b64encode(ps.encode("utf-16-le")).decode()
    try:
        r = subprocess.run(
            ["ssh", VPS_HOST, "powershell", "-NoProfile", "-EncodedCommand", enc],
            capture_output=True, text=True, timeout=45,
        )
    except (OSError, subprocess.SubprocessError):
        return [("watchdog-loops", "RED", f"ssh {VPS_HOST} failed -- watchdog loop states unverifiable")]
    if r.returncode != 0:
        return [("watchdog-loops", "RED", f"ssh {VPS_HOST} rc={r.returncode} -- watchdog loop states unverifiable")]
    vps_now: dt.datetime | None = None
    task_rc: dict[str, str] = {}
    log_lines: dict[str, list[str]] = {}
    for ln in r.stdout.splitlines():
        parts = ln.rstrip("\r\n").split("\t", 2)
        if parts[0] == "NOW" and len(parts) >= 2:
            try:
                vps_now = dt.datetime.strptime(parts[1].strip(), "%Y-%m-%dT%H:%M:%SZ")
            except ValueError:
                pass
        elif parts[0] == "TASK" and len(parts) >= 3:
            task_rc[parts[1]] = parts[2].strip()
        elif parts[0] == "LOG" and len(parts) >= 3:
            log_lines.setdefault(parts[1], []).append(parts[2])
    out: list[tuple[str, str, str]] = []
    for c in WATCHDOG_LOOP_CHECKS:
        t = c["task"]
        rc_s = task_rc.get(t)
        if rc_s is None:
            out.append((t, "RED", "no task state returned -- watchdog unverifiable"))
            continue
        if rc_s == "MISSING":
            out.append((t, "RED", f"watchdog task MISSING on {VPS_HOST} -- nothing keeps the gateway alive"))
            continue
        try:
            rc = int(rc_s)
        except ValueError:
            rc = 0
        if rc not in TASK_RESULT_PASS_CODES:
            out.append((t, "RED", f"watchdog last tick FAILED (LastTaskResult={rc_s}) -- relaunch loop or broken watchdog; read {c['log']}"))
            continue
        if vps_now is None:
            out.append((t, "RED", "VPS clock line unparseable -- loop recency unverifiable"))
            continue
        alarm_re = re.compile(c["alarm_re"])
        alarms: list[dt.datetime] = []
        for line in log_lines.get(t, []):
            if not alarm_re.search(line):
                continue
            try:
                ts = dt.datetime.strptime(line[:20], "%Y-%m-%dT%H:%M:%SZ")
            except ValueError:
                continue
            alarms.append(ts)
        if not alarms:
            out.append((t, "PASS", f"rc={rc_s}, log tail clean (no WARN/ERROR)"))
            continue
        newest = max(alarms)
        newest_age = (vps_now - newest).total_seconds() / 60.0
        in_window = [a for a in alarms if (vps_now - a).total_seconds() / 60.0 <= c["window_min"]]
        if len(in_window) >= c["loop_n"] and newest_age <= c["active_min"]:
            out.append((t, "RED", f"RELAUNCH LOOP ACTIVE: {len(in_window)} alarm ticks in {c['window_min']}min, newest {newest_age:.0f}min ago -- watchdog is fighting a healthy/broken service; read {c['log']}"))
        else:
            out.append((t, "PASS", f"rc={rc_s}, no active loop (last alarm {newest_age/60:.1f}h ago, {len(in_window)} in {c['window_min']}min window)"))
    return out


def main() -> int:
    quiet = "--quiet" in sys.argv
    # UTC, not box-local: Mac runs NZ (UTC+12) — bare date.today() flips the
    # trading day ~12h early and false-STALEs every live feed (same idiom as
    # the per-feed helper above).
    today = dt.datetime.now(dt.timezone.utc).date()
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
            for path, max_age_min, desc, _flags in manifest:
                vps_rows.append((path, "SKIP", f"weekend (market closed) -- age not enforced (max {max_age_min}min)"))
        else:
            ages = vps_live_dump_ages(manifest)
            if ages is None:
                # ssh unreachable / failed entirely -> cannot verify a load-bearing
                # live feed -> treat as RED (each manifested LIVE dump is unverifiable).
                live_red += len(manifest)
                for path, max_age_min, desc, _flags in manifest:
                    vps_rows.append((path, "RED", f"VPS UNREACHABLE (ssh {VPS_HOST} failed) -- feed unverifiable (max {max_age_min}min)"))
            else:
                for path, max_age_min, desc, flags in manifest:
                    # wkday-daily producers last wrote FRIDAY; Monday needs +48h weekend
                    # grace or the row false-REDs all Monday until the evening run.
                    eff_max = max_age_min
                    grace = ""
                    if "wkday-daily" in flags and now_utc.weekday() == 0:
                        eff_max = max_age_min + 48 * 60
                        grace = " +48h Mon grace (wkday-daily)"
                    age_min = ages.get(path)
                    if age_min is None:
                        live_red += 1
                        vps_rows.append((path, "RED", f"no age returned for {path} -- writer/path missing (max {max_age_min}min)"))
                    elif age_min > eff_max:
                        live_red += 1
                        vps_rows.append((path, "RED", f"age={age_min:.0f}min > max {eff_max}min{grace} -- VPS writer stalled ({desc})"))
                    else:
                        vps_rows.append((path, "PASS", f"age={age_min:.0f}min (max {eff_max}min{grace}) {desc}"))

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

    # ── VPS producer scheduled-task health (catches a silent DISABLE at the source) ──
    # Runs every day incl. weekend: a task Disabled on Saturday is still a Monday-morning
    # stale feed. This is the guard the operator asked for after the 07-13 StockMoverFeed
    # was found Disabled since the migration with nothing flagging it.
    task_rows = vps_feed_task_health()
    for _tname, _tstat, _td in task_rows:
        if _tstat != "PASS":
            live_red += 1

    # ── Watchdog relaunch-loop detector (S-2026-07-18ah, operator ask) ─────────────
    # Runs every day incl. weekend: the 07-18 IBC relaunch loop fired on a Saturday.
    wdog_rows = vps_watchdog_loop_health()
    for _wname, _wstat, _wd in wdog_rows:
        if _wstat != "PASS":
            live_red += 1

    # ── Deploy hash-drift gate (S-2026-07-23: silent dead deploy, VPS 6 behind) ────
    # Runs every day incl. weekend: drift is drift regardless of market hours.
    drift_row = vps_hash_drift()
    if drift_row[0] != "PASS":
        live_red += 1

    # ── GapShort forward-ledger liveness (S-2026-07-23: ledger silently lost) ──────
    gapled_row = vps_gapshort_ledger_health()
    if gapled_row[0] != "PASS":
        live_red += 1

    # ── Engine<->broker position parity (S-2026-07-23: no more eyeball reconcile) ──
    pospar_row = vps_position_parity()
    if pospar_row[0] != "PASS":
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
        for tname, tstat, tdetail in task_rows:
            print(f"  {tstat:7} [vps-task ] {tname:24} {tdetail}")
        for wname, wstat, wdetail in wdog_rows:
            print(f"  {wstat:7} [vps-wdog ] {wname:24} {wdetail}")
        print(f"  {drift_row[0]:7} [vps-drift] {'deploy_hash_drift':24} {drift_row[1]}")
        print(f"  {gapled_row[0]:7} [vps-gapled] {'gapshort_fwd_ledger':23} {gapled_row[1]}")
        print(f"  {pospar_row[0]:7} [vps-pospar] {'engine_broker_parity':23} {pospar_row[1]}")
        if live_red:
            print("  -> A LIVE feed is stale: fix the feeder BEFORE trusting any signal/telemetry.")
        elif res_red:
            print("  -> Research/paper feed stale (rdagent panel). Run: bash tools/rdagent/qlib_refresh.sh")
    return 1 if live_red else (2 if res_red else 0)


if __name__ == "__main__":
    sys.exit(main())
