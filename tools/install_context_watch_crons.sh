#!/usr/bin/env bash
# install_context_watch_crons.sh -- idempotent cron install (operator rule: crontab
# edits ONLY via committed script + backup; never inline sed/heredoc pastes).
# Adds two Mac-side daily context watchers (S-2026-07-23a):
#   22:40 Mon-Fri  congress_trades_check.py    (Kadoa congress filings -> data/congress_bigcap.json)
#   22:45 Mon-Fri  engine_exceptional_watch.py (live-ledger exceptional-engine flag + mimic-x2 queue)
set -euo pipefail
TS=$(date +%s)
crontab -l > "/tmp/ct.bak.$TS" 2>/dev/null || true
( crontab -l 2>/dev/null | grep -v "congress_trades_check.py" | grep -v "engine_exceptional_watch.py"
  echo "40 22 * * 1-5 cd /Users/jo/Omega && /usr/bin/python3 tools/congress_trades_check.py >> /tmp/congress_check.log 2>&1"
  echo "45 22 * * 1-5 cd /Users/jo/Omega && /usr/bin/python3 tools/engine_exceptional_watch.py >> /tmp/exwatch.log 2>&1"
) | crontab -
echo "[install] done; backup at /tmp/ct.bak.$TS"
crontab -l | grep -E "congress_trades|exceptional_watch"
