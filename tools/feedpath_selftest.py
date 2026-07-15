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
  [4] DOWNSTREAM    /api/fxladder_companion USDCAD ts advances (<= 26 trading-hours
                    lag) — proves bridge -> consumer -> dispatch -> H1 roll -> book.
  [5] NO-FALLBACK   if [3] fresh but [2] fails => "PRIMARY DOWN — RUNNING ON
                    FALLBACK" (the exact silent state this test exists to kill).
  [6] BIGCAP-CONSUMER  the bigcap L1 bridge (:7784) heartbeat shows consumer=Y — the
                    binary is consuming live mids so the in-binary daily-close writer
                    is fed. consumer=N => sp500_long_close.csv freezes and the bigcap
                    up-jump ladder silently stops firing (S-2026-07-16 root cause: the
                    Omega service env was missing OMEGA_BIGCAP_BRIDGE=1 — the SAME
                    missing-env failure class as [1], for a bridge nothing guarded).

Exit 0 GREEN / 2 RED. Run from SessionStart hook + Mac cron (30 min, notification
on RED via install_feedpath_cron.sh). FX weekend (Sat/Sun UTC + Fri>=22 UTC) skips
freshness checks honestly (reports SKIP, not PASS).
"""
import datetime
import json
import subprocess
import sys
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

def fx_market_open(now=None):
    now = now or datetime.datetime.now(datetime.timezone.utc)
    wd, hr = now.weekday(), now.hour
    if wd == 5: return False                      # Sat
    if wd == 6 and hr < 21: return False          # Sun pre-open
    if wd == 4 and hr >= 22: return False         # Fri post-close
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

    # [4] downstream book advance
    try:
        with urllib.request.urlopen(f"{DESK}/api/fxladder_companion", timeout=10) as r:
            j = json.load(r)
        ts = next((p.get("ts", 0) for p in j.get("pairs", []) if p.get("pair") == LADDER_PAIR), 0)
        lag_h = (datetime.datetime.now(datetime.timezone.utc).timestamp() - ts) / 3600.0
        if not open_now:
            rec("DOWNSTREAM", True, f"{LADDER_PAIR} ladder ts lag {lag_h:.0f}h (weekend) ", skip=True)
        else:
            ok = lag_h <= 26
            rec("DOWNSTREAM", ok, f"{LADDER_PAIR} ladder last-bar lag {lag_h:.1f}h (<=26h) — full chain bridge->consumer->H1->book")
    except Exception as e:
        rec("DOWNSTREAM", False, f"ladder api unreachable: {e}")

    # [5] the silent-fallback state itself
    if open_now and bridge_fresh and not consumer_ok:
        rec("NO-FALLBACK", False,
            "PRIMARY DOWN — bridge records but binary not connected: FX is RIDING THE BLACKBULL FALLBACK silently")
    else:
        rec("NO-FALLBACK", True, "no silent-fallback state detected")

    # [6] BIGCAP-CONSUMER — the bigcap L1 bridge (:7784) primary path. IDENTICAL failure class to
    #     [1]/[2] for FX. S-2026-07-16: the Omega service env was missing OMEGA_BIGCAP_BRIDGE=1, so
    #     the binary never consumed :7784 -> in-binary daily-close writer got 0 fresh mids ->
    #     data/rdagent/sp500_long_close.csv froze -> the bigcap up-jump ladder never saw a new day
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
        rec("BIGCAP-CONSUMER", consumer_y,
            "bigcap bridge :7784 consumer=Y — binary consuming live mids, daily-close writer fed" if consumer_y
            else "bigcap bridge :7784 consumer=N — binary NOT consuming; check OMEGA_BIGCAP_BRIDGE=1 in the "
                 "Omega service env + single bridge proc (daily-close writer starved -> sp500_long_close.csv "
                 "FREEZES -> bigcap up-jump ladder stops firing, masked by the yfinance fallback)")

    hdr = "FEED-PATH SELFTEST " + ("RED — PRIMARY FEED PATH BROKEN, fix before trusting any FX/MGC signal"
                                    if red else "GREEN — primary IBKR path live end-to-end")
    print(hdr)
    print("\n".join(checks))
    print("-> A working fallback is what makes a dead primary invisible; this test watches the primary.")
    sys.exit(2 if red else 0)

if __name__ == "__main__":
    main()
