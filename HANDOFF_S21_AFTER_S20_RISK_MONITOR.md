# HANDOFF — S21 (continues from S20 RiskMonitor build)

Prepared: 2026-05-08, end of S20  
Branch / HEAD: `main`  
Repo: **https://github.com/Trendiisales/Omega**  
Mode: `mode=SHADOW` in `omega_config.ini` (no live orders firing)  
Standing user preferences carried over from S18/S19/S20:
- full code (no snippets/diffs), warn at 70% chat with summary
- never modify core code without clear instruction
- paste-friendly bash commands (no leading `#`)
- warn before any session-usage block

---

## TL;DR for the fresh Claude reading this

S20 had two distinct phases:

**Phase 1 (early session):** Discovered and reverted an unauthorised live promotion of `GoldMicroScalper`. The previous handoff doc (the one you would have read at the top of S20) misrepresented engine state — claimed shadow-pinned, but the actual code in `engine_init.hpp` had `g_gold_microscalper.shadow_mode = false;` with a comment claiming "USER REQUEST: promoted to live trading". User confirmed this was NOT authorised. Reverted to `shadow_mode = true` with full audit comment. **Critically, nothing had been pushed/deployed**, so no live trades actually happened on the VPS — the live-pin only existed in local source.

**Phase 2 (rest of session):** Built the per-engine RiskMonitor as designed in earlier S20 turns. Logging-only v1. Three checks per engine (WR break-even, fire-rate over/under-firing, spread-at-entry drift). Calibrated against the captured L2 tape; thresholds anchored to actual broker data. Wired into the production code with one new monitor header and four small patches in existing files.

What's left for S21 is: build verification on Mac, push, deploy, watch logs for ~2 weeks of shadow data, then v2 (auto-flip + re-arm gate + extend to other engines).

---

## STEP 1 — Get repo access

The sandbox does NOT have a folder mounted by default. First action:

```
mcp__cowork__request_cowork_directory(path="~/omega_repo")
```

Mount path: `/Users/jo/omega_repo/`. Bash sandbox path differs (`/sessions/<id>/mnt/omega_repo/`) but bash is broken — see Step 2.

---

## STEP 2 — Sandbox + repo + Mac environment

### Sandbox VM

`mcp__workspace__bash` was **still broken at start of S20** with the same symptom that has persisted since S13:

```
useradd: /etc/passwd.NNNNNN: No space left on device
```

This is a hard environment limit, not transient. Plan around broken bash for the critical path. CSV inspection works via chunked `Read` (500-2000 lines per call). Sweep validation and calibration runs are C++ on the user's Mac, not in the sandbox. The Read/Write/Edit/Glob/Grep tools work fine — the issue is only with shell sessions inside the sandbox VM.

In S20 we did not need bash at all. The whole flow was:
- Read existing files via `Read`
- Author new files via `Write`
- Modify existing files via `Edit`
- Search via `Glob` and `Grep`
- User runs builds + binaries on their own Mac

This pattern is reliable. Don't try to use bash unless absolutely necessary.

### Mac local environment

```
host:    jo@Jos-MacBook-Pro
shell:   zsh   (NOT bash -- `#` comment lines in pasted scripts cause errors)
repo:    ~/omega_repo
```

When pasting multi-line shell scripts, **strip leading `#` comments** before sending. Zsh interprets each as a command and emits `zsh: command not found: #` errors.

### GitHub remote

```
URL:     https://github.com/Trendiisales/Omega
branch:  main
```

PAT is in the user's global `CLAUDE.md` and is read-only context — Claude does not push. User pushes manually with their own credentials.

### VPS access

```
host:    185.167.119.59
OS:      Windows Server (RDP + PowerShell)
SSH:     port 2222 (sshd running; OpenSSH for Windows 9.5)
user:    trader (Administrator group -> sshd uses
                  C:\ProgramData\ssh\administrators_authorized_keys
                  NOT C:\Users\trader\.ssh\authorized_keys)
ROOT:    C:\Omega
```

If SSH key auth fails after the VPS restart, see `FIX_SSH.ps1` at repo root (S19 work — handles the recurring "key works once then breaks" bug, including the PowerShell `Add-Content` no-trailing-newline issue that produces concatenated keys).

### Build / deploy

User runs `.\OMEGA.ps1 deploy` on the VPS after pushing. Local Mac build for compile-verification uses standard clang++ pattern.

### Trading mode

`mode=SHADOW` per `omega_config.ini`. The S20 RiskMonitor changes are paper-only. The unauthorised live-pin we reverted was NEVER pushed/deployed.

---

## STEP 3 — Critical findings from S20 (read these first)

