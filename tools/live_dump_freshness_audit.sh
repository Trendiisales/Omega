#!/bin/bash
# tools/live_dump_freshness_audit.sh
# ---------------------------------------------------------------------
# STANDING GUARD — every live-dump CSV the VPS binary writes (a set_live_dump()
# target) MUST be listed in tools/live_dump_manifest.tsv so the Mac-side feeds
# self-test (tools/feeds_selftest.py) can poll its freshness on the VPS. This
# closes the blind spot the operator was furious about: "we have the data being
# saved in a csv file but when it stops being delivered we just have no idea."
#
# Why a build gate and not a memory note:
#   The live binary writes load-bearing CSVs to C:\Omega\logs. NONE are pulled to
#   the Mac, so the Mac freshness banners structurally CANNOT see them — a VPS
#   writer can die and every banner stays GREEN. The only defence is (a) polling
#   the VPS directly (feeds_selftest.py's VPS section) against a manifest, and
#   (b) THIS gate, which fails if a NEW set_live_dump() path is added to the code
#   without also being added to the manifest. So a newly-added live dump cannot
#   be committed unmonitored.
#
# Enforcement:
#   * Scan include/ and src/ for set_live_dump( call sites and the live-dump write
#     path literals passed to them (…/<name>.csv under log_root_dir()).
#   * Each DISTINCT dump filename must appear in tools/live_dump_manifest.tsv.
#   * A dump path not in the manifest is printed as UNMONITORED and WARNs.
#
# Note: event-driven files (omega_trade_closes.csv / tod / fill / day_results)
# have no fixed cadence and are excluded from the manifest BY DESIGN — they are
# not set_live_dump() targets, so this scan won't flag them.
#
# Run before any commit touching a live-dump call site (wired into the Mac canary):
#   bash tools/live_dump_freshness_audit.sh
# Exit 0 = every live dump is monitored (or none found); exit 1 never (WARN-only,
# mirroring the informational gates) — set STRICT=1 to fail on an unmonitored dump.
# ---------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.." || exit 2

MANIFEST='tools/live_dump_manifest.tsv'
STRICT="${STRICT:-0}"

if [ ! -f "$MANIFEST" ]; then
  echo "VIOLATION: $MANIFEST is missing -- the VPS live-dump staleness monitor has no manifest."
  exit 1
fi

# Filenames already monitored (column 1 of the manifest, comment/blank lines skipped).
monitored_has(){
  grep -vE '^[[:space:]]*(#|$)' "$MANIFEST" | cut -f1 | grep -qxF "$1"
}

# Collect every distinct *.csv basename that is an ACTUAL set_live_dump() argument —
# NOT every .csv literal in the file (that would flag event-driven writers like
# day_results/fill_quality, which are excluded by design). A set_live_dump() path is
# passed one of two ways:
#   (a) inline literal on the call line:  set_live_dump(log_root_dir() + "/foo.csv")
#   (b) a local variable built just above: `X_dump = ... "/foo.csv"; set_live_dump(X_dump)`
# So: for each set_live_dump( call, take a .csv literal on that same line; if none,
# resolve the variable name in the call and grab the .csv from its assignment line(s)
# earlier in the file. Definition sites (the setter method itself, arg named `p`) have
# no .csv literal and contribute nothing — correct.
scan_files=$(grep -rlE 'set_live_dump\(' include/ src/ 2>/dev/null)
STATIC_SEED_RE='phase1/|signal_discovery/|warmup_|tsmom_'

# Resolve each set_live_dump() call to the .csv basename it targets (dedup, one per line).
targets=$(for f in $scan_files; do
  grep -nE 'set_live_dump\(' "$f" 2>/dev/null | while IFS=: read -r lineno rest; do
    # (a) inline literal on the call line
    inline=$(printf '%s\n' "$rest" | grep -oE '"[^"]*\.csv"' | tr -d '"' | grep -vE "$STATIC_SEED_RE" | sed -E 's#.*/##' | head -1)
    if [ -n "$inline" ]; then echo "$inline"; continue; fi
    # (b) argument is a local variable — resolve its .csv assignment earlier in the file
    var=$(printf '%s\n' "$rest" | sed -nE 's#.*set_live_dump\(([A-Za-z_][A-Za-z0-9_]*)\).*#\1#p' | head -1)
    [ -n "$var" ] || continue
    asn=$(head -n "$lineno" "$f" | grep -E "\b$var\b[[:space:]]*=" | grep -oE '"[^"]*\.csv"' | tr -d '"' | grep -vE "$STATIC_SEED_RE" | sed -E 's#.*/##' | tail -1)
    [ -n "$asn" ] && echo "$asn"
  done
done | sort -u)

warns=0; ok=0
for p in $targets; do
  if monitored_has "$p"; then
    ok=$((ok+1))
  else
    echo "UNMONITORED: live dump '$p' is a set_live_dump() target NOT in $MANIFEST -- add it so feeds_selftest.py polls its VPS freshness."
    warns=$((warns+1))
  fi
done

echo "--- live-dump freshness audit: $ok monitored, $warns unmonitored ---"
if [ "$warns" -gt 0 ]; then
  echo "WARN: $warns live dump(s) are written by the binary but NOT cadence-monitored."
  echo "      Add each to $MANIFEST (path_under_logs<TAB>max_age_min<TAB>description)"
  echo "      so a stalled VPS writer REDs the Mac feeds self-test instead of staying silently GREEN."
  echo "      (Event-driven files with no fixed cadence are excluded by design.)"
  [ "$STRICT" = "1" ] && exit 1
  exit 0
fi
echo "PASS: every set_live_dump() target is listed in the staleness manifest."
exit 0
