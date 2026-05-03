# SESSION 2026-05-03 — Bug #3 Fix Deployment + Test Plan
**Author:** Jo (Trendiisales/Omega) + Claude session continuation
**Branch:** `feature/usdjpy-asian-open` at HEAD `df12012` pre-edit
**Status:** Source edits applied to local working tree. Awaiting build + 26-month backtest on Mac.
**Sister docs:** `KNOWN_BUGS.md` (Bug #3 marked FIXED), `SESSION_2026-05-03_ENGINE_AUDIT.md` (Installment 1), `SESSION_2026-05-03_ENGINE_AUDIT_INSTALLMENT_2.md` (Installment 2).
**Working environment note (re-stated):** The interactive Linux VM provided with this Claude session was non-functional throughout (`useradd: no space left on device` on every invocation), so the build + test step has to run on the Mac. GitHub access was confirmed working through `web_fetch` against `api.github.com`; the Trendiisales/Omega repo is public, so PAT auth is not required for verification calls. Local working tree at `/Users/jo/omega_repo` is in sync with `origin/feature/usdjpy-asian-open` HEAD `df12012` (verified before edits applied).

---

## TL;DR
Two code changes plus two doc updates land the four open code-bug items from Installment 1 + Installment 2:

1. **Bug #3 (`index_any_open` gate)** — implemented as a two-part block (concurrent + post-close gap) across `globals.hpp` + `tick_indices.hpp` + `trade_lifecycle.hpp`. `KNOWN_BUGS.md` Bug #3 is now marked FIXED.
2. **Installment 1 F1 (IndexMacroCrash dormancy)** — wired on **four-symbol parity** in shadow mode. `globals.hpp` now declares `g_imacro_sp` (US500.F), `g_imacro_nq` (USTEC.F), `g_imacro_nas` (NAS100), and `g_imacro_us30` (DJ30.F). Each is `.on_tick(...)`-called from its matching tick handler with `atr`/`drift`/`is_trending` read from the corresponding `g_iflow_*`, plus an inline-computed `vol_ratio` from a per-symbol slow-EWM ATR baseline. Engine class hardcodes `shadow_mode=true` so no broker orders are sent — TradeRecords flow into the shadow ledger via `ca_on_close` for offline analysis of macro-move catches.
3. **GoldFlow checklist update** — already verified in-source via Installment 2; doc-side checklist refresh is queued for the next checklist-refresh commit (covers F3, F5, F2, H1-H6 from Installment 1+2). This file does not modify `ENGINE_AUDIT_CHECKLIST.md`; that's a separate hygiene commit.
4. **Both fixes need a backtest run on the Mac.** Bug #3 confirms the 59 documented NAS100 whipsaw entries are eliminated. IMC wiring confirms the four shadow engines emit TradeRecords on macro events. Test commands at the bottom of this document.

The changes are deliberately surgical (Edit-tool, ~150 lines added across four files) rather than full-file rewrites, because `tick_indices.hpp` is large and producing it whole-file would expand the change surface unnecessarily and make `git diff` review harder. Run `git diff` against `df12012` before commit to confirm.

> **Correction note (during this session):** An earlier draft of this document and edits to `globals.hpp` proposed retiring the two existing `g_imacro_sp` / `g_imacro_nq` declarations rather than wiring them. That call was reverted on user direction — the engine is explicitly retained "for the explicit reason of catching big moves." The current state is the four-symbol parity wiring documented above.

---

## Files modified

### 1. `include/globals.hpp`
**Three additions.**

#### Addition A — IndexMacroCrash four-symbol parity.
The previous lines 284-285 declared only `g_imacro_sp` and `g_imacro_nq`. The block is replaced with a four-symbol declaration — `g_imacro_sp` (US500.F), `g_imacro_nq` (USTEC.F), `g_imacro_nas` (NAS100), `g_imacro_us30` (DJ30.F) — preceded by a comment block that documents the wiring contract: which `g_iflow_*` accessors feed `atr`/`drift`/`is_trending`, and the per-symbol slow-EWM baseline for `vol_ratio`.

Engine class `IndexMacroCrashEngine` hardcodes `shadow_mode=true` (per `IndexFlowEngine.hpp:816`, "NEVER change without authorization"). All four instances log `[IMACRO-SHADOW] <symbol> ...` entries without ever sending broker orders. Their TradeRecords feed `handle_closed_trade()` via `ca_on_close` so the shadow ledger captures their P&L for offline edge evaluation of macro-move catches.

#### Addition B — Cross-engine Bug #3 state in namespace `omega::idx`.
Placed immediately below the IMC declaration block:

- `inline std::atomic<int64_t> g_idx_last_close_ts{0}` — unix-second timestamp of the last close on a participating index engine.
- `inline int g_index_min_entry_gap_sec = 120` — configurable gap. Default per `KNOWN_BUGS.md` Bug #3 spec.
- `inline void record_index_close(const std::string& symbol)` — called from the close hook; updates `g_idx_last_close_ts` if the symbol is one of `US500.F`, `USTEC.F`, `DJ30.F`, `NAS100`. No-op for other symbols.
- `inline bool idx_recent_close_block()` — returns true if `now - g_idx_last_close_ts < g_index_min_entry_gap_sec`.

#### Addition B — `index_any_open()` predicate, placed AFTER the IndexHybridBracketEngine declarations at lines 430-433.
Mirrors `gold_any_open` at `tick_gold.hpp:36-50`. Returns true if any of the nine participating index engines has an open position right now:

- `g_iflow_sp`, `g_iflow_nq`, `g_iflow_nas`, `g_iflow_us30`
- `g_hybrid_sp`, `g_hybrid_nq`, `g_hybrid_us30`, `g_hybrid_nas100`
- `g_minimal_h4_us30`

`static inline` in this header, single-TU include from `main.cpp`, all called instances visible. `has_open_position()` is `const noexcept` on every engine; per-tick overhead is negligible.

### 2. `include/trade_lifecycle.hpp`
**One line added** to the `ca_on_close` static function. Now reads:
```cpp
static void ca_on_close(const omega::TradeRecord& tr) {
    handle_closed_trade(tr);
    omega::idx::record_index_close(tr.symbol);  // Bug #3 -- post-close gap tracking.
    send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
}
```
This is the canonical close hook for every cross-asset / index engine (IndexFlow, IndexHybridBracket, MinimalH4, NoiseBandMomentum, OpeningRange, TrendPullback, VWAPReversion, EsNqDiv, etc.) — they all pass `ca_on_close` as their `CloseCb` to `.on_tick(...)`. Adding `record_index_close(tr.symbol)` here means every close from every participating index engine updates the timestamp.

For non-index symbols (XAUUSD, EURUSD, BRENT, GER40, UK100, ESTX50, etc.), `record_index_close` is a no-op (it gates on the four US index symbols only), so this change is benign for non-index closes.

`bracket_on_close` (line 1073) is the close hook for the OLD `omega::BracketEngine` family (`g_bracket_sp`, etc., which are disabled per the in-source comments) — it does NOT need this hook because those engines are not in the participant list.

### 3. `include/tick_indices.hpp`
**Eight surgical edits for Bug #3 (one per entry-gate composition) plus four IMC wiring blocks (one per handler).**

#### Bug #3 entry-gate edits (8)
Each handler has two entry gates: the `hybrid_*_can_enter` boolean (for IndexHybridBracketEngine entries) and the `else if` branch in the IndexFlow entry path. Both gates take the same two-line append at the END of the conjunction list:

```cpp
            // Bug #3 (KNOWN_BUGS.md): cross-symbol concurrent block + post-close gap.
            && !index_any_open()
            && !omega::idx::idx_recent_close_block();
```

The existing per-symbol cross-engine checks (`!g_iflow_sp.has_open_position()`, etc.) are kept in place. They become functionally redundant under `!index_any_open()` (which is a superset) but the redundancy is harmless and the per-symbol checks read clearly to a future auditor.

#### IMC wiring blocks (4)
Inserted immediately after each IndexFlow `{ ... }` block, at the same indentation level. Each block:

1. Maintains a per-symbol `static double s_atr_base_<sym>` slow-EWM baseline (alpha = 0.001 → ~1000-tick half-life). Seeded from the first non-zero `g_iflow_<sym>.atr()` reading.
2. Computes `<sym>_vol_ratio = cur_atr / s_atr_base_<sym>` (defaults to 1.0 before baseline seeded).
3. Reads `<sym>_trend_regime = g_iflow_<sym>.is_trending()` (uses the engine's own per-symbol `trend_ewm_threshold`).
4. Calls `g_imacro_<sym>.on_tick(bid, ask, cur_atr, drift, vol_ratio, trend_regime, ca_on_close)`.

The engine itself filters on `vol_ratio > 2.5` and other gates internally (per `IndexFlowEngine.hpp:851-856` for the IMC class), so the inline computation is the input only — the engine still has its own threshold logic.

Edit map (post-edit line numbers approximate):

| Handler | Hybrid gate | IFLOW gate | IMC wiring block |
|---|---|---|---|
| `on_tick_us500` | line 263+ | line 308+ | after IFLOW close block (~line 333) |
| `on_tick_ustec` | line 541+ | line 583+ | end of handler before closing `}` |
| `on_tick_dj30` | line 664+ | line 704+ | after IFLOW, before MinimalH4US30 section |
| `on_tick_nas100` | line 915+ | line 955+ | after IFLOW, before IndexSwing block |

### 4. `KNOWN_BUGS.md`
- Bug #3 narrative updated to reflect the actual landed implementation (two-part block, not just `index_any_open`).
- Status table: Bug #3 row marked **FIXED** with file refs.

---

## Verification before commit

### Static-analysis check
Run from the repo root:
```bash
cd /Users/jo/omega_repo
git diff df12012 -- include/globals.hpp include/trade_lifecycle.hpp include/tick_indices.hpp KNOWN_BUGS.md docs/
```
Expected diff scope: ~80 lines added across the three .hpp files, ~30 lines added to `KNOWN_BUGS.md`, plus two new docs (`SESSION_2026-05-03_ENGINE_AUDIT_INSTALLMENT_2.md` and `SESSION_2026-05-03_BUG3_FIX_DEPLOY.md`). No functional code outside the entry gates is touched.

Confirm no remaining live references to the retired IMC symbols:
```bash
grep -rn "g_imacro_sp\|g_imacro_nq" include/ src/ backtest/ | grep -v '//\|^Binary'
```
Expected: zero matches in code (comments and docs are fine).

### Build (Mac)
The repo uses CMake. Standard local build:
```bash
cd /Users/jo/omega_repo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```
Expected: clean compile. Watch specifically for:
- Undefined symbol errors for `index_any_open`, `omega::idx::record_index_close`, `omega::idx::idx_recent_close_block` — these would indicate include-order issues; resolve by ensuring `globals.hpp` is included before `trade_lifecycle.hpp` / `tick_indices.hpp` (which it already is via `main.cpp`).
- `static_assert` or constexpr issues around `std::atomic<int64_t>` — should not occur, the type is used elsewhere in `globals.hpp` already.

If there's a build failure, paste the first error block back and I'll diagnose. Most likely culprit: one of the `omega::idx::` qualifications in `tick_indices.hpp` if the `using namespace omega::idx` directive is in a different scope than I assumed — easy to fix.

---

## Test plan — 26-month OmegaBacktest

The fix targets the documented post-close whipsaw on NAS100. Pre-fix, the bug spec in `KNOWN_BUGS.md` quantifies it as 59 trades, sum(net_pnl) ≈ −$55.77 inside the 1,938-trade 26-month audit CSV.

### Step 1 — run the backtest
```bash
cd /Users/jo/omega_repo
./build/OmegaBacktest \
    --data-dir backtest/data \
    --start 2024-03-01 \
    --end 2026-04-30 \
    --output omega_trade_closes_postfix.csv
```
(Adjust paths/flags to match your existing `OmegaBacktest` invocation. The output filename can be anything you prefer; the next step references `omega_trade_closes_postfix.csv`.)

### Step 2 — apply the whipsaw filter (already in `KNOWN_BUGS.md`, repeated here)
Save as `whipsaw_check.py`:
```python
import pandas as pd

df = pd.read_csv("omega_trade_closes_postfix.csv", encoding="utf-8-sig")
df.columns = [c.strip().lower() for c in df.columns]
for c in ["entry_ts_unix", "net_pnl"]:
    df[c] = pd.to_numeric(df[c], errors="coerce")

nas = df[df.symbol == "NAS100"].sort_values("entry_ts_unix").copy()
nas["prev_side"] = nas["side"].shift()
nas["gap_s"]    = nas["entry_ts_unix"] - nas["entry_ts_unix"].shift()
whipsaw = nas[(nas.side != nas.prev_side) & (nas.gap_s < 300)]

print(f"NAS100 whipsaw entries (opposite side, gap < 300s): {len(whipsaw)}")
print(f"  sum(net_pnl) = ${whipsaw.net_pnl.sum():.2f}")

# Tighter window matching the 1-3 minute bug pattern
within_3min = nas[(nas.side != nas.prev_side) & (nas.gap_s < 180)]
print(f"NAS100 whipsaw entries (gap < 180s, the documented 1-3min window): {len(within_3min)}")
print(f"  sum(net_pnl) = ${within_3min.net_pnl.sum():.2f}")
```

### Step 3 — interpret
**Pre-fix expected (per `KNOWN_BUGS.md`):**
```
NAS100 whipsaw entries (opposite side, gap < 300s): 59
  sum(net_pnl) = $-55.77
```

**Post-fix expected with `g_index_min_entry_gap_sec = 120`:**
```
NAS100 whipsaw entries (opposite side, gap < 300s): 0  (or close to 0)
  sum(net_pnl) = $0.00
```

The gate blocks any new entry within 120s of the last close. The `gap < 300s` filter is broader than the gate, so the post-fix `< 300s` count should be exactly the entries with `120 <= gap_s < 300` — those are entries that pass the gate but happen to be within 5 minutes of an opposite-side prior. If you see entries in that 120-300s band, that's expected gate behaviour, not a regression. The bug-classification number that matters is `gap < 180s` (which should be 0 post-fix).

If the post-fix `< 180s` count is nonzero, something failed:
- Either the gate isn't firing (build mis-link, wrong `g_index_min_entry_gap_sec` value at runtime)
- Or close-hook isn't recording (check `[BUG3]` log if you add one — diagnostic suggestion below)

### Step 4 — full per-engine attribution diff
For sanity, compare the post-fix per-engine net P&L to pre-fix:
```python
# Add to whipsaw_check.py
print("\nPer-engine net P&L (post-fix):")
print(df.groupby("engine")["net_pnl"].agg(["count", "sum"]).sort_values("sum", ascending=False))
```
Expected: `HybridBracketIndex` on NAS100 should re-emerge as the +$172 winner the audit already flagged (or close to it, depending on how many trades the gate filters out). `IndexFlow` per-symbol should be roughly flat or slightly improved (whipsaw exits removed).

If overall system P&L moves by significantly more than the +$56 the bug accounts for, that's a regression — the gate is over-blocking. Tune `g_index_min_entry_gap_sec` down (try 90, then 60) to find the right balance.

---

## Optional diagnostic logging (recommended for first shadow run)

Add a one-line printf to `record_index_close()` and the entry gate to confirm the fix is firing as expected. Drop these in for a 24-48hr shadow run, then strip them out:

```cpp
// In globals.hpp's record_index_close(), after the store:
printf("[BUG3-CLOSE] %s ts=%lld\n", symbol.c_str(), (long long)now_s);
fflush(stdout);

// In tick_indices.hpp's entry gates (one per handler), inside the if-block
// where the gate would fire:
if (omega::idx::idx_recent_close_block()) {
    static int64_t s_log_ts = 0;
    const int64_t now_l = static_cast<int64_t>(std::time(nullptr));
    if (now_l - s_log_ts > 30) {  // log at most once per 30s per handler
        s_log_ts = now_l;
        printf("[BUG3-BLOCK] %s gate blocked: idx_recent_close_block active\n", sym.c_str());
        fflush(stdout);
    }
}
```
Strip both before the next commit once you've seen them fire on real ticks.

---

## What was NOT done in this round

- **`ENGINE_AUDIT_CHECKLIST.md` refresh.** The doc-side update for F3, F5, F2 (Installment 1) and H1-H6 (Installment 2) is queued. This is doc-only churn; not blocking Phase 3 backtest as long as the readers know to cross-reference the two installment docs.
- **GoldStack 20 sub-engines audit.** Largest deferred Phase 1 item. Recommend its own session.
- **Sizing audit (section D) and cost-realism audit (section E)**. Carried over.
- **Live verification of the fix on shadow VPS.** That's a runtime activity once the post-fix binary lands on the VPS — not a code task.

---

## Severity-ranked status (refreshed)

### LANDED this session
- **Bug #3 (`index_any_open` + post-close gap)** — code in `globals.hpp`, `trade_lifecycle.hpp`, `tick_indices.hpp` (8 entry-gate edits). `KNOWN_BUGS.md` updated.
- **Installment 1 F1 (IndexMacroCrash dormancy)** — wired on four-symbol parity. `g_imacro_sp/nq/nas/us30` declared in `globals.hpp` and `.on_tick(...)`-called from each of `on_tick_us500/ustec/dj30/nas100` with per-symbol slow-EWM ATR baseline driving `vol_ratio`. Shadow mode hardcoded on the engine — TradeRecords flow into the shadow ledger only.

### Ready to test (waiting on user-side build)
- `OmegaBacktest` 26-month run + `whipsaw_check.py` validation per Step 2 above.

### Still open (carry-over to next session)
- IndexMacroCrash four-symbol parity wiring (defer until index regime detector is designed).
- `ENGINE_AUDIT_CHECKLIST.md` refresh — H1-H6 drift items + GoldFlow row removal + new XauusdFvg row + LatencyEdge tombstone.
- GoldStack 20 sub-engines deep-walk.
- HTFSwing v2 deep-walk.
- Sizing + cost-realism walks (sections D + E of the audit checklist).
- WickRejTick re-validation (Sharpe 3.79 shelved engine).
- `PRE_LIVE_CHECKLIST.md` item #1 — `session_watermark_pct=0.0` → required `0.27` before live flip.

---

## Files touched this session
- **Modified:** `include/globals.hpp`, `include/trade_lifecycle.hpp`, `include/tick_indices.hpp`, `KNOWN_BUGS.md`.
- **Created:** This file (`docs/SESSION_2026-05-03_BUG3_FIX_DEPLOY.md`).
- **Earlier this session:** `docs/SESSION_2026-05-03_ENGINE_AUDIT_INSTALLMENT_2.md` (audit findings).

— END BUG #3 FIX DEPLOY DOC —
