# =============================================================================
# cap_gateway_heap.ps1 — hard-cap the IB Gateway JVM heap so it can't leak the
# box to a freeze.
#
# WHY: IB Gateway is a Java app (~750MB and the prime climb suspect — JVM is
# GC-lazy, grows toward whatever -Xmx allows). Capping -Xmx forces GC to reclaim
# instead of climbing. Pairs with mem_trace: if Gateway is the climber, this is
# the fix; if it isn't, this is cheap insurance and costs nothing.
#
# Finds the IB Gateway .vmoptions file (install path varies by version) and
# ensures a single -Xmx768m line (replacing any existing -Xmx). 768m is safe for
# a Gateway feed session; if you see disconnects/GC thrash, raise to 1024m.
#
# Run ONCE on the VPS, then RESTART IB Gateway (IBC AutoRestart 03:00 picks it up,
# or restart manually in a market-closed window):
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\cap_gateway_heap.ps1
# =============================================================================
$ErrorActionPreference = 'Stop'
$Xmx = '-Xmx768m'

# IB Gateway ships ibgateway.vmoptions under its versioned install dir.
$roots = @(
    'C:\Jts\ibgateway',
    "$env:USERPROFILE\Jts\ibgateway",
    'C:\IBKR\ibgateway',
    'C:\Jts'
) | Where-Object { Test-Path $_ }

$vmo = $roots |
       ForEach-Object { Get-ChildItem -Path $_ -Recurse -Filter '*.vmoptions' -ErrorAction SilentlyContinue } |
       Select-Object -First 1

if (-not $vmo) {
    Write-Error "No .vmoptions found under: $($roots -join ', '). Locate IB Gateway install and pass its dir."
    exit 1
}

Write-Output "Found: $($vmo.FullName)"
$lines = Get-Content $vmo.FullName | Where-Object { $_ -notmatch '^\s*-Xmx' }   # strip any old -Xmx
$lines += $Xmx
Set-Content -Path $vmo.FullName -Value $lines
Write-Output "Set $Xmx (old -Xmx removed). RESTART IB Gateway for it to take effect."
Write-Output "Verify after restart:  (Get-Process | ? {`$_.Name -match 'ibgateway|java'} | Select Name,@{n='MB';e={[int](`$_.WS/1MB)}})"
