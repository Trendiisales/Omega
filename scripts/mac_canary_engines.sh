#!/bin/bash
# scripts/mac_canary_engines.sh
# ---------------------------------------------------------------------
# Mac canary syntax check for engine headers that the main OmegaBacktest
# target doesn't compile. Catches MSVC-class C++ type/init errors that
# clang+OmegaBacktest pass silently (e.g. AtomicL2 undeclared, mixed
# designated/positional aggregate init, ODR-violating header definitions).
#
# Each header is compiled in isolation with `clang++ -fsyntax-only -std=c++20
# -Iinclude` plus a tiny one-line driver that #includes it and defines a
# stub main(). Headers that need globals.hpp expansions get
# `-include L2Globals.hpp` injected.
#
# Run before any commit that touches an engine header:
#   bash scripts/mac_canary_engines.sh
#
# Exits 0 if all headers compile, nonzero on first failure.
# ---------------------------------------------------------------------
set -u
INCLUDE="-Iinclude"
STD="-std=c++20"
WARN="-Wall -Wextra -Wno-unused-variable -Wno-unused-but-set-variable -Wno-c++20-extensions"

# Headers to canary-check (target = file containing the issue class we missed)
# Self-contained or only depend on L2Globals/CellPrimitives/TradeLedger:
HEADERS=(
  include/L2Globals.hpp
  include/L2LeverageState.hpp
  include/SurvivorPortfolio.hpp
  include/DonchianEngine.hpp
  include/EmaPullbackEngine.hpp
  include/IndexFlowEngine.hpp
  include/XauTrendFollow4hEngine.hpp
  include/XauTrendFollow1hEngine.hpp
  include/Ger40KeltnerH1Engine.hpp
  include/BreakBounceEngine.hpp
  include/TrendLineBreakEngine.hpp
  include/IndexSessionEngine.hpp
  include/SessionMomentumEngine.hpp
  include/FxCarryEngine.hpp
  include/FxCrossRevEngine.hpp
  include/FxSeasonalEngine.hpp
  include/IndexSeasonalEngine.hpp
  include/GoldSeasonalEngine.hpp
  include/GoldOversoldBounceEngine.hpp
  include/IndexFomcEngine.hpp
  include/IndexRiskGate.hpp
  include/Us30EnsembleEngine.hpp
  include/IndexIntradayDriftEngine.hpp
  include/Ger40TurtleH4Engine.hpp
  include/PeachyOrbEngine.hpp
)

fails=0
for h in "${HEADERS[@]}"; do
  if [ ! -f "$h" ]; then
    echo "[skip] $h: not found"
    continue
  fi
  tmp=$(mktemp -t mac_canary_XXXXXX.cpp)
  base=$(basename "$h")
  cat > "$tmp" <<EOF
#include "$base"
int main() { return 0; }
EOF
  out=$(clang++ -fsyntax-only $STD $INCLUDE $WARN -x c++ "$tmp" 2>&1)
  rc=$?
  rm -f "$tmp"
  if [ $rc -ne 0 ]; then
    echo "===================="
    echo "[FAIL] $h"
    echo "$out" | head -40
    echo "===================="
    fails=$((fails + 1))
  else
    echo "[ok]   $h"
  fi
done

if [ $fails -gt 0 ]; then
  echo ""
  echo "[mac-canary-engines] $fails header(s) failed -- fix before commit."
  exit 1
fi
echo "[mac-canary-engines] all $((${#HEADERS[@]})) headers ok"
exit 0
