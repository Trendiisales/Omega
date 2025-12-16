#!/usr/bin/env bash
# =============================================================================
# check_regression.sh - CI Regression Guard
# =============================================================================
# Fail build if banned patterns appear.
# Run before every commit / CI build.
# =============================================================================

set -e

echo "=== CHIMERA REGRESSION GUARD ==="

FAIL=0

# Banned tick types
if grep -rn "UnifiedTick" --include="*.hpp" --include="*.cpp" . 2>/dev/null; then
    echo "❌ BANNED: UnifiedTick found"
    FAIL=1
fi

if grep -rn "TickFull" --include="*.hpp" --include="*.cpp" . 2>/dev/null; then
    echo "❌ BANNED: TickFull found"
    FAIL=1
fi

# Banned engine patterns
if grep -rn "MotherEngine" --include="*.hpp" --include="*.cpp" . 2>/dev/null; then
    echo "❌ BANNED: MotherEngine found"
    FAIL=1
fi

# Banned concurrency primitives on hot path (check main engine files)
if grep -n "std::mutex" engine/*.hpp 2>/dev/null; then
    echo "❌ BANNED: std::mutex in engine headers"
    FAIL=1
fi

if grep -n "std::condition_variable" engine/*.hpp 2>/dev/null; then
    echo "❌ BANNED: std::condition_variable in engine headers"
    FAIL=1
fi

# Check for heap allocation in hot path headers
if grep -n "new " engine/*.hpp strategy/*/*.hpp 2>/dev/null | grep -v "// COLD"; then
    echo "⚠️  WARNING: 'new' found in engine/strategy headers - verify not on hot path"
fi

if [ $FAIL -eq 1 ]; then
    echo ""
    echo "❌ REGRESSION CHECK FAILED"
    exit 1
fi

echo "✅ All regression checks passed"
exit 0
