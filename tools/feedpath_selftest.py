#!/usr/bin/env python3
"""FEED-PATH SELFTEST — silent-fallback detector (S-2026-07-08c, operator order).

WHY THIS EXISTS: the 2026-07-07 VPS migration silently dropped the Omega service's
AppEnvironmentExtra (OMEGA_IBKR_BRIDGE=1). The IBKR consumer thread never started on
the new box; five FX pairs degraded invisibly to the BlackBull fallback and the only
symptom was USDCAD (IBKR-only) never printing a bar. A fallback that works is exactly
what makes a dead primary invisible — so this test asserts the PRIMARY path end-to-end
and screams when anything rides a fallback.

CHECKS (all must pass in market hours):
  [1] SERVICE-ENV   registry AppEnvironmentExtra carries every required env
                    (OMEGA_IBKR_BRIDGE=1 + OMEGA_BIGCAP_BRIDGE=1 — extend
                    REQUIRED_ENVS when new ones ship).
  [2] CONSUMER-UP   current boot stdout has an [IBKR-CONSUMER] line AND port 9701
                    shows an ESTABLISHED pair (listener alone = consumer dead).
  [3] BRIDGE-FRESH  today's bridge L1 csv for an IBKR-ONLY canary (USDCAD) is
                    growing (mtime <= 20 min in FX hours). IBKR-only symbols are the
                    canaries precisely because no fallback can mask them.
  [4] DOWNSTREAM    the FX-ladder book advances (bridge -> consumer -> dispatch -> H1
                    roll -> book). S-2026-07-24: was a single HTTP endpoint that ROTTED
                    (all 3 ladder panels 404 in the build) -> permanent false-RED. Now
                    classifies: API OK+fresh=PASS; endpoint 404 but state file fresh=PASS
                    (book alive, panel gone); API server unreachable OR state file stale=RED.
  [5] NO-FALLBACK   if [3] fresh but [2] fails => "PRIMARY DOWN — RUNNING ON
                    FALLBACK" (the exact silent state this test exists to kill).
  [6] BIGCAP-CONSUMER  the bigcap L1 bridge (:7784) heartbeat shows consumer=Y — the
                    binary is consuming live mids so the in-binary daily-close writer
                    is fed. consumer=N => sp500_long_close.csv freezes and the bigcap
                    mimic ladder silently stops firing (S-2026-07-16 root cause: the
                    Omega service env was missing OMEGA_BIGCAP_BRIDGE=1 — the SAME
                    missing-env failure class as [1], for a bridge nothing guarded).
  [7] ORDER-ROUTE   OUTCOME (S-2026-07-24): do live orders actually REACH THE BROKER? The
                    feed path is worthless if the exec socket (:4001, SEPARATE from the
                    :9701 market-data bridge) is down. RED on an [IBKR-EXEC] reject storm
                    (201/460/10289), a BLOCK storm ('not connected' + 0 fills), or intents-
                    sent-but-0-fills. Caught the 2026-07-24 disconnected-exec state that ran
                    all day with [1]-[6] GREEN. SKIP when no intents fired (quiet market).

Exit 0 GREEN / 2 RED. Run from SessionStart hook + Mac cron (30 min, notification
on RED via install_feedpath_cron.sh). FX weekend (Sat/Sun UTC + Fri>=22 UTC) skips
freshness checks honestly (reports SKIP, not PASS).
"""
import base64
import datetime
import json
import re
import subprocess
import sys
import urllib.error
import urllib.request

VPS = "omega-new"
# S-2026-07-10 (G1, registry): derive the desk URL from the ssh alias's HostName so a VPS
# re-IP or cutover is ONE edit (~/.ssh/config), not a hardcoded IP that silently rots. Falls
# back to the last-known IP if ssh -G is unavailable. Override with OMEGA_DESK_URL.
def _desk_url() -> str:
    import os
    if os.environ.get("OMEGA_DESK_URL"):
        return os.environ["OMEGA_DESK_URL"]
    try:
        out = subprocess.run(["ssh", "-G", VPS], capture_output=True, text=True, timeout=5).stdout
        for ln in out.splitlines():
            if ln.startswith("hostname "):
                return f"http://{ln.split()[1]}:7779"
    except Exception:
        pass
    return "http://45.85.3.79:7779"
