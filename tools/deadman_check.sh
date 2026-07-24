#!/bin/bash
# deadman_check.sh — DEAD-MAN'S SWITCH for the Omega alerting itself (audit gap 13).
#
# THE PROBLEM: every monitor in this repo proves the TRADING SYSTEM is healthy. NOTHING
# proves the ALERTING is healthy. If the Mac sleeps, cron/launchd dies, or the box is
# unreachable, every monitor simply stops running — and silence reads as "all green".
# That is the universal false-green: the loudest failure is the one that makes no sound.
#
# THE SWITCH: tools/ibkr_login_watch.sh (a 1-min cron) stamps a heartbeat every tick:
#   /tmp/omega_alerting_heartbeat.epoch  <- unix seconds of the last completed tick.
# This checker reads that heartbeat and alerts if it is STALE (older than MAX_AGE_S) —
# meaning the 1-min alerting cron has not run, i.e. the alerting is dead.
#
# ── SKETCH / OWED ────────────────────────────────────────────────────────────────────
# Run LOCALLY this is only a partial guard: a checker on the same Mac cannot fire if that
# Mac is the thing that is asleep (it is asleep too). The ROBUST dead-man's switch runs
# OFF-BOX — e.g. on omega-new or a phone/uptime service (healthchecks.io, cronitor) that
# the Mac PINGS each tick and that alarms when the ping stops. This local version catches
# "cron died / crashed while the Mac is awake" today; the off-box ping is OWED. To adopt
# the off-box form: have ibkr_login_watch.sh also `curl -fsS https://hc-ping.com/<uuid>`
# each tick; the service alarms when the ping goes quiet. No secrets are committed here.
# ──────────────────────────────────────────────────────────────────────────────────────
#
# Usage:  bash tools/deadman_check.sh            # exit 0 alive / 2 stale / 1 no-heartbeat
# Cron (optional, NOT yet installed — sketch):
#   */10 * * * * /Users/jo/Omega/tools/deadman_check.sh
set -u
HEARTBEAT=/tmp/omega_alerting_heartbeat.epoch
MAX_AGE_S=$((10*60))   # the source cron is every 1 min; 10 min stale = alerting dead
LOG=/tmp/deadman_check.log
now=$(date +%s)
ts() { date -u '+%Y-%m-%d %H:%MZ'; }
notify() { /usr/bin/osascript -e "display notification \"$2\" with title \"$1\" sound name \"Basso\"" 2>/dev/null; }

if [ ! -f "$HEARTBEAT" ]; then
  echo "[$(ts)] RESULT: RED no heartbeat file — alerting never ran (or /tmp cleared)" >> "$LOG"
  notify "☠️ ALERTING DEAD (no heartbeat)" "No Omega alerting heartbeat file — the 1-min login-watch cron has never stamped it. The monitors may be silent. Check cron/launchd on this Mac."
  exit 1
fi
hb=$(cat "$HEARTBEAT" 2>/dev/null || echo 0)
age=$(( now - hb ))
if [ "$age" -ge "$MAX_AGE_S" ]; then
  echo "[$(ts)] RESULT: RED heartbeat stale ${age}s (>= ${MAX_AGE_S}s) — alerting cron not running" >> "$LOG"
  notify "☠️ ALERTING DEAD (stale ${age}s)" "Omega alerting heartbeat is ${age}s old — the 1-min monitors are NOT running (Mac slept / cron died). Silence is NOT proof of health. Wake + verify cron."
  exit 2
fi
echo "[$(ts)] RESULT: GREEN heartbeat ${age}s old — alerting alive" >> "$LOG"
exit 0
