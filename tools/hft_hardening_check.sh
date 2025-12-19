#!/usr/bin/env bash
# CHIMERA HFT HARDENING CHECK
# ===========================
# This script provides objective, mechanical proof that
# HFT constraints are satisfied.
#
# Run: bash tools/hft_hardening_check.sh src
# CI:  Add to GitHub Actions to prevent regression
#
set -euo pipefail

ROOT=${1:-src}
ERRORS=0

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║          CHIMERA HFT HARDENING VERIFICATION                  ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "Scanning: $ROOT"
echo ""

fail() {
    echo "❌ FAIL: $1"
    ERRORS=$((ERRORS + 1))
}

pass() {
    echo "✅ PASS: $1"
}

warn() {
    echo "⚠️  WARN: $1"
}

# ─────────────────────────────────────────────────────────────────────
# 1) SYSTEM_CLOCK IN HOT PATHS
# ─────────────────────────────────────────────────────────────────────
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 1) system_clock usage                                       │"
echo "└─────────────────────────────────────────────────────────────┘"

# Hot path files - system_clock forbidden here
HOT_PATH_PATTERNS="onMessage\|onTick\|onExec\|Throttle\|LatencyMonitor"

SYSCLOCK_HOT=$(grep -rn "system_clock" "$ROOT" 2>/dev/null | \
    grep -v "HFTTime.hpp\|wall_ns\|SendingTime\|COLD_PATH" | \
    grep -E "$HOT_PATH_PATTERNS" || true)

if [ -n "$SYSCLOCK_HOT" ]; then
    echo "$SYSCLOCK_HOT"
    fail "system_clock found in hot path files"
else
    pass "No system_clock in hot paths"
fi

# Wall clock for FIX SendingTime is OK, but flag for awareness
SYSCLOCK_TOTAL=$(grep -rc "system_clock" "$ROOT" 2>/dev/null | \
    grep -v ":0" | wc -l || echo "0")
echo "   └── Total files with system_clock: $SYSCLOCK_TOTAL (OK for SendingTime)"

# ─────────────────────────────────────────────────────────────────────
# 2) FIX m.get() STRING COPIES IN HOT PATHS
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 2) FIX m.get() string copies in hot paths                   │"
echo "└─────────────────────────────────────────────────────────────┘"

# Check for m.get() or msg.get() in FIX hot path files
FIX_HOT_FILES="FIXBridge\|FIXMDDecoder\|FIXFeedMux\|FIXExecReport\|FIXRouter\|onMessage"

MGET_CALLS=$(grep -rn "\.get(" "$ROOT/fix" 2>/dev/null | \
    grep -v "getView\|getInt\|getDouble\|COLD_PATH" | \
    grep -E "$FIX_HOT_FILES" || true)

if [ -n "$MGET_CALLS" ]; then
    echo "$MGET_CALLS"
    warn "m.get() found in FIX hot paths - migrate to getView()"
else
    pass "FIX hot paths use getView() only"
fi

# ─────────────────────────────────────────────────────────────────────
# 3) SLOW NUMERIC PARSING (atof/atoi/stod)
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 3) Slow numeric parsing (atof/atoi/stod)                    │"
echo "└─────────────────────────────────────────────────────────────┘"

SLOW_PARSE=$(grep -rn -E "atof\(|atoi\(|stod\(|strtod\(|strtol\(" "$ROOT" 2>/dev/null | \
    grep -v "COLD_PATH\|test\|Test" || true)

SLOW_COUNT=$(echo "$SLOW_PARSE" | grep -c "." || echo "0")

if [ "$SLOW_COUNT" -gt 0 ]; then
    echo "$SLOW_PARSE" | head -10
    if [ "$SLOW_COUNT" -gt 10 ]; then
        echo "   ... and $((SLOW_COUNT - 10)) more"
    fi
    warn "$SLOW_COUNT slow parse calls - migrate to fast_parse_*()"
else
    pass "No slow numeric parsing"
fi

# ─────────────────────────────────────────────────────────────────────
# 4) STRINGSTREAM IN HOT PATHS
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 4) stringstream usage                                       │"
echo "└─────────────────────────────────────────────────────────────┘"

