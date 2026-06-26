#!/usr/bin/env bash
# omega_deploy.sh — one-command non-hanging VPS deploy from the Mac.
#
# WHY: running `ssh omega-vps ... OMEGA.ps1 deploy` in the FOREGROUND ties the
# MSVC build to the ssh session. Any client-side timeout (or dropped connection)
# SIGHUPs the remote build mid-flight -> service stuck StopPending, half-built
# binary. This wrapper launches the deploy DETACHED on the VPS (survives
# disconnect), then polls + verifies the running git hash == origin/main.
#
# Usage:  bash tools/omega_deploy.sh         # push current main, then deploy
#         bash tools/omega_deploy.sh --no-push
set -euo pipefail
HOST=omega-vps
LOG="C:/Omega/logs/deploy_$(date +%Y%m%d_%H%M%S).log"

if [[ "${1:-}" != "--no-push" ]]; then
  echo "[deploy] pushing origin/main..."
  git push origin main
fi
WANT=$(git rev-parse --short origin/main)
echo "[deploy] target hash = $WANT"

echo "[deploy] launching DETACHED deploy on $HOST (survives disconnect)..."
ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-Command','cd C:\\Omega; .\\OMEGA.ps1 deploy *> $LOG' -WindowStyle Hidden\""

echo "[deploy] polling for service Running + new binary (up to ~12 min)..."
ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"\$d=(Get-Date).AddSeconds(700); while((Get-Date) -lt \$d){ \$svc=(Get-Service Omega -ErrorAction SilentlyContinue).Status; \$hash=(git -C C:\\Omega rev-parse --short HEAD); if(\$svc -eq 'Running' -and \$hash -eq '$WANT'){ break }; Start-Sleep 20 }\""

echo "[deploy] verifying running binary hash..."
ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Write-Output ('origin/main = '+(git -C C:\\Omega rev-parse --short origin/main)); Write-Output ('VPS HEAD   = '+(git -C C:\\Omega rev-parse --short HEAD)); Write-Output ('service    = '+(Get-Service Omega).Status); Get-Content C:\\Omega\\logs\\omega_service_stderr.log -Tail 40 | Select-String 'Git hash|version=' | Select-Object -Last 2 | ForEach-Object { \$_.Line }\""
echo "[deploy] done. Confirm the three hashes above all == $WANT."
