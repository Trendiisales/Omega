#!/usr/bin/env python3
# ============================================================================
# update_guard_selftest.py  (S-2026-07-12)  — MAX-PROTECTION software-update guard
#
# WHY: a software update that AUTO-RESTARTS any box kills live trading (Mac crypto
# crons + chimera relay; omega-new Omega.exe; josgp1 chimera service). This guard
# asserts NO box can auto-reboot for an update, and NOTIFIES if a reboot is pending
# so the operator schedules a maintenance window (stop -> update -> reboot -> restart).
#
# Covers all 3 boxes:
#   Mac (this)   — AutomaticallyInstallMacOSUpdates must be 0.
#   omega-new    — WindowsUpdate AU: AUOptions=2 (notify) + NoAutoRebootWithLoggedOnUsers=1.
#   josgp1       — apt unattended Automatic-Reboot must NOT be true; chimera systemd-enabled.
#
# RED (exit 2) = a box's auto-reboot protection is weakened -> re-lock it.
# WARN (still green) = a reboot is PENDING somewhere -> plan a manual window.
#
# ── INPUT / STATE / OUTCOME classification (S-2026-07-24 audit) ──────────────
#   INPUT   : registry/config/apt values pulled read-only (AUOptions, NoAutoReboot,
#             Automatic-Reboot, AutomaticallyInstallMacOSUpdates, reboot-required).
#   STATE   : the CONFIGURED posture — will a box auto-reboot; is the service *enabled*.
#   OUTCOME : does live trading actually SURVIVE / is it RUNNING right now?
#
# THE OUTCOME GAP this audit closed: the guard verified `systemctl is-enabled chimera`
# and the Windows auto-reboot registry — both are CONFIG PROXIES for "trading survives a
# reboot". Neither proves the trading process is ACTUALLY RUNNING. `is-enabled` != `is-active`:
# a box can have flawless no-auto-reboot posture AND an enabled unit while the trading
# process crashed/stopped hours ago (cf. the josgp1 mode-conflict crash-loop and the
# omega service left Stopped/Manual after the 07-07 migration) — and the OLD guard would
# print GREEN over dead trading. FIX: each remote box now also asserts the OUTCOME —
# chimera `is-active`==active (not merely enabled) and omega-new's Omega.exe process is
# actually running — so a passing guard means "protected AND trading now", not just "config ok".
# ============================================================================
import subprocess, sys

fails, warns = [], []

def run(cmd, timeout=30):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return (r.stdout or "") + (r.stderr or "")
    except Exception as e:
        return f"__ERR__ {e}"

def mac():
    v = run(["defaults", "read", "/Library/Preferences/com.apple.SoftwareUpdate",
             "AutomaticallyInstallMacOSUpdates"]).strip()
    if v != "0":
        fails.append(f"Mac: AutomaticallyInstallMacOSUpdates={v!r} (must be 0) — auto-restart RE-ENABLED")
    lst = run(["softwareupdate", "--list"])
    if "action: restart" in lst.lower() or "restart," in lst.lower():
        warns.append("Mac: a restart-required macOS update is available (won't auto-install now; plan a manual window)")

def win():
    # single-quote the registry paths (the 'Auto Update' key has a space that breaks
    # unquoted Test-Path); outer -Command stays double-quoted, no nesting conflict.
    ps = (r"$au=Get-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU' "
          r"-ErrorAction SilentlyContinue; "
          r"Write-Output ('AUO='+$au.AUOptions+' NAR='+$au.NoAutoRebootWithLoggedOnUsers+' RBP='"
          r"+(Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired')); "
          # OUTCOME: is the Omega trading process ACTUALLY running now? (config posture is only a
          # proxy for 'trading survives a reboot' — this asserts the live outcome directly.)
          r"$op=Get-Process Omega -ErrorAction SilentlyContinue; Write-Output ('OMEGARUN='+[bool]$op)")
    o = run(["ssh", "omega-new", 'powershell -NoProfile -Command "' + ps + '"'])
    if "__ERR__" in o:
        warns.append(f"omega-new: unreachable — update posture unverifiable ({o.strip()[:60]})"); return
    if "AUO=2" not in o:
        fails.append(f"omega-new: AUOptions is not 2/notify (auto-install risk) -> {o.strip()}")
    if "NAR=1" not in o:
        fails.append(f"omega-new: NoAutoRebootWithLoggedOnUsers is not 1 (auto-reboot risk) -> {o.strip()}")
    if "RBP=True" in o:
        warns.append("omega-new: Windows reboot PENDING (plan a manual window)")
    if "OMEGARUN=True" not in o:
        fails.append("omega-new: Omega.exe is NOT running now — the no-auto-reboot config is only a "
                     "PROXY for 'trading survives'; the live OUTCOME is that Omega is DOWN (not "
                     "trading). Investigate/restart before trusting a green update posture.")

def lin():
    o = run(["ssh", "chimera-direct",
             "grep -rhE 'Automatic-Reboot' /etc/apt/apt.conf.d/ 2>/dev/null | grep -v '^//'; echo ---; "
             "[ -f /var/run/reboot-required ] && echo REBOOT_PENDING; "
             # STATE (config): is the unit enabled -> would auto-start after a reboot.
             "echo ENABLED=$(systemctl is-enabled chimera 2>/dev/null); "
             # OUTCOME (live): is chimera ACTUALLY running now. enabled != active — a crashed/
             # stopped unit stays 'enabled' while trading is dead (the proxy-vs-outcome gap).
             "echo ACTIVE=$(systemctl is-active chimera 2>/dev/null)"])
    if "__ERR__" in o:
        warns.append(f"josgp1: unreachable — update posture unverifiable ({o.strip()[:60]})"); return
    if 'automatic-reboot "true"' in o.lower():
        fails.append("josgp1: apt unattended Automatic-Reboot=true — auto-reboot RE-ENABLED")
    if "REBOOT_PENDING" in o:
        warns.append("josgp1: reboot PENDING (plan a window; chimera is systemd-enabled so it recovers, but interrupts trading)")
    if "ENABLED=enabled" not in o:
        fails.append("josgp1: chimera service NOT enabled — would NOT auto-start after a reboot")
    if "ACTIVE=active" not in o:
        fails.append("josgp1: chimera enabled but NOT RUNNING now (is-active != active) — the "
                     "auto-reboot config is a PROXY; the live OUTCOME is DEAD trading. Restart chimera.")

for fn in (mac, win, lin):
    try: fn()
    except Exception as e: warns.append(f"{fn.__name__}: check errored: {e}")

print("UPDATE-GUARD SELF-TEST — auto-reboot protection across Mac + omega-new + josgp1")
for w in warns: print("  WARN ", w)
if fails:
    for f in fails: print("  FAIL ", f)
    print("RESULT: RED — a box can auto-reboot for a software update (would kill live trading). Re-lock it.")
    sys.exit(2)
print("  PASS  all 3 boxes locked (no auto-reboot); chimera survives a reboot")
print("RESULT: GREEN")
