#!/bin/bash
# =============================================================================
# CHIMERA HFT - Easy Start Script
# =============================================================================
# Usage: ./start.sh [options]
#   --build       Force rebuild
#   --testnet     Use Binance testnet (default)
#   --production  Use Binance production
#   --live        Enable live trading (DANGER!)
#   --gui-only    Start only the dashboard (no engine)
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
DASHBOARD="$SCRIPT_DIR/chimera_dashboard_v4.html"
GUI_PORT=8081
ENGINE_PORT=9001

# Parse arguments
FORCE_BUILD=0
USE_TESTNET=1
LIVE_MODE=0
GUI_ONLY=0

for arg in "$@"; do
    case $arg in
        --build)       FORCE_BUILD=1 ;;
        --testnet)     USE_TESTNET=1 ;;
        --production)  USE_TESTNET=0 ;;
        --live)        LIVE_MODE=1 ;;
        --gui-only)    GUI_ONLY=1 ;;
        --help|-h)
            echo "Usage: $0 [--build] [--testnet|--production] [--live] [--gui-only]"
            exit 0
            ;;
    esac
done

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}"
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║              CHIMERA HFT ENGINE LAUNCHER                     ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo -e "${NC}"

# Check if dashboard exists
if [ ! -f "$DASHBOARD" ]; then
    echo -e "${RED}ERROR: Dashboard not found at $DASHBOARD${NC}"
    exit 1
fi

# Build if needed
if [ ! -f "$BUILD_DIR/chimera" ] || [ $FORCE_BUILD -eq 1 ]; then
    echo -e "${YELLOW}Building Chimera...${NC}"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    CMAKE_OPTS=""
    if [ $USE_TESTNET -eq 1 ]; then
        CMAKE_OPTS="$CMAKE_OPTS -DBINANCE_USE_TESTNET=ON"
        echo -e "  Binance: ${CYAN}TESTNET${NC}"
    else
        CMAKE_OPTS="$CMAKE_OPTS -DBINANCE_USE_TESTNET=OFF"
        echo -e "  Binance: ${YELLOW}PRODUCTION${NC}"
    fi
    
    if [ $LIVE_MODE -eq 1 ]; then
        CMAKE_OPTS="$CMAKE_OPTS -DCHIMERA_LIVE=ON"
        echo -e "  Mode: ${RED}LIVE TRADING${NC}"
    else
        echo -e "  Mode: ${GREEN}SHADOW (dry-run)${NC}"
    fi
    
    cmake $CMAKE_OPTS .. > /dev/null
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    cd "$SCRIPT_DIR"
    echo -e "${GREEN}Build complete!${NC}"
fi

# Kill any existing processes
echo -e "${YELLOW}Cleaning up old processes...${NC}"
pkill -f "python3 -m http.server $GUI_PORT" 2>/dev/null || true
pkill -f "$BUILD_DIR/chimera" 2>/dev/null || true
sleep 1

# Start dashboard server
echo -e "${CYAN}Starting dashboard server on port $GUI_PORT...${NC}"
cd "$SCRIPT_DIR"
python3 -m http.server $GUI_PORT > /dev/null 2>&1 &
DASHBOARD_PID=$!
sleep 1

if ! kill -0 $DASHBOARD_PID 2>/dev/null; then
    echo -e "${RED}Failed to start dashboard server${NC}"
    exit 1
fi

echo -e "${GREEN}Dashboard: http://localhost:$GUI_PORT/chimera_dashboard_v4.html${NC}"

# Start engine (unless gui-only)
if [ $GUI_ONLY -eq 0 ]; then
    echo -e "${CYAN}Starting Chimera engine...${NC}"
    cd "$BUILD_DIR"
    ./chimera &
    ENGINE_PID=$!
    cd "$SCRIPT_DIR"
    
    sleep 2
    if ! kill -0 $ENGINE_PID 2>/dev/null; then
        echo -e "${RED}Engine failed to start${NC}"
        kill $DASHBOARD_PID 2>/dev/null
        exit 1
    fi
    
    echo -e "${GREEN}Engine running (PID: $ENGINE_PID)${NC}"
    echo -e "${GREEN}Metrics API: http://localhost:$ENGINE_PORT/metrics${NC}"
else
    echo -e "${YELLOW}GUI-only mode - engine not started${NC}"
    ENGINE_PID=""
fi

echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  CHIMERA IS RUNNING                                          ║${NC}"
echo -e "${GREEN}║                                                              ║${NC}"
echo -e "${GREEN}║  Dashboard: http://localhost:$GUI_PORT/chimera_dashboard_v4.html    ║${NC}"
echo -e "${GREEN}║  API:       http://localhost:$ENGINE_PORT/metrics                   ║${NC}"
echo -e "${GREEN}║                                                              ║${NC}"
echo -e "${GREEN}║  Press Ctrl+C to stop                                        ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Open browser (macOS/Linux)
if command -v open &> /dev/null; then
    open "http://localhost:$GUI_PORT/chimera_dashboard_v4.html"
elif command -v xdg-open &> /dev/null; then
    xdg-open "http://localhost:$GUI_PORT/chimera_dashboard_v4.html"
fi

# Trap Ctrl+C
cleanup() {
    echo ""
    echo -e "${YELLOW}Shutting down...${NC}"
    [ -n "$ENGINE_PID" ] && kill $ENGINE_PID 2>/dev/null
    kill $DASHBOARD_PID 2>/dev/null
    echo -e "${GREEN}Goodbye!${NC}"
    exit 0
}
trap cleanup SIGINT SIGTERM

# Wait for engine or dashboard
if [ -n "$ENGINE_PID" ]; then
    wait $ENGINE_PID
else
    wait $DASHBOARD_PID
fi
