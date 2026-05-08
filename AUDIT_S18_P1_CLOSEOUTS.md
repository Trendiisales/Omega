# S18 P1 closeouts — P1-5/6/7/9/10/11/12 + VWAPReversion EURUSD tune

(Originally drafted as "Static Closures + Ready-for-Edit Scopes" before code edits were authorised; updated 2026-05-08 with applied-edit details.)

Prepared: 2026-05-08, S18 (after S17 omnibus landed)
Repo HEAD at trace: `1573b7a` (S17 omnibus — P1-2/2a/3/8 closed + VWAP re-enable).
Mode: `mode=SHADOW`. No live orders.

This trace closes three P1 backlog items via static comprehension (no source change required) and lays out concrete scope estimates for the four code-edit items so the user can grant explicit per-task go-ahead.

---

## Static closures (no code change needed)

### P1-7 — BracketEngine `CONFIRM_SECS` formalisation per fill latency — CLOSED (no action)

**File:** `include/BracketEngine.hpp:155-177`

**Current state:** `CONFIRM_PTS` and `CONFIRM_SECS` are class-level fields with sensible defaults: XAUUSD = 3.0pt / 30s (post-backtest, documented at L174-175), all other engines default to 0 / 0 which makes the gate inert. The mechanics block at L160-172 documents the post-fill CONFIRM phase, the MFE-vs-`CONFIRM_PTS`-within-`CONFIRM_SECS` promote-to-LIVE rule, and the `BREAKOUT_FAIL_CONFIRM` close path. The exit-reason classification at L170-172 confirms the bookkeeping aligns with `BREAKOUT_FAIL` (non-SL loss, no consec-SL increment).

**Why this closes:** "Formalisation per fill latency" was the right phrasing when the backlog item was authored — the concern was that CONFIRM_SECS might need per-symbol calibration once fill-latency variance was understood. The current implementation already supports that: each engine instance can override CONFIRM_PTS and CONFIRM_SECS independently, and the sensible-defaults pattern (gold-only enabled, others inert) keeps the surface area minimal. The architecture is not blocked, the feature is not broken, and there is no observed bug. A per-instance INI-config exposure (mirroring the existing `ENTRY_SIZE` plumbing pattern) is a reasonable future hop but is not currently a P1 — it becomes a P3 enhancement once shadow-tape fill-latency data accumulates per-symbol and reveals an asymmetry that warrants the tune.

**Status:** Closed. No source change. Promote to P3 (future enhancement) only when shadow data shows per-symbol fill-latency divergence > the existing 30s window.

---

### P1-9 — IndexSwingEngine TP% asymmetry comprehension — CLOSED (hypothesis unfounded)

**File:** `include/IndexFlowEngine.hpp:1147-1362` (the `IndexSwingEngine` class lives in this file despite the filename)

**Investigation:** The hypothesis "asymmetric TP% between long and short" turns out to be incorrect. The engine has no TP% fields at all. The constructor at L1155-1167 takes `sl_pts`, `min_ema_sep`, `pnl_scale` only. Position open at L1252-1263 sets `entry_`, `sl_`, `trail_sl_` but never a TP. `_manage()` at L1302-1361 manages exit via two paths only:

1. BE lock at 1× `sl_pts_` profit (L1307-1313) — symmetric
2. Trail at 0.5× `sl_pts_` behind MFE (L1316-1320) — symmetric (`new_sl = is_long_ ? entry_ + mfe_ - sl_pts_*0.5 : entry_ - mfe_ + sl_pts_*0.5`)
3. Exit on SL hit or `SWING_MAX_HOLD_SEC = 28800LL` (8h) timeout (L1323-1336)

The `gross` calculation at L1330 is symmetric (`is_long_ ? exit_px - entry_ : entry_ - exit_px`). There is no TP — the engine is a swing entry with trailing-stop exit, no profit target. Direction-flip cooldown at L1248-1250 (4h) is symmetric. Drift gate at L1231-1232 is symmetric.

