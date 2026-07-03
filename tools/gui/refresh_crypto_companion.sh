#!/bin/bash
# Cross-box push: crypto companion live-state json  josgp1 (chimera-direct) -> omega-vps.
#
# The native UpJumpCompanionEngine emit (src/main.cpp emit_companion_state) writes
#   chimera-direct:/home/jo/ChimeraCrypto/data/crypto_companion_state.json
# once at startup + on every H1 bar close. The Omega desk :7779 endpoint
#   GET /api/crypto_companion  ->  loadFile("crypto_companion_state.json")
# reads it from  C:\Omega\crypto_companion_state.json  on the VPS. josgp1 has no
# direct route to the VPS, so the Mac (ssh to both) bridges the file across.
#
# Idempotent, cheap (~1-2s), safe to run on a short interval. The panel polls
# every 15s and the source changes ~hourly, so a stale hop is harmless.
#
#   bash refresh_crypto_companion.sh
#   schedule: com.omega.crypto-companion-push.plist (launchd, every 120s)
set -uo pipefail
TS="$(date '+%Y-%m-%d %H:%M:%S')"
SRC="chimera-direct:/home/jo/ChimeraCrypto/data/crypto_companion_state.json"
TMP="/tmp/crypto_companion_state.json"
DST="omega-vps:C:/Omega/crypto_companion_state.json"

# 1. pull from the crypto box
if ! scp -q "$SRC" "$TMP" 2>/dev/null; then
  echo "[$TS] pull FAILED ($SRC) — keeping prior VPS copy"; exit 0
fi

# 2. sanity: non-empty + looks like our schema before pushing
if [ ! -s "$TMP" ] || ! grep -q '"legs"' "$TMP"; then
  echo "[$TS] pulled file missing/invalid — not pushing"; exit 0
fi

# 3. push to the VPS
if scp -q "$TMP" "$DST" 2>/dev/null; then
  echo "[$TS] pushed $(wc -c < "$TMP") bytes -> VPS"
else
  echo "[$TS] push FAILED -> VPS (will retry next interval)"
fi