DESK = _desk_url()
REQUIRED_ENVS = ["OMEGA_IBKR_BRIDGE=1", "OMEGA_BIGCAP_BRIDGE=1"]
# S-2026-07-16: the bigcap L1 bridge (:7784) publishes its own heartbeat with a consumer=Y/N
# flag + a [BIGCAP-ALERT] when the binary isn't consuming. Read the LAST heartbeat: single-quoted
# PowerShell literals only (no nested double-quotes) so the ssh->cmd->powershell hop can't mangle
# it. Output form "<log_age_min>~<last HB line>".
_BIGCAP_HB_CMD = (
    "powershell -NoProfile -Command "
    "\"$f='C:\\Omega\\logs\\bigcap_bridge.log'; "
    "$a=[int]((Get-Date).ToUniversalTime()-(Get-Item $f).LastWriteTimeUtc).TotalMinutes; "
    "$l=(Get-Content $f -Tail 40 | Select-String 'HB ' | Select-Object -Last 1); "
    "Write-Output ([string]$a + '~' + [string]$l)\""
)
CANARY_L1 = "USDCAD"          # IBKR-only: no fallback can mask a dead primary (bridge L1 freshness)
# S-2026-07-10: DOWNSTREAM checks the ACTIVE ladder pair. USDCAD was disabled from the FX ladder
# (S-2026-07-09c: FAIL all-6 on 3Y IBKR; only GBPUSD passes) -> its ladder ts never advances ->
# the downstream check was PERMANENTLY RED (false alarm). GBPUSD is the only live FX ladder pair,
# so its book-advance is what proves bridge->consumer->H1->book. (CANARY_L1 stays USDCAD: the raw
# bridge L1 freshness is still the IBKR-only primary-death detector, independent of the ladder.)
LADDER_PAIR = "GBPUSD"

def ssh(cmd, timeout=25):
    try:
        r = subprocess.run(["ssh", VPS, cmd], capture_output=True, text=True, timeout=timeout)
        return r.stdout
    except Exception as e:
        return f"__SSH_ERR__ {e}"

# S-2026-07-24 downstream state file: the /api/fxladder_companion HTTP endpoint was
# UNREGISTERED in the running build (all three mimic-ladder panels 404) while the book
# itself keeps writing this state file every bar. So the HTTP 404 was a ROTTED-PROXY
# false-RED, not a routing break. The TRUE book-advance outcome is this file's mtime.
FXLADDER_STATE = r"C:\Omega\fxladder_companion_state.json"

def state_file_age_min(path):
    """Age (minutes) of an on-box file's last-write, via dir (quoting-proof over ssh).
    Returns float minutes, or None if the file is missing/unreadable."""
    din = ssh("dir " + path + " 2>nul")
    for ln in din.splitlines():
        parts = ln.split()
        if len(parts) >= 4 and (parts[-1].endswith(".json") or parts[-1].endswith(".csv")):
            try:
                ampm = parts[2] if parts[2] in ("AM", "PM") else ""
                tstr = parts[1] + ((" " + ampm) if ampm else "")
                fmt = "%m/%d/%Y %I:%M %p" if ampm else "%m/%d/%Y %H:%M"
                mt = datetime.datetime.strptime(parts[0] + " " + tstr, fmt).replace(
                    tzinfo=datetime.timezone.utc)
                return (datetime.datetime.now(datetime.timezone.utc) - mt).total_seconds() / 60.0
            except Exception:
                return None
    return None

