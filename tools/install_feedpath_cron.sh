#!/usr/bin/env bash
# install_feedpath_cron.sh — idempotent Mac-cron installer for the feed-path
# selftest (S-2026-07-08c). Per the crontab rule: committed script + backup,
# never inline edits. 30-min cadence; macOS notification + log on RED.
set -euo pipefail
TS=$(date +%s)
crontab -l > /tmp/ct.bak.$TS 2>/dev/null || true
LINE='*/30 * * * * python3 /Users/jo/Omega/tools/feedpath_selftest.py >> /tmp/feedpath_selftest.log 2>&1 || osascript -e '"'"'display notification "FEED-PATH SELFTEST RED — primary IBKR path broken (see /tmp/feedpath_selftest.log)" with title "OMEGA FEEDPATH"'"'"''
( crontab -l 2>/dev/null | grep -v feedpath_selftest ; echo "$LINE" ) | crontab -
echo "installed (backup /tmp/ct.bak.$TS):"
crontab -l | grep feedpath
