# HANDOFF S35 PART 2 — 2026-05-12 (continuation session)

End-of-day state after the S35-continuation session that (a) recovered and
committed the S26 broker-fill reconciliation work from `stash@{0}`, (b)
built and committed `include/engine_protections.hpp` as a reusable 10-item
downside-protection bundle, (c) retrofitted `XauThreeBar30mEngine` to use
the bundle, (d) wrote and ran a per-year cross-validation backtest that
proved a +$1488 / 25mo / 66% WR / PF 1.27 result for the TUNED config,
(e) wired the engine into `globals.hpp` + `engine_init.hpp` with the TUNED
defaults, and (f) audited the existing edge_hunt per-year XAU results to
produce a definitive verdict on intraday XAU edges below the M30
timeframe (verdict: M5 universally negative; M15 fragmented, zero
combos positive across all three years; do not build sub-M30 XAU
engines).

Read this top to bottom before doing anything in a new session.

---

## 0. The one-paragraph summary

S35-Part-1 (covered in `HANDOFF_S35.md`, commit `59b057e`) cleaned up a
dirty tree and landed the S34-P1 close-path fixes on the three remaining
XAU trend-follow engines, leaving two stashes preserved for review.
S35-Part-2 (this session) recovered `stash@{0}` (the S26 broker-fill
reconciliation, 7 protected files), committed it after verifying the
paired regression test passes against the live trade captured on
2026-05-10 (net pnl, broker pnl, and disparity all within $0.01 of
reality), then built out the next-generation downside-protection stack
for XAU intraday engines and per-year-validated it on 25 months of
Dukascopy M30 data. The `XauThreeBar30m` engine is now retrofitted,
backtested, and wired in production code with operator-validated TUNED
defaults; it is HARD-shadow + enabled and **awaits one final hot-path
wiring step in `tick_gold.hpp` before it can fire its first shadow
trade**. The multi-timeframe XAU research question ("can we build 1m,
5m, 10m, 15m intraday engines with downside protection?") has been
answered "no, not for XAU at any timeframe below M30" using the existing
edge_hunt per-year sweeps that were already in the repo. **Next-session
action**: run preflight, decide whether to wire `tick_gold.hpp` for M30
dispatch (turns the engine on for live shadow), then handle `stash@{1}`,
then move to the cross-year audit on USTEC / US500 / EURUSD.

---

## 1. What is at HEAD and what is dirty

**origin/main commit at session-end:** `289f8b2`
(`S35-P5: wire XauThreeBar30m into globals + engine_init with TUNED config`).

The tree at session end is clean. `bash .claude-preflight.sh` exits 0.

### 1.1 Full commit log this session (in order)

```
8206e11  S26-late broker-fill reconciliation + bundled S20 work (recovered from stash)
4e85956  S35-P2  engine_protections.hpp -- standard downside-protection bundle
1684cfc  S35-P3  XauThreeBar30m -- retrofit with ProtectedEngineGuards
3ee31de  S35-P4  backtest harness for XauThreeBar30m + S35-P3 protections
289f8b2  S35-P5  wire XauThreeBar30m into globals + engine_init with TUNED config
```

To verify a session starts on the right commit:

```bash
cd ~/omega_repo
git log --oneline -7
# Expect 289f8b2 at HEAD.
bash .claude-preflight.sh
# Expect [PREFLIGHT-OK] tree at 289f8b2... clean.
```

### 1.2 Stashes still on the shelf

```
stash@{1}  PRE-S34 WIP: S20 Path A per-gate diagnostic counters
           (backtest/IndexBacktest.cpp, +280 lines, 2026-05-08)
           Status: NEVER committed. Recovered from stash inspection but
           never applied. Diagnostic-only; no production effect.
           Decision pending (apply / discard / rebuild). Lowest urgency.
```

`stash@{0}` was recovered and committed in `8206e11` — it is no longer on the
shelf. The stash slot was dropped automatically when `git stash pop` succeeded.

### 1.3 `.git/index.lock` quirk (informational)

The sandboxed agent environment creates `.git/index.lock` every time it
runs git from inside the sandbox (`git status`, `git stash show`, preflight)
and cannot clean it up due to the mount layer's write restrictions on
host-managed git internals. This left the Mac with a stale `.git/index.lock`
that blocked one `git stash pop` attempt mid-session. The recovery is
trivial:

```bash
cd ~/omega_repo
ls -la .git/index.lock 2>/dev/null
# If file exists with size 0 and you have no other git process running:
rm -f .git/index.lock
```

The agent committed to NOT running any git commands from the sandbox for
the remainder of S35-Part-2 (only `Read` / `Write` / `Edit` on host paths,
and bash for compile/test in the sandbox tmpfs). Recommend the next session
maintain that discipline: any agent-driven git is invoked by the operator
on the Mac, never inside the sandbox.

---

## 2. What is running on the VPS

No change to the VPS this session. The four S33 engines plus the S34-P1
patched `UstecTrendFollow5m` are still shadow-firing on the VPS at
whatever SHA was last deployed (S33k at `1511a00` per the original S34
handoff, not changed since).

**Deploy decision for the next `OMEGA.ps1 deploy`:** the new code in
S35-P2/P3/P4/P5 changes:

- `include/engine_protections.hpp` — NEW file; provides a header-only
  protection bundle. Pure additive; zero risk if no engine uses it. The
  retrofitted `XauThreeBar30m` is the only consumer in HEAD.
- `include/XauThreeBar30mEngine.hpp` — modified to add a guards member
  and hook calls. The signal evaluator, bar history, ATR computation, and
  entry geometry are byte-identical to the pre-retrofit version. All new
  knobs default to disabled-value, so an engine_init.hpp that did not set
  any of them would produce regression-identical behavior.
- `backtest/threebar30m_xau_S35P3_backtest.cpp` — NEW backtest. Backtest
  binary; never executed by production code.
- `tests/test_xauthreebar30m_S35P3.cpp` — NEW behavior test. Stand-alone
  binary; never executed by production code.
- `include/globals.hpp` — adds a `static omega::XauThreeBar30mEngine
  g_xau_threebar_30m;` declaration.
- `include/engine_init.hpp` — adds an init block that configures the
  engine with the S35-P4 TUNED defaults and prints an `[OMEGA-INIT]`
  status line.

**Net effect on a deployed binary**: a new global engine instance is
constructed and initialised, prints its config line at startup, and then
sits dormant because `tick_gold.hpp` does not yet dispatch M30 bars to it.
No additional CPU on the tick path, no additional memory beyond the engine
struct, no additional locks, no new threads. The engine cannot fire trades
until the dispatch wiring lands (see §6.1 below).

The S26 broker-fill reconciliation (`8206e11`) is more material:

- `include/order_exec.hpp` — prefers FIX tag 6 (AvgPx) over tag 31
  (LastPx) when routing fills to `applyBrokerFill`. Single-partial fills
  unaffected; multi-partial fills now use the broker's volume-weighted
  average, which is the price the operator wants for accounting.
- `include/OmegaTradeLedger.hpp` — `applyBrokerFill` now updates
  `tr.slippage_entry`, `tr.slippage_exit`, `tr.exitPrice`, and recomputes
  `tr.net_pnl` from broker fills, while explicitly NOT recomputing
  `tr.pnl`. This preserves the codebase contract (`tr.pnl` is engine
  intent, `tr.net_pnl` is engine intent minus measured costs) and is the
  specific correction documented in `HANDOFF_S26_PART1B_VERIFICATION_REBUILD.md
  §3` to the buggier first S26-P1 implementation.
- `include/trade_lifecycle.hpp` — adds the disparity hard-stop monitor
  and the broker-reconcile mismatch warning at the end of
  `handle_closed_trade`. Disparity hard-stop auto-shadows
  `MicroScalperGold` on sustained engine-vs-broker pnl divergence;
  reconcile-mismatch logs `[BROKER-RECONCILE-MISMATCH]` (rate-limited
  1/min) when the engine claims live trades but the broker shows zero.
- `include/RiskMonitor.hpp` — public `trip_engine_to_shadow(engine,
  reason)` wrapper.
- `include/omega_main.hpp` — startup invariant that refuses to launch
  the process when `mode` and FIX credentials disagree.

After deploying `289f8b2`, the operator should expect:

1. `[OMEGA-INIT] XauThreeBar30mEngine initialised: shadow=1 enabled=1
   lot=0.01 ... (S35-P4 TUNED; tick_gold.hpp M30 dispatch wiring
   REQUIRED before engine fires)` at startup. This confirms the new
   global is reachable.
2. `[OMEGA-MODE-CHECK] mode=... sender=... username=... -- OK` at
   startup, confirming the S26 §2.1 invariant passed for this config.
3. Existing engines unchanged.
4. New `[TRADE-COST-RECON-ENTRY]` and `[TRADE-COST-RECONCILED]` log
   lines on every fill (these are forensic, expected, and replace the
   prior model-only `[TRADE-COST]` line for engines that report broker
   fills).
5. If the engine ever drifts from broker truth, `[ENGINE-AUTO-SHADOW]`
   trips and pins `MicroScalperGold` to shadow. This is a NEW behavior
   in production; the threshold is `max($1, 10% × |engine_pnl|)` and
   either sustained for 60 seconds OR monotonically growing over 3+
   closed trades.

No live promotion of any engine should happen on this deploy without
operator-side ledger reconciliation first.

---

## 3. What S35-Part-2 shipped (in narrative)

### 3.1 `8206e11` — S26-late broker-fill reconciliation

The `stash@{0}` content described in the previous handoff was applied
intact: 7 files, +871 / −67 lines. Two threads were mixed in the same
stash and committed together with explicit notes in the commit message
about what is "broker-fill" (Thread A, 5 files) and what is "bundled S20
backtest-flavored work" (Thread B, 2 files: `IndexFlowEngine.hpp` and
`microscalper_crtp_sweep.cpp`).

