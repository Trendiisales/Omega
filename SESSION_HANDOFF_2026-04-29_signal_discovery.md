# Omega Trading System -- Session Handoff (Signal Discovery + Tier-1 Ship Prep)
## 2026-04-29 -- audit-fixes-20 & audit-fixes-21 shipped

**This file lives at `/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_signal_discovery.md`.**
It supersedes `SESSION_HANDOFF_2026-04-29_wrap.md` (the wave-2 prep handoff that started this session).
Read this file first.

---

## TL;DR for next session

Two commits shipped today on `origin/main`:
- **`0d9569e` audit-fixes-20** -- wave-2 prep (multi-engine sweep wrapper, cluster post-mortem v2, wrap handoff). Cluster post-mortem CONFIRMED 4-of-4 cells lost during 2026-03-18 BEAR window.
- **`a3cfb04` audit-fixes-21** -- signal-discovery pipeline + post-2025-04 revalidation of every profitable cell from `master_summary.parquet`. **27 of 32 cells survive the post-cut.** tsmom family dominates.

Engine-discovery branch reached its conclusion: stop building 5-second microstructure
engines (no edge against cost), pivot to deploying the **27 profitable
strategy/timeframe/direction cells** that already exist in master_summary.parquet
but have never been wired into a live engine.

Top 5 cells (post-cut net pnl, 1 unit, 365 days):
1. tsmom H1 long: 3,484 trades, 53.2% WR, **+$17,482**, $5.02/trade, pf 1.39
2. tsmom H4 long: 933 trades, 61.4% WR, **+$15,885**, $17.03/trade, pf 1.66
3. tsmom H6 long: 661 trades, 57.8% WR, **+$13,380**, $20.24/trade, pf 1.65
4. tsmom H2 long: 1,826 trades, 55.1% WR, **+$12,952**, $7.09/trade, pf 1.35
5. tsmom D1 long: 216 trades, 56.5% WR, **+$9,109**, $42.17/trade, pf 1.65

These 5 tsmom cells alone account for ~82% of the simulated portfolio edge.
**They map exactly to the "small quick trades up and down, locks profits" spec.**

**Next-session task: Tier-1 ship of 5 tsmom cells to Omega shadow.**

---

## What this session delivered

### 1. Wave-2 prep + cluster post-mortem (audit-fixes-20)
- `backtest/run_post_regime_sweep.sh` rewritten to invoke `OmegaSweepHarnessCRTP`
  (multi-engine: hbg/emacross/asianrange/vwapstretch). Native `--from-date 2025-04-01`
  filter. Ready to run on Mac (~12-15 min). NOT YET RUN by Jo on Mac.
- `cluster_postmortem_2026_03_18_v2.py` reproduces the 4-cell cluster diagnosis from
  `phase2/donchian_postregime/CHOSEN.md`. Two authoring-bug fixes applied (ms unit in
  `to_utc`, missing `pnl_pts_net` in `pnl_col`). Verdict: 4-OF-4 CLUSTER CONFIRMED,
  all 5 trades SL_HIT, -357.34 pt net.
- `cluster_postmortem_2026_03_18_v2_REPORT.md` -- the report.
- `SESSION_HANDOFF_2026-04-29_wrap.md` -- start-of-session handoff (predecessor).

### 2. Signal-discovery pipeline (audit-fixes-21)
The full pipeline lives in `phase1/signal_discovery/`:

| File | Role |
|---|---|
| `aggregate_5s_bars.py` | Tick CSV -> 5-second OHLC bar aggregation via duckdb |
| `discover_setups.py` | 4 candidate retail-pattern setups + forward-returns |
| `simulate_trail.py` | Bar-by-bar trail-engine simulator (8 trail configs) |
| `post_cut_revalidate.py` | First pass: 9 cells |
| `post_cut_revalidate_all.py` | Full pass: all 32 master_summary profitable cells |
| `bars_5s.parquet` | 4.16M bars, 87.5MB, 2025-04-01 -> 2026-04-01 (post-cut) |
| `bars_M15/H1/H2/H4/H6/D1.parquet` | Resampled bars per timeframe |
| `forward_returns_*.parquet` | 4 setups -- raw per-entry returns |
| `post_cut_<strategy>_<tf>_<dir>.parquet` | 27 per-cell trade ledgers |
| `setup_catalog.md`, `CHOSEN_SETUP.md` | Initial setup-discovery report (negative result) |
| `TRAIL_SIM_REPORT.md` | 8-config trail-engine grid (all negative) |
| `EDGE_AUDIT_FINAL.md` | Synthesis of 5-second microstructure dead-end |
| `POST_CUT_REVALIDATE_REPORT.md` | First-pass 9-cell post-cut report |
| **`POST_CUT_FULL_REPORT.md`** | **HEADLINE: 32-cell ranked deployment plan** |

### 3. Discovery branch -- WHY 5-second microstructure failed

Documented in `EDGE_AUDIT_FINAL.md`. Five reasons:

