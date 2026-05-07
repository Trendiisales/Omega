# INVENTORY_VERIFICATION_2026-05-07

Phase 1 of the engine audit. Cross-references three sources of truth — `include/globals.hpp` (declarations), `include/engine_init.hpp` (`enabled` / `shadow_mode` flags + `register_engine` calls), and the tick dispatch hot path (`tick_*.hpp` / `on_tick.hpp` / `quote_loop.hpp`) — to assign every `g_*` engine instance a definitive runtime status.

**Scope this pass:** classification only. No code changes. Phase 2 (per-engine PnL pull from VPS shadow log) and Phase 4 (zombie removal) are out of scope and require explicit instructions before action.

**Status taxonomy used below:**

- **LIVE** — wired in dispatch, `enabled=true`, `shadow_mode=false`. Places real orders.
- **SHADOW** — wired in dispatch, `enabled=true`, `shadow_mode=true`. Generates trade records but no real orders.
- **DISABLED** — wired in dispatch but `enabled=false` (or guarded by `g_disable_*=true`). Existing positions still managed; no new entries.
- **DEAD** — declared in `globals.hpp` but no `.on_tick()` / `.evaluate()` call exists anywhere in the dispatch hot path. Pure zombie globals consuming binary size and confusion.
- **MANAGE-ONLY** — wired in dispatch but only the position-management branch is reached; new-entry path is gated off via a separate disable mechanism.

---

## 1. Headline counts

| Status | Count |
|---|---|
| LIVE (real orders) | 0 (none in the cohort I can directly verify; some Tier-1+ engines may be live via `kShadowDefault=false` config — see open questions) |
| SHADOW (paper) | 21 |
| DISABLED (`enabled=false` or disabled flag set) | 18 |
| MANAGE-ONLY | 2 (`g_bracket_gold`, `g_candle_flow`) |
| DEAD (no dispatch path) | 27 |
| **Total declared in `globals.hpp`** | **~68** |

The headline finding: **~40% of engine globals declared in `globals.hpp` are dead — declared, sized in the binary, but never reached from any tick path.** Removing them is a low-risk cleanup pass.

---

## 2. Compression-cohort (FX + Gold session-open) — patched this session

