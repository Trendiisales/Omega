AURUM-BREAK-PULLBACK-V1
=======================

CONTENTS
--------
AurumBreakPullback.cpp  Complete C++20 engine source.
build.command           macOS build script.
README.txt               This file.

WHAT THIS IS
------------
A self-contained XAUUSD/MGC backtest and paper-engine core implementing:

- London and New York opening ranges.
- Long and short 30-minute EMA/VWAP regime filters.
- Cross-market XAU/MGC breakout confirmation.
- First controlled five-minute pullback entry.
- XAU/MGC cost comparison and single-venue routing.
- Structural ATR stop with minimum cost clearance.
- +1R stressed-cost floor.
- 40% partial exit at +1.25R.
- ATR runner after +1.75R.
- Failed-breakout and end-of-session exits.
- One trade per session and two trades per day.
- Daily, weekly and 3% account drawdown locks.
- Actual simulated fill booking and CSV trade ledger.
- London and New York daylight-saving calculations.

This source does not contain broker credentials or a broker-specific live order API.
The live broker/FIX adapter must submit/cancel orders and return actual fills to the
same state machine. The included executable is immediately usable for CSV backtests
and paper-engine evaluation.

SAVE AND BUILD ON MAC
---------------------
1. Download and unzip this folder into:

   /Users/jo/Downloads/AurumBreakPullbackEngine

2. In Terminal run:

   chmod +x "$HOME/Downloads/AurumBreakPullbackEngine/build.command"
   "$HOME/Downloads/AurumBreakPullbackEngine/build.command"

The compiled executable is created here:

   /Users/jo/Downloads/aurum_break_pullback

DIRECT BUILD COMMAND
--------------------
clang++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
  "$HOME/Downloads/AurumBreakPullbackEngine/AurumBreakPullback.cpp" \
  -o "$HOME/Downloads/aurum_break_pullback"

INPUT CSV
---------
Both files must be sorted oldest to newest and overlap in UTC time.
The engine automatically aggregates ticks/quotes to one-minute bars.

Recognised timestamp headers:
  timestamp, time, datetime, date_time, date, epoch, ts

Recognised price headers:
  bid + ask
  or last, price, mid, close, last_price, trade_price

Recognised optional volume headers:
  volume, vol, size, qty, quantity, trade_size

Timestamp forms:
  2026-07-20T08:30:00Z
  2026-07-20T20:30:00+12:00
  Unix seconds, milliseconds, microseconds or nanoseconds

EXAMPLE RUN
-----------
"$HOME/Downloads/aurum_break_pullback" \
  --xau "/Users/jo/Tick/2yr_XAUUSD_tick.csv" \
  --mgc "/Users/jo/Tick/MGC.csv" \
  --equity 100000 \
  --xau-base-cost-bp 6 \
  --xau-entry-slip-bp 1 \
  --xau-exit-slip-bp 2 \
  --mgc-rt-commission 2.50 \
  --mgc-entry-slip-ticks 1 \
  --mgc-exit-slip-ticks 1 \
  --ledger "$HOME/Downloads/aurum_trades.csv" \
  --verbose

SHOW ALL OPTIONS
----------------
"$HOME/Downloads/aurum_break_pullback" --help

IMPORTANT CERTIFICATION RULE
----------------------------
Do not connect this to live capital merely because it compiles or produces a
positive full-sample result. Longs and shorts must pass separately at normal and
2x costs, both chronological halves, walk-forward folds and neighbouring settings.
The executable prints the required gate reminder at the end of each test.