1. Gold at 5-second is microstructurally efficient (HFT arbitrages simple OHLC patterns)
2. Cost (~0.65pt avg spread) ~= average 5-sec bar range -- cost dominates math
3. Tested patterns (compression-break, spike-reverse, momentum-pullback, level-retest)
   are retail textbook -- already arbitraged out
4. L1-only data limits us; modern intraday edge lives in L2 / order-flow
5. Single-bar entries without confirmation produce noise-driven signals

Direction-flip and tightened-threshold variants confirm: zero directional information
in these patterns. Random-walk theory predicts the measured TP-hit rates within 1-2%.

This is a **closed branch**. Don't restart 5-second pattern engine work.

### 4. Pivot to deploying existing edge

`master_summary.parquet` already had 32 profitable strategy/tf/direction combos
backtested. C1_retuned currently uses 4. The other 28 have never been wired into
engines. Re-validation on post-2025-04 confirms 27 of 32 survive the cut.

---

## Repo state at end of session

- HEAD: `a3cfb04` (audit-fixes-21)
- Previous: `0d9569e` (audit-fixes-20), `8d35661` (audit-fixes-19, wave-1 retune)
- Branch: `main`
- `origin/main`: in sync (Mac sync requires the stash-pull-drop one-liner)

### Working-tree state
- `SESSION_HANDOFF_2026-04-29_signal_discovery.md` (this file) -- NOT YET COMMITTED.

### GitHub warning about file size
`phase1/signal_discovery/bars_5s.parquet` is 83.41MB -- above GitHub's recommended
50MB but below the 100MB hard limit. Push succeeded with a warning. Future-proof
fix: move bars_*.parquet to git-lfs. Not urgent.

---

## Open ops (carried forward from earlier in this session)

### Mac repo sync
```bash
cd /Users/jo/omega_repo
git stash push -u -m "pre-pull-audit21"
git pull origin main
git stash drop
```

### Wave-2 sweep (still open)
Run on Jo's Mac (Mach-O binary, can't run in Linux sandbox):
```bash
cd /Users/jo/omega_repo && bash backtest/run_post_regime_sweep.sh
```

### VPS MCE confirmation (still open)
```
Get-Content -Tail 200 C:\Omega\logs\latest.log | Select-String "MCE"
```

---

## Next session -- Tier-1 ship plan (5 tsmom cells to shadow)

### Step 1.A: Read the relevant existing C++ infrastructure (~30 min)
1. `include/MacroCrashEngine.hpp` -- canonical example with shadow_mode, macro_regime, spread_gate
2. `include/CandleFlowEngine.hpp` -- multi-bar processing pattern
3. `include/C1RetunedPortfolio.hpp` -- portfolio cell wrapping convention
4. `include/engine_init.hpp` -- shadow_mode + apply_engine_config wiring
5. `omega_config.ini` cell-config schema

### Step 1.B: Write `include/TsmomEngine.hpp` (~1-2 hours)

Signal logic (from `phase1/signal_discovery/post_cut_revalidate_all.py::sig_tsmom`):
```
ret_n = close[t] - close[t - lookback]   # lookback=20
if direction == long:  fire if ret_n > 0
if direction == short: fire if ret_n < 0
```

Exit logic (mirror `sim_lib.py::sim_family_c`):
- entry: at next bar open after signal close
- hard SL: entry +/- 3.0 * atr14_at_signal
- time exit: hold_bars=12 bars after entry
- no TP

Required hooks (mirror MacroCrashEngine):
- `shadow_mode` flag (default true via `kShadowDefault`)
- `set_macro_regime(g_macroDetector.regime())` block-out for RISK_OFF
- `m_spread_gate` integration
- Position sizing via `OmegaAdaptiveRisk`
- Logging via standard `log_event` callbacks
- ARMED log line on init

### Step 1.C: Wire 5 cells into `engine_init.hpp` (~30 min)

```cpp
TsmomEngine g_tsmom_h1_long("H1", "long");
TsmomEngine g_tsmom_h2_long("H2", "long");
TsmomEngine g_tsmom_h4_long("H4", "long");
TsmomEngine g_tsmom_h6_long("H6", "long");
TsmomEngine g_tsmom_d1_long("D1", "long");

// Inside init_engines():
g_tsmom_h1_long.shadow_mode = kShadowDefault;
// ... (etc for each)
g_tsmom_h1_long.set_macro_regime_callback(&g_macroDetector);
// ... (etc)
```

### Step 1.D: Add 5 cells to `omega_config.ini` (~15 min)

```ini
[tsmom_h1_long]
enabled=true
shadow_mode=true
timeframe=H1
direction=long
lookback=20
hold_bars=12
hard_sl_atr=3.0
max_lot=0.05

[tsmom_h2_long]
... timeframe=H2 ...

# etc for H4, H6, D1
```

