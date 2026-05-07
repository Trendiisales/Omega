# HANDOFF — S13 cTrader cull (next session, complete in one shot)

Date prepared: **2026-05-08**
Prepared during: previous session (S12 trade-fixes + S13 CSV-writer fix)
Target: a fresh Claude session with full context budget
User intent: do NOT deploy until ALL of this is committed. Then ONE deploy.

---

## State of the world right now (read this first)

### Already LIVE on the VPS
- **S12** — `commit 2734f0e` — TsmomEngine MAE_EXIT cooldown + TrendPullback consec-loss tracking + relaxed hard TIME_STOP. Deployed via `OMEGA.ps1 deploy` on 2026-05-07 21:22 UTC. Service running, hash matches HEAD, bar state PASS.

### Pushed but NOT deployed
- (none — S13 CSV-writer fix is sitting in working tree, see below)

### In working tree — uncommitted
- `include/tick_gold.hpp` — XAUUSD L2 CSV writer rewrite (S13). cTrader-sourced columns (`raw_bid`, `raw_ask`, `depth_events_total`, `micro_edge`, `g_l2_watchdog_dead`) all zeroed. Schema kept at 17 cols so today's CSV can be appended. `l2_imb` stays live (FIX-fed via `g_macro_ctx.gold_l2_imbalance`).
- Push script ready at `/Users/jo/Library/Application Support/Claude/local-agent-mode-sessions/.../outputs/PUSH_S13_L2_CSV_FIX.sh` — DO NOT run separately. Roll it into the cull commit.

### Open verifier failures from last deploy
- `L2 Tick CSV XAUUSD` STALE 1473s — should self-fix once S13 deploys (writer fires unconditionally on every XAUUSD tick).
- `L2 Tick CSV US500` STALE 250s, `L2 Tick CSV USTEC` STALE 1474s — writers in `on_tick.hpp` already write zeros for cTrader fields; staleness is upstream FIX delivery, NOT a cTrader code issue. Investigate separately if it persists post-S13.

---

## Goal of this session

Complete the **cTrader Open API cull** (~2500 lines) plus the S13 CSV writer fix in a single commit, push, then user runs `.\OMEGA.ps1 deploy`. cTrader has been runtime-disabled since 2026-04-29 (`OmegaConfig::ctrader_depth_enabled = false`). Removal is functionally inert per `NEXT_SESSION.md` lines 67-97.

User preference reminders (from CLAUDE.md):
- "Always give full code with context and ensure correct syntax. No snippets, adds, paste, diffs, alway provide full file." — for the *files I rewrite end-to-end* (e.g., omega_runtime.hpp top, omega_config.ini), give full file. For surgical removals inside large files (omega_main.hpp), Edit tool is correct.
- "Never modify core code unless instructed clearly" — this IS the clear instruction. Proceed.
- "Warn me in advance when chats are at 70%, give summary."
- "warn me before I get a time management/session usage block by Claude"

---

## Inventory — every file to touch (keep this list checked off)

### A. Files to DELETE outright

```
include/CTraderDepthClient.hpp        (~1200 lines, the cTrader Open API client)
include/RealDomReceiver.hpp           (~330 lines, cBot DOM receiver on port 8765)
ctrader_cbot/OmegaDomStreamer.cs      (cBot side, C# code)
ctrader_cbot/TickScalper.cs           (verify if used — DELETE if pure cTrader)
DEPLOY_S12_FINDING_A.sh               (script for the cTrader self-recovery patch — obsolete)
```

