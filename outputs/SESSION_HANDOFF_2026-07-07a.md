# SESSION HANDOFF 2026-07-07a — Honest-accounting fixes: BE-floor + companions become the primary profit stack

Operator ask: make the BE-floor/companion designs the primary profit makers, fix all
accounting defects (real fills, real costs, no overfitting, "no more bullshit"), show
before/after, advise culls, and prepare for the shadow→LIVE flip.

## What shipped in this branch (Omega repo)

1. **`5feca6b` — all six BE-floor companions book REAL fills + costs**
   (Gold / Xag / Usoil / Fx / Index / StockDayMover):
   - Trail exits book at **worse-of(floor, observed close)** — the floor is an order
     target, not a fill. Window flushes at the observed close.
   - New `Config.rt_cost_bp` (gold/xag 6, usoil/stock 8, fx 2, index 6) debited per clip.
   - Dual columns everywhere: `pts/pct/usd` = legacy model (unchanged, comparison only);
     `pts_real/pct_real/usd_real` = **the judgeable record — CAN be negative.**
     `wins` now counts real wins. State JSON, closed CSV, fwd book, open-leg uPnL all
     carry both columns (old files parse; real accrues forward from this deploy).
   - `ledger_fn_`/`close_fn_` receive the **real fill** — the shadow ledger can disagree
     with the model again (it was circular before).
   - Entry-anchor parity: window ref anchors the bar AFTER the detector signal
     (`win_pend_`, persisted) — matches the validated research (`ei = i+1` in every
     `*_befloor_ls.py` + `daymover_pername_screen.py:49`). Live previously armed one bar
     early off a different ref.
   - Gap guard: no NEW window on a non-contiguous detector span (weekend/outage gap
     masquerading as a "2h jump"). Exits still honoured.
   - Crash double-book fix: leg reset persisted inside `close_leg_`.
   - StockDayMover P0s: **short return was `entry/px-1`** (a 100→50 cover booked +100%
     instead of +50% — every Neg-book crash winner was convexly inflated); fixed to
     `(entry-px)/entry`. Poller-thread data race fixed (book mutex). Torn-read/split
     guard on the live daily feed (>50% single-day jump rejected + logged).

2. **`b1801bd` — StallCompanion pyfloat key backport** + transparent key migration
   (old `%.4f`-keyed .tsv state rebuilt from row fields — no ENGINE_EXIT/reopen churn).

