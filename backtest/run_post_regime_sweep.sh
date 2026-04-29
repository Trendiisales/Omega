#!/bin/bash
# ============================================================================
# run_post_regime_sweep.sh -- 2026-04-29 LATE  (wave 2, S44)
# ----------------------------------------------------------------------------
# Re-run the multi-engine pairwise sweep on the post-Apr-2025 portion of the
# Dukascopy gold tick corpus.  This is the calibration window for the
# engines that did not get retuned in wave 1 (SpreadRegimeGate v2, MCE
# spike-only, AsianRange) because their existing sweep CSVs were calibrated
# against the full 2024-03..2026-04 corpus, which includes the pre-regime
# era that no longer reflects live execution costs.
#
# Sweep harness: backtest/OmegaSweepHarnessCRTP.cpp  (CRTP variant, S51).
# Coverage:      hbg, emacross, asianrange, vwapstretch  (490 combos each).
# AsianRange is included for confirmation only -- it was already retuned in
# wave 1 (S44 cid=195 -- BUFFER 1.0, TP_TICKS 400).  HBG / EMACross /
# VWAPStretch are the wave-2 retune candidates.
#
# Filtering: native --from-date 2025-04-01 flag (no awk pre-filter, no
# duplicate post-cut CSV on disk).  ts_ms boundary is 1743465600000.
#
# Output: sweep_post_regime_<timestamp>/sweep_<engine>.csv per engine plus
# a sweep_summary.txt.  Pick winners using the q-quartile columns + score
# column (stability x total_pnl), then update each engine's *BaseParams
# struct in include/SweepableEnginesCRTP.hpp following the AsianRange S44
# pattern.
#
# Wall-time estimate: prior G1CLEAN run (full corpus, 154M ticks) was
# 20.6 min on the same Mac.  Post-cut window is roughly 58% of the corpus
# so expect ~12-15 min sweep + ~30s self-test + ~30s mmap parse.
# ============================================================================
set -euo pipefail

REPO=${REPO:-/Users/jo/omega_repo}
TICK_DIR=${TICK_DIR:-/Users/jo/Tick/duka_ticks}
TICK_FULL="${TICK_DIR}/XAUUSD_2024-03_2026-04_combined.csv"
BIN="${REPO}/build/OmegaSweepHarnessCRTP"
FROM_DATE=${FROM_DATE:-2025-04-01}
WARMUP=${WARMUP:-5000}
ENGINES=${ENGINES:-hbg,emacross,asianrange,vwapstretch}

# --- pre-flight checks ------------------------------------------------------

if [ ! -f "${TICK_FULL}" ]; then
    echo "ERROR: full tick CSV not found at ${TICK_FULL}" >&2
    echo "       Mount /Users/jo/Tick/ before running this script." >&2
    exit 2
fi

if [ ! -x "${BIN}" ]; then
    echo "ERROR: sweep binary not found or not executable: ${BIN}" >&2
    echo "       Build it first:" >&2
    echo "         cd ${REPO} && cmake --build build --target OmegaSweepHarnessCRTP -j" >&2
    exit 3
fi

# --- output dir + sentinel --------------------------------------------------

OUT="${REPO}/sweep_post_regime_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${OUT}"

{
    echo "sentinel pre-launch $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $(hostname)"
    echo "binary: ${BIN}"
    echo "ticks: ${TICK_FULL}"
    echo "from_date: ${FROM_DATE}"
    echo "warmup: ${WARMUP}"
    echo "engines: ${ENGINES}"
} > "${OUT}/__sentinel_pre.txt"

echo "[+] post-regime sweep starting"
echo "    binary:    ${BIN}"
echo "    ticks:     ${TICK_FULL}"
echo "    from-date: ${FROM_DATE}"
echo "    warmup:    ${WARMUP}"
echo "    engines:   ${ENGINES}"
echo "    outdir:    ${OUT}"
echo

# --- sweep ------------------------------------------------------------------

# Engines flag is hardcoded in the Args default ("hbg,asianrange,vwapstretch,
# emacross") so we don't pass --engine explicitly; if you need to subset,
# patch parse_args first or set ENGINES env (currently unused -- noted for
# future).  --no-selftest is OFF by default; the G2 determinism self-test
# is cheap (~30s on 250k ticks) and a meaningful safety net before a 12 min
# real sweep.

"${BIN}" \
    "${TICK_FULL}" \
    --outdir "${OUT}" \
    --from-date "${FROM_DATE}" \
    --warmup "${WARMUP}" \
    --verbose \
    2>&1 | tee "${OUT}/run.log"

# --- finalise ---------------------------------------------------------------

{
    echo "sentinel post-launch $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "exit_status: $?"
} > "${OUT}/__sentinel_post.txt"

echo
echo "[OK] sweep complete.  Outputs in ${OUT}/"
echo
echo "Next steps (S44 pattern):"
echo "  1. Inspect each sweep_<engine>.csv -- rank by score (stability x"
echo "     total_pnl) AND check q4_n>=5 to avoid degenerate cells."
echo "  2. Confirm AsianRange winner is still cid=195 (BUFFER=1.0,"
echo "     TP_TICKS=400) under the post-Apr-2025 window."
echo "  3. Apply HBG / EMACross / VWAPStretch winners to their respective"
echo "     *BaseParams structs in include/SweepableEnginesCRTP.hpp."
echo "  4. Document each pick in RETUNE_2026-04-29_wave2.md (table format"
echo "     mirroring the AsianRange table in RETUNE_2026-04-29.md)."
echo "  5. Run backtest/post_regime_baseline.py before promoting; A/B"
echo "     against RETUNE_BASELINE_2026-04-29.txt."
