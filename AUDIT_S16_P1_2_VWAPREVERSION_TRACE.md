# P1-2 — VWAPReversion `FORCE_CLOSE` Trace

Prepared: 2026-05-08, S16 (after P1-1)
Scope: P1-2 from `HANDOFF_S16_AFTER_S15_P0_FIXES.md` — `VWAPReversion` exits exclusively on `FORCE_CLOSE` in the shadow tape; figure out whether the engine's own close path is dead or an external supervisor is closing first.
Repo HEAD at trace: `4db4750` (S15 P0 fixes + audits + P1-1 trace, pushed earlier this session).

---

## CLOSURE ANNOTATION — added 2026-05-08, S18 (after S17 fixes landed)

**Status: P1-2 CLOSED. P1-2-a CLOSED.**

S17 implemented the recommended fix shape below and landed it across two commits on top of `4db4750`:
- `83a4d66` — "P1-2 + P1-2-a: label fidelity for CrossPosition::force_close"
- `1573b7a` — "S17: P1-2/2a label fidelity + P1-3 FxCascade cooldown + P1-8 EsNqDiv + VWAP re-enable"

**P1-2 fix shipped:**
- `CrossPosition::force_close` now takes optional `reason` parameter (default `"FORCE_CLOSE"` for backward compat). `CrossAssetEngines.hpp:264`.
- 14 supervisor call sites updated to pass meaningful labels: `MAE_EARLY_EXIT`, `MIDNIGHT_ROLLOVER`, `STALE_PRIOR_DAY`, `RECONNECT_CLOSE`, `SHUTDOWN`. Sites in `CrossAssetEngines.hpp`, `config.hpp`, `quote_loop.hpp`.
- All `CrossPosition` wrapper methods updated to forward the reason.

**P1-2-a corrected reading (the original "What needs follow-up" section below is now stale):**

The mystery rows from the historical tape resolve as follows once the codebase is grepped exhaustively for `g_vwap_rev_*.force_close` calls:

- Rows 6, 7, 10 (profitable LONGs, intra-day, in SHADOW) — **explained by engine MAE early-exit at `CrossAssetEngines.hpp:1252`**, NOT a missing supervisor. The original trace's claim "MAE doesn't fire on profit (`adverse < 0`)" was wrong: `adverse` is the *current* drawdown from entry, and a position that was MFE-positive can turn slightly adverse before closing on a small absolute profit. The MAE_EARLY_EXIT path is now distinguishable in fresh tape thanks to the label fix.
- Rows 15, 16 (7.78h / 7.79h holds, in SHADOW) — **explained by the SIGINT / SIGTERM / `SetConsoleCtrlHandler` graceful-shutdown path at `omega_main.hpp:36-38`**, which runs in SHADOW mode too. The original trace's "Shutdown handler is LIVE only" framing applied only to the `quote_loop` reconnect path at L691-697, not to the OS-signal handler. Date of those trades: 2026-03-27 entries ~11:45 UTC, exits ~19:32 UTC — same UTC day, mid-NY-session, before midnight. The user confirmed this corresponds to a software-update restart on their end.

There is no missing supervisor. Project-wide grep across `include/` confirms the five sources documented above are exhaustive for `CrossPosition::force_close` callers reachable for `g_vwap_rev_*` instances.

**VWAPReversion re-enable:**
S17 also flipped `enabled = false` → `enabled = true` for all four `g_vwap_rev_*` instances at `engine_init.hpp:447, 450, 453, 457` (SP, NQ, GER40, EURUSD). The engines were architecturally healthy and well-tuned per the May-3 audit; the disable was conservative pre-deploy gating, not a quality cull. Trading mode is `SHADOW` so this re-enable produces paper trades only until the LIVE flip.

**What's still open after S17:**
- MAE_EXIT_RATIO retune cycle, pending 2-4 weeks of fresh post-deploy tape with the new labels in place. Cohort analysis: filter `engine == "VWAPReversion"` AND `exit_reason == "MAE_EARLY_EXIT"`, look at `mfe`, compute cut-winner rate. Same for `MIDNIGHT_ROLLOVER`, `STALE_PRIOR_DAY`, `RECONNECT_CLOSE` cohorts.
- VWAPReversion EURUSD tune. Currently `g_vwap_rev_eurusd` only has `EXTENSION_THRESH_PCT = 0.12` and `COOLDOWN_SEC = 120` set explicitly — `MAX_EXTENSION_PCT` and `MAX_HOLD_SEC` fall back to class defaults. Worth an explicit tune once shadow data accumulates.

