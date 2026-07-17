#!/bin/bash
# scripts/persist_restore_semantics_audit.sh
# ---------------------------------------------------------------------
# STRUCTURAL GATE — persist/restore semantics audit (added S-2026-07-17k,
# SESSION_HANDOFF_2026-07-17k item 3: "confirm no more trade cancellations on
# deploy" — operator wants a STRUCTURAL guarantee, not prose).
#
# WHY (history, commit c1903e88): IndexBearShortEngine saved
# PositionSnapshot.entry_ts in MILLISECONDS while the snapshot contract is
# EPOCH SECONDS -> the central phantom-drop exemption (keyed on the restored
# entry_ts) never matched -> a restored position's close was silently DROPPED
# from the ledger (2026-07-17 boot: NAS SHORT TIME_STOP +$111.75 vanished).
# Second bug in the same engine: entry_bar_seq was not restored while the seed
# replay advances bar_seq_ before restore runs -> bar_seq_ - entry_bar_seq >=
# MAX_HOLD instantly true -> spurious TIME_STOP on the first live bar (the
# hold=1463bars tell). Both fixed in c1903e88; this audit closes the CLASS so
# no other engine can re-introduce either shape.
#
# WHAT (two classes, scanned across include/*.hpp):
#   CLASS 1 — UNIT CONVENTION: every site that writes .entry_ts into an
#   omega::PositionSnapshot (the persist/display snapshot; contract = EPOCH
#   SECONDS, asserted below against OpenPositionRegistry.hpp derive-don't-copy)
#   must either (a) visibly convert with /1000 (ms source), (b) carry a
#   `// seconds-native` or `PERSIST-UNITS: seconds` comment on the line or the
#   3 lines above, or (c) be explained in scripts/persist_restore_allowlist.txt
#   (key = "<Base>.hpp:<lhs>.entry_ts=<rhs>", whitespace-stripped, one
#   documented reason per entry). Snapshot vars are DERIVED per file: non-const
#   `PositionSnapshot&` params (the persist_save archetype) + local
#   `PositionSnapshot x;` declarations (engine_init display/persist lambdas).
#
#   CLASS 2 — HOLD-CLOCK RE-ANCHOR: every engine header that DEFINES a restore
#   function (persist_restore / persist_load / restore_position /
#   load_live_state*) AND carries a bar-seq / hold-counter time exit must
#   re-anchor its hold clock inside the restore body: an assignment of the
#   entry-seq/held field (entry_bar_seq/bars_held/held/... = ...) OR a
#   persisted-held stream read (`>> held_`, the StockDipTurtle
#   load_live_state_ pattern — persisted, not recomputed, fully valid).
#   Wall-clock engines (hold measured off entry_ts/entry_ms) pass by restoring
#   the entry timestamp. Anything else must be in the allowlist
#   (key = "HOLD:<Base>.hpp") with a documented reason — including the LATENT
#   IndexBearShort-shaped engines that are currently unwired/disabled; those
#   entries carry a MUST-FIX-BEFORE-RE-ENABLE note, same convention as the
#   disabled-engine list in ungated_engine_allowlist.txt.
#
# Wired into scripts/mac_canary_engines.sh (pre-commit canary).
# Run standalone: bash scripts/persist_restore_semantics_audit.sh
# Exit 0 = clean; exit 1 = new unexplained violation (P1 — the deploy-eats-
# trades class); exit 2 = script self-check failure (zero files/sites scanned,
# snapshot contract line drifted, allowlist missing).
# ---------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.." || exit 2

ALLOW="${ALLOW:-scripts/persist_restore_allowlist.txt}"

if [ ! -f "$ALLOW" ]; then
  echo "FAIL [STRUCTURAL]: allowlist $ALLOW is missing."
  exit 2
fi
allow_keys=$(grep -vE '^[[:space:]]*(#|$)' "$ALLOW" | awk '{print $1}')
is_allowed(){ printf '%s\n' "$allow_keys" | grep -qxF "$1"; }

# ---- contract self-check (derive-don't-copy) --------------------------------
# The whole CLASS-1 premise is "PositionSnapshot.entry_ts is EPOCH SECONDS".
# Assert the contract still says so at its single source of truth. If this
# line moved/changed in OpenPositionRegistry.hpp, the audit's premise is dead:
# update BOTH in the same commit.
if ! grep -qE 'int64_t[[:space:]]+entry_ts.*seconds' include/OpenPositionRegistry.hpp; then
  echo "FAIL [ZERO-PARSE]: PositionSnapshot.entry_ts 'epoch seconds' contract line not found"
  echo "      in include/OpenPositionRegistry.hpp. The unit contract this audit enforces has"
  echo "      moved or changed -- update the audit + contract in the SAME commit."
  exit 2
fi

