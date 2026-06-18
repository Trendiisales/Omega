#!/bin/bash
# backtest/certify_or_die.sh — MANDATORY data gate. Run a tick file through the
# integrity gate + check it against the certified registry BEFORE any backtest.
# Hard-exits 1 if the file is not certified-clean, so dirty data physically
# cannot reach a backtest. This is the ENFORCEMENT the advisory gate lacked.
#
# Usage:
#   backtest/certify_or_die.sh <file.csv> [lo] [hi]      # certify one file
#   backtest/certify_or_die.sh --run <file.csv> -- <cmd...>   # certify then exec cmd
#
# Registry: backtest/CERTIFIED_DATA.tsv (instrument<TAB>path<TAB>rows<TAB>span<TAB>date).
# A file already in the registry whose size is unchanged passes instantly;
# otherwise the gate runs and, on pass, the file is recorded.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
GATE="$HERE/data_integrity_gate.py"
REG="$HERE/CERTIFIED_DATA.tsv"

run_mode=0; runcmd=()
if [ "${1:-}" = "--run" ]; then run_mode=1; shift; fi
FILE="${1:-}"; shift || true
LO=""; HI=""
if [ "$run_mode" = 1 ]; then
  # collect optional lo/hi until '--', then the command
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    if [ -z "$LO" ]; then LO="$1"; elif [ -z "$HI" ]; then HI="$1"; fi; shift
  done
  [ "${1:-}" = "--" ] && shift
  runcmd=("$@")
else
  LO="${1:-}"; HI="${2:-}"
fi

[ -z "$FILE" ] && { echo "usage: certify_or_die.sh <file.csv> [lo] [hi]"; exit 2; }
[ -f "$FILE" ] || { echo "CERTIFY FAIL: no such file: $FILE"; exit 1; }

abspath="$(cd "$(dirname "$FILE")" && pwd)/$(basename "$FILE")"
size=$(stat -f%z "$abspath" 2>/dev/null || stat -c%s "$abspath" 2>/dev/null)

# already certified + unchanged size -> instant pass
if [ -f "$REG" ] && grep -qF "	$abspath	" "$REG"; then
  regsize=$(grep -F "	$abspath	" "$REG" | head -1 | cut -f6)
  if [ "$regsize" = "$size" ]; then
    echo "CERTIFIED (registry): $abspath"
    [ "$run_mode" = 1 ] && exec "${runcmd[@]}"
    exit 0
  fi
  echo "NOTE: $abspath size changed since certification — re-running gate."
fi

echo "Running integrity gate on $abspath ..."
out=$(python3 "$GATE" "$abspath" ${LO:+$LO} ${HI:+$HI} 2>/dev/null); rc=$?
echo "$out"
if [ "$rc" -ne 0 ]; then
  echo "CERTIFY FAIL: $abspath REJECTED by integrity gate — NOT usable for backtest."
  exit 1
fi
# record/refresh registry row
rows=$(echo "$out" | grep -oE "rows=[0-9,]+" | head -1 | tr -d 'rows=,')
span=$(echo "$out" | grep -oE "range: [0-9-]+ \.\. [0-9-]+" | head -1 | sed 's/range: //')
inst=$(basename "$abspath" | grep -oiE "xau|nsx|nas|us500|ustec|dj30|ger40|eur|gbp|gold" | head -1 | tr a-z A-Z)
[ -f "$REG" ] || printf "instrument\tpath\trows\tspan\tcertified\tsize_bytes\n" > "$REG"
# drop any stale row for this path, append fresh
grep -vF "	$abspath	" "$REG" > "$REG.tmp" 2>/dev/null || true; mv "$REG.tmp" "$REG"
printf "%s\t%s\t%s\t%s\t%s\t%s\n" "${inst:-?}" "$abspath" "${rows:-?}" "${span:-?}" "$(date +%Y-%m-%d)" "$size" >> "$REG"
echo "CERTIFIED + recorded: $abspath"
[ "$run_mode" = 1 ] && exec "${runcmd[@]}"
exit 0
