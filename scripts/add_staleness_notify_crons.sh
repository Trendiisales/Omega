#!/usr/bin/env bash
# add_staleness_notify_crons.sh — idempotent installer for ACTIVE staleness notification
# (staleness audit 2026-07-08, operator: "make sure this can never happen, and i want to
# know what preventative action you take to stop this and notify us").
#
# Gap it closes: tools/feeds_selftest.py only ran at Claude session start (print-only) —
# a live feed could go stale for DAYS between sessions with zero notification. The wave
# companion selftest ran hourly by cron but only wrote a log on RED. Neither screamed.
# protection_selftest.py + liveness_check.py already notify via osascript — this brings
# the remaining two selftests up to the same standard.
#
# Installs (idempotent — marker-matched, second run = no-op):
#   1. feeds_selftest.py every 30min; macOS notification + Basso on exit 1 (LIVE feed stale).
#      exit 2 (research AMBER) logs only — not wake-the-operator material.
#   2. wave_companion_selftest RED notification appended to the existing hourly wave cron.
#
# SAFETY (feedback-crontab-edit-via-script — an inline sed/heredoc once WIPED the crontab):
#   * backs up live crontab to /tmp/ct.bak.<epoch> BEFORE any change
#   * builds new crontab in a temp file, installs via `crontab <file>` (atomic)
#   * restore with:  crontab /tmp/ct.bak.<epoch>
set -euo pipefail

TS=$(date +%s)
BAK="/tmp/ct.bak.${TS}"
NEW="/tmp/ct.new.${TS}"

crontab -l > "$BAK" 2>/dev/null || : > "$BAK"
echo "[staleness-crons] backed up live crontab -> $BAK ($(wc -l <"$BAK" | tr -d ' ') lines)"
cp "$BAK" "$NEW"

FEEDS_MARK='tools/feeds_selftest.py'
FEEDS_LINE='*/30 * * * * /usr/bin/python3 /Users/jo/Omega/tools/feeds_selftest.py >> /tmp/feeds_selftest_cron.log 2>&1; rc=$?; if [ "$rc" = "1" ]; then /usr/bin/osascript -e '"'"'display notification "FEEDS RED -- a LIVE feed is stale; run tools/feeds_selftest.py" with title "📡 LIVE FEED STALE" sound name "Basso"'"'"'; fi  # staleness-audit 2026-07-08: active notify (was session-start print only)'

if grep -qF "$FEEDS_MARK" "$NEW"; then
  echo "[staleness-crons] feeds_selftest cron already present -- skip"
else
  printf '# --- feeds staleness ACTIVE notify (staleness audit S-2026-07-07x, 2026-07-08) ---\n%s\n' "$FEEDS_LINE" >> "$NEW"
  echo "[staleness-crons] feeds_selftest 30min notify cron ADDED"
fi

# Wave selftest: append a RED notifier to the existing hourly wave line (keeps one cron slot).
WAVE_MARK='wave_companion_selftest RED notify'
if grep -qF "$WAVE_MARK" "$NEW"; then
  echo "[staleness-crons] wave selftest notify already present -- skip"
elif grep -q 'wave_companion_selftest\.py' "$NEW"; then
  # NB: the wave line carries a trailing shell comment ("# [wave-companion ...]") — an
  # appended notifier would land INSIDE the comment and never run. Insert it directly
  # after the selftest's log redirect instead (marker text keeps this idempotent).
  awk -v add=' || /usr/bin/osascript -e '"'"'display notification "WAVE-COMPANION SELF-TEST RED -- engine dead/stale/arm-broken; see /tmp/wave_companion_selftest.log" with title "🌊 WAVE SELFTEST FAILED" sound name "Basso"'"'"' ; : wave_companion_selftest RED notify staleness-audit-2026-07-08' '
    /wave_companion_selftest\.py/ && $0 !~ /^[[:space:]]*#/ {
      sub(/wave_companion_selftest\.py >> \/tmp\/wave_companion_selftest\.log 2>&1/, "&" add)
    }
    { print }
  ' "$NEW" > "${NEW}.2" && mv "${NEW}.2" "$NEW"
  echo "[staleness-crons] wave selftest RED notify APPENDED to hourly wave cron"
else
  echo "[staleness-crons] WARN: no active wave_companion_selftest cron line found -- nothing to append"
fi

if cmp -s "$BAK" "$NEW"; then
  echo "[staleness-crons] no changes -- crontab untouched"
else
  crontab "$NEW"
  echo "[staleness-crons] installed. diff summary:"
  diff "$BAK" "$NEW" | sed 's/^/  /' || true
  echo "[staleness-crons] restore with: crontab $BAK"
fi
