#!/bin/bash
# Build the gap-short C++ stack. Mac/Linux: g++. VPS production: build with MSVC
# (cl.exe) — TWS API is cross-platform; same .cpp set.
set -e
cd "$(dirname "$0")"
TWS=../third_party/twsapi/client
echo "[1/3] TWS client lib..."; (cd $TWS && g++ -std=c++17 -O2 -c *.cpp && ar rcs libtwsclient.a *.o)
echo "[2/3] IbkrClient...";    g++ -std=c++17 -O2 -I$TWS IbkrClient.cpp bid64_stub.cpp $TWS/libtwsclient.a -o ibkrclient
echo "[3/3] GapShortEngine..."; g++ -std=c++17 -O2 -I$TWS GapShortEngine.cpp bid64_stub.cpp $TWS/libtwsclient.a -o gapshort_engine
echo "done -> ibkrclient, gapshort_engine"
