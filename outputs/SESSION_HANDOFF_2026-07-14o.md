# SESSION HANDOFF 2026-07-14o — warm-seed fixes SHIPPED + seed-registry structural gate (operator "never again")

## WHAT THIS SESSION DID (resumed 14n queue item 1 — DONE, then operator escalated to systemic)

### Commit `55f66198` (S-2026-07-14ag) — the 2 audit items, end-to-end
- **warmup_USTEC.F_H4.csv** (was 94d stale; seeds 2 ACTIVE Survivor cells USTEC_4h_RSI_N7 +
  USTEC_4h_ZMR via the dynamic path SurvivorPortfolio.hpp:808): rebuilt fresh from the VPS
  nightly NQ H1 pull (agg → H4, 396 bars to 13-07, sec-ts, CERTIFIED CLEAN) + nightly recipe
  added to seed_refresh `_INDEX` ("USTEC.F" → CME NQ H4). **`_SEC_TS` matters:** Survivor's
  `Cell::seed_from_csv` reads ts as SECONDS verbatim (no ms-norm) — recipe writes ms=False.
- **warmup_GBPUSD_H1.csv** (live FxLadder GBPUSD cell, engine_init.hpp:1882; data ended
  2026-04-11; hidden by the FX skip-regex): rebuilt 9000 bars = Tick/GBPUSD_IBKR_H1.csv (3Y
  IDEALPRO, →09-07) + VPS live ladder bars (fxladder_companion_gbpusd_h1.csv, →14-07 03:00),
  CERTIFIED CLEAN + nightly `_FOREX` recipe (IDEALPRO **MIDPOINT** — forex has no TRADES).
- **Audit blindness fixed:** dynamic-Survivor parser (synthesizes paths from active `add({...})`
  roster) in BOTH audit copies; `DISABLED` regex `GBPUSD` → `GBPUSD(?!_H1)`; SPXUSD skip synced
  into seed_refresh's copy (it had DRIFTED — existed in tools copy only) w/ updated rationale
  (Survivor re-enabled but no SPXVolGatedDonch cell active).
- warmup_XAUUSD_H4 snapshot pulled fresh (dynamic scan newly surfaces it).

### Commit `f108d512` (S-2026-07-14ah) — the never-again layer
- **NO-REFRESH-PATH structural gate** in tools/seed_freshness_audit.py: every ACTIVE seed
  (literal + dynamic) must be refreshed by seed_refresh.py — refresh set derived by IMPORTING
  its tables (`_REBUILD_TARGETS/_GOLD_TFS/_INDEX/_FOREX/_ALIASES`), zero hand-copy — or carry a
  `KNOWN_UNREFRESHED` owner entry (XAUUSD_M1 histdata-monthly / sp500_long_close →
  OmegaStockMoverFeed / mgc_30m_live → live producer). Violation = **exit 3**.
- **`--registry-only`** (structural only, deterministic) wired into `scripts/mac_canary_engines.sh`
  → a new seed with no refresh path FAILS THE PRE-COMMIT CANARY. Negative-tested (recipe removal
  → rc=3).
- **Single audit copy:** seed_refresh.py phase-3 inline duplicate DELETED → subprocess-delegates
  to tools/seed_freshness_audit.py, exit code passthrough. Nightly task and deploy gate can
  never diverge again.
- **NEW FIND fixed en route: `data/mgc_h4_hist.csv`** — boot-seeds ENABLED `g_mgc_tf_4h`
  (omega_main.hpp:203, warmup_or_die); MgcFastDonchianFeed.hpp says "regenerated at deploy" but
  NO regenerator existed anywhere; static since 07-07, would cross 14d threshold ~21-07.
  Nightly regeneration added (MGC H4 pull, H1-agg fallback, keeps bar_start_ms header).
- **Mac-vs-VPS semantics:** VPS copy load-bearing (stale there = fail/abort, unchanged); Mac git
  snapshots of nightly-VPS-refreshed seeds now `[snapshot-lag]` WARN (killed ~21 false STALE
  lines that drowned real signals). `[orphan-refresh]` warn for consumer-less recipes.
- **Collation:** all 32 nightly-refreshed snapshots scp'd from omega-new + committed. Mac audit:
  0 stale / 0 lag. CLAUDE.md rule added under Warm-Seed Mandate.

### Deploy state
- omega-new tree == origin/main == **f108d512**; NO binary change (python/CSV/doc only, no
  rebuild/restart needed). VPS-side verified: full audit GREEN (0 stale), `--registry-only`
  GREEN, seed_refresh `--only audit` delegation works.
- VPS working-tree seed dirt reconciled (checkout-then-pull; committed bytes WERE the VPS
  bytes). Only remaining VPS dirt: `symbols.ini` (pre-existing, not touched).
- Vault: SeedFreshnessSafeguard.md new 14-07 section + index.md + log.md; pin → f108d512.

## WATCH (new, tonight)
- **23:30 OmegaSeedRefresh = FIRST live exercise of the 3 new recipes.** Deliberately NOT run
  midday (same-day gateway bulk-pull incident killed orders — memory
  feedback-no-bulk-pulls-production-gateway). Tomorrow verify log shows
  `refreshed (…): USTEC.F_H4(…), GBPUSD_H1(…), mgc_h4_hist(…)` and no new `skipped/failed`.
  If GBPUSD qualify fails (IDEALPRO perms), seed stays at today's fresh snapshot — not urgent,
  but fix the recipe.
- Inherited 14n watch items unchanged (CLIP-JF PJ opens, bigcap ladder windows, IBKR-EXEC
  XAUUSD.M paper fills).

## NEXT SESSION QUEUE
1. **Verify tonight's seed_refresh first-run** (above).
2. **Orphan-refresh recipes** (4, warn-only): warmup_DJ30_H1 / ESTX50_M5 / GER40_H4 / NAS100_M5
   refreshed nightly but consumer-less — drop recipes or note why kept (each = a nightly
   gateway request).
3. **Delete-candidates (14n item 1c, still open):** warmup_US30_H1/M15 + warmup_NAS100_H4 exist
   on disk with zero code consumers (US30_H1 has only a stale TODO comment at
   engine_init.hpp:2769 — Us303BarMom is disabled). Operator keep/delete call; harmless either way
   (not audited, not refreshed).
4. **chimera-direct git reconcile** (operator window) — unchanged from 14n.
5. **backtest/ source triage** (75 untracked) — unchanged from 14n.
6. Audit P3 list — unchanged from 14n (feeds_selftest↔data_health_monitor consolidation now has
   a pattern to follow: the seed audit's import-the-tables single-source approach).

## TRAPS (carry forward + new)
- **Seed formats:** Survivor cell loader = SECONDS only (no ms-norm); FxLadder norm_ts_ + XauTF4h
  loaders tolerate both; mgc_h4_hist keeps `bar_start_ms` ms header. `_SEC_TS` set in
  seed_refresh controls per-symbol units — check it before adding any Survivor-consumed recipe.
- **VPS pulls touching seed CSVs:** VPS working tree is ALWAYS dirty on nightly-refreshed seeds
  by design. `git checkout -- <those paths> && git pull --ff-only` is the safe pattern ONLY when
  the incoming commit contains equal-or-fresher copies (this session: bytes came FROM the VPS).
- Live boxes: omega-new 45.85.3.79 / chimera-direct 143.198.89.54; omega-vps/chimera-vps aliases
  DISABLED. VPS ssh form: literally `ssh omega-new "..."`.
- zsh eats bare `===` in Bash tool calls; BSD awk has no strftime.
- Immediate-entry ban stands EXCEPT 4 live PJ cells; BeFloor stays retired.
