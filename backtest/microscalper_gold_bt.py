#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# DEPRECATED 2026-05-08 S19. Replaced by the C++/CRTP sweep harness:
#   backtest/microscalper_crtp_sweep.cpp
#
# Build:
#   clang++ -std=c++17 -O3 -DNDEBUG -I include \
#       backtest/microscalper_crtp_sweep.cpp \
#       -o backtest/microscalper_crtp_sweep
#
# Run:
#   backtest/microscalper_crtp_sweep \
#       backtest/l2_ticks_2026-04-09.csv \
#       data/l2_ticks_2026-04-16.csv \
#       l2_ticks_2026-04-10.csv \
#       --warmup 1000 --top 20 --verbose
#
# This file is kept only as a tombstone so `git log` carries the rename
# trail; safe to `rm` once you've confirmed the C++ harness compiles.
# -----------------------------------------------------------------------------
import sys

sys.stderr.write(
    "[DEPRECATED] microscalper_gold_bt.py is replaced by the CRTP harness.\n"
    "Use:  backtest/microscalper_crtp_sweep  (build instructions in this file).\n"
)
sys.exit(2)
