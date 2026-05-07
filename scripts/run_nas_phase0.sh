#!/usr/bin/env bash
#
# run_nas_phase0.sh
# =================
# One-shot wrapper for the Nasdaq (HistData NSXUSD) Phase 0 FVG sniff
# test. Two steps:
#   1) Build a UTC-shifted combined Dukascopy-format CSV from the
#      monthly HistData ASCII tick files Jo downloaded into ~/Tick/Nas/
#      (EST -> UTC +5h). Same prep script we used for USDJPY; the
#      regex now also accepts " (N)" duplicate-folder fallbacks for any
#      month that doesn't have a plain-named version on disk.
#   2) Run scripts/usdjpy_xauusd_fvg_signal_test.py at 15min on the
#      combined file.
#
# Usage (from repo root, on Jo's Mac):
#     bash scripts/run_nas_phase0.sh
#
# Re-running: the prep step is skipped if the combined CSV already
# exists. Set FORCE_PREP=1 to force a rebuild:
#     FORCE_PREP=1 bash scripts/run_nas_phase0.sh
#
# All variables you might want to tweak are at the top.
#
# (Earlier versions of this wrapper drove a Dukascopy download +
# combine pipeline. That path was replaced once Jo downloaded the
# HistData NSXUSD monthly zips directly into ~/Tick/Nas/, which is
# faster and avoids a multi-hour Dukascopy fetch.)

set -euo pipefail

# ----------------------------------------------------------------------
# Knobs
# ----------------------------------------------------------------------
SYMBOL="NAS"             # used in the FVG test report header / cache filename
TF="15min"

# Sniff-test window. Jo has 16 months of HistData NSXUSD downloaded
# (Jan 2025 - Apr 2026), so we use the full range for max statistical
# power. This breaks strict parity with the XAUUSD/USDJPY 6-month
# windows but the headline gates and component-importance ranking
# should hold up to that asymmetry; a 16-month sample on Nasdaq is
# more informative than a 6-month subset.
START_DATE="2025-01-01"
END_DATE="2026-05-01"

# Months to ingest (--end is exclusive in the FVG test, so we need
# Jan 2025 .. Apr 2026 inclusive on the prep side).
START_MONTH="2025-01"
END_MONTH="2026-04"

# Source layout (HistData NSXUSD monthly extracted folders)
SRC_ROOT="${HOME}/Tick/Nas"
SRC_PATTERN="HISTDATA_COM_ASCII_NSXUSD_T*"

# Combined-tick output (lives next to the source data)
COMBINED_CSV="${HOME}/Tick/Nas/NSXUSD_2025-01_2026-04_combined_UTC.csv"

# FVG sniff-test output dir (under the repo, mountable for follow-up review)
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
