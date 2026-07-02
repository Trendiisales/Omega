# QNDX (Nasdaq TSMom50) — moved OFF the ~/Crypto book ONTO Omega (S-2026-07-03)

**Operator instruction:** "ensure [the Nasdaq entry] is not [on] the crypto GUI
… it must be activated and working … MOVE THIS NASDAQ ENTRY OFF CRYPTO AND ONTO
OMEGA." The crypto GUI (:8090) was showing a QNDX (Nasdaq-100 SQF) trend
position as its only tradeable row — a Nasdaq index trade does not belong on a
crypto book; it was only there because account U23757894 is not crypto-eligible.

## Omega side — DONE (this repo, PR #2)

- `include/NasdaqTsmom50Engine.hpp` — the QNDX trend leg as a first-class Omega
  C++ engine on the native NAS100 FIX feed (no IBKR client, no new TU). Rule
  reverse-confirmed against the live book **to the cent** (book flip level
  26,479.47 == ^NDX close exactly 50 trading days back): LONG while daily close
  > close[t−50], flat otherwise. Flip exit + **8% disaster stop** (30y faithful
  ^NDX daily sweep: net UP, worst −12.2%→−8.0%, PF holds 1.94 — upgrades the
  book's "no stop by design"). Vol-target sizing (the book's "0.96×" column).
  Shorts were tested and are DEAD (PF 1.05 30y) — long-only shipped.
- Wired ACTIVE (enabled=true, SHADOW like the QNDX book it replaces):
  `globals.hpp` instance, `engine_init.hpp` config+seed (`[SEED]` line),
  `tick_indices.hpp` dispatch, mac-canary list, adverse-protection audit PASS.
- Seed refreshed: `phase1/signal_discovery/warmup_NAS100_D1.csv` now ends at the
  last completed daily bar; seeded state on boot = **mom=LONG** (matches the
  live book's open LONG). On first NAS100 tick after deploy the engine opens
  the equivalent shadow LONG — position continuity across the migration.
- Timing note: the engine evaluates momentum on COMPLETED daily bars (the
  backtested form; signals at close, next-open act). The Mac book marked the
  flip level intraday — expect the Omega flip reference to lag the old GUI's by
  one trading day intraday and converge at each UTC rollover.

## Mac side — operator / next Mac session (~/Crypto is not in this repo)

1. Flatten/close the QNDX shadow position in the book state (its realized PnL
   row goes to `crypto_inbound.csv` as usual so the Omega ledger keeps the
   history), then remove QNDX from the book's instrument list so
   `refresh_shadow.py` / `state.json` / the :8090 GUI no longer carry it
   (TSMom50 AND the flat RSIrev leg).
2. The :8090 GUI then shows crypto legs only (currently all hidden/unexecutable
   on U23757894). The GUI's QNDX row disappearing IS the acceptance check.
3. Do NOT re-add index instruments to the crypto book — index edges live in
   Omega (this engine + the DJ30/SPX turtles).

## BearRecovery (BTC/ETH) — NOT on the crypto GUI

The new `CryptoBearRecoveryEngine` is **not wired into ~/Crypto or its GUI**:
nothing in the book imports it, and the C++ signal CLI
(`tools/crypto_bear_recovery.cpp`) only runs when explicitly invoked. Wiring it
into the book's refresh cycle is a deliberate, separate operator step.

## Luke engine compatibility — CHECKED, no conflicts

- **IBKR clientIds all distinct:** Luke/BigCapMomo 86, NqMomoIbkr 87 (dormant),
  crypto bridge 95, the Mac QNDX book used 80. The new NasdaqTsmom50 engine
  uses **no IBKR client at all** (native FIX NAS100 feed) → zero collision.
- **No strategy overlap:** Luke trades scanner-selected STOCKS (TOP_PERC_GAIN,
  SPY-200MA regime, A/C daily setups); TSMom50 trades the NAS100 INDEX. In the
  BULL phase both can be long simultaneously by design — different books.
- **No TU/build impact:** the engine is a pure-std header in the main TU (same
  class as the index turtles), not a TWS TU; Luke's isolation pattern
  (bigcap_momo_ibkr.cpp) is untouched.
- Note: the dormant `NqMomoIbkr` (NQ futures momentum, clientId 87) stays
  dormant — its own edge failed cost-robustness; it is NOT this strategy.
