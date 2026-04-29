# Omega Trading System — Session Handoff (Night)
## 2026-04-29 — discovery session; reframe; SpreadRegimeGate v2 written; System B (C1_retuned) confirmed viable

**This file lives at `/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_night.md`.**
It supersedes `SESSION_HANDOFF_2026-04-29_evening.md` (which has a forward
pointer to this file at its top). Read this file first.

---

## TL;DR for next session

This session started as "validate Option 1 / plan Option 2" and reframed
twice. The big realisations:

1. **System B (Python H1 portfolio, `C1_retuned`) is viable and ship-ready.**
   +74.12% return, Sharpe 2.651, max DD -5.85%, walk-forward TEST/TRAIN/
   VALIDATE all PASS. Donchian H1 long retuned to `(period=20, sl_atr=3.0,
   tp_r=5.0)` plus Bollinger H2/H4/H6 long, max 4 concurrent, 0.5% risk.
   See `phase2/donchian_postregime/CHOSEN.md`. **Next physical step: launch
   live shadow paper-trading run, 4–8 weeks.**

2. **System A (C++ tick engines: HBG, CFE, MCE) is not viable in current
   form.** Even after spread-cost normalisation, all three are net-negative
   across the full 2024-03..2026-04 corpus. Empirical knee analysis on the
   recost CSVs shows the "best" gate threshold retains only 1.2% of trades
   for break-even. The strategies don't have demonstrable edge at any
   spread regime. SpreadRegimeGate v2 was written this session as a
   *defensive bleed-prevention* layer, not a profit engine.

3. **The microstructure regime change is April 2025**, confirmed from
   `tick_q.csv`. Median spread stepped from 0.55pt to 0.68pt between
   2025-03 and 2025-04, then sustained 0.55–0.92pt through 2026-04. This
   is the *execution-cost* regime axis. Distinct from System B's
   `2026-01-28` regime split which is price/momentum-based.

4. **Jo wants a new System A intraday tick engine** with these properties:
   trade clear opportunities, tight trails, bank profit early, exit when
   it turns, multiple small trades ($20–100 per trade), pyramiding when
   available, simple, all guardrails. **NOT YET BUILT.** Three open
   questions need Jo's confirmation before signal-discovery work begins.

The next-session agenda is in §"Next session — concrete first step" below.

---

## Repo state at end of session

- HEAD: unchanged from start (`6e10eba audit-fixes-18: cost_recalc (Opt3)
  + SpreadRegimeGate (Opt1) on HBG/CFE/MCE`)
- origin/main: in sync (no push this session)
- branch: `main`
- **UNCOMMITTED CHANGES**: 2 files modified/added in this session, see
  §"Files written this session". They are NOT pushed. Decision pending
  on whether to commit.

### Mounted folders in Cowork session
- `/Users/jo/omega_repo` (existed)
- `/Users/jo/Tick/duka_ticks` (mounted this session) — 4.6GB combined
  CSV at `XAUUSD_2024-03_2026-04_combined.csv`, 154,265,440 rows,
  schema `timestamp,askPrice,bidPrice` (timestamp is unix-ms).
  Available for Phase 1 signal-discovery work next session.

---

## What landed in this session

### 1. SpreadRegimeGate v2 (audit-fixes-19, uncommitted)

`/Users/jo/omega_repo/include/SpreadRegimeGate.hpp` — full rewrite,
overwriting v1. Drop-in API compatible (no engine rewiring needed).

**Four problems v1 had, fixed in v2:**

- **Fixed 0.5pt threshold was a guess.** v2 uses an adaptive threshold:
  `T_eff = clamp(p75_of_7day_hourly_medians, ABS_FLOOR=0.40,
  ABS_CEIL=0.70) × macro_mult`. Floor and ceiling come from empirical
  bucket analysis on the recost CSVs (below 0.40pt = break-even region;
  above 0.70pt = full bleed regime).
- **No hysteresis.** v2 has two thresholds: `T_close = T_eff` and
  `T_open = T_eff − HYST_BAND` (0.05pt deadband). Combined with 1h
  smoothing, prevents thrash without needing a separate dwell-tick
  filter.
- **Median measured regime, not execution.** v2 adds a 60s short-window
  median plus a spike check: `current_spread > SPIKE_MULT(2.5) ×
  short_median` blocks the fire even if 1h regime is OK.
- **No cross-system regime awareness.** v2 has an optional
  `set_macro_regime(string)` setter. `RISK_OFF` widens threshold by
  10%; `RISK_ON` tightens by 5%. Default `NEUTRAL` = no scaling = same
  as v1 behaviour. Engines that don't call the setter get drop-in
  v1-equivalent macro behaviour.

**API additions** (all backwards-compatible):
- `can_fire(double current_spread)` — explicit spread for spike check
- `set_macro_regime(const std::string&)` — macro modulation
- Diagnostic accessors: `current_long_median`, `current_short_median`,
  `effective_close_threshold`, `effective_open_threshold`,
  `current_macro_mult`, `state` (OPEN/CLOSED enum), `long_sample_count`,
  `short_sample_count`, `hourly_history_size`