All six are SHADOW (the `shadow_mode=true` is hard-set in `engine_init.hpp` and the engine's own header default also pins it to true). All six have S58 cold-start guard, S59 fill-spread reject, and S61 range-history persistence as of this session.

| Instance | Class | Status | enabled | shadow_mode | Dispatch site |
|---|---|---|---|---|---|
| `g_gold_midscalper` | `GoldMidScalperEngine` | **SHADOW** | (always-on) | true (`engine_init.hpp:69`) | `tick_gold.hpp:2136-2184` |
| `g_eurusd_london_open` | `EurusdLondonOpenEngine` | **SHADOW** | (always-on) | true (`engine_init.hpp:77`) | `tick_fx.hpp:122-138` |
| `g_usdjpy_asian_open` | `UsdjpyAsianOpenEngine` | **SHADOW** | (always-on) | true (`engine_init.hpp:88`) | `tick_fx.hpp:264-280` |
| `g_gbpusd_london_open` | `GbpusdLondonOpenEngine` | **SHADOW** | (always-on) | true (`engine_init.hpp:99`) | `tick_fx.hpp:196-212` |
| `g_audusd_sydney_open` | `AudusdSydneyOpenEngine` | **SHADOW** | (always-on) | true (`engine_init.hpp:110`) | `tick_fx.hpp:339-355` |
| `g_nzdusd_asian_open` | `NzdusdAsianOpenEngine` | **SHADOW** | (always-on) | true (`engine_init.hpp:122`) | `tick_fx.hpp:392-408` |
| `g_xauusd_fvg` | `XauusdFvgEngine` | **SHADOW** | (always-on) | true (`engine_init.hpp:140`, pinned in class) | `tick_gold.hpp:2217` |

This cohort is the cleanest in the codebase. Promotion to LIVE requires the documented 2-week shadow validation gate per each handoff doc.

---

## 3. Gold portfolio + supporting engines

| Instance | Class | Status | Evidence |
|---|---|---|---|
| `g_gold_stack` | `GoldEngineStack` | **LIVE-or-SHADOW** | Dispatched at `tick_gold.hpp:351`. Sub-engines toggled per regime; 5 sub-engines audit-disabled (see §5). Top-level shadow_mode follows `kShadowDefault` |
| `g_h1_swing_gold` | `H1SwingEngine` | **SHADOW** | enabled=true (`engine_init.hpp:549`), shadow=true (`:548`). Wired at `tick_gold.hpp:1916` |
| `g_h4_regime_gold` | `H4RegimeEngine` | **SHADOW** | enabled=true (`:553`), shadow=true (`:552`). Wired at `tick_gold.hpp:1918` |
| `g_minimal_h4_gold` | `MinimalH4Breakout` | **SHADOW** | enabled=true (`:571`), shadow=true (`:570`). Wired at `tick_gold.hpp:1923` |
| `g_c1_retuned` | `C1RetunedPortfolio` | **SHADOW** | enabled=true (`:585`), shadow=true (`:584`). Wired at `tick_gold.hpp:1927` |
| `g_tsmom` | `TsmomPortfolio` | **SHADOW** (via `kShadowDefault`) | enabled=true (`:619`), shadow=`kShadowDefault` (`:618`). Wired at `tick_gold.hpp:1930` |
| `g_tsmom_v2` | `cell::TsmomPortfolioV2` | **SHADOW** (always) | enabled=true (`:661`), shadow=true ALWAYS regardless of mode (`:660`, refactor-validation engine). Wired at `tick_gold.hpp:1933` |
| `g_donchian` | `DonchianPortfolio` | **SHADOW** (via `kShadowDefault`) | enabled=true (`:689`), shadow=`kShadowDefault` (`:688`). Wired at `tick_gold.hpp:1937` |
| `g_ema_pullback` | `EpbPortfolio` | **SHADOW** (via `kShadowDefault`) | enabled=true (`:709`), shadow=`kShadowDefault` (`:708`). Wired at `tick_gold.hpp:1940` |
| `g_trend_rider` | `TrendRiderPortfolio` | **SHADOW** | enabled=true (`:755`), shadow=true (`:754`). Wired at `tick_gold.hpp:1943` |
| `g_trend_pb_gold` | `TrendPullbackEngine` | **DISABLED** | `enabled=false` (`engine_init.hpp:541`). Wired but new-entry path off |
| `g_nbm_gold_london` | `NoiseBandMomentumEngine` | **LIVE** (intended) | enabled=true (`engine_init.hpp:1557`). Wired at `tick_gold.hpp:2226`. shadow_mode follows `kShadowDefault` |
| `g_pdhl_rev` | `PDHLReversionEngine` | **SHADOW** | enabled=true (`engine_init.hpp:1884`), shadow=true (`:1883`). Wired at `tick_gold.hpp:2254` |
| `g_macro_crash` | `MacroCrashEngine` | **SHADOW** | enabled=true (`:277`, S57 re-enabled with tightened thresholds), shadow=true (`:224`). Wired at `tick_gold.hpp:1595` |
| `g_candle_flow` | `CandleFlowEngine` | **MANAGE-ONLY** | `g_disable_candle_flow=true` (`globals.hpp:257`) blocks new entries. Wired at `tick_gold.hpp:2329` for management |
| `g_ema_cross` | `EMACrossEngine` | **SHADOW** | shadow_mode=`kShadowDefault` (`:25`). No explicit enabled flag found. Wired at `tick_gold.hpp:2395` |
| `g_rsi_reversal` | `RSIReversalEngine` | **DISABLED** | `enabled=false` (`engine_init.hpp:324`, "DISABLED 2026-05-01 — backtest negative EV") |
| `g_rsi_extreme` | `RSIExtremeTurnEngine` | **DISABLED** | `enabled=false` (`:31`, "DISABLED 2026-05-01 — 0/153 combos profitable") |
| `g_bracket_gold` | `GoldBracketEngine` | **MANAGE-ONLY** | `g_disable_bracket_gold=true` (`globals.hpp:258`) blocks new entries. Wired at `tick_gold.hpp:1346` for management |

**Notable:** the gold tick path (`tick_gold.hpp`) is the densest dispatcher in the codebase — 14 engine on_tick calls plus the gold-stack sub-engines.

---

## 4. Index engines (`omega::idx::` + cross-asset on indices)

| Instance | Class | Status | Evidence |
|---|---|---|---|
| `g_iflow_sp` | `IndexFlowEngine` | **LIVE** (intended) | Wired at `tick_indices.hpp:272`. No explicit enabled=false found |
| `g_iflow_nq` | `IndexFlowEngine` | **LIVE** (intended) | Wired at `tick_indices.hpp:529` |
| `g_iflow_nas` | `IndexFlowEngine` | **LIVE** (intended) | Wired at `tick_indices.hpp:854` |
| `g_iflow_us30` | `IndexFlowEngine` | **LIVE** (intended) | Wired at `tick_indices.hpp:632` |
| `g_imacro_sp` | `IndexMacroCrashEngine` | **SHADOW** (pinned) | Wired at `tick_indices.hpp:297`. shadow_mode hardcoded in class |
| `g_imacro_nq` | `IndexMacroCrashEngine` | **SHADOW** (pinned) | Wired at `tick_indices.hpp:553` |
| `g_imacro_nas` | `IndexMacroCrashEngine` | **SHADOW** (pinned) | Wired at `tick_indices.hpp:878` |
| `g_imacro_us30` | `IndexMacroCrashEngine` | **SHADOW** (pinned) | Wired at `tick_indices.hpp:656` |
| `g_minimal_h4_us30` | `MinimalH4US30Breakout` | **SHADOW** | enabled=true (`:777`), shadow=true (`:776`). Wired at `tick_indices.hpp:671` |
| `g_orb_us` | `OpeningRangeEngine` | **DISABLED** | `enabled=false` (`engine_init.hpp:1561`). Still wired |
| `g_orb_ger30` | `OpeningRangeEngine` | **DISABLED** | `enabled=false` (`:1562`) |
| `g_orb_uk100` | `OpeningRangeEngine` | **DISABLED** | `enabled=false` (`:1563`) |
| `g_orb_estx50` | `OpeningRangeEngine` | **DISABLED** | `enabled=false` (`:1564`) |
| `g_vwap_rev_sp` | `VWAPReversionEngine` | **DISABLED** | `enabled=false` (`:447`) |
| `g_vwap_rev_nq` | `VWAPReversionEngine` | **DISABLED** | `enabled=false` (`:450`) |
| `g_vwap_rev_ger40` | `VWAPReversionEngine` | **DISABLED** | `enabled=false` (`:453`) |
| `g_vwap_rev_eurusd` | `VWAPReversionEngine` | **DISABLED** | `enabled=false` (`:457`) |
| `g_nbm_sp` | `NoiseBandMomentumEngine` | **DISABLED** | `enabled=false` (`:1551`). Wired at `tick_indices.hpp:211` |
| `g_nbm_nq` | `NoiseBandMomentumEngine` | **DISABLED** | `enabled=false` (`:1552`) |
| `g_nbm_nas` | `NoiseBandMomentumEngine` | **DISABLED** | `enabled=false` (`:1553`) |
| `g_nbm_us30` | `NoiseBandMomentumEngine` | **DISABLED** | `enabled=false` (`:1554`) |
| `g_trend_pb_sp` | `TrendPullbackEngine` | **LIVE** (intended) | `enabled=true` (`:822`, RE-ENABLED S14). Wired at `tick_indices.hpp:234` |
| `g_trend_pb_nq` | `TrendPullbackEngine` | **LIVE** (intended) | `enabled=true` (`:819`, RE-ENABLED S14). Wired at `tick_indices.hpp:472` |
| `g_trend_pb_ger40` | `TrendPullbackEngine` | **DISABLED** | `enabled=false` (`:824`, "not live-validated") |

**Cross-asset:**

| Instance | Class | Status | Evidence |
|---|---|---|---|
| `g_ca_esnq` | `EsNqDivergenceEngine` | **DISABLED** | `enabled=false` (`engine_init.hpp:1573`). Wired at `tick_indices.hpp:101` |
| `g_ca_eia_fade` | `OilEventFadeEngine` | **DISABLED** | `enabled=false` (`:1568`). Wired at `tick_oil.hpp:37` |
| `g_ca_brent_wti` | `BrentWtiSpreadEngine` | **DISABLED** | `enabled=false` (`:1569`). Wired at `tick_oil.hpp:48` |
| `g_ca_fx_cascade` | `FxCascadeEngine` | **DISABLED + DEAD-ENTRY** | `enabled=false` (`:1570`). Position-active checked in `on_tick.hpp:829` but no `.on_tick()` call found |
| `g_ca_carry_unwind` | `CarryUnwindEngine` | **DISABLED + DEAD-ENTRY** | `enabled=false` (`:1571`). Same pattern as fx_cascade |
| `g_nbm_oil_london` | `NoiseBandMomentumEngine` | **DISABLED** | `enabled=false` (`:1558`). Wired at `tick_oil.hpp:57` |

---

## 5. GoldEngineStack sub-engines (audit-disabled)

These are sub-engines pushed into `g_gold_stack` and gated at the dispatch loop. They never enter, but the parent stack still routes other regimes through them.

| Sub-engine | Status | Bleed (4-week, 2026-04-30 audit) |
|---|---|---|
| `session_momentum` | DISABLED | 53.3% WR but negative-tail dominated |
| `intraday_seasonality` | DISABLED | Sharpe 1.08 sim, live edge collapsed |
| `vwap_snapback` | DISABLED | Sample never grew |
| `vwap_stretch_reversion` | DISABLED | Net negative even after Apr-9 fix |
| `dxy_divergence` | DISABLED | Insufficient live trades |

Total ~$3K bleed across these five over the audit window (per `globals.hpp:237`).

---

## 6. DEAD globals (the zombie list — zero dispatch references)

These are declared in `globals.hpp` but searching the entire `include/` tree found **no `.on_tick()` call in any dispatch path**. Some have `.pos.active` queries used purely for blocking other engines — that's a passive read, not a dispatch.

### 6a. FX Tier-0 BreakoutEngines (5)

| Instance | Class | Symbol | Why dead |
|---|---|---|---|
| `g_eng_eurusd` | `BreakoutEngine` | EURUSD | "FX engines globally disabled 2026-04-06" per EUR handoff doc. `tick_fx.hpp` has zero references. Superseded by `g_eurusd_london_open` |
| `g_eng_gbpusd` | `BreakoutEngine` | GBPUSD | Same. Superseded by `g_gbpusd_london_open` |
| `g_eng_audusd` | `BreakoutEngine` | AUDUSD | Same. Superseded by `g_audusd_sydney_open` |
| `g_eng_nzdusd` | `BreakoutEngine` | NZDUSD | Same. Superseded by `g_nzdusd_asian_open` |
| `g_eng_usdjpy` | `BreakoutEngine` | USDJPY | Same. Superseded by `g_usdjpy_asian_open` |

`engine_init.hpp:166-170` still sets `shadow_mode=true` on these but the engines never receive ticks. **Five clean removals.**

### 6b. Index Tier-0 BreakoutEngines (5)

| Instance | Class | Symbol | Why dead |
|---|---|---|---|
| `g_eng_sp` | `SpEngine` | US500.F | `tick_indices.hpp:91-94` shows the dispatch lines commented out: `// dispatch(g_eng_sp, g_sup_sp, ...)`. Comment at `:17` says "g_eng_sp/nq/us30/nas100 are disabled (never build compression)" |
| `g_eng_nq` | `NqEngine` | USTEC.F | Same pattern |
| `g_eng_us30` | `Us30Engine` | DJ30.F | Same pattern |
| `g_eng_nas100` | `Nas100Engine` | NAS100 | Same pattern |
| `g_eng_ger30` | `EuIndexEngine` | GER40 | Suspect dead by analogy. Need a final `tick_indices.hpp` confirmation pass |
| `g_eng_uk100` | `EuIndexEngine` | UK100 | Same suspicion |
| `g_eng_estx50` | `EuIndexEngine` | ESTX50 | Same suspicion |
| `g_eng_cl` | `OilEngine` | USOIL.F | No dispatch found. Comment at `globals.hpp:597`: "Oil uses g_eng_cl (breakout) + g_ca_eia_fade + g_ca_brent_wti only" — but the actual `g_eng_cl.on_tick` call wasn't found in the grep. Worth a final `tick_oil.hpp` pass |
| `g_eng_brent` | `BrentEngine` | BRENT | Same |

**Action:** confirm the EU index, oil, and brent engines on a final pass. The four US index Tier-0 engines are confirmed dead by the explicit comment in `tick_indices.hpp:17` plus the commented-out dispatch lines.

### 6c. Bracket engines other than gold (12)

| Instance | Symbol family | Why dead |
|---|---|---|
| `g_bracket_sp` / `_nq` / `_us30` / `_nas100` | US indices | No `.on_tick()` call anywhere. Position-active checks at `tick_indices.hpp:70` are used only for blocking new index entries |
| `g_bracket_ger30` / `_uk100` / `_estx50` | EU indices | Same |
| `g_bracket_brent` | Brent | Same |
| `g_bracket_eurusd` / `_gbpusd` / `_audusd` / `_nzdusd` / `_usdjpy` | FX | Confirmed dead per EUR engine handoff doc Section 1: "configured every startup but never invoked from any dispatch path" |

**12 clean removals.** `g_bracket_gold` is the only bracket engine still actively dispatched (manage-only via `g_disable_bracket_gold`).

### 6d. VWAPAtrTrail (4)

| Instance | Why dead |
|---|---|
| `g_vwap_atr_trail_sp` | Was an upgrade for `g_vwap_rev_sp`, which is itself disabled. No dispatch found |
| `g_vwap_atr_trail_nq` | Same dependency on disabled VWAPRev |
| `g_vwap_atr_trail_nas` | Comment in `globals.hpp:325`: "no VWAPRev, unused" |
| `g_vwap_atr_trail_us30` | Comment in `globals.hpp:326`: "no VWAPRev, unused" |

**4 clean removals.**

### 6e. Cross-asset entry-path-dead (2)

`g_ca_fx_cascade` and `g_ca_carry_unwind` — `enabled=false` AND no `.on_tick()` call. They have position-active queries in `on_tick.hpp:829-832` but nothing actually fires entries. Functionally dead even if the disable flag were flipped.

---

## 7. Engines visible in `g_engine_heartbeat` but with no instance found

Heartbeat registry references `g_engine_heartbeat.register_engine("Tsmom_H1_long" ... "Tsmom_D1_long" ...)` (`engine_init.hpp:2107-2111`) — these are sub-cells of `g_tsmom`, not standalone instances. Same for "ISwingSP" / "ISwingNQ" (likely sub-cells of an index swing portfolio). Worth confirming in a deep pass that these aren't a third source of stale references; for now they're sub-cells of registered portfolios, not orphan globals.

---

## 8. SHADOW vs DISABLED counts by family

| Family | LIVE-intended | SHADOW | DISABLED | DEAD |
|---|---|---|---|---|
| Compression cohort | 0 | 7 | 0 | 0 |
| Gold portfolio + supporting | 1 (`nbm_gold_london`) | 11 (h1_swing, h4_regime, minimal_h4, c1, tsmom, tsmom_v2, donchian, ema_pullback, trend_rider, gold_stack via `kShadowDefault`, pdhl_rev) + 2 manage-only | 4 (rsi_reversal, rsi_extreme, trend_pb_gold, candle_flow as `g_disable_*`) + 5 sub-engines | 0 |
| Index portfolio | 4 iflow + 2 trend_pb_sp/nq | 4 imacro + minimal_h4_us30 | 4 vwap_rev + 4 nbm + 4 orb + trend_pb_ger40 + 5 ca | 0 |
| Tier-0 / Bracket / VWAPTrail | 0 | 0 | 0 | 25 |

The DEAD column is concentrated entirely in the legacy Tier-0 / bracket / VWAPAtrTrail trio. Those three families are the obvious cleanup targets.

---

## 9. Open questions (need VPS data or deeper read)

1. **`kShadowDefault` resolved value at runtime.** If `g_cfg.mode == "LIVE"`, then `kShadowDefault=false` and several engines (tsmom, donchian, ema_pullback, gold_stack top-level, ema_cross, pdhl_rev) are LIVE rather than SHADOW. Need to confirm `g_cfg.mode` value on the VPS — it's a config-driven flag, not a code constant.

2. **Tier-0 EU index dispatch.** Confirmed dead for US (sp/nq/us30/nas100) via explicit comment in `tick_indices.hpp:17`. Strongly suspect ger30/uk100/estx50 follow the same pattern but want a direct confirmation read of the EU index dispatch block before listing them as removals.

3. **Tier-0 oil + brent dispatch.** Comment in `globals.hpp:597` says "Oil uses g_eng_cl" — but the grep didn't find `g_eng_cl.on_tick` anywhere. Needs final confirmation in `tick_oil.hpp`.

4. **`g_iswing_sp` / `g_iswing_nq`.** Referenced in `tick_indices.hpp:311, 892` but not declared in the `globals.hpp` snapshot I read. Probably declared further down (line 967-968 in `engine_init.hpp` shows `.shadow_mode=true` settings). Worth confirming source of declaration.

5. **Per-engine PnL distribution over last 30 days.** The whole point of Phase 2. Cannot proceed locally — needs the VPS shadow CSV.

---

## 10. Phase 4 (removal) — what was actually safe to remove, and what wasn't

**Important correction to §10's earlier draft:** my Phase 1 framing was too optimistic about what's removable. "No `.on_tick()` dispatch" doesn't mean "no references." A targeted grep across `include/` after the user said "do 10 now" surfaced extensive supporting plumbing for most of the candidates I'd flagged.

### 10a. Actually removed in this session (4 instances)

Only one cluster of candidates had **zero non-declaration references** anywhere in the codebase:

- `g_vwap_atr_trail_sp` / `_nq` / `_nas` / `_us30` — declarations at `globals.hpp:323-326`. Grep returned the declarations themselves and doc references only. No engine_init.hpp config, no tick_*.hpp dispatch, no quote_loop.hpp / trade_lifecycle.hpp / on_tick.hpp plumbing.

These four were replaced with a removal-marker comment block at the same location. See git diff for `include/globals.hpp` in the commit that follows this doc.

### 10b–c. Deferred — not safe to remove blind

The candidates I'd grouped as "highest confidence" turned out to have substantial reference graphs across 6–8 files. Removing them properly is a multi-file refactor that needs a parse-check before commit. The bash sandbox in the agent environment is broken (S61 handoff doc §"Sandbox issue"), so I can't run `g++ -fsyntax-only` locally — and shipping a 100+-edit refactor without compile validation is exactly the kind of change that breaks production on next deploy.

Affected globals and the supporting plumbing they have to coexist with, by family:

**FX Tier-0 BreakoutEngines (5)** — `g_eng_eurusd/gbpusd/audusd/nzdusd/usdjpy`. Their `.on_tick()` is never called, but the engines are referenced in:
- `engine_init.hpp` — `apply_engine_config()`, `.ENTRY_SIZE`, `.ACCOUNT_EQUITY`, `.MIN_GAP_SEC`, `.MAX_TRADES_PER_MIN`, parameter print blocks
- `engine_init.hpp:166-170` — `.shadow_mode = true` settings
- (See `docs/SESSION_2026-05-02_EURUSD_LONDON_OPEN_HANDOFF.md` for full inventory)

**US index Tier-0 (4)** — `g_eng_sp/nq/us30/nas100`. Most-plumbed dead globals in the codebase:
- `trade_lifecycle.hpp` — `.ACCOUNT_EQUITY` sync, `be_pnl()` computations, `.pos.active`/`.pos.is_long` coverage counts, `.vwap()` reads, `cov_add()`
- `quote_loop.hpp` — `.phase`, `.recent_vol_pct`, `.base_vol_pct` diag, `stale_beng()`, `fc_snap()`, `sfc()` template calls
- `on_tick.hpp` — `.seed()` calls, `.pos.active` coverage gates, `.phase` macro-context updates, `push_live_trade()` UI feeds, `chk()` checks
- `tick_indices.hpp` — `sup_decision(g_sup_sp, g_eng_sp, ...)` per-symbol supervisors
- `engine_init.hpp` — `apply_engine_config()`, `apply_be()`, `.ENTRY_SIZE`, `.ACCOUNT_EQUITY`, parameter print blocks

**Non-gold bracket engines (12)** — `g_bracket_sp/nq/us30/nas100/ger30/uk100/estx50/brent` plus the 5 FX brackets. Plumbed in:
- `order_exec.hpp` — `.on_reject()`, `fill_bracket()` dispatch (FX brackets only — index brackets need confirmation)
- `quote_loop.hpp` — `stale_bracket()`, `fc_bracket()`, `sbk()`, `fc_bracket_snap()` template calls
- `trade_lifecycle.hpp` — `be_pnl()`, `.pos.active` coverage
- `engine_init.hpp` — `.ATR_PERIOD`, `.ATR_RANGE_K`, `.symbol`, `.ENTRY_SIZE`, `.MAX_RANGE`, `.configure()`, `apply_bracket()`, `wire_bracket()`
- `config.hpp` — `mid_bracket()` calls
- `on_tick.hpp` — `.pos.active`, `.pos.entry`, `.pos.tp`, `.pos.sl`, `.pos.size`, `.pos.entry_ts`, `.pos.is_long`, `push_live_trade()`
- `VERIFY_ALL_ENGINES.ps1` — diagnostic regex patterns

**EU index + commodity Tier-0 + cross-asset disabled engines** — same pattern, smaller magnitude.

### 10d. Recommended path forward

The full removal of the 25-engine "legacy Tier-0 / bracket / VWAPTrail" cluster is a real session of work, not a pass at the end of an audit session. Suggested approach:

1. **Per-family removal sessions** — one session each for FX Tier-0, US index Tier-0, FX brackets, index brackets. Each session = source edit + parse-check via `scripts/s61_parsecheck.cpp`-style harness + a separate commit.
2. **Build validation gate** — Mac-side `g++ -std=c++17 -I include -DOMEGA_BACKTEST -fsyntax-only` on a wrapper that includes `globals.hpp` and the dispatch headers. Commit only when parse-clean.
3. **Production smoke** — after each removal commits, the next VPS deploy should validate via `OMEGA.ps1 deploy` that the engine count line in startup logs decrements correctly.

### 10e. Why this matters in framing

The Phase 1 audit's "DEAD" classification was correct *for the dispatch question*: do these engines fire? No. But it was wrong as a removal-readiness signal. A more useful classification next time would distinguish:

- **Dispatch-dead, plumbing-dead** — can be removed in a single Edit (only VWAPAtrTrail qualified this session).
- **Dispatch-dead, plumbing-live** — needs a multi-file refactor session before removal.
- **Dispatch-dead, plumbing-load-bearing** — the position-active queries are part of coverage gates that affect other engines' behavior; removal needs behavioral analysis, not just compile-check.

The 24 remaining "DEAD" globals are mostly category 2, with some in category 3 (e.g. the `.pos.active` checks in `tick_indices.hpp:69` are part of the index-coverage block-other-engines logic).

---

## 11. What this audit pass does NOT yet cover (Phases 2-4)

- Phase 2 — pull shadow CSV from VPS, count fires per engine, sum PnL, flag <5 fires/month or net-negative engines.
- Phase 3 — improvement queue ranked by expected $/10d impact.
- Phase 4 — actual removal of dead globals (gated on user approval).

The next session can pick up at Phase 2 the moment VPS log access is in place.

---

## 12. One immediate finding worth surfacing

The "old" Tier-0 + bracket cohort accounts for 25 of the 27 confirmed/suspected dead globals — roughly 37% of declared engines in `globals.hpp`. That cluster represents architectural debt from before the compression-cohort (`*LondonOpen`/`*AsianOpen`/`MidScalper`) and `IndexFlow`/`IndexMacroCrash` redesign. The current production system uses the newer engines exclusively; the old Tier-0 / bracket layer is purely vestigial.

A targeted cleanup pass — touching only `globals.hpp` declarations and any orphan `engine_init.hpp` references for these 25 engines — would be a low-risk win and would shrink the audit surface for next session by more than a third.

End of inventory verification.
