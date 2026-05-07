#!/usr/bin/env bash
# Cost-sensitivity walk-forward — same data + spec, halved spread + slippage.
# If neither strategy clears the gate even at tight broker costs, shelve both.
# Generated 2026-05-03.

set -euo pipefail

cd "$(dirname "$0")"

CSV="/Users/jo/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv"

if [[ ! -f "$CSV" ]]; then
    echo "ERROR: 2yr CSV not found at $CSV" >&2
    exit 1
fi

echo "Cost-sensitivity run: --spread-pips 0.2 --slippage-pips 0.1"
echo "(Default was 0.5 + 0.3; this halves round-trip cost.)"
echo "Expected runtime: 10-30 minutes."
echo "----------------------------------------"

python3 wf_compare.py \
    --bars "$CSV" \
    --samples 100 \
    --train-months 6 \
    --test-months 3 \
    --step-months 3 \
    --spread-pips 0.2 \
    --slippage-pips 0.1

echo "----------------------------------------"
echo "If both still fail: write postmortem, shelve both, ship FVG only."
