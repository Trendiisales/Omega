// tick_fx.hpp -- FX tick handlers
//
// 2026-04-06 GLOBAL FX DISABLE (lineage):
//   Zero winning trades across all FX pairs. Root causes analysed at the time:
//     BreakoutEngine: enters after move is done (0.12% = 25-40% of daily range)
//     FxCascadeEngine: cascades from an edgeless EUR signal, 8pip TP barely covers spread
//     CarryUnwindEngine: VIX.F too sparse on cTrader for timing to work
//     VWAPReversionEngine: 2hr EWM VWAP chases price, enters INTO trends
//     FxFixWindowEngine: DST timing wrong in summer, CVD needs volume data we don't have
//
//   FX symbols remained SUBSCRIBED for macro context:
//     EUR/GBP/JPY/AUD/NZD mid prices feed into g_macro_ctx for gold correlation
//     No orders are sent from any FX handler beyond what is wired below.
//
// 2026-05-02 EURUSD RE-ENABLE:
//   on_tick_eurusd now dispatches to EurusdLondonOpenEngine. Other FX pairs
//   (GBPUSD/AUDUSD/NZDUSD) remain inert -- no orders sent. The new
//   engine uses a different signal model than the four April-disabled
//   engines:
//     - 06:00-09:00 UTC session window (concentrates on highest-edge hour)
//     - Compression-breakout (NOT VWAP reversion, NOT cascade signal)
//     - RR=2 TP target with 2-pip SL_BUFFER (covers ~3x typical round-trip cost)
//     - BE-lock at 6 pips MFE (caps compounding even on late entries; S55)
//     - Same-level re-arm block (20min post-SL / 10min post-win; S56)
//     - News-blackout-gated (NFP/CPI/FOMC/ECB) at IDLE->ARMED transition
//     - shadow_mode = true by default (live promotion only after paper validation)
//
// 2026-05-02 USDJPY RE-ENABLE:
//   on_tick_usdjpy now dispatches to UsdjpyAsianOpenEngine. Same architectural
//   pattern as EURUSD with JPY pip math (1 pip = 0.01 price, USD_PER_PRICE_UNIT
//   = 100 at 0.10 lot) and a 00:00-04:00 UTC Asian-session window (Tokyo open
//   + first 4 hours, pre-Frankfurt-handoff). News blackout via "USDJPY" symbol
//   (in both USD and JPY country sets per OmegaNewsBlackout.hpp).
//   shadow_mode = true by default; promote to live after a 2-week paper run
//   shows >=30 trades / WR >= 60% / net positive after costs.

#include "EurusdLondonOpenEngine.hpp"
#include "UsdjpyAsianOpenEngine.hpp"

// ── EURUSD ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_eurusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // Update macro context price -- needed by gold correlation logic.
    // Done unconditionally before any engine logic.
    g_macro_ctx.eur_mid_price = (bid + ask) * 0.5;

    const int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;

    // Two-phase dispatch (mirrors GoldMidScalper pattern in tick_gold.hpp:2180-2222):
    //   1) If position open, manage unconditionally (regardless of tradeable/lat_ok).
    //      This guarantees SL/TP/trail/BE management never stops mid-trade.
    //   2) If no position, run entry path gated by tradeable && lat_ok.
    //      The engine internally short-circuits on session window, news
    //      blackout, spread, ATR gate, and same-level block.
    //
    // DOM fields plumbed from g_macro_ctx EUR-side fields. Note:
    //   - There is no eur_slope field, so book_slope is hard-coded 0.0.
    //     The engine's slope branch is naturally inert at slope=0.
    //   - l2_real is wired to g_macro_ctx.ctrader_l2_live -- when cTrader
    //     depth is offline (FIX-only feed), DOM filter is bypassed
    //     (engine's safe l2_real==false fallback).
    if (g_eurusd_london_open.has_open_position()) {
        g_eurusd_london_open.on_tick(
            bid, ask, now_ms,
            /* can_enter        */ false,  // already in position; entry-path skipped via LIVE branch
            /* flow_live        */ false,
            /* flow_be_locked   */ false,
            /* flow_trail_stage */ 0,
            bracket_on_close,
            /* book_slope */ 0.0,
            /* vacuum_ask */ g_macro_ctx.eur_vacuum_ask,
            /* vacuum_bid */ g_macro_ctx.eur_vacuum_bid,
            /* wall_above */ g_macro_ctx.eur_wall_above,
            /* wall_below */ g_macro_ctx.eur_wall_below,
            /* l2_real    */ g_macro_ctx.ctrader_l2_live);
    } else {
        const bool can_enter = tradeable && lat_ok;
        g_eurusd_london_open.on_tick(
            bid, ask, now_ms,
            can_enter,
            /* flow_live        */ false,
            /* flow_be_locked   */ false,
            /* flow_trail_stage */ 0,
            bracket_on_close,
            /* book_slope */ 0.0,
            /* vacuum_ask */ g_macro_ctx.eur_vacuum_ask,
            /* vacuum_bid */ g_macro_ctx.eur_vacuum_bid,
            /* wall_above */ g_macro_ctx.eur_wall_above,
            /* wall_below */ g_macro_ctx.eur_wall_below,
            /* l2_real    */ g_macro_ctx.ctrader_l2_live);
    }

    (void)sym; (void)regime; (void)dispatch;
}

