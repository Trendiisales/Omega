#!/bin/bash
# scripts/ungated_engine_audit.sh
# ---------------------------------------------------------------------
# STANDING GUARD — ungated-engine audit (CLAUDE.md "Standing Audit Checks" §1,
# promoted from an inline 2-idiom grep to this script, S-2026-07-14 latent-class
# sweep item 10, failure class: static-scan blindness).
#
# WHY: the CLAUDE.md inline grep matched only 2 opener idioms
# (`pos[_]?\.active *= *true|pos[_]?\.open\(sig`). Engines using other opener
# idioms had ZERO coverage: MgcFast/MgcSlow (`pos_active_ = true`),
# CrossSectionalIndex (`legs_.push_back`), GoldTsmomD1V2 (`w_ = want`).
# scripts/adverse_protection_audit.sh already maintains the canonical WIDE
# opener regex (ENTRY_RE, widened S-2026-07-08/-08c). This script DERIVES that
# regex from the adverse audit at runtime — derive-don't-copy, the same pattern
# persistence_audit.sh uses for dynamic register_source shapes — so the two
# audits can never drift apart. Zero-parse of the ENTRY_RE line = FAIL (exit 2).
#
# WHAT: every include/*.hpp (superset of the adverse audit's *Engine.hpp/
# *Engines.hpp/*Stack.hpp family — CLAUDE.md §1 always scanned all headers)
# that trips ENTRY_RE must either
#   (a) reference a cost gate (OmegaCostGuard / ExecutionCostGuard), or
#   (b) be explained in scripts/ungated_engine_allowlist.txt (one basename per
#       line, reason documented per entry — false positives, tombstoned/dormant
#       headers, the disabled-engine list, documented exceptions).
# Any NEW unexplained hit = exit 1 = the P1-regression class CLAUDE.md §1 names
# (history: 2026-06-10 audit found 21 enabled shadow engines cost-blind).
# Stale allowlist entries (no longer tripping) are WARNED so the list stays honest.
#
# NOTE on gate semantics: a literal OmegaCostGuard/ExecutionCostGuard reference
# is necessary-not-sufficient (an injected gate_fn like StockDipTurtle's counts
# because engine_init passes ExecutionCostGuard — the literal lives in the header
# comment/gate hook). The allowlist carries anything the literal grep cannot see.
#
# Wired into scripts/mac_canary_engines.sh (pre-commit canary).
# Run standalone: bash scripts/ungated_engine_audit.sh
# Exit 0 = clear; exit 1 = new unexplained ungated hit; exit 2 = structural
# breakage (ENTRY_RE zero-parse / allowlist missing).
# ---------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.." || exit 2

# ADVERSE_SRC / ALLOW overridable for negative-testing only.
ADVERSE_SRC="${ADVERSE_SRC:-scripts/adverse_protection_audit.sh}"
ALLOW="${ALLOW:-scripts/ungated_engine_allowlist.txt}"

# ---- derive ENTRY_RE from the adverse audit (single source of truth) --------
ENTRY_RE_LINE=$(grep -m1 -E "^ENTRY_RE='" "$ADVERSE_SRC" 2>/dev/null)
if [ -z "$ENTRY_RE_LINE" ]; then
  echo "FAIL [ZERO-PARSE]: could not extract an ^ENTRY_RE=' line from $ADVERSE_SRC."
  echo "      The opener-idiom regex is DERIVED from the adverse audit (derive-don't-copy)."
  echo "      If ENTRY_RE moved/renamed there, update this extractor in the SAME commit."
  exit 2
fi
eval "$ENTRY_RE_LINE"
if [ -z "${ENTRY_RE:-}" ]; then
  echo "FAIL [ZERO-PARSE]: ENTRY_RE extracted from $ADVERSE_SRC evaluated EMPTY."
  exit 2
fi

GATE_RE='OmegaCostGuard|ExecutionCostGuard'

if [ ! -f "$ALLOW" ]; then
  echo "FAIL [STRUCTURAL]: allowlist $ALLOW is missing."
  exit 2
fi
allow_names=$(grep -vE '^[[:space:]]*(#|$)' "$ALLOW" | awk '{print $1}')
is_allowed(){ printf '%s\n' "$allow_names" | grep -qxF "$1"; }

# ---- scan --------------------------------------------------------------------
fails=0; gated=0; allowed=0
hit_names=" "
for h in include/*.hpp; do
  [ -f "$h" ] || continue
  grep -Eq "$ENTRY_RE" "$h" || continue          # not a position-opener
  base="$(basename "$h" .hpp)"
  if grep -qE "$GATE_RE" "$h"; then gated=$((gated+1)); continue; fi
  hit_names="$hit_names$base "
  if is_allowed "$base"; then allowed=$((allowed+1)); continue; fi
  echo "UNGATED (new, unexplained): $h"
  echo "        trips opener regex, has no OmegaCostGuard/ExecutionCostGuard reference,"
  echo "        and is not explained in $ALLOW."
  fails=$((fails+1))
done

# ---- stale allowlist entries (warn-only; keep the list honest) ---------------
for n in $allow_names; do
  case "$hit_names" in
    *" $n "*) : ;;
    *) echo "WARN: stale allowlist entry '$n' — include/$n.hpp no longer trips ungated (gated, renamed, or deleted). Remove/annotate it." ;;
  esac
done

echo "--- ungated-engine audit: $gated gated, $allowed allowlisted, $fails new-unexplained ---"
if [ "$fails" -gt 0 ]; then
  echo "FAIL: an engine opens positions with NO cost gate and NO documented explanation."
  echo "      Fix = gate the entry path with ExecutionCostGuard::is_viable (the 2026-06-10"
  echo "      21-engine pattern), OR — only if it is genuinely a false positive / disabled /"
  echo "      tombstoned / independent-mimic case — add the basename to $ALLOW"
  echo "      WITH a documented reason. An ENABLED engine appearing here = P1 regression."
  exit 1
fi
echo "PASS: every opener-idiom header is cost-gated or documented in the allowlist."
exit 0
