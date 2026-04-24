# S17 DATA-GAP AUDIT

Audit of what data was needed during S17 CFE/IMB_EXIT analysis vs what was
actually in logs. Focused on identifying additions that would materially
improve future analysis quality.

## ~~Tier 1 — CRITICAL: log integrity bugs~~ ALREADY FIXED PRE-S17

### ~~GHOST-TRADE-1 — CFE PnL calc with uninitialized entry_price~~

**Status: RESOLVED before S17.**

**Original observation (this doc first version, now corrected):** 2 trades in
the 10-day log window showed `pnl_usd=-$4767.37` / `-$4810.93` with
`held=1776xxxxxs` (UTC epoch seconds). These were on 2026-04-14 12:23:36
and 2026-04-16 08:54:19. The extractor's `|net| > $1000` filter caught them.

**Root cause:** double-close race condition where two threads entered
`close_pos()` on the same position. The second thread saw `pos = OpenPos{}`
already reset by the first, so the second's `tr.pnl` computed with
`pos.entry == 0` (→ `pnl_raw = -exit_px/100 = -48.11`) and `held` computed
from `now_ms - entry_ts_ms` with `entry_ts_ms == 0` (→ `held = epoch_sec`).

**Fix landed:** commit `602aed07` on **2026-04-20 19:36 UTC** — added
`m_close_mtx` mutex + `!pos.active` re-check inside the lock so second
entrants bail silently (see line 1240-1250 of `CandleFlowEngine.hpp`).

**Verification:** post-fix days 04-21, 04-22, 04-23 scanned for any CFE
trade with `|pnl_usd| > 300` or `held > 86400` — **zero residual ghosts.**

**The 2 observed ghosts were pre-fix data**, correctly filtered out by the
audit extractor. No new code required. **This section was originally
mis-categorised by S17 as a live risk. It is not.**

### ~~GHOST-TRADE-2 — stdout interleaving on same log line~~

**Status: PARTIALLY RESOLVED; S17 additional hardening landing now.**

Observed line on 04-14 12:23:36:
```
[CFE] SL_HIT SHORT @ 4[CFE] SL_HIT SHORT @ 4767.37 sl=4767.22 trail=0
```

Two separate `std::cout << A << B << C` chains from different threads
interleaved mid-line. The mutex landed in 602aed07 serializes close paths
(so the double-close problem is gone), but chained `std::cout << ...`
across multiple fields in OTHER paths (ENTRY lines, TRAIL-ENGAGED, etc.)
is still not atomic under thread preemption.

**S17 hardening (commit will follow this doc):** convert the two most
frequent chained-cout sites (`[CFE] EXIT` in close_pos_locked and
`[SHADOW-CLOSE]` in trade_lifecycle.hpp) to `ostringstream` → single
`std::cout <<` write. Prevents any future recurrence on these paths even
if another race sneaks in.

---

## Tier 2 — HIGH: data we need that isn't logged (S17 landing these now)

### MISSING-1 — Unrealized PnL / MFE during trade hold

**What we had:** `pos.mfe` only logged at EXIT time.

**What S17 adds:** `[CFE-HOLD]` diagnostic, emitted every 30s while a
position is open. Fields: unrealized_pts, unrealized_usd, mfe, mae,
hold_s, trail_active, trail_sl, atr, imb_ticks, l2_imb.

**Why it matters:** S17's tick-replay simulator had to reconstruct trade
trajectory from the 30s-throttled XAUUSD TICK stream. Having the engine
itself emit its internal state every 30s:
1. Removes simulator's approximation error (was biased both directions)
2. Enables live diagnostic of "why didn't this trade hit trail" analysis
3. Reveals pattern: do losing trades peak high then collapse, or fade?

**Cost:** ~30-40 bytes/trade/30s. For 100 trades/day averaging 90s hold
≈ 300 extra log lines/day. Negligible.

### MISSING-2 — ATR on DRIFT entry lines

**What we had:**
```
[CFE] ENTRY SHORT @ 4704.97 sl=4705.92 ... atr=2.36 spread=0.22     <- has atr
[CFE] SUSTAINED-DRIFT-ENTRY LONG @ 4711.42 sl=4707.06 drift=-1.03 ... size=0.010  <- no atr
```

The regular ENTRY line carried `atr=X.XX`. SUSTAINED-DRIFT-ENTRY and
DRIFT-ENTRY variants did not. Simulator had to approximate ATR from
sl_pts (close but not identical).

**What S17 adds:** `atr=X.XX` on DRIFT-ENTRY and SUSTAINED-DRIFT-ENTRY
lines. Two one-liners in existing printers.

