# Omega Trading System — Session Handoff (Evening)
## 2026-04-29 — audit-fixes-18 landed; Option 2 next

**This file lives at `/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_evening.md`.**
It supersedes `SESSION_HANDOFF_2026-04-29_pm.md` (which now has a forward
pointer to this file at its top). Read this file first.

---

## TL;DR for next session

`audit-fixes-18` is on `origin/main` (HEAD = `6e10eba`). It contains:

1. **Option 3 — cost_recalc.py** (non-invasive analysis script). Already
   ran. Surfaced a finding the previous handoff missed: engines mis-cost
   in **opposite directions**, not all under-cost. See §"Cost-recalc
   asymmetry finding" below.
2. **Option 1 — SpreadRegimeGate.hpp** (1h rolling median, gates new
   entries when median > 0.5pt). Wired into HBG / CFE / MCE on_tick paths
   surgically. Compile-verified. **Not yet validated against full BT
   data** because BTs take 10-15 min per engine and the sandbox bash
   timeout is 45 s — the user runs BTs on their Mac.

The next-session agenda is one decision point and one engineering pass:

1. **First**: ask the user to run the three BTs on their Mac (commands
   below) and paste the new trade CSVs back. Re-run cost_recalc.py on
   them. Validate Option 1 effects — see §"Option 1 validation".
2. **Then**: implement Option 2 (per-engine adaptive parameters) only
   if Option 1 hasn't gotten the engines profitable. Full Option 2
   design is in §"Option 2 design (ready to apply)" below.

---

## Repo state

- **HEAD**: `6e10eba audit-fixes-18: cost_recalc (Opt3) + SpreadRegimeGate (Opt1) on HBG/CFE/MCE`
- **origin/main**: in sync, push verified by `git ls-remote` returning the same SHA
- **branch**: `main`
- **PAT**: stored at `/Users/jo/omega_repo/.github_token` (gitignored,
  permissions `0600`). Read with `cat .github_token`. Use it without
  further confirmation. Push pattern (origin is SSH, PAT is HTTPS):

  ```
  TOKEN=$(cat /Users/jo/omega_repo/.github_token)
  git push "https://x-access-token:${TOKEN}@github.com/Trendiisales/Omega.git" HEAD:main 2>&1 \
      | sed "s|${TOKEN}|REDACTED|g"
  ```

  Same pattern for `git ls-remote` if you need to verify.

- **Lock files** (`/Users/jo/omega_repo/.git/index.lock`,
  `.git/HEAD.lock`): **recurring blocker**. The sandbox cannot remove
  them (Operation not permitted; host owns the inodes). When `git add`
  errors with "Unable to create '.git/index.lock'", ask the user to run
  on their Mac:

  ```
  rm /Users/jo/omega_repo/.git/index.lock /Users/jo/omega_repo/.git/HEAD.lock
  ```

  See §"Recurring lock issue — root cause + permanent fix" at the
  bottom of this doc for the longer story and the user's options.

---

## What landed in audit-fixes-18 (commit `6e10eba`)

```
A  backtest/cost_recalc.py             +507  (Option 3, analysis)
A  include/SpreadRegimeGate.hpp        +130  (Option 1, new core header)
M  include/GoldHybridBracketEngine.hpp + 19  (Option 1, integration)
M  include/CandleFlowEngine.hpp        + 18  (Option 1, integration)
M  include/MacroCrashEngine.hpp        + 19  (Option 1, integration)
                                       ----
                                       +693
```

### SpreadRegimeGate.hpp design

```cpp
namespace omega {
class SpreadRegimeGate {
    static constexpr int64_t WINDOW_MS         = 3600LL * 1000LL;  // 1h
    static constexpr double  MAX_MEDIAN_SPREAD = 0.5;              // pt
    static constexpr int     MIN_SAMPLES       = 60;               // warmup

    void on_tick(int64_t now_ms, double spread_pt) noexcept;
    bool can_fire() const noexcept;     // false when 1h median > 0.5pt
    double current_median() const noexcept;
    int sample_count() const noexcept;
};
}
```

