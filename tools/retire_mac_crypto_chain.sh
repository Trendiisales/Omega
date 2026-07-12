#!/bin/bash
# retire_mac_crypto_chain.sh — S-2026-07-13 operator: "we should only have 1 crypto system".
# The 2026-07-12 consolidation retired the Mac wave/ladder books but the shadow_refresh
# intraday chain SURVIVED (com.chimera.book-refresh launchd + book-watchdog keep-alive +
# binary_freshness cron) and was still trading (state.json Jul-12 18:03, Macd/Kelt stops
# in LAST-15). This script stands the WHOLE Mac crypto chain down, idempotently:
#   [1] launchd: bootout + archive com.chimera.book-refresh / com.chimera.book-watchdog
#       (com.omega.crypto-companion-push is KEPT — that is the josgp1->omega-new display
#        relay, plumbing for the ONE system, not a trading book)
#   [2] crontab: remove the binary_freshness guard line (guarded the retired Mac binaries)
#       via full backup + filter, never inline sed (feedback-crontab-edit-via-script)
#   [3] leave all CSV history in place — the desk ALL-TIME fold keeps the closed history;
#       the book simply never writes a new row.
set -uo pipefail
TS=$(date +%s)
LA=~/Library/LaunchAgents
echo "[1] launchd stand-down"
for J in com.chimera.book-refresh com.chimera.book-watchdog; do
  launchctl bootout "gui/$(id -u)/$J" 2>/dev/null && echo "  bootout $J" || echo "  $J not loaded"
  if [ -f "$LA/$J.plist" ]; then
    mv "$LA/$J.plist" "$LA/$J.plist.retired-$TS" && echo "  archived $J.plist -> .retired-$TS"
  fi
done
echo "[2] crontab: drop binary_freshness (backup first)"
crontab -l > "/tmp/ct.bak.$TS" || exit 1
echo "  backup /tmp/ct.bak.$TS ($(wc -l < /tmp/ct.bak.$TS) lines)"
grep -v "binary_freshness" "/tmp/ct.bak.$TS" > "/tmp/ct.new.$TS"
if [ -s "/tmp/ct.new.$TS" ]; then
  crontab "/tmp/ct.new.$TS" && echo "  crontab installed ($(wc -l < /tmp/ct.new.$TS) lines)"
else
  echo "  ABORT: filtered crontab empty — restore from /tmp/ct.bak.$TS"; exit 1
fi
echo "[3] kill any live shadow_refresh process"
pkill -f "/Users/jo/Crypto/build/shadow_refresh" 2>/dev/null && echo "  killed" || echo "  none running"
echo "DONE — Mac crypto chain retired. ONE system: josgp1 (relay kept)."
