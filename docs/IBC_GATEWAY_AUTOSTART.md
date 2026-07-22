# IBC -- IB Gateway Headless Auto-Start (Production-Critical)

Production rule: **IB Gateway MUST auto-restart unattended.** Today (2026-05-29)
Gateway lives as an interactive process under the RDP-logged-in user. It dies
on RDP disconnect, on VPS reboot, and (most painfully) on IBKR's mandatory
Sunday compulsory restart. Every recovery requires a human to RDP in and
relaunch -- unacceptable once live.

IBC (Interactive Brokers Controller) wraps IB Gateway with auto-login,
auto-restart, and Windows-service lifecycle. Installed once, then Gateway
survives reboot + compulsory restart + 2FA prompt cycle without intervention.

GitHub: <https://github.com/IbcAlpha/IBC>

---

## 1. Install IBC

Download the latest Windows release zip from
<https://github.com/IbcAlpha/IBC/releases>. As of 2026-05 the current release
is `IBCWin-3.21.0.zip` (verify on the page before downloading).

```powershell
# Unzip to C:\IBC (this is the canonical path; IBC scripts assume it)
New-Item -ItemType Directory -Path C:\IBC -Force | Out-Null
Expand-Archive -Path $env:USERPROFILE\Downloads\IBCWin-3.21.0.zip `
               -DestinationPath C:\IBC -Force

# Verify the four executables IBC ships
Get-ChildItem C:\IBC -Filter "*.bat" | Select Name
# Expect: StartGateway.bat  StartTWS.bat  StopGateway.bat  StopTWS.bat
```

---

## 2. Configure IBC

IBC reads `C:\IBC\config.ini` at launch. The default file ships with every
setting commented out -- you tune only the credentials + Gateway path.

```powershell
# Copy the example into the active config slot
Copy-Item C:\IBC\config.ini.example C:\IBC\config.ini -Force
notepad C:\IBC\config.ini
```

Set (uncomment and fill in) these lines. Everything else can stay default.

```ini
# -- credentials --
IbLoginId=        <your IBKR username>
IbPassword=       <your IBKR password>
# Live = leave TradingMode blank or set to live. Paper = paper.
TradingMode=paper

# -- gateway version + install path --
# IB Gateway version on disk. Check installed-versions dir name.
IbGatewayVersion=1037
# Some installs put Gateway under Jts; both layouts are auto-detected
# if IbDir is unset. Only set this if auto-detect fails.
# IbDir=C:\Jts

# -- auto-relogin on the weekly Sunday restart --
# The single most important setting. Without this you re-login by hand
# every Sunday. Default off in IBC; flip it on.
ReloginAfterSecondFactorAuthenticationTimeout=yes
SecondFactorAuthenticationExitInterval=

# -- IB Gateway auto-restart on its own internal timer (paranoia layer) --
# Tick the matching checkbox in Gateway -> Configure -> Lock and Exit
# -> Auto restart as well. IBC enforces; the checkbox tells Gateway to
# obey IBC's restart request.
AutoRestartTime=

# -- API socket -- must match what Omega.exe and the bridge connect to --
# Paper account default = 4002. Live = 4001. Bridge currently uses 4002.
OverrideTwsApiPort=4002

# -- accept all 2FA prompts via mobile / IB Key automatically --
# Without this, Gateway pops a prompt window on every restart and the
# task hangs forever waiting on a click.
IbAutoClosedown=no
ExistingSessionDetectedAction=secondaryexit
```

**Critical 2FA note.** If your IBKR account is enrolled in IB Key (mobile
push for every login) you must EITHER:
- Approve the push within 3 minutes of every restart, OR
- Switch to "no second factor" (paper accounts only), OR
- Use Secure Login System with a physical card reader

IB Key + headless server is a known pain. For paper trading there is no
second-factor required, so the most painless production config is to keep
the paper account live during the validation phase and only migrate to
the live account once IBC has cleared two consecutive Sunday restarts.

---

## 3. Test IBC manually (no service yet)

Before wiring it as a service, prove it launches by hand:

```powershell
# Foreground launch -- expect Gateway window to appear, log in automatically,
# and connect with API socket 4002 active.
C:\IBC\StartGateway.bat

# In a second shell, verify Gateway is up + listening
Get-Process ibgateway, javaw -EA SilentlyContinue
Test-NetConnection -ComputerName 127.0.0.1 -Port 4002