def exec_order_health():
    """OUTCOME: are LIVE orders actually landing at the broker? The whole feed path is
    pointless if the exec socket is down and every order blocks. S-2026-07-24: the IBKR
    execution socket dropped and EVERY NAS100 order was BLOCKED 'not connected' (0 fills)
    all day while feeds (this test's [1]-[6]) stayed GREEN — the market-data bridge (:9701)
    is a SEPARATE socket from the exec broker socket (:4001). This is the 'to the point
    orders can route' end-to-end check. Reads a recent window of the live stdout log for
    [IBKR-EXEC] placed/fill/reject/blocked. Returns dict or None."""
    ps = (r"$f='C:\Omega\logs\omega_service_stdout.log'; "
          r"$s = Get-Content $f -Tail 4000 -ErrorAction SilentlyContinue; "
          r"if (-not $s) { Write-Output 'EXEC UNREADABLE'; exit } "
          r"$placed  = ($s | Select-String -Pattern 'IBKR-EXEC.*\(eng=').Count; "
          r"$fills   = ($s | Select-String -SimpleMatch '[IBKR-EXEC] FILL ').Count; "
          r"$rejects = ($s | Select-String -Pattern 'IBKR-EXEC. err (201|460|10289)').Count; "
          r"$blocked = ($s | Select-String -SimpleMatch '[IBKR-EXEC] BLOCKED').Count; "
          r"$rl = ($s | Select-String -SimpleMatch '[IBKR-EXEC] BLOCKED' | Select-Object -Last 1).Line; "
          r"Write-Output ('EXEC placed=' + $placed + ' fills=' + $fills + ' rejects=' + $rejects + "
          r"' blocked=' + $blocked + ' last=' + $rl)")
    b64 = base64.b64encode(ps.encode("utf-16-le")).decode()
    out = ssh("powershell -NoProfile -EncodedCommand " + b64, timeout=40)
    m = re.search(r'EXEC placed=(\d+) fills=(\d+) rejects=(\d+) blocked=(\d+)(?: last=(.*))?', out)
    if not m:
        return None
    reason = ""
    if m.group(5):
        rm = re.search(r'--\s*(.+)$', m.group(5).strip())
        reason = rm.group(1).strip() if rm else m.group(5).strip()
    d = dict(placed=int(m.group(1)), fills=int(m.group(2)),
             rejects=int(m.group(3)), blocked=int(m.group(4)), reason=reason)
    d["intents"] = d["placed"] + d["blocked"]
    return d

def fx_market_open(now=None):
    # FX week = Sun 17:00 -> Fri 17:00 America/New_York (5pm NY close, DST-aware).
    # The old fixed "Fri >= 22 UTC" window was winter-only: in July (EDT) the market
    # closes 21:00 UTC, so 21:00-22:00 Fri produced a false BRIDGE-FRESH RED
    # (2026-07-18: USDCAD "stale 50min" at 21:50 UTC — last tick was 21:00:00, the close).
    now = now or datetime.datetime.now(datetime.timezone.utc)
    try:
        from zoneinfo import ZoneInfo
        ny = now.astimezone(ZoneInfo("America/New_York"))
        wd, hr = ny.weekday(), ny.hour
        if wd == 4 and hr >= 17: return False     # Fri post-close (5pm NY)
        if wd == 5: return False                  # Sat
        if wd == 6 and hr < 17: return False      # Sun pre-open (5pm NY)
        return True
    except Exception:
        wd, hr = now.weekday(), now.hour          # fallback: old fixed-UTC window
        if wd == 5: return False
        if wd == 6 and hr < 21: return False
        if wd == 4 and hr >= 21: return False     # conservative: earliest (summer) close
        return True

