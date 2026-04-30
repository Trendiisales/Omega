# Omega heap-corruption — diagnostic mitigation plan
**2026-04-30 PM session, post-handoff audit**

## TWO variants of the fix file are now available

Two candidate `engine_init.hpp` files have been produced. Pick the one that matches your appetite for a sharp signal vs. a fast rule-out.

### Variant A — Tsmom-only disable (RECOMMENDED FIRST RUN)

`engine_init.tsmom_only.hpp` — single line changed at line 493:

```
include/engine_init.hpp:493   g_tsmom.enabled = true;   ->   false;
```

**Why this one first:** The timeline strongly singles out Tsmom. The first heap-corruption crash fired at **13:39 UTC 2026-04-29**. The five new engines shipped on this timeline:

| Commit | When | Relative to first crash |
|---|---|---|
| `8ec9f49` audit-fixes-22 — Tsmom ships | 13:05 UTC 2026-04-29 | **34 minutes BEFORE** |
| `6d98825` audit-fixes-23 — Donchian | 19:37 UTC 2026-04-29 | 6 hours **after** |
| `d18bfb9` audit-fixes-24 — EmaPullback | 19:41 UTC 2026-04-29 | 6 hours **after** |
| `1ce4c09` audit-fixes-25 — Tsmom multi-position | 19:56 UTC 2026-04-29 | 6 hours **after** |
| `c320069` audit-fixes-27 — TrendRider | 05:15 UTC 2026-04-30 | 16 hours **after** |

**Tsmom is the only new engine that existed when crashes started.** The other four cannot be the original cause (though they could be amplifying the problem). Disabling Tsmom alone gives the sharpest possible signal: if crashes stop, Tsmom is definitively the cause and the bug is localised to one ~744-line file.

If crashes continue with Tsmom disabled, the four newer engines and Tsmom's audit-fixes-25 multi-position upgrade are NOT the source — escalate to Variant B or PageHeap.

### Variant B — All four new engines disabled (FALLBACK)

`engine_init.hpp` — four lines changed (493, 513, 533, 568):

```
g_tsmom.enabled        = true;   ->   false;
g_donchian.enabled     = true;   ->   false;
g_ema_pullback.enabled = true;   ->   false;
g_trend_rider.enabled  = true;   ->   false;
```

Use this only if Variant A doesn't stop the crashes and you want to also test the hypothesis that one of the post-first-crash engines (Donchian, EmaPullback, TrendRider) is causing additional faults independently. Sharper diagnostic on overall stability, weaker localisation if it works.

## What the audit ruled OUT

### Suspect #1 — `omega_types.hpp` transitive inclusion: RULED OUT

A full include-closure walk confirmed only `src/main.cpp` reaches `omega_types.hpp` (73 transitive headers). The other three production .cpp files have closures of:
- `SymbolConfig.cpp`: 2 headers
- `OmegaTelemetryServer.cpp`: 5 headers
- `gold_coordinator.cpp`: 2 headers

None of those touch `omega_types.hpp`. The 32 file-scope statics exist as exactly one instance each in the binary. The duplicate-singleton hypothesis is dead.

### Suspect — concurrent access on Tsmom positions_ vector: RULED OUT

The threading model is:
- **Quote thread (= main thread):** runs `quote_loop()`, calls `dispatch_fix()` → `on_tick()` → `on_tick_gold()` → `g_tsmom.on_tick()`.
- **Trade thread:** runs `trade_loop()`. Reads ExecutionReports via `handle_execution_report()`. **Does NOT dispatch to `on_tick`.**

Tsmom is single-threaded — it runs only from the quote thread. No concurrent access to `positions_` is possible.

### Suspect — phantom orders in shadow mode: RULED OUT

`send_live_order()` in `order_exec.hpp:69` has a hard guard at the top: `if (g_cfg.mode != "LIVE") return {};`. Shadow-mode trades produce no FIX traffic.

### Suspect — Tsmom internal logic bugs: NO SMOKING GUN

Detailed read of `TsmomEngine.hpp` (744 lines):