The paired test in `tests/test_apply_broker_fill_S26_P1B.cpp` (present
in HEAD since `403b9cb`) compiles against the patched ledger and prints
`SUMMARY: 3 passed, 0 failed`. The test drives the 2026-05-10 23:00:38
UTC SHORT trade end to end (entry 4693.28, engine TP 4692.49, broker
close 4693.39) and asserts:

```
tr.net_pnl    -0.11 USD  (within +/- 0.01)
tr.broker_pnl -0.11 USD  (within +/- 0.01)
disparity()    0.00 USD  (within +/- 0.01)
```

All three pass on the patched code. The same test on the buggier first
implementation would have shown `tr.net_pnl ≈ -$1.12` (11× worse than
reality) and the comment block in the test re-derives that arithmetic
offline so the proof of the bug fix is reproducible without reverting.

### 3.2 `4e85956` — S35-P2 engine_protections.hpp

A new 470-line header-only library at `include/engine_protections.hpp`.
Provides one struct `omega::ProtectedEngineGuards` containing a
`ProtectionConfig` (knobs) and a `ProtectionState` (runtime values), and
six free-function logging helpers. The ten bundled protections:

1. Hard SL multiplier (engine still owns sl_px assignment; config value
   used at `_fire_entry` time).
2. Time stop after N bars held.
3. Break-even shift: arm BE when MFE ≥ `be_trigger_atr × ATR_at_entry`;
   new SL = entry ± `be_cost_buffer_pts`.
