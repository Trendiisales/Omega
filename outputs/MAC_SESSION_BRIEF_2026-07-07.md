# MAC SESSION BRIEF — finish the honest-accounting / rider-settings work and deploy
(Written by the cloud session on branch `claude/trading-engines-review-4gd67v`, PR #3.
That session has no access to ~/Tick, ~/Crypto, the Mac build, or the VPS — this one does.
Everything below is already committed on the branch; your job is the Mac/VPS half.)

## Context in 8 lines (decisions already made with the operator — do NOT relitigate)

1. All six BE-floor companions + crypto producers now book DUAL columns: `pts/usd` (legacy
   model) vs `pts_real/usd_real` (fills at observed closes / worse-of, per-symbol real RT
   cost debited). **The `*_real` column is the only judge.** "neg=0 by construction" is dead.
2. The proposed 50..400 TP ladder was BACKTESTED and is NOT viable (net negative on all 14
   instruments, every fill model — `outputs/LADDER_BT_RESULTS_2026-07-07.txt`). Clip books
   (BE-floor tiers) stay SHADOW-only; tier-viability gate (`min_gb_mult`) weeds tiers whose
   giveback < 3x the instrument's real cost.
3. The RIDER (UpJump pattern: jump in, ride, symmetric-jump out) is the structure that
   survives real costs. New engine `include/JumpRiderEngine.hpp` runs it on 12 CFD symbols
   (shadow), fed beside the BE-floor sinks.
4. Walk-forward sweeps (`backtest/rider_sweep_wf.py`, IS=first 60% ranked / OOS=last 40%
   verdict / plateau check) on 9y Binance + the operator's first Tick run locked:
   **crypto majors BTC/ETH UpJump 4%x48h, SOL 7%x48h** (uniform 2%/24h is net negative —
   supersedes the old uniform-2% rule, operator approved); **XAUUSD rider W2/0.75%/BE05**;
   WATCH: USDJPY W2/0.30%, GER40 W4/0.50%/BE10, NAS100 W4/0.75%/BE05, US500 W2/0.75%.
   FAILED WF (shadow baseline only): XAG/USOIL/EUR/GBP/AUD/NZD/DJ30 riders.
5. Daily edges re-validated at real costs: EMAx/Kelt/TSMom50 on BTC+ETH; **TSMom90 is a NEW
   robust leg** (strongest on SOL); XAUUSD daily TSMom50 and DJ30 daily EMAx both-window
   positive on the 2y samples.
6. Crypto-repo fixes live in `outputs/crypto_realfills_7d040d4.patch` (apply to ~/Crypto
   @ 7d040d4): wave-companion real fills + ledger dedup + forward anchor, atomic state
   writes, contracts/vt freeze, stall parse-fail guard, live_mark race fix, UpJump<pct>x<W>
   strat parsing + retuned major legs.
7. The FIRST Tick sweep run only saw part of the data: gold matched the 2022-23 H1 file
   only, indices fell back to repo warmups. The loader now reads the REAL inventory
   (OMEGA.md §1): gold duka ticks (`duka_ticks/XAUUSD_*combined.csv`, ts,ask,bid ASK-FIRST),
   histdata index sets (`xregime/US500_2426.csv`, `NAS2022_bear_h1.csv`,
   `duka_multiyear/usa500idxusd*`/`usatechidxusd*`, `book_combined/DJ30_clean.csv`,
   `histdata_book/GER40/**`). Formats are sniffed per file and folded to H1.
8. Full detail: `outputs/SESSION_HANDOFF_2026-07-07a.md`, `outputs/RIDER_SWEEP_WF_2026-07-07.txt`,
   PR #3 description, and the commit messages on this branch.

## YOUR TASK LIST (in order)

1. `git fetch origin claude/trading-engines-review-4gd67v && git checkout claude/trading-engines-review-4gd67v`
   (or cherry the branch onto a fresh main checkout — but the branch is rebased on main already;
   run `bash tools/check_branch_freshness.sh`).
2. **Re-run the sweeps on the FULL Tick data** (gold parses a 4.6GB tick file — minutes):
   `TICKDIR=~/Tick CDATA=~/Crypto/backtest/data python3 backtest/rider_sweep_wf.py | tee /tmp/rider_tick2.txt`
   `TICKDIR=~/Tick CDATA=~/Crypto/backtest/data python3 backtest/daily_edge_scan.py | tee /tmp/daily_tick2.txt`
   Check the `[data]` lines per symbol — US500/NAS100 must show the 2022-bear chunks + 2024-26
   sets; XAUUSD must show the combined tick file. If a known file is missed, extend
   `TICK_SPECS` in `backtest/rider_sweep_wf.py` (documented at the top) and note it.
3. **Lock gold/index entries+exits from the new results.** Rules (operator-agreed, firm):
   adopt ONLY configs with OOS net > 0 AND plateau "ok" AND >= 30 IS trades; prefer the
   simpler exit when within noise (FLIP < BE05/BE10 < TR); update the `JR[]` table in
   `include/engine_init.hpp` (fields: W, thr, rt_cost_bp, be_arm_mult — BE05=0.5, BE10=1.0)
   and the header comment's GO/WATCH/FAILED lists. Symbols that fail again keep family
   defaults and stay FAILED (shadow baseline only).
4. **Canary + audit**: `cmake --build build --target OmegaBacktest -j && bash scripts/mac_canary_engines.sh`
   (must be green — includes the adverse-protection audit and GUI drift gate).
5. **Crypto patch**: `cd ~/Crypto && git apply ~/Omega/outputs/crypto_realfills_7d040d4.patch`
   (base 7d040d4; resolve trivially if drifted), rebuild the producers
   (shadow_refresh, shadow_refresh_intraday, wave_companion, stall_companion, live_mark, ibkrcrypto_bt),
   commit there with a message referencing this brief.
6. Commit the setting updates on the Omega branch, push, mark PR #3 ready, **merge to main**
   (only after canary green).
7. **Deploy**: `OMEGA.ps1 deploy` path on the VPS, verify running `Git hash:` == origin/main,
   check boot logs for `[SEED]` lines incl. the 12 `JumpRider` symbols and the six BE-floor books.
8. **Vault mandate (NOT optional)**: update `/Users/jo/Memory-Omega` — entity pages for the
   honest-accounting change (all six companions), JumpRiderEngine (new), the rider-sweep
   verdicts, crypto UpJump re-lock; index.md pointers; log.md entry (NZ time) naming the
   deployed commit.
9. Post-deploy: confirm each state JSON now carries `*_real` fields and `viable` flags
   (`/api/gold_companion`, `/api/jumprider`, etc.), and the wave companion stamped its
   forward anchor. The 30-day forward `*_real` gate is the go-live criterion — nothing
   flips LIVE in this session.

## Standing constraints (from CLAUDE.md — the usual, plus session-specific)

- Model-fill numbers are display-only everywhere now; never quote them as performance.
- No parameter picked from a single unsplit sample; IS/OOS + plateau or it doesn't ship.
- The alt-coin UpJump legs (DOGE/ADA/TRX/AAVE/NEAR/BNB/OP) are UNTESTED at the 4x48
  definition — leave at UpJump2, do not promote without their own sweep.
- Do not resurrect anything from the FAILED list because a sub-sample looks good.
