# Omega Trading System — Session Handoff (PM)
## 2026-04-29 — audit-fixes-16 + 17 + structural diagnosis

This continues `SESSION_HANDOFF_2026-04-29.md` (the AM doc). Read both, in order.
**Read this file first** before doing any work in the next session.

---

## TL;DR for next session

The HBG S47 T4a ratchet bug is fixed and pushed (`1847108`). The fix
exposed a deeper structural problem: **2026-Q1 is a real market
microstructure regime shift, not a tick-data artifact**. XAUUSD median
spread roughly doubled from 2024 to 2026 (0.34pt → 0.92pt peak), and tick
density nearly doubled. The engines' fixed cost model
(`apply_realistic_costs(tr, 3.0, ...)`) is understating real costs in
2026 by 2-3×. **Until cost is reckoned correctly, every per-trade P&L
report from this session understates losses for 2026-Q1.**

The next-session agenda is three options in order: 3 → 1 → 2 (defined
below in §"Three options to test").

---

## Repo state

- **HEAD**: `1847108` (or higher if audit-fixes-17 was committed before handoff)
- **origin/main**: in sync, push verified by `git ls-remote`
- **branch**: `main`
- **PAT**: stored at `/Users/jo/omega_repo/.github_token` (gitignored).
  Read it with `cat .github_token`. Use it without further confirmation
  in the next session. The user's standing instruction is *use the PAT
  without arguments*. Do not lecture about rotation; the user has
  already heard that and chose to keep it.

### Pushing from a sandbox where origin is SSH

`origin` is `git@github.com:Trendiisales/Omega.git` (SSH). PATs are HTTPS
auth. Push via a one-shot URL — do **not** modify the remote config:

```
TOKEN=$(cat /Users/jo/omega_repo/.github_token)
git push "https://x-access-token:${TOKEN}@github.com/Trendiisales/Omega.git" HEAD:main 2>&1 \
    | sed "s|${TOKEN}|REDACTED|g"
```

The `.git/index.lock` file may go stale if the host's git GUI is open.
If `git add` errors with "Unable to create '.git/index.lock'", ask the
user to close any running git GUI (Sourcetree, GitHub Desktop) and `rm`
the lock, then retry.

---

## What landed in audit-fixes-16 (commit `1847108`)

```
M  .gitignore                               +50 lines (transient files)
M  include/GoldHybridBracketEngine.hpp      +34 / -9 (S47 T4a ratchet fix)
A  backtest/cfe_iter15_diag.py              +369
A  backtest/mce_diag.py                     +391
A  backtest/tick_quality.py                 +354
```

**HBG S47 T4a ratchet fix** (lines 314-358 of `GoldHybridBracketEngine.hpp`):

- Before: `m_range_history.push_back(range)` only ran AFTER the
  threshold check passed (i.e., on a fire). With `EXPANSION_MULT=1.10`
  this monotonically biased the median upward; eventually no compression
  could pass.
- After: push runs unconditionally for every "qualifying compression"
  (one that already passed `MIN_RANGE/MAX_RANGE`, `MIN_BREAK_TICKS`, and
  `COST_FAIL`), BEFORE the threshold check. The gate now means "current
  range ≥ 1.10 × median of recent qualifying compressions" instead of
  "≥ 1.10 × median of recent fires."
- Constants unchanged. Single edit point. No other call sites touched.

**Empirical validation of the fix**:

| | Pre-fix (43 trades total) | Post-fix |
|---|---:|---:|
| Total | 43 / -$134 / -$3.12/tr | **1,590 / -$5,192 / -$3.27/tr** |
| 2026-02 trades | 0 (locked out) | **616** |
| 2026-03 | 0 | **295** |
| 2026-04 | 0 | **102** |

Ratchet broken: ✅. Engine still net-negative: ⚠. 2026-Q1 over-fires
catastrophically: 🚨 (this is what next session attacks).

---

## The structural diagnosis (this session's most important finding)

