# XauusdFvgEngine — design plan for the C++ live harness port

**Status:** Plan-first, code-second. Sign off / mark up this doc, then the next
chat writes `include/XauusdFvgEngine.hpp` end-to-end against it.

**Lineage:** `HANDOFF_FVG_BACKTEST.md` (XAUUSD 15m cleared for VPS shadow,
PF 1.95–2.44 OOS over two WF cuts). v3 of `scripts/fvg_pnl_backtest_v3.py`
is the bit-exact reference; `scripts/usdjpy_xauusd_fvg_signal_test.py` is the
core (NEVER MODIFY) — its FVG detection / scoring / ATR algorithms must be
ported faithfully into C++.

**Sister engines for pattern matching:**
- `include/UsdjpyAsianOpenEngine.hpp` (S59) — closest in spirit; tick-driven,
  bracket-style, shadow-mode-pinned, emits `omega::TradeRecord` via callback.
- `include/GoldHybridBracketEngine.hpp` / `include/GoldMidScalperEngine.hpp` —
  XAUUSD-side conventions for tick math, pip scale, gold-coordinator
  registration.

---

## 1. Architectural shape

A new header, `include/XauusdFvgEngine.hpp`, defining `class XauusdFvgEngine`
in `namespace omega`. Mirrors the S59 USDJPY Asian-Open class structure:

- Public member constants for every v3 parameter (frozen — see §3).
- Private state: 15-min OHLC bar accumulator, ATR(14) RMA state, tick-volume
  20-bar rolling mean, three-bar look-back for FVG detection, queue of
  unmitigated FVGs, single-position state.
- Public `on_tick(bid, ask, now_ms, can_enter, on_close, ...)` that the
  gold dispatcher (`tick_gold.hpp`) calls every XAUUSD tick.
- Public `force_close(...)` mirroring S59 for emergency unwinds.
- `bool shadow_mode = true` member, pinned to `true` (not `kShadowDefault`)
  in `engine_init.hpp` on first deployment per the same convention as
  `g_usdjpy_asian_open` and `g_gold_midscalper`. Promotion to live happens
  AFTER explicit operator decision following ≥2-week shadow run with WR
  matching the 50% backtest expectation and PF in the 1.5–1.8 band.
- `bool has_open_position()` proxy so the gold-side `gold_any_open`
  exclusion gate can include this engine.
- `using CloseCallback = std::function<void(const omega::TradeRecord&)>;`
  same callback shape as S59.

The XauusdFvg engine is a **15-minute-bar engine on a tick stream**, not a
tick-window-structural engine like S59 (which is a compression-bracket on a
tick window). That's the single biggest structural difference from the sister
engines and dictates how the on-tick path is laid out.

---

## 2. On-tick state machine

The engine has two interleaved pipelines on every tick: (a) the bar
accumulator that closes a 15-min bar and runs FVG detection, and (b) the
position-management loop that runs on every tick once a trade is live.

```
on_tick(bid, ask, now_ms, ...):
  mid    = (bid + ask) / 2
  spread = ask - bid
  now_s  = now_ms / 1000

  # (1) Bar accumulator -- always runs.
  bar_idx = floor(now_s / 900)              # 900s = 15min, UTC-aligned
  if bar_idx != current_bar_idx:
      if current_bar_idx != UNSET:
          close_bar()                       # finalize OHLC, update ATR/tv_mean
          on_bar_close(closed_bar)          # FVG detection, mitigation check
      current_bar_idx = bar_idx
      open_bar(mid, now_s)                  # start fresh bar
  else:
      update_bar(mid)                       # extend high/low/close, ++tick_count

  # (2) Position management -- on every tick when LIVE.
  if pos.active:
      manage(bid, ask, mid, now_s, on_close)
      return                                # one position at a time

  # (3) Pending-mitigation check on tick prices (for in-bar entries).
  if can_enter:
      try_intrabar_entry(bid, ask, mid, now_s, on_close)
```

