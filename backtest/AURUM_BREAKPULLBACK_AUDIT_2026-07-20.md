# AURUM-BREAK-PULLBACK-V1 — Adversarial Audit + Honest Backtest

Date: 2026-07-20 (S-2026-07-20af)
Auditor: Omega research session (RESEARCH ONLY — not wired, not deployed, no live box touched)
Engine drop: external, self-contained C++20 (`backtest/aurum/AurumBreakPullback.cpp`, 2114 lines)
Verdict: **DEAD** (structurally cannot trade as shipped; underlying edge is net-negative when forced to trade)

---

## 1. Headline

The engine **cannot open a single trade as shipped** — a fatal state-machine bug makes the entire
pullback-entry / position-management / partial / runner / stop / DD-lock machinery **unreachable dead
code**. On 46.5M ticks of certified-clean gold it books **0 trades**. After fixing the one-line bug,
the strategy trades but is **net-negative on every certification leg** (long, short, 1×, 2×, bull
regime, bear regime). It is not refinable into a viable book without a ground-up redesign of the entry
logic. It is also untestable in its *intended* two-venue form because **no minute/tick MGC data exists**.

---

## 2. Build result

- `clang++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic` — **clean, zero warnings, zero errors.**
- No compile blockers; no source edits needed to build. Binary runs, parses CSVs, streams 46M ticks
  in ~25s, writes ledger + summary. Engineering quality of the *plumbing* is high (streaming 1m
  aggregation, civil-date DST, no OOM). The defect is in the trading state machine, not the build.

---

## 3. Code-vs-README audit

### 3a. FATAL BUG — entry path is unreachable (engine can never trade)

`process_completed_5m()` sets, right after the regime check passes:

```cpp
state_ = EngineState::RegimeReady;      // executed on EVERY 5m bar once regime is OK
...
if (state_ != EngineState::WaitPullback) return;   // pullback/entry block guard
```

The breakout-arming block sets `state_ = WaitPullback` and **returns**. On the *next* 5m bar the
function re-enters, unconditionally overwrites `state_` back to `RegimeReady` at the top, and the
pullback block's guard `if (state_ != WaitPullback) return;` then fires — so the pullback-validation,
`EntryPending`, `open_position`, `manage_position`, partial, runner, and every stop/lock path are
**never reached**. Empirically proven with an instrumented build:

```
author defaults, as shipped:  setup_armed=9  entry_pending=0  → 0 trades
one-line guard added          setup_armed=10 entry_pending=6  → 2 trades
```

Fix that makes the path reachable (proven): `if (setup_.session == SessionId::None) state_ = EngineState::RegimeReady;`

**Consequence for the README:** every lifecycle claim (+1R floor, 40% partial at +1.25R, ATR runner
after +1.75R, failed-breakout/EOS exits, session/daily/DD locks) is *present in the source but
vacuously unimplemented at runtime* — none of it executes.

### 3b. Mis-calibrated filter — inert on gold even after the bug is fixed

`min_atr_to_cost_multiple = 5.0` requires the 15-minute ATR to exceed 5× the stressed round-trip cost:
- XAU stressed RT = max(1.5×9, 9+4) = **13.5bp** → gate demands 15m ATR ≥ **67.5bp = 0.675% of price**.
- Real gold 15m ATR ≈ 9–13bp (~0.1–0.2%). The gate is **~5–7× too high** for the instrument.

Funnel (bug-fixed, author defaults, 6mo): of 5562 regime-OK 5m bars, **5406 rejected on the XAU
ATR-cost gate**, leaving 156; the MGC side rejects 0 (its cost model is far cheaper). Net: ~2 trades /
6 months — statistically uncertifiable regardless of edge.

### 3c. Verified README claims (code IS present and correct, just unreachable)

- London + NY opening ranges (London local 07:00–07:30, NY 08:00–08:30): YES.
- 30m EMA(20/50)/VWAP long+short regime, requiring BOTH XAU and MGC to agree: YES.
- Cross XAU/MGC breakout confirm (both closes must break; both must trigger on entry minute): YES.
- Cheaper-venue routing (XAU bp vs MGC futures bp, MGC integer contracts, min-1): YES.
- Structural ATR stop with min cost clearance (`stop_bp = max(raw, min_stop_cost_multiple×stress, …)`): YES.
- +1R floor: arms at MFE ≥ 1.0R; floor PRICE = entry ± (stress_cost + 2bp) ≈ breakeven+ (NOT +1R — the
  R refers to the *trigger*, not the floor level; README wording is defensible but easy to misread).