### Cross-engine pattern — every engine has a profitable subset and a losing subset

| Engine | Profitable subset | Losing subset |
|---|---|---|
| **HBG** post-fix-16 | TP_HIT 165 trades, +$1,429, **+$8.66/tr** | SL_HIT 850 trades, **-$7.58/tr** |
| **MCE** | MAX_HOLD 25 trades, +$430, **+$17.19/tr** | SL_HIT 320 trades, **-$2.15/tr** |
| **CFE** | MFE > 2pt 280 parents, +$598, **+$2.14/tr** | MFE 0–0.5pt 713 parents, **-$2.05/tr** |

The edge exists. The engines can't filter at *entry time* to take only
the profitable subset. They take all signals and lose on the majority
that don't develop.

### Math at current R:R is structurally negative

| Engine | WR | Win$/Loss$ | Implied break-even WR |
|---|---:|---:|---:|
| HBG | 25.8% | 1.14 | **46.7%** (gap: 21pp) |
| MCE | 18.8% | 8.0 | 11.1% (above! but MAX_HOLD only fires 7%) |
| CFE | 35.3% | ≈0.6 | high (needs both selectivity + exit) |

### 2026-Q1 regime shift confirmed by data

`tick_quality.py` (after column-order bug fix in `audit-fixes-17`)
reveals **the data is BETTER in 2026 (denser, fewer gaps) but more
EXPENSIVE to trade (wider spreads)**:

```
month     |spread_med|  ticks/mo   tpm_med   gap_p95
                pt
2024-03      0.34       2.7M         75      2465ms
2024-12      0.44       5.1M        155      1259ms
2025-09      0.57       5.8M        161      1155ms
2026-01      0.70       9.1M        280       653ms
2026-02      0.92       7.5M        220       755ms     ← peak spread
2026-03      0.79       9.4M        270       702ms
2026-04      0.71       5.8M        216       810ms
```

Median spread ~2.7× wider in 2026-Q1 vs 2024-Q1. The engines'
`apply_realistic_costs(tr, 3.0, 100.0)` assumes 3-pip = 0.3pt fixed
spread. Real 2026-Q1 spread is 0.7-0.9pt = 7-9 pips. **Cost is
understated by 2-3× in every 2026 trade reported.**

### Why HBG over-fires in 2026-Q1 specifically

- Denser ticks → more compressions detected per unit time
- Wider spreads → each compression's range is larger absolutely (but
  the gate filters by *relative* range vs median)
- `m_range_history` post-fix-16 reflects the new high-density regime, so
  the median rises with it; the gate becomes uniformly easy to pass
- Result: 417 / 616 / 295 trades in Jan / Feb / Mar of 2026 (vs ~5 / month baseline)

---

## Three options to test, in priority order

### Option 3 (do first, no engine touch) — `cost_recalc.py`

Highest-leverage measurement we can build without a rebuild. Re-cost
every existing trade CSV using the spread regime data:

```python
# Pseudo:
for row in trade_csv:
    actual_spread = lookup_median_spread_for(month_of(entry_ts))
    actual_cost   = 2 * actual_spread * size  # round-trip
    corrected_net = gross_pnl - actual_cost
```

The spread regime is already in `tick_q.csv` from the
`tick_quality.py` run. Or compute on the fly. Output: per-engine
"reported P&L was $X, corrected for actual spread is $Y, gap = $Z."

**Validation**: pre-2026 numbers should change very little (spreads were
near 0.3pt). 2026-Q1 numbers should worsen substantially. The
*delta* between reported and corrected is the cost-mis-estimation that
needs fixing in the engine.

**Files**: write `backtest/cost_recalc.py`. Read `tick_q.csv` for
per-month spread baseline; or recompute from the source tick file.

### Option 1 (do second) — `SpreadRegimeGate.hpp`

Add a 1-hour rolling median spread tracker. If median spread > 0.5pt,
all engines sit out. ~30 lines new file + one call site per engine in
`tick_gold.hpp` and possibly `tick_indices.hpp`.

