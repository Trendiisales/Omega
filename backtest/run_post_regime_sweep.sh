#!/bin/bash
# ============================================================================
# run_post_regime_sweep.sh -- 2026-04-29 LATE
# ----------------------------------------------------------------------------
# Re-run the existing sweep harness on the post-Apr-2025 portion of the
# Dukascopy tick corpus.  This is the calibration window for the engines
# that did not get retuned in this session because their existing sweep
# CSVs were calibrated against the full 2024-03..2026-04 corpus (which
# includes the pre-regime era that no longer reflects live execution
# costs).
#
# Output: sweep_post_regime_<timestamp>/sweep_<engine>.csv per engine.
# Pick winners using the q-quartile columns or a separate ranking pass.
# ============================================================================
set -euo pipefail

REPO=${REPO:-/Users/jo/omega_repo}
TICK_DIR=${TICK_DIR:-/Users/jo/Tick/duka_ticks}
TICK_FULL="${TICK_DIR}/XAUUSD_2024-03_2026-04_combined.csv"
TICK_POST="${TICK_DIR}/XAUUSD_post_2025-04_combined.csv"
CUT_MS=1743465600000   # 2025-04-01 00:00:00 UTC

cd "$REPO"

if [ ! -f "$TICK_FULL" ]; then
    echo "ERROR: full tick CSV not found at $TICK_FULL" >&2
    echo "Mount /Users/jo/Tick/ before running this script." >&2
    exit 2
fi

if [ ! -f "$TICK_POST" ]; then
    echo "[+] Filtering $TICK_FULL to post-Apr-2025 -> $TICK_POST"
    head -1 "$TICK_FULL" > "$TICK_POST"
    awk -F, -v c=$CUT_MS 'NR>1 && $1 >= c' "$TICK_FULL" >> "$TICK_POST"
    echo "[+] $(wc -l < "$TICK_POST") rows in post-regime CSV (incl. header)"
fi

OUT="sweep_post_regime_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"

cd backtest
echo "[+] Building duka_sweep..."
g++ -O2 -std=c++17 -I ../include -o duka_sweep duka_sweep.cpp
echo "[+] Running sweep on $TICK_POST -> ../$OUT/"
./duka_sweep "$TICK_POST" --out "../$OUT/"

echo
echo "[OK] sweep complete.  Outputs in $REPO/$OUT/"
echo "Next step: rank each sweep_<engine>.csv by total_pnl + stability,"
echo "          apply winning params to the corresponding engine's"
echo "          *BaseParams struct (CRTP version) following the"
echo "          AsianRange pattern in S44 (2026-04-29)."