**Compile-verified in sandbox** against all three engines (HBG, CFE, MCE)
with `g++ -O2 -std=c++17 -I include`. Exit 0 on all three; only the
pre-existing `snprintf` truncation warnings, identical to v1.

### 2. spread_knee.py (audit-fixes-19, uncommitted)

`/Users/jo/omega_repo/backtest/spread_knee.py` — new analysis tool.
Reads any `*_trades_recost.csv` and computes per-engine and combined
P&L bucket curves + cumulative-exclude curve. Identifies the spread
threshold that maximises retained P&L. Read-only; touches no engine
code. Run output for 2026-04-29 reference:

```
PER-ENGINE KNEE SUMMARY
engine    knee_T   kept_n  excl_n      no_gate    with_gate       delta
hbg         0.39       14    1576   $-3,427.50      $ 15.99     $ 3,443.49
cfe         0.34        0    1975   $-2,233.70      $  0.00     $ 2,233.70
mce         0.71       96     249   $  -532.13      $ 14.52     $   546.65

RECOMMENDED SYSTEM-WIDE THRESHOLD: 0.39pt
  combined retained P&L =  $6.08  (no-gate: -$6,193.33)
  combined retained n   = 48 of 3910 trades (1.2%)
```

The "best" gate keeps 1.2% of trades to barely break even. This is the
finding that confirms System A engines do not have edge.

### 3. Discovery work (no files written, knowledge captured here)

Read and synthesised:
- `tick_q.csv` — month-level spread quality summary; identified April
  2025 step change in median spread (0.55 → 0.68).
- `phase1/signals/_master_summary.txt` — 66 signal/tf/direction
  combinations tested across asian_break, bollinger, donchian,
  ema_pullback, rsi_revert, tsmom on M15/H1/H2/H4/H6/D1.
- `phase2/donchian_postregime/CHOSEN.md` — the locked Donchian H1
  decision memo. Param `(20, 3.0, 5.0)` chosen over auto-pick `(10, 3.0,
  5.0)` for stability + canonical-period alignment. C1_retuned ships.
- `phase2/donchian_postregime/sweep_results.csv` — 101 combos with
  `(*, 3.0, 5.0)` cluster dominating.
- `phase2/optionD/optionD_summary.txt` — Variants A/B/C compared.
  A=long-only-5 (+74%), B=all7-conservative (-33% maxDD, dies),
  C=strictest-4 (+57%).
- `phase2/optionD/C1_C2_summary.txt` — A/B between C1_max4 and
  C1_retuned. Retuned dominates on every metric.
- `phase2/optionD/walkforward_C_report.txt` — TRAIN/VALIDATE/TEST splits
  all PASS. PF rises across windows (1.19 → 1.39 → 1.41).
- `logs.zip` (390MB, 647 files) — extracted to `/tmp/omega_logs/` in the
  sandbox. Contains 10 days of live trade closes April 14–24, pyramid
  diagnostics, regime trail logs, multi-day L2 ticks. **Not deeply
  examined yet** — deferred to next session if useful.
- `cbe_short1_lifecycle.txt` — 0 bytes, empty placeholder. The "CBE"
  name is unused.

---

## Recommendations on viability

### System B — VIABLE, ship to shadow

`C1_retuned` portfolio is the deliverable. Walk-forward PASSES, post-
regime PF lifts (1.334 → 1.630), every A/B vs canonical wins. The only
thing keeping it from running is starting the shadow trade.

**Recommendation:** start live shadow paper-trading 4–8 weeks per
`CHOSEN.md` next-session opener. Halt criteria: cluster days stacking
>2× expected frequency in first 2 weeks. Post-mortem the 2026-03-18
4-cell cluster in parallel.

### System A current engines (HBG, CFE, MCE) — NOT VIABLE

Across the full 2024-2026 corpus, none have demonstrable edge. The
recost analysis confirmed it. The bucket curve confirmed it. CFE has no
profitable subset at any spread; HBG and MCE have weak positive pockets
that don't survive aggregation.

**Recommendation:** apply SpreadRegimeGate v2 if the engines stay live
(reduces bleed in wide regimes), but do NOT commit fresh capital to
HBG/CFE/MCE in their current calibration. The honest framing is that
these are sunk-cost — the gate is damage limitation, not an alpha
recovery.

### System A new tight-trail intraday engine — VIABILITY UNKNOWN, signal-discovery first

Jo's described properties (clear opportunities, tight trails, bank
early, frequent small trades, pyramiding, guarded) describe an
intraday scalping engine that doesn't yet exist. We do not know if
edge exists at this resolution on post-regime tick data. Phase 1
findings show what works on bars (Donchian, Bollinger, EMA pullback,
tsmom dominate at H1+ timeframes); whether tick-level analogues have
edge is unanswered.

**Recommendation:** before writing any engine code, run signal-
discovery analysis on the 4.6GB tick CSV (post-2025-04 portion).
Catalog candidate setup types (compression+break, spike-and-reverse,
level retest, momentum pullback, OFI on L2), compute forward-return
distributions at 30s/2min/5min/15min/30min horizons. If a setup type
shows edge with t-stat > 2 and reasonable frequency, build the engine
around it. If none does, the user has saved 1–2 weeks of fruitless
engine-building.