def main():
    checks, red = [], False
    def rec(tag, ok, msg, skip=False):
        nonlocal red
        state = "SKIP" if skip else ("PASS" if ok else "FAIL")
        if not ok and not skip: red = True
        checks.append(f"  {state:4s} [{tag}] {msg}")

    open_now = fx_market_open()

    # [1] service env
    reg = ssh(r'reg query "HKLM\SYSTEM\CurrentControlSet\Services\Omega\Parameters" /v AppEnvironmentExtra 2>nul')
    missing = [e for e in REQUIRED_ENVS if e not in reg]
    rec("SERVICE-ENV", not missing,
        f"AppEnvironmentExtra carries {REQUIRED_ENVS}" if not missing
        else f"MISSING {missing} — consumer thread will NOT start (the 07-07 migration failure class)")

    # [2] consumer up: boot line + established pair
    con_line = ssh(r'findstr /C:"IBKR-CONSUMER" C:\Omega\logs\omega_service_stdout.log')
    est = ssh("netstat -ano | findstr :9701 | findstr ESTABLISHED")
    boot_ok = "IBKR-CONSUMER" in con_line
    est_ok  = "ESTABLISHED" in est
    consumer_ok = boot_ok and est_ok
    if not open_now:
        # S-2026-07-11: FX closed (weekend) -> IBKR gateway/broker down -> the 9701 socket is legitimately
        # NOT ESTABLISHED. Require only the consumer BOOT LINE (thread configured); it re-establishes at
        # market open. Skipping the socket demand stops the weekend false-RED (operator: no weekend alarm).
        rec("CONSUMER-UP", boot_ok,
            "consumer boot line present; 9701 down (weekend, broker closed) — re-establishes at open" if boot_ok
            else "consumer boot line MISSING — consumer thread never started", skip=boot_ok)
    else:
        msg = ("consumer boot line + 9701 ESTABLISHED" if consumer_ok
               else f"boot-line={'y' if boot_ok else 'NO'} established={'y' if est_ok else 'NO'}")
        # ZOMBIE-BRIDGE diagnosis (S-2026-07-14): a bridge process whose TcpBroadcaster is dead
        # still HOLDS the single-instance lock (:19000+client_id), so every 5-min task refire
        # exits via the guard and 9701 stays unbound while the L1 csv keeps growing — permanent
        # silent RED until the zombie dies. Name the state + the PID so the fix is one kill.
        if open_now and not consumer_ok:
            listen_9701 = ssh("netstat -ano | findstr :9701 | findstr LISTENING")
            lock_19099 = ssh("netstat -ano | findstr :19099 | findstr LISTENING")  # 19000+client-id 99
            if "LISTENING" in lock_19099 and "LISTENING" not in listen_9701:
                pid = lock_19099.split()[-1] if lock_19099.split() else "?"
                msg += (f" | ZOMBIE BRIDGE: pid {pid} holds lock :19099 with broadcaster dead —"
                        f" taskkill /PID {pid} /F; the 5-min OmegaIbkrBridge refire respawns clean")
        rec("CONSUMER-UP", consumer_ok, msg)

    # [3] bridge freshness on the IBKR-only canary
    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    # dir-based (quoting-proof over ssh; powershell nested quotes mangle)
    din = ssh(r"dir C:\Omega\logs\ibkr_l2\ibkr_l1_" + CANARY_L1 + "_" + today + r".csv 2>nul")
    mt = ""
    for ln in din.splitlines():
        parts = ln.split()
        if len(parts) >= 4 and parts[-1].endswith(".csv"):
            # e.g. "07/08/2026  08:22 AM       500123 ibkr_l1_USDCAD_2026-07-08.csv" (VPS local=UTC)
            try:
                ampm = parts[2] if parts[2] in ("AM", "PM") else ""
                tstr = parts[1] + ((" " + ampm) if ampm else "")
                fmt = "%m/%d/%Y %I:%M %p" if ampm else "%m/%d/%Y %H:%M"
                mt = datetime.datetime.strptime(parts[0] + " " + tstr, fmt).strftime("%Y-%m-%d %H:%M:%S")
            except Exception:
                mt = ""
            break
    if not open_now:
        rec("BRIDGE-FRESH", True, "FX closed (weekend window) — freshness not assessable", skip=True)
        bridge_fresh = None
    else:
        try:
            age_min = (datetime.datetime.now(datetime.timezone.utc)
                       - datetime.datetime.strptime(mt, "%Y-%m-%d %H:%M:%S").replace(tzinfo=datetime.timezone.utc)
                       ).total_seconds() / 60.0
            bridge_fresh = age_min <= 20
            rec("BRIDGE-FRESH", bridge_fresh, f"{CANARY_L1} L1 csv age {age_min:.0f}min (<=20)")
        except Exception:
            bridge_fresh = False
            rec("BRIDGE-FRESH", False, f"{CANARY_L1} L1 csv missing for {today} — bridge not subscribed/running")

    # [4] downstream book advance. S-2026-07-24 rewrite: the HTTP endpoint alone was a
    # ROTTED PROXY — /api/fxladder_companion (and the other two ladder panels) 404 in the
    # running build while the book's state file keeps advancing, so the old check sat
    # PERMANENTLY false-RED on a 404 that does NOT block routing (a shadow display panel).
    # A monitor that cries wolf gets ignored. Now we classify honestly:
    #   * HTTP endpoint OK + ts fresh          -> PASS (full chain proven via API)
    #   * HTTP 404/error BUT state file fresh   -> PASS (book IS advancing; endpoint just
    #                                              unregistered in this build) + note it
    #   * desk/API server unreachable (refused/timeout, NOT a 404) -> RED (infra down)
    #   * endpoint down AND state file stale/missing -> RED (book genuinely stalled)
    # The genuine ROUTING-block detection now lives in [7] ORDER-ROUTE (exec socket),
    # which is where the operator's "to the point orders can route" concern truly belongs.
    if not open_now:
        rec("DOWNSTREAM", True, "FX closed (weekend) — book-advance not assessable", skip=True)
    else:
        http_lag_h, http_err, api_down = None, None, False
        try:
            with urllib.request.urlopen(f"{DESK}/api/fxladder_companion", timeout=10) as r:
                j = json.load(r)
            ts = next((p.get("ts", 0) for p in j.get("pairs", []) if p.get("pair") == LADDER_PAIR), 0)
            http_lag_h = (datetime.datetime.now(datetime.timezone.utc).timestamp() - ts) / 3600.0
        except urllib.error.HTTPError as e:
            http_err = f"HTTP {e.code}"           # server is UP, this endpoint is gone (e.g. 404)
        except Exception as e:
            api_down = True; http_err = str(e)     # connection refused / timeout = server DOWN

        if http_lag_h is not None:
            ok = http_lag_h <= 26
            rec("DOWNSTREAM", ok, f"{LADDER_PAIR} ladder last-bar lag {http_lag_h:.1f}h (<=26h) via API "
                                  f"— full chain bridge->consumer->H1->book")
        elif api_down:
            rec("DOWNSTREAM", False, f"desk/API server ({DESK}) UNREACHABLE ({http_err}) — "
                                     f"desk infra down, downstream unverifiable")
        else:
            # endpoint 404'd but the server is up: fall back to the TRUE book-advance
            # outcome = the state file's mtime on the box (un-proxied by the HTTP panel).
            age = state_file_age_min(FXLADDER_STATE)
            if age is None:
                rec("DOWNSTREAM", False, f"fxladder endpoint {http_err} AND state file missing/unreadable "
                                         f"— book genuinely stalled")
            elif age <= 90:
                rec("DOWNSTREAM", True, f"fxladder endpoint {http_err} (panel unregistered in build) but "
                                        f"state file fresh ({age:.0f}min) — book IS advancing; feed->book chain OK")
            else:
                rec("DOWNSTREAM", False, f"fxladder endpoint {http_err} AND state file STALE ({age:.0f}min) "
                                         f"— book stalled")

    # [5] the silent-fallback state itself
    if open_now and bridge_fresh and not consumer_ok:
        rec("NO-FALLBACK", False,
            "PRIMARY DOWN — bridge records but binary not connected: FX is RIDING THE BLACKBULL FALLBACK silently")
    else:
        rec("NO-FALLBACK", True, "no silent-fallback state detected")

    # [6] BIGCAP-CONSUMER — the bigcap L1 bridge (:7784) primary path. IDENTICAL failure class to
    #     [1]/[2] for FX. S-2026-07-16: the Omega service env was missing OMEGA_BIGCAP_BRIDGE=1, so
    #     the binary never consumed :7784 -> in-binary daily-close writer got 0 fresh mids ->
    #     data/rdagent/sp500_long_close.csv froze -> the bigcap mimic ladder never saw a new day
    #     and never fired, silently masked by the yfinance OmegaStockMoverFeed fallback. The bridge
    #     screams "[BIGCAP-ALERT] consumer=N ... Omega NOT consuming :7784" every heartbeat but
    #     nothing surfaced it. Connection is independent of RTH (holds post-close), so this is NOT
    #     market-gated: consumer=Y is required whenever the bridge is alive. If the bridge itself
    #     isn't emitting fresh heartbeats (no fresh HB) -> SKIP (down/off-hours), don't false-RED.
    hb_out = ssh(_BIGCAP_HB_CMD)
    try:
        age_s, _, hb = hb_out.strip().partition("~")
        hb_age = int(age_s)
    except Exception:
        hb_age, hb = None, ""
    if hb_age is None or hb_age > 10 or "HB " not in hb:
        rec("BIGCAP-CONSUMER", True,
            f"bigcap bridge heartbeat not fresh (log age {hb_age}min) — bridge down/off-hours, consumer not assessable",
            skip=True)
    else:
        consumer_y = "consumer=Y" in hb
        # S-2026-07-16: post-TEE (omega_main.hpp pump_feed::run live_cb), the :7784 consumer is the
        # mimic LADDER's ONLY live-confirm tick source (the 45 bigcap STK names ride here, not the
        # :9701 bridge). consumer=Y => ticks reach stockmover_ladder_book().on_live_tick, so a pending
        # +thr window can actually confirm + open, AND the daily-close writer gets live mids. consumer=N
        # => the ladder is BLIND (opens zero legs) even with a fresh CSV — the exact silent state that
        # made it "not fire daily". OMEGA_BIGCAP_BRIDGE=1 in the service env is what starts this consumer.
        rec("BIGCAP-CONSUMER", consumer_y,
            "bigcap bridge :7784 consumer=Y — binary consuming live ticks (ladder live-confirm gate fed + daily-close writer fed)" if consumer_y
            else "bigcap bridge :7784 consumer=N — binary NOT consuming; the mimic LADDER is BLIND (live-confirm "
                 "gate starved -> opens ZERO legs) and the daily-close writer is starved. Check OMEGA_BIGCAP_BRIDGE=1 "
                 "in the Omega service env + single bridge proc.")

    # [7] ORDER-ROUTE — the OUTCOME the whole feed path exists to enable: do live orders
    #     actually REACH THE BROKER? A perfect feed is worthless if the exec socket is down.
    #     S-2026-07-24: [1]-[6] all GREEN while the IBKR EXEC socket (:4001, a SEPARATE socket
    #     from the :9701 market-data bridge) was dropped and EVERY order blocked 'not
    #     connected' — signals fired into a dead path, 0 fills, all day, unseen. This is the
    #     "verify end-to-end to the point orders can route" check. Not market-gated on the
    #     feed window: if intents fired at all and none landed, that is a real failure. A
    #     quiet window (0 intents) can't fail -> SKIP so we never false-RED off-hours.
    oe = exec_order_health()
    if oe is None:
        rec("ORDER-ROUTE", True, "exec stdout log unreadable — order-routing outcome not assessable", skip=True)
    elif oe["rejects"] >= 3:
        rec("ORDER-ROUTE", False, f"REJECT STORM: {oe['rejects']} IBKR order rejects (201/460/10289) — "
                                  f"orders bouncing off a broker filter, NOT filling (the LOT_SIZE-storm class)")
    elif oe["blocked"] >= 5 and oe["fills"] == 0:
        rec("ORDER-ROUTE", False, f"BLOCK STORM: {oe['blocked']} orders BLOCKED pre-send (reason: "
                                  f"{oe['reason'] or '?'}) + 0 fills — exec socket down: every signal fires "
                                  f"into a dead path, NOTHING trades (feeds green, orders don't route)")
    elif oe["placed"] >= 3 and oe["fills"] == 0:
        rec("ORDER-ROUTE", False, f"ZERO-FILL: {oe['placed']} orders sent to broker, 0 fills — effectively "
                                  f"SHADOW despite live feed path")
    elif oe["intents"] == 0:
        rec("ORDER-ROUTE", True, "no order intents in window (market quiet / no signals) — routing not exercised",
            skip=True)
    else:
        rec("ORDER-ROUTE", True, f"{oe['fills']} broker fills in window (placed={oe['placed']} "
                                 f"blocked={oe['blocked']} rejects={oe['rejects']}) — orders ARE landing")

    hdr = "FEED-PATH SELFTEST " + ("RED — PRIMARY FEED PATH BROKEN, fix before trusting any FX/MGC signal"
                                    if red else "GREEN — primary IBKR path live end-to-end")
    print(hdr)
    print("\n".join(checks))
    print("-> A working fallback is what makes a dead primary invisible; this test watches the primary.")
    sys.exit(2 if red else 0)

if __name__ == "__main__":
    main()