`git rm` these in the push script. Do NOT delete them via Write tool (you can't `git rm` from inside a write).

### B. Files to EDIT

#### 1. `src/main.cpp`
- **Line 76**: remove `#include "CTraderDepthClient.hpp" // cTrader Open API v2 -- full order book depth feed`
- **Line 77**: remove `#include "RealDomReceiver.hpp"   // OmegaDomStreamer cBot receiver -- real XAUUSD DOM sizes on port 8765`

#### 2. `include/omega_main.hpp`
This is the biggest blast zone. Read it in chunks, do surgical Edits, verify after each.

| Lines | What to remove | Notes |
|---|---|---|
| **407-412** | `g_real_dom_receiver.start();` block + `[STARTUP] RealDomReceiver started` printf | Whole 6-line block goes |
| **420-644** | Entire `if (g_cfg.ctrader_depth_enabled && ...) { ... } else if ... else ...` cTrader init block | ~225 lines. Replace with: `// cTrader Open API removed S13 2026-05-08 -- FIX 264=0 provides L2` |
| **708-732** | Check 2 cTrader credentials block | The whole `if (g_cfg.ctrader_depth_enabled) { ... } else { ... }` |
| **759-792** | Check 5 (cTrader depth client connected) AND Check 6 (XAUUSD depth events flowing) | Both blocks — they call `g_ctrader_depth.depth_active.load()` and `g_ctrader_depth.depth_events_total.load()` |
| **794-817** | Check 7 reads `g_ctrader_depth.depth_events_total.load()` and `g_l2_gold.raw_bid/raw_ask.load()` | Replace with FIX-fed equivalent or delete the check (verifier already handles L2 staleness via CSV check) |
| **836-839** | Final-status block reads `g_macro_ctx.gold_l2_real` (still valid — keep, this is FIX-fed since S8) | KEEP |
| **842-987** | Entire L2 WATCHDOG THREAD (`std::thread([](){...}).detach()`) | ~145 lines. Reads `g_ctrader_depth.depth_events_total` and writes `g_l2_watchdog_dead`. Drop the whole thread. The 10-min periodic bar-save block (lines 899-920) is INSIDE this thread — extract it into its own `std::thread` so periodic saves keep working. |
| **988-1029** | `if (g_cfg.ctrader_depth_enabled) { ... wait for cTrader before FIX ... }` block | Drop entirely; FIX always starts immediately |
| **~1037** | `g_ctrader_depth.stop()` shutdown call | Remove |

After all edits: grep `omega_main.hpp` for `ctrader|CTrader|g_ct_ms_|raw_bid|raw_ask|g_l2_watchdog_dead|g_real_dom_receiver|RealDomReceiver` — must return zero hits.

#### 3. `include/omega_runtime.hpp`
- **Lines 5-17**: remove `set_ctrader_tick_ms()` and `ctrader_depth_is_live()` functions entirely.
- After deletion the file should start `#pragma once\n// omega_runtime.hpp -- extracted from main.cpp\n// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp\n\n// RTT\nstatic double g_rtt_last = ...`
- Per user pref, write the full file (it's ~815 lines so big but doable since the head is small and the rest is unchanged).

#### 4. `include/omega_types.hpp`
- **Lines 278-287**: remove the cTrader config fields block (`ctrader_access_token`, `ctrader_refresh_token`, `ctrader_ctid_account_id`, `ctrader_depth_enabled`)
- **Lines 296-297**: remove the `// ?? cTrader Open API depth client ...` comment + `static CTraderDepthClient g_ctrader_depth;` line

#### 5. `include/globals.hpp`
- **Line 546**: `static std::atomic<bool> g_l2_watchdog_dead{false};` — DELETE (only the L2 watchdog thread + tick_gold writer read it; both go in this cull)
- **Lines 558-562**: comments referencing CTraderDepthClient — clean up
- **Lines 727-738** in the `AtomicL2` struct: drop `raw_bid`, `raw_ask`, `micro_edge` fields. Keep `imbalance`, `microprice_bias`, `has_data`, `last_update_ms` (FIX-fed since S8).
- **Lines 788-808**: drop `g_ct_ms_*` atomics block AND `get_ctrader_tick_ms_ptr()` function — both unreferenced after omega_main + omega_runtime cleanup.

#### 6. `include/quote_loop.hpp`
- **Lines 286-394**: diagnostic loop reads `g_ctrader_depth.depth_events_total`, `depth_active`. Find the block, remove diagnostic prints that reference cTrader. Keep all FIX-related diagnostics.

#### 7. `include/on_tick.hpp`
- **Lines 440-450** (approx): `[L2-STATUS]` print line that reads `ctrader_l2_live` AND `g_ctrader_depth.depth_events_total` — remove or rewrite to print FIX-only L2 status.
- **Lines 580-601** (approx): `g_telemetry.UpdateL2(...)` lambda reads `depth_active` and `last_depth_event_ms` — rewrite to source from FIX `last_update_ms`.
- **Lines 466-478** (approx): comment about cTrader incremental updates — cosmetic cleanup.
- The SP/USTEC/NAS100 CSV writers at ~1820+ are ALREADY cTrader-clean (write zeros for cTrader fields). Don't touch.

#### 8. `include/tick_gold.hpp`
- **The CSV writer at lines 1008-1042 is ALREADY DONE in this session's working tree** (S13). Verify the edit is intact via grep.
- **Lines 2260-2261**: `g_pdhl_rev.on_tick(...)` passes `g_l2_gold.raw_bid.load(...)` and `g_l2_gold.raw_ask.load(...)` as parameters. Once `raw_bid/raw_ask` fields are removed from `AtomicL2`, this will break the build. Two options:
  - (a) Replace the two args with `0, 0` literals
  - (b) Update the `g_pdhl_rev.on_tick` signature to drop those args entirely (cleaner — find the engine def in `PDHLReversionEngine.hpp` and remove the params)
  Recommend (a) for S13; defer (b) to a later cleanup pass.
- Comments at 336, 970 referencing `CTraderDepthClient` — cosmetic cleanup.

#### 9. `include/OmegaFIX.hpp`
- **Line 130**: comment `// Stateful signals (need previous snapshot -- in CTDepthBook, CTraderDepthClient.hpp):` — cosmetic cleanup.

#### 10. `include/OHLCBarEngine.hpp`
- **Line 5**: header comment `// Uses the SAME cTrader SSL connection as CTraderDepthClient.` — rewrite. NOTE: this engine may have actual cTrader code dependencies (it's the bar engine); read carefully before assuming it's cosmetic. If it does, it's a bigger cull and may need to defer to a later session.

#### 11. `omega_config.ini`
- Remove the entire `[ctrader_api]` section.

#### 12. `OMEGA.ps1` (deploy script)
- Search for `CTRADER`, `cTrader`, `ctrader` — remove pre-flight cTrader checks. Keep everything else. The deploy script's [11/12] config check probably greps for the `[ctrader_api]` section; remove that grep.

#### 13. `VERIFY_STARTUP.ps1`
- Search for `CTRADER-EVTS`, `CTRADER-L2-CHECK`, `ctid=43014358`, `cTrader Feed` — remove those Add-Result blocks.
- Lines 652-653, 702-735ish handle the L2 tick CSV staleness checks — KEEP these (they're useful), but remove any cTrader-specific diagnosis text.

#### 14. `omega-terminal/src/api/types.ts`
- Search for `cTrader`, `ctrader`, `CTrader` — remove those types/fields. Likely just one or two interface fields like `ctraderActive` or similar. Update any consumers in the same file.

### C. Files to INSPECT but probably leave

| File | Reason |
|---|---|
| `backtest/l2_edge_discovery.cpp` | Backtest tool, references cTrader DOM data format. KEEP — historical analysis still useful. |
| `phase2/C1_RETUNED_SHADOW.md` | Doc reference, not code. KEEP. |
| `docs/SESSION_*.md`, `NEXT_SESSION_*.md`, `S12_INVESTIGATIONS.md` | Historical session notes. KEEP. |
| `scripts/omega_session_diagnostic.py` | Diagnostic tool that may grep for cTrader log lines. INSPECT — remove cTrader-specific greps if cheap. |
| `MONITOR.ps1`, `OMEGA_STATUS.ps1` | Same as above — inspect for cTrader log greps, remove if cheap. |

---

## Order of operations (do it in this order — minimises broken intermediate states)

1. **Read first**: `omega_main.hpp` lines 1-50 + 1020-1060 (the bits I didn't fully see) so you understand surrounding context.
2. **Cleanup the small files first** (low risk, sets up dependency removal):
   - `omega_runtime.hpp` (full file write — drop the top 13 lines)
   - `omega_types.hpp` (Edit — drop the 2 blocks)
   - `omega_config.ini` (Edit — drop `[ctrader_api]` section)
3. **Engine-side reference cleanup** (so `g_ctrader_depth` and friends are no longer referenced):
   - `globals.hpp` (Edit — drop watchdog atomic, AtomicL2 fields, g_ct_ms_* + helper)
   - `on_tick.hpp` (Edit — L2-STATUS, GUI lambda)
   - `quote_loop.hpp` (Edit — diagnostic loop)
   - `tick_gold.hpp` line 2260-2261 (Edit — replace with literal 0, 0)
   - `OmegaFIX.hpp`, `OHLCBarEngine.hpp` (Edit — comment cleanup; verify OHLCBarEngine isn't cTrader-coupled)
4. **The big one**: `omega_main.hpp`
   - Do the 8 sections in order from the table above. After EACH section, grep the file to confirm the symbols are gone.
   - The L2 watchdog thread (842-987) contains the periodic bar-save block (899-920) — extract it into its own thread BEFORE deleting the watchdog. Otherwise periodic bar saves stop firing.
5. **Last C++ edit**: `src/main.cpp` (Edit — drop the 2 includes). At this point nothing in the codebase should reference `CTraderDepthClient` or `RealDomReceiver`.
6. **Verify with grep across full repo**: `grep -ri "g_ctrader_depth\|CTraderDepthClient\|RealDomReceiver\|g_real_dom_receiver\|g_ct_ms_\|get_ctrader_tick_ms\|ctrader_depth_is_live\|set_ctrader_tick_ms" include/ src/` — must return zero hits OUTSIDE comments and docs.
7. **PowerShell + UI**:
   - `OMEGA.ps1`, `VERIFY_STARTUP.ps1` (Edit)
   - `omega-terminal/src/api/types.ts` (Edit — re-build the UI on next deploy will catch any broken consumers)
8. **Write the push script** (template below). The script does the `git rm` for the deleted files PLUS commits all edits PLUS pushes via PAT.
9. **Stop**. Do NOT run the script — user runs it themselves.

---

## Push script template (drop into outputs)

```bash
#!/usr/bin/env bash
# S13 -- complete cTrader Open API cull
#   - S13 CSV writer fix (tick_gold.hpp -- already in working tree from prior session)
#   - Full cull of CTraderDepthClient + RealDomReceiver + cBot
set -euo pipefail
cd ~/omega_repo

# Pre-flight syntax check if g++ available
if command -v g++ >/dev/null 2>&1; then
    g++ -std=c++17 -fsyntax-only -Iinclude -x c++ src/main.cpp 2>&1 | head -60 || true
fi

git remote set-url origin \
  "https://Trendiisales:${GITHUB_PAT}@github.com/Trendiisales/Omega.git"

# Delete files (git tracks the removal)
git rm include/CTraderDepthClient.hpp \
       include/RealDomReceiver.hpp \
       ctrader_cbot/OmegaDomStreamer.cs \
       ctrader_cbot/TickScalper.cs \
       DEPLOY_S12_FINDING_A.sh

# Stage all edits
git add -A

git status --short

git commit -m "S13: cull cTrader Open API surface; FIX-only L2 going forward

cTrader Open API was runtime-disabled 2026-04-29 (ctrader_depth_enabled=false
default) once FIX 264=0 began delivering full L2 reliably. This commit removes
the dead code path entirely.

Files deleted (~1500 lines):
  include/CTraderDepthClient.hpp       cTrader Open API v2 client
  include/RealDomReceiver.hpp          OmegaDomStreamer cBot receiver (port 8765)
  ctrader_cbot/OmegaDomStreamer.cs     cBot side (C#)
  ctrader_cbot/TickScalper.cs          cBot side (C#)
  DEPLOY_S12_FINDING_A.sh              cTrader self-recovery patch -- obsolete

Files edited (~1000 lines stripped):
  src/main.cpp                         drop 2 cTrader includes
  include/omega_main.hpp               drop init block, watchdog thread, startup
                                       checks, FIX-launch wait, shutdown call
  include/omega_runtime.hpp            drop set_ctrader_tick_ms / ctrader_depth_is_live
  include/omega_types.hpp              drop ctrader_* config fields, g_ctrader_depth
  include/globals.hpp                  drop g_l2_watchdog_dead, g_ct_ms_* atomics,
                                       AtomicL2.{raw_bid,raw_ask,micro_edge},
                                       get_ctrader_tick_ms_ptr helper
  include/quote_loop.hpp               drop cTrader diagnostic prints
  include/on_tick.hpp                  rewrite L2-STATUS + GUI L2 lambda for FIX-only
  include/tick_gold.hpp                already done S13 + replace pdhl on_tick raw_*
                                       args with literal 0, 0
  include/OmegaFIX.hpp                 comment cleanup
  include/OHLCBarEngine.hpp            comment cleanup
  omega_config.ini                     drop [ctrader_api] section
  OMEGA.ps1                            drop cTrader pre-flight checks
  VERIFY_STARTUP.ps1                   drop CTRADER-EVTS / CTRADER-L2-CHECK / ctid checks
  omega-terminal/src/api/types.ts      drop cTrader UI types

Periodic bar-save block (was inside the L2 watchdog thread) extracted into its
own dedicated thread so saves keep firing on the 10-min cadence.

Functionally inert at runtime: cTrader has been disabled by config since
2026-04-29 so no live behavior changes. l2_imb continues to be FIX-fed via
fix_dispatch.hpp (S8 fix). Hydrator-required CSV columns (ts_ms, mid, bid, ask)
preserved.

Bundled fix from prior session:
  XAUUSD L2 tick CSV writer cTrader columns zeroed (was already in working
  tree from S13 prep)."

git push origin HEAD

echo
echo "[done] S13 cTrader cull pushed."
echo "Next on Windows:  .\\OMEGA.ps1 deploy"
```

---

## Validation after deploy

When the user runs `.\OMEGA.ps1 deploy`, expected outcomes:

1. **Build succeeds**. If it fails, the most likely culprits are:
   - Forgotten reference to `g_ctrader_depth.X` somewhere (grep again)
   - `g_pdhl_rev.on_tick` signature mismatch — confirm step B.8 done
   - PowerShell config-check at deploy step 11 still greps for `[ctrader_api]`
2. **Service starts cleanly**. Verifier should show:
   - PASS Hash vs HEAD
   - PASS Bar State / Bar State Load
   - **No** `[CTRADER-AUDIT]` or `[L2-WATCHDOG]` log lines (those threads are gone)
   - L2 Tick CSV XAUUSD goes from STALE → recent within seconds (S13 CSV-writer fix delivers)
3. **L2 tick CSV** for XAUUSD shows valid rows with `l2_imb` populated (non-0.5) and all cTrader columns as `0` / `0.0`. Header unchanged (17 cols).
4. **L2_ALERT.txt** at `C:\Omega\logs\L2_ALERT.txt` will no longer be touched (no more watchdog). Old alert file may persist — that's cosmetic.

---

## What this handoff intentionally does NOT do

- **Phase C cosmetic rename** (`g_macro_ctx.ctrader_l2_live` → `g_macro_ctx.l2_live` across ~15 files). NEXT_SESSION.md item 10. Skip — pure cosmetic, defer.
- **CSV column reduction**. NEXT_SESSION.md item 9 says hydrator only needs `ts_ms, mid, bid, ask` so we *could* drop columns. We're keeping the 17-col schema for backward-compatible appends to today's CSV. Reduce in a later session if desired (will require purging old CSVs first).
- **`tick_gold.hpp:2260-2261` engine signature cleanup**. We're patching with literal `0, 0`. The cleaner fix is to drop the two args from `g_pdhl_rev.on_tick` signature in `PDHLReversionEngine.hpp` and adjust the engine internals. Defer.

---

## Chat budget guidance for the new session

- Reading 12 files + editing them + writing the push script will consume roughly 80-100k of context.
- Use **parallel reads** aggressively (e.g., read globals.hpp + omega_runtime.hpp + omega_types.hpp + omega_config.ini in one batch).
- Avoid full-file reads of `omega_main.hpp` (it's ~1100 lines) — read by section using `offset`/`limit`.
- After each major edit, do a **scoped grep** to verify the symbol is gone — far cheaper than re-reading the file.
- Warn the user proactively when chat hits 70% per their preference.

---

## Files that CONFIRM the existing plan

- `NEXT_SESSION.md` lines 67-110 — the original S9 cull plan, this handoff is the implementation guide for items 7-10.
- `S12_INVESTIGATIONS.md` — earlier investigation that recommended adding a self-recovery loop instead of culling. Superseded by this cull (recovery is moot once cTrader is gone).

---

## Final note for the new session

The user is operating Omega in production (live shadow mode). They want **zero surprise behavior changes**. This cull is a code-hygiene removal, not a feature change. Every removed line is either dead at runtime (cTrader disabled) or replaceable by an FIX-equivalent that already works. If you find a removal that DOES change runtime behavior, STOP and ask before proceeding.

Good luck.