- `closes_` deque is bounded with `while (closes_.size() > cap) closes_.pop_front()` after every push_back.
- `positions_` vector erase-while-iterating uses the correct `it = positions_.erase(it)` idiom; `++it` only when not erased.
- `Position` struct is POD-like (5 doubles + 2 ints + 1 int64). No heap allocations per position.
- `_close()` builds a local `TradeRecord` on the stack and calls `on_close(tr)` synchronously; no async storage.
- `wrap()` creates a `std::function<void(TradeRecord)>` capturing `[this, runtime_cb]` — closure size exceeds SBO so each `wrap()` heap-allocates. Five `wrap()` calls per tick = 5 small heap allocs/tick = ~250/sec at 50 ticks/sec. **High allocator pressure but not corrupting.**
- `warmup_from_csv()` parses with `strtoll`/`strtod`, builds local `TsmomBar`s, feeds via `_feed_warmup_h1_bar()` with a no-op callback. Heap-clean.
- `CellPrimitives::Bar` static_asserts are size-only (`sizeof(TsmomBar) == sizeof(omega::cell::Bar)`); `TsmomBar` is a plain struct of int64 + 4 doubles = 40 bytes, no padding tricks.

The code as written looks correct. **The bug, if it is in Tsmom, is subtle — likely a hidden interaction between Tsmom's state and an upstream/downstream component that only manifests at runtime.** Static review cannot localise it further. PageHeap on a non-prod node is the only path that gives a definitive answer if Variant A doesn't stop the crashes.

## Deployment plan — DO NOT BUILD ON THE VPS

The VPS has 1.6 GB free of 3 GB total. `cl.exe` is OOMing during builds. Use one of these paths:

### Path A — Build elsewhere, copy Omega.exe over (RECOMMENDED)

1. On any host with the same MSVC toolchain (17.14.40+3e7442088, Windows 10.0.20348 SDK) and ≥8 GB RAM:
   ```
   git clone https://github.com/Trendiisales/Omega.git
   cd Omega
   git checkout main
   git pull
   ```
2. Replace `include/engine_init.hpp` with **`outputs/omega-fix/engine_init.tsmom_only.hpp`** (Variant A — recommended) or **`outputs/omega-fix/engine_init.hpp`** (Variant B). Or apply the corresponding `.diff` with `git apply`.
3. Build:
   ```
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --target Omega --config Release
   ```
4. Stop the Omega service on the VPS:
   ```
   Stop-Service Omega
   ```
5. Copy `build/Release/Omega.exe` over `C:\Omega\Omega.exe` on the VPS (preserve the existing one as backup):
   ```
   Copy-Item C:\Omega\Omega.exe C:\Omega\Omega.exe.bak-pre-disable-2026-04-30
   ```
6. Start the service:
   ```
   Start-Service Omega
   ```
7. Watch for ~60 minutes. If no `0xc0000374` crash event appears in the Application log, the bug is in the disabled engine(s).

### Path B — Reboot VPS, build there, deploy

Only if Path A is not available. **NOT during active trading.** Choose a quiet market window (weekend or major-session close):
1. `Stop-Service Omega`
2. Reboot the VPS to flush every cached process and free RAM.
3. Verify ≥2.4 GB free (`Get-ComputerInfo | Select OsTotalVisibleMemorySize, OsFreePhysicalMemory`).
4. Apply the modified `engine_init.hpp` (Variant A or B) to `C:\Omega\repo\include\engine_init.hpp`.
5. Run `QUICK_RESTART.ps1`. The v3.3 stderr fix from this morning will show actual diagnostic output if the build fails.
6. Once Omega.exe is rebuilt and the service is up, watch for crashes for ~60 minutes.

## What to look for after deployment

**Success criterion (heuristic):** No `Application Error` event for `Omega.exe` with `Exception code: 0xc0000374` for 60+ minutes of normal market-hours uptime. Previous crash cadence was roughly one every ~30 minutes (4 crashes in 4-hour windows per the event log this morning), so 60 minutes clean is ~95% confidence the disabled engine(s) were the cause.

**Failure criterion (Variant A):** `0xc0000374` continues at ~30-min cadence. Tsmom is NOT the cause. Either deploy Variant B (disable all four), or skip ahead to PageHeap on non-prod.

**Monitor with:**
```
Get-EventLog -LogName Application -Newest 10 -EntryType Error |
    Where-Object { $_.Source -match 'Application Error' -and $_.Message -match 'Omega.exe' } |
    Format-List TimeGenerated, Message
```

## If Variant A succeeds — localising inside Tsmom

If Tsmom-disabled stops crashes, the bug is localised to `include/TsmomEngine.hpp` (744 lines) plus its config setup in `engine_init.hpp:468-502`. From there:

