# NEXT_SESSION — S9 Priority Brief

Date checkpoint: 2026-05-06
Previous: S8 (this session)

## What S8 fixed (DEPLOYED locally, awaiting VPS deploy)

Four files changed. Functional changes only — no dead-code removal.

### 1. `include/on_tick.hpp` — gold L2 imbalance bug fix + cBot override removal

The block at lines 302-393 was rewritten end-to-end.

Before: `g_macro_ctx.gold_l2_imbalance` read from `g_l2_gold.micro_edge` (a `GoldMicrostructureAnalyzer` output computed only inside `CTraderDepthClient`). With cTrader runtime-disabled since 2026-04-29, that field was stuck at the default 0.5 forever. Every gold engine reading `gold_l2_imbalance` was getting a constant 0.5 — direction-blind for ~7 days of live trading and any post-2026-04-29 backtest sweep.

After: reads `g_l2_gold.imbalance` (FIX-fed via `fix_dispatch.hpp`). Comments rewritten to "FIX MarketData (264=0 full book)". Real DOM cBot override block (`OmegaDomStreamer` port 8765) removed. `g_macro_ctx.ctrader_l2_live` is now sourced from FIX freshness on gold/sp/eur instead of `g_ctrader_depth.depth_events_total > 0`.

The `[GOLD-L2-LIVE]` log line will now show the real FIX vol-weighted imbalance value rather than 0.5. Watch for this on first VPS restart — confirms the fix.

### 2. `include/fix_dispatch.hpp` — ct_fresh gate dropped + FIX-side microprice added

The atomic L2 write at lines 148-201 is now unconditional. Pre-S8 code only wrote when cTrader had been silent >2s; since cTrader is permanently off, the gate was dead code that added a branch and a comment block referencing cTrader as authoritative.

FIX-side microprice computation added inline:
- `microprice = (bid_size·ask_price + ask_size·bid_price) / (bid_size + ask_size)`
- `microprice_bias = microprice − mid`
- Gracefully degrades to 0 when broker omits tag 271 (sizes=0).

This restores the ±1 confirmation/contradiction signal in `OmegaEdges::entry_score_l2` (lines 1254-1269) for ALL symbols that previously depended on `microprice_bias`, including gold and indices. Without this, every entry score across the system was missing one signal axis since 2026-04-29.

The on_tick suppression block at the old lines 228-256 (`if (!ctrader_depth_is_live(sym))` gate) was deleted. FIX always posts to `engine_dispatch_post_tick` now.

### 3. `include/engine_init.hpp` — FX engines pinned shadow

The 5 BracketEngine FX instances (`g_eng_eurusd`, `g_eng_gbpusd`, `g_eng_audusd`, `g_eng_nzdusd`, `g_eng_usdjpy`) at lines 143-157 were `kShadowDefault` (= LIVE when `g_cfg.mode=="LIVE"`). Now pinned `shadow_mode=true` regardless. Trigger trade: 2026-05-06 09:19:24 GBPUSD LONG GbpusdLondonOpenSL net -$45.50.

The `_london_open / _asian_open / _sydney_open` cohort was already pinned shadow on local Mac per S7 deferred culls — but those changes hadn't been deployed to VPS, which is why GBPUSD trades were still firing live. This commit completes the deferred-cull batch from S7: deploying it to VPS will switch off ALL FX live-trading in one go.

Promote back to `kShadowDefault` only after a 2-week paper validation showing ≥30 trades with WR ≥35% net positive after costs.

## What S8 deliberately did NOT do (deferred to S9)

The cTrader Open API surface is still in the codebase as dead code. ~2500 lines across 11 files:

- `omega_main.hpp:407-412` — RealDomReceiver start
- `omega_main.hpp:420-644` — entire cTrader init block (credentials, lambdas, name aliases, bar subs, audit thread, start/stop)
- `omega_main.hpp:708-732` — startup verification Check 2 (cTrader credentials)
- `omega_main.hpp:759-792` — startup verification Check 5+6 (cTrader connection)
- `omega_main.hpp:794-817` — Check 7 reads cTrader `raw_bid/raw_ask` (FIX has these too — just rewire)
- `omega_main.hpp:842-987` — L2 watchdog thread monitors `g_ctrader_depth.depth_events_total`
- `omega_main.hpp:1000-1029` — FIX-launch wait gate
- `omega_main.hpp:1037` — `g_ctrader_depth.stop()` at shutdown
- `omega_runtime.hpp:5-17` — `set_ctrader_tick_ms` and `ctrader_depth_is_live`
- `globals.hpp:725-809` — `AtomicL2.micro_edge / raw_bid / raw_ask` (cTrader-only fields), `g_ct_ms_*` per-symbol staleness trackers, `get_ctrader_tick_ms_ptr` helper
- `omega_types.hpp:297` — `static CTraderDepthClient g_ctrader_depth;`
- `omega_types.hpp:284-287` — config fields `ctrader_access_token / refresh_token / ctid_account_id / ctrader_depth_enabled`
- `omega_config.ini` — `[ctrader_api]` section
- `quote_loop.hpp:286-394` — diagnostic loop reads of `g_ctrader_depth.depth_events_total`, `depth_active`
- `on_tick.hpp:440-450` — `[L2-STATUS]` print line reads `ctrader_l2_live` AND `g_ctrader_depth.depth_events_total`
- `on_tick.hpp:580-601` — `g_telemetry.UpdateL2(...)` lambda reads `depth_active` and `last_depth_event_ms` for the GUI L2-active flag
- `on_tick.hpp:466-478` — comment about cTrader incremental updates (cosmetic)
- `tick_gold.hpp:336, 970, 1034` — comments + CSV writer column referencing `g_ctrader_depth.depth_events_total`
- `OmegaFIX.hpp:130` — comment reference (cosmetic)
- `OHLCBarEngine.hpp:5` — header comment (cosmetic)
- `globals.hpp:558, 561, 733` — comments (cosmetic)
- `RealDomReceiver.hpp` — DELETE (file)
- `CTraderDepthClient.hpp` — DELETE (file)
- `ctrader_cbot/OmegaDomStreamer.cs` — DELETE (file)
- `src/main.cpp:76` — `#include "CTraderDepthClient.hpp"` line

This is functionally inert because `OmegaConfig::ctrader_depth_enabled = false` by default. Deleting it changes zero runtime behavior. The reason to do it: enforce "zero cTrader API for L2" at the code level so a future config flip can't silently re-enable cTrader.

## S9 priority order

1. **Verify the S8 fix on VPS.** First restart after deploy: confirm `[GOLD-L2-LIVE] imb=` log shows non-0.5 values (real FIX-fed imbalance). Confirm zero new GBPUSD/EURUSD/etc. live trades fire.
2. **Re-evaluate disabled engines under working L2.** With `gold_l2_imbalance` no longer stuck at 0.5, gold engines that L2-gate on imbalance may behave differently. Particular interest: HybridBracketGold (still tagged DEPRECATED in `GoldHybridBracketEngine.hpp` from S7 — was that based on broken-L2 backtests? worth a re-test of the 432-combo sweep with the fixed read line).
3. **Cull cTrader Open API surface** — see the file:line list above. Order: gut omega_main.hpp first (largest blast radius, isolated changes), then helper headers (omega_runtime.hpp, globals.hpp ct_ms trackers, omega_types.hpp), then file deletions (CTraderDepthClient.hpp, RealDomReceiver.hpp, ctrader_cbot/), then comment cleanup, then build verify, then commit.
4. **Cull `OmegaDomStreamer` cBot** — bundled with item 3. Surface is small (RealDomReceiver.hpp + the C# bot file + 1 startup line + 1 override block in on_tick.hpp).
5. **Clean tick CSV writer dead columns** — `tick_gold.hpp:993-995` header and `tick_gold.hpp:1026-1041` rows reference `depth_events_total / raw_bid / raw_ask / micro_edge`. After cull, those will all log 0 — drop from CSV. Hydrator only needs `ts_ms,mid,bid,ask` so this is safe. CRITICAL: do not change column ORDER or rename `ts_ms,mid,bid,ask`.
6. **microprice_bias rename in MacroContext consumers (cosmetic)** — `g_macro_ctx.ctrader_l2_live` → `g_macro_ctx.l2_live`. ~15 files, no behavior change.
7. **Re-test gold engines.** Run the 432-combo sweep harness again, this time with the L2 reads fixed. Compare verdicts to S7. Re-evaluate whether the bracket-breakout family is dead or whether prior tests were on broken-L2 data.

## Restart procedure

After deploying the S8 commit to VPS:
1. Stop Omega NSSM service (`nssm stop Omega`)
2. Pull latest on VPS: `cd C:\Omega && git pull origin main`
3. Build: `cd C:\Omega\build\Release && cmake --build . --config Release`
4. Start: `nssm start Omega`
5. Tail latest log: `Get-Content C:\Omega\logs\latest.log -Tail 200 -Wait`
6. Watch for `[GOLD-L2-LIVE] imb=` lines — should show varying values, not 0.500
7. Watch for any `GbpusdLondonOpen` / `EurusdLondonOpen` / `g_eng_eurusd` etc. live trade entries — should be zero (shadow only)

## Files not in this commit (still on local Mac, deferred)

Per S7 deferred state, these were modified locally but not pushed:
- `backtest/fx_london_open_bt.cpp` — full rewrite, BT harness reference
- `include/GoldHybridBracketEngine.hpp` — DEPRECATED tombstone
- `include/OmegaCostGuard.hpp` — verify line 60 = 0.00060 (revert if different)

The `engine_init.hpp` shadow-pinning from S7 IS in this commit (S8 made an additional FX change in the same file).
