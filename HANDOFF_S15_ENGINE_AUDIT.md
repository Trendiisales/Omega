# HANDOFF — S15 engine audit (catalog + flag sheet)

Date prepared: **2026-05-08**
Prepared during: S13 cTrader cull + S14 Donchian breakout fail-safe session
Target: a fresh Claude session with full context budget
User intent: full audit of every active engine in Omega, flag issues, identify quick-win improvements grounded in the live shadow tape.

---

## Deliverable

A single markdown file at `~/omega_repo/AUDIT_S15_ENGINES.md` with this structure (one section per engine):

```
## <EngineName>
**Status:** active / shadow_only / culled
**Config source:** [omega_config.ini section]  [engine_init.hpp lines]  [engine .hpp file]
**Trades in shadow tape (last 30 days):** N | WR x% | net $X | mean MAE / mean MFE
**Risk read:** 2-3 sentences on the configuration's safety profile.
**Issues flagged:**
  - [P0/P1/P2] short description of issue, file:line reference.
**Quick-win improvements:**
  - [estimate: 1h / 1 session] specific change, expected effect.
**Defer:**
  - longer-term work that needs backtesting before shipping.
```

P0 = ship-stoppers (open positions could be lost / phantom-fire risk / wrong sizing).
P1 = data quality / reliability (frozen feed handling, unreachable code paths, dead engines burning cycles).
P2 = code hygiene / minor optimisation.

End the file with a "Top 5 quick wins" priority list ranked by effort × impact.

---

## Audit scope: every engine

Read each of these files end-to-end. The list comes from a `Glob include/**/*Engine*.hpp` plus `Glob include/**/*Portfolio*.hpp` plus the cross-asset / index families defined in `CrossAssetEngines.hpp`. Group as below.

### Tier-1 portfolios (Phase 1/2 winners, post-cut survivors)
1. **TsmomEngine** — `include/TsmomEngine.hpp` + `[tsmom]` config
   5 cells: H1/H2/H4/H6/D1 long. Sizing capped at `max_lot_cap=0.02` (S12 risk-budget fix).
2. **DonchianEngine** — `include/DonchianEngine.hpp` + `[donchian]` config
   7 cells: H2 long, H4/H6/D1 long+short. **Just had S14 breakout fail-safe added — confirm it's still in tree before audit.**
3. **EmaPullbackEngine** — `include/EmaPullbackEngine.hpp` + `[ema_pullback]` config
   4 cells: H1/H2/H4/H6 long.
4. **TrendRiderEngine** — `include/TrendRiderEngine.hpp` + `[trend_rider]` config
   6 cells: H2/H4 long+short, H6 long, D1 long. **Highest sizing of all portfolios** (`risk_pct=0.040`, `max_lot_cap=0.50`). Worst-case 6-cell simultaneous-SL ≈ 24% portfolio drawdown — flag if shadow tape shows even 2-cell concurrent loss bursts.
5. **C1RetunedPortfolio** — `include/C1RetunedPortfolio.hpp` + `[c1_retuned]` config
   4 cells: Donchian H1 long retuned + Bollinger H2/H4/H6 long.

### Tier-2 single-engine breakouts and minimal strategies
6. **MinimalH4Breakout** — `include/MinimalH4Breakout.hpp` + `[minimal_h4]` config
7. **MinimalH4US30Breakout** — `include/MinimalH4US30Breakout.hpp` + `[minimal_h4_us30]` config
8. **BreakoutEngine** (XAUUSD compression breakout) — `include/BreakoutEngine.hpp` + `[breakout]` config
9. **HBG / HybridBracket / BracketEngine** — search for `BracketEngine` in include/. `[bracket_gold]` config.

### Symbol-specific engines (per-instrument breakout/momentum)
10. **SpEngine / NqEngine / OilEngine / NbmEngine** — `include/SymbolEngines.hpp` (single header). Config sections `[sp]`, `[nq]`, `[us30]`, `[nas100]`, `[oil]`, `[brent]`, `[fx]`, `[gbpusd]`, `[audusd]`, `[nzdusd]`, `[usdjpy]`, `[eu_index]`.
11. **IndexSwingEngine** (SP / NQ swing entries) — instantiated in `omega_types.hpp:308-309`. `[sp]` / `[nq]` config.

