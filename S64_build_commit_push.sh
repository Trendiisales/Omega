#!/bin/bash
# S63 + S64 combined commit + push -- 2026-05-13
# ============================================================================
# Lands two clean commits on origin/main:
#
#   1) S63 -- VWR-pattern LOSS_CUT/BE_RATCHET plumbing for 7 non-FX engines
#             (PDHL, RSIReversal, NoiseBandMomentum, XauusdFvg, XauThreeBar30m,
#              IndexMacroCrash, IndexFlow base) + audit + verify harness.
#             Backtest-verified neutral on XAU October 2025 (no regression).
#
#   2) S64 -- FX-engine circuit-breaker layer for the 5 FX London/Asian/Sydney
#             Open engines: immediate LOSS_CUT_PCT cold-loss cut + consec-loss
#             daily lockout. Closes the 07:02-then-08:55 GBP repeat-SL hole.
#
# Each commit is gated by a clean Mac canary build (cmake --build), so the
# script aborts if anything breaks the OmegaBacktest target.
#
# Usage:  bash ~/omega_repo/S64_build_commit_push.sh
# ============================================================================

set -e
cd "$HOME/omega_repo"

# ---------------------------------------------------------------------------
# 0. Stale .git/index.lock cleanup (project handoff pattern).
# ---------------------------------------------------------------------------
if [ -f .git/index.lock ]; then
  LOCK_SIZE=$(stat -f%z .git/index.lock 2>/dev/null || stat -c%s .git/index.lock 2>/dev/null || echo 0)
  if [ "$LOCK_SIZE" = "0" ]; then
    rm -f .git/index.lock
  fi
fi

# ---------------------------------------------------------------------------
# 1. Pre-flight: move/rename the colliding handoff doc from outputs/ to
#    docs/handoffs/ before staging.
# ---------------------------------------------------------------------------
if [ -f outputs/SESSION_HANDOFF_2026-05-13b.md ] && \
   [ ! -f docs/handoffs/SESSION_HANDOFF_2026-05-13g.md ]; then
  echo "=== [S63/S64] moving outputs/SESSION_HANDOFF_2026-05-13b.md -> docs/handoffs/SESSION_HANDOFF_2026-05-13g.md ==="
  mv outputs/SESSION_HANDOFF_2026-05-13b.md \
     docs/handoffs/SESSION_HANDOFF_2026-05-13g.md
fi

echo "=== [S63/S64] git status BEFORE any commit ==="
git status --short

# ---------------------------------------------------------------------------
# 2. Mac canary build (authoritative).
# ---------------------------------------------------------------------------
echo "=== [S63/S64] cmake --build build --target OmegaBacktest -j ==="
cmake --build build --target OmegaBacktest -j

# ---------------------------------------------------------------------------
# 3. COMMIT 1 -- S63 plumbing (7 non-FX engines + verify harness + docs).
# ---------------------------------------------------------------------------
S63_FILES=(
  "include/CrossAssetEngines.hpp"
  "include/IndexFlowEngine.hpp"
  "include/PDHLReversionEngine.hpp"
  "include/RSIReversalEngine.hpp"
  "include/XauThreeBar30mEngine.hpp"
  "include/XauusdFvgEngine.hpp"
  "S63_build_verify_compare.sh"
  "docs/handoffs/SESSION_HANDOFF_2026-05-13a.md"
  "docs/handoffs/SESSION_HANDOFF_2026-05-13g.md"
)

echo "=== [S63] staging plumbing files ==="
# S63 patches to CrossAssetEngines + IndexFlowEngine also overlap with S64 (FX
# engines live in different files), so it's safe to stage these S63 files
# together. The S64-only FX header changes go to the second commit below.
for f in "${S63_FILES[@]}"; do
  if [ -f "$f" ]; then
    git add "$f"
  fi
done

echo "=== [S63] commit ==="
git commit -m "S63: VWR-pattern LOSS_CUT + BE_RATCHET plumbing for 7 non-FX engines + audit

Adds the canonical VWAPReversionEngine in-flight protection (LOSS_CUT
+ BE_RATCHET, originally added in part-H) to seven additional engines
that share the mean-reversion + fixed-timeout profile, per
outputs/IN_FLIGHT_PROTECTION_AUDIT_2026-05-13.md (which is gitignored).

Tier 1 (full pattern):
  PDHLReversionEngine
  NoiseBandMomentumEngine  (in CrossAssetEngines.hpp)

Tier 2 -- full pattern:
  XauusdFvgEngine
  XauThreeBar30mEngine
  IndexMacroCrashEngine    (in IndexFlowEngine.hpp)

Tier 2 -- LOSS_CUT only (existing BE_LOCK or staircase covers giveback):
  RSIReversalEngine             (BE_LOCK at ATR_MULT)
  IndexFlowEngine IdxOpenPosition (staircase BE@1xATR / 0.5x@2x / 0.25x@4x)

All defaults runtime-mutable via engine_init.hpp. Set any _PCT = 0.0 to
disable that phase. The Tier 2 LOSS_CUT-only engines have BE_ARM_PCT
implicit at 0.0 by not adding the BE_RATCHET branch.

A/B verification (XAUUSD October 2025, 9.3M ticks): PDHLReversion (the
only patched engine that fired on this XAU-only tape) dpnl = 0.00 with
144 LOSS_CUT + 326 BE_CUT exits replacing prior L2_FLIP / FORCE_CLOSE
exits at neutral pnl. Defaults are conservative -- no regression, no
clear improvement yet. Per-engine tuning is follow-up via engine_init
overrides; the other 6 engines need a non-XAU tape to validate.

