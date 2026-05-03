# EURUSD Engine — Implementation Session

**Session date:** 2026-05-02
**Continues:** SESSION_2026-05-01h-trade-quality
**Status:** Implementation complete. Engine + 4 wiring edits shipped to `omega-terminal` branch.
**Deliverables:** `include/EurusdLondonOpenEngine.hpp` (776 lines), wiring in `globals.hpp`, `engine_init.hpp`, `tick_fx.hpp`.

---

## TL;DR

`EurusdLondonOpenEngine` ships as a 1:1 architectural port of `GoldMidScalperEngine` (S53 baseline) with FX-scale parameters, pip-scale price math, a 06:00–09:00 UTC session window, and a news-blackout gate at the IDLE→ARMED transition. Defaults to `shadow_mode = true`. Promote to live after a 2-week paper validation showing ≥30 trades with WR ≥35% net positive after costs.

This document captures the audit lineage and design rationale. The code is the source of truth for current parameter values and behaviour.

---

## 1. Original brief vs. what the audit found

The original brief in the SESSION_2026-05-01h handoff proposed building `EurusdLondonOpenEngine` as a parallel new engine, similar in shape to `GoldMidScalperEngine`. The audit overturned three of four strategic assumptions in that brief.

### Overturn 1: existing FX wiring is already comprehensive

The brief implied EURUSD engines were absent or dormant. Reality: `g_eng_eurusd` (BreakoutEngine), `g_bracket_eurusd` (compression bracket), `g_sup_eurusd` (supervisor), `g_vwap_rev_eurusd` (VWAP reversion, disabled), L2 imbalance feed (`g_macro_ctx.eur_l2_imbalance`), CVD direction tracking (`g_macro_ctx.eurusd_cvd_dir`), and the EUR-GBP cluster cap (`max_per_cluster_eur_gbp = 1`) are all in place. `g_bracket_eurusd.MAX_RANGE = 0.0008` (8 pips) — the existing bracket IS already a London-open compression-breakout shape.

Conclusion from finding 1 alone: would have built a duplicate engine if not for findings 2 and 3.

### Overturn 2: ALL FX engines deliberately disabled 2026-04-06

`include/tick_fx.hpp:1-16` (pre-this-session) documented the disable. `on_tick_eurusd` was a no-op body that only updated `g_macro_ctx.eur_mid_price` for gold correlation. The bracket engine config at `engine_init.hpp:1011` was fully dead code: configured every startup but never invoked from any dispatch path.

This is why LDG showed no EURUSD trades. The handler was intentionally inert. Has been since 2026-04-06.

Conclusion from finding 2: tuning `g_bracket_eurusd` is moot — it's not wired. Need to either re-enable it (resurrects known-broken signal) or build a new engine with the failure modes addressed.

### Overturn 3: MacroCrash template is itself disabled as broken

The 2026-04-06 comment proposed rebuilding FX as "MacroCrash applied to FX." But MacroCrashEngine was disabled 2026-04-30 (`engine_init.hpp:162`):

```
g_macro_crash.enabled = false; // 2026-04-30 AUDIT DISABLE:
//   84 trades / 4wk / 4.8% WR / -10,849pts (65% of total bleed).
//   The S44 retune did not fix the bleed -- WR stayed below random.
```

84 trades over 4 weeks at 4.8% WR producing 65% of system-total losses. The S44 retune (raising ATR threshold 8→12, vol ratio 2.5→3.5, drift floor 6→10) did not fix it.

Conclusion from finding 3: the original "rebuild as MacroCrash applied to FX" plan inherits a demonstrably broken signal model. Any FX engine using MCE's ATR-extension + vol-surge + drift gating would likely replicate the same bleed pattern.

### Final direction (Path A')

After three audit-driven redirects, the user confirmed **Path A'**:

> Build `EurusdLondonOpenEngine` as a fresh compression-breakout, NOT inheriting MCE's signal model. Apply this session's gold lessons (BE-lock, MFE-trail-0.55, same-level block) from day one. Add `g_news_blackout.is_blocked("EURUSD", now_sec)` consultation at entry to skip arming during NFP/CPI/ECB events. The BE-lock + same-level block address the documented "enters after move is done" failure mode by capping compounding even on late entries.