# Stop the manual run before continuing
C:\IBC\StopGateway.bat
```

If StartGateway prints `Auto-login completed` and 4002 binds -- you're ready
for service install. If it errors on credentials, fix config.ini and retry.

---

## 4. Install IBC + Gateway as a Windows Service via NSSM

IBC ships .bat scripts, not native services. NSSM (Non-Sucking Service
Manager) wraps any executable as a Windows service. NSSM is already on the
VPS for Omega.exe.

```powershell
# Install Gateway as service (run as Administrator)
nssm install IBGateway "C:\IBC\StartGateway.bat"

# Set service user = same account that owns the bridge task.
# Service must run AS A USER (not LocalSystem) so it has access to
# the Jts/IBC paths and can present a desktop session to Gateway.
nssm set IBGateway ObjectName "$env:USERDOMAIN\$env:USERNAME" '<password>'

# Auto-restart on crash
nssm set IBGateway AppExit Default Restart
nssm set IBGateway AppRestartDelay 30000

# Log file capture (IBC writes to its own log too, but NSSM stdout is
# useful for service-level diagnostics)
nssm set IBGateway AppStdout C:\Omega\logs\ibgateway_stdout.log
nssm set IBGateway AppStderr C:\Omega\logs\ibgateway_stderr.log

# Start type = Automatic so VPS reboot brings Gateway up before any
# logon happens.
nssm set IBGateway Start SERVICE_AUTO_START

# Start it now
Start-Service -Name IBGateway
Start-Sleep 60
Get-Service IBGateway | Select Status
Test-NetConnection 127.0.0.1 -Port 4002 | Select TcpTestSucceeded
```

If `TcpTestSucceeded: True` and `Get-Process ibgateway` returns a process,
Gateway is now a real service.

---

## 5. Chain the OmegaIbkrBridge task to wait for the service

Currently `OmegaIbkrBridge` uses `-AtStartup` and retries every 5 min. With
Gateway as a service that starts BEFORE the task fires, the very first
bridge attempt usually succeeds. No script change needed -- the 5min retry
just stops retrying once the first connection lands.

If you want to be neat about it, add a dependency so the bridge task waits
for IBGateway service:

```powershell
# Tag OmegaIbkrBridge as waiting on IBGateway service
sc.exe config OmegaIbkrBridge depend=IBGateway
```

Note: only works because the scheduled task itself is wrapped as a service
by NSSM; if it remains a plain Task Scheduler entry the depend= directive
does nothing. Easier path: leave the 5min retry to handle the race.

---

## 6. Verify the full chain end-to-end

After service + IBC + bridge are all installed, reboot the VPS and check
that everything comes up unattended:

```powershell
Restart-Computer -Force

# After reboot, RDP back in (or use Get-Service from another shell)
Start-Sleep 180   # give 3 min for boot + Gateway + bridge + watchdog

# Gateway service running
Get-Service IBGateway | Select Status

# Bridge task fired and is running
Get-ScheduledTaskInfo -TaskName 'OmegaIbkrBridge' |
    Select LastTaskResult, NextRunTime
Get-Process python -EA SilentlyContinue |
    Where {$_.CommandLine -like '*ibkr_dom_bridge*'} |
    Select Id, StartTime

# CSVs growing
$today = (Get-Date).ToString('yyyy-MM-dd')
Get-ChildItem "C:\Omega\logs\ibkr_l2\ibkr_l2_*_$today.csv" |
    Select Name, Length, LastWriteTime

# Watchdog clean
Get-ScheduledTaskInfo -TaskName 'OmegaIbkrL2Freshness' |
    Select LastTaskResult
