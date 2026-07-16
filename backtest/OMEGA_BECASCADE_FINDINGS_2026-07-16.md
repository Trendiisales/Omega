# Omega BE-CASCADE companion — wire findings (S-2026-07-16)

Operator: "wire the becascade across all Omega symbols." Ported the crypto-validated
BE-CASCADE mimic (`chimera::UpJumpLadderCompanion`, ChimeraCrypto build dcb645e) to Omega
as an independent SHADOW clip book. Vendored frozen at `include/BeCascadeCompanionEngine.hpp`;
Omega wrapper `include/OmegaBeCascadeBook.hpp` (`omega::be_cascade_book()`, 47 cells).

## Engine mechanism (confirmed)
`stagger_mode=1` **BE_CASCADE**: releases the next of up-to-8 legs only once EVERY open leg is
break-even (`mfe >= stagger_be_bp`=20bp) → at most ONE un-BE'd leg at a time. Post-arm BE-floor
(never books negative after arming) + `loss_cut_bp=150` cold cut → worst clip ~−155bp across all
47 cells. Independent additive shadow book; judged STANDALONE (feedback-companion-independent-engine).

## Validation (backtest/omega_becascade_bt.cpp — drives THIS engine class)

### Non-stock (H1, per-symbol REAL RT) — ACTIVE
| sym | feed | verdict |
|---|---|---|
| XAUUSD | standard research H1 | PASS (MGC folds in — same underlying, not double-wired) |
| US500 / NAS100 / GER40 | research H1 | PASS (fixed instruments, no survivorship) |
| EURUSD / GBPUSD / AUDUSD | **certified IBKR 3Y H1** (`*_IBKR_H1.csv`) | PASS PF~5–6, 2×cost+ |

FX note: the becascade PASSES on the certified IBKR feed where the reclip FX up-jump *ladder*
was edgeless (`engine_init.hpp:1959`) — different mechanism (floored cascade vs reclip). The
initial pass on `EURUSD_merged`/`AUDUSD_befloor` (favorable-slice histdata) was re-tested on the
real IBKR feed before wiring; it holds.

### Stocks (daily) — 16 ACTIVE / 23 rank-out-warm
- OHLC backtest (Yahoo daily) showed **39/39 PASS** — but that is **survivorship-inflated**
  (the 39 = today's winners) AND OHLC-optimistic (real H/L drove the intrabar floor/cut).
- Re-validated **close-only on the REAL live feed** (`data/rdagent/sp500_long_close.csv`, h=l=c):
  **only 16/39 pass.** ACTIVE: NVDA AMD AVGO MU NFLX NOW DELL ORCL QCOM INTC AMZN GOOGL MSFT AAPL
  CRM ADBE. The other 23 fail/0-trade close-only → wired `rank_out` (warm detector, book nothing,
  re-rank later), mirroring `stockmover_ladder_book()`'s ranked-out set (which independently ranks
  OUT the same TSLA/COIN/PLTR/MSTR/META/UBER… names). Universe = 39 (rd-agent full-ranking == BIGCAP).

## Wire
- 7 non-stock ACTIVE (H1, fed inline from tick_fx / tick_indices / tick_gold) + 39 stock cells
  (16 active + 23 rank_out, fed by the book's internal 15-min poller over sp500_long_close.csv,
  close-only, seed-gated so history primes the detector without booking).
- SHADOW → `logs/shadow/omega_shadow.csv` (engine=`BeCascade`), $10k nominal notional/clip.
- Mac canary GREEN: build + adverse-protection (0 new-viol) + mimic-drawdown (0 active-viol) +
  ungated (0 new) + seed-registry + roster-parity + persistence all pass.
- **VPS MSVC rebuild owed** (operator-side OMEGA.ps1) — first live-fidelity check of the vendored
  985-line header under MSVC /WX; boot log should show the `[OMEGA-INIT][SEED] BE-CASCADE` line.