- 40% partial at +1.25R, runner after +1.75R (chandelier = highest − 1.20×ATR): YES.
- 1 trade/session, 2/day, daily 0.5% / weekly 1.25% / account 3% DD locks (account-DD force-liquidates
  via RiskGovernor; others block new entries only): YES.

### 3d. Minor logic quirk (not a dishonesty)

Session flatten uses `new_york_clock >= 16:30` for **both** sessions, so a London trade rides until
NY 16:30 (≈ London 21:30/22:30), not its own session flatten. Cosmetic given the engine doesn't trade.

### 3e. Look-ahead check — CLEAN

No forward leakage found. Indicators/EMA/VWAP/opening-range are all built from completed bars up to
the current minute; breakout uses the just-*completed* 5m bar and entry is checked on the *following*
minute bar (genuine next-bar entry, causal). A position opened on a minute is not managed until the
next minute (no same-bar entry+exit optimism). Stops are checked before favourable-excursion updates
within a bar (the one genuinely conservative choice).

### 3f. HONESTY / FILL-OPTIMISM bugs (the session theme) — latent, confirmed in code + ledger

These are latent (the engine doesn't trade as shipped) but are the exact "booking at level, not real
fill" class flagged this session, and would inflate any future certification once 3a/3b are fixed:

1. **Stops book at the stop LEVEL + a fixed slippage, never gap-through.** `close_position(active_stop, …)`
   books the exit at `active_stop`, then `apply_exit_slippage` adds a *fixed* 2bp (XAU) / 1 tick (MGC).
   Ledger proof (forced-trading run): every INITIAL_STOP long books `exit = stop − 0.10` exactly (one
   MGC tick), e.g. stop 3975.006 → exit 3974.906, **regardless of how far the minute bar's low actually
   pierced**. On a gap-through the true fill is near the bar open/low, far worse. Results are optimistic
   on gap days by an unbounded amount capped only by the fixed slip.
2. **The +1R "floor" inherits the same flaw** — it moves the stop to ~breakeven and, on a gap through
   that floor, still books at floor − fixed-slip, hiding the real sub-BE tail (identical mechanism to
   the S-17f `book_mimic_stop_`-at-level tautology).
3. **Partial books at the exact +1.25R target price** (a resting-limit assumption) with *adverse* slip
   applied — mildly conservative for a limit, but the trigger is MFE-based (`highest` this bar), so a
   spike-and-reverse bar books 40% at +1.25R and the remainder at the floor in the same bar.
4. Entry books at `trigger ± fixed entry-slip` (stop-order-at-level) — same optimism on gap entries.

---

## 4. Data-integrity verdict (mandatory gate, per input file)

`backtest/data_integrity_gate.py` run on every candidate file:

| File | Rows | Span | Verdict |
|---|---|---|---|
| `/Users/jo/Tick/xau_6mo_corrected.csv` (XAU tick, timestamp,bid,ask ms) | 46,465,933 | 2025-11-02 … 2026-04-24 | **CERTIFIED CLEAN** |
| `/Users/jo/Tick/xau_2022bear_tick.csv` (XAU tick) | 17,834,719 | 2022-06-01 … 2022-09-30 | **CERTIFIED CLEAN** |
| `/Users/jo/Tick/2yr_XAUUSD_daily.csv` | 738 | — | **REJECTED** (1 backward ts, 154h jump — non-chronological). Not used. |

**MGC data gap (critical):** no minute-level or tick MGC exists anywhere in `/Users/jo/Tick`. Only
`mgc_2024_2026.h1.csv` (H1), `mgc_2024_2026.h4.csv` (H4), `mgc_30m_hist.csv` (30m), and a Yahoo daily.
The engine requires XAU **and** MGC aligned at exact minute timestamps for its opening range, 5m
breakout, cross-confirm, and venue routing. **The intended two-venue configuration cannot be tested
faithfully — the confirming/second venue has no minute data.**

**Proxy used (stated limitation):** MGC was proxied with the *same* certified XAU tick file
(MGC ≈ XAU, correlation ≈ 1). This makes the cross-market confirm **trivially satisfied** (it is
therefore NOT tested), and routes trades to the cheaper venue (MGC futures cost model at XAU prices —
a fair economic proxy). Results below reflect the **single-market ORB-pullback core only**; the
cross-market filter is untested for lack of data.

---

## 5. Honest backtest — certification legs

Costs: 1× = README example (XAU 6bp base +1/+2 slip; MGC $2.50 RT +1/+1 tick). 2× = doubled.
The engine's own gate (printed at run end): PF ≥ 1.35 @1×, ≥1.15 @2×, both halves +, ≥70% WF folds +,
net/maxDD ≥ 2.0 @1× / ≥1.0 @2×, **longs and shorts must pass independently**.

### 5a. As shipped (no edits), 1× cost, both regimes
**0 trades.** (Fatal bug 3a.) Nothing to certify.

### 5b. Bug 3a fixed only, author-default params, 6mo, 1×
**2 trades** in 6 months. Uncertifiable (n far too small; ATR-cost gate 3b). Not a result.

### 5c. Forced-trading probe — bug 3a fixed + gates relaxed (ATR-cost 5.0→1.0, pullback 0.20→0.60,
invalid-inside 0.15→0.80) so it trades enough to evaluate the underlying edge. **Most generous
configuration; still fails every leg:**

