#!/bin/bash
# ============================================================
#  Build OmegaBacktest natively on Mac
# ============================================================
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/backtest/OmegaBacktest_mac"

echo "Building OmegaBacktest for Mac..."
echo "Repo: $REPO"

# Always remove old binary so stale binary never masks a failed build
rm -f "$OUT"

clang++ -O3 -std=c++20 \
    -o "$OUT" \
    "$REPO/backtest/OmegaBacktest.cpp" \
    "$REPO/src/SymbolConfig.cpp" \
    -I"$REPO" \
    -I"$REPO/include" \
    -I"$REPO/src" \
    -I"$REPO/backtest" \
    -include "$REPO/backtest/OmegaTimeShim.hpp" \
    -include "$REPO/backtest/mac_compat.hpp" \
    -DOMEGA_BACKTEST \
    -Wno-unused-parameter \
    -Wno-unused-function \
    -Wno-reorder \
    -Wno-unknown-pragmas \
    -Wno-deprecated-declarations \
    -Wno-non-pod-varargs \
    -pthread

if [ ! -f "$OUT" ]; then
    echo ""
    echo "*** BUILD FAILED -- binary not created ***"
    exit 1
fi
echo ""
echo "Done: $OUT  ($(date))"
echo ""
echo "Usage:"
echo "  $OUT <ticks.csv> --engine flow --warmup 10000 --report results/flow.csv --quiet"
echo "  $OUT <ticks.csv> --engine flow,breakout --warmup 10000"
echo ""
echo "Engines: gold  flow  latency  cross  breakout  stoprun"
