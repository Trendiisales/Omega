# HANDOFF — S22 (DeepStrike live deploy + GUI panic button continuation)

Prepared: 2026-05-08, end of S21
Branch / HEAD: `main` (push pending)
Repo: **https://github.com/Trendiisales/Omega**
Mode: `mode=SHADOW` in `omega_config.ini` — but **GoldMicroScalper has explicit `shadow_mode = false` per-engine pin and trades LIVE** via the load-bearing-interlock pattern.
Account: **8077780** (already configured in `omega_config.ini` `[fix]` section).
Lot: **0.03** (`LIVE_LOT` in engine + `max_lot_gold` cap aligned).

---

## TL;DR for the fresh Claude reading this

S21 took the user from "build the RiskMonitor" through to "GoldMicroScalper authorised live on account 8077780 at 0.03 lot under the single-engine DeepStrike policy". Live promotion is committed in source but not yet pushed at handoff time. Three panic mechanisms are in place. RiskMonitor is in auto-pin mode (`logging_only = false`) — it will actually flip the engine to shadow on any of its three trip thresholds.

User explicitly chose **Option C** (constrain current Omega to single-engine + live) over Option B (separate DeepStrike fork). Reasoning preserved in this doc and in code comments. The "DeepStrike" name appears in code comments and the handoff but is not a separate codebase.

What's left for S22 is: push + deploy, smoke-test the panic button, watch live trades. **Plus** one optional follow-up — adding a real PANIC button to the omega-terminal React dashboard. The backend endpoint is already in place; only the frontend work is missing.

---

## STEP 1 — Critical safety state at handoff

**Per-engine state (in source, not yet deployed):**

| Engine | shadow_mode | enabled | Trades live? |
|---|---|---|---|
| `g_gold_microscalper` | **false** (live-pinned) | true | **YES — single live engine** |
| Every other engine using `kShadowDefault` | true (forced by hard-pin at `engine_init.hpp` line 17) | varies | No |
| `g_gold_midscalper`, `g_gold_stack`, `g_bracket_gold`, `g_xauusd_fvg`, `g_h1/h4_swing/regime_gold`, `g_minimal_h4_gold`, `g_trend_pb_gold`, `g_nbm_gold_london` | true (explicit pins) | varies | No |
| FX cohort (eurusd / usdjpy / gbpusd / audusd / nzdusd open engines) | true (explicit pins) | varies | No |

**Critical guarantee:** the `kShadowDefault = true` hard-pin at `engine_init.hpp` line 17 means even if `omega_config.ini` mode is later flipped to LIVE, **no engine goes live except those with an explicit `shadow_mode = false` pin**. Today only microscalper has that pin. This is the defensive whitelist the user asked for.

**Calibration anchored:**
- Backtest WR: 92.5%, post-cost BE_WR at 0.03 lot: ~82.1%, monitor TRIP_WR: 82.16%. ~10pp of headroom.
- MAX_SPREAD tightened from 1.0pt to 0.5pt for max protection.
- RiskMonitor `logging_only = false` — auto-pins engine to shadow on trip.

---

## STEP 2 — Files changed in S21 (not yet pushed)

| File | Change |
|---|---|
| `omega_config.ini` | `max_lot_gold` 0.05 → 0.03 |
| `include/engine_init.hpp` | `kShadowDefault = true` hard-pin; microscalper live promotion comment + `= false` pin; `g_nbm_gold_london` explicit shadow pin; `register_shadow_pin_cb` for microscalper auto-pin handler |
| `include/GoldMicroScalperEngine.hpp` | `LIVE_LOT` 0.10 → 0.03; `MAX_SPREAD` 1.0pt → 0.5pt; **kill-switch file check** in `on_tick` (polls `KILL_MICROSCALPER` every 100 ticks) |
| `include/RiskMonitor.hpp` | `logging_only = false`; new `register_shadow_pin_cb()` public method + `_maybe_auto_pin()` helper; auto-pin wiring in all three evaluators (WR / fire-rate / spread); WOULD-TRIP labels become TRIPPED when `logging_only = false` |
| `src/api/OmegaApiServer.cpp` | New `GET /api/v1/omega/microscalper/panic` endpoint (creates `KILL_MICROSCALPER` sentinel file) and `GET /api/v1/omega/microscalper/status` endpoint (returns whether the file exists) |

Plus the earlier S20 work (still not pushed): `backtest/calibrate_risk_thresholds.cpp`, `include/RiskMonitor.hpp` (initial creation), `include/globals.hpp`, `include/trade_lifecycle.hpp`, MSVC build fix.