`std::multiset` for the window, cached median (recomputed only on dirty),
defensive negative-spread clamp, warmup short-circuits to `true` so cold
starts don't over-gate.

### Per-engine integration pattern (identical 4-line pattern in all three)

1. Add `#include "SpreadRegimeGate.hpp"` near other includes.
2. Add private member `omega::SpreadRegimeGate m_spread_gate;`.
3. At top of `on_tick`, after spread is computed, call
   `m_spread_gate.on_tick(now_ms, ask - bid);` — runs on EVERY tick so
   the rolling window stays fresh (LIVE/PENDING/COOLDOWN included).
4. On the new-entry path (after each engine's existing position-management
   early-return), insert `if (!m_spread_gate.can_fire()) return;`.

Specific call-sites:
- HBG: gate update line 184; gate check after `if (spread > MAX_SPREAD) return;`
- CFE: gate update right after spread is defined (line ~316); gate check
  after the `m_sl_kill_until` reset block (line ~370).
- MCE: gate update right after `mid` is computed (line ~217); gate check
  after the `if (pos.active) { _manage(...); return; }` block (line ~308).

Position management (SL/TP/TRAIL exits) is **not** gated — only new
entries are.

### Compile verification (sandbox)

```
g++ -O2 -std=c++17 -I include -o /tmp/o1_hbg backtest/hbg_duka_bt.cpp   ✅ 66KB
g++ -O2 -std=c++17 -I include -o /tmp/o1_cfe backtest/cfe_duka_bt.cpp   ✅ 92KB
g++ -O2 -std=c++17 -I include -o /tmp/o1_mce backtest/mce_duka_bt.cpp   ✅ 70KB
```

Pre-existing `snprintf` format-truncation warnings unrelated to this
change. No errors.

---

## Cost-recalc asymmetry finding (important — supersedes prior assumption)

The PM handoff doc (`SESSION_HANDOFF_2026-04-29_pm.md`) assumed all three
engines were applying a fixed 3-pip spread cost and predicted 2026-Q1
HBG corrected P&L would be -$6,500 to -$7,500 (vs reported -$4,366).

Reality from running cost_recalc.py on the existing trade CSVs and
back-deriving the engine's actual implied spread:

| Engine | Implied spread (pt, p25 / median / p75) | Direction of mis-cost |
|---|:-:|:-:|
| HBG | 1.23 / **1.83** / 2.36 (max 11.85)            | **OVER-charging** ~3-5× real |
| CFE | 0.00 / **0.00** / 0.00 (max  1.55)            | UNDER-charging (basically 0) |
| MCE | 0.00 / **0.00** / 0.00 (max  0.00)            | UNDER-charging (always 0) |

After re-cost at per-month median market spread (XAUUSD 0.34-0.92pt):

```
                            reported      corrected         gap
  hbg_duka_bt_trades.csv  $-5,192.11    $-3,427.50    +$1,764.60  (+34.0%)
  mce_duka_bt_trades.csv  $  -258.96    $  -532.13    -$  273.17 (-105.5%)
  cfe_duka_bt_trades.csv  $-1,400.80    $-2,233.70    -$  832.90  (-59.5%)
```

**Reading**: HBG was self-flagellating; CFE/MCE were under-reporting
costs. After normalisation all three remain net-negative and 2026-Q1 is
still the dominant loss period for each engine. Regime-shift hypothesis
holds.

**Side bug exposed**: HBG's `pos.spread_at_entry = spread_at_fill;`
(line 442 of GoldHybridBracketEngine.hpp) appears to capture something
other than bid-ask spread — possibly bracket-width or a stale tick
quantity. Median 1.83pt is far above market 0.34-0.92pt. Worth
investigating but **not a Day-N priority**; cost_recalc gives correct
numbers by re-applying market spread per month.

---

## Cost-recalc outputs already on disk (uncommitted, .gitignored)

