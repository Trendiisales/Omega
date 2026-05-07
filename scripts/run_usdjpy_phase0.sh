#!/usr/bin/env bash
#
# run_usdjpy_phase0.sh
# ====================
# One-shot wrapper for the USDJPY Phase 0 FVG sniff test:
#   1) Build a UTC-shifted combined Dukascopy-format CSV from the
#      monthly HistData ASCII tick files (EST -> UTC +5h).
#   2) Run scripts/usdjpy_xauusd_fvg_signal_test.py at 15min on it.
#
# Usage (from repo root, on Jo's Mac):
#     bash scripts/run_usdjpy_phase0.sh
#
# Re-running: the prep step is skipped if the combined CSV already
# exists. Set FORCE_PREP=1 to force a rebuild:
#     FORCE_PREP=1 bash scripts/run_usdjpy_phase0.sh
#
# All variables you might want to tweak are at the top.

set -euo pipefail

# ----------------------------------------------------------------------
# Knobs
# ----------------------------------------------------------------------
SYMBOL="USDJPY"
TF="15min"

# Sniff-test window (matches XAUUSD baseline for parity)
START_DATE="2025-09-01"
END_DATE="2026-03-01"

# Months to ingest (HistData files are monthly; --end is exclusive in
# the FVG test, so we need months Sep 2025 .. Feb 2026 inclusive).
START_MONTH="2025-09"
END_MONTH="2026-02"

# Source layout
SRC_ROOT="${HOME}/Tick/USDJPY"
SRC_PATTERN="HISTDATA_COM_ASCII_USDJPY_T*"

# Combined-tick output (lives next to the source data)
COMBINED_CSV="${HOME}/Tick/USDJPY/USDJPY_2025-09_2026-02_combined_UTC.csv"

# FVG sniff-test output dir (under the repo, so the sandbox can read it)
OUT_DIR="$(pwd)/fvg_phase0/${SYMBOL}_${TF}"
LOG_FILE="${OUT_DIR}/run.log"

# Hours to add to each EST timestamp to get UTC. HistData publishes
# EST as fixed UTC-5 (no DST), per their convention.
SHIFT_HOURS=5

# Pandas chunksize for the prep step
CHUNKSIZE=2000000

# ----------------------------------------------------------------------
# Pre-flight
# ----------------------------------------------------------------------
echo "[wrap] cwd: $(pwd)"
echo "[wrap] python: $(command -v python3 || true)"
python3 --version

if [[ ! -d "${SRC_ROOT}" ]]; then
    echo "[fatal] SRC_ROOT not a directory: ${SRC_ROOT}" >&2
    exit 1
fi

if [[ ! -f "scripts/prep_histdata_est_to_dukascopy_utc.py" ]]; then
    echo "[fatal] prep script not found at scripts/prep_histdata_est_to_dukascopy_utc.py" >&2
    echo "        run this wrapper from the omega_repo root." >&2
    exit 1
fi

if [[ ! -f "scripts/usdjpy_xauusd_fvg_signal_test.py" ]]; then
    echo "[fatal] FVG test script not found at scripts/usdjpy_xauusd_fvg_signal_test.py" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

# ----------------------------------------------------------------------
# Step 1: prep (combine + EST->UTC shift)
# ----------------------------------------------------------------------
if [[ -f "${COMBINED_CSV}" && "${FORCE_PREP:-0}" != "1" ]]; then
    SIZE_GB=$(du -h "${COMBINED_CSV}" | cut -f1)
    echo "[wrap] combined CSV already exists (${SIZE_GB}); skipping prep."
    echo "[wrap] re-run with FORCE_PREP=1 to rebuild it."
else
    echo "[wrap] running prep step -> ${COMBINED_CSV}"
    python3 -u scripts/prep_histdata_est_to_dukascopy_utc.py \
        --src-root    "${SRC_ROOT}" \
        --pattern     "${SRC_PATTERN}" \
        --start       "${START_MONTH}" \
        --end         "${END_MONTH}" \
        --out         "${COMBINED_CSV}" \
        --shift-hours "${SHIFT_HOURS}" \
        --chunksize   "${CHUNKSIZE}"
fi

# ----------------------------------------------------------------------
# Step 2: FVG sniff test
# ----------------------------------------------------------------------
echo "[wrap] running FVG sniff test -> ${OUT_DIR}"
python3 -u scripts/usdjpy_xauusd_fvg_signal_test.py \
    --symbol   "${SYMBOL}" \
    --tick-csv "${COMBINED_CSV}" \
    --out-dir  "${OUT_DIR}" \
    --start    "${START_DATE}" \
    --end      "${END_DATE}" \
    --tf       "${TF}" 2>&1 | tee "${LOG_FILE}"

echo "[wrap] done. outputs in: ${OUT_DIR}"
echo "[wrap] log:               ${LOG_FILE}"
