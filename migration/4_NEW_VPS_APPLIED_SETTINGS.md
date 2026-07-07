# New-box (45.85.3.79) applied settings — record of what actually shipped

Date: 2026-07-07 (NZ). Box: `fxut9342750`, ForexVPS Edge, 6 GB RAM, user `trader`.
This file records every setting applied to the new box during the migration
session, including fixes NOT in the original bootstrap/restore scripts, so the
box can be rebuilt from scratch without re-deriving any of it.
**No credentials in this file** (repo rule — see RUNBOOK.md).

## 1. MSVC runtime fix (crash-loop root cause)

Omega.exe (VS2022-built) crash-looped on boot with 0xc0000005 inside
`MSVCP140.dll` — the box shipped a VS2019-era 14.29 redistributable.

- **Fix:** installed `vc_redist.x64.exe` → runtime **14.44.35211**.
- Any fresh Windows box for Omega MUST get the latest VC++ x64 redist BEFORE
  first service start. Symptom of the miss: nssm service goes `Paused`
  (crash-throttle), Application event log shows Application Error 1000 in
  MSVCP140.dll.
- Installer left at `C:\vc_redist.x64.exe` (delete after verified deploy).

## 2. Windows Firewall inbound rules

ForexVPS support (2026-07-07 email reply): **their network blocks only SMTP**;
there is no provider edge firewall and no portal self-service firewall page.
All port work is Windows Firewall on the box itself.

Rules added (idempotent — `netsh` duplicates are harmless but check first):

```
netsh advfirewall firewall add rule name="Omega SSH 2222" dir=in action=allow protocol=TCP localport=2222
netsh advfirewall firewall add rule name="Omega GUI 7779" dir=in action=allow protocol=TCP localport=7779
netsh advfirewall firewall add rule name="Omega API 7781" dir=in action=allow protocol=TCP localport=7781
```

Verified from Mac: `nc -vz 45.85.3.79 2222|7779|7781` → all succeeded.
(`2_NEW_VPS_BOOTSTRAP.ps1` contains the same rules; they were re-applied by
hand this session — the earlier bootstrap run predated the support
confirmation that nothing upstream blocks them.)

## 3. Scheduled tasks — re-registration fix (14 tasks)

`3_NEW_VPS_RESTORE_VERIFY.ps1` imported the exported task XMLs
(`C:\omega_migration_unpacked\_system\tasks\*.xml`). 24 registered clean
(SYSTEM-principal ones, `S-1-5-18`). **14 failed** with
`No mapping between account names and security IDs (0x80070534)`.