### Cross-asset engines (defined in `CrossAssetEngines.hpp`)
12. **EsNqDivergence** — `[cross_asset]` config. `esnq_enabled=false` currently — **flag if disabled but still being ticked every loop**.
13. **OilFade**
14. **BrentWtiSpread**
15. **FxCascade**
16. **CarryUnwind**
17. **OpeningRangeBreakout (ORB)**
18. **VWAPReversion**
19. **TrendPullbackEngine** (S12 added consec-loss tracking — confirm wired)
20. **NextBarMomentum (NBM)**

### Gold-stack engines (under GoldEngineStack)
21. **GoldEngineStack** as a whole — `include/GoldEngineStack.hpp` + `[gold_stack]` config
22. **CompressionBreakout / ImpulseEntry / SessionMomentum / VWAPSnap / SweepProEngine / SweepPressureEngine** — sub-engines inside `GoldEngineStack.hpp`
23. **PDHLReversionEngine** — `include/PDHLReversionEngine.hpp` (S13 left a literal `0, 0` workaround in the call site — check if engine signature still expects raw_bid/raw_ask)

### Index / L2 flow engines
24. **IndexFlowEngine** + **IndexMacroCrashEngine** — `include/IndexFlowEngine.hpp`. Reads L2 imbalance — confirm post-S13 it's reading `g_l2_*.imbalance` not anything cTrader-derived.

### RSI engines
25. **RSIReversalEngine** — `include/RSIReversalEngine.hpp`
26. **RSIExtremeTurnEngine** — `include/RSIExtremeTurnEngine.hpp` (S12 included a fix for prev_bar_rsi cold-start)
27. **EMACrossEngine** — `include/EMACrossEngine.hpp`

### Ledger / supervisor
28. **OmegaTradeLedger** — read `include/OmegaTradeLedger.hpp` for the trade-recording contract that all engines call into.
29. **MacroRegimeDetector** — `include/MacroRegimeDetector.hpp`. RISK_OFF blocks new entries on portfolio engines (`block_on_risk_off=true`). Confirm the regime detection logic is sane.

---

## Live shadow data (user is providing)

The user is copying live shadow CSVs from the VPS to:
```
~/omega_repo/audit_input/
```
Expected files:
- `omega_shadow.csv` — every shadow trade since the file rolled, in `omega::TradeRecord` row format. Columns include `id, symbol, side, engine, entryPrice, exitPrice, sl, tp, size, pnl, mfe, mae, entryTs, exitTs, exitReason, regime, atr_at_entry, spreadAtEntry, shadow`.
- `omega_shadow_signals.csv` — every signal that fired (whether or not it produced a position). Lighter weight, useful for cross-checking that gates (RISK_OFF / max_concurrent / cooldown) actually fired when expected.
- `trade_close_YYYY-MM-DD.csv` — per-UTC-day rolling close logs. May overlap with omega_shadow.csv. Use whichever has cleaner data.

**If `~/omega_repo/audit_input/` is empty or missing**, prompt the user to run the export — see HANDOFF_S15_ENGINE_AUDIT.md "Step 1" earlier in this conversation thread, OR re-derive: PowerShell on VPS, `Copy-Item C:\Omega\logs\shadow\*.csv $dest`.

### How to read the shadow CSV efficiently

1. Use the file tools' `Read` on the CSV header to confirm the column layout.
2. Use `Bash` (`mcp__workspace__bash`) for aggregation if the workspace VM is healthy. Sample queries:
   - Trades per engine: `awk -F, 'NR>1{print $4}' omega_shadow.csv | sort | uniq -c | sort -rn`
   - WR per engine: `awk -F, 'NR>1{n[$4]++; if($10>0) w[$4]++} END{for(k in n) printf "%s\t%d\t%.1f\n", k, n[k], 100*w[k]/n[k]}' omega_shadow.csv`
   - Worst single trades: `awk -F, 'NR>1{print $10, $4, $1}' omega_shadow.csv | sort -n | head -20`
   - MAE / MFE distributions per engine: similar awk pattern, group by `$4`.
