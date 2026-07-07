#!/usr/bin/env bash
# =============================================================================
# pull_vps_display.sh — mirror the VPS's READ-ONLY display/analysis data to the
# Mac so dashboards + analysis run on the Mac, not the 3GB box.
#
# WHY: the only reason the desk/health/bracket views ran on the VPS was that the
# files live there. Pull them to the Mac once every few min over ONE multiplexed
# ssh connection (cheaper than running the GUI processes on the VPS, and far
# cheaper than the ad-hoc AI ssh queries the RAM guard exists to kill). The live
# trading path (engine + Gateway + recorders) NEVER moves — this only copies
# OUT the artifacts they produce.
#
# SAFE: read-only pulls. ControlMaster reuses ONE connection for all files.
# BatchMode + short ConnectTimeout => a frozen box fails fast, never hangs the
# Mac cron. Missing files are skipped, not fatal.
#
# Cron (Mac), every 5min:
#   */5 * * * * /Users/jo/Omega/tools/pull_vps_display.sh >> /tmp/pull_vps_display.log 2>&1
# =============================================================================
set -uo pipefail

MIRROR="$HOME/Omega-vps-mirror"
mkdir -p "$MIRROR/logs/trades" "$MIRROR/logs/health" "$MIRROR/state"

# one shared, short-lived multiplexed connection for every scp below
CM=(-o ConnectTimeout=8 -o BatchMode=yes -o ServerAliveInterval=10 -o ServerAliveCountMax=2 \
    -o ControlMaster=auto -o ControlPath=/tmp/ssh-omega-pull-%r@%h:%p -o ControlPersist=30)

pull () {  # pull <remote-path> <local-dir>   (non-fatal on miss/timeout)
  scp -q "${CM[@]}" "omega-vps:$1" "$2" 2>/dev/null \
    && echo "[$(date '+%F %T')] ok  $1" \
    || echo "[$(date '+%F %T')] MISS $1"
}

# ledger (shadow trades) + health + open positions — the display/analysis inputs
pull 'C:/Omega/logs/trades/*.csv'            "$MIRROR/logs/trades/"
pull 'C:/Omega/logs/health/status.json'      "$MIRROR/logs/health/"
pull 'C:/Omega/HEALTH_STATUS.json'           "$MIRROR/"
pull 'C:/Omega/state/open_positions.dat'     "$MIRROR/state/"
pull 'C:/Omega/bracket-bot/data/trades.ndjson' "$MIRROR/"
# companion telemetry mirror: the python stall_accountant cron that wrote this was retired
# 2026-07-06 (C++ StallCompanion in Omega.exe now owns C:/Omega/companion_state.json); cockpit
# + feeds_selftest still read the Mac copy, so keep it fresh from the VPS truth (2026-07-08).
pull 'C:/Omega/companion_state.json'         "$HOME/stall-accountant/"

# tear down the shared connection
ssh -O exit "${CM[@]}" omega-vps 2>/dev/null || true
