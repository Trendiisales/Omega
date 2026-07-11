#!/bin/bash
# scripts/trade_visibility_gate.sh — producer<->desk completeness gate
# (S-2026-07-12 never-again audit, class A: an entire system trading invisibly).
#
# WHY: chimera/josgp1 produced shadow trades for WEEKS with NO pipeline to the operator's
# desk (closes died in journalctl). scripts/mimic_pnl_completeness_gate.sh only covers
# endpoint<->fold; the missing class was producer<->endpoint — a book with NO endpoint at
# all. This gate enforces tools/trade_visibility_manifest.tsv, the closed-world record of
# every trade-producing book across omega-new / mac-crypto / josgp1.
#
# CHECKS:
#   (1) STATUS      any manifest row status=STRANDED -> FAIL. FIX-IN-FLIGHT -> loud WARN
#                   (chimera pipeline + the two stranded Mac crypto companions, 2026-07-12).
#   (2) DRIFT-EP    every /api/* endpoint a WIRED* row claims must exist in
#                   src/gui/OmegaTelemetryServer.cpp (manifest can't rot silently).
#   (3) DRIFT-FILE  every Mac-local producer path in the manifest must exist.
#   (4) COVERAGE    every money-book endpoint served by the telemetry server
#                   (companion|ladder|rdagent_book|crypto_trades) must appear in the
#                   manifest — a NEW endpoint without a manifest row = FAIL. This is the
#                   forcing function: a new book must be manifested in the same commit.
#   (5) CRON-PUSH   every active (non-comment) Mac crontab line that scp/pushes a
#                   state/trades file to the VPS must correspond to a manifest transport
#                   entry — a new push pipeline without a manifest row = FAIL.
#
# Exit 0 = every known producer visible (or explicitly research/retired); 1 = a gap.
# Wired into scripts/mac_canary_engines.sh after the pnl completeness gate.
set -u
cd "$(dirname "$0")/.." || exit 2
MAN=tools/trade_visibility_manifest.tsv
SRV=src/gui/OmegaTelemetryServer.cpp
fails=0; warns=0; rows=0

[ -f "$MAN" ] || { echo "FAIL: $MAN missing"; exit 1; }

# ---- (1)(2)(3) row-by-row -------------------------------------------------
while IFS=$'\t' read -r book system producer transport endpoint panel fold status note; do
  case "$book" in \#*|"") continue;; esac
  rows=$((rows+1))
  case "$status" in
    STRANDED)
      echo "STRANDED: $book ($system) — producer=$producer writes money/trades the desk CANNOT see."
      fails=$((fails+1));;
    FIX-IN-FLIGHT)
      echo "WARN FIX-IN-FLIGHT: $book ($system) — known visibility gap, fix owed. ${note:0:120}"
      warns=$((warns+1));;
    WIRED|WIRED-DISPLAY|RETIRED-ok|RESEARCH-ONLY|OBSERVATION) :;;
    *)
      echo "BAD-STATUS: $book has unknown status '$status' (typo?) — treating as FAIL."
      fails=$((fails+1));;
  esac
  # (2) endpoint drift (only endpoints this repo serves)
  case "$endpoint" in
    /api/*)
      ep="${endpoint#/api/}"
      if ! grep -q "GET /api/$ep" "$SRV"; then
        echo "DRIFT-EP: $book claims $endpoint but OmegaTelemetryServer.cpp does not serve it."
        fails=$((fails+1))
      fi;;
  esac
  # (3) producer-file drift (Mac-checkable paths only)
  case "$producer" in
    josgp1:*|NONE|"") :;;
    /*)  [ -e "$producer" ] || { echo "DRIFT-FILE: $book producer $producer missing."; fails=$((fails+1)); };;
    *)   [ -e "$producer" ] || { echo "DRIFT-FILE: $book producer $producer missing (repo-relative)."; fails=$((fails+1)); };;
  esac
done < "$MAN"

# ---- (4) endpoint coverage: served money endpoints must be manifested ------
SERVED=$(grep -oE 'GET /api/[a-z0-9_]+' "$SRV" | sed 's#GET ##' | sort -u \
         | grep -E 'companion|ladder|rdagent_book|crypto_trades')
for ep in $SERVED; do
  if ! grep -q "	$ep	" "$MAN"; then
    echo "UNMANIFESTED-ENDPOINT: telemetry server serves $ep but no manifest row claims it — new book shipped without a visibility row."
    fails=$((fails+1))
  fi
done

# ---- (5) cron push coverage: every active VPS push pipeline is manifested --
# Any live crontab line that scp's a trades/state artifact to the VPS must map to a
# manifest transport. Detects a NEW push pipeline (i.e. a new book) added without a row.
CRON_PUSHES=$(crontab -l 2>/dev/null | grep -v '^#' | grep -oE 'omega-(new|vps):C:/Omega/[^ ;]*' | sort -u)
for dst in $CRON_PUSHES; do
  base=$(basename "$dst" | sed 's/\.[a-z]*$//')
  if ! grep -qiE "$(basename "$dst")|${base}" "$MAN"; then
    echo "UNMANIFESTED-PUSH: crontab pushes $dst to the desk but no manifest row mentions it."
    fails=$((fails+1))
  fi
done

echo "--- trade visibility: $rows book row(s), $warns FIX-IN-FLIGHT warn(s), $fails gap(s) ---"
if [ "$fails" -gt 0 ]; then
  echo "FAIL: a trade-producing book is invisible to the desk, or the manifest drifted."
  echo "      Every new book needs a tools/trade_visibility_manifest.tsv row IN THE SAME COMMIT."
  exit 1
fi
if [ "$warns" -gt 0 ]; then
  echo "PASS (with $warns FIX-IN-FLIGHT warning(s) — visibility debt is on record, not hidden)."
else
  echo "PASS: every known trade producer reaches the desk or is explicitly research/retired."
fi
exit 0
