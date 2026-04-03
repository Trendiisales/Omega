# Incident: 2026-04-02 Trump Tariff Crash

## Context

**Date:** April 2, 2026 (Liberation Day tariff announcement)  
**Market:** XAUUSD (Gold), USTEC.F (Nasdaq), US500.F (S&P 500)  
**Gold range:** $4,597 – $4,700+ intraday (~$103 range)  
**Session PnL:** +$844 carry-in from prior session, final ~+$39 after indices losses

## What Happened

Trump announced sweeping tariffs ("Liberation Day"). Markets went into violent two-phase action:

**Phase 1 (01:00–04:30 UTC) — GOLD PHASE**  
Gold initially rallied (safe-haven bid), then reversed sharply lower as risk-off hit everything.  
GoldFlow caught both sides: LONG into the rally, SHORT into the crash.  
Multiple entries with reload, STAIR partial exits, DOLLAR-RATCHET locking profit.  
System banked significant profit before indices opened.

**Phase 2 (13:00–20:00 UTC) — INDICES CHAOS**  
US equity indices (USTEC, US500) opened and immediately went into extreme volatility.  
Multiple FORCE_CLOSE events — connectivity/session issues during peak volatility.  
Net indices result: significant losses that partially offset the gold gains.

## Key Learning

1. Gold handled the crash cleanly — stealth SL + ratchet lock worked perfectly.
2. Indices suffered from FORCE_CLOSE losses during volatile reconnect cycles.
3. **No hard stop existed** — if VPS had crashed during the big SHORT block (04:05–04:24),  
   the position would have been exposed with no broker-side protection.
4. This is the canonical incident that motivated the Hard Stop architecture.

## Hard Stop Simulation Results

Against this incident, the v2 Hard Stop architecture was validated:
- All 14 gold positions: hard stop placed, cancelled cleanly on stealth exit
- Crash scenario: hard stop at entry+20pts caps exposure to $320 max (not unlimited)
- Tombstone guard: prevents double-fill on reconnect (the critical bug that was found)
- STAIR size tracking: hard stop updates to remaining size after partial exits
- Cascade scenario (5× LONG SL hits): all 5 hard stops cancelled cleanly, 2 msgs/trade overhead

See: `simulation/hard_stop_sim_v2.cpp` — all scenarios pass, 0 failures.
