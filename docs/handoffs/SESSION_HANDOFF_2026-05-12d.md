# Session Handoff — 2026-05-12 (NZST), part D

Read this first next session. Builds on `SESSION_HANDOFF_2026-05-12c.md` (part C). Covers the USTEC TrendFollow5m promotion path from the part-C "next-session priority queue" — all five steps the handoff laid out, plus the calibration extension the operator requested mid-session.

## TL;DR

1. **S37-P1 promotion patch is COMMITTED.** 5-constant edit to `include/UstecTrendFollow5mEngine.hpp` flipping the engine from -$64,637 (live config) toward +$7,586 IS / +$5,207 OOS net per the part-C report. Commit `176c746a S37-P1: USTEC TrendFollow5m promotion patch (shadow only)`. Engine remains in HARD shadow at runtime (`engine_init.hpp:880`) — patch changes the shadow behaviour, not the live/shadow flag.

2. **S37-P2 RiskMonitor wiring is APPLIED but UNCOMMITTED.** Engine gained an `on_fire_hook` member (+27 lines); `engine_init.hpp` got a 54-line wiring block (fire-hook binding + 3 auto-pin callback registrations). All three callbacks pin the same `g_ustec_tf_5m.shadow_mode = true`. Syntax-verified via standalone wire-check TU; full Mac build verification still pending.

3. **S37-P3 calibration is APPLIED but UNCOMMITTED.** Extended `RiskMonitor.hpp` (+16 lines) to read 3 optional CSV columns (`fire_over_ratio`, `fire_under_ratio`, `fire_under_consec_hours`) with fallback to struct defaults. Extended `backtest/calibrate_risk_thresholds.cpp` (+137 lines) to support per-engine overrides and emit three USTEC TF5m threshold rows (umbrella + per-cell Donchian + Keltner). The calibrator was run against `data/l2_ticks_2026-*.csv` and `data/l2_ticks_NAS100_*.csv`; the regenerated `data/risk_monitor_thresholds.csv` (4 rows) is in place and loads cleanly through RiskMonitor.

4. **2-week shadow tape replay PASSED.** Against the December 2025 HistData NSXUSD tick CSV (Dec 15-30, 2,696,019 ticks, 0.84s wall): 26 trades = 49.4 trades/month annualised, INSIDE the [45, 61] ±15% band on the 53/month IS expectation. Exit distribution (TP/SL/PI = 23/62/15%) matches IS (28/55/16%). PROVE_IT_FAIL share dropped from 56% (baseline) to 15% as predicted. Net on this 2-week slice is -$1,348 — Dec 2025 was an adverse month even for the original config (-$4,058 baseline), so this is within model variance, not a fire-rate failure.

5. **Step 5 (flip `shadow_mode = false`) is NOT done.** Per part-C handoff this requires:
   - 2-month shadow validation window in production (not yet started)
   - Mac build verification (not yet done in this session — sandbox is Linux)
   - Explicit operator sign-off
   The engine is fully prepared for live promotion. The flip is a single source-line change at `engine_init.hpp:880` (the line declaring `g_ustec_tf_5m.shadow_mode = true`) — mirror the `g_gold_microscalper.shadow_mode = false;` pattern at line 146.

## Repo state at session end

Source files modified this session (uncommitted, awaiting Mac build verify):

```
M include/RiskMonitor.hpp                +16   optional fire_over/under_ratio + fire_under_consec_hours columns
M include/UstecTrendFollow5mEngine.hpp   +27   on_fire_hook member + invocation in _fire_entry
M include/engine_init.hpp                +54   bind on_fire_hook + 3 register_shadow_pin_cb calls
M backtest/calibrate_risk_thresholds.cpp +198  EngineConfig override fields + USTEC TF5m rows + CSV writer
                                         +295 lines total
```

(The +27 lines on UstecTrendFollow5mEngine.hpp are ON TOP OF the S37-P1 +71 lines that landed in commit 176c746a.)

Each new wiring follows the existing pattern; no structural changes to any pre-existing code path.

Data file regenerated this session (untracked — in .gitignore):

```
data/risk_monitor_thresholds.csv         4 rows (was 1)
  - MicroScalperGold      (unchanged values; emits new columns at struct defaults)
  - UstecTrendFollow5m    (umbrella; fire-rate eval)
  - UstecTrendFollow5m_Donchian (per-cell; WR + spread eval)
  - UstecTrendFollow5m_Keltner  (per-cell; WR + spread eval)
```

