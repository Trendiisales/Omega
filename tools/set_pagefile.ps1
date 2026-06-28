# =============================================================================
# set_pagefile.ps1 — P0 SAFETY NET: give the 3GB box a fixed pagefile so RAM
# pressure becomes a SLOWDOWN, not a hard FREEZE.
#
# WHY: reboot drops the live engine + breaks recording (recording gap = P0,
# operator hard rule). A pagefile lets Windows swap cold pages (GUIs/monitors)
# to disk under pressure instead of freezing — the engine + feeds stay live and
# RECORDING CONTINUES. This is strictly better than the reboot band-aid.
#
# Sets a FIXED 4096-6144 MB pagefile on C: (fixed avoids fragmentation/resize
# stalls). Disables Windows automatic management first. REQUIRES A REBOOT to
# take effect — schedule the reboot for a market-closed window.
#
# Run ONCE on the VPS (admin), then reboot when markets are closed:
#   powershell -ExecutionPolicy Bypass -File C:\Omega\tools\set_pagefile.ps1
#
# Cost: SSD write wear + latency spikes while swapping. Acceptable — only cold
# pages swap; hot feed/engine working set stays resident. Beats freezing.
# =============================================================================
$ErrorActionPreference = 'Stop'

$Init = 4096
$Max  = 6144

# 1. turn OFF automatic pagefile management
$cs = Get-CimInstance Win32_ComputerSystem
if ($cs.AutomaticManagedPagefile) {
    Set-CimInstance -InputObject $cs -Property @{ AutomaticManagedPagefile = $false }
    Write-Output "Automatic pagefile management: DISABLED"
} else {
    Write-Output "Automatic pagefile management: already off"
}

# 2. set a fixed pagefile on C:
$pf = Get-CimInstance Win32_PageFileSetting -Filter "SettingID='pagefile.sys @ C:'"
if ($pf) {
    Set-CimInstance -InputObject $pf -Property @{ InitialSize = $Init; MaximumSize = $Max }
    Write-Output "Existing C: pagefile resized -> ${Init}-${Max} MB"
} else {
    New-CimInstance -ClassName Win32_PageFileSetting `
        -Property @{ Name = 'C:\pagefile.sys'; InitialSize = $Init; MaximumSize = $Max } | Out-Null
    Write-Output "C: pagefile created -> ${Init}-${Max} MB"
}

Write-Output ""
Write-Output "DONE. REBOOT REQUIRED (market-closed window) for the pagefile to take effect."
Write-Output "Verify after reboot:  Get-CimInstance Win32_PageFileUsage | Select Name,AllocatedBaseSize,CurrentUsage"
