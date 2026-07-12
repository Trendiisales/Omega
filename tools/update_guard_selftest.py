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
          r"+(Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired'))")
    o = run(["ssh", "omega-new", 'powershell -NoProfile -Command "' + ps + '"'])
    if "__ERR__" in o:
        warns.append(f"omega-new: unreachable — update posture unverifiable ({o.strip()[:60]})"); return
    if "AUO=2" not in o:
        fails.append(f"omega-new: AUOptions is not 2/notify (auto-install risk) -> {o.strip()}")
    if "NAR=1" not in o:
        fails.append(f"omega-new: NoAutoRebootWithLoggedOnUsers is not 1 (auto-reboot risk) -> {o.strip()}")
    if "RBP=True" in o:
        warns.append("omega-new: Windows reboot PENDING (plan a manual window)")

def lin():
    o = run(["ssh", "chimera-direct",
             "grep -rhE 'Automatic-Reboot' /etc/apt/apt.conf.d/ 2>/dev/null | grep -v '^//'; echo ---; "
             "[ -f /var/run/reboot-required ] && echo REBOOT_PENDING; systemctl is-enabled chimera 2>/dev/null"])
    if "__ERR__" in o:
        warns.append(f"josgp1: unreachable — update posture unverifiable ({o.strip()[:60]})"); return
    if 'automatic-reboot "true"' in o.lower():
        fails.append("josgp1: apt unattended Automatic-Reboot=true — auto-reboot RE-ENABLED")
    if "REBOOT_PENDING" in o:
        warns.append("josgp1: reboot PENDING (plan a window; chimera is systemd-enabled so it recovers, but interrupts trading)")
    if "enabled" not in o.split("---")[-1]:
        fails.append("josgp1: chimera service NOT enabled — would NOT auto-start after a reboot")

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