Plus this handoff doc and `HANDOFF_S21_AFTER_S20_RISK_MONITOR.md`.

---

## STEP 3 — Push and deploy (USER ACTION)

On the Mac:

```
cd ~/omega_repo
git add omega_config.ini \
        include/engine_init.hpp \
        include/GoldMicroScalperEngine.hpp \
        include/RiskMonitor.hpp \
        src/api/OmegaApiServer.cpp \
        HANDOFF_S22_DEEPSTRIKE_LIVE.md
git commit -m "DeepStrike Option C live: single-engine + auto-pin monitor + panic"
git push origin main
```

On the VPS (PowerShell, RDP or local console — NOT via SSH from Mac):

```
cd C:\Omega
git pull
.\OMEGA.ps1 deploy
```

---

## STEP 4 — Smoke test the panic button BEFORE the engine has any open positions

This is non-optional. Test the kill-switch path before staking real money on it.

1. After deploy, watch the log for the startup line:
   ```
   [RISK-MON] loaded 1 engine threshold rows from data/risk_monitor_thresholds.csv
              logging-only mode = FALSE
   [RISK-MON] auto-pin callback registered for engine=MicroScalperGold
   ```
   `logging-only mode = FALSE` is the auto-pin armed signal. The fourth line confirms the callback is wired.

2. Test path A (kill-switch file):
   ```powershell
   New-Item C:\Omega\KILL_MICROSCALPER -ItemType File
   ```
   Within 30 seconds, log should show:
   ```
   [MICRO-SCALPER-GOLD] KILL-SWITCH file detected, forcing SHADOW (no further fires until restart)
   ```

3. Test path B (API endpoint, from any browser):
   - Delete the file: `Remove-Item C:\Omega\KILL_MICROSCALPER`
   - Redeploy to clear shadow pin: `.\OMEGA.ps1 deploy`
   - Verify engine fires live again (FIRE log line ends `[LIVE]`).
   - Hit the panic URL in browser:
     `http://<vps>:<port>/api/v1/omega/microscalper/panic`
     where `<port>` is whatever the API server binds (likely 7779 or 7781 — test `/status` first).
   - Within 30 seconds engine logs the same KILL-SWITCH line.

4. After both tests pass, clear the file and redeploy ONE MORE TIME so the engine is in known-live state for actual trading.

If either path fails: do not proceed to live trading. Diagnose first.

---

## STEP 5 — Three panic mechanisms (operator reference)

Bookmark this list. In an emergency:

1. **Browser URL (fastest, no console):** hit `http://<vps>:<port>/api/v1/omega/microscalper/panic`. Engine goes shadow within ~30 seconds. Bookmark this in the same Chrome window where the dashboard lives.

2. **Manual VPS file:** `New-Item C:\Omega\KILL_MICROSCALPER -ItemType File`. Same effect, no app dependency.

3. **Stop the service:** `Stop-Service "Omega Trading Engine"` (or whatever the exact service name is — check on the VPS first). Open positions stay on broker with their SL/TP active at the exchange; the engine's reversal-exit / BE-trail logic stops running.

Plus the **automatic** auto-pin via RiskMonitor: triggers on any of WR < 82.16% / fire-rate over (>2.5x) / fire-rate under (<0.4x for 3 consecutive hours) / spread median > 0.33pt over rolling 50+ trades. Operator does nothing; engine auto-shadows.

---

## STEP 6 — Resume after panic (deliberate, not reflexive)

By design there is **no resume endpoint or button**. Resuming requires:

1. Delete the kill-switch file: `Remove-Item C:\Omega\KILL_MICROSCALPER`
2. If the auto-pin monitor tripped: change `g_gold_microscalper.shadow_mode = false` is already in source from this S21 commit; nothing to flip.
3. Redeploy: `.\OMEGA.ps1 deploy`. Engine restarts with the source pin (live).

Why this asymmetry: panic should be one-click; resume should be deliberate enough that you've thought about why you panicked first.

---

## STEP 7 — Risk profile (read before going live)

