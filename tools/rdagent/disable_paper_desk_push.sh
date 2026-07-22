#!/usr/bin/env bash
# disable_paper_desk_push.sh (S-2026-07-23g, operator hard order: "paper is paper and it
# is NOT in a live setting") — idempotently comment out the Mac cron that pushes the
# rdagent PAPER basket book onto the LIVE VPS desk. The Mac-side hourly paper compute
# (OMEGA-RDA-BASKET-HOURLY) is left running: it is the research-side forward record the
# operator needs as evidence for a possible certified LIVE conversion; it no longer
# reaches any live display.
# Crontab edited per feedback-crontab-edit-via-script: backup first, restore from
# /tmp/ct.bak.<ts> if anything looks wrong.
set -euo pipefail
TS="$(date +%s)"
BAK="/tmp/ct.bak.${TS}"
crontab -l > "$BAK"
echo "[paper-purge] crontab backed up to $BAK"
if grep -q '^[^#].*OMEGA-RDAGENT-BASKET-PUSH' "$BAK"; then
  sed 's|^\([^#].*OMEGA-RDAGENT-BASKET-PUSH.*\)$|# [PAPER-PURGE S-2026-07-23g operator order: no paper on live desk] \1|' "$BAK" | crontab -
  echo "[paper-purge] OMEGA-RDAGENT-BASKET-PUSH disabled"
else
  echo "[paper-purge] already disabled (idempotent no-op)"
fi
crontab -l | grep 'BASKET-PUSH' || true
