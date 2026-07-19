#!/bin/bash
# scripts/mimic_drawdown_audit.sh
# ---------------------------------------------------------------------
# STANDING GUARD — every ACTIVE mimic/companion book must carry a DOCUMENTED,
# backtested DRAWDOWN-CANCEL verdict. Operator rule S-2026-07-11 ("i want to know
# why this is not possible on ALL our mimic engines they should ALL be operating on
# exactly the same principle enforce it").
#
# Why a drawdown-cancel is MANDATORY on a mimic (and not on a real engine):
#   A mimic/companion runs its OWN book alongside the real engine and NEVER touches,
#   moves, or closes the real position (CompanionDominanceError / feedback-companion-
#   independent-engine). Because cutting a mimic leg has ZERO effect on the real trade,
#   an aggressive cold-loss cut is FREE protection — the exact opposite of a real
#   trend engine, where the 2026-06-17 sweep proved tightening HURTS net. So every
#   mimic gets the lever; the only question the backtest answers is WHERE to set it.
#
# The reference lever (bigcap_mimic_lc_sweep.py, S-2026-07-11): drawdown-cancel a leg
# whose price falls lc% below its live cost-base. On DAILY stock data lc is near-inert
# (losses gap-through overnight, worst -33%->-28%) but free (net flat/up) -> lc=15 ref.
# On finer-bar mimics (crypto intraday, FX H1) the same cut bites harder. Each mimic
# must BACKTEST its own lc and RECORD the verdict — a "near-inert on daily bars, lc=15
# floor (ref bigcap_mimic_lc_sweep)" verdict is valid; SKIPPING the backtest is not.
#
# Enforcement:
#   * A mimic header satisfies the guard if it contains the tag
#       DRAWDOWN-CANCEL:
#     anywhere (a comment stating the backtested lc verdict).
#   * RETIRED books (the BE-floor family, enabled=false, hands-off) are grandfathered
#     via scripts/mimic_drawdown_legacy.txt — reported as backfill-owed, do NOT fail.
#   * Any ACTIVE mimic that is NEITHER annotated NOR legacy FAILS the build.
#
# Scanned headers: include/*Companion.hpp + include/*Mimic*.hpp + include/*Ladder*.hpp
#   (the mimic/companion/ladder naming conventions). S-2026-07-14 (latent-class sweep
#   item 14c): *Ladder*.hpp added -- every ladder book to date IS a mimic/companion
#   (GoldTrendMimicLadder, FxMimicLadderCompanion, StockDayMoverLadderCompanion), but a
#   future header named e.g. FooLadder.hpp alone would have dodged the gate entirely.
#   Files matching several globs are deduped. A new mimic idiom -> add its glob here.
#
# Run before any commit touching a mimic header (wired into the Mac canary):
#   bash scripts/mimic_drawdown_audit.sh
# Exit 0 = clear; exit 1 = an active mimic is missing its drawdown-cancel verdict.
# ---------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.." || exit 2

TAG='DRAWDOWN-CANCEL:'
LEGACY_FILE='scripts/mimic_drawdown_legacy.txt'

is_legacy(){ [ -f "$LEGACY_FILE" ] && grep -qxF "$1" "$LEGACY_FILE"; }

fails=0; warns=0; ok=0
# sort -u: a file can match more than one glob (e.g. GoldTrendMimicLadder.hpp hits both
# *Mimic*.hpp and *Ladder*.hpp) -- dedupe so counts stay honest.
for h in $(printf '%s\n' include/*Companion.hpp include/*Mimic*.hpp include/*Ladder*.hpp | sort -u); do
  [ -f "$h" ] || continue
  base="$(basename "$h" .hpp)"
  if grep -q "$TAG" "$h"; then ok=$((ok+1)); continue; fi
  if is_legacy "$base"; then
    echo "NOTE: $base is retired (BE-floor family) — grandfathered, backfill owed."
    warns=$((warns+1)); continue
  fi
  echo "VIOLATION: $h is an ACTIVE mimic but has no '$TAG' verdict and is not retired."
  fails=$((fails+1))
done

echo "--- mimic drawdown-cancel audit: $ok annotated, $warns legacy(retired), $fails active-violation(s) ---"
if [ "$fails" -gt 0 ]; then
  echo "FAIL: an ACTIVE mimic ships without a backtested drawdown-cancel verdict."
  echo "      A mimic never touches the real trade, so a cold cut is FREE protection —"
  echo "      backtest the lc, then add a header comment, e.g.:"
  echo "        // DRAWDOWN-CANCEL: lc=15% of leg cost-base; near-inert on daily bars (gap-through,"
  echo "        //   worst -33->-28%) but free (net flat/up, all-6+2x pass). ref bigcap_mimic_lc_sweep (S-2026-07-11)"
  exit 1
fi
echo "PASS: every active mimic carries a documented drawdown-cancel verdict."
[ "$warns" -gt 0 ] && echo "NOTE: $warns retired mimic(s) grandfathered (hands-off, no backfill required)."
exit 0
