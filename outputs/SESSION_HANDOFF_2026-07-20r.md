# SESSION HANDOFF — 2026-07-20r — HONEST LEDGER SHIPPED BOTH SYSTEMS (crypto 6d5d0dd + Omega 58a478e2, both LIVE-verified) — BECASC allowlist-11 vs retire = OPERATOR DECISION

Predecessor: `outputs/SESSION_HANDOFF_2026-07-20q.md`. This session executed the 20q NEW
OPERATOR ORDER ("build the honest ledger, use the CORRECT cost, run it now") end-to-end on
BOTH systems.

## ✅ DONE — CRYPTO (ChimeraCrypto `6d5d0dd`, DEPLOYED josgp1, verified running; Crypto repo `c62bab8`)
1. **Engine honest booking:** per-leg `fill_px` recorded at every REAL open
   (`intrabar_confirm_opens_`, `step_leg_` open, reclip, jf/be legs) = the px
   `announce_open_` hands the mirror. `emit_clip_` books gross from `fill_px`, NEVER
   anchored `le`; ALL columns honest; `ClipRecord.entry_px`=fill, new `anchor_le` field
   preserves the anchor for audits; log shows `(anch=)` + `fill=`. Design mechanics
   (confirm gate, floor/stop LEVELS, spawn) unchanged — booking only.
2. **Measured cost:** `depth_cost_model_bt` run 15 coins × $100..$100k
   (`ChimeraCrypto/backtest/depth_cost_all15_2026-07-20.txt`). Pilot-size RT 28.0–29.5bp
   ALL coins; thin coins blow out only at size (RUNE $25k=59, IMX $25k=249, HBAR $100k=92).
   New `data/depth_cost_measured.csv` → boot `set_depth_slip` → `[COST-MEASURED]` ×15;
   BECASC `rt=28.0` literal → `safe_cost_bps(sym)`. Missing file = LOUD warning.
3. **Re-cert at honest basis + measured cost: 369/380 live BECASC cells FAIL, 11 survivors**
   (all g0.75 wide-tail: DOGE ×7 [MIM05-W2/W4/W12, MIM10-W2/W4/W12, MIM15-S],
   RUNE-MIM05-W4, RUNE-MIM15-F, AVAX-MIM15-F, INJ-MIM05-W4).
   `Crypto/backtest/HONEST_LEDGER_MEASURED_RECERT_2026-07-20.md` + raw + `join_live_cells.py`
   in `honest_basis_measured_2026-07-20/`. Harness gains `UM_NATIVE=1` (books engine
   net_bp_real directly) — **CAVEAT: under the OHLC e1 drive the confirm fill lands at the
   fed bar HIGH = pessimistic BOUND, not the live fill; certify on the transform basis
   (validated byte-exact vs the RT=30 run via anchor_le).**
4. Boot gates verified: `[COST-MEASURED]` 15 coins, `[MIMIC-FLOOR-GATE]` 419/419 0 VIOLATION,
   `[MIMIC-LIVE-PAUSE]` BECASC intact, reconcile PASS, cash $1100.

## ✅ DONE — OMEGA (`58a478e2`, DEPLOYED omega-new, running binary verified == HEAD == origin)
Anchored-basis audit (20q item 4) RAN. Verdict:
- **Entry basis HONEST on Omega** — FX BE-ENTER + Gold BE-ENTER open AT the confirm level
  (the price a real resting order fills at); crypto's anchored-entry error NOT present.
- **Exit booking was the S-17f tautology — FIXED:**
  - `FxMimicLadderCompanion::book_clip_`: BE clamp (`fill=max(fill,be)` + `pct=max(pct,0)`)
    REMOVED; LOSS_CUT/TRAIL_STOP book worse-of stop level vs pierce extreme. (Every pre-arm
    LOSS_CUT used to book +0 while the real market close realized −5·thr.)
  - `GoldTrendMimicLadder` (+ SurvivorPortfolio mimic arms route through it): BE_FLOOR
    booked 0.0 / LOSS_CUT+TRAIL booked the resting LEVEL / WINDOW_CAP clamped ≥0 — all now
    book the OBSERVED ret at trigger.
  - Design pillars unchanged (BE-ENTRY, triggers, timing, clip count). nNeg>0 now = real
    sub-BE exit. Config comments corrected (never restate nNeg==0 as execution guarantee).
- **Costs already IBKR-measured** (XAU 5bp = 2×1.5bp comm + spread; FX IDEALPRO ~2bp) —
  no crypto-style hand-quote found. Ledger TradeRecords (primary record) now honest.
- Mac canary EXIT=0 (prebe gate = DESIGN gate per 17f framing, passes).

## Vault/memory (all updated)
- Memory-Chimera: `MimicShadowEntryBasisError` FIXED-AT-SOURCE block + index + log.
- Memory-Omega: `GoldTrendMimicLadder` + `FxUpJumpLadder` + `BeFloorOnOpenFoundation`
  honest-ledger notes + index + log; **pin advanced 5509e99f → 58a478e2** (backfill cleared).
- Auto-memory `feedback-shadow-entry-basis-honesty` + MEMORY.md updated.

## ⚡ OPERATOR DECISION PENDING (unchanged in kind, candidate set now 11)
BECASC mirror stays PAUSED. Choose:
- **(a) Allowlist re-arm** the 11 measured-cost survivors — needs a per-cell allowlist
  mechanism in LiveMimicMirror (substr pause can't express 11 cells). Survivors are
  top-heavy (H2 ≪ H1) but pass the full gate incl. 2× measured cost.
- **(b) Retire the BECASC family** (mirror decommission + ledgers retired).
Alternative designs remain EXHAUSTED (0/23,520, 20q).

## 🔎 OPEN
- **Expect failed-cell crypto shadow banks to turn visibly negative** — the honest ledger
  booking the real ≈−confirm floor churn. ORDERED honesty, NOT a regression — do not
  "fix" it back.
- Omega FX/Gold books: honest-basis forward record now decides viability; the old certs
  were run with the clamped booking. Optional next: re-run each book's own harness with
  honest booking (fx_ibkr_ladder_sweep / goldmimic harnesses) if the operator wants a
  BT verdict before the forward record accumulates.
- Crypto shadow-first REBUILD project memory still standing (separate order, 20b scope).

## GOTCHAS (inherited + new)
- Never judge honest-fill booking under an OHLC drive (fill=bar HIGH = bound). Transform
  basis (confirm level) = the live-fill model; validated via `ClipRecord.anchor_le`.
- deploy_to_box.sh: DEPLOY_FILES override used (main.cpp + MimicLadderCompanion.hpp +
  depth_cost_measured.csv + provenance txt); Mac reconciled via stash/reset/pop.
- Harness: `-I/Users/jo/ChimeraCrypto/include`; runner scripts to FILE (zsh ate `=====` echo).
- clangd false errors on Omega headers/harnesses — ignore; canary/clang++ build is truth.
- docs/IBC_GATEWAY_AUTOSTART.md still uncommitted on Omega Mac (pre-existing, not this
  session's — left alone).