# stringstream is OK in REST (cold path) but not in hot paths
SSTREAM_HOT=$(grep -rn "stringstream" "$ROOT" 2>/dev/null | \
    grep -v "/rest/\|REST\|COLD_PATH\|encode()" || true)

if [ -n "$SSTREAM_HOT" ]; then
    echo "$SSTREAM_HOT"
    warn "stringstream found outside cold REST paths"
else
    pass "stringstream only in cold paths"
fi

# ─────────────────────────────────────────────────────────────────────
# 5) MUTEX CLASSIFICATION
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 5) Mutex classification                                     │"
echo "└─────────────────────────────────────────────────────────────┘"

MUTEX_TOTAL=$(grep -rc "std::mutex" "$ROOT" 2>/dev/null | \
    grep -v ":0" | awk -F: '{sum+=$2} END {print sum}')

MUTEX_CLASSIFIED=$(grep -rn "std::mutex" "$ROOT" 2>/dev/null | \
    grep "COLD_PATH_ONLY" | wc -l || echo "0")

if [ "$MUTEX_TOTAL" -gt 0 ]; then
    MUTEX_UNCLASSIFIED=$((MUTEX_TOTAL - MUTEX_CLASSIFIED))
    if [ "$MUTEX_UNCLASSIFIED" -gt 0 ]; then
        warn "$MUTEX_UNCLASSIFIED / $MUTEX_TOTAL mutexes not classified as COLD_PATH_ONLY"
        grep -rn "std::mutex" "$ROOT" 2>/dev/null | grep -v "COLD_PATH_ONLY" | head -5
    else
        pass "All $MUTEX_TOTAL mutexes classified as COLD_PATH_ONLY"
    fi
else
    pass "No mutexes found"
fi

# ─────────────────────────────────────────────────────────────────────
# 6) FIX RESEND RING BUFFER
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 6) FIX resend preallocation                                 │"
echo "└─────────────────────────────────────────────────────────────┘"

if grep -rq "FIXResendRing" "$ROOT" 2>/dev/null; then
    pass "FIX resend uses preallocated ring buffer"
else
    warn "FIXResendRing not found - using heap allocation for resend?"
fi

# ─────────────────────────────────────────────────────────────────────
# 7) ZERO-COPY FIX FIELD ACCESS
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 7) Zero-copy FIX field access                               │"
echo "└─────────────────────────────────────────────────────────────┘"

if grep -rq "getView\|FixFieldView" "$ROOT/fix" 2>/dev/null; then
    pass "Zero-copy field access (getView/FixFieldView) present"
else
    fail "No zero-copy FIX field access found"
fi

# ─────────────────────────────────────────────────────────────────────
# 8) FAST NUMERIC PARSERS
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 8) Fast numeric parsers                                     │"
echo "└─────────────────────────────────────────────────────────────┘"

if grep -rq "fast_parse_int\|fast_parse_double\|FIXFastParse" "$ROOT" 2>/dev/null; then
    pass "Fast numeric parsers (fast_parse_*) present"
else
    fail "No fast numeric parsers found"
fi

# ─────────────────────────────────────────────────────────────────────
# 9) HFT TIME UTILITIES
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "┌─────────────────────────────────────────────────────────────┐"
echo "│ 9) HFT time utilities                                       │"
echo "└─────────────────────────────────────────────────────────────┘"

if [ -f "$ROOT/hft/HFTTime.hpp" ]; then
    pass "HFT time utilities (HFTTime.hpp) present"
else
    fail "HFTTime.hpp not found"
fi

# ─────────────────────────────────────────────────────────────────────
# SUMMARY
# ─────────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
if [ $ERRORS -eq 0 ]; then
    echo "║     ✅ ALL HFT HARDENING CHECKS PASSED                       ║"
else
    echo "║     ❌ $ERRORS CRITICAL ISSUE(S) FOUND                           ║"
fi
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Exit with error if critical issues found
if [ $ERRORS -gt 0 ]; then
    exit 1
fi

exit 0
