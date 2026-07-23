#!/bin/bash
# CANONICAL crypto-desk-trades reset (S-2026-07-16).
#
# Clears the desk last-15 crypto-trades feed COMPLETELY and DURABLY:
#   1. josgp1 SOURCE   chimera-direct:~/ChimeraCrypto/data/chimera_inbound.csv       (header-only)
#   2. omega  COPY     omega-new:C:/Omega/logs/trades/chimera_inbound.csv            (header-only)
#   3. josgp1 LEDGER   chimera-direct:~/ChimeraCrypto/data/live_trades.csv           (header-only)
#      + regen         chimera-direct:~/ChimeraCrypto/data/live_realized.json        (empty trades)
#   4. omega  COPY     omega-new:C:/Omega/live_realized.json                         (empty trades)
# each with a timestamped .pre_reset_<TS> backup.
#
# WHY THIS EXISTS: two independent append feeds back the desk "LAST 15 TRADES":
#   * chimera_inbound.csv  -> /api/crypto_trades   (companion CLIP rows, book=chimera)
#   * live_trades.csv      -> live_realized.json   -> /api/crypto_live_pnl  (the "MIMIC-LIVE
#                             real Binance cash" rows + the CRYPTO ALL-TIME fold, _clivetot)
# Both are APPEND-ONLY on josgp1; the relay (refresh_crypto_companion.sh, launchd every 120s)
# whole-file-overwrites BOTH omega copies FROM the josgp1 sources. Prior ad-hoc resets cleared
# only the omega COPY -> the josgp1 SOURCE kept its rows -> the relay re-pushed them within 2 min
# ("it came back after the fix").
# S-2026-07-23 ROOT-CAUSE: the reset ONLY cleared chimera_inbound.csv, but the 174 stuck desk rows
# lived in live_trades.csv (133 BACKFILL-XCHG + 41 paper KILL/clip rows, ZERO orderIds -> mislabeled
# "real Binance cash", folding a fake -$14.78 into CRYPTO ALL-TIME). live_realized.json is REGENERATED
# AT BOOT from live_trades.csv, so clearing the json alone is futile; the CSV is the source of record.
# ENGINE-MEMORY CAVEAT: the running binary holds realized_ in memory and rewrites live_realized.json on
# every book_close_/boot -> clearing the files WITHOUT a restart lets the in-memory 174 rewrite. A
# `sudo systemctl restart chimera` is REQUIRED for the ledger clear to hold (load_realized_ reloads the
# empty CSV). Clearing the SOURCE (+ restart) is the only durable reset. Use THIS script, never a one-box clear.
# BROKER-EVIDENCE RULE (feedback-real-trade-broker-evidence): only rows backed by a real Binance orderId
# belong in these ledgers. Paper/backfill/no-orderId rows are display contamination -> purge them here.
#
#   bash tools/gui/reset_crypto_desk_trades.sh
set -uo pipefail
TS="$(date '+%Y%m%d_%H%M%S')"
HDR='id,entry_ts,exit_ts,sym,strat,side,entry,exit,net_usd,reason'

echo "[reset $TS] 1/4 josgp1 SOURCE chimera_inbound.csv (backup)"
# S-2026-07-20f: companion_trades.json is NO LONGER cleared here — since the live-only
# rebuild it is LOAD-BEARING local state on josgp1 (the -J1 resurrection dedup
# jf_leg_already_closed reads it to stop a stale det-state resurrecting a closed
# jump_floor leg into a PHANTOM REAL BUY on the live mirror — the ETH-PJ7W24 zombie
# class). It is not a desk feed anymore (export_desk_trade is a no-op). Emptying it
# would re-open that incident class. chimera_inbound.csv alone is the desk relic.
ssh chimera-direct "cd ~/ChimeraCrypto/data && \
  cp -p chimera_inbound.csv chimera_inbound.csv.pre_reset_$TS 2>/dev/null || true; \
  head -1 chimera_inbound.csv > .cin.tmp 2>/dev/null && mv .cin.tmp chimera_inbound.csv || printf '%s\n' '$HDR' > chimera_inbound.csv; \
  echo '  SOURCE now:' && cat chimera_inbound.csv"

echo "[reset $TS] 2/4 omega COPY chimera_inbound.csv (backup)"
ssh omega-new "copy /Y C:\\Omega\\logs\\trades\\chimera_inbound.csv C:\\Omega\\logs\\trades\\chimera_inbound.csv.pre_reset_$TS >nul && echo $HDR> C:\\Omega\\logs\\trades\\chimera_inbound.csv && echo   OMEGA now: && type C:\\Omega\\logs\\trades\\chimera_inbound.csv"

echo "[reset $TS] 3/4 josgp1 LEDGER live_trades.csv (header-only) + live_realized.json (empty)"
LT_HDR='exit_ts_ms,entry_ts_ms,coin,tag,qty,entry_px,exit_px,realized_usd,reason'
ssh chimera-direct "cd ~/ChimeraCrypto/data && \
  cp -p live_trades.csv live_trades.csv.pre_reset_$TS 2>/dev/null || true; \
  cp -p live_realized.json live_realized.json.pre_reset_$TS 2>/dev/null || true; \
  head -1 live_trades.csv > .lt.tmp 2>/dev/null && mv .lt.tmp live_trades.csv || printf '%s\n' '$LT_HDR' > live_trades.csv; \
  printf '{\n  \"ts\": %s,\n  \"note\": \"reset $TS — paper/backfill purged; real orderId-backed fills only\",\n  \"total_usd\": 0.0,\n  \"trades\": []\n}\n' \"\$(date +%s)\" > live_realized.json; \
  echo '  LEDGER now:' && wc -l live_trades.csv && cat live_realized.json"

echo "[reset $TS] 4/4 omega COPY live_realized.json (empty)"
ssh omega-new "copy /Y C:\\Omega\\live_realized.json C:\\Omega\\live_realized.json.pre_reset_$TS >nul 2>&1 & echo {\"ts\":0,\"total_usd\":0,\"trades\":[]}> C:\\Omega\\live_realized.json && echo   OMEGA live_realized now: && type C:\\Omega\\live_realized.json"

echo ""
echo "[reset $TS] ⚠ RESTART REQUIRED: the running binary holds realized_ in memory and will rewrite"
echo "            live_realized.json from the in-memory 174 on the next book_close_/boot. Run:"
echo "              ssh chimera-direct 'sudo systemctl restart chimera'"
echo "            then confirm boot log shows '[LIVE-LEDGER] realized ledger loaded: 0 trade(s)'."
echo "[reset $TS] done — relay keeps it clean; DESK-TRADE GUARD drops future net<0 clips; only"
echo "            real orderId-backed fills (feedback-real-trade-broker-evidence) repopulate the ledgers."
