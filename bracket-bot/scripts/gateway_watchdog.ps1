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

$Port    = 4001  # 2026-07-18: gateway logs in LIVE -> API port 4001 (java listener, 4 API clients attached). Old 4002 probe never matched -> 5-min IBC relaunch loop against a healthy session ('NOT up after 120s' spam).
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

# --- Launch via IBC (unattended autologin) ----------------------------------
# CHANGED 2026-07-01: was launching RAW ibgateway.exe -> GUI login screen, no
# credential typing -> the gateway sat logged-out until a human logged in (twice).
# Now launch IBC's StartGateway.bat, which reads C:\IBC\config.ini (IbLoginId +
# IbPassword set, TradingMode=paper) and TYPES the credentials, so the API socket
# comes up unattended. TWS_MAJOR_VRSN=1047 is set inside the bat. The bat requires
# /INLINE when run from Task Scheduler.
$ibcBat = 'C:\IBC\StartGateway.bat'
if (-not (Test-Path $ibcBat)) {
    Log "ERROR: IBC launcher $ibcBat missing -- cannot autologin (would sit at login screen)"
    exit 2
}
Log "launching IBC ($ibcBat /INLINE) for unattended autologin (port $Port was not listening)"

try {
    Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', ('"' + $ibcBat + '"'), '/INLINE' -WindowStyle Hidden
    Log "IBC launched OK; checking listen (up to 120s for login)"
} catch {
    Log "ERROR: IBC launch failed: $_"
    exit 3
}

# Wait up to 120s for the API socket to come up (IBC login + cred typing is slower
# than a raw start; a paper account has no 2FA so 120s is ample).
$ok = $false
for ($i = 0; $i -lt 24; $i++) {
    Start-Sleep -Seconds 5
    if (Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue) {
        $ok = $true; break
    }
}

if ($ok) {
    Log "port $Port LISTENING -- Gateway healthy (IBC autologin succeeded)"
    exit 0
} else {
    Log "WARN: IBC launched but port $Port still not listening after 120s -- check C:\IBC\Logs (2FA? bad creds? version mismatch?)"
    exit 1
}

