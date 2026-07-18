# FX/INDEX LADDER BOUNDED CATCH-UP CERT — seed-replay window recovery — 2026-07-18

**Task (operator-ordered, handoff 2026-07-18p):** port the crypto bounded catch-up to the Omega
companions with **their own cert** (crypto cert `Crypto/backtest/CATCHUP_OUTAGE_CERT_2026-07-18.md`
is never carried across). Gold-side twin cert: `backtest/GOLDMIMIC_CATCHUP_CERT_2026-07-18.md`.

**VERDICT: CERTIFIED — wired `catchup_max_age_bars = 24` on all 5 FxLadderPair cells of the
class: GBPUSD (FX H1) + US500 / NAS100 / GER40 / M2K (index H1).**

## Mechanism (detector-replay class — the crypto topology)

FxUpJumpLadderCompanion owns its jump DETECTOR: a jump bar that closes while the service is
down is a permanently lost window — on restart the seeded history rebuilds bars but
`detect` only ran on live bars, so the always-on book holds a window the restarted book
never opens. The port: `catchup_replay_()` called from `finalize_seed()` replays the seeded
history through the SAME detector rule (same flush→detect bar order, contiguity guard,
weekend-arm block) and re-opens the window iff:

- an always-on book would still be **flat-in-window** (BE never crossed since the trigger
  bar — replay proves it bar-by-bar);
- age ≤ `catchup_max_age_bars` (24 H1 bars); default `0` = feature OFF;
- cell is floored family (`be_entry_pct>0 && be_floor_on_open`), not retired, not
  regime-blocked, no in-flight `win_`/`legs_`.

age < `pend_bars` → re-spawns the 5 PENDING legs (shared `spawn_base_batch_`, pbars=age) —
they open ONLY via the live BE-ENTRY path (books nothing until fav ≥ confirm; BE-floor
FOUNDATION intact, no new pre-BE window). age ≥ pend → legless window restore (trail
accounting only). A BE cross that happened during the outage → window NOT reopened
(crypto SKIP rule — never a backdated fill).

Also cert-caught and fixed: `save_fwd_book_`/`save_live_state_` persisted at the 6-sig-fig
stream default — a reloaded state was quantized so post-restart clips diverged from the
always-on book. Now `precision(15)` (same fix applied to GoldTrendMimicLadder state files).

## Harness

`backtest/fx_catchup_outage_bt.cpp` — drives the REAL `FxLadderPair` (no model),
engine_init config parity per cell. Traps handled (documented for reuse):

- **Per-run fresh temp state dir** — FxLadderPair persists deploy/h1/book/live/closed
  files; any shared dir leaks state across runs (the fx_ibkr_ladder_sweep-class trap).
  `deploy_ts` pre-written 0 so seed replay is unrestricted.
- **Restart = new instance** + `seed_bar` over the full history + `finalize_seed()` —
  the production boot path, not a hand-rolled reload.
- **Sorted-PAIRWISE clip compare, rel 1e-8** — llround-quantized clip keys are
  .5-boundary-fragile (two equal books can hash to adjacent keys); pairwise relative
  compare on sorted clip lists is the robust equivalence check.
- **book.txt p6→p15 finding** — the first surgical run failed one NAS100 window on book
  PRECISION only (clips identical); root cause the 6-sig-fig persistence above, fixed in
  the engine, harness rerun clean.

Data (all `backtest/data_integrity_gate.py` CERTIFIED CLEAN this session):
`Tick/GBPUSD_IBKR_H1.csv`, `Tick/SPXUSD_2022_2026.h1.csv`, `Tick/NSXUSD_2022_2026.h1.csv`,
`Tick/GER40_1h_yahoo.csv`, `Tick/M2K_h1.csv`.

Modes (env `FXCU_MODE`, `FXCU_COSTX`):

- **surgical** — per detected jump window: A always-on (reference) vs B outage-across-
  the-jump-bar without catch-up (window lost) vs C same outage WITH catch-up. PASS =
  C recovers the window and books clips EQUIVALENT to A (pairwise rel 1e-8) in **0
  mismatched windows**, every cell, both costs. This is the fidelity gate.
- **grid** — outage every N=24 bars × L∈{2,6,12,24} bar lengths across the full history:
  A always-on / B no-catch-up / C catch-up. Gates: C.book>0, C.pf ≥ min(1.3, 0.95·A.pf),
  C.worst ≥ min(A.worst, B.worst) − 0.01.

