# Harness Fidelity Checklist

**Purpose:** prevent the recurring "backtest looks great, live loses money" failure.
Same pattern hit Omega twice in 2026 alone:

| Date | Engine | Backtest | Live | Root cause |
|---|---|---|---|---|
| 2026-05-11 | GoldMicroScalper rk12 | WR 92.5%, PF 4.40, +$203/oz/28d | WR 7.1%, −$1,237/28d | Harness filled TP at `tp_px` not at touch (`microscalper_crtp_sweep.cpp:671`). 79% of phantom edge. |
| 2026-05-27 | XauThreeBar30m + slope gate | net −$5,000 (broken) | (would have been good) | Synthetic intra-bar tick path triggers S63 `LOSS_CUT_PCT=0.05` ($1.75 cut on $3500 entry) on every trade. Harness mis-modeled vs live tick flow. |

Both bugs cost research cycles and risked deploying broken engines. This checklist exists so the next research session catches the bug pattern in **minutes**, not after a multi-week deploy.

---

## Mandatory checks BEFORE trusting any harness output

### Check 1 — Baseline match against handoff numbers

If a handoff document reports a baseline (e.g. "+$1488/25mo, PF 1.27"), the FIRST run of the harness with default config MUST produce that number ±10%. If it doesn't:

- **STOP. Diagnose.** Do not patch config and re-run hoping it fixes itself.
- The mismatch IS the signal: either (a) the data is different, (b) the engine has changed, (c) the harness has changed, or (d) the harness was always broken.
- Walk the engine's exit-reason counts. If exits are concentrated in one reason (e.g. 985/1100 `LOSS_CUT`), that's the artifact entry point.

**Concrete protocol:**
```bash
./backtest/<harness> --csv <known_dataset> --quiet > out.txt
# Compare summary to handoff. If delta > 10%, STOP and read the exit reason histogram.
grep -A2 "EXIT REASON COUNTS" out.txt
```

### Check 2 — Exit reason histogram sanity

For ANY backtest, after run, eyeball the exit reason distribution:

| Pattern | Likely meaning |
|---|---|
| > 70% on a single reason | Harness artifact almost certain — one exit logic is dominating |
| 0 `TP_HIT` on a TP-based strategy | TP unreachable in synthetic ticks (path too tight) or BE/trail closing first |
| > 50% `LOSS_CUT` on S63-enabled engines | Synthetic-tick adverse-first path triggers PCT cuts immediately — **disable PCT cuts in harness configs only** (production keeps them) |
| `TIME_STOP` on engine with time-stop=0 | Should not happen; harness vs engine cfg drift |
| 100% `FORCE_CLOSE` at end | Engine never naturally exits; check SL/TP geometry |

### Check 3 — Fill price audit

Walk the engine's `_close()` and `_manage_open()` paths in the source. For EVERY exit, what price is used as the fill?

**Sanity rules:**
1. TP fill MUST cross spread: `bid` if long, `ask` if short. Never `tp_px` directly.
2. SL fill MUST cross spread on the worse side: `sl_px - slip` if long, `sl_px + slip` if short.
3. BE/trail same as SL.
4. FORCE_CLOSE / time stop at `bid`/`ask` not `mid` or `close`.

If the harness fills at `tp_px` or `sl_px` literally, every win is overstated by half-spread + slip. On XAU at 0.25-0.50pt spread, that's 30-60% phantom edge over many trades.

### Check 4 — S63 in-flight PCT cut compatibility

Engines using S63 pattern (`LOSS_CUT_PCT`, `BE_ARM_PCT`, `BE_BUFFER_PCT`) trigger on early adverse moves measured in % of entry. These PCT thresholds are tiny on instruments like XAU (`LOSS_CUT_PCT = 0.05` → 0.05% → $1.75 on $3500 entry).

**If the harness uses synthetic intra-bar tick path** (open → adverse extreme → favourable extreme → close):
- The first tick is the adverse extreme → triggers LOSS_CUT_PCT immediately.
- Real live flow has many ticks per bar, most NOT extreme adverse → LOSS_CUT only fires on cold-direct losers.
- **Fix:** disable `LOSS_CUT_PCT/BE_ARM_PCT/BE_BUFFER_PCT` in harness configs ONLY. Engine config in `engine_init.hpp` stays at production values. Document the toggle in harness source so it's not lost.

**Engines with S63 pattern** (audit list, 2026-05-27):
- `IndexFlowEngine` (g_iflow_sp/nq/nas/us30) — `LOSS_CUT_PCT=0.07`
- `XauThreeBar30mEngine` — `LOSS_CUT_PCT=0.05`
- `VWAPReversionEngine` (originating pattern)
- 29 engine headers contain at least one of the three PCT fields. Audit on demand:
  ```bash
  grep -l "LOSS_CUT_PCT\|BE_ARM_PCT\|BE_BUFFER_PCT" include/*.hpp
  ```

### Check 7 — POSIX vs MSVC cross-compilation symbols

The Mac canary build catches most issues but does not exercise the Windows
MSVC compiler path. Symbols that POSIX/Mac headers define implicitly may
be undefined on MSVC, causing the VPS build to break only when the operator
runs `OMEGA.ps1 deploy`.

Known traps to avoid in new engine headers:
- `M_PI`, `M_E`, `M_LN2`, etc. — POSIX-only macros from `<cmath>`. Use
  `constexpr double TWO_PI = 6.28318530717958647692...;` instead.
- `strdup`, `getline` — POSIX C extensions. Use `std::string` ops or
  `_strdup` (MSVC variant).
