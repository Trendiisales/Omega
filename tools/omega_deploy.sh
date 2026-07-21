#!/usr/bin/env bash
# omega_deploy.sh — one-command non-hanging VPS deploy from the Mac.
#
# WHY: running `ssh omega-new ... OMEGA.ps1 deploy` in the FOREGROUND ties the
# MSVC build to the ssh session. Any client-side timeout (or dropped connection)
# SIGHUPs the remote build mid-flight -> service stuck StopPending, half-built
# binary. This wrapper launches the deploy DETACHED on the VPS (survives
# disconnect), then polls + verifies the running git hash == origin/main.
#
# S-2026-06-29 DEPLOY-SPEED: added a LOUD no-op assertion. A prior silent failure
# (detached Start-Process launched a PID that died instantly, no log written) was
# only discovered after the 12-min poll timed out with stale hashes. The launch
# now returns the real PID + log path; we assert within ~10s that the log exists
# and the deploy is actually progressing, and ABORT LOUDLY otherwise instead of
# polling for nothing. The single source of truth for the log filename is the
# powershell-side Get-Date (one call), echoed back to us -- no bash/ps timestamp
# split that could print a name that never matches the real file.
#
# Usage:  bash tools/omega_deploy.sh         # push current main, then deploy
#         bash tools/omega_deploy.sh --no-push
set -euo pipefail
# S-2026-07-10: repointed omega-vps(185, RETIRED old box) -> omega-new(45.85.3.79, LIVE box).
# The 07-07 VPS migration cut production over to 45.85.3.79 but this deploy path was never
# repointed (the fix sat in unmerged draft PR #4). Result: deploys silently landed on the dead
# old box while the operator traded on the new one for days. omega-new IS the live desk
# (feeds_selftest.py already uses VPS_HOST="omega-new").
# S-2026-07-14 (dead-box ref audit): the former "override with HOST=omega-vps" escape hatch
# is now HARD-BLOCKED below — NO code path may deploy to the retired box, ever.
HOST="${HOST:-omega-new}"
case "$HOST" in
  omega-vps|185.167.119.59)
    echo "[deploy][FATAL] HOST=$HOST is the RETIRED dead box (old VPS, no broker connection," >&2
    echo "                decommission pending). The live desk is omega-new (45.85.3.79)." >&2
    echo "                Refusing to deploy. See CLAUDE.md \"WHICH BOX IS LIVE\"." >&2
    exit 1;;
esac

# ── flags ────────────────────────────────────────────────────────────────────
NO_PUSH=0; FORCE_CLOSE_WINDOW=0; CLEAN=0; ALLOW_STALE_SEED=0
for a in "$@"; do
  case "$a" in
    --no-push)             NO_PUSH=1;;
    --force-close-window)  FORCE_CLOSE_WINDOW=1;;
    # --clean: forward -Clean to OMEGA.ps1 (full rebuild). REQUIRED for header-only
    # wires — incremental MSBuild can skip the header->main.cpp recompile (correct
    # stamped hash, missing code; memory project-header-wire-incremental-stale-build).
    --clean)               CLEAN=1;;
    # --allow-stale-seed: forward -AllowStaleSeed to OMEGA.ps1 — override the fail-
    # closed seed-freshness gate. Use ONLY when the blocking stale seed is a KNOWN
    # separate issue orthogonal to the change (e.g. a GUI-only deploy while an
    # enabled-engine seed can't refresh because IBKR gateway is down) AND the restart
    # does not regress that seed vs the currently-running binary. NEVER a blanket use.
    --allow-stale-seed)    ALLOW_STALE_SEED=1;;
    *) echo "[deploy][WARN] unknown arg: $a (accepted: --no-push --force-close-window --clean --allow-stale-seed)" >&2;;
  esac
done