## Surgical results (the 0-mismatch fidelity gate) — PASS, 0 mismatched windows

| cell | cost | windows | lostB | recovered clips | mismatched |
|---|---|---|---|---|---|
| GBPUSD | 1x | 20 | 6 | 101 | **0** |
| US500  | 1x | 18 | 2 | 91 | **0** |
| NAS100 | 1x | 40 | 7 | 201 | **0** |
| GER40  | 1x | 16 | 3 | 80 | **0** |
| M2K    | 1x | 5 | 0 | 29 | **0** |
| **TOTAL** | 1x | **99** (H1-half 53 / H2-half 46) | **18** | **502** | **0 ⇒ PASS** |
| GBPUSD | 2x | 20 | 6 | 101 | **0** |
| US500  | 2x | 18 | 2 | 91 | **0** |
| NAS100 | 2x | 40 | 7 | 201 | **0** |
| GER40  | 2x | 16 | 3 | 80 | **0** |
| M2K    | 2x | 5 | 0 | 29 | **0** |
| **TOTAL** | 2x | **99** (H1-half 53 / H2-half 46) | **18** | **502** | **0 ⇒ PASS** |

`lostB` = windows B permanently loses that C recovers. Every recovered window's clips
match the always-on book pairwise (rel 1e-8).

## Grid results — PASS both costs, 0 gate fails (20 cell-runs × 2 costs)

cost×1 (d(C−B) = catch-up book net minus no-catch-up net, per cell-run):

| cell | L=2 | L=6 | L=12 | L=24 | A net | B net range | C pf range |
|---|---|---|---|---|---|---|---|
| GBPUSD | +6.64% | +6.37% | +5.25% | +6.64% | +137.95% | +118.97..+122.97% | 66.5..102.9 |
| US500  | −2.75% | +0.00% | +2.57% | +1.39% | +416.80% | +368.67..+423.39% | 34.3..124.5 |
| NAS100 | +2.06% | −2.86% | −3.48% | +0.00% | +1226.44% | +1160.48..+1196.43% | 21.9..25.3 |
| GER40  | +0.18% | +0.00% | +0.00% | +0.00% | +270.60% | +266.89..+270.13% | 7.2..7.6 |
| M2K    | +0.00% | +0.00% | +0.00% | +0.00% | +496.89% | +391.43..+425.12% | 11.6..22.4 |

cost×2: same shape, all gates OK (GBPUSD d +5.21..+6.52%; US500 −2.55..+2.17; NAS100
−3.42..+2.00; GER40 +0.00..+0.08; M2K all +0.00). GRID TOTAL fails=0 both costs.

Negative d(C−B) cells (US500 L=2, NAS100 L=6/12) are **window-selection variance**, not a
catch-up cost: recovering a window changes which subsequent windows the book is free to
take; reported, not gated — the gated claims are book>0 / pf floor / worst-clip floor,
all green. GBPUSD (the only cell whose venue restarts actually cross jump bars weekly) is
the consistent beneficiary: +5.2..+6.6% per grid run.

**Unit tests** `backtest/catchup_unit_test.cpp` (FX half): 14/14 — default-off, age
bound, pend-elapsed legless restore, BE-crossed-in-outage skip, weekend-arm block,
contiguity guard, non-floored cfg refusal, retired/regime-block refusal, in-flight skip,
p15 state round-trip.

## LIMITATION (follow-up owed)

Recovery sees only the bars the SEED covers. The nightly warmup-refresh + state-reset +
multi-day-outage cases are exactly covered (the seed CSVs span them). A same-day
mid-session missed bar that is in NEITHER the persisted h1 state NOR the seed CSV is
unrecoverable — Omega has no boot backfill feed (crypto uses REST klines at boot).
Possible future fix: boot H1 backfill via the IBKR bridge pull (NOT via the exec
gateway — standing rule). Until then the bound is honest: the feature recovers what the
seed proves, and spawns nothing it cannot prove.

## Scope notes

- All 5 wired cells certified in one pass (never-half-symbols). Cells not wired keep
  `catchup_max_age_bars=0` (default OFF).
- SHADOW book — nothing here is a real forward trade; shadow = live severity applies.
- Companion standalone framing: the catch-up restores the companion's OWN windows; no
  comparison vs the parent/WIDE is made or valid.