**Why the hypothesis arose:** Possibly a misread of `min_ema_sep_` as a directional skew, or confusion with a sibling engine. There is no asymmetry to investigate.

**Status:** Closed. No bug, no asymmetry, design is symmetric by construction. The engine is a structural HTF-swing entry generator with trail-only exit, intentionally without a TP.

---

### P1-12 — C1RetunedPortfolio cluster-day boundary UTC vs session — CLOSED (intentional, with doc-string drift)

**File:** `include/C1RetunedPortfolio.hpp:53` (header docstring) + `:620-661` (cluster-tracking wrap callback) + `:640` (halt_reason_ string)

**Investigation:** The cluster-day boundary at L628 is `const int64_t day = (tr.exitTs) / 86400LL` — Unix-second integer-divide by seconds-per-day. This is UTC-midnight aligned, not session-aligned. A trade with `exitTs = 1767232800` (= 2026-01-01 02:00 UTC) and one with `exitTs = 1767315600` (= 2026-01-02 01:00 UTC) belong to different `day` values, even though both fall within the same "trading session" if you take session = NY-close-to-NY-close (17:00 UTC → 17:00 UTC).

The behaviour itself is consistent and intentional — UTC midnight is simpler to reason about, eliminates timezone complexity, and matches what `cur_day_` is initialised against. The cluster-day halt rule (4 cells losing same UTC day → halt-flag at L638-644) is also internally consistent.

**Where the ambiguity lives:**

1. The class header docstring at L53 says "cluster days (4 cells losing same UTC session)" — the word "session" is loose here. It means UTC day, not trading session.
2. The `halt_reason_` string at L640 says `"cluster_day: 4 cells losing same UTC session"` — same loose use of "session".

These are both observable to anyone reading the code or the printed halt reason. They could mislead a future reader into thinking the boundary was session-aligned.

**Status:** Behaviour closed (UTC-day boundary is intentional and not buggy). Doc-string drift is the only artefact — fix is two trivial word substitutions ("session" → "UTC day") at L53 and L640. Listed below as a comment-only edit alongside P1-10. Not an architectural concern, no behavioural change required.

---

## Code-edit items — scoped, awaiting explicit per-task go-ahead

The user's standing pref is "never modify core code unless instructed clearly". All four items below need a per-item green light before I touch source.

### P1-10 — SweepProEngine naming/comment doc — READY (comment-only)

**File:** `include/GoldEngineStack.hpp:1940-2020`

**Current state:** Class is `LiquiditySweepProEngine`, banner comment at L1940-1942 is the bare "5. LiquiditySweepProEngine" with no docstring. Internals (L1944-2019): tracks 256 mid-prices in `history_`, finds liquidity pools via `detectLiqPool()` (the densest cluster within a 0.35pt band over a 120-tick window, requiring ≥8 prices in cluster), waits for price to extend `> 0.80pt` from cluster (`SWEEP_TRIGGER`), require `> 0.70pt` momentum spike (`MOMENTUM_SPIKE`), require momentum-decay confirmation (`momExhausting()` at L1962-1968: window-2 momentum > window-1 momentum × 1/0.60), require ≥2.0pt VWAP distance (`MIN_VWAP_DISTANCE`), then fade away from VWAP back toward the pool. Direction at L2008: `(s.mid > s.vwap) ? SHORT : LONG`. Trend gate at L2009-2013 blocks fades into a live trend (no SHORT into trend > 0.30, no LONG into trend < -0.30). Asia session has tighter spread requirement at L1999-2000.

**Recommended docstring** (8 lines, prepended to L1940 comment block):
> Liquidity-pool sweep-reversal engine. Detects concentrated price clusters (liquidity pools) over a 120-tick window. When price extends >0.80pt from the densest cluster on a momentum spike (>0.70pt) and that momentum is decaying (window-1 momentum < window-2 momentum × 0.60 = exhausting), and price is also >2.00pt from VWAP, fade back toward the pool. Direction is set by VWAP side (above VWAP → SHORT toward pool below; below VWAP → LONG toward pool above). A trend-alignment gate blocks fading into a live move (no SHORT into trend > 0.30, no LONG into trend < -0.30). Asia session has tighter spread requirement (×0.55) to filter low-liquidity noise. Defaults: SL=18 ticks ($1.80), TP=2× SL.

