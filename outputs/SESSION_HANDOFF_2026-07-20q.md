# SESSION HANDOFF — 2026-07-20q — mimic ALTERNATIVE SEARCH EXHAUSTED (0/23,520 honest cells) — BECASC: allowlist-8 vs retire = OPERATOR DECISION

Predecessor: `outputs/SESSION_HANDOFF_2026-07-20p.md`. This session ran the 20p pending order
to completion: **"if be cascade is not working find me an alternative that works."**

## ✅ DONE THIS SESSION (Crypto `1dd133a`, pushed)
All three ordered alternatives certified at the HONEST own-fill basis (`confirm_anchor_epx=false`,
le = real fill, no booking transform), fleet-wide 35 coins × thr{0.5,1.0,1.5,2.0}%, gate =
omit-2022 + net>0 + PF≥1.3 + both WF halves + base AND 2× cost:

1. **Single-leg mimic** (`UM_SINGLE=1` → mimic_stagger=false, engine's own plain mimic_floor =
   ONE managed T1/event; lc{30,60} × W{2,4,8,12} × g{0.2,0.5,0.75,0.9}, OHLC tick drive):
   **0/4480 PASS.** Churn is per-event confirm crossing, NOT cascade multiplication — PREBE
   cuts stay 55–70% of clips at legs=1. Best cell RUNE thr1.5 lc30 W4 g0.75 +121% base
   PF1.40 → −106% at 2× cost.
2. **e0-EXACT close-confirm BECASC** (`UM_E0=1` → live design stagger_mode=1/be_bp=20/confirm60
   uniform/legs8, confirm evaluated ONLY at close via `stop_check_only(low)+observe(close)`
   drive, le = REAL close fill): **0/5600 PASS.** The 747/840 e0-honest figure (20p, flagged
   upper bound) is HOLLOW — at real close fills close-confirm dies everywhere. **Item 2a of
   the 20p list (engine-side real-fill-px record + intrabar-confirm revert) is now POINTLESS
   for rescue — do not build it.** Honest tails visible: worst cells −1148..−1213bp (gap-through).
3. **Wide-g shallow cascade** (legs{2,3} × g{0.75,0.9} × inc{38,60,100} × lc{30,60} ×
   W{2,4,8,12}): **0/13440 PASS.** All near-misses H1(2021)-carried (H2 ≈ +1..+47 vs H1
   hundreds); best family BNB thr2.0 lc30 W8 re-run at RT=60: net +4%, PF 1.00, H2 −252%.

Artifacts: harness `Crypto/backtest/cascade_increment_bt.cpp` (UM_SINGLE + UM_E0 modes),
runners+raw in `single_leg_2026-07-20/`, `e0_exact_2026-07-20/`, `wideg_cascade_2026-07-20/`,
findings `Crypto/backtest/MIMIC_ALTERNATIVE_SEARCH_FINDINGS_2026-07-20.md`.
Vault: Memory-Chimera `MimicShadowEntryBasisError` (status + new DONE block) + index + log
[17.08]. Auto-memory `feedback-shadow-entry-basis-honesty` + MEMORY.md updated.

## ⚡⚡ NEW OPERATOR ORDER (2026-07-20, supersedes the (a)/(b) question below) — BUILD THE HONEST LEDGER, CORRECT COST, RUN NOW
Operator (verbatim intent): sick of dishonest ledgers/inconsistency — **build the honest ledger,
use the CORRECT cost (measured, not hand-quoted 30/60), run it now.** Spec:
1. **Engine-side honest booking (ChimeraCrypto `MimicLadderCompanion.hpp`):** add per-leg
   `fill_px` recorded at the REAL open (`cur` at the moment of open in `intrabar_confirm_opens_`
   AND `step_leg_`'s open block AND reclip path — the same px `announce_open_` hands the mirror).
   `emit_clip_` + `book_mimic_stop_` book gross from `fill_px`, NEVER from anchored `le`.
   Shadow ledger == mirror economics by construction, all cells, forever.
2. **Correct cost:** per-coin MEASURED RT from DepthLiquidationModel → CryptoCostLedger
   `safe_cost_bps` (memory `feedback-crypto-cost-authoritative-depth-model` — never hand-quote
   20/30/35/60 again). Re-cert the fleet with per-coin measured base cost (stress = 2× measured).
3. Deploy to josgp1 (deploy_to_box.sh — Mac edits stay UNCOMMITTED for its guard; NEVER
   git reset on josgp1), boot gates green, then the honest ledger is the ONLY record.
4. **Omega side ("what is waiting for us on Omega"):** SAME anchored-basis audit owed —
   `FxMimicLadderCompanion` (be_floor_on_open book-clamp), `GoldTrendMimicLadder`,
   SurvivorPortfolio mimic arms: any shadow clip booked from an anchor a real order can't
   fill at is inflated the same way. Audit each booking site vs the price a real order fills
   at; re-cert with IBKR measured cost (XAU RT = 2*0.00015*px+spread, memory
   `project-ibkr-cost-basis`). Nothing on Omega is trusted until this audit runs.

## OPERATOR DECISION PENDING (kept for reference; subsumed by the order above)
Design space exhausted. Only living cells anywhere: the **8 anchored-live survivors
(7 DOGE + 1 RUNE wide-g tail lanes, c944c79)**. Choose:
- **(a) Allowlist re-arm:** build per-cell allowlist mechanism in LiveMimicMirror (substr
  pause can't express 8 cells), re-arm ONLY those; shadow `emit_clip_` epx-basis stays wrong
  for the rest → scope the fix to the allowlisted cells.
- **(b) Retire BECASC family:** mirror decommission + shadow ledgers retired.
Mirror stays PAUSED (josgp1 `7bb43cb`) until the verdict. Nothing touched on the box this session.

## 🔎 OPEN (unchanged from 20p)
- Shadow BECASC ledgers still book from epx — resolution follows the (a)/(b) verdict.
- Crypto shadow-first REBUILD (project memory) still standing; Omega audit owed.
- Memory-Omega vault 2 commits BEHIND (pin lag) — backfill on next Omega-side session.

## GOTCHAS (inherited + new)
- Harness: `-I/Users/jo/ChimeraCrypto/include`, data `/Users/jo/Crypto/backtest/data/<COIN>USDT_1h.csv`.
- clangd false include errors on these harnesses — ignore; clang++ build is truth.
- Runner scripts to FILE always (inline xargs/heredoc broke zsh twice in 20o/20p).
- `stop_check_only()` = the engine's stop-only tick (no detector feed, no confirm opens) —
  the tool that made the e0-exact drive possible without engine change.
- Never judge an own-fill design under the anchored worse-of drive (20p trap, still true).
