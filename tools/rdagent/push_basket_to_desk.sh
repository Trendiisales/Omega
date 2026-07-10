#!/usr/bin/env bash
# push_basket_to_desk.sh (S-2026-07-11) — push the rdagent stock basket (paper book + model
# ranking) from the Mac to the VPS Omega desk, so the META-type stock picks show ON THE DESK
# instead of being stranded on the Mac-only :7799 page. Mirrors refresh_crypto_companion.sh.
# The desk serves them via /api/rdagent_basket + the STOCK BASKET panel.
set -uo pipefail
TS="$(date '+%Y-%m-%d %H:%M:%S')"
BOOK="/tmp/rda_basket.json"                                   # paper book (equity/pnl/orders)
RANK="$HOME/Omega/data/rdagent/latest.json"                  # model ranking (23 names)
DSTDIR="omega-new:C:/Omega/data/rdagent"
ok=0
[ -s "$BOOK" ] && scp -q "$BOOK" "$DSTDIR/rda_basket.json" 2>/dev/null && ok=$((ok+1))
[ -s "$RANK" ] && scp -q "$RANK" "$DSTDIR/latest.json"     2>/dev/null && ok=$((ok+1))
echo "[$TS] rdagent basket -> desk: pushed $ok/2 files"
[ "$ok" -ge 1 ] || exit 1
