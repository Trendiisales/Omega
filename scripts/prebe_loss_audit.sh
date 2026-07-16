#!/bin/bash
# scripts/prebe_loss_audit.sh
# ---------------------------------------------------------------------
# STANDING GATE — PRE-BE LOSS AUDIT (added S-2026-07-17, operator mandate
# "why is there not a check", enforcing memory feedback-no-prebe-loss-ever).
#
# THE HOLE THIS CLOSES
#   scripts/adverse_protection_audit.sh only checks that a verdict TAG is
#   PRESENT ("is there a backtested adverse-protection decision"). It does NOT
#   inspect the booking path. Every vulnerable companion (FxUpJumpLadderCompanion
#   LOSS_CUT/TRAIL_STOP; GoldTrendMimicLadder pre-arm LOSS_CUT/WINDOW_CAP;
#   BeCascadeCompanionEngine PREBE_CUT/PREBE_STOP/REVERSAL_CUT) carries such a tag
#   and PASSES that script YET books clips net<0 BEFORE break-even is covered.
#   The hard rule (feedback-no-prebe-loss-ever, BOTH systems): a companion/mimic
#   clip must NEVER book net<0 pre-BE. The ONLY compliant shape is BE-ENTRY /
#   BE-floor-on-open (open at fav>=BE, book NOTHING until BE is covered, floor at
#   BE thereafter). A written rule with no gate fails the first time a session
#   ignores it — so this is a build gate, not prose.
#
# WHAT THIS GATE DOES (complements, does NOT duplicate, the adverse audit)
#   adverse_protection_audit.sh : "is there a verdict?"          (tag present)
#   THIS (prebe_loss_audit.sh)  : "can a clip book pre-BE neg?"  (booking path)
#
#   1. Derives the in-class header set: include/*Companion*.hpp, *Ladder*.hpp,
#      *Mimic*.hpp  UNION  any include/*.hpp defining a class/struct whose name
#      contains Companion|Ladder|Mimic (content-derived — no blind hardcode).
#   2. Finds every CLIP-BOOKING site: a call to an emitter
#      (book_clip_ | jf_book_ | emit_clip_ | emit_be_clip_) carrying a quoted
#      reason token. A site is SAFE-BY-EXEMPTION only if EVERY reason token on the
#      line is a recognised BE-floor reason (BE_FLOOR / RESTORE_FLOOR /
#      FLOOR_TRAIL_STOP). ANY other reason (LOSS_CUT / PREBE_CUT / PREBE_STOP /
#      REVERSAL_CUT / REVERSAL_EXIT / WINDOW_EXIT / WINDOW_CAP / TRAIL_STOP /
#      ENGINE_EXIT / RESTORE_PREBE / a brand-new reason) is RISK-BY-DEFAULT — a
#      clip that can settle below entry+cost. Safe-by-default: unknown reasons are
#      risk, so a new negative reason cannot slip in silently.
#   3. Requires each RISK site to be BE-FLOOR PROTECTED, recognised by:
#        FILE-level marker (whole header compliant — the fix landed):
#          confirm_anchor_epx | be_floor_on_open | (mimic_floor && be_floor)
#        SITE-level marker (within +/-WINDOW lines of the booking site):
#          the file markers above, or a std::max(entry,..)/std::min(entry,..)
#          floor clamp on the booked fill/level.
#      A risk site with NO such marker is UNPROTECTED. An UNPROTECTED site is a
#      FAIL unless its header basename is documented in the allowlist.
#   4. Allowlist (scripts/prebe_loss_allowlist.txt): one basename per line + a
#      documented reason. Grandfathers the currently-vulnerable engines as
#      "backfill-owed — fix landing this session" so the gate is green NOW while
#      the debt stays visible. THE ALLOWLIST IS SELF-RETIRING: once an agent lands
#      a confirm_anchor_epx / be_floor_on_open marker in a grandfathered header,
#      that header is recognised compliant WITHOUT the allowlist entry, and its
#      now-stale entry is WARNED so the list stays honest.
#
# Wired into scripts/mac_canary_engines.sh (the mandated pre-commit canary), so a
# companion that can book a pre-BE negative clip cannot be committed.
#
# Run standalone: bash scripts/prebe_loss_audit.sh
# Exit 0 = clear (every risk site floored or grandfathered);
#      1 = a NEW unprotected pre-BE booking site (fix it or document it);
#      2 = structural breakage (no in-class headers found / allowlist missing).
#
# See scripts/PREBE_LOSS_GATE.md for the full semantics + the sketch of the
# later runtime boot gate (Chimera-style [MIMIC-FLOOR-GATE] N/N floored).
# ---------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.." || exit 2

