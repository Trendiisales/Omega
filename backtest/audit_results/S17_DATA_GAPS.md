# S17 DATA-GAP AUDIT

Audit of what data was needed during S17 CFE/IMB_EXIT analysis vs what was
actually in logs. Focused on identifying additions that would materially
improve future analysis quality.

## Tier 1 — CRITICAL: log integrity bugs (fix required)

### GHOST-TRADE-1 — CFE PnL calc with uninitialized entry_price

**Evidence:**
- `logs/omega_2026-04-14.log` 12:23:36 — `[CFE] EXIT SHORT @ 4767.37 reason=SL_HIT pnl_raw=-47.6737 pnl_usd=-4767.37 mfe=0.000 held=1776169416s`
- `logs/omega_2026-04-16.log` 08:54:19 — `[CFE] EXIT SHORT @ 4810.93 reason=STAGNATION pnl_raw=-48.1093 pnl_usd=-4810.93 mfe=0.000 held=1776329658s`

**Root cause analysis (derived):**
- `pnl_raw = (entry_px - exit_px) / 100` for SHORT
- If `entry_px == 0`: `pnl_raw = -exit_px/100 = -48.11` (matches observed)
- `held_s = (now_ms - entry_ts_ms) / 1000` with `entry_ts_ms == 0` gives `held = now_ms/1000`, which is the epoch-seconds value (1776329658s = 2026-04-16 08:54 UTC)

**The bug: CFE is processing an exit on a position where `pos.entry` and
`pos.entry_ts_ms` are both zero**, suggesting double-close or close-after-reset.

**Impact on live trading:** These $-4767 and $-4810 "trades" have `[SHADOW]`
tag so no real loss, but if this path fires in LIVE mode the shadow guards
won't catch it. **Potential live risk.**

**Fix required (not in this session — diagnostic only for S18):**
1. At the top of CFE's close path, assert `pos.active && pos.entry > 0 && pos.entry_ts_ms > 0`
2. If not, log `[CFE-GHOST-CLOSE] skipped` and return without emitting EXIT/PnL
3. Add a separate counter `m_ghost_close_attempts` visible in diagnostics

---

### GHOST-TRADE-2 — stdout interleaving on same log line

**Evidence:**
```
08:54:18 [CFE] EXIT LONG @ 4810.93 reason=STAGNATION pnl_raw=-0.0310 pnl_usd=-3.10 mfe=0.420 held=[CFE] STAGNATION-EXIT LONG held=300646300s [SHADOW]
```

Two separate `printf`/`std::cout` calls from different threads interleaved
inside a single line. The token `held=` from one line was followed mid-line
by the entirety of the other line's STAGNATION-EXIT diagnostic.

**Impact:** log parsing is fragile when line is corrupted. The interleaving
produced an artificially massive `held=` value that fed into the ghost-trade
extraction above.

**Fix required:**
- Audit all stdout call sites in CFE (and potentially the whole engine
  stack) to ensure each log line is built as a single `std::stringstream`
  or `std::string` then emitted with one `std::cout <<` call.
- The existing pattern of chained `std::cout <<` across multiple fields is
  not thread-safe even with stdout-level locking because scheduler can
  preempt mid-chain.

---

## Tier 2 — HIGH: data we need that isn't logged

### MISSING-1 — Unrealized PnL / MFE during trade hold

**What we have:** `pos.mfe` is only logged at EXIT time.

**What we need:** A `[CFE-HOLD]` diagnostic firing every ~30s while a position
is open, logging:
- current unrealized pnl_pts
- current mfe
- current mae (max adverse excursion)
- time since entry
- current trail state (armed? trail_sl level?)
- current imb against-tick counter

**Why it matters:** S17's tick-replay simulator had to reconstruct trade
trajectory from the 30s-throttled XAUUSD TICK stream. Having the engine
itself emit its internal state every 30s would:
1. Remove simulator's approximation error
2. Enable live diagnostic of "why didn't this trade hit trail" analysis
3. Reveal pattern: do losing trades peak high then collapse, or fade gradually?

**Cost:** ~30-40 bytes/trade/30s. For 100 trades/day averaging 90s hold = ~300 extra log lines/day. Negligible.

---

### MISSING-2 — ATR on DRIFT entry lines

**What we have:**
```
[CFE] ENTRY SHORT @ 4704.97 sl=4705.92 sl_pts=0.95 ... atr=2.36 spread=0.22 [SHADOW]     <- has atr
[CFE] SUSTAINED-DRIFT-ENTRY LONG @ 4711.42 sl=4707.06 drift=-1.03 ... size=0.010 [SHADOW] <- no atr
```

