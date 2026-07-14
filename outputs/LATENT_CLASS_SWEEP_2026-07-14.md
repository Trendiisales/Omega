# LATENT-CLASS SWEEP 2026-07-14 — "what else is hiding like the seed bug"

5 parallel read-only agents, one per failure class that produced past incidents
(venue-basis leftovers, comment-claims-mechanism, skip-list rot, hand-synced
duplicates, static-scan blindness). All findings verified with file:line by the
agents; nothing modified.

## WHY THESE KEPT GETTING MISSED (the 5 mechanisms)

1. **Second copies** — a change lands at the obvious point, the duplicate keeps the
   old assumption (OmegaCostGuard fixed / GoldEngineStack gate forgotten; tools
   audit fixed / seed_refresh copy drifted).
2. **Comment-as-mechanism** — comment claims automation; nothing executes it
   ("regenerated at deploy" mgc_h4_hist = fiction; "kept in sync" = drifted).
3. **Absence-blindness** — monitors catch bad states, not missing activity
   (zero gold trades = "quiet market"; unaudited seed = green).
4. **Skip-list rot** — skip justified by "X disabled" outlives X's re-enable
   (GBPUSD, SPXUSD).
5. **Static scan vs runtime construction** — grep sees literals; system builds
   paths/names/enables at runtime (Survivor seed paths, injected gates).
Root: every check was shaped like the LAST incident and the checks themselves
were never negative-tested (ExitCode bug left the seed deploy-gate inert for weeks).

## FIX QUEUE (priority order)

### P1 — live money / fail-open
1. **`data/vix_term_ratio.txt` gate is FICTION-fed, fails OPEN.** 6 IndexSeasonal
   engines `gate_by_vix=true` (engine_init.hpp:3022, IndexSeasonalEngine.hpp:84).
   Producer `tools/fetch_vix_ratio.py` exists, NO scheduler anywhere. Stale file →
   engines trade UNGATED. Already flagged in STALENESS_REGISTRY.md gap G5, never
   actioned. Fix: schedule producer (VPS task or Mac cron) + feeds_selftest row +
   consider fail-closed on stale.
2. **Gold lot-size canary gate blind to `include/omega_main.hpp`.** 5 MGC engines
   `lot = 1.0` (omega_main.hpp:97,115,152,191,208), 2 LIVE non-shadow (fastdon:97,
   volbrk:152). Values intentional (1 MGC micro) but unenforced — fat-finger
   `lot=100` there ships. Fix: extend gate to omega_main.hpp + `LOT-GATE-OK`
   annotations on the 5 lines.
3. **Live XAUUSD cost-guard calls pass BlackBull FEED spread while exec=IBKR** —
   systemic GoldCostGateIBKR residue. tick_gold.hpp:675 + :1516 (dispatch +
   GoldBracket), GoldVolBreakoutM30Engine.hpp:397 (only LIVE gold intraday
   engine). OmegaCostGuard.hpp:60-63 assumes a tight futures spread is supplied;
   callers violate. Partial phantom veto on marginal entries (no zero-trade
   symptom: 1.5x hurdle + max_spread prefilter mask it). Fix pattern in-repo:
   MgcSlowDonchian passes exchange tick 0.10, not feed spread. C++ change →
   canary + deploy.

### P2 — traps that will bite next incident
4. **`tools/refresh_warmup_seeds.py` = zombie pre-fold copy** with stale roster
   (missing USTEC/USTEC.F/M2K/NAS100_D1/DJ30_H1/GBPUSD/_ALIASES) AND
   `VERIFY_STARTUP.ps1:1002` still names it as the operator remediation command →
   "ran the fix, still RED" trap. Fix: delete file + repoint hint to
   `tools/seed_refresh.py --only ibkr`.
5. **MondayRiskOn invisible to persistence audit** — display source registered
   with runtime-built name (engine_init.hpp:5557 `register_source(eng->engine_name)`),
   enabled=true shadow, NO persist wire, not allowlisted. Same dynamic-name class
   as Survivor seeds. 2 more dynamic register_source wrappers (6798, 7465 —
   dormant instances today). Fix: persist-wire MondayRiskOn + teach audit the
   dynamic shapes (or ban runtime names).
6. **`data/index_regime.txt`** producer `tools/fetch_macro_regime.py` unscheduled
   (IndexRiskGate.hpp:19 claims "daily"). Fails safe (risk_off()=false) but gate
   silently dead. Schedule with #1 (same producer family).
7. **Adverse-protection audit glob misses `*Engines.hpp`/`*Stack.hpp`** —
   GoldEngineStack (LIVE g_gold_stack, inline sub-engine classes, 0 tags) never
   scanned; CrossAssetEngines/HTFSwingEngines/SweepableEngines/LatencyEdgeEngines
   also outside glob. Fix: widen glob + annotate or allowlist w/ reasons.
8. **Hygiene:** `scripts/adverse_protection_legacy.txt` header claims "EMPTY/
   backfill complete" while holding 3 entries (fix comment or finish the 3);
   `persistence_audit.sh` dead allowlist entry `FxCrossRevEURGBP` (source
   un-registered S-2026-06-29); `SURV_ADD` parser in seed_freshness_audit.py needs
   a parsed-count>0 guard (format change → silent zero → blind again).

### P3 — structural hardening (pattern: derive-don't-copy, like the seed fix)
9. feeds_selftest vs data_health_monitor: 7 shared files with CONTRADICTORY
   thresholds (2 trading-days vs 4 calendar) + disjoint coverage → one registry,
   one consumer imports the other.
10. Ungated-engine audit greps only 2 opener idioms — MgcFast/MgcSlow (`pos_active_
    = true`), CrossSectionalIndex (`legs_.push_back`), GoldTsmomD1V2 (`w_ = want`)
    have ZERO coverage (all gated today). Widen ENTRY_RE to match adverse audit's.
11. fail_verdict_guard only sees `g_X.enabled=true`; sleeve/cell `e.enabled=true`
    shape (engine_init.hpp:3019 etc.) evades. Widen when any sleeve engine gets a
    FAIL verdict.
12. engine_state_audit.py thr() drifted (missing XAUUSD_M1 45d override) → false
    STALE noise; import OVERRIDES from seed_freshness_audit.
13. Residual `omega-vps` refs: tools/rdagent/refresh_close_ibkr.py:8,
    ram_workingset_trim.ps1, prune_hung_ssh_queries.ps1 (dead-box class).
14. ibkr_dom_bridge STOCKS vs BIGCAP_LAD roster (in-sync today, hand-mirrored);
    INDEX_FUTURES contract map in 3 files; mimic gate glob misses future
    `*Ladder.hpp`-only names; orphan-refresh recipes (DJ30_H1/ESTX50_M5/GER40_H4/
    NAS100_M5) consumer-less.

## VERIFIED CLEAN
ExecutionCostGuard IBKR rows (incl MGC/GC), mimic-book conservative costs,
seed skip-regex post-fix, persistence shadow books all still shadow,
FAIL_VERDICTS/TOMBSTONES consistent with enabled=false, mgc-hist claims now real,
MacroGoldGate/log-rotation/news-blackout automation real, no residual dynamic
seed-path instance outside Survivor (now covered).
