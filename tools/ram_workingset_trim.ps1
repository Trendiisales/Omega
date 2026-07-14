# ram_workingset_trim.ps1 -- live-safe working-set reclaim for the VPS. Written for
# the retired 3GB box (2026-07-03 thrash era); still the first-line reclaim on the
# live box omega-new (45.85.3.79), which has more RAM headroom.
#
# EmptyWorkingSet() trims each process's ACTIVE working set back to its minimum:
# clean pages are dropped, private dirty pages are paged out, and both fault back
# in on next access. This is the SAME mechanism Windows applies under memory
# pressure -- NO process restart, NO data loss, NO open-order risk (unlike
# OmegaRamRelief's gateway JVM restart). It reclaims the large chunk that
# ram_standby_purge cannot: at peak thrash the standby list is near-empty (~50MB),
# but private working sets still hold hundreds of MB of cold pages.
#
# Proven 2026-07-03 during the nightly-restart thrash: a manual EmptyWorkingSet
# sweep took free RAM 149MB -> 460MB (+311MB) with the gateway + Omega.exe running.
#
# Runs every 20 min (OmegaRamWorkingSetTrim task). This is the first-line reclaim;
# it complements -- does not replace -- ram_standby_purge (cache pages) and
# OmegaRamRelief (JVM restart, last resort). The durable cure remains a RAM bump
# to 4-6GB; this keeps the box off the starvation floor between now and then.
#
# Live-safe: EmptyWorkingSet never restarts a process, so unlike the gateway
# relief-restart it carries no live-order risk and needs no :4001 suppression.
$ErrorActionPreference = 'SilentlyContinue'
$log = 'C:\Omega\logs\ram_workingset_trim.log'

Add-Type 'using System;using System.Runtime.InteropServices;public class WS{[DllImport("psapi.dll")]public static extern bool EmptyWorkingSet(IntPtr h);}'

$before = [int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)
$n = 0
foreach ($p in Get-Process) {
    try { [WS]::EmptyWorkingSet($p.Handle) | Out-Null; $n++ } catch { }
}
Start-Sleep -Seconds 2
$after = [int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)

$ts = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
Add-Content $log "$ts trimmed=$n before=${before}MB after=${after}MB reclaimed=$($after - $before)MB"
