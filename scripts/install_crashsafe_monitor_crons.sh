#!/usr/bin/env bash
# scripts/install_crashsafe_monitor_crons.sh — idempotent Mac-cron installer that puts
# EVERY monitor/selftest behind tools/monitor_crashsafe_wrap.sh (S-2026-07-12 never-again
# audit, class B: a monitor crash masquerading as a system-RED).
#
# WHY: feedpath_selftest.py crashed under cron's /usr/bin/python3 3.9 (datetime.UTC) and
# the `|| osascript "path broken"` wrapper reported PRIMARY FEED PATH BROKEN — a monitor
# failure displayed as a system failure. Inverse class: staleness_scan's grep-for-RED made
# a monitor crash silent. The wrapper classifies MONITOR-CRASH vs SYSTEM-RED vs GREEN and
# notifies honestly ("🩺 MONITOR BROKEN" vs the monitor's own RED message).
#
# Per the crontab rule (feedback-crontab-edit-via-script): committed idempotent script +
# timestamped backup; NEVER inline sed/heredoc crontab edits. Restore: crontab /tmp/ct.bak.<ts>
#
# Interpreters are PINNED (/usr/bin/python3, /bin/bash, /usr/bin/osascript inside the
# wrapper) — no more bare `python3` drifting with cron's PATH.
set -euo pipefail
WRAP=/Users/jo/Omega/tools/monitor_crashsafe_wrap.sh
[ -x "$WRAP" ] || { echo "FATAL: $WRAP missing/not executable"; exit 1; }
TS=$(date +%s)
crontab -l > /tmp/ct.bak.$TS 2>/dev/null || true
echo "crontab backed up to /tmp/ct.bak.$TS"