Get-Content C:\Omega\logs\ibkr_l2_alerts.log -Tail 5 -EA SilentlyContinue
```

Pass: `IBGateway = Running`, bridge `LastTaskResult = 0`, CSVs > 0 bytes
within 60s of boot, no alert log entries.

---

## 7. Weekly compulsory restart -- what should happen

IBKR forces Gateway out every Sunday between 01:00 and 03:00 ET (timing
varies by region; check IBKR notice). With IBC installed:

1. Gateway exits at the IBKR-mandated time.
2. IBC waits `SecondFactorAuthenticationExitInterval` (default 0 = no wait),
   then relaunches Gateway.
3. Gateway auto-logs in via IBC config credentials.
4. API socket 4002 re-binds.
5. The bridge's 5-min retry tick reconnects.
6. CSVs resume writing.

Total downtime: typically 2-5 minutes. The freshness watchdog will alarm
once (STALE entry in `ibkr_l2_alerts.log`) and then go quiet as data resumes.

If after a Sunday cycle the alert log keeps firing -> IBC failed to relogin.
Check `C:\IBC\logs\<date>.txt` for the 2FA / credential rejection that
blocked auto-login.

---

## 8. Migration to LIVE account

**STATUS: LIVE since 2026-07-19 00:05 UTC.** Account `U23757894` confirmed
via `C:\IBC\Logs\IBC-*.txt` (`Login has completed` + `U23757894 Trader
Workstation Configuration` dialog, U-prefix = live not `DU`=paper). Same
IbLoginId/IbPassword as paper -- IBKR's server maps the login to
live-or-paper based on the requested trading mode, no separate live
credentials needed (the handoff doc's "need live creds" assumption was
wrong).

Two things change for live:
- `TradingMode=live` in `config.ini`
- Bridge `--port 4001` (was 4002 for paper) and Omega FIX session port.

**Gotcha (bit us 2026-07-19): `C:\IBC\StartGateway.bat` has its own
`set TRADING_MODE=paper` line that OVERRIDES `config.ini`'s `TradingMode`
key.** Editing config.ini alone silently does nothing -- IBC logs
`Setting Trading mode = paper` at login regardless. Must edit BOTH files:
`config.ini` (`TradingMode=live`) AND `StartGateway.bat`
(`set TRADING_MODE=live`). Backups left as `*.bak_20260719_paper` in
`C:\IBC\`.

Update both atomically. The bridge task already pulls from
`register_omega_ibkr_bridge.ps1`; change `$Port = 4002` to `$Port = 4001`
in that script, re-run as Administrator, then re-run
`register_ibkr_l2_watchdog.ps1`. Live also forces real 2FA (IB Key mobile
push, confirmed 2026-07-19 -- took 158s, exceeded the config's implicit
warning threshold but completed before the 60s post-2FA exit timer).
**Unresolved as of 2026-07-19: `AutoRestartTime=03:00` daily unattended
restart will hit the same 2FA push with nobody there to approve it at
3am** -- `TWOFA_TIMEOUT_ACTION=exit` means IBC just exits cleanly and the
5-min watchdog loop retries against a dead login screen until a human
approves. Either tie IB Key approval to a separate auto-acknowledge
gateway (not officially supported), move `AutoRestartTime` to a time
someone's awake, or accept manual approval each occurrence.

---

## 9. Failure modes + diagnostics

| Symptom | Likely cause | Where to look |
|---|---|---|
| `Get-Service IBGateway` shows Stopped on boot | NSSM credentials wrong; service can't start as user | `nssm get IBGateway ObjectName`; reset password |
| Gateway window flashes then exits | IBC config.ini error | `C:\IBC\logs\<date>.txt` last 50 lines |
| Gateway up but port 4002 unbound | `OverrideTwsApiPort` not set; Gateway picked a random port | Edit config.ini, restart service |
| 4002 bound but bridge keeps restarting | API permissions revoked after compulsory restart | Gateway -> Configure -> API -> Settings; tick "Enable ActiveX and Socket Clients" + add 127.0.0.1 to Trusted IPs |
| Sunday alarm fires every week | IBC unable to handle 2FA prompt | Switch paper -> live without 2FA, or accept manual weekly approval |
| ALL CSVs empty post-boot | Bridge crashing on missing Python deps | Verify `requirements.txt` install completed; check `tools/ibkr_dom_bridge_requirements.txt` is current |
| Watchdog log spams across midnight | UTC day rollover writes to next day's CSV; old day still 0B | Expected; watchdog skips the rollover minute since it reads "today" only |

---

## 10. Companion files in this repo

* `tools/register_omega_ibkr_bridge.ps1` -- bridge task (run AFTER IBC)
* `tools/register_ibkr_l2_watchdog.ps1` -- freshness watchdog task
* `tools/ibkr_l2_freshness_check.ps1` -- watchdog script body
* `tools/ibkr_dom_bridge_requirements.txt` -- pinned Python deps
* `tools/ibkr_dom_bridge.py` -- the bridge itself

Installation order on a fresh VPS:

1. Install IB Gateway (manual download from IBKR portal).
2. Install IBC + register `IBGateway` NSSM service (this doc).
3. Run `register_omega_ibkr_bridge.ps1`.
4. Run `register_ibkr_l2_watchdog.ps1`.
5. Reboot and verify per section 6.