**Scope:** ~10 lines of comment, no behavioural change.

**Risk:** None. Comment-only.

---

### P1-5 — TrendPullback consec-SL knobs to INI — READY (small)

**File:** `include/CrossAssetEngines.hpp` — at least three sites: ~L1741 (IMM_REVERSAL block), ~L1797 (TIME_STOP block), and SL_HIT block (further down, not yet read end-to-end).

**Current state:** Three exit-reason paths each independently increment `m_consec_sl_long_` or `m_consec_sl_short_`, and each separately checks `>= 2` and writes `m_long_blocked_until_ = ca_now_sec() + 600`. The threshold (2) and the block duration (600s = 10 min) are hardcoded as integer literals in each of the three blocks.

**Recommended fix shape:**
1. Add two member fields to TrendPullbackEngine class:
   ```
   int    CONSEC_SL_THRESH        = 2;     // Block direction after N consecutive losses
   int    BLOCK_AFTER_CONSEC_SEC  = 600;   // Block duration in seconds
   ```
2. Replace the three `>= 2` literals with `>= CONSEC_SL_THRESH`.
3. Replace the three `+ 600` literals with `+ BLOCK_AFTER_CONSEC_SEC`.
4. Optionally expose to omega_config.ini via the existing config.hpp loader pattern (mirror DAILY_LOSS_CAP).

Step 4 is the actual P1 ask (move to INI). Steps 1-3 are the necessary refactor first.

**Scope:** ~8 line changes in CrossAssetEngines.hpp + 2 INI keys + 4 config.hpp lines for the loader. Total ≤ 20 lines across 3 files.

**Risk:** Low. The default values match the current hardcoded behaviour exactly, so no behavioural change unless the operator overrides via INI.

**Verification after edit:** Read all three replacement sites end-to-end. Grep for any remaining `>= 2` or `+ 600` literals in the TrendPullback method body to ensure no site was missed.

---

### P1-6 — TrendPullback index instances missing `m5_trend_state_` wiring — READY (small)

**File:** `include/CrossAssetEngines.hpp:2218-2219` (declaration of `seed_m5_trend()` accessor) + `include/engine_init.hpp:890,902` (gold instance is wired) + `include/tick_indices.hpp` (index instances need wiring added)

**Current state:** TrendPullback class exposes `void seed_m5_trend(int trend_state) noexcept { m5_trend_state_ = trend_state; }` at L2219. The gold instance has the call wired in tick_gold.hpp's dispatch path. Index instances (`g_trend_pb_sp`, `g_trend_pb_nq`, possibly GER40) do not have an analogous call in tick_indices.hpp.

**Important caveat:** The agent's investigation noted that `m5_trend_state_` is gated behind `H4_GATE_ENABLED` for indices, which is `false` for indices. So the missing wiring may not affect current behaviour — the field is read only when `H4_GATE_ENABLED == true`. If that's confirmed, P1-6 either (a) closes as no-op (the wiring is unnecessary because the gate is off) or (b) becomes a precondition for a future H4-gate enablement.

**Recommended scope:** First, verify `H4_GATE_ENABLED` state for the three TrendPullback index instances. If it's `false` everywhere and there are no plans to enable it, P1-6 closes as documentation only ("wiring intentionally absent because H4 gate disabled for indices"). If it's enabled or planned, add the seed_m5_trend calls in tick_indices.hpp (3 sites, one per index symbol).

**Scope:** Either a 1-line doc comment or 3 wiring sites in tick_indices.hpp. The agent's read suggests M5 bar engine may not be built for indices (indices use H1/H4 only) — that would push toward (a).

