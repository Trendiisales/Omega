#!/bin/bash
# CANONICAL crypto-desk-trades reset (S-2026-07-16).
#
# Clears the desk last-15 crypto-trades feed COMPLETELY and DURABLY:
#   1. josgp1 SOURCE   chimera-direct:~/ChimeraCrypto/data/chimera_inbound.csv  (header-only)
#   2. josgp1 LEDGER   chimera-direct:~/ChimeraCrypto/data/companion_trades.json (empty)
#   3. omega  COPY     omega-new:C:/Omega/logs/trades/chimera_inbound.csv        (header-only)
# each with a timestamped .pre_reset_<TS> backup.
#
# WHY THIS EXISTS: chimera_inbound.csv is APPEND-ONLY on josgp1 and the relay
# (refresh_crypto_companion.sh, launchd every 120s) whole-file-overwrites the omega copy
# FROM the josgp1 source. Prior ad-hoc resets cleared only the omega COPY -> the josgp1
# SOURCE kept its rows -> the relay re-pushed them within 2 min ("it came back after the
# fix"). Clearing the SOURCE is the only durable reset. Use THIS script, never a one-box clear.
#
#   bash tools/gui/reset_crypto_desk_trades.sh
set -uo pipefail
TS="$(date '+%Y%m%d_%H%M%S')"
HDR='id,entry_ts,exit_ts,sym,strat,side,entry,exit,net_usd,reason'

echo "[reset $TS] 1/2 josgp1 SOURCE chimera_inbound.csv (backup)"
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

echo "[reset $TS] 2/2 omega COPY chimera_inbound.csv (backup)"
ssh omega-new "copy /Y C:\\Omega\\logs\\trades\\chimera_inbound.csv C:\\Omega\\logs\\trades\\chimera_inbound.csv.pre_reset_$TS >nul && echo $HDR> C:\\Omega\\logs\\trades\\chimera_inbound.csv && echo   OMEGA now: && type C:\\Omega\\logs\\trades\\chimera_inbound.csv"

echo "[reset $TS] done — relay keeps it clean; the DESK-TRADE GUARD drops any future negative companion clip."