ALLOW="${ALLOW:-scripts/prebe_loss_allowlist.txt}"
WINDOW="${WINDOW:-6}"          # +/- lines around a booking site scanned for a site marker

# ---- reason / emitter / marker vocabulary -----------------------------------
# Emitter calls that BOOK a clip into a forward money book. A booking site is one
# of these WITH a quoted reason token (function *definitions* have no reason token
# and are skipped; book_mimic_stop_ books at the resting stop >= BE and carries no
# reason token, so it is inherently floor-safe and not listed).
EMITTER_RE='(book_clip_|jf_book_|emit_clip_|emit_be_clip_)[[:space:]]*\('
# BE-floor reasons: a clip tagged ONLY with these settles at/above break-even in
# the REAL column (not just a clamped MODEL number). Booked AT a resting stop that
# is >= entry+cost, never at a worse-of close.
#   BE_FLOOR / RESTORE_FLOOR / FLOOR_TRAIL_STOP : floor books, settle >= BE.
#   FLOOR_TRAIL_CLIP : the intrabar mimic-floor exit — books at lg.stop_px which is
#     ratcheted to >= le*(1+RT), so gross_real = gross - RT >= 0. PROVABLY floored.
# DELIBERATELY NOT SAFE: BE_TRAIL_CLIP / ENGINE_EXIT — the be_floor-family honest
# real column books the WORSE-OF H1 close (gross_real = (cur/le-1)), which can be
# BELOW BE when price gapped through the stop between closes. Those are the exact
# pre-BE-negative clips this gate exists to expose, so they stay RISK-by-default.
SAFE_REASON_RE='^(BE_FLOOR|RESTORE_FLOOR|FLOOR_TRAIL_STOP|FLOOR_TRAIL_CLIP)$'
# Any quoted ALL-CAPS token on an emitter line is treated as a reason.
REASON_TOKEN_RE='"[A-Z][A-Z0-9_]+"'
# FILE-level compliance markers -> the whole header is BE-floored-on-open.
FILE_MARKER_RE='confirm_anchor_epx|be_floor_on_open|mimic_floor[[:space:]]*&&[[:space:]]*be_floor'
# SITE-level markers (file markers + an entry-clamp floor on the booked value).
SITE_MARKER_RE='confirm_anchor_epx|be_floor_on_open|std::max[[:space:]]*\([[:space:]]*entry|std::min[[:space:]]*\([[:space:]]*entry'

# ---- allowlist ---------------------------------------------------------------
if [ ! -f "$ALLOW" ]; then
  echo "FAIL [STRUCTURAL]: allowlist $ALLOW is missing."
  echo "      Every gate in this repo carries an allowlist; a missing one is a"
  echo "      structural break, not a silent pass."
  exit 2
fi
allow_names=$(grep -vE '^[[:space:]]*(#|$)' "$ALLOW" | awk '{print $1}')
is_allowed(){ printf '%s\n' "$allow_names" | grep -qxF "$1"; }

