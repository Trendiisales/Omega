# FULL SYSTEM AUDIT — 2026-07-14 (Omega + ChimeraCrypto/Crypto)

Operator ask (handoff 14m): "ensure we have no more staleness issues, no dangling
references, circular code, duplications etc." Executed as 6 parallel read-only
audit agents (staleness-local, staleness-VPS, dangling-refs, circular/ODR,
duplications, standing-checks). NOTHING was fixed/mutated — findings only.

## VERDICT

**System is structurally healthy. Zero P1s.** No enabled-ungated engine, no
include cycles in any repo, no reachable ODR violation, no dangling ref in any
active cron/LaunchAgent/engine_init path, all mandated gates green, vault pins
current. The real findings are a short list of latent hazards + hygiene debt.

## RANKED FINDINGS

### P2 — latent, will bite when touched

1. **`~/.ssh/config` still resolves `omega-vps` → 185.167.119.59 (dead box).**
   Root enabler for every stale ref. Automation is repointed (no active cron
   hits it), but any human/tool typing `ssh omega-vps` lands on the retired box.
   Also `chimera-vps` → 154.45.251.118 alias still present.
   Fix: comment/remove both aliases (or point omega-vps at 45.85.3.79 with a
   loud comment). Cheap, closes the class.
2. **Hardcoded `omega-vps` in crypto-side scripts** (latent — invoking crons
   disabled): `Crypto/ops/backup_pull.sh:4`, `Crypto/gui/server.py:53` (+
   compiled `gui_server.cpp:220`), `ChimeraCrypto/companion/stall_accountant.py:152,178,416`.
   Fire at dead box the moment re-enabled/manually run.
3. **chimera-direct git history FORKED from origin**: box HEAD `008ee4b`, 14
   box-only vs 16 origin-only commits (deploy-script box-local commits create
   same-content twins under different hashes; merge-base 2aa93533). Live
   include/src content IDENTICAL to origin — no code risk — but hash-based
   box==origin verification is structurally broken, and the box's own
   check_box_sync.sh/deploy_to_box.sh differ from origin's. Needs deliberate
   reconcile (reset box onto origin at quiet moment).
4. **Warm-seed CSVs stale** (mtime lies — data ends earlier than file touch):
   worst `warmup_US30_H1/M15` last bar **2025-10-30 (~8.5mo)**, `warmup_GER40_M5`
   2026-01-01; broad H1/H4/M5 tier ends 2026-04-11. Only `warmup_M2K_H1` fresh
   (2026-07-09). Any warm-restart on those symbols/timeframes mis-seeds.
   Matches open vault items SeedFreshnessSafeguard / STALENESS_REGISTRY G3
   (seed_freshness_audit not on cron).
5. **3 Disabled omega-new scheduled tasks**: OmegaGapShortDaily,
   OmegaNYShadowTally, OmegaPumpShadow. Look like retired-family tooling but
   silent-disable class has bitten before — confirm intentional, record.
6. **ChimeraCrypto: 9 divergent same-qualified-name type definitions in
   orphaned legacy headers** (CapitalControlLayer ×2, InstitutionalEngine ×2,
   L2Book ×2, Level ×3, QueueEstimator ×2, BinanceWebSocket vs _NEW,
   LockFreeRingBuffer ×2, Regime ×2, LatencyTracker ×2). 0 TUs reach them
   today, but one careless include = AtomicL2-class failure. Cull candidates.

### P3 — hygiene debt

7. **backtest/ gitignore gap**: 22 compiled Mach-O binaries + `backtest/data/`
   + `backtest/_mimicext/` + `__pycache__/` untracked with no ignore rules.
8. **backtest/ untracked triage (90 entries)**: commit-candidates =
   `mimic_ladder_overlay.py` (registry-referenced) + 4 docs (ALLWEATHER_BOOK,
   MIMIC_REVERSAL_INTRADAY_FINDINGS, WEEKEND_RISK_LAYERS_FINDINGS,
   XS_ENGINE_WIRING); ~46 unreferenced experiment sources = operator decides.
9. **Selftest overlap**: feeds_selftest.py ↔ data_health_monitor.py = two
   independent staleness engines w/ overlapping feed lists — consolidate onto
   one feed registry. equity_daily_integrity's overlap w/ data_integrity_gate
   is documented-intentional (shared primitives could still be factored).
10. **backtest/ has 408 .cpp, zero shared headers**: 56 local `atr()`, 52 CSV
    loaders, 54 cost-gate copies. Fix to one copy doesn't propagate. Candidate:
    backtest/{indicators,csv_io,cost_gate}.hpp.
11. **Scripts defaulting to nonexistent `~/omega_repo`**: smoke_test_part_L.sh:89,
    S63_build_verify_compare.sh:35, plus patch_*/fix_* one-shots; runbook docs
    (OMEGA.md:71, IBKR_DATA_PULL.md:8, old handoffs) still say trader@185.
12. **Cron/launchd minor**: two `0 */4` jobs fire same minute
    (refresh_daily_feeds.py + qlib_refresh.sh); two yfinance close-refreshers
    overlap; stale archive copy of com.omega.cockpit.plist.
13. **Backtest sources w/ dangling includes of purged engines** (no longer
    compile): ShadowBookBacktest.cpp, fx_carry_momentum.cpp + ~6 fx_* others,
    omega_bt.BROKEN_STALE_INCLUDES.cpp (self-labeled).
14. **Dormant BeFloor python residue**: tools/gold_befloor_companion.py +
    install_gold_befloor_cron.sh exist, cron NOT installed. (BeFloor retirement
    itself verified correctly wired: enabled=false + selftest[7] enforces.)

### Doc drift (fix in CLAUDE.md when convenient)

- Ungated-audit allowlist stale by 3 (NqMomentum + SqueezeSlingshot tombstoned;
  StockDipTurtle gated via injected ExecutionCostGuard gate_fn — grep for
  literal "OmegaCostGuard" can't see injected gates).
- GoldEngineStack chokepoint "exactly 2 hits" → now 3 (extra = comment L4229;
  still single .open() call site).
- Adverse-protection counts 87/3/0 (doc says 86). Backfill owed: Donchian,
  EmaPullback, TrendRider.
- 12 AUDITED_CONFIGS DEAD verdicts absent from TOMBSTONES.tsv (guard still CLEAN).

## CLEAN BILL (verified, don't re-derive)

- Include cycles: **0** in Omega (819 files), Crypto, ChimeraCrypto.
- ODR: 15 namespace-scope defs in Omega all in deliberately single-TU headers
  (globals.hpp/omega_main.hpp/omega_types.hpp), each verified 1-TU. Omega
  sandbox header forks reach disjoint TU sets — benign.
- Ungated-engine audit: PASS in substance, 0 enabled-and-ungated.
- Mac canary: OmegaBacktest builds; 37 headers ok; all sub-gates green
  (tombstone-guard 13/22/0, mimic drawdown-cancel, pnl-completeness,
  trade-visibility 29 rows 0 gaps, GUI-drift, persistence, gold lot-size).
- omega-new: tree == running binary (69e13b5), gap to origin/main = docs +
  display_truth_selftest.py (Mac-side) only. No redeploy owed.
- Active Mac crontab 18/18 + LaunchAgents 5/5 targets exist; automation fully
  repointed to live boxes (PR#4 landed for the automation path).
- Vault pins current both systems; graphify stamp current (watch:
  .graphify_refresh.lock present 15:28 — confirm clears).
- engine_init.hpp: no config-for-deleted-engine; JumpRider fully purged.
- Duplicate-CSV scare = false positive (11 .claude/worktrees mirrors).