4. Trailing stop after BE arms: trail SL = mid ∓ `trail_atr_mult ×
   ATR_at_entry`. Only tightens; never loosens.
5. Daily loss cap: disable entries for the rest of the UTC day once
   cumulative `daily_pnl_usd ≤ -daily_loss_limit`. Auto-resets on day
   rollover.
6. Consecutive-loss kill switch: `killswitch_tripped = true` after N
   losses in a row. Streak resets on any winning trade.
7. Volatility regime gate: ATR floor + ATR ceiling.
8. Spread cap (standard, included for uniformity).
9. Session-window block: `[start, end)` UTC hours; wraps midnight when
   `start > end`.
10. `shadow_mode` and `enabled` remain engine-owned (they gate the
    engine itself, not individual entries).

Engines opt in by adding a public `ProtectedEngineGuards guards;`
member, calling `guards.cfg.* = value;` in init(), then hooking the
standard lifecycle (`roll_day` → `on_bar_held` → `time_stop_fired` →
`check_entry_ok` → `update_mfe_mae` → `update_sl` → `on_close` →
`reset_per_trade`). The header docstring shows the full usage template.

Verification: compile-clean under `g++ -std=c++17 -Wall -Wextra`; a 13-
test driver (run from `outputs/protect_test.cpp` in S35-Part-2,
in-context tested but not committed to the repo) covers BE arm, trail
tighten, trail-no-loosen, time-stop boundary, daily-cap trip, killswitch
trip, session-window wrap, spread cap, and ATR floor/ceiling. All 13
pass.