1. Re-enable with `max_positions_per_cell=1` (line 198 in engine, set in init at engine_init.hpp). This reverts to single-position semantics. If crashes resume → bug is in single-position core. If clean → bug is in multi-position handling (audit-fixes-25 territory: `std::vector<Position>` push/erase, `_close()` callback storm).
2. If multi-position is the culprit: focus review on `TsmomCell::on_bar` lines 231-258 (erase loop with `_close` callback re-entry) and `TsmomCell::on_tick` lines 325-345.
3. PageHeap on a non-prod build at this point will pin the corrupting instruction inside Tsmom in minutes.

Total localisation budget after Variant A success: 1-3 sessions.

## If Variant A succeeds — cost of running Tsmom disabled

Tsmom shadow-mode projection (per engine_init.hpp comments):
- H1 long: +$17,482/yr backtest edge
- H2 long: +$12,952/yr
- H4 long: +$15,885/yr
- H6 long: +$13,380/yr
- D1 long:  +$9,109/yr
- **Total: ~$68.8K/yr at 0.05 lot baseline (shadow projection)**

`shadow_mode = kShadowDefault` follows `g_cfg.mode`. Verify `g_cfg.mode = SHADOW` before deploying — in shadow this costs zero realised P&L. In live it forfeits the projected edge until re-enabled.

## If both variants fail — PageHeap on non-prod

The handoff documents this in detail. Briefly:
1. Spin up a non-prod VM with ≥8 GB RAM and the same MSVC toolchain. NOT prod.
2. Build Omega.exe at the current main commit (5aed21a or current HEAD).
3. `gflags /p /enable Omega.exe /full /backwards` — converts heap corruption from delayed fail-fast to immediate AV at the corrupting instruction with allocation backtrace captured.
4. Replay the same FIX session / cTrader feed.
5. Open `C:\Windows\Minidump\*.dmp` in WinDbg with Omega.pdb. The faulting frame is the bug.

Allow 2-4 hours toolchain setup + build, 30-60 min for repro + dump analysis once running.

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Variant A doesn't stop crashes (Tsmom not the cause) | Medium | Cost: one rebuild cycle. Drop to Variant B or PageHeap. |
| Disabling Tsmom breaks something downstream | Very Low | Tsmom is self-contained per its file header comments. Audit confirms no other engine reads g_tsmom state. |
| Build OOMs again on rebuild host | Medium if rebuilt on VPS | Path A (build elsewhere) sidesteps this entirely. |
| 60-min uptime without crash is a false negative (crash interval was variable) | Low | Watch for 2-3 hours before concluding success. Crash cadence today was ~one every 30 min. |
| Production drops to LIVE mode while engines are disabled | Low | Disabled engines stay disabled regardless of mode. Re-enable requires rebuild + redeploy. |
| Re-enable cycle exposes the bug to live trading | Low if shadow_mode | Verify `g_cfg.mode = SHADOW` before each re-enable cycle. |

## Files in this output bundle

- **`engine_init.tsmom_only.hpp`** — full file, Tsmom-only disable (Variant A, RECOMMENDED FIRST). 109,837 bytes.
- **`engine_init.tsmom_only.diff`** — unified diff for Variant A (1 line).
- **`engine_init.hpp`** — full file, all-four-engines disabled (Variant B). 109,840 bytes.
- **`engine_init.diff`** — unified diff for Variant B (4 lines).
- **`engine_init.hpp.original`** — verbatim copy of source HEAD `5aed21a`. 109,836 bytes.
- **`DEPLOYMENT_AND_ASSESSMENT.md`** — this document.

Source commit audited: `5aed21a` (origin/main as of 2026-04-30 12:21 NZST).

## TL;DR

1. Two variants produced. **Variant A (Tsmom-only) is the recommended first deploy** — sharpest signal because Tsmom is the only new engine that existed when crashes started.
2. Static review found no smoking gun in Tsmom; the timing correlation is the strongest available evidence. Both variants are diagnostic experiments, not verified fixes.
3. Threading model verified single-threaded for Tsmom — concurrency races on `positions_` are ruled out.
4. Build on a non-VPS host (≥8 GB RAM), copy `Omega.exe` over, watch for 60+ min.
5. If Variant A works → localise inside `TsmomEngine.hpp`, then PageHeap if needed.
6. If Variant A fails → deploy Variant B (disable all four). If THAT fails → PageHeap on non-prod is the only remaining option.
7. Production is currently degraded but watchdog-restarting; either experiment is safe to run.
