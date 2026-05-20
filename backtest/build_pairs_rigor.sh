#!/bin/bash
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/backtest/pairs_rigor_cpp"
rm -f "$OUT"
clang++ -O3 -std=c++20 \
    -o "$OUT" \
    "$REPO/backtest/pairs_rigor_cpp.cpp" \
    -I"$REPO" -I"$REPO/include" -I"$REPO/backtest" \
    -include "$REPO/backtest/OmegaTimeShim.hpp" \
    -include "$REPO/backtest/mac_compat.hpp" \
    -DOMEGA_BACKTEST \
    -Wno-unused-parameter -Wno-unused-function -Wno-reorder \
    -Wno-unknown-pragmas -Wno-deprecated-declarations
echo "built: $OUT"