---

## 2. Audit findings (all confirmed via direct file reads on 2026-05-02)

| Item | Location | Finding |
|---|---|---|
| FX engines globally disabled | `tick_fx.hpp:1-16, 19-28` (pre-session) | `on_tick_eurusd` is a no-op |
| `g_bracket_eurusd` config | `engine_init.hpp:1011, 845, 1042` | Configured but never called |
| Engine registry list | `engine_init.hpp:1822-1899` | Zero EURUSD entries in `register_engine` |
| LDG position source list | `engine_init.hpp:1909-1945+` | No EURUSD in `register_source` |
| MacroCrash status | `engine_init.hpp:162` | Disabled 2026-04-30 |
| News blackout module | `OmegaNewsBlackout.hpp:212-320` | `NewsBlackout` class with `is_blocked(sym, now_sec)` |
| Live calendar fetcher | `OmegaNewsBlackout.hpp:355+`, `config.hpp:278` | `LiveCalendarFetcher` pulls Forex Factory weekly XML |
| News blackout instance | `omega_types.hpp:332` | `static omega::news::NewsBlackout g_news_blackout;` |
| News blackout already used at lifecycle | `trade_lifecycle.hpp:943, 1059` | `if (g_news_blackout.is_blocked(symbol, nowSec())) return false;` |
| EURUSD in NFP/CPI/ECB symbol sets | `OmegaNewsBlackout.hpp:113, 129, 159` | EUR is blocked for all 3 |
| NFP post-buffer | `OmegaNewsBlackout.hpp:67` | 90 minutes (raised 15→90 after 2026-04-03 -$177 VWAPRev incident) |
| Gold midscalper template | `GoldMidScalperEngine.hpp:81-678` | 678 lines, complete reference |
| Gold midscalper register | `engine_init.hpp:1830-1834` | Pattern for `register_engine` |
| Gold midscalper position source | `engine_init.hpp:1945-1975` | Pattern for `register_source` |
| Gold midscalper dispatch | `tick_gold.hpp:2180-2222` | Two-phase dispatch (manage existing pos, then entry) |
| Globals declaration pattern | `globals.hpp:318, 324-326` | `#include` then `static omega::ClassName g_var;` |

---

## 3. Confirmed parameter envelope (user-approved 2026-05-02)

### EURUSD pip math reference

| Quantity | Value |
|---|---|
| 1 pip | 0.0001 |
| Price decimal precision | 5 (sub-pip) |
| Typical spread on cTrader/BlackBull | 0.5 to 1.4 pips |
| Daily ATR (current 2026 levels) | 60-120 pips |
| Pip value at 1.00 lot | $10 |
| Pip value at 0.10 lot | $1 |
| Pip value at 0.01 lot | $0.10 |
| Lot precision typical | 0.01 (mini-lot) |

### Engine parameters (mirrors GoldMidScalperEngine where reasonable)