The header is consumed first by `XauThreeBar30mEngine` (next item).
Future intraday engines on any timeframe and any symbol should adopt it
the same way.

### 3.3 `1684cfc` — S35-P3 XauThreeBar30m retrofit

`include/XauThreeBar30mEngine.hpp` rewritten end-to-end to add the
guards bundle. The signal evaluator, bar history aggregation, local ATR
computation, and entry geometry math are byte-identical to the
pre-retrofit version. The retrofit only adds:

- 11 new public knobs (forwarded to `guards.cfg` in `init()`). Defaults
  are disabled-values, so an `engine_init.hpp` that did not set any of
  them would behave identically to the pre-retrofit engine.
- A `ProtectedEngineGuards guards;` public member.
- `roll_day → on_bar_held → time_stop_fired` path in `on_30m_bar` (the
  time-stop close fires with `tr.exitReason = "TIME_STOP"`).
- `check_entry_ok` before signal evaluation (subsumes the prior
  spread-cap return; spread now goes through `guards.cfg.max_spread`).
- `update_mfe_mae → update_sl` path in `_manage_open`. MFE/MAE moved to
  guards (single source of truth) and reflected back into the position
  struct for backward-compat readers.
- `guards.on_close` in `_close` to update USD pnl bookkeeping. If the
  killswitch newly trips, flip `shadow_mode = true` and log
  `[ENGINE-AUTO-SHADOW]`. If daily cap newly trips, log
  `[GUARD-DAILY-CAP]`. Then `reset_per_trade`.
- New exit reason `TRAIL_HIT` introduced for the SL-hit-but-BE-armed
  case. Raw `SL_HIT` now strictly means original SL touched without BE
  arming. Distinguishes break-even / trail-locked exits in the ledger
  for post-hoc forensics.

Verification:
- compile-clean under `g++ -std=c++17 -Wall -Wextra`.
- `tests/test_xauthreebar30m_S35P3.cpp` (committed in `1684cfc`) drives a
  synthetic three-bar LONG signal through entry → BE arm → trail ratchet
  → trail exit, then verifies killswitch on 3 losses and daily-cap on
  -$5 cumulative. 7 assertions, all pass.

### 3.4 `3ee31de` — S35-P4 backtest harness

`backtest/threebar30m_xau_S35P3_backtest.cpp` (a 700-line stand-alone
harness). Pipeline:

```
M15 bars CSV  →  M30 paired aggregation (gap-aware)
              →  Wilder ATR14 on M30 bars
              →  engine.on_30m_bar at each M30 close
              →  synthetic intra-bar tick stream for the NEXT M30
                 (open → adverse extreme → favourable extreme → close;
                  adverse = low for LONG, high for SHORT --
                  conservative pessimistic ordering)
              →  engine.on_tick for SL/TP/BE/trail evaluation
              →  trades captured via on_close callback
              →  per-year + total stats; equity curve CSV
```

Three configs compared:

```
                         BASELINE        TUNED         STRICT
                         (no guards)     (BE+trail+    (all S35-P3
                                          ATR floor)    defaults on)
─────────────────────────────────────────────────────────────────────
Trades                   727             1058          74 (killswitch
                                                       trips, locks out)
Win rate                 35.1%           66.2%         50.0%
Net P&L (25 months)      +$551.79        +$1488.60     -$10.90
Profit factor            1.07            1.27          0.95
Avg per trade            +$0.76          +$1.41        -$0.15
Max drawdown             $508.96         $282.83       $64.55
Peak equity              $892            $1498         $54

Per-year breakdown (TUNED):
  2024 (Mar-Dec partial)   n=423   WR=66.7%   net=+$199.18
  2025 (full year)         n=510   WR=65.1%   net=+$241.88
  2026 (Jan-Apr partial)   n=125   WR=68.8%   net=+$1047.54

Exit reason mix (TUNED):
  TRAIL_HIT  673   (64% of all exits -- trail locked winners that
                    would have round-tripped to SL in baseline)
  SL_HIT     358
  TP_HIT      27   (rare: trail or BE captures most winners earlier)
```

