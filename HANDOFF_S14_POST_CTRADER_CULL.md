# HANDOFF — S14 (post-S13 cTrader cull)

Date prepared: **2026-05-08**
Prepared during: S13 cull session (commit `4827ad4` on `origin/main`)
Target: a fresh Claude session AFTER the user has run `.\OMEGA.ps1 deploy`
User intent: verify deploy, watch first 30 minutes of live shadow tape, pick up the deferred work in priority order.

---

## State of the world

### Commit on main
- `2734f0e` — S12 (TsmomEngine MAE_EXIT cooldown + TrendPullback consec-loss tracking + relaxed hard TIME_STOP). **DEPLOYED.**
- `4827ad4` — S13 cTrader Open API cull + bundled S13 CSV writer fix. **PUSHED, not yet deployed when this handoff was written.** First action of the new session: confirm the user has deployed it. If they have, run the post-deploy verification below; if they have not, prompt them to.

### Repo physical changes in S13 (commit `4827ad4`)

Files deleted (~1500 lines):
- `include/CTraderDepthClient.hpp`
- `include/RealDomReceiver.hpp`
- `ctrader_cbot/OmegaDomStreamer.cs`
- `ctrader_cbot/TickScalper.cs`
- `DEPLOY_S12_FINDING_A.sh`

Files edited (~1000 lines stripped):
- `src/main.cpp` — two cTrader includes removed.
- `include/omega_main.hpp` — cTrader init block (~225 lines), Check 2, Checks 5+6, L2 watchdog thread, FIX-launch wait, shutdown call all removed; periodic 10-min bar-save extracted into its own thread; Check 7 rewired to use `g_l2_gold.last_update_ms`.
- `include/omega_runtime.hpp` — `set_ctrader_tick_ms` / `ctrader_depth_is_live` deleted.
- `include/omega_types.hpp` — `ctrader_*` config fields and `g_ctrader_depth` static deleted.
- `include/globals.hpp` — `g_l2_watchdog_dead`, `AtomicL2.{raw_bid, raw_ask, micro_edge}`, `g_ct_ms_*` atomics, `get_ctrader_tick_ms_ptr` helper deleted. `g_feed_stale_xauusd` kept as a now-dormant flag (read-only path; permanently false).
- `include/quote_loop.hpp` — depth-events watchdog block replaced with FIX-staleness check on `g_l2_gold.last_update_ms`; CTRADER_RECONNECT block removed.
- `include/on_tick.hpp` — `[L2-STATUS]` line and `UpdateL2` `l2_live` lambda rewritten to source from `g_l2_*`. Cosmetic comment cleanup.
- `include/tick_gold.hpp` — S13 CSV writer fix already in tree; pdhl `on_tick(...)` raw_bid/raw_ask args replaced with literal `0, 0`; `g_feed_stale_xauusd` comment updated.
- `include/OmegaFIX.hpp`, `include/OHLCBarEngine.hpp`, `include/engine_dispatch.hpp` — header-docstring cleanup; verified zero real code dependencies on `CTraderDepthClient`.
- `include/engine_config.hpp` — `[ctrader_api]` section parser deleted.
- `omega_config.ini` — `[ctrader_api]` section deleted.
- `OMEGA.ps1` — `ctrader_bar_failed.txt` cleanup deleted.
- `VERIFY_STARTUP.ps1` — Check 8 rewritten around the new `[L2-STATUS] l2_live=...` line; Check 8b2 (CTRADER-L2-CHECK) removed; staleness-detail strings updated to refer to FIX 264=0.
- `omega-terminal/src/api/types.ts` — `l2_live` docstring updated.

Macro-context field name `g_macro_ctx.ctrader_l2_live` was NOT renamed (Phase C cosmetic rename — see deferred work below).

### Open verifier failures expected to clear after deploy
- `L2 Tick CSV XAUUSD` STALE — clears within seconds because the S13 writer fires unconditionally on every XAUUSD tick.
- `L2 Tick CSV US500` STALE / `L2 Tick CSV USTEC` STALE — these are upstream FIX delivery issues, NOT cTrader-related. They MAY persist post-S13. Investigate separately if so.

---

## First task in the new session: verify the deploy

Walk the user through these checks in order. If any FAIL, stop and triage before moving on to deferred work.

### 1. Service health
- `Get-Service Omega` should show `Running`.
- VERIFY_STARTUP.ps1 should report PASS on Hash vs HEAD, Bar State, Bar State Load, Mode (SHADOW), FIX feed, L2 Feed (FIX 264=0), L2 Tick CSV XAUUSD.

