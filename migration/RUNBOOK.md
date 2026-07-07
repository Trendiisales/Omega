# VPS Migration Runbook — 185.167.119.59 → 45.85.3.79 (ForexVPS Edge)

Date opened: 2026-07-07. Reason: memory pressure on the old 4GB box (the
2026-06-27 RDP freeze was RAM thrash); the Edge package adds RAM + CPUs.

New box: `45.85.3.79`, hostname `VM16893535923733529.forexvps.net`,
RDP **45.85.3.79:42014** (provider port — must be included), user `trader`.
Password: operator's password manager. **Never in this repo.**

> Repo-side address propagation already shipped in S-2026-07-07g — every
> operational script now points at 45.85.3.79. This runbook is the
> server-side half.

> **`4_NEW_VPS_APPLIED_SETTINGS.md`** records what was ACTUALLY applied to
> the new box on 2026-07-07 incl. fixes not in these scripts (vc_redist
> 14.44 crash fix, scheduled-task SID/hostname rewrite, firewall rules,
> git-history restore, ForexVPS provider facts). Read it before rebuilding.

## Order of operations

| # | Where | What |
|---|-------|------|
| 1 | New box (RDP :42014) | `2_NEW_VPS_BOOTSTRAP.ps1` as Admin — sshd:2222, firewall 2222/7779/7781, git |
| 2 | Old box | `1_OLD_VPS_EXPORT.ps1` — builds `C:\Omega_migration\omega_migration_*.zip` |
| 3 | Old box | `scp -P 2222 C:\Omega_migration\omega_migration_*.zip trader@45.85.3.79:C:/omega_migration.zip` |
| 4 | New box | `3_NEW_VPS_RESTORE_VERIFY.ps1` as Admin — restores + starts + prints PASS/FAIL block |
| 5 | Mac | `ssh-keygen -R "[45.85.3.79]:2222"` then `ssh -p 2222 trader@45.85.3.79 exit` (accept new host key; keys carried over so no password expected — if prompted, rerun `FIX_SSH.ps1` on the new box) |
| 6 | Mac | reload x1-live launchd job: `launchctl unload ~/Library/LaunchAgents/com.omega.x1-live.plist && cp incidents/2026-06-02-x1-overlay-validation/com.omega.x1-live.plist ~/Library/LaunchAgents/ && launchctl load ~/Library/LaunchAgents/com.omega.x1-live.plist` |
| 7 | New box | IB Gateway: install per `docs/IBC_GATEWAY_AUTOSTART.md` (C:\IBC + C:\Jts settings already restored), log in — **expect IBKR's new-location security challenge** |
| 8 | New box | First real deploy: `cd C:\Omega ; powershell -File OMEGA.ps1 deploy` once MSVC/CMake toolchain is installed (until then the carried-over `Omega.exe` runs) |

## External parties that see the new IP

- **BlackBull FIX**: confirm whether the FIX session is IP-whitelisted; if so
  add `45.85.3.79` BEFORE cutover or the engine connects to nothing.
- **IBKR**: first Gateway login from the new IP triggers a security prompt;
  have the operator's 2FA device at hand.

## Cutover + decommission gates

Do **not** cancel the old box until ALL of:

1. `3_NEW_VPS_RESTORE_VERIFY.ps1` prints all-PASS (4002 may fail until
   Gateway is up — clear it before this gate counts).
2. Deploy Hygiene three-way check on the new box: VPS `git rev-parse HEAD`
   == `git rev-parse origin/main` == stderr `Git hash:` line.
3. Shadow ledger intact: `C:\Omega\logs\trades\omega_trade_closes.csv` row
   count on new box >= old box (it is THE record of engine performance).
4. One clean trading session observed on the new box (GUI :7779 shows live
   telemetry, no RED from `tools\omega_health_alarm.ps1`).
5. Memory-Omega vault updated (deploy mandate): entity page + index + log
   naming the migration and the verified running hash.

Then, on the old box: stop services (`nssm stop Omega`, `nssm stop
IBGateway`), take a final copy of `C:\Omega\logs\`, and only then cancel.

## Post-migration follow-ups

- **Rotate the trader password** (it transited a chat session during setup).
- Once new RAM size is confirmed, revisit `tools/omega_health_alarm.ps1`
  RAM thresholds (RED <250MB free was tuned for the 3GB box; still safe as a
  floor, but a proportional bump gives earlier warning).
- Latency spot-check vs broker: the old box was London ~68ms RTT
  (`backtest/CULL_S13_LATENCY_EDGE.md`). If the new DC differs materially,
  re-examine latency-sensitive assumptions.
- Disable sshd password auth on the new box after key auth is confirmed
  (`PasswordAuthentication no` in `C:\ProgramData\ssh\sshd_config`).