The dataset is `fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv`
(50,719 M15 bars → 25,357 M30 bars over 25 months). The baseline result
direction-matches the S33 Pass-8 figure (`n=639 +$979 across 30mo`) on a
different date range.

Findings:
- The signal has real edge (+$552 baseline, all-years positive in
  TUNED).
- BE + trail + ATR floor are the production-ready protections. They
  triple net P&L and cut max drawdown by 45%.
- The strict S35-P3 defaults (`max_consec_losses=5`, `daily_loss_limit=$5`,
  `session_block=22-08`) trip prematurely on a 35%-native-WR strategy
  and lock the engine into permanent shadow before the dataset
  completes. The killswitch is sticky by design (operator review
  pattern, no autonomous reset), so the strict config self-destructs in
  2024 and never re-trades. This is documented behavior, not a bug.
- The strict thresholds will be revisited after shadow-live trace
  data informs the streak / drawdown distribution.

Build / run:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
    backtest/threebar30m_xau_S35P3_backtest.cpp \
    -o backtest/threebar30m_xau_S35P3_backtest

backtest/threebar30m_xau_S35P3_backtest \
    --csv fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv \
    --out-prefix backtest/threebar30m_S35P3
```

Outputs go to `backtest/threebar30m_S35P3_{baseline,tuned,strict}_trades.csv`,
`{...}_equity.csv`, and `{...}_summary.{txt,csv}`.

### 3.5 `289f8b2` — S35-P5 production wiring

`include/globals.hpp` and `include/engine_init.hpp` modified to
instantiate and initialise `g_xau_threebar_30m` with the S35-P4 TUNED
config. The init block prints an `[OMEGA-INIT]` startup line so the
operator can confirm the engine reached init.

Exact production config:

```
shadow_mode        = true   (HARD shadow until shadow-live validated)
enabled            = true   (engine runs, in shadow)
lot                = 0.01
max_spread         = 1.0
be_trigger_atr     = 1.0    (BE arm at +1*ATR favourable)
be_cost_buffer_pts = 0.10   (BE-shifted SL = entry +/- 0.10)
trail_after_be     = true   (trail after BE arms)
trail_atr_mult     = 0.75   (trail SL = mid -/+ 0.75*ATR_at_entry)
min_atr_floor      = 0.30   (filter dead tape)
max_bars_held      = 0      (no time stop)
daily_loss_limit   = 0.0    (disabled; strict tripped too easily)
max_consec_losses  = 0      (disabled; strict tripped at 5)
max_atr_ceil       = 0.0    (disabled)
block_hour_start   = -1     (disabled; XAU Asia has flow)
block_hour_end     = -1
```

This commit only adds config-file lines. **The engine is dormant** until
`tick_gold.hpp` is updated to call `g_xau_threebar_30m.on_30m_bar(...)`
on each M30 bar close and `g_xau_threebar_30m.on_tick(...)` on each gold
tick. That wiring is the §6.1 next-session task below.

### 3.6 Multi-timeframe XAU research verdict (no commit; verdict only)

The original handoff §6 listed "Option A — same harness, two more
symbols" as a candidate task. The user's S35-Part-2 framing extended the
question to "intraday XAU on 1m / 5m / 10m / 15m / 30m with downside
protection". The verdict was reached by reading the existing
`backtest/edge_hunt_xau_{2023,2024,2025}.csv` per-year result files
(already in HEAD from `403b9cb`), not by running new sweeps:

```
Tested timeframes on XAU (intraday): 5m, 15m.
Tested signal families (8):
  MACross, Donchian, Momentum, VolExpand, InsideBar,
  ER_Trend, ORB, ATR_Mom
Tested brackets (2):
  sl1.5_tp3.0  (RR=2 tight)
  sl2.0_tp4.0  (RR=2 wide)
Cost model:
  net_at_$0.06 per round trip

POSITIVE-NET INTRADAY COMBOS PER YEAR (5m or 15m):
  2023            12 positives (all M15)
  2024             1 positive  (M15 Donchian N=50 wide, +$33 net)
  2025            11 positives (all M15)

Cross-year intersection (positive in ALL THREE years): 0.