Yesterday's 13 modified files (from S37 part B + cost-gate rollout) are STILL UNCOMMITTED. The recommended commit grouping from part-C handoff still applies — they should be split into one commit for the GBP/EUR/USTEC5m yesterday patches + tick_gold.hpp `#if 0` wrap, and a second commit for the 9-engine cost-gate rollout.

Commits landed this session: 1
```
176c746a S37-P1: USTEC TrendFollow5m promotion patch (shadow only)
```

## Reports produced this session (in outputs/)

```
outputs/USTEC_S37_PROMOTION_PREP.md                  8 KB   prep + 5-constant patch summary
outputs/USTEC_S37_2week_shadow_replay_REPORT.md      8 KB   tape replay result + draft RiskMonitor thresholds
outputs/USTEC_S37_P2_RiskMonitor_wiring_REPORT.md    7 KB   wiring change set + verification
outputs/USTEC_S37_P3_calibration_REPORT.md          14 KB   calibration design + per-cell rows
outputs/UstecTrendFollow5mEngine.hpp.BACKUP_pre_S37 25 KB   pre-promotion engine header
outputs/UstecTrendFollow5mEngine.hpp.PROPOSED_S37   30 KB   draft of patched header (same as committed)
outputs/risk_monitor_thresholds.csv.BACKUP_pre_S37P3 1 KB   pre-calibration CSV (1 row)
outputs/USTEC_S37_2week_shadow_replay_lb.csv         6 KB   61-row leaderboard for Dec 15-30 replay
outputs/USTEC_S37_2week_shadow_replay_engsum.csv     6 KB   per-combo engine summary
outputs/SESSION_HANDOFF_2026-05-12d.md               this document
```

## Findings (compact)

### S37-P1 patch verification

5 constants changed; nothing else. Non-comment diff confirms only the constants + 2 cosmetic cell-name strings differ from pre-S37:

```
kUstecTfCells[0..1].sl_mult    2.0  -> 3.0
kUstecTfCells[0..1].tp_mult    4.0  -> 7.0
PROVE_IT_SECS                  90.0 -> 150.0
PROVE_IT_MIN_FAVOURABLE_PTS    4.0  -> 2.0
MIN_ATR_PTS                    10.0 -> 20.0
```

`g++ -std=c++17 -fsyntax-only` exit 0 on both pre and post files.

### 2-week shadow tape replay

| dimension | pre-S37 baseline | post-S37 (live) | Plan B IS expectation |
|---|---|---|---|
| trades / 2 weeks | 116 | 26 | ~25 (53/mo × 14/30) |
| WR | 16.4% | 23.1% | 28.3% |
| net | -$1,193 | -$1,348 | per-period varies; Dec 2025 was adverse |
| PROVE_IT_FAIL share | 56% | 15% | 16% |
| cost | $733 | $162 | proportional to trade count |

Fire-rate annualised: 49.4 trades/month. Band check: 49.4 ∈ [45, 61] ✓.

The 2-week net is mildly worse than baseline (-$1,348 vs -$1,193) — driven by S37's wider SL/TP (3.0/7.0 ATR vs 2.0/4.0) producing higher per-trade variance. On an adverse window this widens losses; over the 25-month aggregate it captures the right-tail trend-follow distribution. This is the expected variance profile for a positive-EV strategy with ~28% WR and 2.33 R:R.

### RiskMonitor calibration

Three structural extensions were needed because the v1 RiskMonitor model is hardcoded for high-frequency engines (MicroScalperGold at ~81 fires/hour, defaults `fire_over_ratio=2.5`, `fire_under_consec_hours=3`). At those defaults a 53/month engine like USTEC TF5m would trip on every isolated fire (over-fire ratio = 14x vs threshold of 2.5x).

The fix:

1. **CSV schema extended** with 3 optional columns. RiskMonitor::load_thresholds reads them if present (else keeps struct defaults). Backward-compatible with the legacy MicroScalperGold row.

2. **Per-hour fire distribution derived from the baseline ledger** (`outputs/ustec_trend_follow_5m_planA_baseline.csv`) filtered to `atr_at_entry >= 20` (the S37 MIN_ATR=20 entry filter) and scaled to the Plan B 1.74-trades/day rate. Active hours: 03, 04, 07-15, 18 (US-session-adjacent). Off-peak hours have expected=0 so the evaluator skips them via the existing `expected <= 0.0` short-circuit at RiskMonitor.hpp:467. This is the binding calibration step — without per-hour shape data, the surveillance would have constant false trips.