files_scanned=0
for h in include/*.hpp; do [ -f "$h" ] && files_scanned=$((files_scanned+1)); done
if [ "$files_scanned" -eq 0 ]; then
  echo "FAIL [ZERO-SCAN]: no include/*.hpp files found -- wrong cwd or tree layout drifted."
  exit 2
fi

# ============================================================================
# CLASS 1 — snapshot entry_ts unit convention
# ============================================================================
# awk per file emits:  <status>|<line#>|<key>
#   status: CONV (visible /1000), COMMENT (seconds-native marker), CAND
#   key:    <Base>.hpp:<lhs>.entry_ts=<rhs>   (whitespace-stripped, ;-truncated)
u_checked=0; u_conv=0; u_comment=0; u_allowed=0; u_viol=0
u_hit_keys=" "
u_out=$(for h in include/*.hpp; do
  awk -v BASE="$(basename "$h")" '
  {
    # -- collect snapshot var names: non-const ref params + local declarations
    line=$0
    while (match(line, /(omega::)?PositionSnapshot&[ \t]*[A-Za-z_][A-Za-z0-9_]*/)) {
      seg=substr(line,RSTART,RLENGTH); pre=substr(line,1,RSTART-1)
      if (pre !~ /const[ \t]*$/) { n=seg; sub(/.*&[ \t]*/,"",n); vars[n]=1 }
      line=substr(line,RSTART+RLENGTH)
    }
    line=$0
    while (match(line, /(omega::)?PositionSnapshot[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*[;={]/)) {
      seg=substr(line,RSTART,RLENGTH)
      n=seg; sub(/^(omega::)?PositionSnapshot[ \t]+/,"",n); sub(/[ \t]*[;={].*/,"",n); vars[n]=1
      line=substr(line,RSTART+RLENGTH)
    }
    # -- scan for <var>.entry_ts = <rhs> write sites
    for (v in vars) {
      if (match($0, "(^|[^A-Za-z0-9_.])" v "\\.entry_ts[ \t]*=[^=]")) {
        seg=substr($0,RSTART)
        if (seg !~ ("^" v "\\.")) seg=substr(seg,2)   # drop boundary char
        sub(/\/\/.*/,"",seg); sub(/;.*/,"",seg); gsub(/[ \t]/,"",seg)
        if (seg ~ /\/1000/)                                    st="CONV"
        else if ($0 ~ /seconds-native|PERSIST-UNITS/ ||
                 p1 ~ /seconds-native|PERSIST-UNITS/ ||
                 p2 ~ /seconds-native|PERSIST-UNITS/ ||
                 p3 ~ /seconds-native|PERSIST-UNITS/)          st="COMMENT"
        else                                                   st="CAND"
        print st "|" NR "|" BASE ":" seg
      }
    }
    p3=p2; p2=p1; p1=$0
  }' "$h"
done)

while IFS='|' read -r st ln key; do
  [ -z "${st:-}" ] && continue
  u_checked=$((u_checked+1))
  case "$st" in
    CONV)    u_conv=$((u_conv+1));;
    COMMENT) u_comment=$((u_comment+1));;
    CAND)
      u_hit_keys="$u_hit_keys$key "
      if is_allowed "$key"; then u_allowed=$((u_allowed+1)); else
        f="${key%%:*}"
        echo "UNIT VIOLATION (new, unexplained): include/$f:$ln"
        echo "        '$key' writes entry_ts into a PositionSnapshot with no visible /1000,"
        echo "        no seconds-native/PERSIST-UNITS marker, and no allowlist entry."
        echo "        (PositionSnapshot.entry_ts is EPOCH SECONDS -- an ms value here silently"
        echo "        breaks the phantom-drop exemption: the c1903e88 dropped-close bug.)"
        u_viol=$((u_viol+1))
      fi;;
  esac
done <<EOF
$u_out
EOF

if [ "$u_checked" -eq 0 ]; then
  echo "FAIL [ZERO-SCAN]: CLASS 1 found no snapshot entry_ts write sites at all -- the"
  echo "      persist_save archetype has drifted; fix the extractor in the SAME commit."
  exit 2
fi

# ============================================================================
# CLASS 2 — hold-clock re-anchor on restore
# ============================================================================
RESTORE_NAMES='persist_restore|persist_load|restore_position|load_live_state'
HOLD_RE='entry_bar_seq|MAX_HOLD|max_hold|bars_held|hold_bars|bars_in_trade|[^A-Za-z0-9_]held_?[[:space:]]*>=|[^A-Za-z0-9_]hold_?[[:space:]]*>='
COUNTER_RE='entry_bar_seq|bars_held|bars_in_trade|\.held[^A-Za-z0-9_]|[^A-Za-z0-9_]held_[^A-Za-z0-9_]'
# anchor = assignment of the entry-seq/held field OR persisted-held stream read
ANCH_COUNTER='(^|[^A-Za-z0-9_])(entry_bar_seq|[A-Za-z0-9_]*bars_held|bars_in_trade|held)[A-Za-z0-9_]*[[:space:]]*=[^=]|>>[[:space:]]*[A-Za-z0-9_]*held'
ANCH_WALL='(^|[^A-Za-z0-9_])(entry_ts|entry_ms|ets|base_entry_ts)[A-Za-z0-9_]*[[:space:]]*=[^=]|>>[[:space:]]*(entry_ts|ets|entry_ms)'

