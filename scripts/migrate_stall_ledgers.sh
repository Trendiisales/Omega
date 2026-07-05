#!/usr/bin/env bash
# migrate_stall_ledgers.sh — one-shot migration of the Mac stall_accountant per-book
# companion_closed.csv BANK ledgers -> the VPS, so the native C++ StallCompanion continues
# each book's realized bank instead of resetting to $0 at cutover. Idempotent-ish: it
# OVERWRITES the VPS ledger with the Mac one, so run it ONCE, at cutover, BEFORE the C++
# has banked anything new on the VPS (weekend/flat = nothing new banked yet).
#
# Mapping: Mac ~/stall-accountant/<book>/companion_closed.csv  ->  VPS C:\Omega\stall\<book>\
#          the shared "main" book lives at the Mac ROOT (~/stall-accountant/companion_closed.csv)
#          -> VPS C:\Omega\stall\main\.  Only the 25 Omega gold/index books are migrated
#          (crypto-intraday books stay on the Mac, cohort 4). Books with no ledger yet are skipped.
#
# VPS ssh form MUST start literally `ssh omega-vps` (feedback-vps-ssh-command-form).
set -uo pipefail
SRC="$HOME/stall-accountant"

BOOKS=(spx_turtle_clip dj30_turtle_clip spx_turtle_clip_gv40 spx_turtle_clip_gv60 spx_turtle_clip_gv80
       mgc_fastdon_clip gold_panic_bounce_clip xau_tf4h_clip xau_tf1h_clip xau_tfd1_clip
       xau_tfd1_usd xau_tf4h_usd_a xau_tf4h_usd_b xau_tf1h_usd_a xau_tf1h_usd_b
       xau_tf4h_aggr xau_tf1h_aggr xau_tf4h_aggr_b xau_tf1h_aggr_b
       xau_tf2h_usd_a xau_tf2h_usd_b xau_tfd1_usd_b gvb_m30_usd_a gvb_m30_usd_b)

migrate_one() {   # $1 = local ledger path, $2 = VPS book dir name
  local lf="$1" name="$2"
  [ -s "$lf" ] || { echo "  skip $name (no ledger yet)"; return; }
  ssh omega-vps "if not exist C:\\Omega\\stall\\$name mkdir C:\\Omega\\stall\\$name" >/dev/null 2>&1 || true
  if scp -q "$lf" "omega-vps:C:/Omega/stall/$name/companion_closed.csv"; then
    echo "  migrated $name ($(wc -l <"$lf" | tr -d ' ') rows)"
  else
    echo "  ** FAILED $name **"
  fi
}

echo "[migrate] main (root ledger) ->  C:\\Omega\\stall\\main"
migrate_one "$SRC/companion_closed.csv" "main"
echo "[migrate] per-book ledgers:"
for b in "${BOOKS[@]}"; do migrate_one "$SRC/$b/companion_closed.csv" "$b"; done
echo "[migrate] done. The C++ StallCompanion appends to these files -> bank continues."