3. **`outputs/crypto_realfills_7d040d4.patch`** — apply on the Mac to ~/Crypto @ `7d040d4`
   (`cd ~/Crypto && git apply <patch>`). Contains:
   - `wave_companion.cpp`: **ledger no longer re-appends the full 5.5yr history every
     cron run** (rewritten per run, exit stamped with the exit BAR time, not wall time);
     **real fills** (stacked companions arm at the triggering close, not the rung below
     market; COLD cut fills at worse-of(stop, close)); dual model/real columns;
     **forward anchor** (`anchor_ms` stamped on first run — `fwd_realized_usd_real` is
     the true forward record; the replay total is labelled what it is).
   - `shadow_refresh.cpp` + `shadow_refresh_intraday.cpp`: **atomic state.json writes**
     (tmp+rename — a torn read mass-ENGINE_EXITed every stall companion);
     **contracts/vt frozen at entry** (realized P&L was booked on a phantom quantity
     recomputed at close from today's pool split / corr scale / price);
     daily book regime gate now **NDX-only** (operator hard rule feedback-no-200dma-crypto —
     the BTC/ETH/SOL daily trend legs were still 200DMA-gated while the intraday book,
     allocator and wave engine were not; now consistent).
   - `stall_companion.cpp`: unreadable/unparseable CRYPTO_STATE now **skips the harvest**
     (books untouched) instead of being indistinguishable from "all trades closed".
   - `live_mark.cpp`: prices fetched first, state re-read FRESH before write — kills the
     read-fetch-write clobber race that could resurrect a closed trade.
   - vendored `crypto/StallCompanion.hpp`: same key-migration hardening as Omega.
   All patched sources compile clean (`g++ -fsyntax-only -std=c++17`, real nlohmann
   json header, curl stub for live_mark).

## Before / after

"Before" figures are the engines' own claimed/booked numbers. "After" is what the REAL
columns will report — **the data lives on the Mac/VPS, so the real numbers get produced
by the next replay/deploy, not invented here.** Directional expectations are analytic,
from the fill/cost mechanics; treat every one as "pending measured re-run".

| Book / engine | Before (model booking) | After (real fills + costs) | Expected direction | Action |
|---|---|---|---|---|
| BE-floor r20 "banker" (all 6) | 100% win, zero-loss clips | each BE-pinned clip books ~−rt_cost (gold −6bp, oil −8bp…); gap-throughs negative | typical clip ≈ cost ⇒ **likely net-negative in chop** | **cull candidate #1** — keep only if real column stays >0 over 30 fwd days |
| BE-floor r50 | zero-loss | same haircut, clip ≈ 6× cost | marginal | probation, judge on real column |
| BE-floor r100/r150/r400 | zero-loss, big runners | cost haircut small vs clip size; occasional gap-through loss | should survive with modest haircut | **keep — primary tiers** |
| StockDayMover Neg (shorts) | crash winners at `entry/px−1` | correct return + daily-gap fills + 8bp | **largest haircut of the family** (formula alone halves a −50% mover's booked win) | probation; judge per-name real column; earnings weeks are the risk |
| Wave companion BTC | replay $790,785 PF 1.41 (rung fills) | arm at trigger close (multi-rung bars fill worse), COLD at close | ~10–30% net haircut (estimate) — re-sweep required | keep if all-6 passes on real fills |
| Wave companion ETH | replay $81,130 PF 1.53 | same | same | keep if all-6 passes |
| Wave companion "results" | ledger re-appended full history every run; realized = 5.5yr replay | deduped ledger + `fwd_realized_usd_real` | forward record starts ~now | judge ONLY on fwd_real |
| Crypto shadow books (daily+intraday) | qty/vt recomputed at close | frozen at entry | forward fidelity fix, no history change | n/a |
| NDX TSMom50 + BE-ratchet | +95.6% PF1.78 (bt) | unchanged (already close-mark, costed in bt) | none | keep |
| Crypto trend ensemble (EMAx/Kelt ×3 coins) | OOS 1.56–2.48, costed bt | unchanged | none | **keep — core parents** |

## Cull list (recommendation, in order)

1. **r20 banker tier, all six BE-floor books** — clip size ≈ real cost; the "zero-loss"
   cohort is exactly the live bleed. Cull unless its real column proves otherwise fast.
2. **Intraday SQF perp mirror legs (22–28bp)** except `eth_donch40_sqf` — the repo's own
   note says Donch40 is the only strat that beats the perp cost wall; the rest duplicate
   the 2–8bp ladder legs at 3–10× the cost. Keeps the 34-leg zoo honest and small.
3. **BTC IBS (OOS 1.05) / SOL IBS (1.09)** daily legs — thin margins, break under
   vol-target, orthogonality is their only case. Monitor-only; cull on any real-cost
   forward underperformance.
4. **Older Omega directional engines**: judge on the shadow ledger's next 30 forward days
   *at real fills* — anything real-PF < ~1.1 after costs goes to disabled. The existing
   disabled/tombstoned graveyard stays dead (no resurrections).
5. Keep: trend ensemble parents, NDX legs, Regime diversifiers (first to revisit if the
   bear book thins), UpJump parents (companion feeders), BE-floor r100/r150/r400.

## The BE→companion architecture (the ask: "as soon as we cover BE, trigger the companion")

That trigger IS the BE-floor arm: leg opens only once price covers `be_bp` (= RT cost)
from ref, floor at entry, tiered trails take profit — that engine family is the
self-contained version and is now honestly booked. For parent engines (crypto trend /
UpJump), the identical pattern is the stall/wave companion with its arm gate set to
cost-cover: parent rides wide; companion #1 arms when parent uPnL ≥ RT cost; each
further +STEP arms another; COLD/trail floors each companion. Both halves now write
real-fill, cost-debited ledgers, so bull/bear viability is decided by
`*_real` columns, not by construction.

## Verification gates before the LIVE flip (no bullshit checklist)

1. Mac: `git apply outputs/crypto_realfills_7d040d4.patch` in ~/Crypto; rebuild both repos;
   canary + `OMEGA.ps1 deploy`; verify running hash; **update the Memory-Omega vault**
   (deploy mandate).
2. Re-run the model-fill harnesses with real-fill semantics (`*_befloor_ls.py`,
   `wave_ungated_all6_sweep.py` with arm@close + cost) — the old validations no longer
   certify the real column. Any tier/config that only passes on model fills is out.
3. Overfitting: re-lock params on data ≤ 2024 only, verify 2025–26 untouched; require a
   parameter plateau (±50% neighbors keep ≥ ~70% of net), not a spike. The current wave
   config was picked at the net-max of a full-history sweep — that specific check is owed.
4. 30 forward days of `*_real` columns green (or explicitly sized-down partial go-live).
5. LIVE-flip blockers that remain OPEN (not fixed this session): no resting broker stops
   (floor is polled per bar-close); exec callbacks fire from the stock poller thread
   (must be thread-safe before LIVE); FX/USDJPY + USOIL CFD $-scaling approximations;
   BE-floor state paths are cwd-relative (OMEGA.ps1 discipline covers it, but a wrong-cwd
   restart still orphans state).

## Files

- Omega commits: `5feca6b`, `b1801bd` on `claude/trading-engines-review-4gd67v`.
- Crypto patch: `outputs/crypto_realfills_7d040d4.patch` (against ~/Crypto `7d040d4`).