---

## Files written this session (uncommitted)

```
M  include/SpreadRegimeGate.hpp        v2 rewrite (≈230 lines)
A  backtest/spread_knee.py             new analysis tool (≈300 lines)
A  SESSION_HANDOFF_2026-04-29_night.md this file
M  SESSION_HANDOFF_2026-04-29_evening.md  forward-pointer banner only
```

The Mac BTs Jo had running at session start were against the OLD
SpreadRegimeGate.hpp (v1). Those binaries are unaffected by the v2
write. The next time engines are rebuilt on the Mac, they will pick up
v2. **Whether to commit/push v2 + the analysis script is a pending
decision** — see §"Open decisions" below.

---

## Open decisions (pending Jo's input)

### Three questions still unanswered from end of session

1. **Confirm System A vs System B framing.** Is C1_retuned (System B)
   on track to ship to shadow as planned, while System A becomes the
   green-field tick-engine work? Or is something different.

2. **Should the new tick engine eventually replace one of HBG/CFE/MCE
   in the C++ runtime, or run as a fourth alongside?** Replacing CFE
   (the worst performer) makes the most sense if we use the same
   harness pattern. Running alongside preserves optionality but adds
   complexity.

3. **For new-engine signal discovery, confirm post-2025-04 tick corpus
   (13 months, ~80M ticks).** That's the right cut for execution-
   sensitive tick-level work given the April 2025 microstructure
   shift.

### Other pending decisions

- **Commit + push SpreadRegimeGate v2 + spread_knee.py?** If yes, follow
  the PAT pattern from `SESSION_HANDOFF_2026-04-29_evening.md` (PAT at
  `/Users/jo/omega_repo/.github_token`). If no, leave uncommitted; next
  session can decide.
- **Apply v2 to live engines or keep on shelf?** v2 is defensive; if
  HBG/CFE/MCE are being suspended (recommended), then v2 is moot. If
  they stay live for diagnostic / experimental reasons, v2 reduces
  bleed.
- **Macro-regime hook wiring.** v2 has `set_macro_regime()` but engines
  don't currently call it. Wiring is one line per engine. NOT done this
  session — Jo's "never modify core code unless instructed" pref means
  this needs explicit go-ahead.

---

## Next session — concrete first step

Open this doc. Read the TL;DR. Then ask Jo to answer the three open
framing questions above (System A/B framing; replace-or-add for new
engine; tick corpus cutoff). Once those are answered:

**If signal-discovery first (recommended):**
1. Build `phase2/tick_signal_discovery.py` (or similar — name TBD).
2. Reads the 4.6GB tick CSV `/Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv`, filters to post-2025-04.
3. For each candidate setup type, computes forward-return distributions at 30s/2min/5min/15min/30min horizons.
4. Outputs a ranked candidate list with t-stats and frequencies.
5. Decision point: which (if any) setup proceeds to engine build.

Estimated 2–3 evening sessions for full survey across 4–5 candidate
setups. Compression+break is my prior favourite given the user's "clear
opportunity" framing, but the discovery should be open-minded.

**If C1_retuned shadow-trade kickoff first:**
Different track entirely — orchestrating the shadow trade run, log
format alignment, halt-criteria monitor. Punt to System B side; do not
mix with System A discovery work.

### Concrete next-session opener line for Jo to paste

> **State for next session — System A new-engine signal discovery
> kickoff (or System B C1_retuned shadow start, Jo's choice).**
>
> System B is locked: C1_retuned ships to live shadow paper-trading
> per `phase2/donchian_postregime/CHOSEN.md`. System A current engines
> (HBG/CFE/MCE) are not viable; SpreadRegimeGate v2 written but
> uncommitted as defensive layer if they stay live. New System A
> intraday tick engine is green-field; viability unknown until signal-
> discovery analysis runs on post-2025-04 tick data (4.6GB combined
> CSV mounted at `/Users/jo/Tick/duka_ticks/`).
>
> First decision: which track — (a) launch C1_retuned shadow trade,
> (b) start new-engine signal discovery, (c) both. Then proceed.
>
> Read `SESSION_HANDOFF_2026-04-29_night.md` first.

---

## User preferences (carry forward)

- Always provide full code files, not snippets/diffs.
- Warn at 70% chat context with summary. **This handoff written at
  ~75% context after session-end summary; next session starts fresh.**
- Warn before time/session blocks.
- Never modify core code without explicit instruction. Engine wiring
  changes (e.g., wiring `set_macro_regime` into HBG/CFE/MCE on_tick) are
  core-code modifications and require explicit "go ahead" before
  applying.
- Use the PAT without arguments when committing — stored at
  `/Users/jo/omega_repo/.github_token`. Do NOT mention rotation.
- Email: kiwi18@gmail.com
- Name: jo

---

## Where to find this doc on session start

```
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_night.md
```

Plus the previous evening handoff (now superseded but useful for the
"how Options 1 and 2 were originally framed" context):

```
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_evening.md
```

Plus the original PM handoff (the foundational one with cost-recalc
findings):

```
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_pm.md
```
