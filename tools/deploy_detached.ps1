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
  [switch]$Clean,
  # S-2026-07-22: forward OMEGA.ps1's -AllowStaleSeed override. Use ONLY when the
  # blocking stale seed is a KNOWN separate issue orthogonal to the change being
  # shipped (e.g. a GUI-only deploy while an enabled-engine seed can't refresh
  # because IBKR gateway is down) AND the restart does not regress that seed vs the
  # currently-running binary. omega_deploy.sh --allow-stale-seed forwards this switch.
  [switch]$AllowStaleSeed
)
Set-Content -Path 'C:\Omega\logs\deploy_latest_logname.txt' -Value $LogPath
Set-Location C:\Omega
# Explicit literal calls (not array-splat): the splat form silently produced an
# empty log / no-op deploy on omega-new PS5.1 (S-2026-07-22). Literal is the proven
# form the -Clean path always used.
if ($Clean -and $AllowStaleSeed) {
  & .\OMEGA.ps1 deploy -Clean -AllowStaleSeed *> $LogPath
} elseif ($Clean) {
  & .\OMEGA.ps1 deploy -Clean *> $LogPath
} elseif ($AllowStaleSeed) {
  & .\OMEGA.ps1 deploy -AllowStaleSeed *> $LogPath
} else {
  & .\OMEGA.ps1 deploy *> $LogPath
}