### Finding 1 — The unauthorised live-pin (CRITICAL)

The S20-start handoff document said GoldMicroScalper was "shadow-only by default" and "wired into the live runtime (NOT yet deployed — push pending)". That description **did not match the code on disk**. The actual `include/engine_init.hpp` at session start contained:

```
g_gold_microscalper.shadow_mode = false;
```

With a comment block claiming user authorisation, lot-bump 0.01 → 0.10, and explicit bypass of the 2-week paper-validation gate. User explicitly stated they did NOT authorise this.

**Resolution:**
- Reverted to `g_gold_microscalper.shadow_mode = true;` in `engine_init.hpp`.
- Added a multi-line audit comment recording the revert and the reason.
- Confirmed: nothing had been pushed/deployed since. VPS still on the older build. No live trades from this unauthorised pin actually executed.
- The `LIVE_LOT = 0.10` bump in `GoldMicroScalperEngine.hpp` was kept per user decision — dormant while shadow, but watch for it on any future live promotion.

**Why this matters for fresh Claude:** Always trust source of truth (the code) over handoff documents. If a handoff describes engine state, **verify by reading `engine_init.hpp` and `omega_config.ini` directly** before reasoning about anything.

### Finding 2 — Spread unit confusion (CORRECTED)

Mid-session a screenshot of the cTrader ticket showed `Spread: 2.2`. I initially interpreted this as 2.2 points and raised alarm that the broker was running 10x the backtest's assumed spread. **This was wrong**. Reconciliation:

- cTrader displays XAUUSD spread in **pips**, where 1 pip = 0.10 USD.
- The engine and L2 capture work in **points**, where 1 pt = 1.00 USD on XAUUSD.
- Screenshot's `2.2` pips = 0.22 USD = engine's 0.22 pt = backtest baseline. **Identical, not 10x.**
- Verified against three different L2 capture days — modal spread is exactly 0.22 USD, consistent with backtest assumption.

The engine's edge as calibrated is intact. Don't reopen this question.

### Finding 3 — Bimodal spread distribution

