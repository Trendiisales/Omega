# SESSION HANDOFF — 2026-05-07 (S61 + S59 cohort) — to next session

## Headline

Two patch-sets landed and deployed in shadow this session:
- **S61** — `range_history` persistence (warm-restart for the S58 cold-start guard) on all 6 compression-breakout engines.
- **S59 cohort** — fill-spread reject pattern propagated from EUR to the other 5 cohort engines (USDJPY/GBP/AUD/NZD/GoldMidScalper).

Both validated parse-clean on macOS via `scripts/s61_parsecheck.cpp`, deployed to VPS via `OMEGA.ps1`, and confirmed running with matching source/head commits and exe SHA.

## What landed (committed on origin/main)

| Hash range | Description |
|---|---|
| 3b97d59 | (start of session) S59 EUR fill-spread reject + S60 OMEGA.ps1 branch guard |
| (this session) | S61 range_history persistence ×6 engines + S59 cohort propagation ×5 engines + scripts/s61_parsecheck.cpp |

Final hash on VPS: confirmed validated by user's deploy.

## Files modified this session

```
include/EurusdLondonOpenEngine.hpp   -- S61 persistence (S59 already present)
include/UsdjpyAsianOpenEngine.hpp    -- S61 persistence + S59 cohort
include/GbpusdLondonOpenEngine.hpp   -- S61 persistence + S59 cohort
include/AudusdSydneyOpenEngine.hpp   -- S61 persistence + S59 cohort
include/NzdusdAsianOpenEngine.hpp    -- S61 persistence + S59 cohort
include/GoldMidScalperEngine.hpp     -- S61 persistence + S59 cohort
scripts/s61_parsecheck.cpp           -- new: cross-engine syntax-only harness
```

Each engine got ~95 lines added: default ctor + 4 private helpers (`_range_hist_path`, `_try_load_range_history`, `_save_range_history`, `_save_range_history_if_due`) + 3 private members. Plus the S59 cohort engines got a `MAX_FILL_SPREAD = 2 × MAX_SPREAD` constant and the byte-identical `would_fill_long`/`would_fill_short` + `FILL_SPREAD_REJECT` block in PENDING phase.

Per-engine MAX_FILL_SPREAD values:
| Engine | MAX_SPREAD | MAX_FILL_SPREAD |
|---|---|---|
| EUR | 0.00020 (2 pip) | 0.00040 (4 pip) |
| USDJPY | 0.02 (2 pip) | 0.04 (4 pip) |
| GBP | 0.00025 (2.5 pip) | 0.00050 (5 pip) |
| AUD | 0.00020 (2 pip) | 0.00040 (4 pip) |
| NZD | 0.00025 (2.5 pip) | 0.00050 (5 pip) |
| Gold | 2.5 (XAU pts) | 5.0 (XAU pts) |

## S61 design (short form)

**Goal:** when the service restarts, don't make the engines wait ~25 minutes for the S58 cold-start guard to lift if there's recent on-disk history.

**Mechanism:**
- During ARMED phase, throttled save (every 30s) writes `m_range_history` to `C:\Omega\state\<engine>_range_history.csv` as a header line `# range_history v1 saved=<unix_ts>` + one double per line.
- On engine construction (static init, before `main()`), default ctor calls `_try_load_range_history()`. Reads the file; if file exists AND age <= 7200s (2h), pre-fills `m_range_history` with the saved values.
- The existing S58 `if (size < EXPANSION_MIN_HISTORY)` guard is unchanged. When reload succeeds, `size` is already >= 5, so the guard becomes inert immediately.
- All I/O guarded by `#ifndef OMEGA_BACKTEST` so backtests stay deterministic.

**Failure modes:** missing file, permission denied, stale file, corrupt CSV — all log a `RANGE_HIST_LOAD ...` line and proceed with cold-start (current behavior preserved).

## What to watch in shadow logs over the next 7 days

Look for these new log prefixes per engine (e.g. `[EUR-LDN-OPEN]`):
- `RANGE_HIST_LOAD ok n=<N> age_s=<S>` — reload succeeded; S58 guard inert this session
- `RANGE_HIST_LOAD no_file` — first run, no prior state (expected on first boot per engine)
- `RANGE_HIST_LOAD stale saved=<ts> age_s=<S>` — file too old (>2h), cold-start
- `FILL_SPREAD_REJECT spread=<...> max=<...> side=<...>` — S59 caught a spread spike at fill

The expected impact per the 10-day backtest: S59 cohort propagation prevents ~$300 of cumulative losses over a typical 10-day window (mostly GBP $233 + GoldMidScalper $67).

## Open follow-ups (priority order)