Critical: v3 uses **bar high/low** for SL/TP hit detection. To stay
bit-equivalent with the backtest, the live engine should ALSO check SL/TP on
**tick prices** (live data) and, for retro-validation, log the bar-level hit
that v3 would have detected. In practice, the differences are small (0–1
tick of slippage either way), but we should not silently change the rule —
we mirror v3 first, log live-vs-bar deltas in shadow, and only adjust if
live-vs-shadow tracking justifies it.

---

## 3. Frozen parameters (mirror v3 #5 ACCEPTED config)

```cpp
static constexpr const char* SYMBOL = "XAUUSD";
static constexpr int    BAR_SECS              = 900;       // 15-min, UTC-aligned
static constexpr int    ATR_PERIOD            = 14;        // RMA, alpha=1/14
static constexpr int    TICK_VOLUME_WINDOW    = 20;        // 20-bar rolling mean
static constexpr double MIN_GAP_ATR           = 0.10;      // FVG sizing filter
static constexpr double MAX_GAP_ATR           = 5.0;
// Score weights -- v3 production set (Phase-0 reweight v1).
// trend_align and age_decay both DROP OUT (weight 0). Only gap, disp, tv
// contribute. EMA20/EMA50 NOT required for production scoring.
static constexpr double W_GAP_SIZE            = 1.5;
static constexpr double W_DISPLACEMENT        = 1.0;
static constexpr double W_TICK_VOLUME         = 1.0;
static constexpr double W_TREND_ALIGN         = 0.0;       // DROPPED
static constexpr double W_AGE_DECAY           = 0.0;       // DROPPED
// Score gate -- frozen per HANDOFF_FVG_BACKTEST.md "frozen cutoff at 0.48
// with quarterly re-validation" recommendation. Re-validation pass at
// month 3 / 6 / 9 etc. against accumulated live + recent-history data;
// pause if quarter Top PF < 1.2.
static constexpr double SCORE_CUTOFF          = 0.48;
// Risk parameters -- frozen per v3 #5.
static constexpr double SL_ATR_MULT           = 2.5;
static constexpr double TP_ATR_MULT           = 5.0;
static constexpr int    TIME_STOP_BARS        = 60;        // = 15h on 15m
static constexpr double RISK_PER_TRADE_PCT    = 0.005;     // 0.5%
// Cost model -- mirror v3 backtest: 0.5 pips/side slippage at pip=0.10 USD.
static constexpr double SLIPPAGE_PIPS         = 0.5;
static constexpr double PIP_SIZE              = 0.10;      // XAUUSD
// Mitigation-search horizon -- v3 max_age_for_test=500 bars; we cap pending
// FVGs to avoid unbounded queue growth. 500 bars * 15min = 5.2 days. After
// 500 bars without mitigation, drop the FVG.
static constexpr int    MAX_AGE_BARS          = 500;
// Reaction lookforward isn't relevant in the live engine -- v3 uses it only
// for phase-0 stats. The live trade itself just runs until SL/TP/time-stop.
```

**Open question for sign-off (sizing):** the backtest sized to $500 risk
on $100k notional. That implies lot ≈ 500 / (2.5×ATR×100). For ATR ∈ [1, 3]
USD, lot ∈ [0.67, 2.0]. The S59 USDJPY engine caps at LOT_MAX = 0.20 and
runs around the cap. We need an explicit `LOT_MAX` for FVG XAUUSD. Three
options to settle in next chat before the .hpp is written:

  1. `LOT_MAX = 0.20` (match S59 cohort, MUCH smaller than backtest).
     Trades will hit cap on every fire and effectively run at fixed size.
     Real expectancy will then NOT be 0.5%-of-$100k risk, it will be the
     backtest expectancy scaled down by `LOT_MAX / typical_calculated_lot`.
  2. `LOT_MAX = 1.00` (compromise; allows the backtest math to bind on
     low-ATR fires and only caps on high-ATR ones).
  3. `LOT_MAX = 2.00` (matches backtest worst case). Higher capital-at-risk
     ceiling; review against broker margin headroom and the
     `OmegaCrowdingGuard` / `OmegaVolTargeter` budget.

§4.5 below covers the actual sizing math.

---

## 4. Data structures

### 4.1 Pending FVG queue