h_checked=0; h_anchored=0; h_allowed=0; h_viol=0
h_hit_keys=" "
for h in include/*.hpp; do
  [ -f "$h" ] || continue
  # candidate = DEFINES a restore fn (not a call/declaration) AND has hold tokens
  grep -qE "^[[:space:]]*(inline[[:space:]]+)?(bool|void|int)[[:space:]].*(${RESTORE_NAMES})[A-Za-z0-9_]*[[:space:]]*\(" "$h" || continue
  grep -qE "$HOLD_RE" "$h" || continue
  h_checked=$((h_checked+1))
  base="$(basename "$h")"
  # extract every restore-function body (brace-counted from the definition line)
  body=$(awk -v RE="$RESTORE_NAMES" '
    BEGIN{inb=0; depth=0; started=0}
    {
      if (!inb && $0 ~ ("^[ \t]*(inline[ \t]+)?(bool|void|int)[ \t].*(" RE ")[A-Za-z0-9_]*[ \t]*\\(")) {
        if ($0 ~ /\);[ \t]*(\/\/.*)?$/ && $0 !~ /{/) next   # pure declaration
        inb=1; depth=0; started=0
      }
      if (inb) {
        print
        n=gsub(/{/,"{"); m=gsub(/}/,"}"); depth+=n-m
        if (depth>0) started=1
        if (started && depth<=0) { inb=0; started=0; depth=0 }
      }
    }' "$h")
  if grep -qE "$COUNTER_RE" "$h"; then anch="$ANCH_COUNTER"; kind="bar-counter"
  else                                 anch="$ANCH_WALL";    kind="wall-clock"
  fi
  if printf '%s\n' "$body" | grep -qE "$anch"; then
    h_anchored=$((h_anchored+1)); continue
  fi
  key="HOLD:$base"
  h_hit_keys="$h_hit_keys$key "
  if is_allowed "$key"; then h_allowed=$((h_allowed+1)); continue; fi
  echo "HOLD-CLOCK VIOLATION (new, unexplained): $h"
  echo "        has a restore path + a $kind hold exit, but the restore body never"
  echo "        re-anchors the hold clock (no entry-seq/held assignment, no persisted-held"
  echo "        read). A seed replay that advances the bar counter before restore fires a"
  echo "        spurious TIME_STOP at boot (the c1903e88 IndexBearShort bug). Fix = restart"
  echo "        the hold clock in restore (entry_bar_seq = bar_seq_;) or persist the held"
  echo "        counter, or -- only with a documented reason -- add '$key' to $ALLOW."
  h_viol=$((h_viol+1))
done

if [ "$h_checked" -eq 0 ]; then
  echo "FAIL [ZERO-SCAN]: CLASS 2 found no engine with a restore path + hold exit -- the"
  echo "      restore-fn naming has drifted; fix RESTORE_NAMES in the SAME commit."
  exit 2
fi

# ---- stale allowlist entries (warn-only; keep the list honest) ---------------
for k in $allow_keys; do
  case "$u_hit_keys$h_hit_keys" in
    *" $k "*) : ;;
    *) echo "WARN: stale allowlist entry '$k' -- no longer trips the audit (converted, renamed, or deleted). Remove/annotate it." ;;
  esac
done

# ---- summary ------------------------------------------------------------------
echo "[PERSIST-RESTORE] UNITS: $u_checked sites checked ($u_conv /1000-converted, $u_comment seconds-native-commented), $u_allowed allowlisted, $u_viol VIOLATION"
echo "[PERSIST-RESTORE] HOLD-CLOCK: $h_checked engines checked ($h_anchored re-anchored), $h_allowed allowlisted, $h_viol VIOLATION"
total=$((u_viol+h_viol))
echo "[PERSIST-RESTORE] $((u_checked+h_checked)) sites checked, $((u_allowed+h_allowed)) allowlisted, $total VIOLATION"
if [ "$total" -gt 0 ]; then
  echo "FAIL: a persist/restore path can silently eat or cancel a trade on deploy."
  echo "      Fix the unit conversion / hold-clock re-anchor (reference pattern:"
  echo "      include/IndexBearShortEngine.hpp persist_save/persist_restore, c1903e88),"
  echo "      OR add the printed key to $ALLOW WITH a documented reason."
  exit 1
fi
echo "PASS: every snapshot ts write is unit-safe and every restored hold clock re-anchors (or is documented)."
exit 0