# ---- derive the in-class header set (filename globs + content-defined class) --
tmp_set=$(mktemp -t prebe_set_XXXXXX)
trap 'rm -f "$tmp_set"' EXIT
for h in include/*Companion*.hpp include/*Ladder*.hpp include/*Mimic*.hpp; do
  [ -f "$h" ] && echo "$h" >> "$tmp_set"
done
# content-derived: any header defining a Companion/Ladder/Mimic class/struct
grep -rlE '(class|struct)[[:space:]]+[A-Za-z0-9_]*(Companion|Ladder|Mimic)' include/*.hpp 2>/dev/null >> "$tmp_set"
HEADERS=$(sort -u "$tmp_set")

if [ -z "$HEADERS" ]; then
  echo "FAIL [ZERO-PARSE]: no companion/ladder/mimic header found under include/."
  echo "      The in-class set is DERIVED (filename globs + class-name scan); an"
  echo "      empty set means the derivation broke, not that the class is clean."
  exit 2
fi

# ---- scan --------------------------------------------------------------------
fails=0; clean=0; allowed=0; total_risk=0
hit_names=" "        # headers that have >=1 risk site (protected or not)

for h in $HEADERS; do
  [ -f "$h" ] || continue
  base="$(basename "$h" .hpp)"

  # File-level compliance marker -> whole header is BE-floored-on-open.
  file_clean=0
  grep -Eq "$FILE_MARKER_RE" "$h" && file_clean=1

  # Enumerate risk booking sites (emitter line whose reason set is not all-SAFE).
  risk_lines=""
  while IFS= read -r ln; do
    [ -z "$ln" ] && continue
    no="${ln%%:*}"
    body="${ln#*:}"
    # reason tokens on this line
    reasons=$(printf '%s\n' "$body" | grep -oE "$REASON_TOKEN_RE" | tr -d '"')
    [ -z "$reasons" ] && continue          # emitter def / no reason -> not a booking site
    risky=0
    for r in $reasons; do
      echo "$r" | grep -Eq "$SAFE_REASON_RE" || risky=1
    done
    [ "$risky" -eq 0 ] && continue         # every reason is a BE-floor reason -> safe
    risk_lines="$risk_lines $no"
  done <<EOF
$(grep -nE "$EMITTER_RE" "$h")
EOF

  [ -z "$risk_lines" ] && continue         # header books no risk clips at all
  hit_names="$hit_names$base "
  nsites=$(printf '%s\n' $risk_lines | grep -c .)
  total_risk=$((total_risk + nsites))

  # Is every risk site protected?
  unprotected=""
  if [ "$file_clean" -eq 1 ]; then
    :                                      # file marker floors every site
  else
    for no in $risk_lines; do
      lo=$((no - WINDOW)); [ "$lo" -lt 1 ] && lo=1
      hi=$((no + WINDOW))
      if sed -n "${lo},${hi}p" "$h" | grep -Eq "$SITE_MARKER_RE"; then
        continue                           # site-local floor marker -> protected
      fi
      unprotected="$unprotected $no"
    done
  fi

  if [ -z "$unprotected" ]; then
    clean=$((clean + 1))
    if is_allowed "$base"; then
      echo "WARN: stale allowlist entry '$base' — $nsites risk site(s) are now BE-floor"
      echo "      protected (marker present). Remove it from $ALLOW."
    else
      echo "CLEAN: $h — $nsites risk site(s), all BE-floor protected."
    fi
    continue
  fi

  # Unprotected risk sites remain.
  nunp=$(printf '%s\n' $unprotected | grep -c .)
  if is_allowed "$base"; then
    allowed=$((allowed + 1))
    echo "ALLOWLISTED (backfill-owed): $h — $nunp unprotected pre-BE booking site(s) at line(s):$unprotected"
    continue
  fi
  echo "VIOLATION: $h books pre-BE-NEGATIVE clip(s) with NO BE-floor-on-open marker"
  echo "           and is NOT grandfathered. Unprotected booking site(s) at line(s):$unprotected"
  for no in $unprotected; do
    echo "             L$no: $(sed -n "${no}p" "$h" | sed 's/^[[:space:]]*//')"
  done
  fails=$((fails + nunp))
done

# ---- stale allowlist entries (warn-only; keep the list honest) ---------------
for n in $allow_names; do
  case "$hit_names" in
    *" $n "*) : ;;
    *) echo "WARN: stale allowlist entry '$n' — include/$n.hpp no longer books a risk clip (fixed, renamed, or deleted). Remove/annotate it." ;;
  esac
done

echo "--- prebe-loss audit: $clean clean, $allowed allowlisted(backfill-owed), $fails new-unprotected-site(s) across $total_risk risk site(s) ---"
if [ "$fails" -gt 0 ]; then
  echo "FAIL: a companion/mimic/ladder can BOOK A CLIP net<0 BEFORE break-even is covered."
  echo "      This is the hard-forbidden pre-BE loss (feedback-no-prebe-loss-ever, BOTH systems)."
  echo "      Fix = make the cell BE-ENTRY / BE-floor-on-open: open only at fav>=BE, book"
  echo "      NOTHING until BE is covered, floor the exit at entry thereafter. Recognised"
  echo "      markers: a confirm_anchor_epx anchor, a be_floor_on_open clamp, a"
  echo "      std::max(entry,..)/std::min(entry,..) floor on the booked fill, or a mimic_floor"
  echo "      && be_floor cell. If (and ONLY if) the header is retired/dormant/out-of-class,"
  echo "      add its basename to $ALLOW with a documented reason."
  exit 1
fi
echo "PASS: no companion can book a clip net<0 before break-even is covered"
echo "      (every risk site is BE-floored-on-open or grandfathered as backfill-owed)."
[ "$allowed" -gt 0 ] && echo "NOTE: $allowed grandfathered engine(s) still owe a BE-floor-on-open fix (backfill debt)."
exit 0