# ── DEPLOY-WINDOW GUARD (S-2026-07-17h) ──────────────────────────────────────
# A restart during the daily-close DISPATCH window (20:00-23:00 UTC) makes the
# seed-on-boot replay absorb the session's just-closed rows as HISTORY -> the
# day's live signals book NOTHING ("deploy-forward: seed primes the detector
# only, books nothing"). This is the DIRECT root cause of the 2026-07-16 whole-
# book zero-trade gap: 5 restarts (the 17e ship storm) landed inside the window.
# BLOCK a deploy inside the window unless the operator passes --force-close-window.
CLOSE_WIN_START=20; CLOSE_WIN_END=23     # UTC hours, half-open [start,end)
UTC_HOUR=$(date -u +%H)                  # zero-padded; 10# below forces base-10 (avoid octal 08/09)
if (( 10#$UTC_HOUR >= CLOSE_WIN_START && 10#$UTC_HOUR < CLOSE_WIN_END )); then
  if (( FORCE_CLOSE_WINDOW )); then
    echo "[deploy][WARN] inside daily-close dispatch window (${UTC_HOUR}:00 UTC, ${CLOSE_WIN_START}:00-${CLOSE_WIN_END}:00 UTC)" >&2
    echo "               -- proceeding under --force-close-window. Seed-on-boot may absorb today's closes." >&2
  else
    echo "[deploy][FATAL] restart blocked: it is ${UTC_HOUR}:00 UTC, INSIDE the daily-close dispatch" >&2
    echo "                window (${CLOSE_WIN_START}:00-${CLOSE_WIN_END}:00 UTC). A restart here makes seed-on-boot absorb the" >&2
    echo "                session's just-closed rows as history -> today's live signals book" >&2
    echo "                NOTHING (this caused the 2026-07-16 whole-book zero-trade gap)." >&2
    echo "                Wait until after ${CLOSE_WIN_END}:00 UTC, or pass --force-close-window to override." >&2
    exit 1
  fi
fi

if (( ! NO_PUSH )); then
  echo "[deploy] pushing origin/main..."
  git push origin main
fi
WANT=$(git rev-parse --short origin/main)
echo "[deploy] target hash = $WANT"

# IN-FLIGHT MARKER (S-2026-07-15): tells deploy_drift_check.sh a deploy is actively
# running for $WANT so the staleness scan does NOT cry "LIVE feed stale" during the
# legitimate ~10-min build window (operator: "if we deploy we do not get this error").
# NOT a mute — a genuinely undeployed commit with NO deploy in flight still REDs, and
# a stale marker (deploy died/disconnected) expires by age so it can't suppress forever.
# Format: "<epoch> <target-hash>". Removed on any exit.
DEPLOY_INFLIGHT="/tmp/omega_deploy.inflight"
printf '%s %s\n' "$(date +%s)" "$WANT" > "$DEPLOY_INFLIGHT" 2>/dev/null || true
trap 'rm -f "$DEPLOY_INFLIGHT" 2>/dev/null || true' EXIT

echo "[deploy] launching DETACHED deploy on $HOST (survives disconnect)..."
# NOTE: log path + redirect MUST be powershell-side with single backslashes.
# A bash-interpolated forward-slash path (C:/Omega/...) mangles through
# cmd->powershell->Start-Process and the detached deploy silently no-ops.
# The child's stdout/stderr are redirected (*>) into the log so a crash in
# OMEGA.ps1 itself is captured, not lost to the hidden window.
#
# S-2026-07-12 LAUNCH REGRESSION FIX: sshd on omega-new now KILLS Start-Process
# children the moment the launching ssh session closes (verified: a hidden child
# survived while the session was held open, died on close — 3 consecutive
# "PID gone AND log empty" no-ops before diagnosis). The launch therefore goes
# through Invoke-CimMethod Win32_Process Create: the child is parented to the
# WMI provider host, OUTSIDE the ssh job object, and survives disconnect.
# It runs tools/deploy_detached.ps1 (in-repo; scp'd here as a bootstrap in case
# the box checkout predates the file — OMEGA.ps1 deploy pulls it into C:\Omega).
scp -q "$(dirname "$0")/deploy_detached.ps1" "$HOST:C:/Omega/tools/deploy_detached.ps1"
CLEAN_ARG=""; (( CLEAN )) && CLEAN_ARG=" -Clean"
STALE_ARG=""; (( ALLOW_STALE_SEED )) && STALE_ARG=" -AllowStaleSeed"
LAUNCH=$(ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"\$lg='C:\\Omega\\logs\\deploy_'+(Get-Date -Format yyyyMMdd_HHmmss)+'.log'; \$r=Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=('powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\Omega\\tools\\deploy_detached.ps1${CLEAN_ARG}${STALE_ARG} -LogPath '+\$lg)}; Write-Output ('DEPLOY_PID='+\$r.ProcessId+' log='+\$lg)\"")
echo "  $LAUNCH"
PID=$(echo "$LAUNCH" | sed -n 's/.*DEPLOY_PID=\([0-9]*\).*/\1/p')
LOG=$(echo "$LAUNCH" | sed -n 's/.* log=\(.*[Ll]og\).*/\1/p')
if [[ -z "$PID" || -z "$LOG" ]]; then
  echo "[deploy][FATAL] detached launch returned no PID/log -- the deploy did NOT start." >&2
  echo "                raw launch output: $LAUNCH" >&2
  exit 1
fi

# LOUD no-op assertion: within ~10s the child must (a) still be alive AND
# (b) have written its log. A dead PID with an empty/absent log = the deploy
# died on launch -> abort now with whatever the log captured, don't poll 12 min.
echo "[deploy] asserting deploy actually started (PID=$PID)..."
sleep 10
LOGWIN="${LOG//\\//}"   # C:\Omega\... -> C:/Omega/... for display only
ALIVE=$(ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"\$p=Get-Process -Id $PID -ErrorAction SilentlyContinue; \$exists=Test-Path '$LOG'; \$len=if(\$exists){(Get-Item '$LOG').Length}else{0}; Write-Output ('ALIVE='+([bool]\$p)+' LOGEXISTS='+\$exists+' LOGLEN='+\$len)\"")
echo "  $ALIVE"
if echo "$ALIVE" | grep -q 'ALIVE=False'; then
  if echo "$ALIVE" | grep -q 'LOGLEN=0'; then
    echo "[deploy][FATAL] PID $PID is gone AND log is empty -- deploy died on launch (silent no-op)." >&2
    echo "                inspect: $LOGWIN" >&2
    ssh "$HOST" "powershell -NoProfile -Command \"if(Test-Path '$LOG'){Get-Content '$LOG' -Tail 40}else{Write-Output '(no log file)'}\"" >&2 || true
    exit 1
  fi
  # PID gone but log has content -> deploy may have finished very fast (skip-seed +
  # no-op build). Fall through to hash verify, which is the real arbiter.
  echo "  [note] PID exited but log has content -- treating as fast-finish; verifying hashes."
fi

# S-2026-07-12c REWRITE of the poll + verdict: the old poll matched on git HEAD (updated at
# PULL time, minutes before the link finishes) + service Running (often the OLD binary during
# hot-swap) -> it declared "done" instantly while the box was still building, and said "done"
# even when stamp validation FAILED and left the service STOPPED (today's brick went unnoticed
# until the 15-min health poll). The RUNNING BINARY's own 'Git hash:' boot line is the only
# honest signal -> poll on that, and FAIL LOUDLY (popup + exit 1) on anything else.
echo "[deploy] polling for RUNNING BINARY at $WANT (up to ~14 min; git-HEAD match is NOT enough)..."
FINAL=$(ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"\$d=(Get-Date).AddSeconds(840); \$ok=\$false; while((Get-Date) -lt \$d){ \$svc=(Get-Service Omega -ErrorAction SilentlyContinue).Status; \$bh=((Get-Content C:\\Omega\\logs\\omega_service_stderr.log -Tail 60 -ErrorAction SilentlyContinue | Select-String 'Git hash' | Select-Object -Last 1) -replace '.*Git hash:\\s*','').Trim(); if(\$svc -eq 'Running' -and \$bh -and ('$WANT'.StartsWith(\$bh) -or \$bh.StartsWith('$WANT'))){ \$ok=\$true; break }; Start-Sleep 20 }; Write-Output ('RESULT ok='+\$ok+' service='+\$svc+' running_binary='+\$bh+' head='+(git -C C:\\Omega rev-parse --short HEAD)+' origin='+(git -C C:\\Omega rev-parse --short origin/main))\"")
echo "  $FINAL"
if ! echo "$FINAL" | grep -q 'ok=True'; then
  echo "[deploy][FATAL] deploy did NOT come up on $WANT -- see box deploy log below." >&2
  ssh "$HOST" "powershell -NoProfile -Command \"\$dl=(Get-ChildItem C:\\Omega\\logs\\deploy_*.log | Sort LastWriteTime | Select -Last 1); Get-Content \$dl.FullName -Tail 20\"" >&2 || true
  osascript -e "display notification \"deploy FAILED: box not Running at $WANT — service may be STOPPED. Check deploy log.\" with title \"🔴 OMEGA DEPLOY FAILED\" sound name \"Basso\"" 2>/dev/null || true
  exit 1
fi
echo "[deploy] done. Running binary verified at $WANT."