# Each entry: unique-match token (removes the OLD line for that monitor) + the NEW line.
# Tokens are the monitor script filenames — each appears on exactly one live crontab line.
NEW_LINES=(
"*/15 * * * * $WRAP -n protection_selftest -l /tmp/protection_selftest.log -k 1 -r 1 -t \"🛡️ PROTECTION FAILED\" -m \"PROTECTION RED -- a protection is dead/shadow/missing, run protection_selftest.py\" -- /usr/bin/python3 /Users/jo/Omega/tools/protection_selftest.py # protection self-test [crashsafe-wrap S-2026-07-12]"
"*/30 * * * * $WRAP -n feeds_selftest -l /tmp/feeds_selftest_cron.log -k 1,2 -r 1 -t \"📡 LIVE FEED STALE\" -m \"FEEDS RED -- a LIVE feed is stale; run tools/feeds_selftest.py\" -- /usr/bin/python3 /Users/jo/Omega/tools/feeds_selftest.py # staleness-audit 2026-07-08 [crashsafe-wrap S-2026-07-12]"
"*/30 * * * * $WRAP -n feedpath_selftest -l /tmp/feedpath_selftest.log -k 2 -r 2 -t \"OMEGA FEEDPATH\" -m \"FEED-PATH SELFTEST RED — primary IBKR path broken (see /tmp/feedpath_selftest.log)\" -- /usr/bin/python3 /Users/jo/Omega/tools/feedpath_selftest.py # [crashsafe-wrap S-2026-07-12]"
"7 * * * * cd /Users/jo/Crypto/backtest && /Users/jo/Crypto/build/wave_companion >> /tmp/wave_companion.log 2>&1; $WRAP -n wave_companion_selftest -l /tmp/wave_companion_selftest.log -k 1 -r 1 -t \"🌊 WAVE SELFTEST FAILED\" -m \"WAVE-COMPANION SELF-TEST RED -- engine dead/stale/arm-broken; see /tmp/wave_companion_selftest.log\" -- /usr/bin/python3 /Users/jo/Crypto/backtest/wave_companion_selftest.py # [wave-companion S-2026-07-05, crashsafe-wrap S-2026-07-12]"
"13,43 * * * * $WRAP -n staleness_scan -l /tmp/omega_staleness_scan.log -k 1 -q \"^RESULT:\" -R \"RESULT: RED\" -t \"Omega Staleness RED\" -m \"LIVE feed stale — see /tmp/omega_staleness_scan.log\" -- /bin/bash /Users/jo/Omega/tools/staleness_scan.sh # OMEGA-STALENESS-SCAN [crashsafe-wrap S-2026-07-12]"
"*/15 * * * * $WRAP -n data_health_monitor -l /tmp/data_health.log -k 1 -- /usr/bin/python3 /Users/jo/Omega/tools/data_health_monitor.py --quiet # [crashsafe-wrap S-2026-07-12: crash-only notify; designed exit 1 = logged, self-explanatory]"
"*/15 * * * * $WRAP -n liveness_check -l /tmp/liveness_check.log -k \"\" -- /usr/bin/python3 /Users/jo/Omega/tools/liveness_check.py # [crashsafe-wrap S-2026-07-12: self-notifying monitor; wrapper adds crash visibility]"
"*/15 * * * * $WRAP -n omega_health_poll -l /tmp/omega_health_poll.log -k \"\" -- /bin/bash /Users/jo/Omega/tools/omega_health_poll.sh # [crashsafe-wrap S-2026-07-12: self-notifying; wrapper adds crash visibility]"
"*/30 * * * * cd /Users/jo/Crypto/backtest && $WRAP -n crypto_staleness_alarm -l /tmp/crypto_staleness_alarm.log -k \"\" -- /usr/bin/python3 /Users/jo/Crypto/backtest/staleness_alarm.py # [crashsafe-wrap S-2026-07-12: self-notifying; wrapper adds crash visibility]"
"0 */4 * * * $WRAP -n macro_gold_gate -l /tmp/macro_gold_gate.log -k \"\" -- /usr/bin/python3 /Users/jo/Omega/tools/macro_gold_gate.py # [macro-gold-gate refresher — install_macro_gold_gate_cron.sh] [crashsafe-wrap S-2026-07-12: crash visibility for the gate feeder]"
"9,39 * * * * $WRAP -n display_truth_selftest -l /tmp/display_truth_selftest.log -k 2 -r 2 -q \"^RESULT:\" -t \"🖥️ DESK SHOWS WRONG DATA\" -m \"DISPLAY-TRUTH RED — what the desk displays diverges from reality (roster/trades/symbols); see /tmp/display_truth_selftest.log\" -- /usr/bin/python3 /Users/jo/Omega/tools/display_truth_selftest.py # content-parity guard S-2026-07-12 [crashsafe-wrap]"
)
TOKENS=(
  "protection_selftest.py"
  "feeds_selftest.py"
  "feedpath_selftest.py"
  "wave_companion_selftest.py"
  "staleness_scan.sh"
  "data_health_monitor.py"
  "liveness_check.py"
  "omega_health_poll.sh"
  "staleness_alarm.py"
  "macro_gold_gate.py"
  "display_truth_selftest.py"
)

CUR=$(crontab -l 2>/dev/null || true)
for tok in "${TOKENS[@]}"; do
  # remove only ACTIVE lines for this monitor (keep commented history lines)
  CUR=$(printf '%s\n' "$CUR" | awk -v tok="$tok" '!($0 !~ /^[[:space:]]*#/ && index($0, tok))')
done
{
  printf '%s\n' "$CUR"
  printf '%s\n' "${NEW_LINES[@]}"
} | crontab -

echo "installed. Wrapped monitor lines now:"
crontab -l | grep -F "crashsafe-wrap" | sed 's/^/  /'
echo "verify a monitor manually, e.g.:"
echo "  $WRAP -n feedpath_selftest -l /tmp/feedpath_selftest.log -k 2 -r 2 -t T -m M -- /usr/bin/python3 /Users/jo/Omega/tools/feedpath_selftest.py"