Docs:
  S63_build_verify_compare.sh -- A/B harness (auto-detects 5-col Duka)
  docs/handoffs/SESSION_HANDOFF_2026-05-13a.md  -- part-F status refresh
  docs/handoffs/SESSION_HANDOFF_2026-05-13g.md  -- S62 handoff (renamed
                                                  from outputs/SESSION_HANDOFF_2026-05-13b.md)

No core code modified (on_tick.hpp / trade_lifecycle.hpp /
OmegaCostGuard.hpp / engine_init.hpp / OmegaTradeLedger.hpp untouched).
Outputs (audit doc, diagnosis doc, A/B summary) live in the gitignored
outputs/ folder and are NOT shipped with this commit."

echo "=== [S63] commit landed:"
git log --oneline -1

# ---------------------------------------------------------------------------
# 4. COMMIT 2 -- S64 FX circuit breakers (5 FX engines).
# ---------------------------------------------------------------------------
S64_FILES=(
  "include/GbpusdLondonOpenEngine.hpp"
  "include/EurusdLondonOpenEngine.hpp"
  "include/AudusdSydneyOpenEngine.hpp"
  "include/NzdusdAsianOpenEngine.hpp"
  "include/UsdjpyAsianOpenEngine.hpp"
  "S64_build_commit_push.sh"
)

echo "=== [S64] staging FX circuit-breaker files ==="
for f in "${S64_FILES[@]}"; do
  if [ -f "$f" ]; then
    git add "$f"
  fi
done

echo "=== [S64] commit ==="
git commit -m "S64: FX-engine circuit breakers -- immediate LOSS_CUT + consec-loss daily lockout

Triggered by the 2026-05-13 GBP repeat-SL pattern:
  07:02:41 GBPUSD LONG -> SL_HIT (-\$17.20 net)
  08:55:24 GBPUSD LONG -> SL_HIT (-\$17.00 net)
Same direction, ~2h apart, both inside the 07:00-10:00 London window.
The existing per-trade gates (cost, ATR-expansion, same-level block,
news, session, ABS_EXPANSION_FLOOR) are all setup-quality filters; none
of them stop the engine from re-firing after a recent loss.

This patch adds two missing layers to all 5 FX-style compression
engines (GBP/EUR/AUD/NZD/JPY London/Sydney/Asian Open):

1. LOSS_CUT_PCT (default 0.03% of entry)
   Immediate cold-loss cut in manage(). At GBPUSD 1.35 entry this is
   ~4 pips, tighter than the range-based SL_dist (10-15 pips). Cuts
   trades the moment adverse excursion crosses the threshold, BEFORE
   the range-based SL fires. Matches the operator's stated rule:
   'cover costs and cut losing trades immediately'. Set 0.0 to disable.

2. CONSEC_LOSS circuit breaker (CONSEC_LOSS_THRESH=2, BLOCK_S=14400)
   After 2 consecutive losing trades (SL_HIT or LOSS_CUT) in the current
   UTC day, blocks ALL arming for 4 hours -- covers the rest of a
   typical London-open window. Counter resets on a winning trade
   (TRAIL_HIT or TP_HIT) OR a UTC day roll. Persisted to disk
   alongside the post-trade-block state (v2 format extends the existing
   gbpusd_london_open_post_trade_block.csv with consec_loss_count,
   consec_loss_day_utc, consec_loss_block_until_s), so a service restart
   inside an active block does NOT silently bypass it -- closes the same
   class of hole as S62 closed for the same-level block.

Today's pattern under S64:
  07:02 SL -> count=1, no block
  08:55 SL -> count=2, breaker trips -> engine locked out until ~12:55 UTC
  Subsequent SLs prevented for 4h (or until the next UTC day).
LOSS_CUT also tightens each individual SL: worst-case damage drops from
2x ~\$17 to 2x ~\$5.30 (~\$4 LOSS_CUT + \$1.30 cost), capped by the
breaker.

All fields runtime-mutable; engine_init.hpp can override per instance
(e.g. CONSEC_LOSS_THRESH=1 for stricter 'one strike' policy).

Five engine headers, parallel patch:
  include/GbpusdLondonOpenEngine.hpp
  include/EurusdLondonOpenEngine.hpp
  include/AudusdSydneyOpenEngine.hpp
  include/NzdusdAsianOpenEngine.hpp
  include/UsdjpyAsianOpenEngine.hpp

USDJPY uses %.3f price format (0.01 pip scale); 0.03% of 154 ~= 4.6 pips
absolute, same percentage as the other four FX engines.

No core code modified. Builds clean on Mac canary."

echo "=== [S64] commit landed:"
git log --oneline -1

# ---------------------------------------------------------------------------
# 5. Push both commits.
# ---------------------------------------------------------------------------
echo "=== [S63+S64] git push origin main ==="
git push origin main

echo
echo "=== [S63+S64] verify ==="
git log --oneline -3

# ---------------------------------------------------------------------------
# 6. Self-delete.
# ---------------------------------------------------------------------------
rm -f "$0"
echo "=== [S63+S64] DONE -- both commits on origin/main ==="
echo "    Next step: VPS deploy to push the live trading binary."