- **Real-cost BE_WR ≈ 82.1%** (TP=0.79, SL=3.00, slip ≈ $0.33/round-trip at 0.03 lot). Backtest WR was 92.5% — you have ~10pp of headroom before bleeding.
- **Monitor TRIP_WR = 82.16%** — essentially at BE. Auto-pin fires immediately when the engine becomes unprofitable post-cost. This is intentionally tight.
- **One engine, one symbol, one direction** — no diversification. A bad XAUUSD regime hits all P&L at once.
- **No re-arm gate yet.** Once auto-pinned, engine stays shadow until source flip + redeploy. Operator decision required.
- **Eight wins in shadow at 0.10 lot is not a sample.** Backtest expectancy at the new 0.03 lot has been validated only mathematically (lot scaling), not via live paper.
- **Watch first hour live.** Be ready to use one of the three panic mechanisms.
- **Password in plaintext** at `omega_config.ini:49`. If the GitHub repo is public, rotate after going live. Not blocking.

---

## STEP 8 — Pending work for S22+ (after live trading is stable)

1. **Real GUI panic button on omega-terminal dashboard.** The backend endpoint exists; only the React frontend work is missing. The omega-terminal source is gitignored / not in this repo. To wire the button in a fresh session, paste these four files into the chat at session start:
   - `package.json` (small, shows framework + dependencies)
   - The root component (`src/App.tsx` or equivalent — the file that mounts the dashboard layout)
   - The header / status-bar component (search omega-terminal repo for `"SHADOW"` string literal — whichever file renders the top bar with `SHADOW | UP | LONDON | Connected | Q:OK | L2 ✓ | <build_hash>`)
   - The API client / hook file (the file with existing GET calls to `/api/v1/omega/engines`)
   - Optional: tailwind config / theme file for color matching, plus any existing button primitive
   
   Plus the build/deploy command for omega-terminal so a fresh session can verify the build.
   
   Recommended button shape: red pill in the header next to the SHADOW indicator. Click triggers a confirm modal ("PANIC: this will halt live trading immediately. Confirm?"); confirm fires `GET /api/v1/omega/microscalper/panic`; UI shows the kill-switch status (polling `/status` every 5 seconds) so the operator sees confirmation that the trip landed.

2. **Re-arm gate for RiskMonitor v2.** Currently auto-pin is one-way. v2 should add automatic re-arm when shadow paper performance recovers to backtest expectancy on N >= 200 fills. Until then, manual source flip + redeploy is the only resume path. Not a blocker for first live; matters in the second-week-onwards operating tempo.

3. **Extend RiskMonitor to other engines.** Only MicroScalperGold has a row in `ENGINE_TABLE` and only its fire path is hooked. Adding any other engine to monitoring is mechanical: one row in `backtest/calibrate_risk_thresholds.cpp`, one fire hook member in the engine header (parallel to microscalper's `on_fire_hook`), one binding in `engine_init.hpp`.

4. **Lot-cap audit alignment.** `omega_config.ini` `max_lot_gold = 0.03` matches `LIVE_LOT = 0.03`. If live trading authorises a lot bump in future, both must move together.

5. **Donchian H2 Asian-session validation** (replay-based test, decided "leave it be" in S20 turn 14).

6. **The S18/S19 backlog items** that have been tape-dependent (P1-1 LEDGER-CORRUPT-TS post-deploy grep, P1-4 IndexFlow WR investigation, MAE_EXIT_RATIO retune, VWAPReversion EURUSD post-shadow re-tune, Phase 0 FX validation).

---

## STEP 9 — Standing operational rules (carry-over)

From the user's standing prefs and S20/S21 context:

- Repo URL: **https://github.com/Trendiisales/Omega**, branch `main`. User pushes from Mac.
- VPS deploy: user runs `.\OMEGA.ps1 deploy` directly on VPS (RDP or console). NOT via SSH from Mac.
- Sandbox bash is broken (`useradd: No space left on device`). Use Read/Write/Edit/Glob/Grep only.
- Never modify core code without explicit instruction.
- Always full files / no diffs when sharing code with user (doesn't apply to in-place Edits).
- Warn at 70% chat with summary. Be conscious of context burn on long sessions.
- Do NOT push or commit on user's behalf. Hand them the exact zsh-friendly command sequence.
- Do NOT trust handoff documents over source. Always verify engine state by reading `engine_init.hpp` and `omega_config.ini` directly.

---

## STEP 10 — One-line summary

> S21 closed: GoldMicroScalper authorised live on account 8077780 at 0.03 lot under DeepStrike single-engine policy; defensive whitelist hard-pins all other engines to shadow regardless of mode; RiskMonitor in auto-pin mode (logging_only=false) with WR/fire/spread auto-shadow on trip; three panic mechanisms wired (browser URL via new API endpoint, manual VPS kill-switch file, service stop). Pending S22: push + deploy + smoke-test panic before going live. Optional: GUI panic button on omega-terminal (backend ready; frontend source gitignored, paste files at fresh-session start to land it).