```cpp
struct PendingFvg {
    int64_t  formed_bar_idx = 0;              // for age tracking
    char     direction      = 'B';            // 'B'=bull, 'S'=bear
    double   zone_low       = 0.0;
    double   zone_high      = 0.0;
    double   gap_height     = 0.0;
    // Score components frozen at formation.
    double   s_gap_size     = 0.0;
    double   s_displacement = 0.0;
    double   s_tick_volume  = 0.0;
    double   score_at_formation = 0.0;        // diagnostic only -- entry
                                              // score is recomputed at
                                              // mitigation against the live
                                              // ATR/tv state at that bar.
    int64_t  formed_ts      = 0;              // for log lines
};
std::deque<PendingFvg> m_pending_fvgs;        // capped at MAX_PENDING_FVGS
```

`MAX_PENDING_FVGS` = 64 should be ample. Real-world FVG formation rate on
XAUUSD 15m is ~3–5/day; queue rarely exceeds 8.

### 4.2 Bar accumulator

```cpp
struct BarAcc {
    int64_t  bar_idx     = -1;                // floor(now_s / 900)
    int64_t  start_ts    = 0;
    double   open        = 0.0;
    double   high        = 0.0;
    double   low         = 0.0;
    double   close       = 0.0;
    double   spread_sum  = 0.0;
    int      tick_count  = 0;
};
BarAcc m_cur_bar;
// Last 3 closed bars for FVG detection (i-2, i-1, i).
struct ClosedBar {
    double open, high, low, close, spread_mean;
    int    tick_count;
    int64_t bar_idx, ts;
};
std::array<ClosedBar, 3> m_recent_bars;       // ring; m_recent_bars[2] = newest
int m_bars_seen = 0;
// ATR(14) RMA: alpha = 1/14, EWM-style. Matches v3:
//   atr_t = (prev_close, h, l) -> TR; atr.ewm(alpha=1/14, adjust=False).mean()
double m_atr14        = 0.0;
double m_prev_close   = 0.0;
bool   m_atr_ready    = false;                // true once >= ATR_PERIOD bars
int    m_atr_warmup   = 0;
// 20-bar rolling tick-count mean for the tick_volume score component.
std::deque<int> m_tick_count_history;
double m_tv_mean      = 0.0;
```

### 4.3 Live position (mirror S59 layout)

```cpp
struct LivePos {
    bool    active           = false;
    bool    is_long          = false;
    double  entry            = 0.0;           // gross entry (zone_high/low)
    double  entry_with_cost  = 0.0;           // entry + side_cost  (or - for short)
    double  tp               = 0.0;           // gross TP price
    double  sl               = 0.0;           // gross SL price
    double  size             = 0.0;           // lot
    double  atr_at_entry     = 0.0;
    double  score_at_entry   = 0.0;
    double  spread_at_entry  = 0.0;
    double  mfe              = 0.0;
    double  mae              = 0.0;
    int64_t entry_ts         = 0;
    int64_t entry_bar_idx    = 0;             // for time-stop exit
    char    session          = 'X';           // 'A','L','N','O' for log/CSV
} pos;
```

No BE-lock state — v3 frozen config is BE OFF.

---

## 5. Algorithmic details (bit-equivalent with v3)

### 5.1 ATR(14) RMA — matches `usdjpy_xauusd_fvg_signal_test.py:atr()`

```
On each closed bar:
    tr = max( |high - low|,
              |high - prev_close|,
              |low  - prev_close| )
    if !m_atr_ready:
        m_atr14 = (m_atr_warmup == 0) ? tr
                                      : m_atr14 * (1 - 1.0/14.0)
                                      + tr * (1.0/14.0)
        if ++m_atr_warmup >= ATR_PERIOD: m_atr_ready = true
    else:
        m_atr14 = m_atr14 * (1 - 1.0/14.0) + tr * (1.0/14.0)
    m_prev_close = bar.close
```

Note: pandas `ewm(alpha=1/14, adjust=False)` seeds at the first sample, so
seed `m_atr14 = tr` on the first bar. Bit-equivalent.

### 5.2 FVG detection (3-bar) — matches `detect_fvgs()`

After each bar close, if `m_atr_ready` AND we have 3 closed bars:

```
b0 = m_recent_bars[0]      # i-2
b1 = m_recent_bars[1]      # i-1 (middle / displacement bar)
b2 = m_recent_bars[2]      # i   (formation bar)

if b0.high < b2.low:
    direction  = BULL
    gap        = b2.low - b0.high
    zone_low   = b0.high
    zone_high  = b2.low
elif b0.low > b2.high:
    direction  = BEAR
    gap        = b0.low - b2.high
    zone_low   = b2.high
    zone_high  = b0.low
else:
    return                  # no FVG

ratio = gap / m_atr14
if ratio < MIN_GAP_ATR or ratio > MAX_GAP_ATR: return
```

### 5.3 Score components (formation-time fields)

```
# Gap-size component (clipped to [0,1]):
s_gap = clamp((ratio - MIN_GAP_ATR) / (2.0 - MIN_GAP_ATR), 0.0, 1.0)

# Displacement: middle bar body / range, weighted by clamp(range/ATR/2,0,1)
mid_body  = |b1.close - b1.open|
mid_range = b1.high - b1.low
s_disp    = (mid_range > 0)
            ? clamp((mid_body/mid_range) * clamp(mid_range/m_atr14/2.0, 0, 1), 0, 1)
            : 0.0

# Tick-volume: middle-bar tick_count / 20-bar rolling mean. Maps 1x->0,3x->1.
s_tv = (m_tv_mean > 0)
       ? clamp((b1.tick_count/m_tv_mean - 1.0) / 2.0, 0, 1)
       : 0.0

# trend_align and age_decay both have weight 0 -- skip (don't compute).

# Production weighted average:
score_at_formation =
    (W_GAP_SIZE * s_gap + W_DISPLACEMENT * s_disp + W_TICK_VOLUME * s_tv)
  / (W_GAP_SIZE + W_DISPLACEMENT + W_TICK_VOLUME)
```

Push a `PendingFvg` onto `m_pending_fvgs`.

### 5.4 Mitigation entry — matches `measure_reactions()` first-touch

Two checks per tick / per bar:

**Bar-level (post-close, runs in `on_bar_close`):**
For every pending FVG, ask: did the just-closed bar enter the zone?
```
if bar.high >= fv.zone_low AND bar.low <= fv.zone_high:
    # Mitigation hit. Score-recompute happens here.
    if !m_atr_ready: skip (cannot size)
    score_at_entry = score_at_formation     # weights for s_age=0 -> identical
    if score_at_entry < SCORE_CUTOFF:
        drop fvg                             # gate failed
    else:
        open_position(fv, bar, ...)          # see §5.5
```

For production weights (`W_AGE_DECAY = 0`), `score_at_entry` is bit-equivalent
to `score_at_formation` — age decay drops out cleanly. Confirmed by inspection
of v3's `measure_reactions()`: when `W_AGE_DECAY = 0`, the age component does
not contribute to the weighted average.

**Tick-level (intrabar, optional in v1, default OFF):**
For latency-sensitive paths we COULD trigger entry on the first tick that
crosses into the zone, but this DRIFTS from the bit-equivalent backtest.
Recommendation: **bar-level mitigation only in v1**. Add intrabar entry as
an opt-in `bool m_intrabar_entry = false;` later if shadow shows we're
missing too many fast mitigations. Decision deferred to month-3 review.

### 5.5 Order construction (mirror v3 `simulate_trade()`)

```
half_spread = max(spread_mean / 2.0, 0.0)
slip        = SLIPPAGE_PIPS * PIP_SIZE
side_cost   = half_spread + slip          # cost_multiplier = 1.0 in production

if direction == BULL:
    gross_entry     = fv.zone_high
    entry_with_cost = gross_entry + side_cost
    sl              = gross_entry - SL_ATR_MULT * m_atr14
    tp              = gross_entry + TP_ATR_MULT * m_atr14
    is_long         = true
else:                                     # BEAR
    gross_entry     = fv.zone_low
    entry_with_cost = gross_entry - side_cost
    sl              = gross_entry + SL_ATR_MULT * m_atr14
    tp              = gross_entry - TP_ATR_MULT * m_atr14
    is_long         = false

risk_per_unit = SL_ATR_MULT * m_atr14
if risk_per_unit <= 0: skip               # safety
```