// ── GBPUSD ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_gbpusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    g_macro_ctx.gbp_mid_price = (bid + ask) * 0.5;
    (void)sym; (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
}

// ── USDJPY ──────────────────────────────────────────────────
// 2026-05-02: split out from on_tick_audusd to dispatch UsdjpyAsianOpenEngine.
//   The g_usdjpy_mid store (carry-trade / sizing-conversion macro) was
//   previously in on_tick_audusd; moved here so it still fires on every
//   USDJPY tick now that the dispatcher routes USDJPY separately.
template<typename Dispatch>
static void on_tick_usdjpy(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // Store USDJPY mid for tick_value_multiplier (live JPY/USD conversion).
    //   Done unconditionally before any engine logic, same pattern as the
    //   eur_mid_price store in on_tick_eurusd.
    g_usdjpy_mid.store((bid + ask) * 0.5, std::memory_order_relaxed);

    const int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;

    // Two-phase dispatch (mirrors on_tick_eurusd):
    //   1) If position open, manage unconditionally (regardless of tradeable/lat_ok).
    //      This guarantees SL/TP/trail/BE management never stops mid-trade.
    //   2) If no position, run entry path gated by tradeable && lat_ok.
    //      The engine internally short-circuits on session window, news
    //      blackout, spread, ATR gate, and same-level block.
    //
    // DOM fields plumbed from g_macro_ctx JPY-side fields:
    //   - jpy_l2_imbalance, jpy_microprice_bias, jpy_vacuum_ask, jpy_vacuum_bid
    //   - NO jpy_wall_above / jpy_wall_below / jpy_slope -- pass false / 0.0;
    //     the engine's wall and slope branches naturally inert at those values.
    //   - l2_real wired to g_macro_ctx.ctrader_l2_live -- safe-fallback when
    //     cTrader depth is offline.
    if (g_usdjpy_asian_open.has_open_position()) {
        g_usdjpy_asian_open.on_tick(
            bid, ask, now_ms,
            /* can_enter        */ false,
            /* flow_live        */ false,
            /* flow_be_locked   */ false,
            /* flow_trail_stage */ 0,
            bracket_on_close,
            /* book_slope */ 0.0,
            /* vacuum_ask */ g_macro_ctx.jpy_vacuum_ask,
            /* vacuum_bid */ g_macro_ctx.jpy_vacuum_bid,
            /* wall_above */ false,
            /* wall_below */ false,
            /* l2_real    */ g_macro_ctx.ctrader_l2_live);
    } else {
        const bool can_enter = tradeable && lat_ok;
        g_usdjpy_asian_open.on_tick(
            bid, ask, now_ms,
            can_enter,
            /* flow_live        */ false,
            /* flow_be_locked   */ false,
            /* flow_trail_stage */ 0,
            bracket_on_close,
            /* book_slope */ 0.0,
            /* vacuum_ask */ g_macro_ctx.jpy_vacuum_ask,
            /* vacuum_bid */ g_macro_ctx.jpy_vacuum_bid,
            /* wall_above */ false,
            /* wall_below */ false,
            /* l2_real    */ g_macro_ctx.ctrader_l2_live);
    }

    (void)sym; (void)regime; (void)dispatch;
}

// ── AUDUSD/NZDUSD ──────────────────────────────────────────
template<typename Dispatch>
static void on_tick_audusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // 2026-05-02: USDJPY split out to on_tick_usdjpy. This handler now serves
    //   AUDUSD/NZDUSD only (both still inert; no orders sent).
    (void)sym; (void)bid; (void)ask; (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
}