### 2. Log signature changes (these PROVE the cull is live, not just compiled)
On a fresh `latest.log` after deploy, you should see:
- `[OMEGA] cTrader Open API surface removed at S13 -- FIX 264=0 owns L2.` (printed in place of the old cTrader init block)
- `[STARTUP] Starting FIX immediately (FIX 264=0 provides L2)` (no more 45s wait)
- `[L2-STATUS] l2_live=1 gold_real=1 gold_imb=… gold_mp=… gold_age_ms=… sp_real=… cl_real=…` (every 60s)
- NO `[CTRADER-AUDIT]` lines.
- NO `[L2-WATCHDOG]` lines.
- NO `[CTRADER-EVTS]`, `[CTRADER-BOOK]`, `[CTRADER-L2-CHECK]`, `[CTRADER-STATUS]` lines.

If any of those CTRADER-* tags reappear, something didn't compile in or the deployed binary is stale.

### 3. L2 tick CSV
- `C:\Omega\logs\l2_ticks_XAUUSD_<today>.csv` should be growing, with `l2_imb` (column 5) showing non-0.5 values during normal market sessions.
- Columns 6–10 (`l2_bid_vol, l2_ask_vol, depth_bid_levels, depth_ask_levels, depth_events_total`) and column 11 (`watchdog_dead`) and column 16 (`micro_edge`) are all permanently zero by design — the schema is preserved for backward-compatible appends to today's CSV.
- The 17-column header is unchanged.

