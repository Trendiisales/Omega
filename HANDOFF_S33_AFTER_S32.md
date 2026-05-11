# HANDOFF S33 -- After S32 (Option A geometry SHADOW-deployed; reconciliation gap is the new P1)

Session date: 2026-05-11 (Monday evening NZ, mid-Asia at deploy time)
Branch: `main` at `263e278` (S33 commit landed; pushed origin/main)
Mode on VPS (running): `SHADOW` (now from git, not from manual edit)
Mode at HEAD on origin: `SHADOW` (committed in S33, no longer regressing)
max_lot_gold: `0.01` (unchanged)
KILL_MICROSCALPER sentinel on VPS: present
This session's commits: 1 (geometry + config), then a follow-up commit will land at the very end of S33 with the comment fix + two new research binaries.

## 0. THE ONE PARAGRAPH SUMMARY THE NEXT SESSION MUST READ FIRST

S33 did three things. (1) Committed `mode=SHADOW` to origin/main alongside the S30/S31 wide-fine TOP-1 geometry (z=2.0, W=200, TP=35, SL=12, Asia-only 0-7 UTC, all S24 BE/trail/reversal/L2-flip safety features disabled per S32 §3.2 verbatim) into `include/GoldMicroScalperEngine.hpp`, deployed via OMEGA.ps1, and verified the engine is firing in shadow with the new geometry (FIRE SHORT @ 4674.50 sl=4686.50 tp=4639.50 z=2.19, all gates checked). (2) Operator clarified that the May-8 NZ$310 live bleed was NOT a hidden-cost issue ($0.06/RT was correct) but a GUI-vs-cTrader RECONCILIATION gap -- Omega's dashboard reported small losses while cTrader executed materially worse fills + orphan legs. The smoking gun: `omega_main.hpp:281` writes a 32-column trade CSV that does NOT include any of the `broker_*` fields (broker_pnl, broker_entry_filled, broker_close_filled, broker_entry_fill_px, broker_close_fill_px, entry_clOrdId, close_clOrdId) that already exist on the in-memory `TradeRecord` struct (defined in `include/OmegaTradeLedger.hpp:79-125`). FIX ExecutionReports populate those fields in memory via `handle_execution_report`, but the GUI never sees them. (3) Built two new standalone C++ research binaries that ship in this commit, both runnable without any redeploy: `backtest/ledger_reconcile.cpp` (Omega vs cTrader CSV diff with orphan detection and per-exit-reason gap breakdown) and `backtest/l2_edge_sweep.cpp` (CRTP signal-discovery harness across 6 candidate signal families x 4 session windows x multi-symbol with auto-detect from filename, supports both S19 L2 schema and 3-column Dukascopy/HistData ticks, per-symbol baselines for XAUUSD/US500/USTEC/NAS100/EURUSD/GBPUSD/USDJPY). The next session's first task is the cTrader ledger export from May 8-9 plus running `ledger_reconcile` against `omega_trade_closes_2026-05-08.csv` -- that single diff will tell us whether the GUI gap is fill-price, missing legs, or PnL math, and which of the three suspect protected files needs a fix.

## 1. WHAT IS RUNNING vs WHAT IS NOT RUNNING

Running on the VPS right now:

* `Omega.exe` built from commit `263e278` (assuming next deploy lands the comment fix below; current binary is `263e278`).
* `GoldMicroScalperEngine` with S33 Option A geometry:
  * ENTRY_Z = 2.0, ENTRY_LOOKBACK = 200
  * TP_DIST_PTS = 35.0, SL_DIST_PTS = 12.0
  * SESSION_START_HOUR = 0, SESSION_END_HOUR = 7 (Asia only)
  * BE_TRIGGER_PTS = 999.0, TRAIL_DIST_PTS = 999.0 (BE/trail disabled)
  * REVERSAL_LOOKBACK = 0, REVERSAL_DELTA_PTS = 999.0 (unreachable; safety guard added)
  * L2_FLIP_THRESH = 999.0
  * MAX_HOLD_SEC = 7200, COOLDOWN_S = 60
  * L2_IMB_LONG_MIN = 0.50, L2_IMB_SHORT_MAX = 0.50 (CAVEAT: not fully ungated; still biases by direction)
  * MAX_SPREAD = 1.0
  * REGIME_THRESHOLD = 1.0 (Kaufman gate disabled)
  * LIVE_LOT = 0.01
