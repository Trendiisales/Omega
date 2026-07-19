#!/bin/bash
# Idempotent cron install for the Trade Sentinel (per feedback-crontab-edit-via-script:
# never inline sed/heredoc crontab edits — this script backs up, filters its own marker
# lines, re-adds them, and installs atomically. Restore: crontab /tmp/ct.bak.<ts>).
#
#   */15 (:02,:17,:32,:47)  trade_sentinel.py — incremental per-trade loss triage + win miner
#     (+ macOS notify). 15min chosen S-2026-07-20h: hourly missed the first 2 live gold closes
#     by 3min -> 57min advice lag; */5 would triple read-only ssh spawns on the 3GB omega-new
#     box for no advisory gain (sentinel is post-close, never a gate).
#   Sun 08:03   mine_losses.py     — weekly deep dimensional pattern mine (both systems)
#
# Usage:  bash tools/ml_loss_miner/install_sentinel_cron.sh          # install/refresh
#         bash tools/ml_loss_miner/install_sentinel_cron.sh remove   # uninstall
set -euo pipefail

REPO="/Users/jo/Omega"
PY="/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python"
MARK="# OMEGA-TRADE-SENTINEL"
TS=$(date +%Y%m%d_%H%M%S)
BAK="/tmp/ct.bak.${TS}"

crontab -l 2>/dev/null > "${BAK}" || true
echo "crontab backed up to ${BAK}"

NEW=$(grep -vF "${MARK}" "${BAK}" || true)

if [[ "${1:-}" != "remove" ]]; then
  NEW+=$'\n'"2,17,32,47 * * * * cd ${REPO} && ${PY} tools/ml_loss_miner/trade_sentinel.py --notify >> /tmp/trade_sentinel_cron.log 2>&1 ${MARK}"
  NEW+=$'\n'"3 8 * * 0 cd ${REPO} && ${PY} tools/ml_loss_miner/mine_losses.py --system both >> /tmp/trade_sentinel_cron.log 2>&1 ${MARK}"
fi

printf '%s\n' "${NEW}" | sed '/^$/N;/^\n$/D' | crontab -
echo "installed. current sentinel lines:"
crontab -l | grep -F "${MARK}" || echo "  (none — removed)"
