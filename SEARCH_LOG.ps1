#Requires -Version 5.1
# ==============================================================================
#  OMEGA - LOG SEARCH  (PowerShell-native grep replacement)
#
#  PURPOSE:
#    Search the Omega log for any pattern. Replaces grep on Windows.
#    Results are printed with line numbers and optionally filtered by time.
#
#  USAGE:
#    .\SEARCH_LOG.ps1 GF-GATE-BLOCK
#    .\SEARCH_LOG.ps1 GF-BAR-BLOCK -Last 50
#    .\SEARCH_LOG.ps1 "GOLD-STACK|GoldFlow" -Last 100
#    .\SEARCH_LOG.ps1 GF-OUTER-BLOCK -Since "23:00"
#    .\SEARCH_LOG.ps1 can_arm_bracket -Since "23:10" -Until "23:50"
#    .\SEARCH_LOG.ps1 "XAUUSD" -LogPath C:\Omega\logs\omega_2026-04-02.log
#
#  COMMON PATTERNS:
#    GF-GATE-BLOCK          -- what is blocking GoldFlow entry
#    GF-BAR-BLOCK           -- bar indicator blocks (RSI/trend/spread)
#    GF-OUTER-BLOCK         -- gold_can_enter=0 (session/trail/impulse)
#    can_arm_bracket        -- bracket arm state
#    GOLD-STACK-ENTRY       -- GoldStack trades that fired
#    GoldFlow               -- all GoldFlow activity
#    ASIA-GATE              -- Asia session vol gate
#    POST-IMPULSE           -- post-impulse cooldown blocks
#    DEAD-ZONE-BYPASS       -- dead zone macro override
#    BRACKET-RANGE          -- bracket range diagnostics
#    BRK-DIAG               -- bracket state every bar
# ==============================================================================

param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$Pattern,

    [string]$LogPath   = "",
    [string]$OmegaDir  = "C:\Omega",
    [int]   $Last      = 0,          # show last N matching lines (0 = all)
    [string]$Since     = "",         # filter lines at or after HH:MM (UTC)
    [string]$Until     = "",         # filter lines at or before HH:MM (UTC)
    [switch]$NoNumbers               # suppress line numbers
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

# --- Locate log ---------------------------------------------------------------
if ($LogPath -eq "") {
    $candidates = @(
        "$OmegaDir\logs\latest.log",
        "$OmegaDir\logs\omega_$(Get-Date -Format 'yyyy-MM-dd').log",
        "$OmegaDir\omega.log"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $LogPath = $c; break }
    }
}

if ($LogPath -eq "" -or -not (Test-Path $LogPath)) {
    Write-Host "[SEARCH_LOG] ERROR: Cannot find Omega log." -ForegroundColor Red
    Write-Host "  Use: .\SEARCH_LOG.ps1 <pattern> -LogPath C:\Omega\logs\omega_YYYY-MM-DD.log" -ForegroundColor Yellow
    exit 1
}

Write-Host "[SEARCH_LOG] $LogPath  pattern=`"$Pattern`"" -ForegroundColor DarkGray
if ($Since -ne "" -or $Until -ne "") {
    Write-Host "             time filter: Since=$Since Until=$Until" -ForegroundColor DarkGray
}
Write-Host ""

# --- Read + filter ------------------------------------------------------------
$allLines = Get-Content -Path $LogPath -ErrorAction Stop

$lineNum = 0
$matches_ = [System.Collections.Generic.List[object]]::new()

foreach ($line in $allLines) {
    $lineNum++
    if ($line -notmatch $Pattern) { continue }

    # Time filter: log lines start with HH:MM:SS or [HH:MM:SS]
    if ($Since -ne "" -or $Until -ne "") {
        $timeVal = $null
        if ($line -match '^\[?(\d{2}:\d{2}:\d{2})') {
            try { $timeVal = [TimeSpan]::Parse($Matches[1]) } catch {}
        }
        if ($timeVal -ne $null) {
            if ($Since -ne "") {
                try {
                    $sinceTs = [TimeSpan]::Parse($Since)
                    if ($timeVal -lt $sinceTs) { continue }
                } catch {}
            }
            if ($Until -ne "") {
                try {
                    $untilTs = [TimeSpan]::Parse($Until)
                    if ($timeVal -gt $untilTs) { continue }
                } catch {}
            }
        }
    }

    $matches_.Add([PSCustomObject]@{ LineNum = $lineNum; Text = $line })
}

# --- Apply -Last limit --------------------------------------------------------
$matchCount = $matches_.Count
$toShow = [System.Collections.Generic.List[object]]::new()
if ($Last -gt 0 -and $matchCount -gt $Last) {
    foreach ($m in ($matches_ | Select-Object -Last $Last)) { $toShow.Add($m) }
} else {
    foreach ($m in $matches_) { $toShow.Add($m) }
}
$showCount = $toShow.Count

# --- Print --------------------------------------------------------------------
if ($showCount -eq 0) {
    Write-Host "  (no matches)" -ForegroundColor DarkGray
} else {
    foreach ($m in $toShow) {
        if ($NoNumbers) {
            Write-Host $m.Text
        } else {
            $numStr = "$($m.LineNum)".PadLeft(6)
            Write-Host "$numStr  $($m.Text)"
        }
    }
}

Write-Host ""
Write-Host "  $matchCount match(es)  ($showCount shown)" -ForegroundColor DarkGray
