#!/bin/bash
# Cross-box push: crypto companion live-state json + chimera closed-trades csv
#   josgp1 (chimera-direct) -> omega-new (live box; was omega-vps=retired, S-2026-07-10).
#
# HOP 1 (S-2026-07-12) — CHIMERA→OMEGA DESK closed trades:
#   chimera src/main.cpp export_desk_trade appends every CLOSED shadow trade
#   (slot engines TSMOM/ICHI/BOLL, UPJUMP parents, companion clips, XSec/XSec2
#   rebalance legs) to
#     chimera-direct:/home/jo/ChimeraCrypto/data/chimera_inbound.csv
#   relayed to omega-new:C:/Omega/logs/trades/chimera_inbound.csv where the
#   :7779 /api/crypto_trades endpoint merges it into the LAST-15-TRADES panel
#   (book="chimera"). DISPLAY-ONLY: never ingested into the ALL-TIME PnL fold
#   (CryptoLedgerInbound reads crypto_inbound.csv only).
#
# HOP 2 — companion live-state json (original job):
#   The native UpJumpCompanionEngine emit (src/main.cpp emit_companion_state)
#   writes chimera-direct:/home/jo/ChimeraCrypto/data/crypto_companion_state.json
#   once at startup + on every H1 bar close. The Omega desk :7779 endpoint
#   GET /api/crypto_companion -> loadFile("crypto_companion_state.json")
#   reads it from C:\Omega\crypto_companion_state.json on the VPS.
#
# josgp1 has no direct route to the VPS, so the Mac (ssh to both) bridges both
# files across. Idempotent, cheap (~2-4s), safe on a short interval. The panels
# poll every 5-15s; sources change on bar closes, so a stale hop is harmless.
#
#   bash refresh_crypto_companion.sh
#   schedule: com.omega.crypto-companion-push.plist (launchd, every 120s)
set -uo pipefail
TS="$(date '+%Y-%m-%d %H:%M:%S')"

# ── HOP 1: chimera closed shadow trades (runs FIRST — the companion hop's
#           early-exits below must never starve the trades relay) ────────────
CH_SRC="chimera-direct:/home/jo/ChimeraCrypto/data/chimera_inbound.csv"
CH_TMP="/tmp/chimera_inbound.csv"
CH_DST="omega-new:C:/Omega/logs/trades/chimera_inbound.csv"
if scp -q "$CH_SRC" "$CH_TMP" 2>/dev/null; then
  if [ -s "$CH_TMP" ] && head -1 "$CH_TMP" | grep -q '^id,'; then
    if scp -q "$CH_TMP" "$CH_DST" 2>/dev/null; then
      echo "[$TS] chimera trades: pushed $(wc -l < "$CH_TMP" | tr -d ' ') lines -> VPS"
    else
      echo "[$TS] chimera trades: push FAILED -> VPS (will retry next interval)"
    fi
  else
    echo "[$TS] chimera trades: pulled file empty/invalid — not pushing"
  fi
else
  echo "[$TS] chimera trades: no source file yet (no closes since exporter shipped) — skipped"
fi

# ── HOP 2: companion live-state json (original job, unchanged) ───────────────
SRC="chimera-direct:/home/jo/ChimeraCrypto/data/crypto_companion_state.json"
TMP="/tmp/crypto_companion_state.json"
DST="omega-new:C:/Omega/crypto_companion_state.json"

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
