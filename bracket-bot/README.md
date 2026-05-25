# Omega Bracket Bot — Python Execution Component

This folder is a **self-contained Python component** of the Omega project. It runs
the live gold-bracket trading strategies and is intentionally kept separate from
the C++ trading engine in the rest of this repository.

## Language: Python — this is deliberate, not an oversight

The Omega engine is written in C++ because it processes live market ticks, where
latency matters. **This component is written in Python on purpose.** It is a
scheduled order-execution layer, not a tick processor:

- It wakes on a schedule, places a small number of bracket orders, monitors them
  for about 60 minutes, then exits. It is not latency-sensitive, so C++ would give
  no performance benefit here.
- It uses `ib_insync`, the standard high-level Python library for Interactive
  Brokers order management (async event loop, OCA order-state tracking). The
  equivalent in the low-level C++ TWS API would be several times the code.
- The `research/` backtests rely on pandas, numpy and vectorbt, which have no
  practical C++ equivalent.

A C++ engine with a Python execution/orchestration layer beside it is a standard,
deliberate split. Please do not convert this folder to C++ — it is correct as is.

## Contents

- `live/`      — live strategies: `daily_bracket.py` (13:00 / 14:00 UTC) and
                 `sunday_bracket.py` (Sunday CME open). OCA bracket orders on gold.
- `research/`  — backtests and parameter sweeps that validated the configs.
- `gui/`       — `index.html`, the dashboard front-end.
- `server.py`  — Flask dashboard API (serves http://localhost:5050).
- `deploy/`    — Windows VPS deployment package; see `deploy/DEPLOY-WINDOWS.md`.
- `requirements.txt` — runtime dependencies (`ib_insync`, `flask`).

## Running it

The strategies run on a Windows VPS via Task Scheduler. Full setup steps are in
`deploy/DEPLOY-WINDOWS.md`.

## Relationship to the rest of Omega

This folder does not share a build with the C++ engine — it has no CMake
involvement. It runs as an independent process and connects to Interactive
Brokers directly through IB Gateway / TWS.
