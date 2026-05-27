# =============================================================================
# run_with_heartbeat.ps1 - wrapper for scheduled bracket scripts
#
# - Writes start + end records to  logs\heartbeat.ndjson
# - Captures stdout / stderr to    logs\scheduled_<task>_<utc>.log
# - On non-zero exit, POSTs to     $env:BRACKET_WEBHOOK_URL (if set)
#
# Invoked by Task Scheduler via:
#   powershell -ExecutionPolicy Bypass -File scripts\run_with_heartbeat.ps1
#     -TaskName "Omega Daily Bracket 1300"
#     -Script   "live\daily_bracket.py"
#     -ScriptArgs "--paper --qty 1 --instrument MGC --strategy DAILY1300"
# =============================================================================
param(
    [Parameter(Mandatory=$true)][string]$TaskName,
    [Parameter(Mandatory=$true)][string]$Script,
    [Parameter(Mandatory=$true)][string]$ScriptArgs
)

$ErrorActionPreference = 'Continue'

$BotDir   = Split-Path -Parent $PSScriptRoot
$LogDir   = Join-Path $BotDir 'logs'
$Heartbeat = Join-Path $LogDir 'heartbeat.ndjson'
$Py       = Join-Path $BotDir '.venv\Scripts\python.exe'

if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir | Out-Null }
if (-not (Test-Path $Py))     { Write-Error "venv python not found at $Py"; exit 4 }

$utc = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$logFile = Join-Path $LogDir ("scheduled_{0}_{1}.log" -f ($TaskName -replace '[^A-Za-z0-9]','_'), $utc)

function Write-HB($stage, $extra) {
    $rec = @{
        ts = [DateTime]::UtcNow.ToString('o')
        host = $env:COMPUTERNAME
        task = $TaskName
        stage = $stage
    } + $extra
    ($rec | ConvertTo-Json -Compress) | Out-File -Append -Encoding utf8 $Heartbeat
}

Write-HB 'wrapper_start' @{ script = $Script; args = $ScriptArgs }

# Build the full command line
$fullArgs = "$Script $ScriptArgs"
$proc = Start-Process -FilePath $Py -ArgumentList $fullArgs `
    -WorkingDirectory $BotDir `
    -RedirectStandardOutput $logFile -RedirectStandardError "$logFile.err" `
    -PassThru -NoNewWindow -Wait

$exit = $proc.ExitCode
Write-HB 'wrapper_end' @{ exit_code = $exit; log = $logFile }

if ($exit -ne 0) {
    $tail = ''
    if (Test-Path "$logFile.err") {
        $tail = Get-Content "$logFile.err" -Tail 20 -ErrorAction SilentlyContinue | Out-String
    }
    $msg = "task=$TaskName exit=$exit`n--- last stderr ---`n$tail"
    Write-Warning $msg

    if ($env:BRACKET_WEBHOOK_URL) {
        $payload = @{ text = "[bracket-bot $env:COMPUTERNAME] $msg" } | ConvertTo-Json -Compress
        try {
            Invoke-WebRequest -Uri $env:BRACKET_WEBHOOK_URL -Method POST `
                -Body $payload -ContentType 'application/json' -TimeoutSec 10 | Out-Null
        } catch {
            Write-Warning "webhook post failed: $_"
        }
    }
}

exit $exit
