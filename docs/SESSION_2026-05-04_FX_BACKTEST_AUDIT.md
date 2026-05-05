# FX Backtest Audit — picking up from the 2026-05-04 deploy of c40ad5f

**Session date:** 2026-05-04
**Continues:** Session summary at end of prior session (audit-fixes-37 deploy: EUR/GBP/USDJPY/AUD/NZD session windows restored, gold SL widened, Tsmom max_lot_cap tightened to 0.02).
**Scope:** Answer the five FX-backtest questions left open in the prior session's "Next-session brief: FX backtest options."
**Deliverable:** This doc + a concrete sweep plan, no code changes.
**Status:** Audit only. No engine, harness, or config files modified.

---

## TL;DR

There is one production-grade per-pair FX backtest harness in the repo: `backtest/usdjpy_bt/` plus its five sweep / walk-forward / exit-analysis Python drivers. It was built for `UsdjpyAsianOpenEngine` and uses the methodology referenced earlier as "S59 USDJPY walk-forward."

For the four other live FX engines (`EurusdLondonOpen`, `GbpusdLondonOpen`, `AudusdSydneyOpen`, `NzdusdAsianOpen`), there is **no per-pair backtest harness checked in** and only **two pairs of HistData tick feeds present on this Mac** (EURUSD + USDJPY). The EURUSD handoff doc (Sections 7b / 7c) cites concrete 14-month sweep results — those numbers exist in the doc but the code that produced them is not in the repo. That is a reproducibility gap to flag.

The cleanest path for this session is:

1. Stand up an EURUSD harness by porting `backtest/usdjpy_bt/` to EURUSD with two real changes (HistData filename pattern + cost model). This validates the harness pattern works for direct USD-quote pairs, and re-grounds the EURUSD S55/S56 numbers in checked-in code.
2. Defer GBP / AUD / NZD harnesses until you decide whether to download HistData for those pairs (none of their tick data is on this Mac). Each costs ~14 GB unzipped per pair per year; a 14-month full-tick set per pair is ~16 GB.
3. Use the **existing S59 USDJPY walk-forward gates as-is** (they're stricter than the EURUSD shadow-promotion gate the handoff docs cite, and they're the right gates for promoting an FX engine to live).

A concrete sweep plan and per-pair priority order are in Section 8.

---

## 1. Backtest harness inventory

### 1.1 USDJPY — full per-pair harness

Files in repo:

| Path | Purpose |
|---|---|
| `backtest/usdjpy_bt/UsdjpyAsianOpenBacktest.cpp` | Standalone C++ tick-replay binary. Reads HistData ASCII USDJPY monthlies, drives a single `UsdjpyAsianOpenEngine` instance, writes per-trade and per-month CSV outputs. Built via `g++ -std=c++17 -O3 -DOMEGA_BACKTEST -I backtest/usdjpy_bt -I include backtest/usdjpy_bt/UsdjpyAsianOpenBacktest.cpp -o build/usdjpy_asian_bt -pthread`. |
| `backtest/usdjpy_bt/OmegaTradeLedger.hpp` | Minimal `omega::TradeRecord` definition for the standalone build. |
| `backtest/usdjpy_bt/SpreadRegimeGate.hpp` | Standalone copy of the spread-regime gate the engine consumes. |
| `backtest/usdjpy_bt/OmegaNewsBlackout.hpp` | Standalone news-blackout stub. |
| `backtest/usdjpy_bt/UsdjpyAsianOpenEngine.hpp` | **Auto-generated** copy of `include/UsdjpyAsianOpenEngine.hpp` with sweep parameters substituted by `scripts/usdjpy_asian_sweep.py`. Git-ignored at line 112 of `.gitignore`. The production header is never modified. |
| `scripts/usdjpy_asian_sweep.py` | Single-axis parameter sweep driver. 8 axes baked in (TP_RR, SL_FRAC, BE_TRIGGER_PTS, MIN_RANGE, MFE_TRAIL_FRAC, COOLDOWN_S, TRAIL_FRAC, SAME_LEVEL_BLOCK_PTS). Sed-rewrites the bt-side header, recompiles, runs, appends one row per cell to `build/usdjpy_sweep_results.csv`. Resumable via label-already-in-CSV check. |
| `scripts/usdjpy_asian_trail_sweep.py` | Trail-specific sweep (referenced from the walk-forward as the source of `write_engine_copy()` + `compile_bt()`). |
| `scripts/usdjpy_asian_walkforward.py` | 8-fold rolling 1-month-test walk-forward with hard / soft / fail verdict. Pre-flight verifies a fixed baseline (PnL=$317.10, DD=$202.51, profitable months=8/14) reproduces deterministically before running the folds. |
| `scripts/usdjpy_asian_exit_sweep.py`, `scripts/usdjpy_asian_exit_analysis.py` | Exit-side analytics. |