### 4. Live shadow tape (first 30 min)
- New entries should still fire from Tsmom / Donchian / EmaPullback / TrendRider portfolios.
- Existing positions should be managed normally (trail / SL).
- Watch for `[L2-DEAD]` alerts in `quote_loop.hpp`'s diagnostic loop — they now fire when `g_l2_gold.last_update_ms` is older than 30s. The thresholds are unchanged from the cTrader era; only the data source flipped.
- Confirm `L2_ALERT.txt` at `C:\Omega\logs\` is no longer being touched. Old contents may persist; that's cosmetic.

---

## Deferred work, prioritised

### Tier 1 — code hygiene worth doing soon
1. **Phase C cosmetic rename**: `g_macro_ctx.ctrader_l2_live` → `g_macro_ctx.l2_live` across ~15 consumer files. Pure rename, no semantic change. Touched files (search-and-replace candidates): `on_tick.hpp`, `quote_loop.hpp`, `OmegaTelemetryServer.hpp`, `tick_*.hpp`, telemetry snap struct definition. Estimate: 15-min single-session task. Source: `NEXT_SESSION.md` line 10 in pre-S13 history.

2. **`PDHLReversionEngine.hpp` signature cleanup**: the engine still accepts `int raw_bid_count, int raw_ask_count` parameters that S13 now passes as literal `0, 0`. Drop the two parameters from the `on_tick` signature and adjust the engine internals. Estimate: 10-min surgical edit. Source: `HANDOFF_S13_CTRADER_CULL.md` step B.8 option (b).

3. **9 docs still carry the live PAT** in `git`-tracked history. The S13 push redacted only `HANDOFF_S13_CTRADER_CULL.md`. The rest are grandfathered through GitHub's secret scanner because they pre-date the scan rule change but they're a real exposure if anyone clones the repo. Files (from a `grep -l 'ghp_[A-Za-z0-9]\\+' .`):
   - `SESSION_HANDOFF_2026-04-29.md`
   - `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_WALKFORWARD_HANDOFF.md`
   - `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_WALKFORWARD.md`
   - `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_TRAIL_FIX.md`
   - `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_PHASE2_FINE.md`
   - `docs/SESSION_2026-05-02_USDJPY_ASIAN_OPEN_HANDOFF.md`
   - `docs/SESSION_2026-05-02_EURUSD_LONDON_OPEN_HANDOFF.md`
   - `docs/SESSION_2026-05-02_HANDOFF_NEXT.md`
   - `docs/SESSION_2026-04-30b_HANDOFF.md`
   Recommended fix: rotate the PAT first (GitHub → Settings → Developer settings → Personal access tokens), redact all 9 docs to `${GITHUB_PAT}` placeholder, single commit, push. The history-rewrite path (BFG / `git filter-repo`) is overkill for this — a forward-only redact + token rotation is the standard remediation.

### Tier 2 — investigate if it persists
4. **L2 Tick CSV US500/USTEC staleness**: the S13 cull does NOT fix this — those CSV writers in `on_tick.hpp` (~line 1820) already wrote zeros for cTrader-derived fields and are independent of the cTrader code path. If they're still STALE after S13 deploys, root-cause is upstream FIX delivery on the index symbols, not the loggers. Check: `[FIX-W]` lines in latest.log should show `MDEntryType=0/1` (bid/ask) arriving for `US500.F` and `USTEC.F`. If not, BlackBull MD subscription for those instruments is the issue, not Omega code.

### Tier 3 — defer indefinitely unless triggered
5. **CSV column reduction** for `l2_ticks_XAUUSD_*.csv`. Hydrator only needs `ts_ms, mid, bid, ask`. We could drop columns 5–17 entirely. Costs: requires purging today's CSV first (header mismatch on append) and updating the hydrator. Benefit: ~60% smaller CSVs. Source: `NEXT_SESSION.md` item 9.

6. **52MB `HISTDATA_COM_ASCII_NSXUSD_T202512.zip`** in repo root: GitHub warns about it on every push. Either move it to Git LFS (`git lfs track *.zip`) or delete it from the repo if it's already extracted into `phase1/` somewhere.

7. **`backtest/l2_edge_discovery.cpp`** still references the cTrader DOM data format. Backtest tool, kept by S13. Inspect at next backtest pass — may or may not need adjusting.

---

## Things that DID NOT change at S13 (so don't blame them)

- FIX session config (`[fix]` section in `omega_config.ini`) — broker, ports, sender/target comp IDs, password are all unchanged.
- All trading engine parameters in `omega_config.ini`.
- All risk gates: `max_positions`, `daily_loss_limit`, `dollar_stop_usd`, `max_lot_*`, `min_entry_gap_sec` etc. all carry over from S12.
- `g_macro_ctx.ctrader_l2_live` field name (Phase C cosmetic rename deferred).
- The 17-column XAUUSD L2 CSV schema.
- `latest.log` rotation behaviour.

---

## Crash / regression triage (quick reference)

If the deploy build FAILS:
- Most likely cause #1: forgotten `g_ctrader_depth.X` reference somewhere I missed.
  Fix: `grep -rn 'g_ctrader_depth\\|g_ct_ms_\\|g_l2_watchdog_dead\\|raw_bid\\|raw_ask\\|micro_edge' include/ src/` — anything outside a comment is the offender.
- Most likely cause #2: a reference to `AtomicL2::raw_bid` / `raw_ask` / `micro_edge` outside `tick_gold.hpp:2260` (which I patched).
- Most likely cause #3: a config consumer still expecting `g_cfg.ctrader_*` fields.
  Fix: `grep -rn 'g_cfg\\.ctrader_' include/ src/` — should return zero hits.

If the deploy succeeds but L2 Tick CSV XAUUSD stays STALE:
- The S13 writer fix in `tick_gold.hpp` lines ~1008–1042 is the unconditional path. Confirm it's hit by checking for `[L2-CSV-OPEN]` in latest.log on first XAUUSD tick.
- If `[L2-CSV-OPEN-FAIL]` shows, it's a filesystem permission issue, NOT code.

If `[L2-DEAD]` fires immediately after deploy:
- Means `g_l2_gold.last_update_ms` isn't being updated. The writer is in `fix_dispatch.hpp` (S8 fix) — confirm it's calling `al->last_update_ms.store(now_ms, …)` on every FIX `W`/`X` message for XAUUSD.

---

## Chat budget guidance

S13 took roughly 65% of a single session's context. S14's verification + Tier-1 work (rename + signature cleanup + PAT redaction) should fit comfortably in one fresh session — together they total ~25 file touches but each is small and well-bounded.

The PAT-redaction task is the largest of the three (9 files, similar redact pattern) and should be done LAST in the session because it requires a token rotation and re-push, both of which the user may want to schedule outside the session window.

---

## Files referenced by this handoff

- `include/CTraderDepthClient.hpp` — DELETED in S13
- `include/RealDomReceiver.hpp` — DELETED in S13
- `omega_config.ini`, `omega_main.hpp`, `globals.hpp`, `omega_runtime.hpp`, `omega_types.hpp`, `on_tick.hpp`, `tick_gold.hpp`, `quote_loop.hpp`, `engine_config.hpp`, `engine_dispatch.hpp`, `OmegaFIX.hpp`, `OHLCBarEngine.hpp`, `OMEGA.ps1`, `VERIFY_STARTUP.ps1`, `omega-terminal/src/api/types.ts` — modified in S13.
- `HANDOFF_S13_CTRADER_CULL.md` — original cull spec, now redacted.
- `NEXT_SESSION.md`, `SESSION_HANDOFF_2026-04-29.md`, `docs/SESSION_*.md` — historical notes; one of them carries the original Tier-1 / Tier-2 priority list.

---

Good luck. The cTrader surface is gone for good now — every entry from this point on is FIX-fed end-to-end.
