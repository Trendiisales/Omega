# HANDOFF — S25 Universal Hedging-Close Fix

Date prepared: **2026-05-11**
Prepared during: S22 watchdog reinstall + audit session (Mon 2026-05-11 09:00 NZ)
Target: a fresh Claude session with full context budget
Operator: Jo (kiwi18@gmail.com), BlackBull live account **8077780**
User intent: **Fix every single engine's close path so any engine can be safely flipped to LIVE without risking another orphan-pair incident.** Do NOT defer. Do NOT half-fix. This is the proper architectural patch the codebase has been waiting for since the S21 microscalper-only hot-fix on 2026-05-09.

User preference reminders (from CLAUDE.md):
- "Always give full code with context and ensure correct syntax. No snippets, adds, paste, diffs, alway provide full file." — when rewriting a file end-to-end, dump the full file.
- "Never modify core code unless instructed clearly" — **THIS HANDOFF IS THE CLEAR INSTRUCTION. Proceed.**
- "Warn me in advance when chats are at 70%, give summary."
- "warn me before I get a time management/session usage block by Claude"

---

## 1. State of the world (read this first)

### 1.1 What's deployed right now

- **Binary**: `Omega.exe`, git `18b62c8`, built `2026-05-09 05:48:49 UTC`
- **Mode**: LIVE (per `omega_config.ini` line 68)
- **LIVE engines**: `MicroScalperGold` ONLY (hardcoded `g_gold_microscalper.shadow_mode = false;` at `engine_init.hpp:146`)
- **SHADOW engines**: every other engine in `omega_config.ini` (~30 engine sections, all with `shadow_mode=true`)
- **`max_lot_gold=0.01`** — operator-reduced from 0.30 on 2026-05-09 explicitly to cap orphan-pair bleed at sub-dollar levels until the hedging path is properly debugged. **DO NOT raise this until S25 is complete and verified on demo.**
- **Broker account mode**: HEDGING (confirmed by cTrader UI showing 14 separate position rows from Friday's incident, not a single netted aggregate)

### 1.2 Watchdog status (S22 work, completed this session)

- `Omega` NSSM service: Running, Automatic start
- `OmegaWatchdog` NSSM service: Running, Automatic start, monitoring Omega + log staleness + L2 CSV + GitHub HEAD
- Watchdog code is now at commit `65d91b4` on `main`:
  - `Get-L2CsvPath` fixed to point at `l2_ticks_XAUUSD_YYYY-MM-DD.csv` (post-S13 filename schema)
  - `Get-RunningHash` fixed to read `omega_build.stamp` first (canonical), log tail fallback
- **Known harmless noise**: `L2-CSV-MISSING` alerts every 15s while XAUUSD is closed (no ticks → no CSV writes). Will self-clear when gold market opens. A follow-up patch should gate this alert on `telemetry.gold_l2_real==1` so it only fires when L2 feed is live but CSV isn't writing — that's a real bug condition.

### 1.3 Friday's incident — the bug we're fixing

- **When**: 2026-05-08 12:33:05 to 12:34:32 UTC (90 seconds)
- **What**: 14 XAUUSD positions opened on live account 8077780. 7 Buy + 7 Sell, alternating, 0.01 lots each, no SL/TP set on any. Net P&L stayed near zero ($12.63 unrealised as of Mon morning).
- **Why**: MicroScalperGold's close-side FIX message included tags **77=C + 1006=<posId> + 721=<posId>**. BlackBull's Spotware gateway:
  - Initially rejected tag 77 at session level (35=3, "Tag not defined")
  - With 77 removed, ambiguity between 1006 and 721 caused the gateway to silently fall back to "new order" semantics
  - In hedging mode, that opened a new opposing position instead of closing the named one
- **Fix**: commit `12902f5` "hedging-mode close: tag 721 only per Spotware spec" — emits **only tag 721 (FIX 4.4 PosMaintRptID)** on close. Deployed in `18b62c8`.

### 1.4 Why we're not done

The S21 fix in `12902f5` is correct AND incomplete. The `build_new_order_single` helper now produces a hedging-safe close **only when the caller passes `position_id`**. Audit of every `send_live_order(...)` call site:

| Call site | Engine / Purpose | Passes position_id? |
|---|---|---|
| `trade_lifecycle.hpp:1290` (microscalper_on_close) | MicroScalperGold | ✅ YES |
| `on_tick.hpp:992` | GoldStack force_close (dollar-stop) | ❌ |
| `on_tick.hpp:1759` | PartialExit (TP1/TP2 close) | ❌ |
| `tick_gold.hpp:1745` | RSIReversal close | ❌ |
| `tick_gold.hpp:1769` | RSIExtreme close (manage path) | ❌ |
| `tick_gold.hpp:1797` | RSIExtreme close (post-entry HTF block) | ❌ |
| `tick_gold.hpp:1845` | (verify which engine) | ❌ |
| `tick_gold.hpp:2509` | CandleFlow close | ❌ |
| `trade_lifecycle.hpp:1149` | Generic close handler (TsMom etc.) | ❌ |
| `trade_lifecycle.hpp:1224` | Generic close handler (variant 2) | ❌ |
| `quote_loop.hpp:740` | Bracket engine close (gold/sp/nq/us30/nas/ger/uk/estx/brent/eur/gbp) | ❌ |
| `quote_loop.hpp:859`  | (same — bracket cohort, different exit path) | ❌ |
| `quote_loop.hpp:959`  | (same) | ❌ |
| `quote_loop.hpp:984`  | (same) | ❌ |
| `quote_loop.hpp:1009` | (same) | ❌ |
| `quote_loop.hpp:1037` | (same) | ❌ |
| `quote_loop.hpp:1102` | (same) | ❌ |

**Total**: 36 close call sites; 1 protected; 35 vulnerable. Today these are safe ONLY because the calling engines are in SHADOW. The moment the operator flips ANY of them to `shadow_mode=false`, Friday's pattern repeats on that engine.

Per the operator's explicit instruction on 2026-05-11: **fix every single one**.

---

## 2. Architectural plan (this is the contract for the session)

Build a **universal hedging registry + close wrapper** so close-side correctness is enforced centrally and any new engine added later inherits the protection automatically.

### 2.1 Phase A — Central registry

**New file**: `include/HedgingRegistry.hpp`

A thread-safe global service that maps `entry_clOrdId → broker_position_id` for every live entry. Populated by the FIX execution-report handler, consumed by the close wrapper.

Required public surface (sketch — design freely, but preserve these semantics):

```cpp
namespace omega {
    class HedgingRegistry {
    public:
        // Called from handle_execution_report on entry-side fills.
        // Idempotent (overwriting an existing entry is allowed; the most
        // recent ACK wins).
        void recordEntry(const std::string& entry_clOrdId,
                         const std::string& broker_position_id);

        // Called from send_live_close. Returns empty string if not found.
        std::string lookup(const std::string& entry_clOrdId) const;

        // Called after a successful close-side ExecReport so the entry's
        // mapping doesn't leak forever. Optional but encouraged.
        void clearEntry(const std::string& entry_clOrdId);

        // Diagnostic snapshot for /api/telemetry — exposes the live size
        // so the GUI can show "registered positions: N".
        size_t size() const;

    private:
        mutable std::mutex mtx_;
        std::unordered_map<std::string, std::string> map_;
    };
    extern HedgingRegistry g_hedging_registry;
}
```

**Definition site**: `include/HedgingRegistry.hpp` (header-only is fine; the codebase is single-TU).

### 2.2 Phase B — TradeRecord plumbing

Add `entry_clOrdId` to `omega::TradeRecord` in `include/omega_types.hpp`. Every engine that opens a position MUST stamp it onto the TradeRecord at entry time so the close callback can read it back.

```cpp
struct TradeRecord {
    ...existing fields...
    std::string entry_clOrdId;   // NEW: set when the engine sends the entry order.
                                 // Empty for shadow-only engines (never used for closes).
};
```

Every engine that calls `send_live_order(...)` for an ENTRY must:
1. Capture the return value (`std::string entry_clOrdId = send_live_order(...)`).
2. Store it on the engine's position state.
3. Copy it into the TradeRecord when the trade is constructed (entry-time `_open` or close-time `_close`, whichever is the source of truth — most engines build the TradeRecord at close time but already have the entry_clOrdId stashed on `pos.entry_clOrdId`).

Microscalper already does (1) and (2) at `tick_gold.hpp:2365` (`g_gold_microscalper.pos.entry_clOrdId = entry_clOrdId;`). Replicate this pattern across every engine.

### 2.3 Phase C — Close wrapper

**Add to `include/order_exec.hpp`**:

```cpp
// Hedging-mode-safe close. Looks up the broker position_id from the
// HedgingRegistry by entry_clOrdId and sends a tag-721 close via
// send_live_order. REFUSES TO SEND if the mapping isn't known yet —
// in that case the engine self-shadows (caller is responsible for
// flipping shadow_mode on failure return).
//
// Returns the close-side clOrdId on success, empty string on refusal.
//
// CRITICAL: every engine close path MUST call THIS, not send_live_order
// directly. Direct send_live_order calls for close-side are a hedging
// safety bug. See HANDOFF_S25.
inline std::string send_live_close(const std::string& symbol,
                                   bool close_is_long,
                                   double qty,
                                   double mid_price,
                                   const std::string& entry_clOrdId) {
    if (g_cfg.mode != "LIVE") return {};  // shadow-mode short-circuit

    if (entry_clOrdId.empty()) {
        std::cerr << "\033[1;31m[HEDGING-CLOSE-NO-CLORDID] "
                  << "REFUSING close on " << symbol
                  << " -- caller did not supply entry_clOrdId. "
                  << "Engine must self-shadow.\033[0m\n";
        std::cerr.flush();
        return {};
    }

    const std::string position_id =
        omega::g_hedging_registry.lookup(entry_clOrdId);
    if (position_id.empty()) {
        std::cerr << "\033[1;31m[HEDGING-CLOSE-NO-POSID] "
                  << "REFUSING close on " << symbol
                  << " entry_clOrdId=" << entry_clOrdId
                  << " -- broker position ID not in registry. "
                  << "Entry ACK race OR broker didn't return tag 721. "
                  << "ORPHAN risk. Engine must self-shadow.\033[0m\n";
        std::cerr.flush();
        return {};
    }

    return send_live_order(symbol, close_is_long, qty, mid_price, position_id);
}
```

**Mark `send_live_order` as the entry-only path going forward.** Add a `[[nodiscard]]` and a doc comment explicitly forbidding close-side use. (Cannot use `[[deprecated]]` because microscalper's internal close path goes through `send_live_order` with explicit position_id — it's the layer below `send_live_close`. Compromise: rename `send_live_order` to `send_live_entry` for entry calls, keep the underlying impl as a private helper that both wrappers call. Decide during implementation.)

### 2.4 Phase D — Populate the registry from FIX

In `include/order_exec.hpp::handle_execution_report`, when an entry-side fill (ordStatus 1 or 2, ClOrdID matches a recently-sent ENTRY in `g_live_orders`) arrives:

```cpp
// Inside the existing "fill" branch:
if (!positionId.empty() && !clOrdId.empty()) {
    omega::g_hedging_registry.recordEntry(clOrdId, positionId);
}
```

This replaces (and generalises) the current microscalper-specific block at `order_exec.hpp:557-562`. The microscalper-specific code can stay as a redundant secondary capture, or be removed — designer's choice. Recommend keeping it for safety during the transition (belt + braces).

When a close-side fill (ordStatus 2 with positionId matching an entry) arrives, call `omega::g_hedging_registry.clearEntry(entry_clOrdId)` to release the mapping. Match by `g_omegaLedger.lookupEntryByClose(close_clOrdId)` if the ledger supports it — else by scanning live_orders.

### 2.5 Phase E — Update every close call site

For each of the 35 unprotected close sites in section 1.4, change from:

```cpp
[&](const omega::TradeRecord& tr) {
    handle_closed_trade(tr);
    if (!engine.shadow_mode)
        send_live_order(sym, tr.side == "SHORT", tr.size, tr.exitPrice);
}
```

to:

```cpp
[&](const omega::TradeRecord& tr) {
    handle_closed_trade(tr);
    if (!engine.shadow_mode) {
        const std::string close_clOrdId = send_live_close(
            sym, tr.side == "SHORT", tr.size, tr.exitPrice, tr.entry_clOrdId);
        if (close_clOrdId.empty()) {
            engine.shadow_mode = true;
            std::cerr << "[HEDGING-AUTO-SHADOW] " << #engine
                      << " forced to shadow_mode after close refusal\n";
        } else {
            g_omegaLedger.stampCloseClOrdId(tr.id, close_clOrdId);
        }
    }
}
```

Apply this pattern to every site in section 1.4's table. The bracket-engine sites in `quote_loop.hpp` may need extra care because they often close pairs (long+short legs) — both legs need the wrapper.

### 2.6 Phase F — Build-time guard

Add a check in `CMakeLists.txt` (or a small `tools/check_hedging_safety.py`) that greps the codebase post-build and FAILS the build if any `send_live_order(` call matches a close pattern (heuristic: same expression appears in a callback that takes `TradeRecord` AND uses `tr.side`). Use `add_custom_target(check_hedging_safety ALL ...)` so the check runs every build.

This makes future regressions structurally impossible.

---

## 3. File-by-file change list

Touch the following files (in this order — earliest files have no dependencies on later ones):

1. **NEW** `include/HedgingRegistry.hpp` — full file, see §2.1 sketch.
2. **EDIT** `include/omega_types.hpp` — add `std::string entry_clOrdId;` to `TradeRecord`. Verify all TradeRecord constructors/initializers handle the new field (default empty).
3. **EDIT** `include/order_exec.hpp` — add `send_live_close()` wrapper (§2.3); update `handle_execution_report` to populate the registry (§2.4); generalise the existing microscalper-specific capture.
4. **EDIT** `include/main.cpp` — instantiate `omega::g_hedging_registry`. Single line.
5. **EDIT** every engine header that opens positions and constructs TradeRecord, to stamp `tr.entry_clOrdId` from the engine's already-stored `pos.entry_clOrdId`. Engines to touch (verify the full list during implementation):
   - `include/GoldStack*.hpp`
   - `include/RSIExtremeTurnEngine.hpp` (and the dispatcher in `tick_gold.hpp` if the engine builds TradeRecord externally)
   - `include/RSIReversalEngine.hpp`
   - `include/CandleFlowEngine.hpp` (CFE)
   - `include/EMACrossEngine.hpp`
   - `include/XauusdFvgEngine.hpp`
   - `include/NbmGoldLondon.hpp` (NbmEngine variant)
   - `include/H1SwingGoldEngine.hpp`, `include/H4RegimeGoldEngine.hpp`
   - `include/BracketEngine.hpp` (covers gold/sp/nq/us30/nas/ger/uk/estx/brent/eur/gbp via instantiations)
   - `include/TsmomPortfolio.hpp` (5 TFs)
   - `include/DonchianPortfolio.hpp` (8 variants)
   - `include/EmaPullbackPortfolio.hpp` (5 variants)
   - `include/TrendRiderPortfolio.hpp` (6 variants)
   - `include/TrendPullbackGold.hpp`
   - Cross-asset engines (EurusdLondonOpen, UsdjpyAsianOpen, GbpusdLondonOpen, AudusdSydneyOpen, NzdusdAsianOpen)
   - `include/MinimalH4Engine.hpp` (and the US30 variant)
   - PartialExit logic in `include/partial_exit.hpp` (or wherever `g_partial_exit` lives)
6. **EDIT** every close call site listed in §1.4 — swap `send_live_order` for `send_live_close` per the template in §2.5.
7. **EDIT** `include/trade_lifecycle.hpp` — both close handlers at lines 1149 and 1224 use the new wrapper. The microscalper handler at line 1280 can stay as-is OR be migrated to `send_live_close` for uniformity (recommend migrate — single source of truth).
8. **EDIT** `include/quote_loop.hpp` — bracket-engine close paths (7 sites).
9. **EDIT** `CMakeLists.txt` — add the build-time grep check (§2.6).
10. **EDIT** `omega_config.ini` — add a comment block documenting the new contract: "Any engine with shadow_mode=false MUST have its close path use send_live_close. Build-time check enforces this."

### Files NOT to touch (already correct)
- `include/fix_builders.hpp` — already emits tag 721 correctly.
- `OMEGA.ps1` — watchdog work done this session; do not modify.
- `INSTALL_OMEGA.ps1` — already correct.

---

## 4. Verification plan (DO NOT skip)

After the implementation is complete:

### 4.1 Compile-time
- Full clean build (`.\OMEGA.ps1 deploy`). Must succeed with no warnings introduced.
- Build-time grep check must pass (`CMake check_hedging_safety target`).

### 4.2 Demo-account smoke test (account 2067070)
- Set `omega_config.ini`: `mode=LIVE`, flip ONE non-microscalper engine to `shadow_mode=false` (recommend `CandleFlow` — fewest entries per session, easiest to verify).
- Run for 1 hour during active market.
- Check `latest.log` for `[HEDGING-CLOSE-NO-POSID]` — should be ZERO if registry is populating correctly.
- Check cTrader demo: every close should reduce position count, NEVER add an opposing position.
- Check `/api/telemetry` field `hedging_registry_size` — should grow on entries, shrink on closes, never grow unbounded.

### 4.3 Failure-mode test
- Deliberately corrupt the registry (e.g., set `recordEntry` to no-op temporarily).
- Verify that close attempts log `[HEDGING-CLOSE-NO-POSID]` and the engine self-shadows.
- Verify NO opposing position appears in cTrader.

### 4.4 Live deploy gate
- Only after ALL of 4.1–4.3 pass on demo, flip ONE engine to LIVE on account 8077780.
- Cap `max_lot_gold` at 0.01 for the LIVE engine's first session. Monitor for orphan-pair pattern.
- After 24h clean operation, gradually re-enable other engines one at a time.

---

## 5. Immediate operator state (as of handoff time, 2026-05-11 09:30 NZ)

### 5.1 Open positions

**14 XAUUSD positions on live account 8077780** from Friday's orphan-pair incident. The operator is in the process of setting Stop Loss + Take Profit on each via the cTrader UI per the schedule provided this session:

| Direction | Stop Loss | Take Profit |
|---|---|---|
| Buy (7 positions) | 4714.20 | 4714.70 |
| Sell (7 positions) | 4714.70 | 4714.20 |

These will close on first tick after gold market reopens (Wellington/Tokyo session, XAUUSD liquidity resumes). Worst-case net realised loss ≈ NZ$10 (vs unrealised +NZ$12.59). Acceptable cost of flat exposure.

**Verify on session start**: ask the operator whether the 14 positions closed cleanly. If any are still open, that's the new session's first priority.

### 5.2 OmegaWatchdog state

- Service: Running, Automatic start
- Watchdog log will show `L2-CSV-MISSING` spam until gold liquidity returns. Harmless. A follow-up patch should gate this alert on `telemetry.gold_l2_real==1` — track as a side task.
- Expect a `HASH-OK: running=18b62c8 == github=<HEAD>` log line at the next 5-minute boundary after the deploy of the S25 work.

### 5.3 Operator wanted the watchdog STOPPED while position-closing was in progress

If the next session begins while the 14 positions are still open, **leave OmegaWatchdog stopped** until the operator confirms positions are flat. The watchdog will see `open_positions=0` in telemetry (because those 14 are orphans from the engine's perspective) and could trigger an auto-deploy mid-trade. After positions are flat: `Start-Service OmegaWatchdog`.

---

## 6. Useful artifacts from this session

- Commit `12902f5` — the microscalper-only hedging-close fix (Sat 2026-05-09 01:44 NZ).
- Commit `65d91b4` — watchdog fixes for Get-L2CsvPath and Get-RunningHash (Mon 2026-05-11 09:15 NZ).
- `omega_build.stamp` on the VPS at `C:\Omega\omega_build.stamp` — canonical source of running git hash.
- `C:\Omega\logs\watchdog.log` — live watchdog state.

---

## 7. Open follow-ups (lower priority than S25 core work)

1. **L2 alert gating** — make `L2-CSV-MISSING` watchdog alert depend on `telemetry.gold_l2_real==1` so it only fires when there's a real L2 feed but the CSV writer is wedged. Otherwise the watchdog log is unreadable during gold-closed windows. Single Edit in `OMEGA.ps1::Invoke-Watchdog`.
2. **Account 2067070 demo cleanup** — verify the 10 orphan demo positions from 2026-05-09 RAW FIX EVIDENCE are flat. The fix-builder comment block referenced them; they may still be hanging on demo.
3. **Bracket engine pair-close audit** — bracket engines open paired long+short legs and close both together. Verify the new `send_live_close` wrapper handles per-leg closure cleanly when only one side hits SL.
4. **GUI surface for HedgingRegistry size** — add `"hedging_registry_size":N` to `/api/telemetry` and surface in the omega-terminal dashboard. Lets the operator see the registry health at a glance.

---

## 8. Hard rules for the new session

- **DO NOT raise `max_lot_gold` above 0.01 until S25 is verified on demo.**
- **DO NOT flip any non-microscalper engine to `shadow_mode=false` until S25 implementation is complete AND demo-verified.**
- **DO NOT modify `fix_builders.hpp::build_new_order_single`** — the tag-721 emission is correct; the wrapper is the right place to add safety.
- **DO follow user preference: full files when rewriting end-to-end, Edit tool for surgical inserts in large files.**
- **DO warn the operator at 70% context usage. Give a summary.**

Good luck. This is the patch that closes Friday for good.