Key harness properties:

- **Tick-faithful.** Streams every HistData tick chronologically through a single engine instance and accepts the engine's own `on_tick(...)` close callback. The engine sees the same code path it sees in production.
- **Single-config-per-binary.** The sweep wrappers recompile per cell. ~7 s per recompile on the Mac.
- **Resumable.** Both sweep and walk-forward skip cells already in the results CSV.
- **Deterministic baseline pre-flight.** `usdjpy_asian_walkforward.py` won't run folds unless the chosen-winner reproduces the prior-session baseline within 1 cent. This is the "tick-data alignment" guard called out as gotcha #3 in the walk-forward handoff.

### 1.2 EURUSD — handoff-doc-only, **no checked-in code**

`docs/SESSION_2026-05-02_EURUSD_LONDON_OPEN_HANDOFF.md` Sections 7b and 7c document an "S55 backtest tune" and an "S56 comprehensive backtest tune" with concrete numbers (14-month HistData EURUSD, 25.3M ticks, S56 in-sample +$626 PF=4.75, OOS +$114 PF=1.04, WR=64.9%). **The harness and sweep scripts that produced those numbers are not in this repo on `main`.** Searched paths:

- `backtest/eurusd_bt/` — does not exist.
- `backtest/**eurusd**`, `backtest/**EURUSD**` — no matches.
- `scripts/**eurusd**` — no matches.
- `Grep eurusd_bt|EurusdLondonOpenBacktest|EurusdLondonOpenSweep` — only the `.gitignore` line matches (and it's for the `usdjpy_bt` shape, no EURUSD analogue).

The EURUSD HistData feed on this Mac (`/Users/jo/Tick/EURUSD/`) **does** cover 2025-03 through 2026-04 (14 months), so the data side is reproducible. The harness side is missing — either the EURUSD bt code was lost, lived on a non-merged branch, or was generated and discarded. This is the single biggest reproducibility risk for the FX cohort and is unrelated to the audit-fixes-37 work that motivated this session.

### 1.3 GBPUSD / AUDUSD / NZDUSD — no harness, no scripts, no data

- No `backtest/gbpusd_bt/`, `backtest/audusd_bt/`, `backtest/nzdusd_bt/`.
- No `scripts/gbp*`, `scripts/aud*`, `scripts/nzd*`.
- No HistData under `~/Tick/GBPUSD/`, `~/Tick/AUDUSD/`, `~/Tick/NZDUSD/`.

The engine headers (`include/GbpusdLondonOpenEngine.hpp`, `AudusdSydneyOpenEngine.hpp`, `NzdusdAsianOpenEngine.hpp`) all carry the same self-description: parameters are inherited from USDJPY S55-S59 as **PRE-SWEEP defaults**, and the engine comments explicitly say a parallel sweep is required before live promotion. Those sweeps cannot run today.

### 1.4 Indices and gold — context only

The gold side (`backtest/omega_backtest_standalone.cpp` + `gold_*` files) and indices side (`IndexFlowBacktest.cpp`, `indices_backtest.cpp`) are healthy and out of scope here. They use the same single-config-per-binary pattern but read different vendor formats.

---

## 2. Available tick data on this Mac

`~/Tick/` audit (sample, full set in repo files at session-time):

| Pair | Coverage | Months | Notes |
|---|---|---|---|
| EURUSD | 2025-03 → 2026-04 | 14 | HistData ASCII tick CSVs, monthly subdirs `HISTDATA_COM_ASCII_EURUSD_T<YYYYMM>/`. Same vendor format the USDJPY harness already parses. |
| USDJPY | 2025-01 → 2026-04 | 16 | Includes a `(1)` duplicate dir for 2025-03 and 2026-02 — already filtered out by `collect_monthly_files()` in the harness (line 305 of `UsdjpyAsianOpenBacktest.cpp`). |
| NSXUSD (NAS100) | 2025-01 → 2026-04 | 16 | Used by the indices harness, not relevant to FX. |
| XAUUSD | One 2yr CSV | n/a | Different format; gold harness consumes it. |
| GBPUSD | **none** | 0 | Not downloaded. |
| AUDUSD | **none** | 0 | Not downloaded. |
| NZDUSD | **none** | 0 | Not downloaded. |

Plus the L2 bundle under `~/Tick/l2_vps/l2_bundle/` for gold (April 2026 only) — not FX.

A `~/Tick/Omega/` legacy mirror exists; it duplicates the gold L2 set but contains no FX content.

**Implication:** EURUSD is the only FX pair besides USDJPY where a backtest can run today. GBP/AUD/NZD require a HistData download before any backtest is possible. HistData free-tier monthly downloads are typical on this project; a 14-month set per pair runs ~16 GB unzipped, manageable but not instantaneous.

---

## 3. Walk-forward gates — what they actually are

The prior session summary cited "WR ≥ 60%, ≥ 30 trades, net positive after costs" as the S59 USDJPY walk-forward gate. That's a paraphrase that conflates two different gates that exist in the repo. Here are the actuals:

### 3.1 The walk-forward gate the script enforces (`scripts/usdjpy_asian_walkforward.py:175-183`)

| Gate | Threshold | Source line |
|---|---|---|
| SOFT total PnL across 8 test months | ≥ $56 | `SOFT_TOTAL_PNL_MIN = 56.0` |
| SOFT max deeply-negative test months | ≤ 2 (threshold $-80) | `SOFT_DEEP_NEG_MAX = 2`, `DEEP_NEG_THRESHOLD = -80.0` |
| SOFT walk-forward profit factor | ≥ 1.05 | `SOFT_PF_MIN = 1.05` |
| HARD walk-forward profit factor | ≥ 1.15 | `HARD_PF_MIN = 1.15` |
| HARD profitable test months | ≥ 5 of 8 | `HARD_PROF_MONTHS_MIN = 5` |
| HARD worst single-month DD | < $200 | `HARD_WORST_DD_MAX = 200.0` |
| HARD-FAIL profit factor | < 1.0 | `FAIL_PF_MAX = 1.00` |
| HARD-FAIL worst single-month PnL | < $-200 | `FAIL_WORST_PNL_MAX = -200.0` |

Win rate is **not** in the verdict logic. The verdict is structured around PF, profitable-month count, and worst-case drawdown — which is the right shape for a "promote to live or not" decision.

### 3.2 The promotion gate the EURUSD handoff doc cites (Section 6)

| Gate | Threshold |
|---|---|
| Closed shadow trades over 2 weeks | ≥ 30 |
| Win rate | ≥ 35% |
| Net expectancy after costs | > 0 |
| London-session activity | most fires in 06–09 UTC |
| Same-level block effectiveness | no SL clusters in block radius |

This is a **shadow → live promotion** gate, not a walk-forward gate. The 35% WR floor is the compression-breakout strategy floor; with TP_RR=2 it's the break-even line. It's deliberately permissive because the BE-lock + same-level block do most of the heavy lifting.

### 3.3 The promotion gate the AUDUSD engine header cites

`include/AudusdSydneyOpenEngine.hpp:60` says "30-trade minimum sample, WR >= 60% per the USDJPY S56 promotion gate." That's a different number again, and S56 is the EURUSD chosen-winner ID, not USDJPY. The header is using "S56" loosely; the actual USDJPY chosen-winner is S59 with a different (RR=0.5, SL_FRAC=1.00) shape.

### 3.4 Recommended gates for this cohort

For each FX engine, two gates apply at different decision points:

1. **Walk-forward survival (before any live-promotion plan):** Use the S59 USDJPY thresholds in 3.1 verbatim. They were tuned for FX-pair PnL magnitudes and they're the strictest set we have. Same gate for all four pairs gives apples-to-apples comparison.
2. **Shadow → live promotion (after walk-forward survives):** Pair-specific. The current EURUSD-style "≥30 trades, WR ≥ 35%, net positive" floor is fine for EUR/GBP because the parameters are RR=2 (TP-bias), but for AUD/NZD where the engine inherits S59's RR=0.5 / SL_FRAC=1.00 (trail-bias), the appropriate WR floor is closer to 60% — that's what the S59 USDJPY 14-month run actually achieved (WR=83.9%) and what makes the negative-asymmetric (avg_loss > avg_win) math work.

The "WR ≥ 60%" header comment in AUDUSD is therefore **correct as a promotion-gate intent** — it's just attributed to the wrong sweep ID. Suggest the walk-forward CSV's `wr` column be checked against 60% as a soft sanity check on top of the structural PF/DD gates from 3.1.

---

## 4. Session-window backtest viability

All five engines now declare production session windows (post audit-fixes-37). Source-of-truth values:

| Engine | START | END | Wraparound? | Wraparound check present? |
|---|---|---|---|---|
| `EurusdLondonOpenEngine.hpp:251-252` | 6 | 9 | No | n/a (forward) |
| `GbpusdLondonOpenEngine.hpp:247-248` | 7 | 10 | No | n/a (forward) |
| `UsdjpyAsianOpenEngine.hpp:242-243` | 0 | 4 | No | n/a (forward) |
| `AudusdSydneyOpenEngine.hpp:269-270` | 22 | 2 | **Yes** | Yes — ternary at line ~437–443 (`SESSION_END > SESSION_START ? forward : wraparound`) |
| `NzdusdAsianOpenEngine.hpp:271-272` | 22 | 4 | **Yes** | Yes — `in_window` ternary at line 439 |

The wraparound-aware in-window check is identical in shape to:

```cpp
const bool in_window =
    (SESSION_END_HOUR_UTC > SESSION_START_HOUR_UTC)
        ? (utc.tm_hour >= SESSION_START_HOUR_UTC &&
           utc.tm_hour <  SESSION_END_HOUR_UTC)
        : (utc.tm_hour >= SESSION_START_HOUR_UTC ||
           utc.tm_hour <  SESSION_END_HOUR_UTC);
if (!in_window) return;
```

This is correct for both shapes: when END > START it reduces to the forward case; when END ≤ START (wraparound) the predicate is the union of `[START..23]` and `[0..END)`.

**Backtest implication:** the harness doesn't need to know about session windows at all. Each engine self-gates inside `on_tick()` based on the UTC hour of the tick timestamp. The harness just feeds chronological ticks; the engine drops the ones outside its window. A "production-session-only" backtest pass is therefore the **default** behaviour — there is nothing to opt into.

The only thing the harness needs is **UTC-correct timestamps**. The USDJPY harness handles this in `parse_histdata_ts()` (line 88 of `UsdjpyAsianOpenBacktest.cpp`) by treating HistData's EST timestamps as UTC for parity with the EURUSD S55/S56 work. The header comment at line 86 acknowledges this is a deliberate alignment choice: HistData publishes EST timestamps but the engine logic is in UTC, and the offset is treated as a constant so session-window tests are still correct *relative to* the timestamps, just shifted by a fixed 5h. **For the live engines, where ticks arrive in true UTC from the broker, this is a divergence that needs to be resolved before backtest numbers are taken as predictive of live behaviour.** Either (a) shift HistData timestamps by +5h on ingest, or (b) shift the engine's session-window constants by -5h for backtest-only and document the mapping. The USDJPY S59 result was found under interpretation (a) implicitly (the comment is accurate for the EURUSD S55/S56 path it was paired with).

That's a real find from this audit — flag it as Item F (carry-forward MEDIUM).

---

## 5. Cost realism — `apply_costs_usd` and what it covers

### 5.1 USDJPY harness cost model (lines 157-170 of `UsdjpyAsianOpenBacktest.cpp`)

```cpp
static double apply_costs_usd(double pnl_usd, double size, bool /*is_long*/) noexcept {
    const double slip_pips_per_side = 0.2;
    const double slip_jpy_price     = slip_pips_per_side * 0.01;
    const double live_mid = g_usdjpy_live_mid.load(std::memory_order_relaxed);
    const double tick_mult = (live_mid > 0.0) ? (100000.0 / live_mid) : 667.0;
    const double slip_usd = slip_jpy_price * size * tick_mult * 2.0; // both sides
    const double comm_usd = 0.20 * size * 2.0; // per-side per-lot
    return pnl_usd - slip_usd - comm_usd;
}
```

What it charges (per round-trip):

- **Slippage:** 0.2 pips per side × 2 sides = 0.4 pips. For USDJPY a pip is 0.01 in price units, so this is 0.004 in price.
- **Commission:** $0.20 per side per lot × 2 sides = $0.40 per round-trip per lot.
- **Spread:** **NOT charged here.** The comment at line 158 says spread is implicitly paid because the engine uses ask for long entries and bid for long exits (and vice-versa), and the harness records `pos.entry = bracket_high` (an ask-cross) and `exit_px = bid` (a bid-cross). That's true for USDJPY where the harness reads ask/bid directly from each tick.

What this misses:

- **Spread regime.** The implicit-spread argument holds for the *mean* spread but not for spread blowouts during news. The engine has its own `MAX_SPREAD` gate (0.02 in JPY for USDJPY, 0.0002 in USD for the others) which mostly handles this — but a pessimistic backtest would also charge a 25th-percentile-of-spread additional cost on a small fraction of trades. Not worth it for a first pass.
- **Swap / overnight.** None of the FX engines hold overnight (max hold = `PENDING_TIMEOUT_S = 180` for the bracket, whatever the trail closes for the position) so swap is not material.
- **BlackBull commission model.** $0.20/lot/side is in line with BlackBull Prime account spec; standard account is $0/commission with wider spread which is actually slightly worse for these engines (the spread blowout would dominate). For a "live realism" backtest, mirror the broker account that will run the engine.

### 5.2 What an EURUSD / GBPUSD / AUDUSD / NZDUSD harness needs to charge

The pip math differs from USDJPY:

| Pair | Pip = | At 0.10 lot, 1 pip = | Quote convention |
|---|---|---|---|
| EURUSD | 0.0001 | $1 | Direct USD |
| GBPUSD | 0.0001 | $1 | Direct USD |
| AUDUSD | 0.0001 | $1 | Direct USD |
| NZDUSD | 0.0001 | $1 | Direct USD |
| USDJPY | 0.01 | $1 (≈, at JPY=150) | Indirect; needs live-mid conversion |

For the four direct-USD-quote pairs the cost function simplifies. The USDJPY-style adapted version:

```cpp
static double apply_costs_usd_direct(double pnl_usd, double size, bool /*is_long*/) noexcept {
    // Direct USD-quote pairs (EUR/GBP/AUD/NZD) — pip = 0.0001, $1 per pip at 0.10 lot.
    const double slip_pips_per_side = 0.2;     // 0.2 pip per side, 0.4 pip round-trip
    const double slip_price_per_side = slip_pips_per_side * 0.0001;
    // pip_value_usd at default 0.10 lot is $1, scales linearly with size.
    // size==0.10 -> $1 per pip; size==1.00 -> $10 per pip; size==0.01 -> $0.10 per pip.
    // Per-side slip in USD = slip_price_per_side * size * 100000 (pips per unit) ... but
    // that simplifies to: slip_pips_per_side * (size / 0.10) * 1.0 USD.
    const double slip_usd = slip_pips_per_side * (size / 0.10) * 1.0 * 2.0;
    const double comm_usd = 0.20 * size * 2.0; // per-side per-lot, same as JPY
    return pnl_usd - slip_usd - comm_usd;
}
```

For a 0.10-lot trade: round-trip cost = 0.4 pips × $1/pip + $0.04 commission = **$0.44**. For comparison, the USDJPY at 0.10 lot pays the equivalent in JPY price units × `100000/mid` × size × 2 + $0.04 ≈ **$0.31** at JPY=150. That's slightly cheaper for JPY because the slippage-in-pips-of-USD is lower (the JPY pip is 100× bigger, so 0.2 of those is 20 USD-equivalent micro-pips).

This is fine for a first implementation. A second-pass refinement would use the live broker's measured spread distribution per pair instead of the implicit-spread-via-ask/bid-cross approach.

### 5.3 Action: Item E in the audit checklist remains MEDIUM

The audit checklist's Item E ("Costs in trade record") is about `tr.spreadAtEntry` and `apply_realistic_costs()` in the **production** trade-lifecycle pipeline, not the backtest cost model. Outstanding issue #10 in the checklist ("Cost-realism audit (section E) walk across all live engines pending") is still pending and is unrelated to the backtest cost discussion above. They share a name but are different things; flag this for clarity in the next checklist refresh.

---

## 6. The reproducibility gap on EURUSD

The EURUSD handoff doc (Sections 7b/7c) cites:

- 14-month HistData backtest, 25.3M ticks
- S55 chosen-winner +$188 PnL / PF 0.89 / 974 trades / WR 56.1%
- S56 chosen-winner +$626 PnL / PF 4.75 / 854 trades / WR 66.6% (in-sample)
- S56 OOS (Oct 2025–Apr 2026): +$114 PnL / PF 1.04 / 308 trades / WR 64.9%
- Lot sizing analysis based on Kelly fraction = 11.5%, half-Kelly = 5.7%

**These numbers are not reproducible from the current `main`.** No EURUSD harness is checked in. The engine header at `include/EurusdLondonOpenEngine.hpp:139-244` carries the S56-tuned parameter values (SL_FRAC=0.80, TP_RR=2.0, TRAIL_FRAC=0.30, MFE_TRAIL_FRAC=0.40, BE_TRIGGER_PTS=0.0006, SAME_LEVEL_BLOCK_PTS=0.0008, SAME_LEVEL_POST_SL_BLOCK_S=1200, COOLDOWN_S=120, MAX_RANGE=0.0050, LOT_MAX=0.20) but there is no checked-in harness that produces those numbers.

**Three possibilities:**

1. The harness was built and used to produce the numbers but wasn't committed (lost in a merge or worktree).
2. The harness lives on a feature branch that wasn't merged to `main` — search `git branch -a` for `eurusd_bt`, `S55`, `S56`, or similar.
3. The numbers were estimated by a different mechanism (manual spreadsheet, side-channel script) and the doc presented them as harness output.

Recommendation: before promoting the EURUSD engine past shadow, the S55/S56 numbers need to be reproducible from a checked-in harness. Otherwise the chosen-winner parameters in `EurusdLondonOpenEngine.hpp` are not auditable.

This is not blocking the **shadow-only** behaviour the engine has today — it just means the live-promotion gate cannot be satisfied with confidence until the EURUSD harness exists in `main`.

---

## 7. Summary of answers to the prior session's five questions

1. **Per-pair FX backtest harnesses for GBP/AUD/NZD?** No. Only USDJPY has one. EURUSD's exists in the handoff doc's narrative but not in the repo (Section 6 is the gap).
2. **HistData / Dukascopy / cTrader feeds, time coverage on Mac?** EURUSD 14 months and USDJPY 16 months in `~/Tick/`, both HistData ASCII tick. GBP/AUD/NZD: zero. Dukascopy and cTrader feeds: not present on this Mac (the repo's `scripts/combine_dukascopy_monthly.py` exists but no Dukascopy data is staged).
3. **Walk-forward gates for new pairs?** Use the S59 walk-forward gates verbatim (Section 3.1). The session summary's paraphrase of the gates was inaccurate; actual gates are PF / profitable-months / DD / total-PnL based, not WR-and-trade-count based.
4. **Production-session-only backtest pass?** It's the default. The engines self-gate on UTC hour inside `on_tick()`; the harness just feeds ticks. Wraparound is correctly handled in AUDUSD and NZDUSD via the END>START ternary. The HistData-EST-as-UTC convention used in the USDJPY harness is a 5h shift relative to live broker UTC ticks — flagged as Item F MEDIUM (Section 4 last paragraph).
5. **`apply_realistic_costs()` per pair?** Production-side `apply_realistic_costs()` audit is Outstanding #10 in `ENGINE_AUDIT_CHECKLIST.md` and is separate from the backtest harness cost model. The backtest cost model in the USDJPY harness (Section 5.1) charges 0.4 pips slip + $0.40/lot commission round-trip; the equivalent for EUR/GBP/AUD/NZD is straightforward (Section 5.2).

---

## 8. Concrete sweep plan

**Priority order, this session and the next 2–3 sessions:**

### 8.1 Session +1 (this one) — backstop the EURUSD gap

Goal: make the EURUSD S55/S56 numbers reproducible from a checked-in harness on `main`.

Steps (each is a separate commit):

1. `audit-fixes-38a`: Create `backtest/eurusd_bt/` by copying `backtest/usdjpy_bt/` and renaming.
   - `UsdjpyAsianOpenBacktest.cpp` → `EurusdLondonOpenBacktest.cpp`. Search-and-replace `USDJPY` → `EURUSD`, `Usdjpy` → `Eurusd`, `usdjpy` → `eurusd`, `JPY` → `EUR`, `g_usdjpy_live_mid` → remove (direct USD, no live-mid needed), HistData prefix `HISTDATA_COM_ASCII_USDJPY_T` → `HISTDATA_COM_ASCII_EURUSD_T`, file pattern `DAT_ASCII_USDJPY_T_` → `DAT_ASCII_EURUSD_T_`. Replace `apply_costs_usd` with the direct-quote version from Section 5.2. Engine include: `EurusdLondonOpenEngine.hpp`.
   - `OmegaTradeLedger.hpp`, `SpreadRegimeGate.hpp`, `OmegaNewsBlackout.hpp`: copy unchanged.
   - `.gitignore`: add `backtest/eurusd_bt/EurusdLondonOpenEngine.hpp` (the auto-generated copy) the same way the USDJPY one is ignored at line 112.
2. `audit-fixes-38b`: Port `scripts/usdjpy_asian_sweep.py` → `scripts/eurusd_london_sweep.py`. Same shape, EURUSD paths, default BASELINE matching the production header values from the EURUSD engine. Sweep axes adjusted for FX-direct-quote pip scale (MIN_RANGE candidates {6, 8, 10, 12, 15} pips, etc).
3. `audit-fixes-38c`: Port `scripts/usdjpy_asian_walkforward.py` → `scripts/eurusd_london_walkforward.py`. Same 8-fold shape, EURUSD baseline from a fresh chosen-winner full-14-month run. Update `BASELINE_PNL` / `BASELINE_DD` / `BASELINE_PROF_M` to whatever the new harness produces.
4. `audit-fixes-38d`: Reproduce the S55 and S56 numbers from the EURUSD handoff. If they match (within ~5% — HistData/data-alignment slop), mark Section 6 of the handoff doc as "✓ reproduced 2026-05-04 from `eurusd_london_walkforward.py` baseline." If they don't match within 20%, that's a real find — the numbers in the handoff are wrong.

Expected wallclock: 2–3 hours of focused work. Mostly mechanical translation + one debug pass on the cost function.

### 8.2 Session +2 — first GBP backtest

Goal: GBP harness + a production-session-only baseline run. Promotion-readiness comes later.

Steps:

1. **Decision needed first:** which vendor for GBP tick data? HistData free-tier covers it; Dukascopy is the alternative. Recommendation: stick with HistData for symmetry with the EURUSD/USDJPY pipeline.
2. Download HistData GBPUSD 2025-03 → 2026-04 (14 months) to `~/Tick/GBPUSD/HISTDATA_COM_ASCII_GBPUSD_T<YYYYMM>/`. ~16 GB.
3. Port `backtest/eurusd_bt/` → `backtest/gbpusd_bt/`. Same recipe as 8.1 step 1. Engine include: `GbpusdLondonOpenEngine.hpp`. Cost model identical to EURUSD (direct USD quote).
4. Port `scripts/eurusd_london_sweep.py` → `scripts/gbpusd_london_sweep.py`. Sweep the same axes; the GBP engine pre-sweep defaults are inherited from EURUSD S56 so the sweep is exploring the *same* space starting from the same point. The GBP-specific question is whether the wider spread profile (0.7–1.5 pips typical vs EUR's 0.5–1.4) shifts the optimum.
5. Run a 14-month full-pass at the inherited EURUSD-S56 defaults. Compare against the S59 walk-forward gates from Section 3.1. If it passes the SOFT bar, run the walk-forward; if it fails, sweep first.

### 8.3 Session +3 — AUD + NZD

Both pairs share the Asian-session window (22-02 and 22-04 UTC, wraparound). Both inherit S59 USDJPY parameters as pre-sweep defaults, which is a different shape from EUR/GBP (RR=0.5 / SL_FRAC=1.00 vs RR=2.0 / SL_FRAC=0.80). The right gate set for both is also the S59 walk-forward gate (3.1).

Steps mirror Session +2 with two pairs in parallel. The key empirical question for AUD/NZD is whether the S59 USDJPY parameter set (which is JPY-volatility-tuned) holds up at the smaller AUD/NZD pip-scale per-second move. Pre-sweep defaults should be sweep-validated, not assumed correct.

### 8.4 Session +4 — promotion-readiness pass

Once each pair has a checked-in harness and a chosen-winner that survives the walk-forward gate:

1. Audit the **production-side** `apply_realistic_costs()` plumbing for each pair (Outstanding #10 in the audit checklist). Confirm `tr.spreadAtEntry` is populated and `tr.net_pnl` reflects spread + commission cost.
2. Convert each pair's chosen-winner constants from the handoff-doc S5x format to a `audit-fixes-XX` commit that updates the production engine header. Each promotion is one commit.
3. After 2 weeks of shadow data on the chosen-winner config, apply the **shadow → live promotion gate** from Section 3.2 (or 3.3 for AUD/NZD's higher WR floor).

---

## 9. Items added to the carry-forward list

| Item | Severity | Owner | Notes |
|---|---|---|---|
| F. HistData EST-as-UTC convention vs live broker UTC ticks | MEDIUM | next session | 5h timestamp shift discussed in Section 4. Doesn't break anything today (both sides use the same convention) but means session-window backtest results are mapped to a different UTC hour than live trades. Either shift HistData on ingest or document the mapping. |
| G. EURUSD harness reproducibility gap | HIGH | session +1 (8.1) | S55/S56 numbers in the EURUSD handoff doc are not reproducible from `main`. Blocks live-promotion confidence even though the engine is shadow-correct today. Fix in `audit-fixes-38a-d`. |
| H. GBP / AUD / NZD HistData not on Mac | LOW | when sessions +2 / +3 run | Need download before any backtest can run. ~16 GB per pair × 14 months. |
| I. AUDUSD engine header references "S56 promotion gate" but means S59 | TRIVIAL | next refresh of the engine header comments | Cosmetic; the 60% WR floor intent is correct. |

These are additive; the existing carry-forwards from `ENGINE_AUDIT_CHECKLIST.md` Outstanding #1-#10 still apply.

---

## 10. Open questions for the user before starting any of the above

1. **HistData download for GBP/AUD/NZD.** Confirm the choice of vendor (HistData ASCII vs Dukascopy CSV vs cTrader pull) before session +2. Recommendation: HistData ASCII for pipeline symmetry. ~16 GB per pair.
2. **EURUSD harness reproduction (8.1).** This needs ~2–3 hours of focused work and produces no new shadow data — it's pure backstop work. Confirm priority before starting; the alternative is to leave EURUSD in shadow indefinitely until it accumulates 30 trades the slow way.
3. **Walk-forward gate uniformity.** Confirm that using the S59 USDJPY thresholds (Section 3.1) for all four FX pairs is acceptable, or whether you want pair-specific thresholds. Recommendation: use S59 for walk-forward, then pair-specific for shadow→live promotion (Section 3.4).

End of audit doc.