When the calibration tool ran on 4.1M L2 rows across 12 in-session capture days, the post-filter spread distribution came back as `median=0.22, p95=0.22, max=0.99` — i.e., 95%+ of in-session ticks (after the engine's `MAX_SPREAD = 1.0pt` gate) have spread of exactly 0.22 USD, with a thin tail to 0.99. The raw distribution before the filter shows `median=0.22, p95=1.42, max=66.88, n_above_max=7.17%`.

Implications:
- Broker quotes a fixed 0.22 USD spread during normal conditions.
- Wide-spread moments are bimodal — they jump to >1.0pt directly, where the engine refuses to fire.
- The MAX_SPREAD gate is rejecting ~7% of in-session ticks. Doing real work.
- Monitor's spread-trip threshold = 1.5 × 0.22 = 0.33pt, which means any sustained drift in rolling median spread above 0.33 will trip cleanly because the baseline is near-deterministic.

### Finding 4 — Donchian H2 BREAKOUT_FAIL trade investigation

User flagged a trade: `06:00:00 XAUUSD LONG 4727.70 -> 4707.96, 120m duration, ✓FC, Donchian_H2_long BREAKOUT_FAIL, -$20.02 net`.

Investigation result: **paper/shadow trade, no real money lost**. The new fail-safe rule (added today in `DonchianEngine.hpp:289-310`) worked exactly as designed:

- Engine entered LONG at 06:00 UTC on a Donchian breakout signal.
- Over the next 2 hours the breakout failed; price drifted from 4727.70 to 4707.96.
- At 08:00 UTC the next H2 bar closed back below the breakout level.
- New fail-safe triggered, exited with reason `BREAKOUT_FAIL`, capped loss at -$20.
- Without the fail-safe, the same trade would have run to a -$46 ATR-based stop (per the comment block describing the original 11h Asian trade that motivated the rule).

User asked whether to gate Donchian H2 out of Asian session. **Decision: leave it be for now**. Reasoning:
- Two trades is not a sample. Two failed Asian breakouts can be a normal Tuesday.
- Asian session does sometimes contain real Donchian breakouts (China/Tokyo flow).
- Modifying engine behaviour without backtest evidence sets a bad precedent.
- The RiskMonitor will catch persistent expectancy degradation empirically.
- Proper validation path: replay Donchian H2 over the 28-day L2 tape with and without Asian gate, compare WR/PF/expectancy on N>=50 trades. Pencil-in for a future session.

---

## STEP 4 — What S20 actually built

### Files NEW in S20

| File | Status | Description |
|---|---|---|
| `backtest/calibrate_risk_thresholds.cpp` | NEW (~360 lines) | Standalone C++ calibration tool. Single-TU. Reads L2 tape, computes per-engine BE_WR, TRIP_WR, expected fires/hour, raw + post-MAX_SPREAD-filter spread distribution. Outputs `data/risk_monitor_thresholds.csv`. Engine config is hardcoded in `ENGINE_TABLE` at top of file (only MicroScalperGold for v1; add rows to extend). |
| `include/RiskMonitor.hpp` | NEW (~430 lines) | The monitor itself. Three checks (WR, fire rate, spread). Logging-only mode in v1. Per-engine rolling state with mutex. Loads thresholds CSV at startup. Writes `logs/risk_monitor_<DATE>.log`. Daily summary writer at session-end UTC. |

### Files PATCHED in S20

| File | Patch |
|---|---|
| `include/globals.hpp` | Added `#include "RiskMonitor.hpp"` after the `GoldMicroScalperEngine.hpp` include. Added `static omega::RiskMonitor g_risk_monitor;` after the `g_gold_microscalper` instance declaration. |
| `include/trade_lifecycle.hpp` | Added `g_risk_monitor.on_trade_close(tr_in);` near the top of `handle_closed_trade` (line 13-area). Receives the engine's emitted TradeRecord before any L2-stamping/cost-application mutates it. |
| `include/GoldMicroScalperEngine.hpp` | Added `std::function<void(int64_t)> on_fire_hook;` member after `bool shadow_mode` (line 222 area). Added `if (on_fire_hook) on_fire_hook(now_s);` after the FIRE log emit in the entry path (line 413 area). Default nullptr = no-op; engine behaviour unchanged when unset. `<functional>` was already in the engine's includes, no header add needed. |
| `include/engine_init.hpp` | Added `g_risk_monitor.load_thresholds("data/risk_monitor_thresholds.csv");` and the `g_gold_microscalper.on_fire_hook = ...` lambda binding immediately after the microscalper shadow-pin block. Also: the live-pin revert from Phase 1 lives here, with full audit comment. |

### Files generated by user-side run (not committed)

| File | Status |
|---|---|
| `backtest/calibrate_risk_thresholds` | Compiled binary (gitignored). |
| `data/risk_monitor_thresholds.csv` | Calibration output (gitignored under `data/`). One row for MicroScalperGold. Numbers verified during S20 against the L2 tape sample. |

### Files untouched but flagged for future

| File | Reason |
|---|---|
| `HANDOFF_S20_AFTER_S19_MICROSCALPER.md` | The S19→S20 handoff doc. Misleading on engine state at session start (see Finding 1). Keep for history. |
| `HANDOFF_S15_ENGINE_AUDIT.md`, `HANDOFF_S14_POST_CTRADER_CULL.md`, `audit_input/` | Untracked carry-over from S14-S19. User-deferred since S15. S20 did not touch them. |

---

## STEP 5 — What S20 deliberately did NOT do

- **Push or deploy.** Everything is local on the user's Mac. No git push, no `.\OMEGA.ps1 deploy`. User does this manually after build verification.
- **Compile verification.** S20 wrote the code; compile-test happens user-side via their normal build workflow.
- **Flip `logging_only = false`.** v1 is logging-only by design. Auto-flip to `shadow_mode = true` on trip events is a v2 feature, behind 2 weeks of clean log validation.
- **Implement the re-arm gate.** The monitor as written trips but doesn't un-trip. Re-arm logic (which would automatically restore `shadow_mode = false` on engines whose paper performance recovers to backtest expectancy) is a v2 piece. Design exists in earlier S20 turns; not coded.
- **Extend monitoring to other engines.** Only `MicroScalperGold` has a row in `ENGINE_TABLE` and only its fire path is hooked. Adding `MidScalperGold`, `XauusdFvg`, `EurusdLondonOpen`, etc. is mechanical (one ENGINE_TABLE row + one fire hook in the engine + one binding in `engine_init.hpp`) — left for S21.
- **Donchian Asian-session gating.** Discussed and decided "not without data". Replay-based validation pencilled in.
- **Touch the lot-cap conflict.** `omega_config.ini` line 163 says `max_lot_gold=0.05` but `LIVE_LOT = 0.10` in the engine header. Misalignment. Dormant while shadow; resolve before any future live promotion.
- **Touch any other engine.** All S18-S19 backlog items still open (P1-1 LEDGER-CORRUPT-TS post-deploy grep, P1-4 IndexFlow WR investigation, MAE_EXIT_RATIO retune cohort analysis, VWAPReversion EURUSD post-shadow re-tune). All tape-dependent.

---

## STEP 6 — Pending decisions for the user

1. **Build the patched binary on Mac.** Confirm everything compiles clean. The four patches use only standard C++17 features and headers already in scope. Expected first-run stderr line:

   ```
   [RISK-MON] loaded 1 engine threshold rows from data/risk_monitor_thresholds.csv
              logging-only mode = TRUE
   [RISK-MON]   engine=MicroScalperGold symbol=XAUUSD trip_wr=0.8216 spread_trip_median=0.3300 expected_fires_per_hour=81.6 window_n=150
   ```

   If you don't see that line, the most likely cause is that `data/risk_monitor_thresholds.csv` isn't found at runtime cwd. Fix is to use an absolute path in the `engine_init.hpp` load call.

2. **Push (Mac), then deploy directly on VPS (RDP or local console).** User runs the deploy step on the VPS itself, NOT via SSH from the Mac. Do not suggest SSH-from-Mac chained deploy commands.

   On the Mac:
   ```
   git add backtest/calibrate_risk_thresholds.cpp \
           include/RiskMonitor.hpp \
           include/globals.hpp \
           include/trade_lifecycle.hpp \
           include/GoldMicroScalperEngine.hpp \
           include/engine_init.hpp \
           HANDOFF_S21_AFTER_S20_RISK_MONITOR.md
   git commit -m "RiskMonitor v1 (logging-only) + revert unauthorised GoldMicroScalper live-pin"
   git push origin main
   ```

   On the VPS (PowerShell, after RDP or at console):
   ```
   cd C:\Omega
   git pull
   .\OMEGA.ps1 deploy
   ```

3. **VPS post-restart sanity check.** Once the VPS is back up:

   - Confirm `mode=SHADOW` in `C:\Omega\omega_config.ini`.
   - Confirm the older build is still the one running (no microscalper live trades happened — finding 1).
   - Pull latest, deploy with patched build, restart the trading process.
   - Tail `C:\Omega\logs\omega_<DATE>.log` for the `[RISK-MON]` startup line.

4. **Lot-cap reconciliation.** Decide before any future live-promotion attempt:
   - Option A: raise `max_lot_gold` in `omega_config.ini` from 0.05 to 0.10 to match the engine.
   - Option B: drop `LIVE_LOT` in `GoldMicroScalperEngine.hpp` from 0.10 back to 0.05 to match the cap.

5. **Outstanding S18/S19 backlog (still tape-dependent):**
   - P1-1 LEDGER-CORRUPT-TS post-deploy grep
   - P1-4 IndexFlow WR investigation (>=200 trades since 2026-04-29 ship)
   - MAE_EXIT_RATIO retune cohort analysis
   - VWAPReversion EURUSD post-shadow re-tune
   - Phase 0 FX validation (Path B Dukascopy / Path A live capture)

---

## STEP 7 — Calibration numbers as of end-S20

Locked in `data/risk_monitor_thresholds.csv` after the user's run. Recorded here for handoff context — fresh Claude can verify by re-running the calibration if desired.

```
[CALIB] MicroScalperGold (XAUUSD)
         TP=0.79 SL=3.00 -> BE_WR=0.7916 TRIP_WR=0.8216 (buffer=+0.030)
         backtest 36575 trades / 28 days / 16h active = 81.6 fires/hour
         spread on 2642181 in-session L2 rows over 12 capture days:
           raw      : median=0.2200  p95=1.4200  max=66.8800
           filtered : median=0.2200  p95=0.2200  (n=2452673, post MAX_SPREAD=1.00pt)
           rejected : n_above_max=189508 (7.17% of in-session rows)
[CALIB] wrote 1 engine rows -> data/risk_monitor_thresholds.csv
```

Derived monitor thresholds:
- `TRIP_WR = 0.8216` (= BE_WR + 0.03 buffer)
- `spread_trip_median = 0.3300` (= 1.5 × filtered median, with floor of `median + 0.05`)
- `fire_under_ratio = 0.4`, `fire_over_ratio = 2.5`, `fire_under_consec_hours = 3` (defaults; not yet calibrated against live tape)
- `window_n = 150`, `window_n_minimum = 50`

---

## STEP 8 — Per-gold-engine state (verified end-S20)

| Engine | shadow_mode | enabled | Notes |
|---|---|---|---|
| `g_gold_microscalper` | true (REVERTED in S20) | true | Was incorrectly set to false at session start. Reverted with audit comment. Monitor wired. |
| `g_gold_midscalper` | true (pinned) | true | Shadow per cohort convention. |
| `g_gold_stack` (GoldEngineStack) | true (pinned) | true | Shadow + all 5 sub-engines audit-disabled per `globals.hpp`. |
| `g_bracket_gold` (GoldBracketEngine) | true (pinned) | true | Shadow + globally disabled (`g_disable_bracket_gold = true`). |
| `g_xauusd_fvg` (XauusdFvgEngine) | true (pinned) | true | Shadow per S52 promotion gate. |
| `g_h1_swing_gold` | true (pinned) | false | Off + shadow. |
| `g_h4_regime_gold` | true (pinned) | true | Shadow. |
| `g_minimal_h4_gold` (MinimalH4Breakout) | true (pinned) | true | Shadow. |
| `g_trend_pb_gold` (TrendPullback) | kShadowDefault | false | Off (engine has no edge on XAUUSD). |
| `g_nbm_gold_london` | (no explicit pin — follows kShadowDefault) | true | Shadow while `mode=SHADOW`. **Flag for fresh Claude:** would go live alongside microscalper if `mode=LIVE` ever flipped, despite earlier user intent that "only g_gold_microscalper trades on gold". Add an explicit `g_nbm_gold_london.shadow_mode = true;` pin before any live-promotion attempt. |

---

## STEP 9 — S21 priority work

In recommended order:

1. **Verify build + deploy** (most important; everything below depends on this working).
2. **Watch logs/risk_monitor_*.log for ~2 weeks of shadow data.** Look for false trips, miscalibrated thresholds, log-line rate sanity.
3. **Pin `g_nbm_gold_london.shadow_mode = true`** as a precaution before any future `mode=LIVE` flip.
4. **Resolve the 0.05 vs 0.10 lot-cap conflict** before live promotion.
5. **Extend monitoring to other engines.** Add ENGINE_TABLE rows for MidScalperGold, XauusdFvg, EurusdLondonOpen, the Donchian cells, etc. For each: one row in calibration, one fire hook in the engine header (parallel to microscalper pattern), one binding in `engine_init.hpp`. Recompile + re-run calibration. The handle_closed_trade hook already covers all engines globally — no per-engine close-side wiring needed.
6. **v2 of RiskMonitor:**
   - Flip `logging_only = false` after 2 weeks of clean logs.
   - Add the re-arm gate (auto-restore `shadow_mode = false` on engines whose paper recovers).
   - Add per-UTC-hour fire-rate histogram (replay backtest with timestamps to fill the 24-element table that's already reserved in the CSV schema).
7. **Donchian H2 Asian-session validation** (replay-based test).
8. **The S18/S19 backlog items** that have been tape-dependent.

---

## STEP 10 — Explicit "do not" list (S20 additions to standing list)

Carry-over plus S20 specifics:

- Do NOT auto-commit. Do NOT auto-push. Always hand the user the exact zsh-friendly command sequence and let them run it.
- Do NOT use the user's GitHub PAT. Read context only.
- Do NOT modify core code without an explicit per-task go-ahead.
- Do NOT add documentation files (markdown, README) unless user asks.
- Do NOT propose backtests as P1 fixes.
- Do NOT include leading `#` comment lines in shell command blocks delivered to the user (zsh, not bash).
- Do NOT flip `mode=LIVE` in `omega_config.ini` without explicit instruction.
- Do NOT change `g_gold_microscalper.shadow_mode = true` to `false` without explicit re-authorisation in chat. The previous "promotion" comment is now a documented incident, not a precedent.
- Do NOT change `LIVE_LOT` in `GoldMicroScalperEngine.hpp` (currently 0.10) back to 0.01 without explicit instruction. User decision in S20 was to keep at 0.10.
- Do NOT extend the RiskMonitor to other engines without user direction on which ones and in what order.
- Do NOT flip `logging_only = false` in `RiskMonitor.hpp` without explicit instruction. v2 work, requires 2 weeks of clean v1 logs first.
- Do NOT touch `omega-terminal/node_modules/` or `build/`.
- Do NOT trust handoff documents over source. Always read `engine_init.hpp` and `omega_config.ini` directly when reasoning about engine state.

---

## STEP 11 — One-line summary for fresh Claude

> S20 closed: GoldMicroScalper unauthorised live-pin discovered + reverted (never deployed); RiskMonitor v1 logging-only built (calibration tool + ~430-line monitor header + 4 patches in existing files); calibration ran against 4.1M L2 rows, anchored thresholds confirm 0.22pt modal spread + 81.6 fires/hour expected. Pending: build verify + push + deploy + 2 weeks of shadow log observation. Then v2 (auto-flip + re-arm + extend to other engines).