- `__attribute__((...))` — GCC/Clang-specific. Use `[[...]]` attributes
  or guard with `#ifdef __GNUC__`.
- `<unistd.h>`, `<sys/socket.h>` — POSIX-only headers. Guard with
  `#ifdef _WIN32` and use Winsock2 equivalents (already established
  pattern in `IbkrDomConsumer.hpp`).

**Detection:** the `Build Verification` section of CLAUDE.md says the Mac
canary is the sufficient check — but only for the OmegaBacktest target,
which does NOT include all production sources. Headers that ship with
the main Omega.exe build path get exercised only on Windows. **The first
operator deploy attempt is the real test for any new header.** Watch for
the build failure paste; revert or hotfix immediately if it breaks.

History: caught 2026-05-27 — `M_PI` in `XauM30HmmGate.hpp` broke the
Windows MSVC build but compiled clean on Mac clang. Hotfix in commit
`babaaae7`. Pattern: replace POSIX-only with `constexpr` literal at
declaration site, not via `#define _USE_MATH_DEFINES`.

### Check 6 — Causality of any state-classifier output

If your strategy/gate consumes the output of an HMM, regime classifier, or
any sequence model that has both forward and backward inference modes, the
backtest MUST use the forward-only (causal) pass.

`hmmlearn.GaussianHMM.predict()` and `predict_proba()` run Viterbi /
forward-backward over the **entire sequence** — these use FUTURE
observations to label PAST bars. Trades at bar `t` cannot access future
data, so any backtest that uses smoothed labels is look-ahead biased.

**Symptom:** unrealistically large lift from a "regime filter" that looks
trivial to implement. The 2026-05-27 HMM gate test showed +79% net lift
with smoothed labels; the causal forward-pass-only implementation showed
+20% lift. Same data, same model, same gate — the +59pp gap was pure
look-ahead.

**Fix:** implement the forward pass manually, OR use the model's
`score_samples` per step with a growing prefix, OR re-fit at every bar
on the expanding window (slow but bulletproof).

Reference impl: `/Users/jo/Tick/mid_freq_research/hmm_causal_test.py`,
`causal_state_path()` function (~30 lines, log-space recursion for numerical
stability).

State agreement between smoothed and causal labels in the 2026-05-27 test
was 88.9% — meaning 11% of bars (the regime transitions) carry the
look-ahead bias. Concentrated at exactly the moments your gate is making
its biggest decisions.

### Check 5 — Synthetic tick path realism

If the harness writes its own tick path (instead of replaying a tick CSV), the path matters:

| Path | Pros | Cons |
|---|---|---|
| `open → low → high → close` (long) | Conservative on TP (open hits SL first if adverse) | Triggers PCT cuts immediately |
| `open → high → low → close` (long) | Optimistic (TP hits first) | Overstates edge |
| Tick CSV replay | Realistic | Slow, needs real tick data |
| Random walk within (low, high) | Compromise | Adds noise |

The XauThreeBar30mBacktest uses `O → adverse → favourable → close` which is the most conservative AND the most LOSS_CUT-triggering. Acceptable IF S63 PCT cuts disabled in the harness; otherwise toxic.

---

## What to do at the START of every research session that touches a harness

1. Read the harness's `make_engine_*` config blocks. Compare to `engine_init.hpp` production config. Note any drift (e.g. `long_only`).
2. Run the harness ONCE with default config. Eyeball:
   - Total trade count (matches handoff?)
   - Net PnL sign and magnitude (matches handoff ±10%?)
   - Exit reason histogram (any one reason >70%?)
3. If anything is off, **diagnose before doing any research**. Walk the engine source. Read the manage path line by line.
4. Only AFTER baseline matches do you trust the harness for variant testing (gate ON/OFF, parameter sweeps, etc.).

---

## Known harness modifications and why

Document any harness-side overrides here so future sessions don't have to rediscover them.

### threebar30m_xau_S35P3_backtest.cpp (2026-05-27)

- **S63 PCT cuts disabled** in `make_engine_baseline()`, `make_engine_protected()`. Synthetic intra-bar tick path triggers them on every trade. Production keeps them at engine_init.hpp values. Without this disable, every config shows net −$3,000+ with 70%+ `LOSS_CUT` exits.
- **`long_only=true`** added to baseline + TUNED configs. Matches S96 production (short side PF=0.84, OOS PF=1.24 long-only). Harness predated S96.
- Slope gate variants `make_engine_tuned_slope(N)` for N ∈ {8, 10, 12} added.

---

## Anti-patterns to avoid

- **"Patch config and re-run":** if the first run is way off baseline, more config tweaks won't fix structural harness issues. Diagnose first.
- **Trusting one config delta:** if config A → −$5000 and config B → −$4500, both are broken. Δ between two broken numbers tells you nothing.
- **Ignoring the exit histogram:** the histogram IS the diagnostic. Stare at it before any numerical interpretation.
- **Assuming "the harness is well-tested":** the MicroScalper harness was shipped, validated, and used to greenlight a deploy. It was structurally broken in one line. No harness is trustworthy by default.

---

## Related docs

- `/Users/jo/Tick/mid_freq_research/TRACK2_microscalper_postmortem.md` — MicroScalper harness bug postmortem (TP-at-tp_px)
- `/Users/jo/Tick/mid_freq_research/CPP_SLOPE_GATE_RESULTS.md` — ThreeBar30m harness LOSS_CUT_PCT artifact + slope gate validation
- `KNOWN_BUGS.md` — phantom-fire / 100x P&L race etc.