The regular ENTRY line carries `atr=X.XX`. SUSTAINED-DRIFT-ENTRY and
DRIFT-ENTRY variants do not. Simulator had to approximate ATR from
sl_pts (which is a close but not identical derivation).

**Fix:** Add `atr=X.XX` to SUSTAINED-DRIFT-ENTRY and DRIFT-ENTRY log
lines. One-line change in each printer.

**Impact:** Affects 112 of 263 CFE entries over 8 days (43%). Simulator
ATR error is likely ±5-10% — material for trail-arm calculations.

---

### MISSING-3 — entry_ts_ms precision on EXIT line

**What we have:** `held=34s` (integer seconds).

**What we need:** `held_ms=34567` or `entry_ts_ms=1776817560123` on the
EXIT line.

**Why it matters:** When reconstructing entry timestamp from exit_ts -
held, the round-to-seconds means the "entry ts" I get is accurate to
±1s. For pairing to other log events (e.g. was there an impulse bar
exactly at entry?) this is usually OK but fails when timing matters.

**Cost:** 3-5 extra chars per EXIT line. ~300 lines/day. Negligible.

---

### MISSING-4 — XAUUSD tick-stream granularity for replay

**What we have:** `[TICK] XAUUSD` fires every 30s (verified).

**The engine itself sees every tick** (cTrader delivers ~10 ticks/s during
active markets). The log is throttled 300× for size control.

**Gap:** S17 tick-replay simulator saw 1 tick per 30s. Fast price paths
(sub-10s SL hits, intra-30s trail-arm moves) are invisible.

**Fix (not trivial):**
- Option A: emit `[TICK-MID] XAUUSD price=X.XX` every 1s instead of 30s.
  Cost: 30x more lines. Probably too much.
- Option B: emit a dedicated `[XAUUSD-BOOK]` snapshot every 5s WHILE a
  CFE (or any engine) position is open. Zero overhead when idle,
  high-resolution when useful.
