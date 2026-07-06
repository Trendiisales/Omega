#!/usr/bin/env bash
# gui_drift_gate.sh — HARD GATE: include/OmegaIndexHtml.hpp MUST equal the
# regenerated output of tools/gui/omega_desk.html. Fails if the committed .hpp
# was hand-edited instead of regenerated from source.
#
# WHY THIS EXISTS (added 2026-07-06): the .hpp header says "do not hand-edit"
# and the vault says "edit .html -> regen -> commit both" -- but that was PROSE,
# never a gate. Three GUI commits (c108439e/62a9b83b FX panel, 7a770358 gold
# panel) hand-edited the .hpp directly (one even manually re-split a raw-string
# chunk, OMEGAD3F, to dodge MSVC C2026) and left omega_desk.html 61 lines behind
# the deployed binary. A prior drift-heal (35caf1a8) fixed the symptom but added
# no check, so it recurred immediately. This promotes the rule to a build gate,
# mirroring the adverse-protection audit / tombstone guard pattern: runs inside
# scripts/mac_canary_engines.sh (the mandated pre-commit canary) so it cannot be
# skipped.
#
# The invariant is CONTENT: concat of the .hpp raw-string chunks == the .html,
# byte-for-byte. Chunk boundaries are an internal packaging detail; we compare by
# regenerating (canonical chunking) and diffing the whole .hpp.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/tools/gui/omega_desk.html"
DST="$ROOT/include/OmegaIndexHtml.hpp"

[ -f "$SRC" ] || { echo "[gui-drift] MISSING source: $SRC"; exit 1; }
[ -f "$DST" ] || { echo "[gui-drift] MISSING header: $DST"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Regenerate into a temp copy (never clobber the committed .hpp inside the gate).
cp "$DST" "$TMP/orig.hpp"
python3 "$ROOT/tools/gui/gen_index_html.py" >/dev/null
cp "$DST" "$TMP/regen.hpp"
# Restore the committed .hpp exactly as it was on disk before the gate ran.
cp "$TMP/orig.hpp" "$DST"

if ! diff -q "$TMP/orig.hpp" "$TMP/regen.hpp" >/dev/null; then
  echo "[gui-drift] FAIL: include/OmegaIndexHtml.hpp != regen(tools/gui/omega_desk.html)"
  echo "[gui-drift] The .hpp was hand-edited or the .html was changed without regenerating."
  echo "[gui-drift] FIX: edit tools/gui/omega_desk.html, then run:"
  echo "[gui-drift]        python3 tools/gui/gen_index_html.py"
  echo "[gui-drift]      and commit BOTH. Never hand-edit the .hpp."
  echo "[gui-drift] --- first diff hunk ---"
  diff "$TMP/orig.hpp" "$TMP/regen.hpp" | head -20
  exit 1
fi

echo "[gui-drift] OK: OmegaIndexHtml.hpp is in sync with omega_desk.html"