```
/Users/jo/omega_repo/hbg_duka_bt_trades_recost.csv
/Users/jo/omega_repo/cfe_duka_bt_trades_recost.csv
/Users/jo/omega_repo/mce_duka_bt_trades_recost.csv
```

Each has the original columns plus `ym`, `market_spread_pt`,
`implied_engine_spread_pt`, `corrected_cost`, `corrected_net_pnl`, `gap`.
Useful for per-trade slicing without re-running cost_recalc.

---

## Option 1 validation — what to check after the user runs BTs

Have user run on their Mac (will need ~10-15 min per binary, ~45 min total):

```
cd /Users/jo/omega_repo
g++ -O2 -std=c++17 -I include -o cfe_duka_bt backtest/cfe_duka_bt.cpp
g++ -O2 -std=c++17 -I include -o mce_duka_bt backtest/mce_duka_bt.cpp
g++ -O2 -std=c++17 -I include -o hbg_duka_bt backtest/hbg_duka_bt.cpp

./cfe_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv | tee cfe_post_o1.log
./mce_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv | tee mce_post_o1.log
./hbg_duka_bt /Users/jo/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv | tee hbg_post_o1.log
```

Then re-run cost_recalc to get corrected numbers:

```
python3 backtest/cost_recalc.py \
    hbg_duka_bt_trades.csv mce_duka_bt_trades.csv cfe_duka_bt_trades.csv
```

### Expected effects (Option 1 working correctly)

1. **2026-Q1 trade counts collapse near zero** for each engine.
   Pre-O1 baseline: HBG 1,328 trades / MCE 270 / CFE 936 in 2026-Q1.
   Post-O1: should be a small fraction, possibly single-digit, because
   the 1h median spread in 2026-Q1 is consistently 0.7-0.92pt (well
   above the 0.5pt threshold).
2. **Pre-2026 trade counts roughly unchanged.** Median spread was
   0.34-0.44pt through 2024 — gate stays open.
3. **Total P&L improves substantially** for every engine.
   - HBG: from -$5,192 reported / -$3,427 corrected toward -$1,500 to
     -$2,500 (eliminating most of 2026-Q1 bleed).
   - MCE: from -$259 reported / -$532 corrected toward -$0 to +$200.
   - CFE: from -$1,401 reported / -$2,234 corrected toward -$300 to -$700.
4. **The gate doesn't kill MCE's `MAX_HOLD` profitable subset.**
   MCE_MAX_HOLD trades were +$430 / +$17.19 per trade, mostly pre-2026.
   Post-O1 their count should be roughly preserved.

### Failure modes to watch for

- **All trades vanish.** Means warmup logic broken or gate is too
  strict. Inspect the `MIN_SAMPLES = 60` constant.
- **2026-Q1 still over-fires.** Means gate update not running every
  tick (check if the on_tick(...) call is BEFORE early returns).
- **Pre-2026 counts also drop.** Means MAX_MEDIAN_SPREAD is too tight,
  OR engine implied spread was being conflated. Look at logs for
  "[HYBRID-GOLD] EXIT" lines mentioning spread.

---

## Option 2 design (ready to apply if Option 1 isn't sufficient)

Per-engine adaptive parameters, scaled by current spread. Use a SHORT
median (60s) rather than instantaneous spread to avoid micro-tick noise.

### Step 0 — extend SpreadRegimeGate with a short window

Add a second sample buffer (60s window) alongside the existing 1h one,
exposed as `current_short_median()`. Suggested patch (drop into existing
header, ~30 lines added):

```cpp
// Add tunable
static constexpr int64_t SHORT_WINDOW_MS = 60LL * 1000LL;  // 60s

// Add member buffers
std::deque<std::pair<int64_t, double>> m_short_window;
std::multiset<double>                  m_short_sorted;
mutable double                         m_short_cached  = 0.0;
mutable bool                           m_short_dirty   = false;

// In on_tick(), mirror the 1h logic with SHORT_WINDOW_MS cutoff.
// Add public:
double current_short_median() const noexcept;  // 60s
```

