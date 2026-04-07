#!/bin/bash
# ============================================================
#  Build standalone GoldFlow backtest tools on Mac/Linux
#  These tools are self-contained (no Omega headers needed)
#
#  Usage:
#    cd backtest
#    chmod +x build_goldflow_tools.sh
#    ./build_goldflow_tools.sh
#
#  Tools produced:
#    goldflow_diag   -- per-trade exit reason breakdown + CSV output
#    goldflow_sweep  -- parameter sweep across TP/SL/impulse/time
#
#  Running:
#    ./goldflow_diag  ticks.csv [trades.csv]
#    ./goldflow_sweep ticks.csv [results.csv]
#
#  Sorting sweep results (requires python3):
#    python3 -c "
#      import csv; rows=list(csv.DictReader(open('results.csv')))
#      rows.sort(key=lambda r: float(r['pnl_usd']), reverse=True)
#      [print(r) for r in rows[:20]]
#    "
# ============================================================

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "======================================================"
echo "  Building GoldFlow backtest tools"
echo "  Dir: $SCRIPT_DIR"
echo "======================================================"

# Detect compiler
CXX="g++"
if command -v g++ &>/dev/null; then
    CXX="g++"
elif command -v clang++ &>/dev/null; then
    CXX="clang++"
else
    echo "ERROR: no C++ compiler found (g++ or clang++)"
    exit 1
fi
echo "  Compiler: $CXX"
echo ""

BUILD_FLAGS="-O3 -std=c++17 -Wall -Wextra -Wno-unused-parameter"

# ── goldflow_diag ─────────────────────────────────────────
echo "[1/2] goldflow_diag..."
rm -f goldflow_diag
$CXX $BUILD_FLAGS -o goldflow_diag goldflow_diag.cpp
if [ ! -f goldflow_diag ]; then
    echo "*** FAILED: goldflow_diag not created"
    exit 1
fi
echo "      OK: ./goldflow_diag"

# ── goldflow_sweep ────────────────────────────────────────
echo "[2/2] goldflow_sweep..."
rm -f goldflow_sweep
$CXX $BUILD_FLAGS -o goldflow_sweep goldflow_sweep.cpp
if [ ! -f goldflow_sweep ]; then
    echo "*** FAILED: goldflow_sweep not created"
    exit 1
fi
echo "      OK: ./goldflow_sweep"

echo ""
echo "======================================================"
echo "  BUILD COMPLETE"
echo "======================================================"
echo ""
echo "  DIAGNOSTIC (exit reason breakdown):"
echo "    ./goldflow_diag ticks.csv"
echo "    ./goldflow_diag ticks.csv trades_out.csv  # also writes per-trade CSV"
echo ""
echo "  SWEEP (parameter grid search):"
echo "    ./goldflow_sweep ticks.csv"
echo "    ./goldflow_sweep ticks.csv results.csv    # writes all combos to CSV"
echo ""
echo "  After sweep, sort by best PnL:"
echo "    sort -t',' -k9 -rn results.csv | head -20"
echo ""
echo "  Key columns in sweep CSV:"
echo "    tp,sl,impulse_min,time_limit_s,pullback,"
echo "    n_total,wins,wr_pct,pnl_usd,"
echo "    n_tp,pnl_tp_usd, n_sl,pnl_sl_usd,"
echo "    n_adverse,pnl_adverse_usd, n_time,pnl_time_usd"
echo ""
