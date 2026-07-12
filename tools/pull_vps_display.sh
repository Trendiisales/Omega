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
mkdir -p "$MIRROR/logs/trades" "$MIRROR/logs/health" "$MIRROR/state" "$MIRROR/data"

# one shared, short-lived multiplexed connection for every scp below
CM=(-o ConnectTimeout=8 -o BatchMode=yes -o ServerAliveInterval=10 -o ServerAliveCountMax=2 \
    -o ControlMaster=auto -o ControlPath=/tmp/ssh-omega-pull-%r@%h:%p -o ControlPersist=30)

pull () {  # pull <remote-path> <local-dir>   (non-fatal on miss/timeout)
  scp -q "${CM[@]}" "omega-new:$1" "$2" 2>/dev/null \
    && echo "[$(date '+%F %T')] ok  $1" \
    || echo "[$(date '+%F %T')] MISS $1"
}

# ledger (shadow trades) + health + open positions — the display/analysis inputs
pull 'C:/Omega/logs/trades/*.csv'            "$MIRROR/logs/trades/"
pull 'C:/Omega/logs/health/status.json'      "$MIRROR/logs/health/"
pull 'C:/Omega/logs/HEALTH_STATUS.json'      "$MIRROR/"  # path fixed 2026-07-08: file lives under logs\ (was C:/Omega/, perpetual MISS)
pull 'C:/Omega/state/open_positions.dat'     "$MIRROR/state/"
pull 'C:/Omega/bracket-bot/data/trades.ndjson' "$MIRROR/"
# warm-seed freshness truth (2026-07-12, audit A3): OmegaSeedRefresh refreshes the BOX
# seed nightly; the Mac repo copy never updates, so data_health_monitor watched a file
# the engine never loads (false 17d-stale). Mirror the box seed; monitor reads this.
pull 'C:/Omega/data/mgc_30m_hist.csv'        "$MIRROR/data/"
pull 'C:/Omega/data/mgc_h1_hist.csv'         "$MIRROR/data/"
# companion telemetry mirror: the python stall_accountant cron that wrote this was retired
# 2026-07-06 (C++ StallCompanion in Omega.exe now owns C:/Omega/companion_state.json); cockpit
# + feeds_selftest still read the Mac copy, so keep it fresh from the VPS truth (2026-07-08).
pull 'C:/Omega/companion_state.json'         "$HOME/stall-accountant/"
# S-2026-07-08c cockpit-staleness fix: the per-BOOK stall dirs (companion_positions.json +
# companion_closed.csv) power the cockpit trades panel; the retired Mac python leftovers
# were frozen at Jul-6 and showed phantom 5-day-old XAU opens. Mirror the LIVE VPS tree.
scp -q -r "${CM[@]}" "omega-new:C:/Omega/stall" "$HOME/stall-accountant/vps_mirror/" 2>/dev/null || true

# tear down the shared connection
ssh -O exit "${CM[@]}" omega-new 2>/dev/null || true