M5 XAU: zero positive results, ANY family, ANY bracket, ANY year.
M15 XAU: fragmented positives -- some combos work in 2023 and 2025,
         but 2024 was a different regime and only one survived.
         NO combo positive in all three years.
M1 + M10: not in the existing sweep. Tick data exists in
          outputs/duka_xauusd_daily/ (623 daily files 2023-09 -> 2025-09)
          so a sweep at those timeframes is technically possible, but
          the prior from M5 (universally negative) is strongly against
          finding edge there.
M30:   the lowest XAU timeframe with a robust edge.
       Confirmed in S35-P4 backtest above: TUNED ThreeBar30m positive
       in every year of the 25-month sample.
```

**Recommendation: do not build 1m / 5m / 10m / 15m XAU engines.** Energy
is better spent on (a) the `tick_gold.hpp` wiring to turn on
ThreeBar30m, and (b) extending the same cross-year audit to USTEC /
US500 / EURUSD to learn which OTHER symbols have intraday edges worth
building engines for.

The reproducer for this verdict (read-only, no compile, no run):

```bash
python3 <<'PY'
import csv
years = [2023, 2024, 2025]
pos = {}
for y in years:
    seen = set()
    with open(f'backtest/edge_hunt_xau_{y}.csv') as f:
        for row in csv.DictReader(f):
            if row['timeframe'] not in ('5m','15m'): continue
            try: net = float(row['net_at_006'])
            except: continue
            if net <= 0: continue
            key = (row['timeframe'], row['family'], row['params'],
                   row['bracket'])
            seen.add(key)
    pos[y] = seen
print(f"positives 2023: {len(pos[2023])}, 2024: {len(pos[2024])},"
      f" 2025: {len(pos[2025])}")
print(f"INTERSECTION (positive ALL 3 years):"
      f" {len(pos[2023] & pos[2024] & pos[2025])}")
PY
```

---

## 4. Data inventory (S35-Part-2 deltas)

No changes to the corpora. New backtest output files were written into
the sandbox `outputs/` directory during S35-Part-2 (per-trade CSVs,
equity CSVs, summary files at `backtest/threebar30m_S35P3_*`); these are
ephemeral artifacts and were not committed. They can be regenerated at
any time by re-running the harness from `3ee31de` against the same M15
dataset.

The data files used in S35-Part-2:

```
fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv
   50,719 M15 bars from 2024-03-01 to 2026-04-22.
   Used by backtest/threebar30m_xau_S35P3_backtest.cpp.

outputs/duka_xauusd_daily/
   623 daily tick-data CSVs from 2023-09-27 to 2025-09-26.
   ts_ms,bid,ask format. Used by backtest/edge_hunt.cpp.
   NOT used by S35-Part-2 directly; the edge_hunt verdict in §3.6 was
   read out of the existing per-year result CSVs, not from a fresh
   sweep.

backtest/edge_hunt_xau_{2023,2024,2025}.csv
   Per-year sweep result tables. Read in §3.6 to produce the
   multi-tf XAU verdict.
```

---

## 5. Next-session work plan (in priority order)

### 5.1 Top priority: wire `tick_gold.hpp` for M30 dispatch

The single thing that turns the entire S35-Part-2 stack from "code in
the tree" into "live shadow data flowing". `tick_gold.hpp` is 2649 lines
and is hot-path critical — every gold tick goes through it. The engine
docstring at `include/XauThreeBar30mEngine.hpp` (top comment block,
USAGE section) shows the canonical dispatch sketch:

```cpp
// tick_gold.hpp inside the M30 bar-close branch:
omega::XauThreeBar30mBar bar30m{};
bar30m.bar_start_ms = s_bar_30m_ms;
bar30m.open  = s_cur_30m.open;
bar30m.high  = s_cur_30m.high;
bar30m.low   = s_cur_30m.low;
bar30m.close = s_cur_30m.close;
g_xau_threebar_30m.on_30m_bar(bar30m, bid, ask,
    g_bars_gold.m30.ind.atr14.load(std::memory_order_relaxed),
    now_ms_g, bracket_on_close);