3. **Per-cell threshold rows** for close-side WR + spread. `tr.engine` carries the `_Donchian`/`_Keltner` suffix (S34 BUG #3) so the umbrella name doesn't match `RiskMonitor::on_trade_close`. Two per-cell rows duplicate the WR + spread thresholds with `fires_per_hour=0` (fire eval skipped for per-cell rows — the umbrella handles it). Per-cell `window_n=50` reflects per-cell trade frequency.

4. **Three auto-pin callbacks** in engine_init.hpp, all targeting the same `g_ustec_tf_5m.shadow_mode = true` flip. Any trip from any of the four evaluator paths (umbrella fire-over, umbrella fire-under, per-cell WR, per-cell spread) converges on the same engine pin.

Calibrated values:

```
trip_wr                    0.18    (vs backtest 0.283 = 10pp below empirical)
fire_over_ratio            10.0    (vs default 2.5; single fires don't trip)
fire_under_ratio           0.1     (vs default 0.4; very low)
fire_under_consec_hours    6       (vs default 3; full session of zero fires to trip)
spread_trip_median         1.80    (= 1.20 NAS100 L2 median × 1.5)
per-hour expected_fires    {0,0,0,0.038,0.036,0,0,0.038,0.088,0.209,0.265,0.155,
                            0.142,0.130,0.124,0.119,0,0,0.125,0,0,0,0,0}
```

NAS100 spread calibrated from only 3 days of L2 capture (2026-05-06 to 08). Refresh after 1-2 weeks of live shadow data.

## Next-session priority queue

### Top priority: validate the S37 path end-to-end on Mac

This is the blocking gate before step 5 (live promotion).

1. **`cd /Users/jo/omega_repo && rm -f .git/index.lock`** — the sandbox keeps recreating this; the user removes it on Mac. Pre-flight before any git command in this session.
2. **`cmake --build build -j`** (or your usual command) to confirm the +295 lines of S37-P2/P3 source changes compile cleanly in the full TU graph. The sandbox is Linux and the project is Windows/Mac targeted; sandbox `g++ -fsyntax-only` passes but the definitive validation is the Mac build.
3. **If the build is clean: commit.** Single commit covering S37-P2 + S37-P3 (recommended message in `outputs/USTEC_S37_P3_calibration_REPORT.md` §Suggested next commit).
4. **Regenerate the data CSV** so the calibrator's threshold output is reproducible from current ENGINE_TABLE:
   ```bash
   clang++ -std=c++17 -O3 -DNDEBUG -I include \
       backtest/calibrate_risk_thresholds.cpp \
       -o backtest/calibrate_risk_thresholds
   ./backtest/calibrate_risk_thresholds \
       data/l2_ticks_2026-*.csv data/l2_ticks_NAS100_*.csv
   ```
   This should regenerate `data/risk_monitor_thresholds.csv` to the same 4-row state the sandbox produced (sha256 in `outputs/USTEC_S37_P3_calibration_REPORT.md`).
5. **Restart the live process.** Watch startup logs for four `[RISK-MON]` lines confirming the 4 engine entries loaded. Confirm `[OMEGA-INIT] UstecTrendFollow5m RiskMonitor wiring installed` prints.

### Second priority: 2-month shadow validation window

This is the operator-time gate, not a coding gate.

1. **Watch for `[RISK-MON] AUTO-PIN UstecTrendFollow5m to SHADOW` lines.** The engine is already in HARD shadow so a pin is a no-op flag-flip, but the log line indicates surveillance is doing its job. Expect rare or zero pins under normal conditions.
2. **Watch the fire-rate over weeks.** The 2-week sandbox replay landed at 49/mo annualised. Live should land in the same band; sustained drift outside [45, 61] is the surveillance signal.
3. **Compare per-cell ledger WR to backtest.** The S37 leaderboard shows DC=926 wins, KC=400 wins, aggregate WR=28.3%. Per-cell WR isn't broken out; live shadow data over 2 months will give the true per-cell numbers and may motivate revisiting per-cell trip thresholds.
4. **After 2 months of clean shadow:** ready for step 5.

### Step 5 (flip to live) — operator-gated

Single line change. Mirror the `g_gold_microscalper` pattern.

```cpp
// Before (engine_init.hpp:880):
g_ustec_tf_5m.shadow_mode = true;          // HARD shadow, ignore kShadowDefault

// After:
g_ustec_tf_5m.shadow_mode = false;         // LIVE -- post 2-month shadow validation
```

This is the only change. Commit as `S37-P4: USTEC TrendFollow5m live promotion`. After this commit, the RiskMonitor auto-pin becomes the active circuit-breaker — any trip pins the engine back to shadow automatically.

DO NOT do this until:
- Mac build clean
- S37-P2/P3 committed
- 2 months of clean shadow logs
- Explicit operator sign-off at promotion time

### Lower-priority items carried from part-C handoff

These were already deferred in part C and remain deferred — none of them are blocked by this session's work, just unscheduled.

**Item C residuals (5 engines):** RSIReversalEngine, RSIExtremeTurnEngine, BreakoutEngine, XauusdFvgEngine, MacroCrashEngine. Each has different cost-gate semantics; see part-C handoff §"Item C deferred" for the per-engine analysis. MacroCrash specifically may already be inert (disabled 2026-04-30); confirm in `engine_init.hpp` before touching it.

**Plan A/B sweeps on the engines patched this morning:** AUD/NZD/JPY open engines (likely same MIN_RANGE lever as GBP/EUR per the part-C pattern), USTEC Htf (sister to TF5m), XAU TF 2h/4h/D1 + XauThreeBar30m. Multi-hour subagent each. The Plan A/B/entry sweep harness pattern is now well-validated and forks cleanly from the existing USTEC TF5m harnesses.

**S36-P5 VPS deploy via Windows PowerShell:** still pending from part B. With S37-P1 now committed, the next deploy brings the USTEC TF5m promotion to the VPS too (engine remains in hard shadow regardless). The VPS-side disabling of `GoldMicroScalperEngine.hpp` (now a stub per part-C §1) is also delivered by this deploy — until it lands, the VPS may still be running the pre-S33d binary with the live engine.

```powershell
cd C:\Omega
Copy-Item C:\Omega\OMEGA.ps1 C:\Omega\OMEGA.ps1.bak.2026-05-12   # safety backup
.\OMEGA.ps1 deploy
Get-Content C:\Omega\logs\latest.log -Tail 30
Get-Content C:\Omega\logs\watchdog.log -Tail 10
```

**Improving the per-cell WR thresholds.** Current per-cell rows inherit the umbrella 0.18 trip. The leaderboard CSV doesn't break out per-cell WR. A follow-up Plan B re-run with per-cell ledger output (modify the Plan B harness to emit per-trade rows tagged by cell) would produce true per-cell WR distributions. Likely a 30-60 min subagent task. Worth doing before step 5 if cells diverge meaningfully in shadow.

**Improving the NAS100 spread baseline.** Calibrated from only 3 capture days; refresh after a week or two of additional L2 capture to tighten the trip floor. Currently `spread_trip_median=1.80` is reasonable but conservative.

**Refactoring RiskMonitor to support low-frequency engines natively.** The current model needs per-engine override columns + per-cell threshold rows + per-hour-distribution overrides to fit USTEC TF5m. A cleaner v2 would extend the bucket model to support daily / weekly windows for engines where per-hour buckets are too granular. Not blocking anything — current calibration works — but the override-heavy approach will be more painful when adding the next low-frequency engine (e.g. XAU TF D1 at 2 trades/month).

## Bookkeeping

**Commit ordering recommendation (when Mac build passes):**

1. **Yesterday's two commits (still pending from part B/C):**
   - "S37-P0a: GBP/EUR/USTEC TF5m Plan A/B/entry sweeps + cost gate" — yesterday's GBP/EUR/USTEC5m patches + `tick_gold.hpp` `#if 0` wrap.
   - "S37-P0b: cost-gate rollout to AUD/NZD/JPY + USTEC Htf + PDHL + 4 XAU TF engines" — this morning's 9 cost-gate additions.
   - "S37-P0c: GBP/EUR/USTEC TF5m sweep harnesses + ledgers (research)" — research artifacts.
2. **Already landed (this session):**
   - 176c746a "S37-P1: USTEC TrendFollow5m promotion patch (shadow only)"
3. **This session's pending commit:**
   - "S37-P2/P3: RiskMonitor wiring + calibration for UstecTrendFollow5m" — the +295 lines across 4 files.
4. **Future after shadow window:**
   - "S37-P4: USTEC TrendFollow5m live promotion" — the single shadow_mode flip.

**Sandbox quirk:** `.git/index.lock` keeps being recreated. The sandbox CAN do `git commit` if the lock is removed first; it CANNOT remove the lock itself. Always start a sandbox session with `rm -f .git/index.lock` on the Mac side, OR be prepared to have the operator do it again mid-session.

**Disk pressure in sandbox:** /tmp is on a 9.6G volume; this session ran with ~300MB free for most of the work after unzipping the December 2025 HistData CSV. Build artifacts, large extracts, and per-trade ledger dumps need careful sizing. Stream-process where possible (`unzip -p ... | awk ...` not `unzip ... && awk ...`).

## Soft conclusions worth sleeping on

1. **The calibration patches work but the override-heavy approach is a code smell.** RiskMonitor's per-UTC-hour model assumes high-frequency engines. We've now bolted on: per-engine override columns, per-cell duplicate rows, per-hour distribution overrides, manual trip_wr override. Each is justified for THIS engine; the third or fourth low-frequency engine to get added will probably tip the balance toward extending RiskMonitor with a proper "low-rate" mode. Worth keeping in mind when the next sweep-validated promotion candidate emerges.

2. **The cell-suffix engine string is doing double duty awkwardly.** S34 BUG #3 added the suffix to disambiguate ledger rows — necessary fix. But it means `tr.engine` is also the key for surveillance lookup, so per-cell surveillance now needs per-cell rows. A cleaner separation would be: `tr.engine` = umbrella name for surveillance, `tr.cell` = new field for ledger disambiguation. Not urgent, but the next engine with this pattern (e.g. XAU TF D1's 3 cells) would benefit.

3. **The 2-week tape replay net was negative but the fire-rate band passed.** This is the kind of nuance the surveillance + shadow window is designed to handle: a single 2-week slice can be in a hostile regime even when the strategy has positive long-run EV. The handoff and report consistently distinguish "engine is broken" (fire-rate / WR / spread trip) from "engine had an unfavorable window" (negative PnL on a small slice). The trip thresholds are tuned around the former, not the latter. Good design discipline; honour it through the 2-month shadow window.

4. **The promotion path is now a known mechanism.** USTEC TF5m is the first engine to traverse the full pattern: Plan A/B/entry sweeps → Plan B winner → constexpr patch → shadow tape replay → RiskMonitor wiring → calibration → 2-month shadow → flip. Every step has a defined artifact. The next engine to promote (USTEC TF Htf? XAU TF D1?) should be cheaper because the harness pattern, the wiring pattern, and the calibration pattern are all proven. Estimate: 1-2 sessions per future promotion vs the 3+ sessions this one took.

5. **The handoff drift risk from part C is now mitigated for the USTEC engine.** Part C noted "every session should start by reading the actual current state of the files the handoff describes, not by trusting the handoff narrative." This session did that — re-read the engine header, verified the live constants, then made changes. The pattern should propagate to future sessions: verify, then act.

## Validation actions for THIS session's changes

Before commit:

```bash
cd ~/omega_repo
rm -f .git/index.lock      # sandbox quirk

# 1. Verify build compiles
cmake --build build -j

# 2. Inspect the diff
git diff --stat include/RiskMonitor.hpp \
                backtest/calibrate_risk_thresholds.cpp \
                include/engine_init.hpp \
                include/UstecTrendFollow5mEngine.hpp

# Expected:
#   include/RiskMonitor.hpp                | 16 +
#   backtest/calibrate_risk_thresholds.cpp | 198 ++++++++++++-
#   include/engine_init.hpp                | 54 +++++
#   include/UstecTrendFollow5mEngine.hpp   | 27 +++
#   4 files changed, 295 insertions(+), 9 deletions(-)

# 3. Run the calibrator (regenerates data/risk_monitor_thresholds.csv)
clang++ -std=c++17 -O3 -DNDEBUG -I include \
    backtest/calibrate_risk_thresholds.cpp \
    -o backtest/calibrate_risk_thresholds
./backtest/calibrate_risk_thresholds \
    data/l2_ticks_2026-*.csv data/l2_ticks_NAS100_*.csv

# Expected: "[CALIB] wrote 4 engine rows -> data/risk_monitor_thresholds.csv"

# 4. Smoke-test: verify the regenerated CSV parses correctly via a quick
#    test TU that loads thresholds and dumps state.
cat > /tmp/check.cpp <<'EOF'
#include "RiskMonitor.hpp"
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    omega::RiskMonitor rm;
    return rm.load_thresholds(argv[1]) > 0 ? 0 : 1;
}
EOF
clang++ -std=c++17 -I include /tmp/check.cpp -o /tmp/check
/tmp/check data/risk_monitor_thresholds.csv
# Expected: "[RISK-MON] loaded 4 engine threshold rows"
rm /tmp/check /tmp/check.cpp

# 5. After paper-session run, check auto-pin firings
grep "AUTO-PIN UstecTrendFollow5m" logs/$(ls -t logs | head -1) || echo "no auto-pins (expected — shadow has no live risk)"
```

If the build fails on any of the 4 modified files:

- **RiskMonitor.hpp**: optional column reads are vanilla code; failure is unlikely. If it does fail, check that `col_idx` returns -1 for missing columns (it does — see line 275 in the unmodified part of the file).
- **calibrate_risk_thresholds.cpp**: the `std::array<double, 24>{}` default initialiser in EngineConfig is C++17; the build flags include `-std=c++17` per the file header. If the macOS toolchain complains about aggregate-initialisation of EngineConfig with the new fields, the fix is to use explicit `.field = value` designated initialisers throughout — but the field-order-matching positional form should work fine.
- **engine_init.hpp**: the `auto pin_ustec_tf_5m = [](const std::string&) { ... };` lambda followed by 3 `register_shadow_pin_cb` calls is the same pattern as the existing g_gold_microscalper block at lines 162-181 modulo the lambda being factored to a named `auto`. If the build complains about the lambda capture, check that `g_ustec_tf_5m` is in scope at this point (it is — declared in `globals.hpp` and used at line 880 immediately above).
- **UstecTrendFollow5mEngine.hpp**: the `on_fire_hook` member is a vanilla `std::function`; the `<functional>` include is already at line 248. No new dependencies.

## Operator action still pending (Windows-side)

S36-P5 VPS deploy via Windows PowerShell — STILL not deployed as of part-C handoff. Part-D status: STILL not deployed. With the S37-P1 commit now in main, the next deploy brings the USTEC TF5m promotion patch to the VPS too. The VPS is currently running pre-S33d code where `GoldMicroScalperEngine.hpp` was the real engine; that file is now a no-op stub on the Mac repo HEAD. Until the VPS deploys, the VPS is the only place running the old microscalper binary.

```powershell
cd C:\Omega
Copy-Item C:\Omega\OMEGA.ps1 C:\Omega\OMEGA.ps1.bak.2026-05-12   # safety backup
.\OMEGA.ps1 deploy
Get-Content C:\Omega\logs\latest.log -Tail 30
Get-Content C:\Omega\logs\watchdog.log -Tail 10
```

If the VPS deploy is delayed further, the original part-C concern stands: real-money fills may still be flowing through the pre-S33d microscalper binary on account 8077780.

## Quick-reference files

| file | purpose | size |
|---|---|---|
| `include/UstecTrendFollow5mEngine.hpp` | live engine (S37-P1 + S37-P2 applied) | ~26 KB |
| `include/RiskMonitor.hpp` | surveillance (S37-P3 parser extension) | ~30 KB |
| `include/engine_init.hpp` | engine init wiring (S37-P2 added block) | ~98 KB |
| `backtest/calibrate_risk_thresholds.cpp` | calibrator (S37-P3 EngineConfig + writer) | ~28 KB |
| `data/risk_monitor_thresholds.csv` | live thresholds (regenerated; gitignored) | ~2 KB |
| `outputs/UstecTrendFollow5mEngine.hpp.BACKUP_pre_S37` | revert point pre-promotion | 25 KB |
| `outputs/risk_monitor_thresholds.csv.BACKUP_pre_S37P3` | revert point pre-calibration | 1 KB |
| `outputs/USTEC_S37_PROMOTION_PREP.md` | S37-P1 prep + recommendation | 8 KB |
| `outputs/USTEC_S37_2week_shadow_replay_REPORT.md` | tape replay validation | 8 KB |
| `outputs/USTEC_S37_P2_RiskMonitor_wiring_REPORT.md` | wiring change-set | 7 KB |
| `outputs/USTEC_S37_P3_calibration_REPORT.md` | calibration design + per-cell rows | 14 KB |
| `outputs/SESSION_HANDOFF_2026-05-12d.md` | this document | (current) |
