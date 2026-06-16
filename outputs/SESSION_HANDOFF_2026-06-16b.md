# SESSION HANDOFF — 2026-06-16b

**Read `OMEGA.md` first**, then this. Massive session. Everything below is committed +
pushed to origin/main; the **live VPS binary is deployed + verified at `5cb6199e`**
(commits after it — `39f9d7e3`/`921c2bf7`/`de20e799` — are standalone *tools*, not in
the binary, so no redeploy needed for them).

## LIVE STATE (verified)
- **Live binary `5cb6199e`** (stderr `Git hash: 5cb6199e7`, HEAD==origin). Contains:
  ORB re-validation (GoldOrb un-tombstoned, NasOrb retuned), survivor regime-dedup,
  + everything through `5cb6199e`. Main book trading **shadow** (GoldSeasonal fired today).
- **IBKR LIVE gateway on 4001** (operator authed, creds/2FA). Data feeds repointed
  4002→4001: `OmegaIbkrBridge`, `OmegaMgcLiveBars`, `OmegaBigCapBridge` (+ `OMEGA_BIGCAP_MKTDATA=1`).
  Recording restored on the live gateway. **Omega still executes BlackBull-FIX, NOT IBKR**
  (IbkrExecutionEngine wired but idle; IBKR = data only).
- **New scheduled tasks:** `OmegaWeeklyReview` (Sat), `OmegaGapperRecorder` (weekdays 22:00),
  `OmegaGexSnapshot` (US-session hourly). The 4 dead order tasks were CULLED; `OmegaGapShortDaily`
  kept (Disabled, paper-safe).

## COMMITS THIS SESSION (all pushed)
`8e223e64` squeeze DEAD · `1ae81f1b` gapshort locate+Intel-myth · `7ffc4711` gapshort ledger+OOS ·
`2ebf2fb8` survivor regime-dedup · `515eee92` bigcap 4001+recorder · `5cb6199e` ORB re-validate **[DEPLOYED]** ·
`39f9d7e3` ledger filter · `921c2bf7` GEX puller · `de20e799` GEX validator+snapshot.

## OUTSTANDING (priority order)

### 1. BigCapMomo → INTO Omega (operator's active ask — "not standalone, show in the GUI")
BigCapMomoEngine is a standalone exe (`ibkr/BigCapMomoEngine.cpp`, built 285KB, NOT running)
that connects to IBKR directly. The `:7783` bridge (`pump/bigcap_feed_bridge.py`, Running) is
**scan-only** — surfaces candidates, no execution/panel. They're disconnected → no trades.
**The Omega GUI running-trades panel is IN-PROCESS telemetry (`g_open_positions` + shared mem)
— a separate exe CANNOT inject.** So the proper build:
  1. Refactor `BigCapMomoEngine.cpp` → an **engine class** header (`on_tick/on_bar` + `on_trade_record`, no `main()`). Keep scan/regime(SPY>200MA)/entry/no-TP-runner logic.
  2. Add an **IBKR US-equity scanner thread to `Omega.exe`** — feasible now: TWS API is linked
     via `OMEGA_WITH_IBKR` (the IbkrExecutionEngine work). It pulls bigcap movers + SPY-regime inside Omega.
  3. Wire `engine_init`: `g_bigcap_momo` shadow_mode=true, register `g_open_positions` +
     `on_trade_record → handle_closed_trade` → shadow ledger + telemetry + GUI (running & closed).
  - This is a CORE-binary build (new data thread + telemetry) → do it carefully, mac-canary, deploy.
  - Ledger schema (42 cols) header is in `logs/trades/omega_trade_closes.csv`. NB: do NOT have a
    separate process append to that CSV concurrently with Omega.exe (interleave risk) — that's why
    it must be in-binary.

### 2. GEX predicate study (the proof)
`OmegaGexSnapshot` (hourly US session) appends `data/gex_history.csv` (SPX/NDX/DAX NTM GEX).
Once each index has ≥40 snapshots (~1 week), run `python ibkr/gex_validate.py data/gex_history.csv`
→ predicate study (P1 vol-dampening, P2/P3 wall reactions, P4 flip mean-rev/trend split vs random).
**If predicates hold OOS → wire net-GEX sign into the regime gate** (pos=favor MR/grid/survivor cells,
neg=favor Donchian/ORB/breakout). NTM works on delayed (no sub); the far-OTM flip needs an OPRA sub.
Touches NO engine routing until validated.