```cpp
// SpreadRegimeGate.hpp -- minimal sketch
class SpreadRegimeGate {
    std::deque<std::pair<int64_t, double>> m_recent;  // (ts_ms, spread)
    static constexpr int64_t WINDOW_MS = 3600 * 1000;
    static constexpr double  MAX_MEDIAN_SPREAD = 0.5;
public:
    void on_tick(int64_t ts_ms, double spread);
    bool can_fire() const;  // false when 1h median spread > MAX_MEDIAN_SPREAD
};
```

Call sites:
- `include/tick_gold.hpp` — gate every engine in `gold_tick_handler()`
  alongside the existing `gold_any_open` check
- The three BT harnesses pick up the gate automatically via the engine
  headers; no harness changes needed if the gate is invoked from inside
  each engine's on_tick (preferred) rather than the tick handler

**Expected effect**: 2026-Q1 trade counts collapse near zero. Total P&L
improves substantially. Pre-2026 P&L mostly unchanged.

**Validation path**: rebuild + re-run all three BTs:
```
g++ -O2 -std=c++17 -I include -o cfe_duka_bt backtest/cfe_duka_bt.cpp
g++ -O2 -std=c++17 -I include -o mce_duka_bt backtest/mce_duka_bt.cpp
g++ -O2 -std=c++17 -I include -o hbg_duka_bt backtest/hbg_duka_bt.cpp
./cfe_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv | tee cfe_post_spread_gate.log
./mce_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv | tee mce_post_spread_gate.log
./hbg_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv | tee hbg_post_spread_gate.log
```

This is core-engine code. Per the user's standing instruction, do NOT
modify core code without explicit approval. Propose the SpreadRegimeGate
header in chat with full file contents, get the user's "apply" word,
then Edit + commit + push.

### Option 2 (only if Option 1 not enough) — per-engine adaptive parameters

All engines scale parameters by current spread:

- **HBG**:
  - `MIN_RANGE_eff = max(MIN_RANGE, 8 * spread)`
  - `SL_BUFFER_eff = SL_BUFFER + 0.5 * spread`
  - `min_tp = TP_RR * sl_dist + 2 * spread + 0.12`
- **MCE**: scale `m_consec_sl` thresholds + dollar-stop bands by spread
- **CFE**: scale `EARLY_FAIL_SOFT_PT` by spread

~50-100 lines per engine. Walk-forward calibration per change.

---

## Open task list

```
#1   Commit BT files                  ✅ done in 95e7b1a
#2   tick_quality.py                   ✅ done; column-order bug fixed in fixes-17
#3   Tier-2 BT (MM/DP/BB)              ❌ engines were intentionally culled
#4   CFE 2026-03 reject                ✅ DIAGNOSED — HMM gate prime suspect
#5   HBG S47 T4a ratchet               ✅ APPLIED in fixes-16; validated
#6   MCE adaptivity diagnostics        ✅ done; ran on real data
#7   CFE iter-15 diagnostics           ✅ done; STAGNATION-tighten > 90s gate
#8   2026-Q1 over-fire investigation   ✅ DIAGNOSED — real regime shift
#9   Tune HBG EXPANSION_MULT           ⏸ may be subsumed by Options 1/2
#10  Build hbg_diag.py                  ⏸ low priority after spread gate
#11  Build cost_recalc.py              📍 NEXT (Option 3 — no rebuild)
#12  Implement spread-regime sit-out   📍 AFTER #11 (Option 1, core code)
```

Plus: per-engine adaptive parameters (Option 2) — open as Task #13 if
Option 1 is insufficient.

---

## Side bugs to track but not fix yet

1. **CFE MAE column = 0 across all trades** in `cfe_duka_bt_trades.csv`.
   The engine isn't recording MAE despite tracking it internally. This
   is a separate bug; doesn't block iter-15 but should eventually be
   fixed. Look for `tr.mae = pos.mae` or equivalent in
   `CandleFlowEngine.hpp` close paths.

