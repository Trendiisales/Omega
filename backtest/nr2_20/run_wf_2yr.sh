#!/usr/bin/env bash
# Walk-forward compare on the 2yr XAUUSD 15m CSV.
# Spec defaults: 6m train / 3m test / 3m step, 100 random samples.
# Generated 2026-05-03.

set -euo pipefail

cd "$(dirname "$0")"

CSV="/Users/jo/omega_repo/fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv"

if [[ ! -f "$CSV" ]]; then
    echo "ERROR: 2yr CSV not found at $CSV" >&2
    exit 1
fi

echo "Running wf_compare.py against $CSV ..."
echo "Settings: --samples 100 --train-months 6 --test-months 3 --step-months 3"
echo "Expected runtime: 10-30 minutes."
echo "----------------------------------------"

python3 wf_compare.py \
    --bars "$CSV" \
    --samples 100 \
    --train-months 6 \
    --test-months 3 \
    --step-months 3

echo "----------------------------------------"
echo "Done. Outputs:"
echo "  wf_compare_nr2_20.csv"
echo "  wf_compare_vwapc.csv"
echo "  wf_compare_correlation.csv"