### 3. Gold-scalp tombstone re-audits (systemic, started)
The 2026-06-15 "6mo shadow-BT net -$X" cull batch was poisoned by ledger artifacts (lot=1.0 + phantoms).
**GoldOrb = wrongly culled → revived (PF2.38).** **GoldOversoldBounce re-audited** (harness pattern:
`/tmp/gob_bt.cpp` — include engine, feed 2yr XAU m5 as ticks, lot=0.01): cull -$457 was wrong, real -$36
but genuinely marginal → leave off. **STILL TO RE-AUDIT:** `XauThreeBar30m` (-$371), `Xau3BarMomGatedH4`,
`Us303BarMomH1` — these need engine_init-CONTEXT harnesses (HMM-gate + protection deps; the quick
standalone fired 0 trades because hmm_gate=null blocks). XauStraddle/Donchian/Epb likely legit.

### 4. Gapper backfill (tomorrow NZ daytime, ATTENDED)
3yr gapshort data gone (no historical OI; can't recreate). Forward recorder (`OmegaGapperRecorder`)
accumulates from now. BACKFILL = build a historical puller (per-ticker daily → find ≥75% gap-days →
1-min pull for those days), staged (daily-pass first ~1.5h for a real estimate), GENTLY paced (~5 req/min,
don't starve live feeds), scp → **`/Users/jo/Tick` on the MAC**. Universe = `tools/recent_gappers.csv`
(607 tickers). Reminder was CronCreate `572014ed` (session-only — won't survive; operator messages).

### 5. Live-execution flips (ALL operator-gated)
- GapShort PAPER→live (`--orders`): needs per-trade realized pnl (execDetails/commissionReport) + a schedule.
- BigCap → live (after the in-Omega integration + paper validation).
- Main `execution_broker=IBKR` cutover (the big one): operator decision + full re-validation.

### 6. Survivor book audit
Backtests net-negative on the warmup tape (GER cells culled + USTEC trend-gated since the +$20k baseline).
Check the live shadow ledger: is the post-cull roster (XAU Donch/MA + USTEC RSI/ZMR) still earning?

## KEY MEMORIES (operator auto-memory)
`omega-tombstone-ledger-pollution` (systemic) · `omega-gex-dealer-gamma` · `omega-ibkr-live-data-test-runbook`
(IBKR cutover + GapShort + the 5-task cull) · `omega-squeeze-slingshot` (DEAD).

## KEEPER TOOLS / HARNESSES
- `backtest/orb_gold_retrace.cpp` — ORB lever sweep. Gold PF2.38 @ RETR=0.382 EXITMODE=trail TREND=1
  COST=0.37 MAXTRADES=1 STOP=tight. NAS via `OR_START=930 RETR=0.5`. **zsh gotcha: use prefix env
  assignment (`RETR=0.382 COST=0.37 ./orb file`), NOT `env "$cfg"` (zsh doesn't word-split → swallows into one var).**
- `/tmp/agg_m5.cpp` — tick (`YYYYMMDD HHMMSSmmm,bid,ask`) → m5 bars (ts,o,h,l,c). Used for NAS/US500 ORB.
- `backtest/squeeze_xregime_nas.cpp` — type-erased multi-trait x-regime harness (squeeze DEAD).
- `backtest/survivor_cap_test.cpp` — 3-way (off/blanket/regime) dedup + ER sweep.
- `ibkr/gex_chain.py` (puller, --append) + `ibkr/gex_validate.py` (predicate study).
- `/tmp/gob_bt.cpp` — the gold-engine re-audit pattern (include engine, feed m5-as-ticks, lot 0.01).
- Gold m5 data: `/Users/jo/Tick/2yr_XAUUSD_tick_fresh.m5.csv`. NAS/US500 tick: `/Users/jo/Tick/xregime/*_full_ds10.csv`.

## DEPLOY NOTE
`OMEGA.ps1 deploy -Fast` = git fetch + `reset --hard origin/main` + rebuild + restart. NO `git clean`
(untracked survive) — but tracked files revert to origin/main, so commit any live-edited tracked file FIRST
(this bit the bigcap port repoint). cmake not on PATH; full path under VS BuildTools. Verify stderr `Git hash:`
== origin/main after.
