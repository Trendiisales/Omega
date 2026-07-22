#!/bin/bash
# scripts/mimic_pnl_completeness_gate.sh
# ---------------------------------------------------------------------
# STANDING GUARD — every money book must reach the ALL-TIME headline. Operator rule
# S-2026-07-11 ("i have been looking at half the actual pnl because you are careless...
# what else is missing or not showing on pnl check everything").
#
# The failure mode this blocks (twice seen): a book gets a telemetry endpoint AND a
# producer, its state file fills with realized USD, but NOTHING folds it into the desk
# ALL-TIME total -> the operator is shown half the real P&L. BigCap2pct was exactly this
# (/api/bigcap2pct_companion existed, orphaned from updDayPnl until S-2026-07-11); rdagent
# and the corrected crypto book were earlier instances.
#
# Two invariants over tools/gui/omega_desk.html + src/gui/OmegaTelemetryServer.cpp:
#   (1) POLLER-EXISTS: every money book endpoint in the telemetry server
#       (/api/*_companion + *ladder* + rdagent_book) must be fetch()'d by a GUI poller.
#       An orphaned endpoint = a book nobody reads.
#   (2) TOTAL-FOLDED: every `window._*tot` total a poller assigns must be referenced
#       inside the updDayPnl() body. A total that is set but never named in updDayPnl
#       never reaches the ALL-TIME headline.
#
# Intentionally-not-polled exceptions (RETIRED BE-floor family — panels + pollers deleted
# S-2026-07-08c, endpoints still serve empty state for history; enabled=false, real-fill dead
# per feedback-befloor-family-retired-handsoff). Their fold terms are harmless dead-$0:
#   * /api/stockmover_companion — LADDER successor /api/stockladder_companion IS folded (_smtot).
#   * /api/gold_companion, /api/usoil_companion, /api/xag_companion — retired, no poller.
# (fx_companion + index_companion ARE still polled by pollFx/pollIndex -> not excluded.)
#
# Run in the Mac canary (pre-commit). Exit 0 = every money book folds; exit 1 = a gap.
# ---------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.." || exit 2
SRV=src/gui/OmegaTelemetryServer.cpp
GUI=tools/gui/omega_desk.html
fails=0

# Intentional non-poll exclusions (retired/culled dead books, empty state).
# S-2026-07-22e additions (operator: dead/culled books never displayed -> GUI pollers deleted):
#   * ^companion$ — StallCompanionRegistry master-disabled S-22c (stall-clip paper aggregate)
#   * fx_companion — FX BE-floor RETIRED S-07-07 (fold-only poller deleted)
#   * index_companion — index BE-floor CULLED S-07-09 (panel + poller deleted)
EXCLUDE_EP='stockmover_companion|gold_companion|usoil_companion|xag_companion|^companion$|fx_companion|index_companion'

# (1) POLLER-EXISTS -----------------------------------------------------
MONEY_EP=$(grep -oE 'GET /api/[a-z0-9_]+' "$SRV" | sed 's#GET /api/##' \
  | grep -E 'companion|ladder|rdagent_book' | grep -vE "$EXCLUDE_EP" | sort -u)
n_ep=0
for ep in $MONEY_EP; do
  n_ep=$((n_ep+1))
  if ! grep -q "fetch('/api/$ep')" "$GUI"; then
    echo "ORPHAN-ENDPOINT: /api/$ep serves a book total but NO GUI poller fetch()'s it."
    fails=$((fails+1))
  fi
done

# (2) TOTAL-FOLDED ------------------------------------------------------
# updDayPnl body: from the function head to the ALL-TIME tween.
UPD=$(awk '/function updDayPnl\(\)/{f=1} f{print} f&&/tweenNum\(.totpnl./{exit}' "$GUI")
n_tot=0
for g in $(grep -oE "window\._[a-z0-9]+tot *=" "$GUI" | grep -oE "window\._[a-z0-9]+tot" | sort -u); do
  n_tot=$((n_tot+1))
  name="._${g#window._}"
  if ! printf '%s\n' "$UPD" | grep -q "$name"; then
    echo "UNFOLDED-TOTAL: $g is assigned by a poller but never referenced in updDayPnl() -> its money never reaches ALL-TIME."
    fails=$((fails+1))
  fi
done

echo "--- pnl completeness: $n_ep money endpoint(s) checked, $n_tot total-global(s) checked, $fails gap(s) ---"
if [ "$fails" -gt 0 ]; then
  echo "FAIL: a money book does not reach the ALL-TIME headline. Wire its poller + fold term"
  echo "      (var xAll=safe(window._xtot); totT=...+xAll) in tools/gui/omega_desk.html, then regen the GUI header."
  exit 1
fi
echo "PASS: every money book endpoint has a poller and every total folds into ALL-TIME."
exit 0
