# Session Handoff — 2026-05-12 (NZST), part C

Read this first next session. Builds on `SESSION_HANDOFF_2026-05-12b.md` (part B). Covers Item B (Plan A/B/entry sweeps on the 3 engines patched yesterday) and Item C (cost-gate rollout to the next batch of engines). Both completed in-session.

---

## TL;DR

1. **Item A was discovered to be moot.** `include/GoldMicroScalperEngine.hpp` is a no-op stub since 2026-05-11 S33d (original 952-line implementation is at `disabled_engines/GoldMicroScalperEngine.hpp.disabled_2026-05-11`). Setting `g_gold_microscalper.shadow_mode = false` at `engine_init.hpp:146` has no behavioural effect because every method on the stub returns immediately. The live-pin is decorative. No real money at risk through this engine on the Mac repo HEAD. **The VPS may still be running the pre-S33d binary** (S36-P5 deploy is still pending) — if so, the live engine is still firing real fills on the VPS until next deploy.
2. **Item B done.** All three engines patched yesterday (GBP, EUR, USTEC TrendFollow5m) had full Plan A / Plan B / entry-side sweeps run across 14–25 months of HistData tick CSVs. Wall-clock per engine landed at 10–17 minutes (not the multi-hour estimate) thanks to a monotonic-deque O(1) bracket scan that the first subagent reverse-engineered to fit the sandbox's 45-second bash timeout.
3. **The big find from Item B: USTEC TrendFollow5m flips from `-$64,637` to `+$5,207 OOS` with a 4-line constexpr config change.** First sweep-validated live-promotion candidate to emerge from any backtest in this codebase. See "Next-session priority queue" below.
4. **GBP and EUR remain structurally unprofitable** in the explored space. Best lever for both is entry-side `MIN_RANGE` tightening (81% / 91% loss reduction respectively), but no config crosses zero. Hold in shadow. Cost gate stays.
5. **Item C done for 9 additional engines.** Cost-gate rollout brought the total from 5 to 14 engines (yesterday's 3 + IndexFlowEngine + CrossAssetEngines = 5 before; this session added AudusdSydneyOpen, NzdusdAsianOpen, UsdjpyAsianOpen, UstecTrendFollowHtf, PDHLReversion, XauThreeBar30m, XauTrendFollow 2h/4h/D1). 5 cases deferred for architectural reasons (see "Item C deferred" below).

---

## Repo state at session end

**13 modified files (additive across all):**

```
M include/AudusdSydneyOpenEngine.hpp     +9   cost gate inserted before phase=PENDING
M include/EurusdLondonOpenEngine.hpp     +9   (yesterday's commit, unchanged)
M include/GbpusdLondonOpenEngine.hpp     +9   (yesterday's commit, unchanged)
M include/NzdusdAsianOpenEngine.hpp      +9   cost gate inserted before phase=PENDING
M include/PDHLReversionEngine.hpp        +11  cost gate inserted before pos = Position{...}
M include/UsdjpyAsianOpenEngine.hpp      +9   cost gate inserted before phase=PENDING
M include/UstecTrendFollow5mEngine.hpp   +11  (yesterday's commit, unchanged)
M include/UstecTrendFollowHtfEngine.hpp  +11  cost gate inserted before p.active = true
M include/XauThreeBar30mEngine.hpp       +11  cost gate inserted before pos.active = true
M include/XauTrendFollow2hEngine.hpp     +10  cost gate inserted before p.active = true
M include/XauTrendFollow4hEngine.hpp     +11  cost gate inserted before p.active = true
M include/XauTrendFollowD1Engine.hpp     +10  cost gate inserted before p.active = true
M include/tick_gold.hpp                  +17  (yesterday's #if 0 wrap of MidScalper dispatch)
                                         +137 lines total
```

Each gate follows the same pattern:

```cpp
// 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
{
    const double spread_pts = ask - bid;  // or use 'spread' if already in scope
    if (!ExecutionCostGuard::is_viable(
            "<SYMBOL>", spread_pts, tp_dist, lot, 1.5))
    {
        return;  // or phase=IDLE + reset brackets for FX engines
    }
}
```

Plus a top-of-file `#include "OmegaCostGuard.hpp"` after the existing engine includes.

**New research artifacts (untracked, may be committed as their own commit):**

```
backtest/gbpusd_london_open_planA_sweep.cpp
backtest/gbpusd_london_open_planB_sweep.cpp
backtest/gbpusd_london_open_entry_sweep.cpp
backtest/gbpusd_planA_baseline.csv          1112-trade ledger
backtest/gbpusd_planA_stats.csv
backtest/gbpusd_planA_leaderboard.csv       180-config Plan A
backtest/gbpusd_planB_leaderboard.csv       180-config Plan B
backtest/gbpusd_planB_oos.csv               4-month OOS validation
backtest/gbpusd_entry_sweep.csv             45-config entry leaderboard

backtest/eurusd_london_open_planA_sweep.cpp
backtest/eurusd_london_open_planB_sweep.cpp
backtest/eurusd_london_open_entry_sweep.cpp
backtest/eurusd_planA_baseline.csv          1001-trade ledger
backtest/eurusd_planA_stats.csv
backtest/eurusd_planA_leaderboard.csv       180-config Plan A
backtest/eurusd_planB_leaderboard.csv       320-config Plan B
backtest/eurusd_planB_oos.csv
backtest/eurusd_entry_sweep.csv             135-config entry leaderboard

backtest/ustec_trend_follow_5m_planA_sweep.cpp
backtest/ustec_trend_follow_5m_planB_sweep.cpp
backtest/ustec_trend_follow_5m_entry_sweep.cpp
outputs/ustec_trend_follow_5m_planA_baseline.csv
outputs/ustec_trend_follow_5m_planA_baseline_monthly.csv
outputs/ustec_trend_follow_5m_planA_stats_pt1.csv
outputs/ustec_trend_follow_5m_planA_stats_pt2.csv
outputs/ustec_trend_follow_5m_planA_leaderboard.csv
outputs/ustec_trend_follow_5m_planA_engsum_pt1.csv
outputs/ustec_trend_follow_5m_planA_engsum_pt2.csv
outputs/ustec_trend_follow_5m_planB_leaderboard.csv
outputs/ustec_trend_follow_5m_planB_engsum_pt1.csv
outputs/ustec_trend_follow_5m_planB_engsum_pt2.csv
outputs/ustec_trend_follow_5m_entry_leaderboard.csv
outputs/ustec_trend_follow_5m_entry_engsum_pt1.csv
outputs/ustec_trend_follow_5m_entry_engsum_pt2.csv
```

**Reports written this session (in `outputs/`, accessible via `computer://` links):**

```
outputs/GBPUSD_PLAN_A_B_REPORT.md                16 KB   GBP Plan A/B/entry findings
outputs/EURUSD_PLAN_A_B_REPORT.md                20 KB   EUR Plan A/B/entry findings
outputs/USTEC_TREND_FOLLOW_5M_PLAN_A_B_REPORT.md 24 KB   USTEC live-promotion candidate
outputs/SESSION_HANDOFF_2026-05-12c.md           this document
```

**Failed cleanup:** `rm include/OmegaCostGate.hpp` failed in the sandbox (read-only delete permission). Operator action required on Mac:

```bash
rm /Users/jo/omega_repo/include/OmegaCostGate.hpp
```

---

## Item B findings (compact)

### GBPUSD London-Open

- 16 months tape (2025-01 to 2026-04, ~32M ticks).
- Baseline: 931 trades, WR 62.19%, **net −$4,159**.
- Best Plan A (MAX_SPREAD=0.00020, TRAIL_FRAC=0.15, MFE_TRAIL_FRAC=0.20): **net −$3,669** (12% loss reduction).
- Best Plan B (TP=2.0, SL=0.60, TF=0.20, MFE=0.40): **net −$3,709** (11%).
- Best entry sweep (LOOKBACK=300, MIN_RANGE=0.0015, MAX_RANGE=0.0050): **net −$773** (81% reduction, 5x fewer trades).
- OOS (4-month held-out): clean, no overfit.
- **Recommendation:** hold in shadow. Cost gate stays.
- **Trail-binding:** both `range*TF` and `mfe*MFE` bind, but TF dominates MFE by ~2x at live MFE=0.40. Opposite of MidScalper where MFE was inert.

### EURUSD London-Open

- 14 months tape (2025-03 to 2026-04, ~25.3M ticks).
- Baseline: 1,001 trades, WR 58.94%, **net −$4,047**.
- Best Plan A (MAX_SPREAD=0.00030, TF=0.55, MFE=0.70): **net −$3,590** (11%).
- Best Plan B (TP=2.5, SL=1.00, TF=0.55, MFE=0.70): **net −$3,272** (19%). SL_FRAC=1.00 dominates — opposite of GBP where SL=0.60 won.
- Best entry sweep (LOOKBACK=300, MIN_RANGE=0.0015, MAX_RANGE=0.0075, MIN_BREAK=5): **net −$375** (91% reduction, 9x fewer trades). Every top-9 has MIN_RANGE=0.0015 (raised from EUR's live 0.0008).
- OOS: family-coherent (all top-5 OOS have TF=0.55, MFE in {0.55, 0.70}, SL in {0.90, 1.00}) but point-divergent. Mild overfit on point-rankings; family signal generalises.
- **Recommendation:** hold in shadow. Cost gate stays.
- **Trail-binding:** *third pattern*. At TF in {0.20, 0.30}, MFE is inert (~$100 spread). MFE only becomes load-bearing at TF >= 0.45 (~$500 spread). TF only becomes load-bearing at MFE >= 0.55. Both bind, but only in the upper-right corner. EUR wants *loose* trails (upper-right); GBP wanted *tight* trails (lower-left). Sister engines, opposite directions — **per-symbol divergence required if trail tuning ever promotes.**

### USTEC TrendFollow5m

(See the next section for the operative summary; deep details in `outputs/USTEC_TREND_FOLLOW_5M_PLAN_A_B_REPORT.md`.)

---

## Next-session priority queue

### TOP PRIORITY — USTEC TrendFollow5m live-promotion candidate

Quoting the operator note for next-session attention:

> USTEC TrendFollow5m (25 mo, 5,989 baseline trades): structurally negative on live config, structurally positive on tuned config. Baseline −$64,637. Plan A best (right-sizing the prove-it timer and TP/SL multipliers) reaches −$1,805 (97.2% loss reduction, gross flips from −$15k to +$17k). Plan B adds a MIN_ATR=20 regime filter and flips to +$7,586 IS / +$5,207 OOS over a 4-month held-out window. Entry sweep adds marginal lift to +$8,393 IS / +$7,632 OOS. Subagent's recommendation: 2-month shadow validation then live promotion via a 4-line constexpr edit (sl_mult, tp_mult, PROVE_IT_SECS, PROVE_IT_MIN_FAVOURABLE_PTS, MIN_ATR_PTS). OOS rate +$1,302/month.
>
> This changes the priority order. USTEC is the first credible live-promotion candidate to emerge from any sweep in this codebase. Item C (cost-gate rollout to 30 more engines) is still on the queue, but you may want to act on USTEC first while the analysis is fresh.

**Action items for the USTEC promotion path (next session, with operator authorisation):**

1. Read `outputs/USTEC_TREND_FOLLOW_5M_PLAN_A_B_REPORT.md` in full and confirm Plan B's recommended constants. Cross-check the OOS leaderboard CSV (`outputs/ustec_trend_follow_5m_planB_leaderboard.csv`) and the engsum splits.
2. Draft the 4-line constexpr patch to `include/UstecTrendFollow5mEngine.hpp` — `sl_mult`, `tp_mult`, `PROVE_IT_SECS`, `PROVE_IT_MIN_FAVOURABLE_PTS`, `MIN_ATR_PTS`. DO NOT apply yet.
3. Run a 2-week tape replay against the patched engine in shadow mode to confirm fire-rate is comparable to backtest expectations.
4. After clean shadow tape, propose explicit live-promotion via `engine_init.hpp` (mirror the historical MicroScalperGold path), with operator sign-off in chat as a rule-4 invariant.
5. Wire `UstecTrendFollow5m` into `RiskMonitor` for fire-rate / WR / spread surveillance with auto-pin-on-trip (same callback pattern as `g_gold_microscalper`).

### Other open items

**Item C residuals (5 engines deferred from this session):**

- `RSIReversalEngine.hpp`, `RSIExtremeTurnEngine.hpp` — no fixed TP at fire time. The cost gate's `tp_dist * tick_usd_per_lot * lot >= cost * ratio` semantics don't apply. These engines exit dynamically via reversal / L2 flip signals. A different gate (MFE-expectation from historical bucket stats) would suit. Either build that or leave these engines un-gated.
- `BreakoutEngine.hpp` — multi-symbol templated engine with pyramid add-ons. TP comes from external `edge.tp_price`. Gate is doable but symbol parameter needs to thread through from the templated `symbol` member. Bracket+trail hybrid with multi-leg sizing complicates per-leg cost-floor evaluation.
- `XauusdFvgEngine.hpp` — `open_position()` is called with bar-close prices, so `bid`/`ask` aren't in scope at the fire site. Either thread spread through the call chain or use a cached class-member spread. ~30 min careful edit.
- `MacroCrashEngine.hpp` — handoff says disabled 2026-04-30 at 4.8% WR / −10,849pts. No `pos.active = true` pattern in source. Likely already inert. Confirm in `engine_init.hpp` and skip if disabled.

**Plan A/B-style sweeps on the engines patched this session:**

- AUD, NZD, JPY open engines (FX cohort sister to GBP/EUR). High likelihood the same MIN_RANGE entry-side lever applies. Same harness pattern as the GBP sweep — fork the GBP harness, swap symbol + tick path, sweep.
- USTEC TrendFollow Htf (sister to TF5m). Less obvious whether the same prove-it timer right-sizing applies on H1+ bars.
- XAU TrendFollow 2h/4h/D1 + XauThreeBar30m. Different signal classes; each needs its own harness. Multi-hour subagent each.

**Bookkeeping:**

- Decide commit grouping. Recommended: one commit for this session's 9 cost-gate additions ("S37-P1: cost-gate rollout to AUD/NZD/JPY + USTEC Htf + PDHL + 4 XAU TF engines"). Yesterday's GBP/EUR/USTEC5m patches + tick_gold.hpp `#if 0` wrap should already be a separate commit from part B.
- Decide whether to commit the research artifacts. Recommended: one commit "S37-P2: GBP/EUR/USTEC TF5m Plan A/B/entry sweep harnesses + ledgers (research; no live behavior change)".
- Delete `include/OmegaCostGate.hpp` on Mac (sandbox lacked delete permission). `rm /Users/jo/omega_repo/include/OmegaCostGate.hpp`.
- Update `outputs/ITEMS_1_2_3_REPORT.md` priority queue to reflect the 9 newly-gated engines. The "30 engines remaining" estimate is significantly over-scoped — the realistic remaining-to-gate set is the 5 deferred above plus whatever lives inside `CrossAssetEngines.hpp` that isn't already gated.

**Operator action still pending (Windows-side, independent of this session's work):**

S36-P5 VPS deploy via Windows PowerShell. Per the part-B handoff, still not deployed. With today's findings about `GoldMicroScalperEngine.hpp` being a stub on the Mac repo, this deploy now also brings the engine-disable to the VPS — so deploying it ends any remaining real-money exposure on account 8077780 if the prior live binary is still running there. **Most consequential decision still on the queue and not on Claude's side.**

```powershell
cd C:\Omega
Copy-Item C:\Omega\OMEGA.ps1 C:\Omega\OMEGA.ps1.bak.2026-05-12   # safety backup
.\OMEGA.ps1 deploy
Get-Content C:\Omega\logs\latest.log -Tail 30
Get-Content C:\Omega\logs\watchdog.log -Tail 10
```

---

## Validation actions for THIS session's changes

Before pushing the source-side changes anywhere:

```bash
cd ~/omega_repo

# 1. Verify build compiles with the 9 new cost-gate additions
cmake --build build -j        # or whatever your usual build command is

# 2. Inspect the diff
git diff --stat
git diff include/AudusdSydneyOpenEngine.hpp \
         include/NzdusdAsianOpenEngine.hpp \
         include/UsdjpyAsianOpenEngine.hpp \
         include/UstecTrendFollowHtfEngine.hpp \
         include/PDHLReversionEngine.hpp \
         include/XauThreeBar30mEngine.hpp \
         include/XauTrendFollow2hEngine.hpp \
         include/XauTrendFollow4hEngine.hpp \
         include/XauTrendFollowD1Engine.hpp

# 3. Remove the redundant wrapper file
rm include/OmegaCostGate.hpp

# 4. After running a paper session, check the cost-gate firing rate across the cohort
grep -c "COST-GUARD" logs/$(ls -t logs | head -1)
# Should be a small fraction (< 5%) of total fires in normal regime.
# If higher on any specific engine, lower its is_viable() ratio from 1.5 -> 1.25
# on a per-engine basis (each call site has the ratio as the last argument).
```

If the build fails on any of the 9 new engines:

- Most likely cause: missing transitive include for `OmegaCostGuard.hpp` in a TU that includes the engine but not its sibling includes. Add `#include "OmegaCostGuard.hpp"` near the top of `include/globals.hpp` in the engine includes section.
- Less likely: `lot` is not in scope inside the `_fire_entry` body of an XAU engine. All four were grep-confirmed to have `double lot = 0.01;` as a class member, but if a build error surfaces it'll point at the line.

Smoke-test results from this session (g++ -fsyntax-only against each modified header):

```
AudusdSydneyOpenEngine.hpp:     OK
NzdusdAsianOpenEngine.hpp:      OK
UsdjpyAsianOpenEngine.hpp:      OK
UstecTrendFollowHtfEngine.hpp:  OK
PDHLReversionEngine.hpp:        OK
XauThreeBar30mEngine.hpp:       OK
XauTrendFollow2hEngine.hpp:     OK
XauTrendFollow4hEngine.hpp:     OK
XauTrendFollowD1Engine.hpp:     OK
```

All clean at syntax level. Definitive validation still requires `cmake --build build` on Mac because the sandbox does not have cmake.

---

## Soft conclusions worth sleeping on

1. **The cost gate is a defensive floor, not an edge.** Across GBP/EUR sweeps (180+320+45 configs each), no cost-gate-adjacent tweak crosses zero. The real edge — when there is one — lives upstream in entry filtering or, for USTEC, in exit geometry (prove-it timer + ATR regime filter). The gate prevents bleeding on unviable setups; it does not create profit.

2. **The trail-binding pathology is symbol-specific, not architectural.** MidScalper had MFE-inert / TRAIL-binding. GBP wants tight trails (TF=0.20). EUR wants loose trails (TF=0.55). Sister engines, opposite tunes. Any future trail tuning must vary BOTH `TRAIL_FRAC` and `MFE_TRAIL_FRAC` in the same sweep, and the optimal config will diverge across symbols. Inheriting tuned values from one symbol to another (which is how EUR and GBP got their identical S56 defaults today) is structurally wrong.

3. **USTEC's positive flip is real but conditional on the regime filter.** Removing `MIN_ATR=20` from Plan B's best config pulls the net back toward Plan A's −$1,805. The 4-line patch is not separable: it is `sl_mult + tp_mult + PROVE_IT_SECS + PROVE_IT_MIN_FAVOURABLE_PTS + MIN_ATR_PTS` as a unit. Promote them together or not at all.

4. **The handoff drift between part A/B and the actual repo state was the biggest single risk this session.** Part A/B both described `g_gold_microscalper` as the one live engine; the actual file on disk was a stub. If today's work had blindly added a cost gate to the stub's no-op `on_tick`, ~30 min of work would have been wasted. The lesson: every session should start by reading the actual current state of the files the handoff describes, not by trusting the handoff narrative. The same drift risk applies to this document — next session, re-verify everything claimed here against `git status` and direct file reads.

5. **The sandbox 45-second bash timeout is now a known constraint.** Any future backtest harness needs O(1) per-tick algorithms or per-month chunking. Backgrounding doesn't survive. This forced one of the most useful engineering decisions of the session (monotonic-deque optimization on the bracket scan, ~50x speed-up).