### 5.6 Sizing (depends on resolved `LOT_MAX` from §3 open question)

```
risk_dollars = RISK_PER_TRADE_PCT * STARTING_EQUITY  # default $500
# tick_value_multiplier for XAUUSD = 100 USD/price-point/lot.
# So sl_dist (price units) * 100 = USD per lot at full sl_dist.
calculated_lot = risk_dollars / (risk_per_unit * 100.0)
size           = clamp(calculated_lot, LOT_MIN, LOT_MAX)
```

Open question: should `STARTING_EQUITY` be a config value pulled from
`g_cfg`? S59 hardcodes the sizing budget directly. Consistency argues for
hardcoding here too.

### 5.7 Per-tick management (mirror S59 `manage()` minus BE)

```
move = is_long ? (mid - gross_entry) : (gross_entry - mid)
if move > pos.mfe: pos.mfe = move
if move < pos.mae: pos.mae = move

# Time stop -- elapsed bars >= TIME_STOP_BARS: exit at next bar's open.
elapsed_bars = (now_s - pos.entry_ts) / BAR_SECS
if elapsed_bars >= TIME_STOP_BARS:
    # In v3, time-stop exit is at the NEXT bar's open, not the time-stop
    # bar's open. So we set a flag here and trigger close on the next bar
    # boundary in on_bar_close.
    pos.time_stop_pending = true

# TP / SL hit (priority: SL on intrabar tie, matching v3's `if sl_hit and tp_hit`):
sl_hit = is_long ? (bid <= pos.sl) : (ask >= pos.sl)
tp_hit = is_long ? (ask >= pos.tp) : (bid <= pos.tp)

if sl_hit:                                # SL takes precedence on tie
    _close(pos.sl, "SL_HIT", ...)
elif tp_hit:
    _close(pos.tp, "TP_HIT", ...)
```

In `on_bar_close`, also handle the deferred time-stop:
```
if pos.active and pos.time_stop_pending:
    _close(closed_bar.open_of_NEXT_bar -- i.e. pos.entry + something, "TIME_STOP", ...)
```
Actually we exit on the next bar's open, which means we trigger the close on
the FIRST tick of the next bar (effectively `mid` at that tick). Cleanest
path: set `pos.time_stop_pending = true` when `elapsed_bars >= TIME_STOP_BARS`,
and have the `on_tick` head close at the first tick after the bar boundary
flips.

### 5.8 `_close()` -- emit `omega::TradeRecord`

```cpp
omega::TradeRecord tr;
tr.id            = ++m_trade_id;
tr.symbol        = "XAUUSD";
tr.side          = pos.is_long ? "LONG" : "SHORT";
tr.engine        = "XauusdFvg";
tr.regime        = "FVG_15M";
tr.entryPrice    = pos.entry_with_cost;
tr.exitPrice     = exit_price - (pos.is_long ? side_cost : -side_cost);
                                                    // mirror v3 net_pnl model
tr.tp            = pos.tp;
tr.sl            = pos.sl;
tr.size          = pos.size;
tr.pnl           = (pos.is_long ? (exit_price - pos.entry)
                                : (pos.entry - exit_price)) * pos.size;
tr.mfe           = pos.mfe * pos.size;
tr.mae           = pos.mae * pos.size;
tr.entryTs       = pos.entry_ts;
tr.exitTs        = now_s;
tr.exitReason    = reason;                          // "SL_HIT" / "TP_HIT" / "TIME_STOP"
tr.spreadAtEntry = pos.spread_at_entry;
tr.atr_at_entry  = pos.atr_at_entry;
tr.shadow        = shadow_mode;

// Bracket-style fields not used (FVG is not a bracket engine).
tr.bracket_hi    = 0.0;
tr.bracket_lo    = 0.0;

if (on_close) on_close(tr);
```