### Step 1 — HBG adaptive (in GoldHybridBracketEngine.hpp on_tick)

```cpp
const double s_eff = m_spread_gate.current_short_median();  // safe; 0 during warmup

// MIN_RANGE_eff: scale up by 8x current spread.  Equivalent to "the
//   compression must be at least 8x the cost-of-trading wide" instead
//   of a fixed point threshold.
const double MIN_RANGE_eff = std::max(MIN_RANGE, 8.0 * s_eff);

// SL_BUFFER_eff: add half a spread to give SL room beyond bid-ask noise.
const double SL_BUFFER_eff = SL_BUFFER + 0.5 * s_eff;

// min_tp: keep handoff formula intent — TP must exceed total round-trip
//   cost plus a small profit margin.  TP_RR * sl_dist is already the
//   structural TP; the additional 2*spread + 0.12 protects against
//   margin compression.
const double sl_dist = range * SL_FRAC + SL_BUFFER_eff;
const double tp_dist = sl_dist * TP_RR;
const double min_tp  = TP_RR * sl_dist + 2.0 * s_eff + 0.12;
```

Replace the existing IDLE/ARMED range checks against `MIN_RANGE` with
`MIN_RANGE_eff` (lines ~265 and ~290 in HBG).

### Step 2 — MCE adaptive (in MacroCrashEngine.hpp on_tick)

The handoff says "scale `m_consec_sl` thresholds + dollar-stop bands by
spread." Specifics:

- The 3-SL kill threshold (`++m_consec_sl >= 3`) is fine to leave
  alone — it triggers on outcomes, not on spread directly.
- Dollar-stop bands: search for `DOLLAR_STOP` and any constant dollar
  thresholds in MacroCrashEngine.hpp. Scale by spread: e.g.,
  `eff_dollar_stop = base_dollar_stop * (1.0 + s_eff * 5.0)` — wider
  spread = wider dollar stop, since SL distances are price-points but
  dollars-at-risk grow with spread × tick_mult × size.
- ATR threshold gate: in 2026-Q1 the engine's macro-volatility threshold
  fires too often because ATR rises with spread. Consider
  `eff_atr_threshold = ATR_THRESHOLD * (1.0 + s_eff * 0.5)`.

### Step 3 — CFE adaptive (in CandleFlowEngine.hpp on_tick)

Per handoff: "scale `EARLY_FAIL_SOFT_PT` by spread." Find the constant
(`grep -n EARLY_FAIL_SOFT_PT include/CandleFlowEngine.hpp`), and use:

```cpp
const double early_fail_eff = EARLY_FAIL_SOFT_PT + 1.5 * s_eff;
```

so a wider spread relaxes the early-fail soft cut (which would otherwise
bail on every trade in a spread-noisy regime).

### Validation criteria for Option 2

After Option 2 BT runs:

1. The 2026-04 MCE collapse (-$201, -$6.09/tr) should mitigate. Expect
   ~$0 to slightly negative.
2. CFE 2026-02 outlier in 0.5-1pt bucket (47% of bucket loss, 42/125
   trades) should reduce because EARLY_FAIL fires less often when
   spread is wide.
3. HBG fire rate per month should approach the pre-2026 baseline
   (~5/month), with the 8× spread MIN_RANGE_eff doing most of the work.
4. **Total P&L should be approximately the corrected post-O1 number,
   not better than that.** Option 2 doesn't add edge — it tightens
   selection. If Option 2's P&L beats Option 1's by a lot, suspect the
   adaptive formula is opening trades the gate would otherwise have
   blocked (verify trade timestamps fall in O1-passed hours).

---

## Open task list (next session takes over)