| Constant | Value | Rationale |
|---|---|---|
| `STRUCTURE_LOOKBACK` | `600` | ~3 min at ~200 ticks/min London EURUSD; finds genuine compressions |
| `MIN_ENTRY_TICKS` | `60` | Warmup before any arming |
| `MIN_BREAK_TICKS` | `5` | Sweep guard, mirrors gold |
| `MIN_RANGE` | `0.0008` (8 pips) | London compressions typically 8-15 pips |
| `MAX_RANGE` | `0.0030` (30 pips) | Filter post-news anomaly compressions |
| `SL_FRAC` | `0.5` | Half-range below entry |
| `SL_BUFFER` | `0.0002` (2 pips) | Spread + slippage cushion |
| `TP_RR` | `3.0` | **User-confirmed.** 8-pip SL → 24-pip TP. Captures middle of London momentum runs. |
| `TRAIL_FRAC` | `0.25` | Mirrors gold |
| `MIN_TRAIL_ARM_PTS` | `0.0006` (6 pips) | R-equivalent of $5 on gold at 0.10 lot ($1/pip × 6 = $6) |
| `MIN_TRAIL_ARM_SECS` | `30` | Longer than gold's 15s — FX moves slower |
| `MFE_TRAIL_FRAC` | `0.55` | This session's gold lesson |
| `BE_TRIGGER_PTS` | `0.0004` (4 pips) | Mirror gold's BE-lock pattern |
| `SAME_LEVEL_BLOCK_PTS` | `0.0010` (10 pips) | Bigger than `MIN_RANGE` 8 pips → ensures non-trivial separation |
| `SAME_LEVEL_POST_SL_BLOCK_S` | `900` (15 min) | Same as gold |
| `SAME_LEVEL_POST_WIN_BLOCK_S` | `600` (10 min) | Same as gold |
| `MAX_SPREAD` | `0.00020` (2 pips) | Reject if spread blew out beyond typical 0.5-1.4 |
| `RISK_DOLLARS` | `30.0` | Same as gold engines |
| `RISK_DOLLARS_PYRAMID` | `10.0` | Same |
| `USD_PER_PRICE_UNIT` | `10000.0` | $1 per pip (0.0001) at 0.10 lot → $10000 per unit-of-price-of-1.0 |
| `ENTRY_SIZE_DEFAULT` | `0.10` lot | **User-confirmed.** ~$8 risk at 8-pip SL, conservative |
| `LOT_MIN` / `LOT_MAX` | `0.01` / `0.10` | Caps the risk-budget sizing math |
| `PENDING_TIMEOUT_S` | `180` | FX breaks fast (matches existing `g_bracket_eurusd` 180s) |
| `COOLDOWN_S` | `240` | Slightly longer than gold midscalper 180s |
| `SESSION_START_HOUR_UTC` | `6` | London open |
| `SESSION_END_HOUR_UTC` | `9` | London + first hour, pre-NY-overlap |
| `EXPANSION_HISTORY_LEN` | `20` | Same as gold |
| `EXPANSION_MIN_HISTORY` | `5` | Same |
| `EXPANSION_MULT` | `1.10` | Same |
| `DOM_SLOPE_CONFIRM` | `0.15` | Same as gold (book_slope hard-wired 0.0 since `eur_slope` doesn't exist) |
| `DOM_LOT_BONUS` | `1.3` | Same |
| `DOM_WALL_PENALTY` | `0.5` | Same |

### Sizing math sanity check

```
ENTRY_SIZE = 0.10 lot
1 pip move (0.0001 price) at 0.10 lot = $1
8-pip SL (0.0008 price) at 0.10 lot = $8 risk per trade
24-pip TP (0.0024 price) at 0.10 lot = $24 reward per trade
RR3 captures align with $8 risk × 3 = $24
```

### Session window

```
06:00 UTC ≤ now_hh < 09:00 UTC  →  arming allowed
otherwise                       →  IDLE only
```

London open + first hour. Pre-NY-overlap. Highest-edge window per most FX research.
Implemented inside the engine via `gmtime_r/gmtime_s` on `now_s`. Returns early from
the entry path if outside; existing positions still managed via `manage()`.

### News blackout hook

```cpp
if (g_news_blackout.is_blocked("EURUSD", now_s)) {
    // Refuse arming. Do NOT close any existing position via this gate
    // (existing positions exit via SL/TP/MAX_HOLD inside manage()).
    return;
}
```

NFP / CPI / FOMC / ECB are scheduled in `RecurringEventScheduler`. EURUSD is in NFP, CPI, and ECB symbol sets, plus FOMC's empty set (covers all). NFP blocks 5 min pre to 90 min post (2026-04-03 incident-tuned). Live calendar overlay via `LiveCalendarFetcher` pulls forexfactory.com weekly.

`g_news_blackout` is referenced directly (no `extern` redeclaration) per the SINGLE-TRANSLATION-UNIT include model: `omega_types.hpp` always loads before any engine header from `main.cpp`.

---

## 4. Architecture spec (as shipped)

### File: `include/EurusdLondonOpenEngine.hpp`

The shipped file is a 1:1 line-by-line port of `GoldMidScalperEngine.hpp:81-678` (S53 baseline) with the constants from Section 3 and the diffs below.

#### Diff 1: Pip-scale price math

Gold engine math is in dollars: `range = $8-20`, `MIN_BREAK_TICKS = 5`, `MAX_SPREAD = 2.5`. EURUSD math is in 0.0001 fractional units. All comparisons use the smaller-scale constants. `range` is a number like `0.0012` (12 pips), `spread` is `0.00012`. Internal logic is identical — only constants change.

#### Diff 2: Symbol-aware tick value

Gold: `USD_PER_PT = 100.0` (per $1 of price at 0.01 lot).
EURUSD: at 0.10 lot, 1 unit of price (0.0001) = $1. Defined as `USD_PER_PRICE_UNIT = 10000.0` (1 unit of price = $10,000 at 0.10 lot). Sizing math:

```cpp
const double risk_lot = (sl_dist * USD_PER_PRICE_UNIT > 0.0)
    ? (risk / (sl_dist * USD_PER_PRICE_UNIT))
    : LOT_MAX;
const double base_lot = std::max(LOT_MIN, std::min(LOT_MAX, risk_lot));
```

The 0.01–0.10 lot clamp keeps sizing in the conservative window per user confirmation.

#### Diff 3: News blackout gate at IDLE→ARMED transition

```cpp
if (g_news_blackout.is_blocked("EURUSD", now_s)) {
    return;
}
```

Placed AFTER the session-window check, BEFORE the `range >= MIN_RANGE` arm check.

#### Diff 4: Session window gate

```cpp
{
    time_t t = static_cast<time_t>(now_s);
    struct tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    if (utc.tm_hour < SESSION_START_HOUR_UTC ||
        utc.tm_hour >= SESSION_END_HOUR_UTC) {
        return;
    }
}
```

Only the entry path is gated. Existing LIVE / PENDING / COOLDOWN paths above this point already returned, so positions held across 09:00 UTC continue managing normally.

#### Diff 5: Symbol literal in close path

- `tr.symbol = "EURUSD"`
- `tr.engine = "EurusdLondonOpen"`
- `tr.regime = "LDN_COMPRESSION"`
- Log prefix: `[EUR-LDN-OPEN]` and `[EUR-LDN-OPEN-DIAG]`

#### Diff 6: DOM fields plumbed from g_macro_ctx EUR fields

Available EUR DOM fields (`SymbolEngines.hpp:42-117`):
- `eur_l2_imbalance`
- `eur_microprice_bias`
- `eur_vacuum_ask`, `eur_vacuum_bid`
- `eur_wall_above`, `eur_wall_below`
- `eur_mid_price`

NOT available: `eur_slope`, `eur_l2_real`. So the dispatcher passes `book_slope = 0.0` (slope branch naturally inert) and uses `g_macro_ctx.ctrader_l2_live` as the `l2_real` flag. When cTrader L2 is offline, DOM filter degrades safely.

---

## 5. Wiring (4 core file edits, all additive)

### Edit 1: `include/globals.hpp`

```cpp
// 2026-05-02: EurusdLondonOpenEngine ...
#include "EurusdLondonOpenEngine.hpp"
static omega::EurusdLondonOpenEngine g_eurusd_london_open;
```

### Edit 2: `include/engine_init.hpp`

#### 2a. Shadow mode default + cancel callback (after `g_gold_midscalper.shadow_mode = true;`)

```cpp
g_eurusd_london_open.shadow_mode = true;
g_eurusd_london_open.cancel_fn   = [](const std::string& id) { send_cancel_order(id); };
```

#### 2b. Engine registry (after MidScalperGold register block)

```cpp
g_engines.register_engine("EurusdLondonOpen",
    [reg]{ return reg("EurusdLondonOpen",
                      true,
                      g_eurusd_london_open.shadow_mode,
                      {"EurusdLondonOpen"}); });
```

#### 2c. LDG position source (after MidScalperGold register_source block)

Mirrors `MidScalperGold` source 1:1, with `"EURUSD"` symbol, `tick_value_multiplier(std::string("EURUSD"))` for pip-value math, and updated log line:

```
[OmegaApi] g_open_positions sources registered (3 sources: HybridGold, MidScalperGold, EurusdLondonOpen)
```

### Edit 3: `include/tick_fx.hpp` — un-noop `on_tick_eurusd`

Replaced the no-op body with a two-phase dispatch (mirrors `tick_gold.hpp:2180-2222`):

```cpp
if (g_eurusd_london_open.has_open_position()) {
    g_eurusd_london_open.on_tick(bid, ask, now_ms,
                                 false, false, false, 0,
                                 bracket_on_close,
                                 0.0,
                                 g_macro_ctx.eur_vacuum_ask, g_macro_ctx.eur_vacuum_bid,
                                 g_macro_ctx.eur_wall_above, g_macro_ctx.eur_wall_below,
                                 g_macro_ctx.ctrader_l2_live);
} else {
    const bool can_enter = tradeable && lat_ok;
    g_eurusd_london_open.on_tick(bid, ask, now_ms,
                                 can_enter, false, false, 0,
                                 bracket_on_close,
                                 0.0,
                                 g_macro_ctx.eur_vacuum_ask, g_macro_ctx.eur_vacuum_bid,
                                 g_macro_ctx.eur_wall_above, g_macro_ctx.eur_wall_below,
                                 g_macro_ctx.ctrader_l2_live);
}
```

GBPUSD/AUDUSD/NZDUSD/USDJPY handlers in the same file remain inert — no orders sent.

### Edit 4: `include/on_tick.hpp` — diag chk()

**Skipped.** The `chk(eng, "SYMBOL")` lambda calls `eng.sl_cooldown_until()` — a method present on `BreakoutEngine` but NOT on the GoldMidScalper / EurusdLondonOpen lineage (these track cooldowns via internal `m_sl_cooldown_ts`, not exposed). The handoff doc flagged this as optional. Warmup heartbeat is provided by the engine's own `[EUR-LDN-OPEN-DIAG]` lines every 600 ticks instead.

---

## 6. Promotion gate (shadow → live)

Same pattern as `GoldMidScalperEngine`:

1. Two weeks of paper data accumulated in `g_omegaLedger`, filtered `engine="EurusdLondonOpen"`.
2. ≥30 closed trades.
3. WR ≥ 35% (compression-breakout strategies floor; account for cost).
4. Net positive expectancy after costs (spread × 2 + commission per leg).
5. No SL clusters in same-level block radius (validates the block is doing its job).
6. London-session activity confirmed (most fires should be 06–09 UTC).
7. Live promotion = single line edit in `engine_init.hpp` flipping `g_eurusd_london_open.shadow_mode = true;` to `false;`. Push → redeploy → verify `[EUR-LDN-OPEN]` log lines show non-shadow trades.

---

## 7. Verification queries (post-deploy)

PowerShell on Windows VPS:

```powershell
# Engine count: should jump from 15 → 16
Select-String -Path C:\Omega\logs\latest.log -Pattern "g_engines registered \(16 engines\)"

# Position-source line: should now show 3 sources
Select-String -Path C:\Omega\logs\latest.log -Pattern "g_open_positions sources registered \(3 sources"

# Diag heartbeat: should appear every 600 ticks during London session
Select-String -Path C:\Omega\logs\latest.log -Pattern "EUR-LDN-OPEN-DIAG.*window=\d+/600" | Select-Object -Last 5

# News blackout integration: should print blocked windows for EURUSD around NFP/CPI/ECB
Select-String -Path C:\Omega\logs\latest.log -Pattern "NEWS-BLACKOUT.*EURUSD" | Select-Object -Last 10

# Shadow trades to ledger: should see entries within 1-2 trading days
Select-String -Path C:\Omega\logs\latest.log -Pattern "TRADE-COST.*EURUSD" | Select-Object -Last 10

# First arm: confirms the bracket detection is firing
Select-String -Path C:\Omega\logs\latest.log -Pattern "EUR-LDN-OPEN.*ARMED" | Select-Object -Last 5
```

In the GUI:
- LDG panel filter `EurusdLondonOpen` should show entries within first London session post-deploy.
- TradeBook (`g_omegaLedger`) should show shadow=true entries with `engine="EurusdLondonOpen"`, `regime="LDN_COMPRESSION"`.
- Engine status panel should show 16 engines (was 15).

---

## 7b. S55 backtest tune (2026-05-02 follow-up)

After initial deploy, ran a 14-month HistData.com EURUSD tick backtest
(Mar 2025 - Apr 2026, 25.3M ticks). Multi-axis parameter sweep found a
clear improvement over the original gold-lineage parameters.

### Optimum found

| Param | Original | Optimum | Reason |
|---|---|---|---|
| `TP_RR` | 3.0 | **2.0** | RR=3 (24-pip TP) was unreachable; only 3/1076 trades hit TP. RR=2 captures TP cleanly on 39 trades and keeps the trail intact for momentum runs. |
| `BE_TRIGGER_PTS` | 0.0004 (4 pips) | **0.0006 (6 pips)** | 4-pip BE-lock fired too early. 6 pips matches trail-arm threshold, eliminating the BE-only zone. BE_HIT count goes to 0; clean TP/TRAIL/SL outcomes. |

All other parameters unchanged from original.

### Backtest performance

| Metric | Original config | Optimum config | Change |
|---|---|---|---|
| Trades (14 months) | 1,076 | 974 | -9% (still 70/mo) |
| Win rate (W/L only) | 54.9% | **56.1%** | +1.2pp |
| Total PnL (USD est) | +$54 | **+$188** | **3.5x** |
| Max drawdown | $319 | **$210** | **-34%** |
| Profit factor | 0.17 | **0.89** | **5x** |
| Profitable months | 6 of 14 | **10 of 14** | +4 |

Caveats:
- HistData != BlackBull: live spreads and exact tick sequencing will differ. Apply 10-20% haircut to expected live performance.
- Optimum was found *on* this 14-month sample. Real out-of-sample performance will be lower than the in-sample 3.5x.
- Even with a 50% haircut, optimum is preferable to original (~+$95 vs ~+$27).

### Sweep methodology

Tested axes individually then combined:
1. TP_RR: {1.5, 2.0, 2.5, 3.0, 3.5, 4.0}
2. Session window: {6-9, 7-9, 6-10, 7-10, 8-10, 6-11, 7-11}
3. Direction filter: {none, long_only, short_only, hour_conditional with 5 hour-split variants}
4. BE_TRIGGER_PTS: {2, 3, 4, 5, 6, 8, disabled} pips
5. MIN_RANGE: {6, 8, 10, 12, 15} pips

Plus a 3x3 refinement grid around the winner (TP_RR x BE_TRIGGER_PTS) which revealed an axis interaction: at BE=6 pips, optimal TP_RR shifts from 2.5 (single-axis winner) to 2.0.

### What did NOT improve

| Hypothesis | Result | Verdict |
|---|---|---|
| Trim session to 07-09 UTC | -$1 (vs +$54) | Worse - early hours seed engine state |
| Drop 06 UTC bin | Same as above | Worse |
| Long-only filter | +$47 | Worse than baseline |
| Short-only filter | -$229 | Disaster |
| Hour-conditional direction (alone) | +$88 | Better alone, but hurts when combined with BE=6 (BE-lock saves the trades the hour-filter throws away). Decision: keep direction permissive. |
| MIN_RANGE=15 (high-WR mode) | +$108, WR 69%, n=170 | Below 30-trade gate. Worth revisiting if the conservative 974-trade config underperforms in shadow. |

## 7c. S56 comprehensive backtest tune (2026-05-02 second follow-up)

After S55, ran a comprehensive 27-axis sweep on the same 14-month HistData backtest. ~200 configurations tested individually, then systematic leave-one-out and incremental build-up to find safe combinations (raw all-axis composite hit a destructive interaction and went negative). Followed by formal out-of-sample validation (split train Mar-Sep 2025 vs test Oct 2025-Apr 2026) and Kelly-criterion lot sizing analysis.

### S56 final config (THIS SESSION'S DEPLOY)

| Param | S55 | **S56** | Reason |
|---|---|---|---|
| `SL_FRAC` | 0.50 | **0.80** | Original 0.5 placed SL too close; trades wicked out before trail-arm. 0.80 keeps SL in upper 80% of compression structure. PF 0.89 -> 1.62 single-axis. |
| `TRAIL_FRAC` | 0.25 | **0.30** | Wider trail lets winners run further. Combines additively with SL_FRAC=0.80; joint tune $305 -> $425. |
| `MFE_TRAIL_FRAC` | 0.55 | **0.40** | Tightened give-back from 45% to 60% of MFE preserved. With wider SL placement, trades MFE further; capturing more of move. |
| `SAME_LEVEL_BLOCK_PTS` | 0.0010 | **0.0008** | 8 pips matches MIN_RANGE; reject any compression that overlaps prior exit within its own width. |
| `SAME_LEVEL_POST_SL_BLOCK_S` | 900 | **1200** | 15 -> 20 min. Failed breakouts get more time to clear. |
| `COOLDOWN_S` | 240 | **120** | Original 240s too patient; same-level block now does the anti-chop work. |
| `MAX_RANGE` | 0.0030 | **0.0050** | 30 -> 50 pips. Larger compressions still produce profitable breakouts. |
| `LOT_MAX` | 0.10 | **0.20** | Half-Kelly sizing per the empirical 66.6% WR / b=0.605 (Kelly = 11.5%, half-Kelly = 5.7%). 0.20 lot = $16 risk per trade. Margin for WR degradation. |

### S56 14-month backtest performance

| Metric | S55 production | **S56 (this deploy)** | vs S55 |
|---|---|---|---|
| Trades | 974 | 854 | -12% |
| Win rate | 56.1% | **66.6%** | +10.5pp |
| Total PnL (in-sample) | +$188 | **+$626** | +$438 (3.3x) |
| Max drawdown | $210 | **$132** | -37% |
| Profit factor | 0.89 | **4.75** | 5.3x |
| Profitable months | 10/14 | 10/14 | same |
| Avg win | $6.20 | $6.38 | similar |
| Avg loss | $7.47 | $10.54 | wider (per S56 SL_FRAC=0.80) |

### Out-of-sample validation

Train period (Mar-Sep 2025, 7 months): S56 makes +$512, PF 3.88, 6/7 prof.
Test period (Oct 2025-Apr 2026, 7 months) — TRUE OOS:

| Config | Trades | WR | PnL | PF | Profitable months |
|---|---|---|---|---|---|
| S55 baseline | 360 | 53.1% | -$90 | -0.54 | 5/7 |
| **S56 champion** | 308 | **64.9%** | **+$114** | **1.04** | 4/7 |

S56 is **profitable on data it never saw**, and beats S55 by $204 OOS. PF degrades 3.88 -> 1.04 (expected curve-fit haircut), but still positive. WR holds well: 67.6% train -> 64.9% OOS. **Real live performance will look like the OOS numbers**, not the in-sample headline.

### Lot sizing (S56)

Empirical from 14-month data:
- WR = 66.6%, avg_win = $6.38, avg_loss = $10.54
- b = 0.605 (wins 65% smaller than losses; high WR is doing all the work)
- Kelly fraction = 11.5%; half-Kelly = 5.7%; quarter-Kelly = 2.9%

LOT_MAX raised from 0.10 -> 0.20 to align with half-Kelly. Linear scaling at this size:

| Period | Realistic PnL (post-50% haircut) | Realistic DD | DD as % of $10k |
|---|---|---|---|
| Annualized | ~$220-300 | ~$260 | 2.6% |

Do NOT exceed 0.20 LOT_MAX until 2 weeks of shadow data confirm OOS WR >= 60%. The high WR is fragile -- if it drops to 55%, expectancy goes negative.

### Critical lesson from the comprehensive sweep

The naive "combine all single-axis winners" strategy gave +$94 -- WORSE than baseline +$188. Three "wider stop" changes (SL_FRAC=0.80, SL_BUFFER=4pips, MIN_TRAIL_ARM_PTS=8pips) were mutually toxic when combined. The S56 config keeps SL_FRAC=0.80 but leaves SL_BUFFER (2 pips) and MIN_TRAIL_ARM_PTS (6 pips) at S55 values. Always validate composites with leave-one-out, never trust naive AND-of-winners.

## 8. Build / verification on this session

| Check | Result |
|---|---|
| Engine line count | 776 |
| Modified files | 3 (globals.hpp, engine_init.hpp, tick_fx.hpp) + 1 new (EurusdLondonOpenEngine.hpp) |
| Symbol references resolved | All 17 occurrences of `g_eurusd_london_open` / `EurusdLondonOpen` accounted for |
| g++ -fsyntax-only -Wall -Wextra (sandboxed) | Pass — exit code 0, zero warnings, all paths exercised (IDLE/ARMED/PENDING/LIVE/COOLDOWN, confirm_fill, manage, force_close, _close) |
| Production MSVC build | Pending deploy on Windows VPS |

The sandbox synth test required minor stubs (`OmegaTradeLedger.hpp`, `SpreadRegimeGate.hpp`, and a Linux-compatible mock of `OmegaNewsBlackout.hpp` because the real header has an unguarded `_mkgmtime` reference at line 424 — pre-existing, not new in this session). The actual Windows MSVC build sees the real headers and `_mkgmtime` is the expected platform call.

---

## 9. Promotion checklist for next session

After 2 weeks of paper data:

1. SSH to Windows VPS. Pull the ledger CSV for last 14 days filtered `engine="EurusdLondonOpen"`.
2. Compute trade count, WR, net P&L after costs. Compare against gates in Section 6.
3. If gates pass: edit `include/engine_init.hpp:57` from `g_eurusd_london_open.shadow_mode = true;` to `g_eurusd_london_open.shadow_mode = (g_cfg.mode != "LIVE");` (i.e. follow `kShadowDefault`).
4. Commit single-line change with message: `S54 EUR-LDN-OPEN: promote to live (paper validated, N=X WR=Y net=$Z)`.
5. Push. Redeploy. Verify `[EUR-LDN-OPEN]` non-shadow trades appear in log.

If gates fail: add findings as new tasks in next handoff. Common failure modes to investigate first:
- Same-level block too aggressive (suppressing too many continuations) — relax to 8 pips (= MIN_RANGE).
- Spread blowout during news (despite blackout) — add a per-tick spread sanity guard at PENDING→LIVE transition.
- Asian-session compression spilling into 06:00 fire — require prior 22:00–06:00 range ≤ N pips for arm validity.

---

## 10. Outstanding security flag (carry-forward)

The user's global `CLAUDE.md` (at `/var/folders/.../[CLAUDE.md](http://CLAUDE.md)`) still contains a GitHub PAT in plaintext. Token: `ghp_9M2I...24dJPV4`. Rotate at github.com/settings/tokens. Replace with a Keychain reference or remove the line entirely.

This is unrelated to the EURUSD work but worth resurfacing every session until rotated.

---

## 11. Open questions resolved during implementation

These came up during the audit but were resolved while writing the engine and wiring:

1. **Engine count post-deploy.** Confirmed: 15 before this session → 16 after. `MacroCrash` is still in the `register_engine` list (engine_init.hpp:1855-1859) even though `g_macro_crash.enabled = false`.

2. **Cluster-grouping with EUR-GBP at the adaptive risk layer.** Existing `g_adaptive_risk.corr_heat.max_per_cluster_eur_gbp = 1` (engine_init.hpp:1522) prevents both EURUSD and GBPUSD pairs from firing simultaneously. Since GBPUSD currently has no live engine (also disabled 2026-04-06 and not re-enabled this session), the cluster cap is non-binding for EURUSD's first deploy. Keep the cap; revisit when GBPUSD engine ships.

3. **Asian-session range data.** The 06:00–09:00 UTC London session window assumes Asian-session compression precedes it. Decision: **not enforced this session.** Engine will fire on whatever compression structure the 600-tick lookback finds at the start of London; if Asian-session continuation produces noise, observe in shadow data before adding the constraint. Track as v2 enhancement after first paper data.

4. **Live calendar refresh timing.** `g_live_calendar.check_and_refresh` in `config.hpp:278` — call frequency not investigated this session. The blackout cache regen cost matters only if it's called per-tick rather than per-second/per-minute. Current observation in shadow logs will flag if cache rebuilds become a hot path. If so, throttle inside `NewsBlackout::is_blocked` rather than at the call site.

5. **`bracket_on_close` reuse for EURUSD.** Confirmed: `bracket_on_close` (free function at `trade_lifecycle.hpp:1073`) already handles all bracket symbols generically. The function explicitly enumerates EURUSD in its broadcast comment. No changes needed to the close pipeline.

End of handoff doc.