Note: shadow_mode populates `tr.shadow`; `handle_closed_trade()` short-
circuits shadow records to audit-only logging per
`OmegaTradeLedger.hpp:71–77`. Ledger / consec-loss / crowding-guard / param-
gate / time-of-day-gate / auto-disable state will not be polluted.

---

## 6. Quarterly re-validation logging

Per the handoff doc's quarterly re-validation step, every closed trade
needs `score_at_entry`, `atr_at_entry`, `session`, and `exit_reason` logged
with enough precision to re-feed `fvg_pnl_backtest_v3.py --train-end <date>`
treating the live period as the test window. Approach:

- `omega::TradeRecord` already carries `atr_at_entry` and `exitReason`.
- Add a side-channel CSV `live_xauusd_fvg.csv` written by a registered
  `on_close` callback in `engine_init.hpp`. Columns:
  `id, entry_ts, exit_ts, side, score_at_entry, atr_at_entry, session,
  spread_at_entry, gross_entry, exit_price, sl, tp, size, pnl, exit_reason,
  shadow`.
- `score_at_entry` is engine-internal; route it via a separate atomic
  on the engine (or extend `TradeRecord` with a `score_at_entry` field —
  cleanest, but touches `OmegaTradeLedger.hpp` which may count as core).
  **Open question:** is `OmegaTradeLedger.hpp` considered core (untouchable)
  or harness scaffolding (extendable)? Pattern check: S59 doesn't add fields
  to TradeRecord, so default to "side-channel CSV with a custom logger".

---

## 7. Integration points

### 7.1 `engine_init.hpp`

Add at the same level as `g_usdjpy_asian_open`:

```cpp
// 2026-05-XX: XauusdFvgEngine -- pinned shadow-only on first deployment
//   regardless of g_cfg.mode. FVG-on-15m engine for XAUUSD per
//   HANDOFF_FVG_BACKTEST.md / DESIGN_XAUUSD_FVG_ENGINE.md. Backtest expects
//   PF 1.5-1.8, ~50% WR, ~25 trades/month. Promote to kShadowDefault after a
//   3-month shadow run that clears the four-gate quarterly re-validation
//   (n>=50, PF>=1.2, PF>All, cost-stress 2x PF>=1.0). Until then this line
//   stays as `true`.
g_xauusd_fvg.shadow_mode = true;
g_xauusd_fvg.cancel_fn   = [](const std::string& id) { send_cancel_order(id); };
g_xauusd_fvg.on_close_cb = [](const omega::TradeRecord& tr) {
    handle_closed_trade(tr);                       // standard ledger path
    log_xauusd_fvg_csv(tr);                        // side-channel for re-validation
};
```

### 7.2 `globals.hpp`

Add `static omega::XauusdFvgEngine g_xauusd_fvg;` next to `g_usdjpy_asian_open`.

### 7.3 `tick_gold.hpp`

Two additions in `on_tick_gold`:

1. Extend `gold_any_open` to include `g_xauusd_fvg.has_open_position()`. The
   FVG engine respects the gold one-at-a-time invariant (it does not run
   alongside a live GoldStack/GoldHybrid/etc. position).
2. Add an `on_tick` call site after the existing gold-engine dispatch:
   ```cpp
   g_xauusd_fvg.on_tick(bid, ask, now_ms,
       gold_can_enter,                               // same gate as cohort
       g_xauusd_fvg.on_close_cb);
   ```

### 7.4 `EngineRegistry.hpp` / `EngineLastRegistry.hpp`

Register the engine name `"XauusdFvg"` so per-engine accounting / culling /
walk-forward state machinery treats it like any other shadow engine.

---

## 8. Risk overlays (passive — engine respects them, doesn't fight them)

The FVG engine inherits the standard cohort overlays without touching them:

- `OmegaNewsBlackout` — call `g_news_blackout.is_blocked("XAUUSD", now_s)`
  at IDLE→entry transitions. If blocked, drop the candidate FVG (don't
  re-queue; the next bar's mitigation check will see if the zone gets
  re-tested after the blackout lifts).
- `SpreadRegimeGate` per-engine instance, fed every tick, consulted at
  entry. Same shape as S59 (`m_spread_gate.on_tick(now_ms, spread)`,
  `m_spread_gate.can_fire()`).
