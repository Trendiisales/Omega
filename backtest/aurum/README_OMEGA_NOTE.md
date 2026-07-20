# Aurum drop — external, under evaluation (NOT an Omega engine)

`AurumBreakPullback.cpp` / `README.txt` / `build.command` here are an **external, self-contained
drop** (AURUM-BREAK-PULLBACK-V1) copied verbatim for tracking during an adversarial audit. It is
**NOT wired into Omega, NOT built by any Omega target, NOT deployed, and touches no live box.**

Audit + honest backtest verdict: **DEAD** — see `backtest/AURUM_BREAKPULLBACK_AUDIT_2026-07-20.md`.
Summary: a one-line state-machine bug makes every trade path unreachable (0 trades as shipped); the
intended XAU/MGC two-venue form is untestable (no minute/tick MGC data exists); and with the bug
fixed + filters relaxed to force trading, the single-market core is net-negative on every
certification leg (long, short, 1×, 2×, bull, bear). Do not pursue.

Files here are the unmodified drop. The instrumented diagnostic build used for the audit lives in the
session scratchpad and is intentionally not committed.