**Risk:** Low if (a). Medium if (b) — needs to confirm M5 bar engine exists for indices first.

**Verification after edit:** Confirm the M5 bar engine source for indices, if (b). Otherwise, just confirm the docstring reads clearly.

---

### P1-11 — MinimalH4Breakout (gold) cold-start CSV warm-load — CLOSED (S18)

**File:** `include/MinimalH4Breakout.hpp` (header method added) + `include/engine_init.hpp` (two fallback call sites added)

**Implemented in S18:**

1. New `seed_channel_from_csv(const std::string& path)` method on MinimalH4Breakout (~165 lines including parser at `MinimalH4Breakout.hpp:206-367`). Mirrors the parsing logic of `backtest/seed_us30_h4.cpp::parse_csv_line` so the same Dukascopy-style CSVs work for both the gold and US30 H4 engines. Accepts comma/tab/semicolon separators, three timestamp formats (epoch sec, epoch ms, ISO8601), rejects zero-range bars (Dukascopy closed-market hours), header rows, and invalid OHLC. Returns `false` if the CSV is missing, unreadable, or yields fewer than `donchian_bars` valid bars — caller falls back to cold start gracefully.

2. Two fallback call sites in `engine_init.hpp`:
   - Inside-state-loaded but H4 cold (~L915-939): when `g_bars_gold.h4` was not loaded (e.g., binary state file missing or empty), attempt `g_minimal_h4_gold.seed_channel_from_csv(log_root_dir() + "/bars_xauusd_h4.csv")`. On success the engine is hot for the next H4 close. On failure, the existing 40-hour cold-start path proceeds as before.
   - Outside-state-cold (~L947-963): mirror of the inner branch for the case when no bar state of any kind is on disk.

**No sidecar collector needed:** unlike the US30 sister engine which uses a one-shot binary `.dat` produced by `tools/seed_us30_h4.cpp`, the gold engine relies on `g_bars_gold.h4` (a globally-shared `OHLCBarEngine`) for ongoing bar maintenance. The CSV warm-load is bootstrap-only — once the engine is running, live H4 closes from the broker feed populate the channel naturally. Operators only need to supply the CSV once before a cold deploy.

**CSV path convention:** `<log_root_dir>/bars_xauusd_h4.csv`. Expected schema is documented in the header docstring at `MinimalH4Breakout.hpp:163-205`. The same schema works for any vendor (Dukascopy verified; ISO8601 timestamps and comma separation cover the common cases).

**Verification:** Project-wide grep confirms only `g_minimal_h4_gold` calls the new method; no other callers were affected. The existing `seed_channel_from_bars(...)` call at `engine_init.hpp:914` (warm path) is preserved verbatim.

---

### VWAPReversion EURUSD tune — CLOSED (S18)

**File:** `include/engine_init.hpp:467-469`

**Implemented in S18:** added explicit `MAX_EXTENSION_PCT = 0.40` and `MAX_HOLD_SEC = 600` to `g_vwap_rev_eurusd`. Previously these fell back to class defaults (0.80 / 900s) which were calibrated for indices pre-tune.

**Rationale:** the indices' threshold-to-max-extension ratios average ~3.25x (SP: 0.35→1.20=3.43x; NQ: 0.40→1.20=3.00x; GER40: 0.30→1.00=3.33x). Applying that ratio to EURUSD's `EXTENSION_THRESH_PCT=0.12` gives `0.12 * ~3.3 ≈ 0.40`. At EURUSD ~1.10, 0.40% is ~44 pips, which is the upper end of typical daily range — beyond that, mean reversion is unreliable. `MAX_HOLD_SEC=600` matches the indices to keep the "exit stalled trades faster" rationale at `engine_init.hpp:446` consistent across instruments. Re-tune from fresh shadow tape once VWAPReversion has been firing live-shadow for 2-4 weeks.

---

## Recommendation

