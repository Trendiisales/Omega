#!/bin/bash
# Cross-compile OMEGA for Windows using Zig
# Run this on Mac or Linux

set -e

echo "=== OMEGA Windows Cross-Compile with Zig ==="

# Check zig is installed
if ! command -v zig &> /dev/null; then
    echo "Zig not found. Install with: brew install zig (Mac) or download from ziglang.org"
    exit 1
fi

# Create build directory
mkdir -p build_win
cd build_win

# Compile all cpp files
SOURCES=$(find ../src -name "*.cpp" | tr '\n' ' ')

echo "Compiling with zig c++ for Windows x86_64..."

zig c++ -target x86_64-windows-gnu \
    -std=c++20 \
    -O2 \
    -DNDEBUG \
    -D_WIN32_WINNT=0x0601 \
    -I../src \
    -lws2_32 \
    -lssl \
    -lcrypto \
    -lpthread \
    -o omega.exe \
    $SOURCES

echo "Build complete: build_win/omega.exe"
echo "Copy omega.exe to VPS"