**Impact:** affects 112 of 263 CFE entries over 8 days (43%). Simulator
ATR error was ±5-10% — material for trail-arm calculations.

### MISSING-3 — entry_ts_ms precision on EXIT line

**What we had:** `held=34s` (integer seconds).

**What S17 adds:** `entry_ts_ms=NNNNNNNNNNN` (int64 epoch-ms) on the EXIT
line so extractors no longer need to derive entry time from held-seconds.

**Cost:** ~20 extra chars per EXIT line. ~300 lines/day.

### MISSING-4 — Trade outcome classification

**What we had:** exit reasons as free-form strings. No single normalised
classification of "winner cut short" vs "loser cut correctly" vs
"full-win" vs "full-loss."

**What S17 adds:** `outcome_class=<CLASS>` field on every SHADOW-CLOSE line.
Classes:
- `WINNER_FULL` : net>0 AND exit in {TRAIL_SL, TP_HIT, TRAIL_HIT}
- `WINNER_CUT`  : net>0 AND exit in {IMB_EXIT, TIMEOUT, STAGNATION,
                  ADVERSE_EARLY, PARTIAL_TP}
- `LOSER_CUT`   : net<0 AND exit in {IMB_EXIT, TIMEOUT, STAGNATION,
                  ADVERSE_EARLY, PARTIAL_TP}
- `STOP_HIT`    : exit == SL_HIT (regardless of mfe)
- `LOSER_FULL`  : net<0 AND exit in {FORCE_CLOSE, BREAKOUT_FAIL, FORCE_STOP}
- `BE`          : |net| < $0.20
- `OTHER`       : fallback

**Impact:** eliminates the S17 mistake where I used IMB_EXIT's PnL-at-exit
as a proxy for signal value without the counterfactual. A `WINNER_CUT`
flag would have made that bias obvious immediately rather than requiring
a tick-replay simulator to catch it.

---

## Tier 3 — MEDIUM: friction (deferred to S18+)

### FRICTION-1 — TRADE-COST vs SHADOW-CLOSE format mixing

Old-format logs (pre-04-21) emit both `[TAG] EXIT` + `[TRADE-COST]` lines.
New-format (04-21+) emit `[TAG] EXIT` + `[SHADOW-CLOSE]`. NEW format
dropped `slip_in` / `slip_out` breakdown. Can compute `slip = gross - net`
but sign convention annoys for SHORT.

**Fix (deferred):** add `slip_in=$X.XX slip_out=$Y.YY` to SHADOW-CLOSE.

### FRICTION-2 — No single normalised trade record

Every close fires 2 lines (engine EXIT + SHADOW-CLOSE). Extractor has to
join them. With `entry_ts_ms` now on EXIT and `outcome_class` on SHADOW-CLOSE,
the pairing is more robust but still required.

**Fix (deferred, larger change):** single canonical `[TRADE-CLOSED]` line
per close with all fields.

### FRICTION-3 — session_slot not on entry line

Currently only on `[GOLD-BRK-DIAG]` every 10s. To attribute a trade to a
session requires time-matching.

**Fix (deferred):** add `slot=N` to every engine ENTRY line.

---

## Tier 4 — LOW: nice-to-have

- `m_imb_against_ticks` counter value on regular EXIT lines
- VWAP distance / bar imbalance at entry
- Whether `m_prev_depth_bid/ask` was zero or stale at IMB decision time

---

## Summary of S17 log additions landing

| File | Change |
|------|--------|
| `include/CandleFlowEngine.hpp` | `atr=` on DRIFT-ENTRY and SUSTAINED-DRIFT-ENTRY |
| `include/CandleFlowEngine.hpp` | `[CFE-HOLD]` mid-trade diagnostic every 30s (ostringstream-atomic) |
| `include/CandleFlowEngine.hpp` | `m_pos_mae` tracking; `mae=` on EXIT; reset on entry |
| `include/CandleFlowEngine.hpp` | `entry_ts_ms=` on EXIT line |
| `include/CandleFlowEngine.hpp` | EXIT line converted to ostringstream-atomic |
| `include/trade_lifecycle.hpp` | `outcome_class=` field on SHADOW-CLOSE |
| `include/trade_lifecycle.hpp` | SHADOW-CLOSE converted to ostringstream-atomic |

## What we should NOT add (correctly identified first pass)

- Per-tick PnL in main log → too verbose; 30s CFE-HOLD is the right tradeoff
- Depth level detail per trade → already in `l2_ticks_*.csv`, don't duplicate
- Regime scores per entry → already in `[SUPERVISOR-XAUUSD]` diag lines

