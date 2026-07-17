# deploy_detached.ps1 — WMI-spawned deploy runner (survives ssh session close).
#
# S-2026-07-12: sshd on omega-new kills Start-Process children when the launching
# ssh session closes, so tools/omega_deploy.sh's old detached launch silently
# no-op'd (PID gone, no log, three consecutive times). omega_deploy.sh now spawns
# THIS file via Invoke-CimMethod Win32_Process Create — the child is parented to
# the WMI provider host, outside the ssh job object, and survives disconnect.
# Same *> log capture as before so a crash inside OMEGA.ps1 is captured.
#
# Invoked by tools/omega_deploy.sh (which also scp's this file to the box as a
# bootstrap in case the checkout there predates it). Do not run by hand unless
# you know why; the canonical entry point stays `bash tools/omega_deploy.sh`.
param(
  [string]$LogPath = ('C:\Omega\logs\deploy_' + (Get-Date -Format yyyyMMdd_HHmmss) + '.log'),
  # S-2026-07-17: header-only wires need a full rebuild (incremental MSBuild can
  # skip the header->main.cpp recompile: correct stamped hash, MISSING code —
  # memory project-header-wire-incremental-stale-build). omega_deploy.sh --clean
  # forwards this switch.
  [switch]$Clean
)
Set-Content -Path 'C:\Omega\logs\deploy_latest_logname.txt' -Value $LogPath
Set-Location C:\Omega
if ($Clean) {
  & .\OMEGA.ps1 deploy -Clean *> $LogPath
} else {
  & .\OMEGA.ps1 deploy *> $LogPath
}
