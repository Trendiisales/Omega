#!/usr/bin/env bash
# scripts/duka_xau_grid_runner.sh
# Generalized resume-safe runner for the honest_backtest_xauusd_v2 harness
# across the 623-day Dukascopy XAU corpus in outputs/duka_xauusd_daily/.
#
# Generalises scripts/duka_wide_grid_runner.sh: the harness binary, label,
# output CSV, and extra harness flags are all parameterised via env vars,
# so the same script drives the S29 session-stratified, wide-extreme, and
# inverted-signal sweeps without duplication.
#
# Usage:
#   OUT_CSV=outputs/X.csv \
#   LABEL=mylabel \
#   BIN=backtest/honest_backtest_xauusd_v2_s29 \
#   HARNESS_FLAGS="--wide --session 7-12 --latency 1" \
#     scripts/duka_xau_grid_runner.sh [CHUNK_DAYS]
#
# Defaults:
#   BIN=backtest/honest_backtest_xauusd_v2_s28
#   OUT_CSV=outputs/duka_xau_grid.csv
#   LABEL=duka_xau_grid
#   HARNESS_FLAGS="--wide --latency 1"
#   CHUNK_DAYS=200
#
# Resume-safe: skips daily files whose basename already appears in column 1
# of OUT_CSV. Run repeatedly until completion.
#
# Output diagnostics: prints processed/skipped counts, elapsed seconds,
# total rows in OUT_CSV, distinct days covered so far, and total days
# available in the corpus.

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BIN="${BIN:-backtest/honest_backtest_xauusd_v2_s28}"
DAILY_DIR="outputs/duka_xauusd_daily"
OUT_CSV="${OUT_CSV:-outputs/duka_xau_grid.csv}"
LABEL="${LABEL:-duka_xau_grid}"
HARNESS_FLAGS="${HARNESS_FLAGS:---wide --latency 1}"
CHUNK_DAYS="${1:-200}"

[[ -x "$BIN" ]] || {
  echo "ERROR: missing or non-executable $BIN" >&2
  exit 1
}
[[ -d "$DAILY_DIR" ]] || {
  echo "ERROR: missing $DAILY_DIR" >&2
  exit 1
}

# Resume-safe: build set of daily-file basenames already represented in OUT_CSV
declare -A DONE
if [[ -f "$OUT_CSV" ]]; then
  while IFS=, read -r fname _; do
    [[ "$fname" == "file" ]] && continue
    DONE["$fname"]=1
  done < "$OUT_CSV"
fi

processed=0
skipped=0
start_ts=$(date +%s)

for f in "$DAILY_DIR"/*.csv; do
  base="$(basename "$f")"
  if [[ -n "${DONE[$base]:-}" ]]; then
    skipped=$((skipped + 1))
    continue
  fi
  # Harness exit-code 1 = "no profitable config" (informational, not an error)
  # shellcheck disable=SC2086
  "$BIN" $HARNESS_FLAGS --csv-out "$OUT_CSV" --label "$LABEL" "$f" > /dev/null || true
  processed=$((processed + 1))
  if (( processed >= CHUNK_DAYS )); then
    break
  fi
done

end_ts=$(date +%s)
elapsed=$((end_ts - start_ts))
total_in_csv=$(( $(wc -l < "$OUT_CSV" 2>/dev/null || echo 1) - 1 ))
days_done=$(awk -F, 'NR>1{print $1}' "$OUT_CSV" 2>/dev/null | sort -u | wc -l)
total_days=$(ls "$DAILY_DIR"/*.csv | wc -l)

echo "RUNNER DONE  processed=$processed  skipped=$skipped  elapsed=${elapsed}s"
echo "             rows=$total_in_csv  days_done=$days_done / $total_days  out=$OUT_CSV"