2. **Index whipsaw fix still pending** (per AM handoff §5). NAS100 /
   IndexFlow ↔ HybridBracketIndex needs an `index_any_open` gate
   analogous to `gold_any_open`. ~30 lines in `tick_indices.hpp`. Low
   urgency, indices are net-flat.

3. **Apply_realistic_costs uses fixed 3-pip spread**. The HBG harness
   captures `spread_at_entry` in `LivePos` (per fixes-15 S51 1A.1.a) but
   the cost model doesn't consume it. Fixing this to use real per-trade
   spread is part of Option 2 / Option 3.

---

## Existing diagnostics — what to run first thing in next session

```
# 1. Read this file (you're doing that now)
cat /Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_pm.md
# also read the AM doc:
cat /Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29.md
cat /Users/jo/omega_repo/KNOWN_BUGS.md

# 2. Confirm repo state
cd /Users/jo/omega_repo
git pull --ff-only
git log --oneline -5

# 3. Verify the existing diagnostics still run cleanly
python3 backtest/cfe_iter15_diag.py cfe_duka_bt_trades.csv | tail -30
python3 backtest/mce_diag.py        mce_duka_bt_trades.csv | tail -30

# 4. Spread regime data is in tick_q.csv (or re-run tick_quality.py if missing)
ls -la tick_q.csv

# 5. Then build cost_recalc.py (Option 3) and proceed
```

---

## Empirical numbers to remember (for cross-checking)

- **HBG post-fix-16**: 1,590 trades / -$5,192 / -$3.27/tr / 25.8% WR / max DD $5,216
  - SL_HIT: 850, TP_HIT: 165, TRAIL_HIT: 575
  - 2026-01/02/03 = 1,328 trades (84% of total), -$4,366 (84% of loss)
- **MCE current**: 345 trades / -$259 / -$0.75/tr / 18.8% WR
  - MAX_HOLD: 25 (only profitable exit, +$17.19/tr)
  - SL_HIT: 320 (-$2.15/tr)
  - 2026-04 collapse: -$201 / -$6.09/tr (worst month)
- **CFE current**: 1,457 parents / -$1,400 / -$0.96/tr / 35.3% WR
  - MFE bucket >2pt: profitable (+$2.14/tr × 280 parents)
  - 2026-02 outlier in 0.5-1pt bucket: 42/125 trades, 47% of bucket loss
  - MAE column = 0 (separate bug)
- **Spread regime**:
  - 2024-03: 0.34pt
  - 2025-09: 0.57pt
  - 2026-01: 0.70pt
  - 2026-02: 0.92pt (peak)
  - 2026-04: 0.71pt
- **Cost assumption in engines**: 0.3pt fixed → understated by 2-3× in 2026

---

## How to validate cost_recalc.py output

1. Pre-2026 trades: corrected P&L should track reported P&L within ~5%
   (spreads were ~0.3pt as assumed)
2. 2026-Q1 trades: corrected P&L should be 30-60% worse than reported
   (spread is 2-3× the assumed cost)
3. Total HBG corrected P&L for 2026-Q1: expect roughly -$6,500 to -$7,500
   (vs reported -$4,366)

If those magnitudes don't materialize, the cost-recalc methodology is
wrong and needs review before proceeding to Option 1.

---

## User preferences (carry forward)

- Always provide full code files, not snippets/diffs
- Warn at 70% chat context with summary
- Warn before time/session blocks
- Never modify core code without explicit instruction
  - Options 1 and 2 are core-code changes — propose first, get "apply" approval
  - Option 3 is non-core, just a new analysis script
- Use the PAT without arguments — stored at `/Users/jo/omega_repo/.github_token`
- Email: kiwi18@gmail.com (committer identity)
- Name: jo

---

## What this session ran out of context to do

The full Option 3 → 1 → 2 sequence. The user explicitly authorised
proceeding with all three; they wanted handoff first because we hit
~75% context. Pick up Option 3 immediately on session start.
