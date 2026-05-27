# =============================================================================
# gateway_watchdog.ps1 - keep IB Gateway alive on the VPS
#
# - Picks the newest C:\Jts\ibgateway\<N>\ibgateway.exe (survives auto-updates)
# - Idempotent: exits clean if a process is already running AND port 4002 listens
# - Logs every action to C:\Omega\bracket-bot\logs\gateway_watchdog.log
#
# Designed to be invoked every 5 min by a scheduled task triggered at logon.
# REQUIRES a logged-on interactive user (RDP session left connected) -- IB
# Gateway is GUI; it cannot start in Session 0.
#
# Prerequisite for unattended login: in Gateway, tick "Store settings on
# server" and save the paper password. The watchdog cannot type credentials.
# =============================================================================
$ErrorActionPreference = 'Continue'

$Port    = 4002
$Root    = 'C:\Jts\ibgateway'
$LogDir  = 'C:\Omega\bracket-bot\logs'
$LogFile = Join-Path $LogDir 'gateway_watchdog.log'

if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }

function Log($msg) {
    $ts = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    "$ts $msg" | Out-File -Append -Encoding utf8 $LogFile
}

# --- Already healthy? --------------------------------------------------------
$listening = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue
if ($listening) {
    # No need to log every 5-min OK heartbeat -- keeps log compact.
    exit 0
}

$running = Get-Process -Name 'ibgateway' -ErrorAction SilentlyContinue
if ($running) {
    Log "ibgateway.exe PID(s) $($running.Id -join ',') running but port $Port not listening -- leaving alone (likely login screen). Action required: complete login or save creds."
    exit 0
}

# --- Launch newest install --------------------------------------------------
if (-not (Test-Path $Root)) {
    Log "ERROR: Gateway root $Root missing -- Gateway not installed?"
    exit 2
}

$newest = Get-ChildItem -Path $Root -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'ibgateway.exe') } |
    Sort-Object { [int]($_.Name -replace '\D','') } -Descending |
    Select-Object -First 1

if (-not $newest) {
    Log "ERROR: no ibgateway.exe under $Root\<version>\"
    exit 2
}

$exe = Join-Path $newest.FullName 'ibgateway.exe'
Log "launching $exe (port $Port was not listening, no ibgateway.exe process)"

try {
    Start-Process -FilePath $exe -WorkingDirectory $newest.FullName -WindowStyle Minimized
    Log "launched OK; checking listen in 60s"
} catch {
    Log "ERROR: launch failed: $_"
    exit 3
}

# Wait up to 60s for the API socket to come up
$ok = $false
for ($i = 0; $i -lt 12; $i++) {
    Start-Sleep -Seconds 5
    if (Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue) {
        $ok = $true; break
    }
}

if ($ok) {
    Log "port $Port LISTENING -- Gateway healthy"
    exit 0
} else {
    Log "WARN: launched but port $Port still not listening after 60s (login screen? saved creds missing?)"
    exit 1
}
