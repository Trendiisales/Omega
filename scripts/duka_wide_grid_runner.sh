#!/usr/bin/env bash
# scripts/duka_wide_grid_runner.sh
# S28 / handoff S27 §4.2 — wide-grid sweep on full Dukascopy 2yr XAU corpus.
#
# Invokes backtest/honest_backtest_xauusd_v2_s28 --wide --latency 1 ungated
# once per daily CSV in outputs/duka_xauusd_daily/. Appends 104 rows
# (52 configs × 2 fill models) per day to OUT_CSV.
#
# Resume-safe: only processes files whose basename does NOT already appear
# in the existing OUT_CSV. Run multiple times until done.
#
# Usage:
#   scripts/duka_wide_grid_runner.sh [CHUNK_DAYS]
# Default CHUNK_DAYS=200 (fits ~40s on this machine).
#
# Output: outputs/duka_wide_grid.csv

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BIN="backtest/honest_backtest_xauusd_v2_s28"
DAILY_DIR="outputs/duka_xauusd_daily"
OUT_CSV="outputs/duka_wide_grid.csv"
LABEL="duka_wide_ungated"
CHUNK_DAYS="${1:-200}"

[[ -x "$BIN"          ]] || { echo "ERROR: missing $BIN. Build with: g++ -std=c++17 -O2 backtest/honest_backtest_xauusd_v2.cpp -o $BIN" >&2; exit 1; }
[[ -d "$DAILY_DIR"    ]] || { echo "ERROR: missing $DAILY_DIR" >&2; exit 1; }

# Build the resume set: which files are already represented in OUT_CSV?
declare -A DONE
if [[ -f "$OUT_CSV" ]]; then
  # Column 1 of the CSV is the tick-file basename.
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
    skipped=$((skipped+1))
    continue
  fi
  # Harness returns 1 if no honest-fill config is profitable. We don't care —
  # we just want the CSV row(s) for every day. Allow non-zero exit.
  "$BIN" --wide --latency 1 --csv-out "$OUT_CSV" --label "$LABEL" "$f" > /dev/null || true
  processed=$((processed+1))
  if (( processed >= CHUNK_DAYS )); then
    break
  fi
done

end_ts=$(date +%s)
elapsed=$((end_ts - start_ts))
total_in_csv=$(( $(wc -l < "$OUT_CSV" 2>/dev/null || echo 1) - 1 ))
days_in_csv=$(( total_in_csv / 104 ))
total_days=$(ls "$DAILY_DIR"/*.csv | wc -l)

echo "RUNNER DONE  processed=$processed  skipped=$skipped  elapsed=${elapsed}s"
echo "             total_csv_rows=$total_in_csv  days_in_csv=$days_in_csv / $total_days"