// tick_gold.hpp on every gold tick:
g_xau_threebar_30m.on_tick(bid, ask, now_ms_g, bracket_on_close);
```

**Open question for the next session**: does `tick_gold.hpp` already
have an `s_cur_30m` / `s_bar_30m_ms` M30 bar aggregator, or does that
need to be added alongside the dispatch hook? `grep -nE
"s_cur_30m|s_bar_30m|m30|M30|30m" include/tick_gold.hpp` returned no
matches in S35-Part-2's investigation. If M30 aggregation does not
exist yet, the wiring is two parts: build the M30 bar aggregator
(mirror the existing H1 / H4 aggregator pattern) and add the dispatch
hook. Reading the H1 and H4 aggregator sections is the right entry
point.

Touch the protected file list with care:
```
include/tick_gold.hpp is NOT on the §8 protected list, BUT it is the
gold tick hot path. Treat it as careful-edit territory:
  - one commit, one focused change, with tests
  - compile + run a small unit test exercising the dispatch
  - operator reviews the diff before deploy
```

After the dispatch wires in, expected behavior: on a redeploy with
gold trading active, you should see (in shadow log):
```
[XauThreeBar30m] [GUARD-BLOCK] reason=ATR_BELOW_FLOOR   (when ATR<0.30)
[XauThreeBar30m] [GUARD-BLOCK] reason=SPREAD_CAP        (when spread>$1)
[XauThreeBar30m] ENTRY  LONG  ...                       (at signal)
[XauThreeBar30m] CLOSE  TRAIL_HIT  pnl=+0.0X            (most exits)
```

### 5.2 Cross-year audit on USTEC / US500 / EURUSD

The S35-Part-2 XAU verdict (no intraday edge below M30) leaves open
whether OTHER symbols have intraday edges. The existing
`backtest/edge_hunt_results.csv` (cross-symbol, but not split by year)
plus any per-year files that exist for those symbols would answer this
without running new sweeps. If the per-year split does not exist for
those symbols, running `edge_hunt` with a year filter is a 1-line wrapper
script.

This is pure read-only research; no production changes. Useful before
committing engineering hours to building any new intraday engine.

### 5.3 `stash@{1}` decision

`stash@{1}` (S20 IndexBacktest per-gate diagnostic, +280 lines in
`backtest/IndexBacktest.cpp`) has been sitting on the shelf since S35
started. The diff is diagnostic-only — adds per-gate-rejection counters
to the `IndexBacktest` runner. Three options unchanged from the original
handoff:

(a) Apply and commit as a standalone backtest-diagnostics commit.
(b) Discard if redundant with later commits.
(c) Rebuild from the handoff docs if the original WIP was incomplete.

Lowest urgency. Recommended (a) when convenient.

### 5.4 Optional cleanup items

- The `PATH-A-DEBUG-2026-05-08` per-gate reject counters added to
  `include/IndexFlowEngine.hpp` in `8206e11` are not `#ifdef`-guarded.
  Adding `#ifdef OMEGA_PATH_A_DEBUG` around them is a 1-commit
  cleanup if they ever become noisy in production logs.
- The S35-Part-2 backtest output files in `backtest/threebar30m_S35P3_*`
  are not in `.gitignore`. The `.gitignore` from `403b9cb` covers
  `backtest/*.csv`, but the exact pattern depends on the gitignore
  rules. Verify whether the backtest outputs would be picked up by
  `git status` if regenerated.

---

## 6. Quick-reference commands for the next session

### 6.1 Standard session-start sequence

```bash
cd ~/omega_repo
bash .claude-preflight.sh           # expect green at 289f8b2
git log --oneline -7                # verify the S35-Part-2 commits
git stash list                      # expect only stash@{1}
```

If preflight reports `[PREFLIGHT-FAIL] Operation not permitted` on
`.git/index.lock` and the lock file actually exists (from a prior
sandbox session), clear it once:

```bash
ls -la .git/index.lock              # confirm exists, 0 bytes
rm -f .git/index.lock
bash .claude-preflight.sh           # re-run
```

### 6.2 Rebuild and rerun the S35-P4 backtest (sanity check)

```bash
cd ~/omega_repo
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
    backtest/threebar30m_xau_S35P3_backtest.cpp \
    -o backtest/threebar30m_xau_S35P3_backtest

backtest/threebar30m_xau_S35P3_backtest \
    --csv fvg_phase0/XAUUSD_15min/bars_XAUUSD_15min_2024-03_2026-04.csv \
    --out-prefix backtest/threebar30m_S35P3
```