- Option C: dump tick stream to a dedicated `l2_ticks_<date>.csv` file
  (this already exists per backtest/ -- verify it's still being written).

**Verification needed for S18:** check `C:\Omega\logs\l2_ticks_*.csv` — if
that file exists and has millisecond resolution, the S17 simulator should
have used it rather than the throttled `[TICK]` log.

---

### MISSING-5 — Trade outcome reason consistency

**What we have:** Exit reasons are free-form strings across engines:
- CFE: `SL_HIT`, `TRAIL_SL`, `IMB_EXIT`, `STAGNATION`, `FORCE_CLOSE`,
  `PARTIAL_TP`, `ADVERSE_EARLY`, `TIMEOUT`, `BE`
- GoldFlow: `SL_HIT`, `ADVERSE_EARLY`
- Bracket: `BREAKOUT_FAIL`, `TRAIL_HIT`, `SL_HIT`, `TP_HIT`

No single normalised classification of "winner cut short" vs "loser cut
correctly" vs "full-win" vs "full-loss".

**Fix:** Add one field to every SHADOW-CLOSE line: `outcome_class=WINNER_CUT|LOSER_CUT|WINNER_FULL|LOSER_FULL|STOP_HIT|BE`.
Classification rule (suggested):
- `WINNER_FULL` : net>0 AND mfe>=1.5*atr AND exit in {TRAIL_SL, TP_HIT}
- `WINNER_CUT`  : net>0 AND mfe>=1.0*atr AND exit in {IMB_EXIT, TIMEOUT, STAGNATION}
- `LOSER_CUT`   : net<0 AND mfe<0.5*atr AND exit in {IMB_EXIT, TIMEOUT, ADVERSE_EARLY}
- `STOP_HIT`    : exit == SL_HIT (regardless of mfe)
- `LOSER_FULL`  : net<0 AND exit in {FORCE_CLOSE, BREAKOUT_FAIL}
- `BE`          : net≈0

**Impact:** eliminates the S17 mistake where I used IMB_EXIT's PnL-at-exit
as a proxy for signal value without the counterfactual. A `WINNER_CUT`
flag would have made that bias obvious.

---

## Tier 3 — MEDIUM: friction / consistency gaps

### FRICTION-1 — TRADE-COST vs SHADOW-CLOSE format mixing

**What we have:** Old format (pre-2026-04-21):
```
[CFE] EXIT LONG @ 4712.03 reason=IMB_EXIT pnl_raw=0.0030 pnl_usd=0.30 mfe=1.900 held=43s [SHADOW]
[TRADE-COST] XAUUSD gross=$0.30 slip_in=$0.00 slip_out=$0.00 net=$0.27 exit=IMB_EXIT
```

New format (2026-04-21+):
```
[CFE] EXIT LONG @ 4712.03 reason=IMB_EXIT pnl_raw=0.0030 pnl_usd=0.30 mfe=1.900 held=43s [SHADOW]
[SHADOW-CLOSE] XAUUSD engine=CandleFlowEngine side=LONG gross=$0.30 net=$0.27 exit=IMB_EXIT reason=engine_shadow -- ...
```

**Gap:** NEW format dropped `slip_in` / `slip_out` breakdown. You can compute
`slip = gross - net` but for SHORT positions with pre-existing adverse slip
the sign convention differs and it's a minor-annoyance-grade defect.

**Fix:** Add `slip_in=$X.XX slip_out=$Y.YY` to SHADOW-CLOSE line.

---

### FRICTION-2 — No single normalized trade record per close

Every close fires **2 lines** (engine EXIT + SHADOW-CLOSE). Extractor has
to join them by ts+engine+side. Works but fragile:
- If two engines close at the same second on same side, pairing is ambiguous
- If one line is dropped (scheduler preemption etc), the other orphans

**Fix:** Emit ONE canonical line per closed trade:
```
[TRADE-CLOSED] ts_ms=1776857194123 symbol=XAUUSD engine=CandleFlowEngine
               side=LONG entry_px=4712.03 exit_px=4715.27 entry_ts_ms=...
               size=0.010 gross=$3.24 net=$3.12 slip_in=$0.06 slip_out=$0.06
               mfe=2.1 mae=-0.3 held_ms=42847 exit_reason=TRAIL_SL
               outcome_class=WINNER_FULL trail_active=1 [SHADOW|LIVE]
```

Single line, all fields, trivial to parse, no join needed.

---

### FRICTION-3 — session_slot not on entry line

**What we have:** `[CFE] ENTRY ...` has rsi_trend, atr, spread. Not session_slot.

`session_slot` (1-6: Asia, London-open, London-main, NY-open, NY-main, after-hours)
is logged only on `[GOLD-BRK-DIAG]` every 10s. To attribute a trade to a
session we have to time-match back to the nearest DIAG.

**Fix:** Add `slot=N` to every engine ENTRY line.

**Impact:** Makes per-session performance slicing possible without
time-matching. Slot-based analysis would have shown e.g. whether the
CFE IMB_EXIT inflection point varies by session (it probably does).

---

## Tier 4 — LOW: nice-to-have

- `m_imb_against_ticks` counter value on regular EXIT lines (not just IMB-EXIT)
- VWAP distance / bar imbalance at entry (bar context)
- Whether `m_prev_depth_bid/ask` was zero or stale at IMB decision time

None of these would have changed the S17 conclusion; they're for finer
per-trade debugging.

---

## Recommended S18 logging PR

Priority order by audit impact:

1. **CFE ghost-close guard** (Tier 1 CRITICAL) — live-risk
2. **`outcome_class` field on closes** (Tier 2 MEDIUM-HIGH) — biggest analysis
   quality lift, reveals "winner cut" bias that S17 nearly fell for
3. **`[CFE-HOLD]` mid-trade state diagnostic** (Tier 2 HIGH) — removes
   simulator approximation error for future tuning
4. **ATR on DRIFT-ENTRY lines** (Tier 2 HIGH) — one-line change, closes
   43% of entries coverage gap
5. **Single canonical `[TRADE-CLOSED]` line** (Tier 3 MEDIUM) — cleans up
   extractor fragility; makes future audits ~5x faster
6. **stdout interleaving audit** (Tier 1 CRITICAL) — not a log addition,
   but a code audit of `std::cout << A << B << ...` patterns

Estimated effort: 2-3 hours total for #2, #3, #4, #5 (log format changes
only, no logic changes).

## What we should NOT add

- Per-tick PnL in main log → too verbose; use dedicated `[CFE-HOLD]` every 30s instead
- Depth level detail per trade → already in `l2_ticks_*.csv`, don't duplicate
- Regime scores per entry → already in [SUPERVISOR-XAUUSD] diag lines
