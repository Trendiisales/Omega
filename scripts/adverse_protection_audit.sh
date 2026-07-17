#!/bin/bash
# scripts/adverse_protection_audit.sh
# ---------------------------------------------------------------------
# STANDING GUARD — every NEW position-opening engine must carry a DOCUMENTED
# in-flight adverse-protection decision, per CLAUDE.md "Engine Adverse-Protection
# Mandate". This makes the NqMomentum regression impossible to repeat.
#
# Why a *documented decision* and not "must have a loss-cut":
#   The 2026-06-17 swing-protection sweep ([[omega-engine-swing-protection-sweep]])
#   proved tightening HURTS most engines — trend-followers run trail-only BY DESIGN.
#   So the rule is not "always add a loss-cut"; it is "you must have BACKTESTED the
#   adverse-protection step and recorded the verdict in the engine header." The
#   verdict may legitimately be a LOSS_CUT config, an ATR-BE ratchet, or
#   "trail-only — backtested, cut hurts net (ref)". What is forbidden is SKIPPING
#   the step, which is exactly what shipped NqMomentum with a -464pt worst trade.
#
# Enforcement:
#   * An engine header satisfies the guard if it contains the annotation tag
#       ADVERSE-PROTECTION:
#     anywhere (a comment stating the backtested verdict).
#   * Engines present at the time this guard was introduced are grandfathered via
#     scripts/adverse_protection_legacy.txt (basenames). They are reported as
#     "backfill owed" warnings but do NOT fail the build.
#   * Any entry-opening engine that is NEITHER annotated NOR in the legacy list
#     (i.e. a NEW engine) FAILS the build until its author records the verdict.
#
# Adding a new engine to the legacy file instead of annotating it is a visible,
# deliberate red flag in code review — do not do it to dodge the check.
#
# Run before any commit touching an engine header (wired into the Mac canary):
#   bash scripts/adverse_protection_audit.sh
# Exit 0 = clear; exit 1 = a new engine is missing its protection verdict.
# ---------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.." || exit 2

# S-2026-07-08: pattern widened -- 'pos_active_ = true' (member WITHOUT the dot,
# MgcFastDonchian30m) and multi-leg 'legs_.push_back' (CrossSectional) evaded the
# original dot-anchored regex, so a LIVE engine shipped un-audited. Any new
# position-representation idiom MUST be added here.
# S-2026-07-08c: + 'w_ = want' -- the weight-book idiom (GoldTsmomD1V2 rebalance
# allocation book holds a signed weight, no pos struct).
# S-2026-07-17u: '<anyref>.active = true' -- the local-ref idiom (auto& p = pos[ci];
# p.active = true) used by XauTrendFollow1h/D1 evaded the pos_-anchored form, so two
# LIVE engines passed with NO verdict (pre-live audit hole H4). Any identifier
# followed by '.active = true' now counts as an opener.
ENTRY_RE='[A-Za-z_][A-Za-z0-9_]*\.active *= *true|pos_active_? *= *true|pos_?\.open\(|\.open\(sig|legs_\.push_back|w_ *= *want'
TAG='ADVERSE-PROTECTION:'
LEGACY_FILE='scripts/adverse_protection_legacy.txt'

# bash-3.2 compatible (macOS default — no associative arrays)
is_legacy(){ [ -f "$LEGACY_FILE" ] && grep -qxF "$1" "$LEGACY_FILE"; }

# S-2026-07-14 (latent-class sweep item 7): glob widened -- multi-engine headers
# (*Engines.hpp: CrossAsset/HTFSwing/Sweepable/LatencyEdge) and *Stack.hpp
# (GoldEngineStack, LIVE g_gold_stack) hold inline position-opening classes but
# were NEVER scanned by the original include/*Engine.hpp glob. All five that trip
# ENTRY_RE are pre-gate legacy files, grandfathered in $LEGACY_FILE (backfill
# owed) -- NOT annotated, because the mandate forbids recording a verdict whose
# backtest was never run. Any NEW opener class added to these files still needs
# its own ADVERSE-PROTECTION: tag (one tag anywhere in the file satisfies the
# whole file -- a known granularity limit; prefer per-class tags when backfilling).
fails=0; warns=0; ok=0
for h in include/*Engine.hpp include/*Engines.hpp include/*Stack.hpp; do
  [ -f "$h" ] || continue
  grep -Eq "$ENTRY_RE" "$h" || continue          # not a position-opener
  base="$(basename "$h" .hpp)"
  if grep -q "$TAG" "$h"; then ok=$((ok+1)); continue; fi
  if is_legacy "$base"; then
    echo "LEGACY (backfill owed): $h — grandfathered, no ADVERSE-PROTECTION: verdict yet."
    warns=$((warns+1)); continue                 # grandfathered — backfill owed
  fi
  echo "VIOLATION: $h opens a position but has no '$TAG' verdict and is not grandfathered."
  fails=$((fails+1))
done

echo "--- adverse-protection audit: $ok annotated, $warns legacy(backfill-owed), $fails new-violation(s) ---"
if [ "$fails" -gt 0 ]; then
  echo "FAIL: a NEW directional engine shipped without a backtested adverse-protection verdict."
  echo "      Backtest the protection step, then add a header comment, e.g.:"
  echo "        // ADVERSE-PROTECTION: loss_cut_atr=2.5 + ATR-BE(1.0/0.3); sweep nq_momentum_faithful.cpp"
  echo "        //   caps worst trade, net preserved both-WF-halves. (S-YYYY-MM-DD)"
  echo "      A 'trail-only, cut hurts net (ref)' verdict is valid — but it must be RECORDED."
  exit 1
fi
echo "PASS: every new directional engine carries a documented adverse-protection verdict."
[ "$warns" -gt 0 ] && echo "NOTE: $warns legacy engine(s) still owe an ADVERSE-PROTECTION annotation (backfill debt)."
exit 0