* `mode=SHADOW` -- now from git, not manual edit. Future deploys cannot regress LIVE.
* `KILL_MICROSCALPER` sentinel present.
* L2 capture writing to `C:\Omega\logs\l2_ticks_XAUUSD_2026-05-11.csv`.
* First FIRE verified shadow: `FIRE SHORT @ 4674.50 sl=4686.50 tp=4639.50 z=2.19 spread=0.22 l2_imb=0.47 slope=-0.06 l2_real=1 regime_er=0.022 [SHADOW]` -- arithmetic confirmed (12 = SL distance, 35 = TP distance, z >= 2.0 threshold met, all gates passed, [SHADOW] tag.

NOT running:

* The reconciliation field writer (broker_* TradeRecord fields are populated in memory but never persisted to CSV).
* Any LIVE micro-scalper trade. Triple-redundant safety: engine_init pins shadow_mode=false, KILL_MICROSCALPER flips it back to true within 100 ticks, order_exec.hpp:135 gates send_live_order on g_cfg.mode == "LIVE" (config is SHADOW).

## 2. WHAT S33 SHIPPED

### 2.1 First commit: `263e278` -- engine geometry + persisted SHADOW config
* `omega_config.ini`: `mode=SHADOW` persisted; carries the May-12 operator-instruction comment block.
* `include/GoldMicroScalperEngine.hpp`: full-file rewrite applying S32 §3.1-3.3 verbatim. All historical S19/S22/S23/S24 comment blocks preserved; new S33 block at the top documents the change list, three caveats, and verification criteria.

### 2.2 Second commit (this handoff's commit): comment fix + two new research binaries
* `include/GoldMicroScalperEngine.hpp` -- Caveat 3 in the S33 block was rewritten to point at the GUI-vs-cTrader reconciliation gap (the actual issue) instead of the (now-resolved) cost question. Comment-only; no behavior change.
* `backtest/ledger_reconcile.cpp` -- standalone C++ binary, ~700 lines. Loads Omega's `omega_trade_closes_YYYY-MM-DD.csv` and a cTrader account ledger CSV; matches trades by symbol+side+entry-time-within-window; outputs per-pair diff CSV and a stdout summary with sum_pnl_delta, mean/median/p95 of |entry_px_delta|, |exit_px_delta|, |pnl_delta|, orphan counts (Omega-only and broker-only), and per-exit-reason gap breakdown.
* `backtest/l2_edge_sweep.cpp` -- standalone CRTP signal harness, ~750 lines. Modeled on `microscalper_crtp_sweep.cpp`. Sweeps 6 candidate signal families across 4 session windows (asia 0-7, london 7-12, ny 12-21, overnight 21-24, plus 24h) over multi-symbol L2 + tick corpora. Per-symbol bracket baselines (XAUUSD/US500/USTEC/NAS100/EURUSD/GBPUSD/USDJPY). Auto-detects symbol from filename and CSV format from header (S19 16-column L2 vs 3-column Dukascopy/HistData). Outputs ranked CSV + stdout leaderboard.

### 2.3 Smoke tests passed
* `ledger_reconcile`: --help works, parses both CSV formats, correctly flags orphan rows when timestamps mismatch.
* `l2_edge_sweep`: ran zscore family across 2 days of XAUUSD (1 L2 capture + 1 Dukascopy day) for asia + london sessions; ran multi-symbol XAU+US500+USTEC for NY session, per-symbol baselines correctly applied.

### 2.4 NOT shipped this session
* Any modification to protected core files (rule 3): `order_exec.hpp`, `OmegaTradeLedger.hpp`, `trade_lifecycle.hpp`, `omega_main.hpp`, `microscalper_crtp_sweep.cpp`, `IndexFlowEngine.hpp`, `RiskMonitor.hpp`. The reconciliation fix lives in `omega_main.hpp:281` (CSV header + writer) and possibly `trade_lifecycle.hpp` (verify handle_execution_report wiring) and needs explicit operator sign-off per rule 3 / 4.
* No Python (operator directive: stop building in Python; everything in C++/CRTP).
* No engine source change beyond the comment fix.
* Other 8 tracked-modified files in S32 §5 still uncommitted (intentional -- they predate S33 and need per-file operator review).

## 3. THE RECONCILIATION GAP (NEW P1)

### 3.1 Diagnosis
Omega's in-memory `TradeRecord` (include/OmegaTradeLedger.hpp:79-125) defines:
```
std::string entry_clOrdId, close_clOrdId;
bool        broker_entry_filled, broker_close_filled;
bool        broker_entry_rejected, broker_close_rejected;
double      broker_entry_fill_px, broker_close_fill_px;
double      broker_pnl;
```
Plus `OmegaTradeLedger::brokerRealisedPnl()` and `brokerOrphanCount()` accessors (include/OmegaTradeLedger.hpp:299+).

The disparity check in trade_lifecycle.hpp:546-667 already computes `disp = engine_pnl - broker_pnl` and logs warnings.

But the trade-CSV writer in omega_main.hpp:281 emits ONLY 32 columns:
```
trade_id,trade_ref,entry_ts_unix,entry_ts_utc,entry_utc_weekday,
exit_ts_unix,exit_ts_utc,exit_utc_weekday,symbol,engine,side,
entry_px,exit_px,tp,sl,size,gross_pnl,net_pnl,
slippage_entry,slippage_exit,commission,
slip_entry_pct,slip_exit_pct,comm_per_side,
mfe,mae,hold_sec,spread_at_entry,
latency_ms,regime,exit_reason,l2_imbalance,l2_live
```
No broker_* fields. So whatever the GUI reads from disk shows engine truth only -- broker truth is in memory and dies at process exit.

### 3.2 Hypotheses (ranked, to confirm via S34 ledger_reconcile run)
1. **Persistence gap (most likely).** Engine ledger has broker_pnl in memory; CSV doesn't carry it; GUI reads CSV; GUI lies. Fix: extend omega_main.hpp:281 header + writer to include broker_* columns. Requires protected-file edit; operator review.
2. **handle_execution_report wiring incomplete.** Even in memory, broker_pnl may be 0 because the FIX ExecReport pipeline drops some fields. Sub-hypothesis under (1); confirm by adding debug logging to handle_execution_report.
3. **Hedging close-side FIX tag bug (S21 fix incomplete).** Close orders use FIX tag 1006 (Spotware) / 721 (FIX 4.4); if the broker rejects on a wrong tag combo, the close leg goes orphan. The May-8 bleed pattern (orphan-pair incidents) is consistent.
4. **Tick-multiplier double-application (Apr-29 audit C-3).** Lifecycle path applies the multiplier twice in some paths. Currently mitigated by `reload_trades_on_startup=false`. May still bite live trades.

### 3.3 Confirmation procedure (S34 first task)
1. Operator exports cTrader account ledger for 8077780, May 8 and May 9 separately, as CSV.
2. Pull `C:\Omega\logs\trades\omega_trade_closes_2026-05-08.csv` and `omega_trade_closes_2026-05-09.csv` from VPS.
3. Build the new tool:
   ```
   clang++ -std=c++17 -O2 -Wall -Wextra -I include \
       backtest/ledger_reconcile.cpp -o backtest/ledger_reconcile
   ```
4. Run for May 8:
   ```
   backtest/ledger_reconcile \
       --omega   /path/to/omega_trade_closes_2026-05-08.csv \
       --ctrader /path/to/ctrader_account_8077780_2026-05-08.csv \
       --symbol  XAUUSD \
       --out     backtest/reconcile_2026-05-08.csv \
       --verbose
   ```
5. Read the printed summary. The interpretation guide is built into the tool's stdout footer:
   * If `sum_pnl_delta` is negative and roughly matches the NZ$ bleed: hypothesis 1 confirmed; the ledger_reconcile is the diagnostic to keep running daily until the broker fields land in CSV.
   * If `Orphan Omega` count is non-zero: hypothesis 3 also active; hedging close path is dropping legs.
   * If per-exit-reason breakdown shows the gap concentrated on SL_HIT or BE_HIT: slippage/fill model on the engine is undercounting the loss at stop-out.
   * If gap concentrates on TP_HIT: TP fill reporting is broken (less likely, but worth confirming).
6. Repeat for May 9.

## 4. EDGE-DISCOVERY ROADMAP (NEW P2)

### 4.1 Available data
* L2 captures (S19 schema, 16 columns including l2_imb, l2_bid_vol, l2_ask_vol):
  * XAUUSD: 15 days (2026-04-22 -> 2026-05-08) plus 16 legacy unprefixed (2026-04-09 -> 2026-04-22, all XAUUSD).
  * US500: 15 days (2026-04-22 -> 2026-05-07).
  * USTEC: 15 days (2026-04-22 -> 2026-05-07).
  * NAS100: 3 days (2026-05-06 -> 2026-05-08).
* Dukascopy XAUUSD daily (3-column ts_ms,bid,ask): 623 files spanning 2023-09-27 -> 2025-09-26 (~2 years).
* HistData EURUSD daily (3-column): 184 files in `outputs/histdata_eurusd_daily/`.
* outputs/eurusd_daily/ (additional EUR coverage).

### 4.2 Sweep plan for S34 / S35
Run `l2_edge_sweep` per symbol, all sessions, all 6 families, against the full corpus. Suggested commands (operator can run any subset):

XAUUSD full sweep (live L2 + 2-year history, all sessions):
```
backtest/l2_edge_sweep \
    --csv 'data/l2_ticks_XAUUSD_*.csv' \
    --csv 'data/l2_ticks_2026-*.csv' \
    --csv 'outputs/duka_xauusd_daily/*.csv' \
    --session asia --session london --session ny --session overnight \
    --top 30 --verbose --out backtest/edge_sweep_xauusd.csv
```

US500 + USTEC + NAS100 (NY-focused, indices have no overnight):
```
backtest/l2_edge_sweep \
    --csv 'data/l2_ticks_US500_*.csv' \
    --csv 'data/l2_ticks_USTEC_*.csv' \
    --csv 'data/l2_ticks_NAS100_*.csv' \
    --session ny --session london \
    --top 30 --verbose --out backtest/edge_sweep_indices.csv
```

EURUSD (Dukascopy historical):
```
backtest/l2_edge_sweep \
    --csv 'outputs/histdata_eurusd_daily/*.csv' \
    --csv 'outputs/eurusd_daily/*.csv' \
    --session london --session ny --session asia \
    --family zscore,kaufman \
    --top 20 --verbose --out backtest/edge_sweep_eurusd.csv
```
(L2-dependent families fall back to "no signal" on 3-column data.)

### 4.3 Survivor pipeline
For each symbol, take the top 5-10 cells by net PnL with n_trades >= 50 and Sharpe >= 0.5. Port them into the existing `honest_backtest_xauusd_v2.cpp` style harness for walk-forward validation with strict purged splits. Only candidates that survive walk-forward go into the engine as new shadow instances (separate engine class; do NOT modify GoldMicroScalperEngine).

### 4.4 Signal families currently implemented (extend by adding a CRTP type)
1. `ZScoreMeanRev<W,Z>` -- 7 cells (W in {50,200,500}, Z in {2.0,2.5,3.0})
2. `L2ImbMomentum<K,T>` -- 6 cells (K in {5,10,20,50}, T in {0.10,0.15,0.20})
3. `SpreadCompressionBreak<W,P>` -- 4 cells (W in {200,500}, P in {0.20,0.50,1.00,2.00})
4. `VacuumRebound<W,Q>` -- 4 cells (W in {200,500}, Q in {0.10,0.20})
5. `SlopeAcceleration<W,A>` -- 4 cells (W in {20,50}, A in {0.001,0.005})
6. `KaufmanRegimeFlip<W,T,Z>` -- 4 cells (W in {200,500}, T in {0.18,0.25}, Z in {2.0,2.5})

Total: 29 cells per (symbol, session). For full XAU sweep across 4 sessions: 116 cells.

## 5. NEXT-SESSION ACTION LIST (priority order)

5.1 Pull cTrader ledger CSV for May 8-9, account 8077780. Run `ledger_reconcile` per §3.3. Report sum_pnl_delta and orphan count. **This is the highest-value data exercise in the project and gates any further LIVE conversation.**

5.2 If §3.3 confirms hypothesis 1, propose protected-file edit to `omega_main.hpp:281` (extend trade CSV header + writer with broker_pnl, broker_entry_filled, broker_close_filled, broker_entry_fill_px, broker_close_fill_px, entry_clOrdId, close_clOrdId). Operator must sign off before commit. Same for the GUI reader if applicable.

5.3 Run S33 shadow telemetry pull from VPS:
```powershell
Get-Content (Get-ChildItem C:\Omega\logs\omega_*.log |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName |
  Select-String "MICRO-SCALPER-GOLD\] (FIRE|EXIT)" | Measure-Object | Select Count
Get-Content (Get-ChildItem C:\Omega\logs\omega_*.log |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName |
  Select-String "MICRO-SCALPER-GOLD\] (FIRE|EXIT)"
```
Compare fire count vs ~1.16/day backtest baseline on Asia 0-7 UTC. >5x or <0.1x deviation warrants investigation BEFORE LIVE talk.

5.4 Run the §4.2 edge sweeps (XAU, indices, EUR). Operator review of leaderboards. Any survivor with n>=50 and Sharpe>=0.5 goes into walk-forward queue.

5.5 Walk-forward validation of survivors -- this needs a new C++ binary (suggest `backtest/walk_forward.cpp`, also CRTP-based). NOT shipped in S33; design pending in next session.

5.6 Carried forward (lower priority):
* Friday postmortem narrative once §3.3 confirms which hypothesis.
* Mutex try/finally fix for OMEGA.ps1:807 (orphaned `Global\OmegaDeployMutex` after Ctrl+C).
* Per-deploy stamp-checked rebuild gating (currently the deploy will happily rebuild even if the source hasn't changed).

5.7 DO NOT:
* Flip mode=LIVE.
* Modify protected files without explicit per-decision operator sign-off.
* Promote any l2_edge_sweep survivor to engine without walk-forward + 1+ week of shadow.
* Build any Python research code (operator directive S33).
* Remove KILL_MICROSCALPER until 24h+ of clean shadow on the new geometry.

## 6. POST-DEPLOY VERIFICATION CHECKLIST (operator already ran S33; repeat after any redeploy)

```powershell
Get-Service Omega
Select-String -Path C:\Omega\omega_config.ini -Pattern "^mode="
Test-Path C:\Omega\KILL_MICROSCALPER
Select-String -Path C:\Omega\include\GoldMicroScalperEngine.hpp -Pattern "S33 OPTION A SHADOW PORT"
Get-ChildItem C:\Omega\logs\omega_*.log | Sort-Object LastWriteTime -Descending |
  Select-Object -First 1 | ForEach-Object { Get-Content $_.FullName -Tail 400 } |
  Select-String "MODE|MICRO-SCALPER-GOLD|RISK-MON"
```

Expected:
1. `Status : Running`
2. `mode=SHADOW`
3. `True`
4. One match on the S33 header
5. `[MODE] mode=SHADOW`, `window=.../200`, FIRE lines tagged `[SHADOW]`

If any fails: `Stop-Service Omega -Force` and diagnose.

## 7. MAC UNCOMMITTED STATE (still present, will revert on next VPS git pull)

These 8 tracked-modified files in S32 §5 are STILL uncommitted:
```
M backtest/IndexBacktest.cpp
M backtest/microscalper_crtp_sweep.cpp
M data/l2_ticks_2026-04-16.csv
M include/IndexFlowEngine.hpp
M include/OmegaTradeLedger.hpp
M include/RiskMonitor.hpp
M include/omega_main.hpp
M include/order_exec.hpp
M include/trade_lifecycle.hpp
```
Per-file operator review still required. DO NOT `git add -A`.

## 8. SAFETY INVARIANTS

1. mode=SHADOW until explicit operator authorisation to flip LIVE.
2. max_lot_gold=0.01 until explicit operator authorisation.
3. Never modify the protected core engine files: `microscalper_crtp_sweep.cpp`, `omega_main.hpp`, `order_exec.hpp`, `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`, `RiskMonitor.hpp`, `trade_lifecycle.hpp`.
4. `include/GoldMicroScalperEngine.hpp` is in the "touch only with operator sign-off" tier (S32 invariant 4, carried forward).
5. Full file output when modifying any file (operator preference).
6. Warn at 70% context with summary (operator preference).
7. KILL_MICROSCALPER stays on VPS until 24h+ of clean shadow on the new geometry.
8. Do not commit other tracked-modified files without explicit per-file operator review.
9. **NEW S33**: All research code is C++/CRTP. No Python (operator directive 2026-05-11).
10. **NEW S33**: Standalone backtest/* binaries that don't link into the engine binary are NOT protected; they can be created/extended freely without redeploy disruption. The two new tools (ledger_reconcile, l2_edge_sweep) are in this category.

## 9. FILES TOUCHED THIS SESSION

Modified at HEAD (committed):
* `omega_config.ini` -- `mode=SHADOW` persisted (commit 263e278)
* `include/GoldMicroScalperEngine.hpp` -- S33 Option A geometry port (commit 263e278), then comment-only Caveat 3 update (this session's second commit)

Created at HEAD (this session's second commit):
* `backtest/ledger_reconcile.cpp` -- Omega vs cTrader CSV diff tool
* `backtest/l2_edge_sweep.cpp` -- multi-symbol multi-session CRTP signal sweep
* `HANDOFF_S33_AFTER_S32.md` -- this file

Not modified (rule 3 / rule 4 / rule 9 compliance):
* All protected core engine files (rule 3).
* `engine_init.hpp` (untouched; live-pin still in place, gated harmless by mode=SHADOW + KILL + order_exec gate).

## 10. NEXT-SESSION FIRST-MESSAGE TEMPLATE

Read `HANDOFF_S33_AFTER_S32.md` end to end before anything else, plus S32 / S31 / S30 / S29 for full context. Then in order:

1. Confirm S33 commits (`git log --oneline -3` should show 263e278 + S33 follow-up).
2. Pull yesterday's shadow log tail from VPS via §6 commands. Report fire count and any [LIVE] tags.
3. Ask operator for cTrader ledger CSV for May 8 + May 9 (account 8077780). Build `backtest/ledger_reconcile` and run it per §3.3. Report the summary.
4. Based on ledger_reconcile output, propose the protected-file edit to omega_main.hpp:281 (broker_* fields in CSV writer). Wait for sign-off.
5. In parallel: build `backtest/l2_edge_sweep` and kick off the §4.2 sweeps. Don't promote anything yet.
6. Do not flip mode=LIVE under any circumstance in this session.

End of S33 handoff.