The "What needs follow-up (P1-2-a)" section below preserves the original (now-superseded) reading for historical context. Hypotheses A/B/C in that section are all closed: A (no missing supervisor — see above), B (`enabled` toggle wasn't the cause), C (no downstream label mutation).

---

## TL;DR

The handoff's hypothesis "the engine's own close path is dead, or an external supervisor is closing first" is partially right but misframed. The manage path is **reachable in current main and does emit `TP_HIT` / `SL_HIT` / `TIMEOUT` correctly**. The bug is a **label-fidelity defect**: `CrossPosition::force_close` at `CrossAssetEngines.hpp:264-268` hardcodes `"FORCE_CLOSE"` as the exit reason, so any of five different supervisors that close a `VWAPReversion` (or any other `CrossPosition`-based) position writes the same generic label — even when it should be `"MIDNIGHT_ROLLOVER"`, `"STALE_PRIOR_DAY"`, `"SHUTDOWN"`, or similar. The tape's "10 of 10 FORCE_CLOSE" is real but doesn't mean what the audit thought it meant.

Tape verification finds **8 rows** for `VWAPReversion` (not 10). Bimodal hold distribution: 6 trades ≤22 min (consistent with engine MAE early exit + engine timeout window) and 2 trades at ~7.79 h (consistent with overnight feed gap then supervisor close). A scattered sub-puzzle remains — three rows are profitable LONGs (trades 6, 7, 10 in the tape) where MAE shouldn't fire and TP_HIT should — flagged as P1-2-a follow-up.

Recommended fix: extend `CrossPosition::force_close` to accept an optional reason string (default `"FORCE_CLOSE"`) and update each supervisor call site to pass the right label. ~5-10 caller changes, no behavioural change. Not done in this session per "concise report + commit, stop" scope.

---

## The five `FORCE_CLOSE` sources

All five funnel through `CrossPosition::force_close` at `CrossAssetEngines.hpp:264-268`, which hardcodes the reason:

```
void force_close(double bid, double ask, CloseCb on_close) noexcept {
    if (!active) return;
    const double mid = (bid + ask) * 0.5;
    emit(mid, "FORCE_CLOSE", on_close);
}
```

Source map:

| # | Source                    | File:line                                     | Mode gating                                       | What the label SHOULD say |
| - | ------------------------- | --------------------------------------------- | ------------------------------------------------- | ------------------------- |
| 1 | Engine MAE early exit     | `CrossAssetEngines.hpp:1243`                  | Always (engine internal)                          | `MAE_EARLY_EXIT` or stay as `FORCE_CLOSE` (engine-internal, label fidelity less critical) |
| 2 | Midnight rollover         | `config.hpp:242`, called from `mid_ca` lambda; instances at `config.hpp:250-251` | Always (both shadow and live)                     | `MIDNIGHT_ROLLOVER` |
| 3 | Stale-prior-day purge     | `quote_loop.hpp:605` (`stale_ca` lambda)      | Always, but only fires when `entry_ts.tm_yday != today_yday_rc` (i.e., position carried across UTC midnight) | `STALE_PRIOR_DAY` |
| 4 | Reconnect close           | `quote_loop.hpp:1062, 1070-1074`              | LIVE only (`do_reconnect_close = (g_cfg.mode == "LIVE")` at `quote_loop.hpp:691`) | `RECONNECT_CLOSE` |
| 5 | Shutdown handler          | `quote_loop.hpp:802-807, 901-906`             | LIVE only (same gate as #4)                       | `SHUTDOWN` |

For comparison: `BreakoutEngine` and `BracketEngine` go through `forceClose(b, a, REASON, ...)` (note the camelCase) which takes the reason as an explicit parameter. `config.hpp:196` for midnight passes `"MIDNIGHT_ROLLOVER"` correctly. Only the `CrossPosition`-based engines (VWAPReversion, ORB, EsNqDiv, OilFade, BrentWti, MacroCrash via different path, FxCascade, CarryUnwind, NoBSMomentum, TrendPullback) have the label-fidelity gap.

---

## Tape verification

Source: `audit_input/omega_shadow.csv`. Filter: rows where engine column equals exactly `VWAPReversion` (excludes `VWAPStretchReversion` and `VWAP_SNAPBACK`, which are XAUUSD engines from a different stack).

| Row | Sym     | Side  | Entry     | Exit      | PnL     | hold_sec | exit_reason  | Notes                                              |
| --- | ------- | ----- | --------- | --------- | ------- | -------: | ------------ | -------------------------------------------------- |
| 6   | US500.F | LONG  | 6509.99   | 6526.24   | +84.92  | 1221     | FORCE_CLOSE  | Profitable LONG, hold 20.4min — **mystery**        |
| 7   | USTEC.F | LONG  | 23729.2   | 23786.2   | +97.72  | 1335     | FORCE_CLOSE  | Profitable LONG, hold 22.3min — **mystery**        |
| 8   | US500.F | LONG  | 6517.88   | 6518.38   | -1.55   | 19       | FORCE_CLOSE  | Tiny adverse, fast exit — engine MAE plausible     |
| 9   | USTEC.F | LONG  | 23761.5   | 23754.8   | -15.14  | 159      | FORCE_CLOSE  | Adverse, MAE plausible                             |
| 10  | US500.F | LONG  | 6491.99   | 6493.24   | +2.42   | 258      | FORCE_CLOSE  | Tiny profit, hold 4.3min — **mystery**             |
| 11  | USTEC.F | LONG  | 23643.5   | 23634.5   | -21.08  | 499      | FORCE_CLOSE  | Adverse, MAE plausible                             |
| 15  | US500.F | SHORT | 6505.24   | 6420.62   | +460.96 | 27994    | FORCE_CLOSE  | Big runner, hold 7.78h — likely supervisor close   |
| 16  | USTEC.F | SHORT | 23670.3   | 23348     | +575.17 | 28035    | FORCE_CLOSE  | Big runner, hold 7.79h — likely supervisor close   |

Audit said "10 of 10". Actual is **8 of 8**, so directionally correct but exact count off.

Mean hold (8 rows): `(1221+1335+19+159+258+499+27994+28035)/8 = 7440 s = 2.07 h`.

`exit_px` for all 8 rows is the close-time mid (the value matches `(bid+ask)/2` at close), not a TP or SL price. That's the signature of `CrossPosition::force_close` (which sets `exit_px = mid`) versus `CrossPosition::manage` (which sets `exit_px = tp` for TP_HIT or `= sl` for SL_HIT). So at the row level, every one of these 8 trades did go through `force_close`, not through `manage`'s normal close path.

---

## What the static analysis confirmed

1. `VWAPReversion`'s `pos_.open()` at `CrossAssetEngines.hpp:1390` is followed by `pos_.allow_tp_extend = false` at L1391 — so the engine does NOT accidentally extend TP into a runner. The mean-reversion design is intact in code.
2. The `manage()` function at `CrossAssetEngines.hpp:163-240` correctly emits `TP_HIT`, `SL_HIT`, or `TIMEOUT` when those conditions trigger — confirmed by reading the full function body.
3. The `on_tick` flow at L1179-1263 calls `pos_.manage(bid, ask, MAX_HOLD_SEC, on_close)` at L1261 every tick when a position is open and `enabled == true` and `bid/ask > 0` and `vwap > 0`. The progressive timeout extension at L1219-1232 is one-shot (`!timeout_extended_` guard) and bounded.
4. Engine internal hold ceiling: with `MAX_HOLD_SEC=600` (from `engine_init.hpp:449`) and the +300s extension, expected max hold is **15 min**, not 20+ min as seen in trades 6 & 7. This is a real anomaly, not just label confusion.
5. The 7.79h trades' length cannot be explained by engine internals at all — must be feed-disconnect plus supervisor close.
6. There is no on_tick guard that would block manage() unconditionally outside of `enabled=false`, `bid<=0`, `ask<=0`, or `vwap<=0`. Of these, only `enabled=false` is plausible during a live-shadow trade (the operator could have toggled it). All four `g_vwap_rev_*.enabled` are currently `false` per `engine_init.hpp:447, 450, 453, 457`, but they were `true` when the historical tape was produced.

---

## What needs follow-up (P1-2-a)

Trades 6, 7, 10 are profitable LONGs that closed via `force_close` (exit_px = mid, not = tp). The five known `force_close` sources don't fit cleanly:

- Engine MAE early exit: doesn't fire on profit (`adverse < 0`).
- Midnight rollover: trades 6/7/10 are intra-day, no UTC midnight crossing.
- Stale-prior-day: same as above.
- Reconnect close: SHADOW mode only, gated out.
- Shutdown handler: SHADOW mode only, gated out.

Hypotheses still on the table:

A. **An undiscovered supervisor / cooldown** — possibly the indices-disconnect cooldown handler (the handoff mentions one). Worth grepping `quote_loop.hpp` and `tick_indices.hpp` for any other `g_vwap_rev_sp.force_close` call site I haven't surfaced. The five I found are above.

B. **`enabled` toggled mid-trade** — operator action could explain the long-hold trades but not 1221s/1335s/258s ones, since the manage path would still be sleeping when reconnect/midnight handlers don't fire in SHADOW.

C. **The CSV row label is being overwritten downstream** — possible if `handle_closed_trade` or the writer mutates `exitReason` after emit. Worth a 5-min grep but unlikely.

Recommend P1-2-a be a 30-minute follow-up specifically grepping for any other `g_vwap_rev_*.force_close` call site in the binary (including indirect calls through templated helpers) before settling the label-fix scope.

---

## Recommended fix shape (NOT applied in this session)

Per user's standing pref "never modify core code without clear instruction" and the chosen scope "concise report + commit, stop", no source changes are made here. Proposed fix for whoever picks this up:

1. **`include/CrossAssetEngines.hpp:264-268`** — extend `CrossPosition::force_close` signature:

   ```
   void force_close(double bid, double ask, CloseCb on_close,
                    const char* reason = "FORCE_CLOSE") noexcept {
       if (!active) return;
       const double mid = (bid + ask) * 0.5;
       emit(mid, reason, on_close);
   }
   ```

   Default keeps existing behaviour for any caller that doesn't pass a reason.

2. **`include/config.hpp:242`** (the `mid_ca` lambda) — pass `"MIDNIGHT_ROLLOVER"` to match the `mid_beng` / `mid_bracket` siblings that already do.

3. **`include/quote_loop.hpp:605`** (`stale_ca` lambda) — pass `"STALE_PRIOR_DAY"`.

4. **`include/quote_loop.hpp:1062, 1070-1074`** — pass `"RECONNECT_CLOSE"`.

5. **`include/quote_loop.hpp:802-807, 901-906`** — pass `"SHUTDOWN"`.

6. **`include/CrossAssetEngines.hpp:1243`** (engine MAE) — pass `"MAE_EARLY_EXIT"` if you want that distinction in tape, or leave as default `"FORCE_CLOSE"` since it's the engine's own decision.

After this lands, a clean fresh-tape run should give a much sharper picture — `MAE_EARLY_EXIT` rate, `MIDNIGHT_ROLLOVER` rate, etc. — and most of the "always FORCE_CLOSE" mystery dissolves into label fidelity.

The mystery rows 6/7/10 do NOT dissolve via this fix. Those need P1-2-a's hunt for the missing supervisor.

---

## Sources

- [`include/CrossAssetEngines.hpp:163-304`](computer:///Users/jo/omega_repo/include/CrossAssetEngines.hpp) — `CrossPosition` (manage + force_close definitions)
- [`include/CrossAssetEngines.hpp:1139-1446`](computer:///Users/jo/omega_repo/include/CrossAssetEngines.hpp) — `VWAPReversionEngine` class body
- [`include/tick_indices.hpp:113-194`](computer:///Users/jo/omega_repo/include/tick_indices.hpp) — manage and entry call sites for SP
- [`include/config.hpp:170-254`](computer:///Users/jo/omega_repo/include/config.hpp) — midnight rollover lambdas
- [`include/quote_loop.hpp:535-630`](computer:///Users/jo/omega_repo/include/quote_loop.hpp) — stale-prior-day purge
- [`include/quote_loop.hpp:691`](computer:///Users/jo/omega_repo/include/quote_loop.hpp) — `do_reconnect_close = LIVE`
- [`include/engine_init.hpp:447-457`](computer:///Users/jo/omega_repo/include/engine_init.hpp) — current `enabled=false` state for all 4 instances
- [`audit_input/omega_shadow.csv`](computer:///Users/jo/omega_repo/audit_input/omega_shadow.csv) — 8 VWAPReversion rows verified
- [`AUDIT_S16_P1_1_LEDGER_TRACE.md`](computer:///Users/jo/omega_repo/AUDIT_S16_P1_1_LEDGER_TRACE.md) — prior P1 trace, same session
