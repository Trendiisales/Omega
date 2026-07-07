# SESSION HANDOFF — VPS migration 185.167.119.59 → 45.85.3.79 (2026-07-07)

**For the next AI session, running in a terminal WITH network access to the
boxes (Mac, or the VPS itself).** The sandbox session that prepared this could
not reach either VPS (all ports blocked); everything below is staged and
pushed — your job is to execute, verify, and only then let the operator
decommission the old box.

## New box facts

- IP `45.85.3.79`, hostname `VM16893535923733529.forexvps.net`, ForexVPS
  "Edge" package (upgrade from the old 4GB box — the 2026-06-27 RDP freeze
  was RAM thrash).
- **RDP: `45.85.3.79:42014`** — the provider port MUST be included. User
  `trader`. Password: operator supplies it (password manager — never commit).
- SSH: NOT running until bootstrap step 1 below installs it on **port 2222**
  (our convention; every Mac-side script assumes it).

## Already done (branch `claude/vps-address-migration-cnxcnb`, PR #4, draft)

1. `fb4cc88` — all operational repo references moved to `45.85.3.79`
   (OMEGA.ps1/MONITOR/CLEAR_AND_VERIFY banners, FIX_SSH test, SYMBOLS.md,
   PRE_LIVE_CHECKLIST, cockpit server+html, rdagent tunnel+scp, L2 helpers,
   x1-live plist+script, OMEGA.md §4 rewritten with new box details).
   Dated handoffs/postmortems keep the old IP as history — intentional.
2. `36b7c2b` — `migration/` kit: the three scripts + RUNBOOK.md used below.

## Execution plan (in order — details in `migration/RUNBOOK.md`)

1. **New box** (RDP `45.85.3.79:42014`, admin PowerShell):
   run `migration\2_NEW_VPS_BOOTSTRAP.ps1`
   → sshd on 2222, firewall 2222/7779/7781, git.
   Verify: `Get-NetTCPConnection -State Listen -LocalPort 2222`.
2. **Old box** (`ssh -p 2222 trader@185.167.119.59` or RDP):
   run `migration\1_OLD_VPS_EXPORT.ps1` (pull the branch first, or copy the
   script over) → `C:\Omega_migration\omega_migration_<stamp>.zip`.
   Then from the old box:
   `scp -P 2222 C:\Omega_migration\omega_migration_*.zip trader@45.85.3.79:C:/omega_migration.zip`
3. **New box**: run `migration\3_NEW_VPS_RESTORE_VERIFY.ps1`
   → restores C:\Omega + tasks + NSSM service + ssh keys + IBC/Jts, starts
   the service from the carried-over binary, prints a PASS/FAIL block.
   **Paste the full block back to the operator.**
4. **Mac**: `ssh-keygen -R "[45.85.3.79]:2222"` then
   `ssh -p 2222 trader@45.85.3.79 exit` (keys carried over — if it prompts
   for a password, rerun `FIX_SSH.ps1` on the new box).
   Reload x1-live: `launchctl unload ~/Library/LaunchAgents/com.omega.x1-live.plist;
   cp incidents/2026-06-02-x1-overlay-validation/com.omega.x1-live.plist
   ~/Library/LaunchAgents/; launchctl load ~/Library/LaunchAgents/com.omega.x1-live.plist`
5. **New box**: IB Gateway install per `docs/IBC_GATEWAY_AUTOSTART.md`
   (C:\IBC + C:\Jts settings already restored). Expect IBKR's new-location
   2FA challenge. Then register the IBGateway NSSM service (doc §4) and
   confirm port 4002 listens.
6. **New box, once toolchain installed**: real deploy
   `cd C:\Omega ; powershell -File OMEGA.ps1 deploy` and run the Deploy
   Hygiene three-way hash check (HEAD == origin/main == stderr `Git hash:`).

## Blocking externals (check BEFORE cutover)

- **BlackBull FIX**: if the session is IP-whitelisted, `45.85.3.79` must be
  added first — otherwise the engine connects to nothing and every engine's
  order path is silently dead.
- **IBKR**: new-IP login challenge (step 5).

## Decommission gates (ALL must pass before cancelling the old box)

1. Verify block all-PASS (incl. 4002 once Gateway is up).
2. Shadow-ledger parity: `omega_trade_closes.csv` row count new >= old.
   The shadow ledger is THE record of engine performance — losing it loses
   the forward track record.
3. One clean trading session on the new box (GUI :7779 live, no RED from
   `tools\omega_health_alarm.ps1`).
4. Memory-Omega vault updated (deploy mandate): entity page + index.md +
   log.md naming the migration + verified running hash.
5. Old box: final copy of `C:\Omega\logs\`, stop services, then cancel.

## Follow-ups after cutover

- **Rotate the trader password** (it transited chat during setup).
- Retune `tools/omega_health_alarm.ps1` RAM thresholds once actual RAM is
  known (RED <250MB free was tuned for the 3GB box).
- Latency spot-check vs broker (old box: London ~68ms RTT, see
  `backtest/CULL_S13_LATENCY_EDGE.md`).
- Disable sshd PasswordAuthentication on the new box after key auth works.
- Merge PR #4 to main same-day (branch-freshness rule: no long-lived
  branches) — the deploy in step 6 builds from main, so merge BEFORE it.