Root cause: those 14 XMLs carry the OLD box's identity in two places —
- `<UserId>S-1-5-21-3293120269-4268945235-2183159064-500</UserId>`
  (old box's built-in **Administrator** SID, not `trader`), and
- literal hostname `fxut8777965` in trigger `UserId`s.

Neither resolves on the new box. Supplying `-User trader -Password ...` does
NOT fix it (it forces password-logon and conflicts with the XML's
`S4U`/`InteractiveToken` logon types → "user name or password is incorrect").

**Working fix** (no password needed — S4U and InteractiveToken don't store
one): rewrite both identities in the XML string, keep the original LogonType,
register without credentials:

```powershell
foreach ($f in Get-ChildItem C:\omega_migration_unpacked\_system\tasks\*.xml) {
  $n = $f.BaseName
  if (Get-ScheduledTask -TaskName $n -ErrorAction SilentlyContinue) { continue }
  $x = (Get-Content $f.FullName -Raw) `
       -replace 'S-1-5-21-3293120269-4268945235-2183159064-500', 'fxut9342750\trader' `
       -replace 'fxut8777965', 'fxut9342750'
  Register-ScheduledTask -Xml $x -TaskName $n -ErrorAction Stop | Out-Null
}
```

Result 2026-07-07: **STILL-MISSING=0** — all 38 exported tasks registered.
The 14 fixed: IbkrGateway, OmegaAuroraSnapshot, OmegaBigCapBridge,
OmegaGapperRecorder, OmegaGapShortDaily, OmegaGexSnapshot,
OmegaHealthMonitor, OmegaIbkrBridge, OmegaIbkrL2Freshness, OmegaLogRotate,
OmegaMgcLiveBars, OmegaPumpShadow, OmegaSeedRefresh, OmegaWeeklyReview.

Gotcha for future scripts: `Register-ScheduledTask` failures are
**non-terminating** — a bare `try/catch` without `-ErrorAction Stop` prints
success while registering nothing. Verify with a Get-ScheduledTask diff, never
trust the loop's own output.

## 4. Git history restore (.git was excluded from the zip)

The export zip did not carry `.git`. Recreated on the new box:

```
cd C:\Omega
git init
git remote add origin git@github.com:Trendiisales/Omega.git
git config core.sshCommand "ssh -i C:/Omega/.ssh/omega_deploy -o StrictHostKeyChecking=accept-new"
git fetch origin            # ~3.5 GB history
git checkout -f -B main origin/main   # AFTER fetch completes
```

Deploy key: `C:/Omega/.ssh/omega_deploy` (read auth verified via ls-remote).
Fetch logs: `C:\git_fetch_out.log` / `C:\git_fetch_err.log`.

## 5. Mac-side ssh config (cutover to direct)

After firewall rules opened, `~/.ssh/config` `omega-new` flipped from the
ProxyJump-via-old-box tunnel to direct:

```
Host omega-new
    HostName 45.85.3.79
    Port 2222
    User trader
    StrictHostKeyChecking accept-new
    ServerAliveInterval 30
```

The reverse-tunnel PowerShell window on the new box is now obsolete and can
be closed. `ssh omega-vps` remains the OLD box until decommission.

## 6. ForexVPS provider facts (learned 2026-07-07)

- Network blocks **SMTP only**; all other inbound ports are box-local
  Windows Firewall.
- Portal (`portal.forexvps.net`) has NO self-service firewall page (Manage
  Product = Language/Backups/OS/Location only). Support email:
  support@forexvps.net; portal live-chat is faster.
- `cp.forexvps.net` is a DIFFERENT credential realm from the portal; the old
  box lives under a separate BlackBull-sponsored account.
- RDP to the new box uses provider port **45.85.3.79:42014**.

## 7. IB Gateway + IBC (installed 2026-07-07, PROVEN, parked until cutover)

Neither `C:\IBC` nor `C:\Jts` was in the migration zip, and the old box's
install4j-bundled JRE lives OUTSIDE both. Three pieces were copied old→new
(tar over ssh — old box's `trader` pubkey added to new box authorized_keys
for direct box-to-box scp):

1. `C:\IBC` (config.ini carries IBKR creds — **never in repo**) +
   `C:\Jts` (Gateway **1047** standalone) — tarred with
   `--exclude IBC/logs --exclude Jts/<jxbrowser-profile-dir>` (live Gateway
   holds locks on those).
2. `C:\Program Files\Common Files\i4j_jres\Oda-jK0QgTEmVssfllLP` (zulu
   17.0.16 JRE). **Without this IBC dies: "Can't find suitable Java
   installation"** — the Gateway launcher's install4j metadata pins that
   exact path. Any future box rebuild must carry `i4j_jres` too.

Launch path = the restored `IbkrGateway` scheduled task →
`C:\Omega\bracket-bot\scripts\gateway_watchdog.ps1` (logon + time triggers,
Interactive as trader).

**Login validation result (03:30 box time):** IBC set username/password,
clicked Paper Log In, reached `Authenticating...` — credentials ACCEPTED, no
2FA challenge (paper mode). Blocked only by `Existing session detected`: the
OLD box's Gateway holds the same paper account and is primary → new box
yielded (exit 1100) and old box's Gateway verified unharmed (java alive,
old 4002 still bound).

**The `IbkrGateway` task on the new box is DISABLED** so its time trigger
doesn't repeatedly fight the old box's live session. At cutover:
stop Gateway on old box → `schtasks /change /tn IbkrGateway /enable` +
`/run` on new box → verify `Test-NetConnection 127.0.0.1 -Port 4002`.

## 8. Still owed (tracked in RUNBOOK + handoff)

- `git checkout -f -B main origin/main` once the fetch finishes; then the
  Deploy Hygiene three-way hash check.
- IB Gateway install (`docs/IBC_GATEWAY_AUTOSTART.md`) — IBKR creds go in the
  IBC config on the box, never in this repo; expect the new-location 2FA
  challenge on the operator's device.
- BlackBull FIX whitelist of 45.85.3.79 (operator).
- Rotate the `trader` Windows password (transited chat during setup);
  disable sshd PasswordAuthentication after.
- Retune `tools/omega_health_alarm.ps1` RAM thresholds for the 6 GB box.
- Remove `C:\git-installer.exe`, `C:\vc_redist.x64.exe`,
  `C:\omega_migration.zip`, `C:\omega_migration_unpacked` after verified
  deploy.
- MSVC/CMake toolchain for on-box deploys.