### 1. Shutdown-save follow-up (optional polish, ~12 lines across 7 files)
Currently relying on the 30s periodic save during ARMED. On hard kill, up to 30s of new range observations could be lost. To plug:
- Add a public `save_state()` method on each of the 6 engines that calls `_save_range_history()`.
- Add 6 `g_<engine>.save_state()` calls in `omega_main.hpp` shutdown sequence (mirrors the gold_stack/trend_pb pattern at lines 1105-1124).

Low risk, byte-identical pattern. Skip if shadow logs over the next week show no missed reload-on-restart cases.

### 2. Tsmom_H1_long backtest re-validation (offline, no live impact)
The `audit-fixes-39 MAE_EXIT_ATR=2.0` (deployed 2026-05-04) breaks parity with the validated `phase1/signal_discovery/post_cut_revalidate_all.py` backtest. Per the comment at `TsmomEngine.hpp:213-218`, the +$17,482 H1 long figure was produced WITHOUT this exit.

**Action:** layer `MAE_EXIT_ATR=2.0` into `phase1/signal_discovery/post_cut_revalidate_all.py::sim_c` and re-run to confirm the cell still has positive expected value with the audit-fixes-39 mitigation applied.

### 3. 2-week shadow watch (passive)
Both S58 cold-start guard and S59 cohort fill-spread reject need 2 weeks of clean shadow data before promoting any of the cohort engines from `shadow_mode=true` to live.

Watch: `RANGE_HIST_LOAD ok` count vs `COLD_START_BLOCK` count (S61 effective if `LOAD ok` dominates after restarts), `FILL_SPREAD_REJECT` count vs total trade count (should be small <5%, indicating it's only catching the actual spike outliers).

## Closed in this session (no follow-up needed)

### HybridBracketGold — engine deleted
Per `globals.hpp:441`: `S12 P3c (2026-05-07): GoldHybridBracketEngine.hpp file DELETED + #include removed at line 347 above. Engine fully retired.`

The handoff's reference to "S59 on HybridBracketGold (~$90/10d savings)" is moot — the engine no longer fires.

### IndexHybridBracketEngine family — engine family deleted
Per `globals.hpp:452-453`: `S12 P3c (2026-05-07): IndexHybridBracketEngine.hpp file DELETED + #include removed at line 347 above. Engine family fully retired.`

The handoff's note about "HybridBracketIndex 74 trades, -$134, 0 S59 rejects" reflects historical data from before the cull.

### Tsmom_H1_long — architectural mismatch with S58/S59
Investigated: the engine is a trend-momentum cell (signal: `ret_n = close[t] - close[t-20]` on H1 bar close, entry at next bar open at ask). It does not use compression `range_history` (uses `closes_` deque with a separate cold-start gate at `TsmomEngine.hpp:370`) and has no PENDING phase (direct market entry, not stop-order fills). The single dominant ~$236 loss in the handoff data is likely a pre-MAE_EXIT_ATR=2.0 trade. With audit-fixes-39 deployed 2026-05-04, future losses are capped at ~$140 per position at 0.05 lot.

**No code change needed** — let MAE_EXIT_ATR=2.0 prove itself in shadow over the 2-week watch window.

## Sandbox issue (UNRESOLVED — 7+ sessions now)

`mcp__workspace__bash` continues to throw `useradd: /etc/passwd: No space left on device` on the agent side. **Cowork restart strongly recommended before next session.** Without it, parse-checks must run Mac-side via:

```
g++ -std=c++17 -I include -DOMEGA_BACKTEST -fsyntax-only scripts/s61_parsecheck.cpp
```

That harness validates all 6 cohort engines together. Add new engines to it as cohort grows.

## Repo info

- Mac path: `~/omega_repo`
- VPS path: `C:\Omega`
- GitHub: https://github.com/Trendiisales/Omega
- Default branch: `main` (S60 OMEGA.ps1 branch guard hard-fails any non-main deploy)

## Critical reminders for next session

1. **Pull on VPS via PowerShell uses `;` not `&&`** — PowerShell 5.x doesn't accept `&&` as a statement separator. Correct form:
   ```powershell
   git checkout main; git pull
   .\OMEGA.ps1 deploy
   ```

2. **Don't `git pull` from a non-main branch.** S60 catches the deploy symptom but not the cause. Always `git checkout main` first.

3. **Mac working tree should be clean after this session's commit + push.** If `git status` shows uncommitted changes, run the suggested commit-push above before starting new work.

## Open audit started (see ENGINE_AUDIT_2026-05-07.md)

Per user request at session end, started a separate engine-audit doc enumerating ALL ~70 engine instances declared in `globals.hpp`, with current status / shadow-or-live / known issues / improvement candidates. That file is the next-session starting point for the broader review.

## Persistent shadow WARNs (by design, unchanged)

- VIX Level cold-start (clears on first VIX.F tick)
- RSI Reversal SHADOW (clears when going LIVE)