- `OmegaCrowdingGuard`, `OmegaVolTargeter` — already global; FVG engine
  doesn't need to know about them. Their hooks live at the trade-lifecycle
  level.

---

## 9. Verification plan (next chat, after .hpp lands)

1. **Static check:** compile clean, no new warnings, header is
   single-translation-unit-include compatible (matches the include model in
   `engine_init.hpp:7`).
2. **Synthetic trace:** feed a known historical FVG window through the
   engine offline (a small C++ harness that replays a few days of
   `bars_XAUUSD_15min_*.pkl` re-emitted as ticks). Assert that:
   - FVGs detected match `fvg_phase0/XAUUSD_15min/fvgs.csv` row-for-row
     (formed_idx, direction, zone_low, zone_high, gap_height, score_at_formation).
   - Mitigation entries match `trades_top.csv` (entry_idx, entry_time).
   - Exits match (exit_idx, exit_reason, gross_exit).
   - PnL within ±$0.10 per trade after rounding (15-min OHLC -> tick
     replay introduces one-tick noise but should be tiny).
3. **First shadow week:** run on live ticks; compare every fired trade's
   live `score_at_entry` against an offline v3 re-run on the same bar
   data. Investigate any drift > 0.001.
4. **Month 3:** quarterly re-validation as specified in
   `HANDOFF_FVG_BACKTEST.md` §2.

The user runs scripts on the Mac and pastes back `summary.txt` blocks — the
hosted-Claude bash sandbox is broken (per handoff §"Constraints").

---

## 10. What stays untouched

- `scripts/usdjpy_xauusd_fvg_signal_test.py` — core, never modified.
- `scripts/fvg_pnl_backtest_v1.py`, `v2.py`, `v3.py` — backtest reference,
  never modified.
- S59 `UsdjpyAsianOpenEngine.hpp` — sister engine, pattern reference, never
  modified.
- Existing gold engines — their internal logic untouched. We only ADD the
  new engine to the `gold_any_open` exclusion gate (one-line edit in
  `tick_gold.hpp`).

---

## 11. Open questions to settle BEFORE the .hpp is written

1. **`LOT_MAX`** value (§3): 0.20 / 1.00 / 2.00. Pick before next chat.
2. **`STARTING_EQUITY`** for sizing: hardcode $100k in the engine to mirror
   the backtest, or pull from `g_cfg`?
3. **Bar-level vs intrabar mitigation entry** (§5.4): bar-level recommended
   for v1 to stay bit-equivalent with backtest. Confirm.
4. **`score_at_entry` logging** (§6): extend `omega::TradeRecord` (touches
   what may be considered core) vs side-channel CSV. Side-channel
   recommended; confirm.
5. **`tick_value_multiplier` for XAUUSD** (§5.6): plan assumes 100 USD per
   price-point per lot per `OmegaTradeLedger.hpp:88`. Confirm this is the
   live broker constant the engine should use for the sizing math.
6. **Intra-cohort exclusion semantics** (§7.3): FVG engine respects
   `gold_any_open` (no entry while another gold engine is live). Confirm
   FVG should ALSO add itself to the gate (so e.g. GoldStack will not enter
   while FVG holds a position). Recommended yes.

---

## 12. Source citations within this design

Every algorithm in this plan is a faithful port of:
- `scripts/fvg_pnl_backtest_v3.py` — `simulate_trade()`, `run_backtest()`
- `scripts/usdjpy_xauusd_fvg_signal_test.py` (core) — `atr()`,
  `add_indicators()`, `detect_fvgs()`, `measure_reactions()`,
  `classify_session()`
- `HANDOFF_FVG_BACKTEST.md` — frozen accepted config, cutoff design,
  shadow-promotion rules
- `include/UsdjpyAsianOpenEngine.hpp` — sister-engine class shape, member
  layout, `_close()` mutex pattern, shadow_mode pinning convention
- `include/OmegaTradeLedger.hpp` — `TradeRecord` field semantics, shadow flag

---

*End of plan. Sign off, mark up §11 open questions, and the next chat
writes the full `include/XauusdFvgEngine.hpp`.*