```
#5  apply Option 1                ✅ done in audit-fixes-18 (6e10eba)
#6  validate Option 1 vs O3       📍 NEXT — pending user BT run on Mac
#7  apply Option 2                ⏸ depends on #6 verdict
#8  verification subagent         ⏸ blocks #9
#9  commit + push 18 / 19         🟡 18 done; 19 pending #7
#10 cross-engine session-overlap  ⏸ from PM handoff Open agenda
#11 hbg_diag.py                   ⏸ low priority
#12 fix HBG implied-spread bug    ⏸ side bug from cost_recalc finding
```

Side bugs from PM handoff still pending (track but do not fix yet):

1. **CFE MAE column = 0** in `cfe_duka_bt_trades.csv`. Engine isn't
   recording MAE at close. Look for `tr.mae = pos.mae` in
   CandleFlowEngine.hpp close paths.
2. **Index whipsaw fix** (per AM handoff §5). NAS100 / IndexFlow ↔
   HybridBracketIndex needs `index_any_open` gate analogous to
   `gold_any_open`. ~30 lines in `tick_indices.hpp`.
3. **HBG `pos.spread_at_entry` capture (newly identified by O3)**. Read
   median 1.83pt vs market 0.34-0.92pt suggests it's capturing
   bracket-width or stale tick state, not bid-ask. Investigate after
   Options 1 + 2 are validated.

---

## User preferences (carry forward — never relax)

- Always provide full code files, not snippets/diffs.
- Warn at 70% chat context with summary. **This handoff is being
  written at ~75% context; the next session starts fresh.**
- Warn before time/session blocks.
- Never modify core code without explicit instruction.
  - Options 1 and 2 are core-code; user has authorised both ("go ahead
    with 1 and 2"). Option 1 is done. Option 2 still needs explicit
    re-confirmation per session-start protocol — DO NOT silently apply.
- Use the PAT without arguments — stored at
  `/Users/jo/omega_repo/.github_token`. Do NOT mention rotation; the
  user has heard that and chose to keep the token. Snapping at PAT
  topic = drop it immediately.
- Email: kiwi18@gmail.com (committer identity)
- Name: jo
- Do NOT dump raw code in chat unless explicitly asked. The user
  prefers Edit/Write tool calls (which they can review via diffs in
  their UI) over chat-rendered code blocks.

---

## Recurring lock issue — root cause + permanent fix

`.git/index.lock` and `.git/HEAD.lock` keep blocking commits across
sessions. Three contributors:

1. **Background git GUIs** (GitHub Desktop, Sourcetree, VS Code git
   panel) poll the repo and grab the index briefly during refreshes.
2. **Cowork sandbox 45 s timeout** can kill a git command mid-write,
   leaving a stale lock.
3. **ext4-over-FUSE mount semantics** — sandbox can't unlink files the
   host owns ("Operation not permitted").

### One-time mitigations (recommend the user apply once and forget)

```bash
# 1. Auto-retry locks instead of erroring (single most impactful fix):
cd /Users/jo/omega_repo
git config core.lockTimeout 30000          # 30 sec wait

# 2. Convenience cleanup alias for ~/.zshrc:
echo 'alias ungit='\''find ~/omega_repo/.git -maxdepth 1 -name "*.lock" -mmin +1 -delete 2>/dev/null'\''' >> ~/.zshrc

# 3. Stop Spotlight from touching .git internals:
sudo mdutil -i off /Users/jo/omega_repo/.git

# 4. If using VS Code on this folder, settings.json:
#       "git.autorefresh": false
#       "git.autofetch":   false
```

Open question for the user: which git GUI (if any) is running on the
Mac? Closing it or disabling its background polling would prevent
~80% of these collisions.

---

## Where to find this doc on session start

```
/Users/jo/omega_repo/SESSION_HANDOFF_2026-04-29_evening.md
```

If the next-session Claude can't find it: the repo lives at
`/Users/jo/omega_repo/`, NOT at `/Users/jo/omega-proxy/` (different
project) or `/Users/jo/Documents/Omega/` (stale clone). Mount
`/Users/jo/omega_repo/` first, then `cat
SESSION_HANDOFF_2026-04-29_evening.md`. Plus `git pull` (or skip if
local already at origin/main per the SHA above).