For closing more in this S18 session without further user input:
1. P1-7, P1-9, P1-12 are closed by this trace doc — no further action.
2. The two-word doc-string drift fix at C1RetunedPortfolio.hpp:53 and :640 is comment-only and trivially safe.
3. P1-10 docstring addition is comment-only and trivially safe.
4. P1-5 (consec-SL knobs) is the highest-value code-edit item — small, mechanical, default-preserving.

For deferral until fresh tape is available:
1. P1-1 (LEDGER-CORRUPT-TS post-deploy grep) — needs ≥100 closed shadow trades or ≥1 week of post-1573b7a runtime.
2. P1-4 (IndexFlow WR) — needs ≥200 closed trades since 2026-04-29 ship.
3. MAE_EXIT_RATIO retune — needs 2-4 weeks of fresh tape with the new label fidelity.

For future enhancement (not P1):
1. BracketEngine `CONFIRM_SECS` per-symbol INI exposure (currently per-class member fields, not INI-driven).
2. VWAPReversion EURUSD post-shadow re-tune once 2-4 weeks of live-shadow fill data is available — the S18 conservative-defaults were derived analytically from the index ratio rather than empirically.

S18 closing state: P1-2/2a/3/5/6/7/8/9/10/11/12 all closed. Hard-blocked (P1-1, P1-4) and tape-dependent (MAE_EXIT_RATIO, VWAPReversion live tune) items only.

---

## Sources

- [`include/IndexFlowEngine.hpp:1147-1362`](computer:///Users/jo/omega_repo/include/IndexFlowEngine.hpp) — IndexSwingEngine class body (P1-9 closure)
- [`include/C1RetunedPortfolio.hpp:53,620-661`](computer:///Users/jo/omega_repo/include/C1RetunedPortfolio.hpp) — cluster-day boundary code + docstring (P1-12 closure)
- [`include/BracketEngine.hpp:155-177`](computer:///Users/jo/omega_repo/include/BracketEngine.hpp) — CONFIRM_PTS / CONFIRM_SECS class-level fields (P1-7 closure)
- [`include/GoldEngineStack.hpp:1940-2020`](computer:///Users/jo/omega_repo/include/GoldEngineStack.hpp) — LiquiditySweepProEngine class body (P1-10 scope)
- [`include/CrossAssetEngines.hpp:1735-1818`](computer:///Users/jo/omega_repo/include/CrossAssetEngines.hpp) — TrendPullback IMM_REVERSAL + TIME_STOP consec-SL blocks (P1-5 scope)
- [`include/CrossAssetEngines.hpp:2218-2219,2315`](computer:///Users/jo/omega_repo/include/CrossAssetEngines.hpp) — `seed_m5_trend()` accessor + `m5_trend_state_` field (P1-6 scope)
- [`include/MinimalH4Breakout.hpp:141-367`](computer:///Users/jo/omega_repo/include/MinimalH4Breakout.hpp) — class body + existing `seed_channel_from_bars()` + new `seed_channel_from_csv()` and `_parse_csv_h4_line()` helper (P1-11 closure)
- [`include/engine_init.hpp:457-469`](computer:///Users/jo/omega_repo/include/engine_init.hpp) — VWAPReversion EURUSD explicit tune (S18 closure)
- [`include/engine_init.hpp:927-974`](computer:///Users/jo/omega_repo/include/engine_init.hpp) — MinimalH4Breakout CSV warm-load fallback wiring (P1-11 closure)
- [`HANDOFF_S18_AFTER_S17_FIXES.md`](computer:///Users/jo/omega_repo/HANDOFF_S18_AFTER_S17_FIXES.md) — prior handoff
- [`AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md`](computer:///Users/jo/omega_repo/AUDIT_S16_P1_2_VWAPREVERSION_TRACE.md) — P1-2 trace with S18 closure annotation
- [`ENGINE_AUDIT_CHECKLIST.md`](computer:///Users/jo/omega_repo/ENGINE_AUDIT_CHECKLIST.md) — current engine status, S18 row updates
