#!/bin/bash
# scripts/companion_coverage_audit.sh
# ---------------------------------------------------------------------
# STANDING GATE — COMPANION GIVEBACK-COVERAGE AUDIT (S-2026-07-17t,
# operator mandate after the IndexBearShort $400 giveback: "fix this now" /
# feedback-profit-lock-mandatory "NO live book rides profit back down;
# every book needs tightest-CERTIFIED giveback per cell").
#
# THE HOLE THIS CLOSES
#   2026-07-17: IndexBearShort NAS100 peaked +$425 and rode back to ~+$25.
#   Its ONLY giveback cover was the shared "main" stall book, whose
#   gate_pct=1.5 arm was a 1:1 transcription of the retired gold-era cron —
#   NEVER certified for IndexBearShort — and sat 0.03% above the leg's
#   1.47% peak, so the clip never armed. Nothing structural objected to an
#   engine whose only profit-lock was an uncertified default.
#
# WHAT THIS GATE DOES
#   1. Derives the set of live snapshot engine tags (every `s.engine = "X"`
#      in engine_init.hpp — the tags the stall-companion zoo mirrors from
#      live_trades).
#   2. Derives the set of DEDICATED coverage tokens: every stall-book
#      `c.include={...}` entry and every mirror `m.legs = {...}` entry.
#   3. A tag is COVERED when some coverage token contains it (the zoo's own
#      icontains include semantics). A tag that is NOT covered must be
#      documented in scripts/companion_coverage_allowlist.txt (one tag per
#      line + reason: main-defaults-cover-certification-owed, symbol-level
#      cover, engine's own manage-path is the certified protection, ...).
#   4. An uncovered, un-allowlisted tag FAILS the build (exit 1) — a NEW
#      engine cannot ship with only the uncertified main-book defaults as
#      its profit lock. Allowlisted-but-uncovered tags print as OWED
#      warnings so the certification debt stays visible. STALE allowlist
#      entries (tag now covered or gone) are warned so the list stays honest.
#
# Wired into scripts/mac_canary_engines.sh (pre-commit canary).
# ---------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.."
INIT=include/engine_init.hpp
ALLOW=scripts/companion_coverage_allowlist.txt
[ -f "$INIT" ]  || { echo "[COMPANION-COVERAGE] FAIL: $INIT missing"; exit 2; }
[ -f "$ALLOW" ] || { echo "[COMPANION-COVERAGE] FAIL: $ALLOW missing"; exit 2; }

# 1. live snapshot engine tags
TAGS=$(grep -oE 's\.engine *= *"[^"]+"' "$INIT" | sed 's/.*"\(.*\)"/\1/' | sort -u)
[ -n "$TAGS" ] || { echo "[COMPANION-COVERAGE] FAIL: zero-parse (no s.engine tags in $INIT)"; exit 2; }

# 2. dedicated coverage tokens: stall include entries + mirror legs
COVER=$( { grep -oE 'c\.include=\{[^}]*\}' "$INIT" | grep -oE '"[^"]+"';
           grep -oE 'm\.legs *= *\{[^}]*\}' "$INIT" | grep -oE '"[^"]+"'; } \
         | tr -d '"' | sort -u )
[ -n "$COVER" ] || { echo "[COMPANION-COVERAGE] FAIL: zero-parse (no include/legs tokens in $INIT)"; exit 2; }

allow_reason() {   # $1 = tag -> reason text or ""
    awk -v t="$1" '!/^[[:space:]]*(#|$)/ { if ($1 == t) { $1=""; sub(/^[[:space:]]*/,""); print; exit } }' "$ALLOW"
}

fails=0; owed=0; covered=0
for tag in $TAGS; do
    hit=""
    while IFS= read -r tok; do
        case "$(echo "$tok" | tr '[:upper:]' '[:lower:]')" in
            *"$(echo "$tag" | tr '[:upper:]' '[:lower:]')"*) hit="$tok"; break;;
        esac
    done <<< "$COVER"
    if [ -n "$hit" ]; then
        covered=$((covered+1))
        continue
    fi
    reason=$(allow_reason "$tag")
    if [ -n "$reason" ]; then
        owed=$((owed+1))
        echo "[COMPANION-COVERAGE] OWED  $tag -- $reason"
    else
        fails=$((fails+1))
        echo "[COMPANION-COVERAGE] FAIL  $tag has NO dedicated giveback book/mirror and no allowlist entry"
        echo "                     -> certify a floored clip cell (stall_clip_sweep precedent) or document it in $ALLOW"
    fi
done

# stale allowlist entries (tag covered now, or tag no longer exists)
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue;; esac
    t=$(echo "$line" | awk '{print $1}')
    if ! echo "$TAGS" | grep -qx "$t"; then
        echo "[COMPANION-COVERAGE] WARN stale allowlist entry '$t' (tag no longer registered) -- remove it"
    else
        while IFS= read -r tok; do
            case "$(echo "$tok" | tr '[:upper:]' '[:lower:]')" in
                *"$(echo "$t" | tr '[:upper:]' '[:lower:]')"*)
                    echo "[COMPANION-COVERAGE] WARN stale allowlist entry '$t' (now has dedicated cover) -- remove it"; break;;
            esac
        done <<< "$COVER"
    fi
done < "$ALLOW"

echo "[COMPANION-COVERAGE] tags=$(echo "$TAGS" | wc -l | tr -d ' ') covered=$covered owed=$owed fail=$fails"
[ "$fails" -eq 0 ] || exit 1
exit 0