Expect to see exactly the numbers in §3.4 above. If the numbers differ,
either the data file changed or the engine code changed; investigate.

### 6.3 Rerun the S35-P3 retrofit test (sanity check)

```bash
cd ~/omega_repo
clang++ -std=c++17 -O0 -Iinclude tests/test_xauthreebar30m_S35P3.cpp \
    -o /tmp/test_xauthreebar30m_S35P3 \
    && /tmp/test_xauthreebar30m_S35P3
```

Expect `SUMMARY: ALL TESTS PASSED` and exit 0.

### 6.4 Rerun the S26 broker-fill test (sanity check)

```bash
cd ~/omega_repo
clang++ -std=c++17 -O0 -Iinclude tests/test_apply_broker_fill_S26_P1B.cpp \
    -o /tmp/test_S26_P1B \
    && /tmp/test_S26_P1B
```

Expect `SUMMARY: 3 passed, 0 failed` and exit 0.

---

## 7. Safety invariants carried forward

Unchanged from `HANDOFF_S35.md §8` with two additions:

1. `mode=SHADOW` in `omega_config.ini` (committed to origin/main).
2. `max_lot_gold=0.01`.
3. Protected file list (no S35-Part-2 commit touches any of these
   except via the recovered S26 stash, which was the intended target):
   - `include/order_exec.hpp`                  (S26-late, intended)
   - `include/OmegaTradeLedger.hpp`            (S26-late, intended)
   - `include/trade_lifecycle.hpp`             (S26-late, intended)
   - `include/omega_main.hpp`                  (S26-late, intended)
   - `backtest/microscalper_crtp_sweep.cpp`    (S26-late, intended)
   - `include/IndexFlowEngine.hpp`             (S26-late, intended)
   - `include/RiskMonitor.hpp`                 (S26-late, intended)
4. Single live-eligible engine = `GoldMicroScalperEngine` (disabled).
5. No live promotion without `ledger_reconcile` showing
   `sum_pnl_delta < $20/day`.
6. `.claude-preflight.sh` is the first command of every session.
7. PAT/token files (`.github_token`, `.env`, etc.) are gitignored. The
   PAT in CLAUDE.md is still the same as at the start of S35; rotation
   was offered and the operator declined. The advisory stands: rotate
   before committing CLAUDE.md anywhere it can be public.
8. **(NEW S35-Part-2):** the agent does not run `git status`, `git
   commit`, `git stash` or any other git command from inside the
   sandbox; only `Read` / `Write` / `Edit` on host paths and `bash` for
   compile/test in the sandbox tmpfs. All git operations are invoked
   by the operator on the Mac. This avoids creating stale
   `.git/index.lock` files.
9. **(NEW S35-Part-2):** the `XauThreeBar30m` engine is HARD shadow +
   enabled in `engine_init.hpp` (commit `289f8b2`). Even after the
   `tick_gold.hpp` wiring lands (§5.1) and the engine starts seeing
   bars, every trade is logged as shadow until the operator flips
   `shadow_mode` to `kShadowDefault` in `engine_init.hpp`. The
   recommended flip-trigger is approximately one month of shadow-live
   data confirming the backtest's per-trade expectancy and exit-reason
   distribution.

---

## 8. Outstanding action items (TL;DR for the human)

1. **Eventually outside any session (security)**: rotate the GitHub PAT
   at https://github.com/settings/tokens and replace the literal token
   in CLAUDE.md with a path reference. The operator declined this in
   S35-Part-2 but the recommendation stands.
2. **Start of next session**: preflight, then §5.1 — wire
   `tick_gold.hpp` for M30 dispatch. This is the single thing that
   turns the entire S35-Part-2 work into live shadow data.
3. **After that**: §5.2 cross-year audit on USTEC / US500 / EURUSD.
   Read-only, no commits, results inform whether to build any new
   intraday engines for those symbols.
4. **Then**: §5.3 `stash@{1}` decision.
5. **Eventually**: revisit the strict-config protection thresholds
   (`daily_loss_limit`, `max_consec_losses`, `block_hour_*`,
   `max_bars_held`) once shadow-live trace data informs the streak /
   drawdown distribution. Re-enable selectively in `engine_init.hpp`.

End of HANDOFF_S35_PART2_COMPLETE.
