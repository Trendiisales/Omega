# apply_boot_stagger.ps1 -- stagger the heavy boot-start tasks so they don't all
# cold-start at t=0 and thrash the VPS (written for the retired 3GB box; the live
# omega-new box has more headroom but the stagger stays harmless-and-useful).
# Simultaneous boot of Omega.exe
# (run_dashboard) + the gateway JVM (250MB) + 3 python bridges into ~150MB free is
# the root of the 20-25min nightly reload: every page fault hits the pagefile.
#
# Fix: add a boot-trigger Delay to each heavy task so the peak is spread across
# ~0-150s. The gateway (IbkrGateway, logon trigger) leads; the engine + bridges
# follow. The bridges connect TO the gateway anyway (own retry loops), so a delayed
# start costs nothing but relieves the cold-start RAM spike. Also gives the
# working-set trimmer a boot trigger so it reclaims DURING the cascade.
#
# Idempotent: re-running just resets the same delays. Only the BootTrigger of each
# task is touched -- periodic TimeTriggers are preserved unchanged.
$ErrorActionPreference = 'Stop'

$delays = [ordered]@{
    'Omega Dashboard'      = 'PT90S'
    'OmegaIbkrBridge'      = 'PT120S'
    'OmegaBigCapBridge'    = 'PT150S'
    'OmegaMgcLiveBars'     = 'PT150S'
    'OmegaIbkrL2Freshness' = 'PT60S'
}

foreach ($tn in $delays.Keys) {
    $t = Get-ScheduledTask -TaskName $tn -ErrorAction SilentlyContinue
    if (-not $t) { Write-Output "SKIP $tn (missing)"; continue }
    $touched = $false
    foreach ($trg in $t.Triggers) {
        if ($trg.CimClass.CimClassName -eq 'MSFT_TaskBootTrigger') { $trg.Delay = $delays[$tn]; $touched = $true }
    }
    if ($touched) { Set-ScheduledTask -TaskName $tn -Trigger $t.Triggers | Out-Null; Write-Output "STAGGER $tn boot delay=$($delays[$tn])" }
    else { Write-Output "NO-BOOT-TRIGGER $tn (unchanged)" }
}

# Give the working-set trimmer a boot trigger (delay 30s) so it reclaims during boot.
$wt = Get-ScheduledTask -TaskName 'OmegaRamWorkingSetTrim'
$hasBoot = $wt.Triggers | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskBootTrigger' }
if (-not $hasBoot) {
    $bt = New-ScheduledTaskTrigger -AtStartup
    $bt.Delay = 'PT30S'
    Set-ScheduledTask -TaskName 'OmegaRamWorkingSetTrim' -Trigger (@($wt.Triggers) + $bt) | Out-Null
    Write-Output 'ADD boot trigger to OmegaRamWorkingSetTrim (delay 30s)'
}

Write-Output '== verify boot delays =='
foreach ($tn in (@($delays.Keys) + 'OmegaRamWorkingSetTrim')) {
    $t = Get-ScheduledTask -TaskName $tn -ErrorAction SilentlyContinue
    if (-not $t) { continue }
    foreach ($trg in $t.Triggers) {
        if ($trg.CimClass.CimClassName -eq 'MSFT_TaskBootTrigger') { Write-Output ("  $tn boot delay=" + $trg.Delay) }
    }
}