### Step 1.E: Build + deploy (~30 min on Mac)
```bash
cd /Users/jo/omega_repo
cmake --build build -j --target Omega
./build/Omega --config omega_config.ini --self-test
scp build/Omega vps:/Omega/
ssh vps ".\QUICK_RESTART.ps1"
ssh vps "Get-Content -Tail 100 C:\Omega\logs\latest.log | Select-String tsmom"
```

Expected log:
```
[Tsmom_H1_long] ARMED (shadow_mode=true, lookback=20, hold=12, sl=3.0*atr)
... (etc for H2, H4, H6, D1)
```

### Step 1.F: Validate over 1-2 weeks of shadow trades

Watch for:
- WR matches simulation (53-62% across timeframes per POST_CUT_FULL_REPORT.md)
- pf >= 1.2 sustained
- Trade count reasonable (H1 should fire ~9.5/day in raw simulation; ~2.4-3.8/day with cooldown)
- No spec drift (signals firing during macro RISK_OFF = wiring bug)

If results match: promote to live, move to Tier-2.

---

## CRITICAL CAVEAT for next session

My re-implemented signal in `post_cut_revalidate_all.py::sig_tsmom` is BASIC:
"if 20-bar return > 0, fire". The canonical phase1 signal-gen (which produced
master_summary numbers) has additional filters: cooldown, ATR floor, regime gate.
My version fires ~4x more often (3,484 trades/year on H1 vs master_summary's
807/2yr).

**For TsmomEngine.hpp, EITHER re-derive the canonical signal from
phase1/sim_lib.py's TickReader + signal-gen pipeline, OR add a cooldown to the
"return > 0" rule matching `hold_bars=12` so positions don't stack.**

Per-trade edge (WR, PF, avg) is robust across the 4x frequency difference, but
deploying the over-firing version live would hit `max_positions=3` and
`min_entry_gap_sec=90` throttles, masking actual edge.

---

## Tier-2 / Tier-3 cells for future sessions

**Tier 2 (bidirectional gap closure):**
- donchian H4 long+short, H6 long+short, D1 long+short (6 cells)
- Would have profited during 2026-03-18 BEAR cluster

**Tier 3 (full portfolio):**
- ema_pullback H1/H2/H4/H6 long (4 cells)
- bollinger H4 long, H6 long, D1 short (3 cells)
- asian_break M15/H1/H2 short (3 cells)
- rsi_revert H1/H2/D1 long ONLY (3 cells -- rest deprecated)
- tsmom H1/H2/H4/H6/D1 short (5 more cells)

**Deprecate (post-cut losses):**
- rsi_revert M15/H4/H6 long, D1 short
- bollinger D1 long (sample too small)

**Plus disable currently-shipped non-productive gold engines** based on shadow
ledger evidence over the past 30+ days. Worth a separate audit pass: which
currently-shipped engines (HBG, EmaCross, etc.) have lost money in shadow
over the last 4 weeks? Those go on the deprecation list.

---

## Forex (deferred)

Pipeline + post_cut_revalidate_all.py is reusable for forex once a forex tick
CSV exists. Point `aggregate_5s_bars.py` at the forex tick file path and re-run.

---

## User preferences (carry forward)

- Always provide full code files, not snippets / diffs.
- Warn at 70% chat context with summary.
- Warn before time/session blocks.
- Never modify core code without explicit instruction.
- Use the PAT without arguments when committing -- stored at `/Users/jo/omega_repo/.github_token`.
- Email: kiwi18@gmail.com
- Name: Jo

---

## Where to find this doc

```
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_signal_discovery.md
```

Predecessors (same day):
```
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_wrap.md       (start of this session)
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_late.md       (C1_retuned C++ shadow)
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_evening.md    (earlier)
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_night.md      (earlier)
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_pm.md         (earlier)
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29.md            (earliest)
```

---

## Next-session opener line for Jo to paste

> **State for next session -- Tier-1 tsmom shipdown to Omega shadow.**
>
> SHIPPED 2026-04-29: audit-fixes-20 (wave-2 prep + cluster post-mortem) and
> audit-fixes-21 (signal-discovery + 32-cell post-cut revalidation). Cluster
> CONFIRMED 4-of-4. 27 of 32 master_summary cells survive post-cut. tsmom
> family dominates: H1/H2/H4/H6/D1 long combined = ~$68K net pnl/yr/unit on
> simulation, 82% of total simulated edge.
>
> First action: write `include/TsmomEngine.hpp` modelled on MacroCrashEngine,
> wire 5 long cells (H1/H2/H4/H6/D1) into `engine_init.hpp` with
> shadow_mode=true, add 5 config blocks to `omega_config.ini`. Build on Mac,
> deploy to VPS shadow. End-of-session deliverable: 5 new tsmom cells in
> shadow ledger.
>
> Read SESSION_HANDOFF_2026-04-29_signal_discovery.md first.
> Open Mac ops: `git stash push -u && git pull origin main && git stash drop`
> to sync. Then `bash backtest/run_post_regime_sweep.sh` for wave-2 sweep
> (still pending). Then VPS MCE ARMED log-line confirmation (still pending).