| Leg | Trades | Win% | PF | Net | avg R | net/DD |
|---|---|---|---|---|---|---|
| 6mo (bull) ALL 1× | 86 | 47.7 | **0.67** | −$2153 (−2.15%) | −0.17 | −0.93 |
| 6mo ALL 2× | 29 | 55.2 | 0.91 | −$190 | −0.05 | −0.26 |
| 6mo LONG 1× | 49 | 49.0 | **0.62** | −$1312 | −0.21 | −0.74 |
| 6mo SHORT 1× | 37 | 46.0 | **0.73** | −$836 | −0.12 | −0.67 |
| 2022 bear ALL 1× | 5 | 20.0 | 0.29 | −$536 | −0.55 | −0.94 |
| 2022 bear ALL 2× | 0 | — | — | $0 | — | — |
| 2022 bear LONG 1× | 2 | 0.0 | 0.00 | −$375 | −0.97 | −1.00 |
| 2022 bear SHORT 1× | 3 | 33.3 | 0.58 | −$160 | −0.27 | −0.42 |

Every leg with trades is **PF < 1.0 and net-negative.** Longs fail. Shorts fail. Both regimes fail.
Both cost levels fail. (2× trades fewer because higher stressed cost raises the min-stop floor and
sizing rejects more setups; 2022-bear 2× = 0.) Chronological-halves / WF folds are moot — no full-
sample leg clears even PF 1.0, so no split can rescue it.

---

## 6. VERDICT — DEAD

1. **As shipped it is inert** — a one-line state clobber (3a) makes every trade path unreachable; 0
   trades on 64M+ certified-clean gold ticks. All README lifecycle features are dead code at runtime.
2. **The intended two-venue design is untestable** — no minute/tick MGC data exists (4). Cross-market
   confirm and venue routing cannot be validated on the data on hand.
3. **The single-market core has no edge** — with the bug fixed and filters relaxed enough to trade, it
   is net-negative on every certification leg across two regimes and two cost levels (5c).
4. **Latent fill-optimism** (3f): stops/floors book at level + fixed slip, not gap-through — would
   inflate any future cert; must be fixed before any figure from this engine is trusted.

### If someone insists on trying to revive it (not recommended), the minimum work is:
- Fix 3a (guard the `RegimeReady` assignment) — mandatory or it never trades.
- Re-calibrate `min_atr_to_cost_multiple` to the instrument (~1.0–1.5 for gold, not 5.0), or express
  the ATR floor in ATR-multiples rather than ×stress-cost.
- Redesign the pullback-entry condition — the "dip to breakout AND close near the range extreme on the
  same 5m bar, else clear" test is near-unsatisfiable; only 6 of 261 armed setups (relaxed) ever
  produced an entry, and the resulting edge is negative anyway.
- Replace fill-at-level booking with worse-of gap-through fills (honest-ledger discipline) BEFORE
  re-certifying, or every result is optimistic.
- Source real minute/tick MGC before claiming the cross-venue thesis is tested at all.
- Even after all of the above, 5c says the underlying breakout-pullback edge on gold is negative — the
  refactor is a rewrite of the thesis, not a tune-up. Recommend **do not pursue.**

---

*Data: XAU tick only; MGC proxied by XAU (cross-confirm untested — no MGC minute data). Costs per
README example, 1× and 2×. Diagnostic/instrumented build kept in session scratchpad, not committed.
Tracked source copy in `backtest/aurum/` is the unmodified external drop under evaluation.*