3. **Be careful** — if VM bash is still broken (it was during S13), fall back to Python in a temp script via the file tools, OR the user can run the awk locally and paste results back.

### Specific patterns to look for

These are red flags from prior session notes — check each engine's tape for them:
- Any trade with `mae` distance > 1.5 × `sl` distance (means MAE was hit but SL didn't fire — broken intrabar logic).
- Holds > 12h with `exitReason == "TIME_EXIT"` and `pnl < 0` (slow bleed; candidate for breakout-fail-style early cuts).
- Any engine with WR < 35% over 50+ trades (worse than backtest by a wide margin).
- Same-direction reentries within `min_entry_gap_sec` (gate failure).
- Trades during 22:00–06:00 UTC with WR materially worse than 06:00–22:00 (Asian session edge erosion — would justify a session filter).
- `exitReason == "DOLLAR_STOP"` firing alongside a stale FIX feed (frozen tick driving false MAE).
- `regime == "RISK_OFF"` trades opening anyway (portfolio's `block_on_risk_off` gate misfiring).

---

## Things the audit MUST NOT do

- **No engine code changes during this audit.** Per user pref ("Never modify core code unless instructed clearly"), the audit produces findings only. Each P0/P1 gets a short proposed-fix paragraph; the user picks which to implement in S16+.
- **No backtests.** That was Option D and the user picked Option A. If a finding genuinely needs a backtest to validate, flag it as deferred.
- **No phase1/phase2 Python re-runs.** Reuse the existing `POST_CUT_FULL_REPORT.md` numbers as the backtest baseline and compare live tape against those.

---

## Files of interest beyond engine .hpp

- `include/engine_init.hpp` — wiring of every engine. Confirms which are actually instantiated and ticked vs. dead code.
- `omega_config.ini` — every section that drives an engine. Cross-reference against engine_init.hpp.
- `include/risk_gates.hpp` (or similar) — global risk gates. The engines all flow through them, so a bug here masquerades as a per-engine bug.
- `include/symbol_gate.hpp` — per-symbol gates.
- `phase1/signal_discovery/POST_CUT_FULL_REPORT.md` — backtest baseline numbers. Use these as the reference WR/PF/$/trade for live-vs-backtest comparison.
- `phase2/donchian_postregime/CHOSEN.md` — C1Retuned baseline.
- `NEXT_SESSION.md`, `S12_INVESTIGATIONS.md`, `SESSION_HANDOFF_*.md` — historical findings already known about specific engines.

---

## Output format reminder

Single file: `~/omega_repo/AUDIT_S15_ENGINES.md`. Per-engine sections in the format above. End with the "Top 5 quick wins" list. Don't commit it (audit findings shouldn't ship to the trading binary path). Write it directly to /Users/jo/omega_repo/ — that's a Cowork workspace folder so the user can open it directly.

If the audit finds genuine P0 issues, ALSO write a one-pager `~/omega_repo/AUDIT_S15_P0_FINDINGS.md` — top of file is a short "what to fix today" so the user doesn't have to read the full audit to triage urgent items.

---

## Token budget

Reading 30+ engines + the shadow CSV + writing the audit doc will be the bulk of the new session. Estimate: 80% of one full session. Don't try to also implement fixes in the same session — leave that for S16.

If at the 70% mark the audit is incomplete, write a partial AUDIT_S15_ENGINES.md, mark the unfinished engines as `**Status:** TODO`, and write a short HANDOFF_S16 pointing to where to pick up.

---

## Reminders carried forward

- S13 cTrader cull is on `main` (`4827ad4`), S13 build fix push happened in this session (untested at handoff time — the user runs `.\OMEGA.ps1 deploy` after this session ends).
- S14 Donchian breakout fail-safe was committed and pushed in this session — confirm by `git log -1` it's the HEAD before starting the audit.
- Three Tier-1 deferred items from S13 still open (see `HANDOFF_S14_POST_CTRADER_CULL.md` for the list): Phase C cosmetic rename, PDHL signature cleanup, PAT redaction across 9 docs.
- The 11h Asian Donchian H2 long that triggered S14 was the most recent loss the user flagged. Look for similar patterns across other engines.

Good luck.
