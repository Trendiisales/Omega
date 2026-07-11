#!/usr/bin/env bash
# omega_deploy.sh — one-command non-hanging VPS deploy from the Mac.
#
# WHY: running `ssh omega-vps ... OMEGA.ps1 deploy` in the FOREGROUND ties the
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
# (feeds_selftest.py already uses VPS_HOST="omega-new"). Override with HOST=omega-vps only to
# touch the retired box deliberately.
HOST="${HOST:-omega-new}"

if [[ "${1:-}" != "--no-push" ]]; then
  echo "[deploy] pushing origin/main..."
  git push origin main
fi
WANT=$(git rev-parse --short origin/main)
echo "[deploy] target hash = $WANT"

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
LAUNCH=$(ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"\$lg='C:\\Omega\\logs\\deploy_'+(Get-Date -Format yyyyMMdd_HHmmss)+'.log'; \$r=Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=('powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\Omega\\tools\\deploy_detached.ps1 -LogPath '+\$lg)}; Write-Output ('DEPLOY_PID='+\$r.ProcessId+' log='+\$lg)\"")
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

echo "[deploy] polling for service Running + new binary (up to ~12 min)..."
ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"\$d=(Get-Date).AddSeconds(700); while((Get-Date) -lt \$d){ \$svc=(Get-Service Omega -ErrorAction SilentlyContinue).Status; \$hash=(git -C C:\\Omega rev-parse --short HEAD); if(\$svc -eq 'Running' -and \$hash -eq '$WANT'){ break }; Start-Sleep 20 }\""

echo "[deploy] verifying running binary hash..."
ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Write-Output ('origin/main = '+(git -C C:\\Omega rev-parse --short origin/main)); Write-Output ('VPS HEAD   = '+(git -C C:\\Omega rev-parse --short HEAD)); Write-Output ('service    = '+(Get-Service Omega).Status); Get-Content C:\\Omega\\logs\\omega_service_stderr.log -Tail 40 | Select-String 'Git hash|version=' | Select-Object -Last 2 | ForEach-Object { \$_.Line }\""
echo "[deploy] done. Confirm the three hashes above all == $WANT."
