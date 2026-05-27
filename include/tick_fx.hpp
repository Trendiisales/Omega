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
//
// 2026-05-04 GBPUSD RE-ENABLE (audit-fixes-36 + S57):
//   on_tick_gbpusd now dispatches to GbpusdLondonOpenEngine. 1:1 architectural
//   port of EurusdLondonOpen with GBP volatility scale (MIN/MAX_RANGE 12-75
//   pips vs EUR's 8-50, ~50% wider tracking GBP's wider daily ATR). Live
//   target window 07:00-10:00 UTC (one hour later than EUR's 06-09; cable
//   compressions cluster around the LSE 08:00 UTC equity open). News
//   blackout via "GBPUSD" symbol auto-includes BoE/UK CPI/UK GDP via the
//   GBP currency set plus NFP/CPI/FOMC via the USD set.
//   shadow_mode = true by default. Session window RESTORED to 07-10 UTC
//   (2026-05-04, post-S57): the audit-fixes-36 0-24 visibility-only
//   widening was reverted to the live-target window after live tape on the
//   gold cohort showed shadow widening pulled comparable engines into the
//   wrong session and produced ✓BE → SL artefacts. The live edge is now
//   active in shadow.
//   Promotion gate: 2-week paper run, >=30 trades, WR >=35% net positive
//   after costs (matches EURUSD S56 promotion gate).
//
// 2026-05-04 AUDUSD RE-ENABLE (audit-fixes-36 + S57):
//   on_tick_audusd now dispatches AUDUSD ticks to AudusdSydneyOpenEngine.
//   1:1 architectural port of UsdjpyAsianOpen with AUD pip math rescaled
//   from JPY 0.01-pip units to AUD 0.0001-pip units (AUD is a USD-quote
//   major, identical pip scale to EUR/GBP). Live target window 22:00-02:00
//   UTC (Sydney open + Tokyo handoff, pre-Frankfurt cutoff). News
//   blackout via "AUDUSD" symbol auto-includes RBA/AU CPI/AU jobs via the
//   AUD currency set plus NFP/CPI/FOMC via the USD set.
//   shadow_mode = true by default. Session window RESTORED to 22-02 UTC
//   (2026-05-04, post-S57) with wraparound-aware in-window check now active
//   in AudusdSydneyOpenEngine -- the audit-fixes-36 0-24 visibility-only
//   widening was reverted. Promotion gate: 2-week paper run, >=30 trades,
//   WR >= 60% net positive after costs (matches USDJPY S56 gate).
//
// 2026-05-04 NZDUSD RE-ENABLE (audit-fixes-36 + S57):
//   on_tick_audusd's NZDUSD branch now dispatches to NzdusdAsianOpenEngine.
//   1:1 architectural port of AudusdSydneyOpen with NZD-side DOM plumbing
//   (vacuum-only, no walls -- mirrors AUD/JPY DOM shape). Live target
//   window 22:00-04:00 UTC (Wellington open + Tokyo handoff, one hour wider
//   than AUD's 22-02 to capture post-Tokyo-open AUDNZD-cross flow
//   settlement). News blackout via "NZDUSD" symbol auto-includes RBNZ/
//   NZ CPI/NZ jobs via the NZD currency set plus NFP/CPI/FOMC via the
//   USD set.
//   shadow_mode = true by default. Session window RESTORED to 22-04 UTC
//   (2026-05-04, post-S57) with wraparound-aware in-window check now active
//   in NzdusdAsianOpenEngine -- the audit-fixes-36 0-24 visibility-only
//   widening was reverted. Promotion gate: matches AUD/JPY S56 gate
//   (>=30 trades / WR >=60% / net positive after costs).
//   This retires the LAST [FX-NO-ENGINE] diag stub from any FX handler --
//   the full FX cohort (EUR/GBP/USDJPY/AUD/NZD) is now wired end-to-end.

#include "EurusdLondonOpenEngine.hpp"
#include "UsdjpyAsianOpenEngine.hpp"
#include "GbpusdLondonOpenEngine.hpp"
#include "AudusdSydneyOpenEngine.hpp"
#include "NzdusdAsianOpenEngine.hpp"
// 2026-05-23: FX Turtle H4 cohort (post-S99 rebuild). Long-only Donchian
//   replacement for the session-open compression engines that S99 killed
//   for negative expectancy. Dispatched BEFORE the S99 kill-switch return.
#include "FxTurtleH4Engine.hpp"

// ── EURUSD ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_eurusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // 2026-05-05 (audit-fixes-40): per-engine heartbeat pulse. Confirms the
    //   dispatcher reaches this handler. Detects "tick path severed before
    //   engine.on_tick" failures (root cause of 19h FX silence on 2026-05-04).
    g_engine_heartbeat.pulse("EurusdLondonOpen");
    // 2026-05-26 (Stage 3): EURUSD-side engine coverage.
    g_engine_heartbeat.pulse("EurusdTurtleH4");
    g_engine_heartbeat.pulse("AmrEurusd");
    g_engine_heartbeat.pulse("EurGbpPairs");
    g_engine_heartbeat.pulse("VwapRevEurusd");

    // Update macro context price -- needed by gold correlation logic.
    // Done unconditionally before any engine logic.
    g_macro_ctx.eur_mid_price = (bid + ask) * 0.5;

    // 2026-05-23: FxTurtleH4 dispatch (post-S99 rebuild). Runs BEFORE the
    //   S99 kill-switch so the new long-only Donchian H4 engine gets ticks
    //   while the disabled session-open bracket cohort stays dead. Engine
    //   is internally gated by `enabled` + `shadow_mode`.
    {
        const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
        auto on_close_cb = [](const omega::TradeRecord& tr){ (void)tr; };
        g_eurusd_turtle_h4.on_tick(bid, ask, now_ms_fx, on_close_cb);
        g_eurusd_turtle_h4.check_weekend_close(bid, ask, now_ms_fx, on_close_cb);
    }

    // S38d 2026-05-26: FxScalpPyramid_EURUSD dispatch (shadow-mode).
    //   M5 Donchian + EMA + ADX scalper. Engine internally gated by enabled.
    //   Runs BEFORE the S99 kill-switch return for the same reason as Turtle.
    //   L2 fields neutral: no FX L2 surface plumbed.
    {
        const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
        const bool fx_can_enter = tradeable && lat_ok;
        g_fx_scalp_eurusd.on_tick(bid, ask, now_ms_fx,
                                  fx_can_enter,
                                  /*l2_imbalance=*/0.5, /*book_slope=*/0.0,
                                  /*vacuum_ask=*/false, /*vacuum_bid=*/false,
                                  /*wall_above=*/false, /*wall_below=*/false,
                                  /*l2_real=*/false,
                                  nullptr);
    }

    // S99: FX engine kill-switch -- all FX LondonOpen/AsianOpen/SydneyOpen
    //   engines disabled after full BreakoutEngine + BracketEngine sweep
    //   showed negative expectancy across all 5 FX pairs. Engine dispatch
    //   cut here at the tick handler level. Macro context price store above
    //   is preserved (used by gold correlation logic). To re-enable: remove
    //   this return and rebuild.
    (void)sym; (void)regime; (void)dispatch; (void)tradeable; (void)lat_ok;
    return;

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

    // AtrMeanRevGrid (shadow) -- forex mean-reversion grid (2026-05-25).
    // Engine internally aggregates H1 bars from tick mids; we only need to
    // forward ticks. Backtest sweep: PF 2.11 @ X=10 (13 trades, 2yr).
    if (g_amr_eurusd.enabled) g_amr_eurusd.on_tick(bid, ask, now_ms);

    // S37g 2026-05-26 FxEnsembleEngine -- donchian_55 H1 LONG cell.
    if (g_fx_ens_eurusd.enabled)
        g_fx_ens_eurusd.on_tick(bid, ask, now_ms, write_shadow_csv);

    // EurGbpPairsEngine (shadow) -- spread mean-reversion. Engine maintains its
    // own H1 OHLC accumulator + rolling z-score; signal logic runs on H1 close.
    // See backtest/sweep_pairs_v2.csv for tuning. Routed in both on_tick_eurusd
    // and on_tick_gbpusd via separate per-leg methods.
    {
        auto ca_on_close = [](const omega::TradeRecord& tr){ (void)tr; };
        const auto psig = g_eur_gbp_pairs.on_tick_eur(bid, ask, now_ms, ca_on_close);
        if (psig.valid) {
            g_telemetry.UpdateLastSignal("EURUSD+GBPUSD",
                psig.long_spread ? "LONG_SPREAD" : "SHORT_SPREAD",
                psig.eur_entry, psig.reason,
                "EUR_GBP_PAIRS", regime.c_str(), "EUR_GBP_PAIRS",
                0.0, 0.0);
        }
    }

    (void)sym; (void)regime; (void)dispatch;
}

// ── GBPUSD ──────────────────────────────────────────────────
// 2026-05-04 (audit-fixes-36 + S57): on_tick_gbpusd now dispatches to
//   GbpusdLondonOpenEngine using the same two-phase pattern as
//   on_tick_eurusd (manage-when-open vs entry-when-flat). The periodic
//   [FX-NO-ENGINE] diag stub has been removed -- the engine's own
//   [GBP-LDN-OPEN-DIAG] warmup logger now provides the operator-visible
//   tick-flow signal (every 600 ticks + first 60).
template<typename Dispatch>
static void on_tick_gbpusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // 2026-05-05 (audit-fixes-40): heartbeat pulse (see on_tick_eurusd).
    g_engine_heartbeat.pulse("GbpusdLondonOpen");
    // 2026-05-26 (Stage 3): GBPUSD-side engine coverage.
    g_engine_heartbeat.pulse("GbpusdTurtleH4");
    g_engine_heartbeat.pulse("AmrGbpusd");

    // Update macro context price -- needed by GBP correlation logic and
    //   the bracket trend bias accessor at on_tick.hpp:1177.
    //   Done unconditionally before any engine logic, same pattern as the
    //   eur_mid_price store in on_tick_eurusd.
    g_macro_ctx.gbp_mid_price = (bid + ask) * 0.5;

    // 2026-05-23: FxTurtleH4 GBP dispatch (post-S99 rebuild).
    {
        const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
        auto on_close_cb = [](const omega::TradeRecord& tr){ (void)tr; };
        g_gbpusd_turtle_h4.on_tick(bid, ask, now_ms_fx, on_close_cb);
        g_gbpusd_turtle_h4.check_weekend_close(bid, ask, now_ms_fx, on_close_cb);
    }

    // S38d 2026-05-26: FxScalpPyramid_GBPUSD dispatch (shadow-mode).
    {
        const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
        const bool fx_can_enter = tradeable && lat_ok;
        g_fx_scalp_gbpusd.on_tick(bid, ask, now_ms_fx,
                                  fx_can_enter,
                                  /*l2_imbalance=*/0.5, /*book_slope=*/0.0,
                                  /*vacuum_ask=*/false, /*vacuum_bid=*/false,
                                  /*wall_above=*/false, /*wall_below=*/false,
                                  /*l2_real=*/false,
                                  nullptr);
    }

    // S99: FX kill-switch (see on_tick_eurusd comment). GbpusdLondonOpen disabled.
    (void)sym; (void)regime; (void)dispatch; (void)tradeable; (void)lat_ok;
    return;

    const int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;

    // Two-phase dispatch (mirrors on_tick_eurusd):
    //   1) If position open, manage unconditionally (regardless of tradeable/lat_ok).
    //      This guarantees SL/TP/trail/BE management never stops mid-trade.
    //   2) If no position, run entry path gated by tradeable && lat_ok.
    //      The engine internally short-circuits on session window, news
    //      blackout, spread, ATR gate, and same-level block.
    //
    // DOM fields plumbed from g_macro_ctx GBP-side fields:
    //   - gbp_vacuum_ask, gbp_vacuum_bid, gbp_wall_above, gbp_wall_below
    //     (mirrors EUR -- gbp_* fields populated alongside eur_* per the
    //     Priority 6 backlog at SymbolEngines.hpp:107-110).
    //   - There is no gbp_slope field, so book_slope is hard-coded 0.0.
    //     The engine's slope branch is naturally inert at slope=0.
    //   - l2_real is wired to g_macro_ctx.ctrader_l2_live -- when cTrader
    //     depth is offline (FIX-only feed), DOM filter is bypassed
    //     (engine's safe l2_real==false fallback).
    if (g_gbpusd_london_open.has_open_position()) {
        g_gbpusd_london_open.on_tick(
            bid, ask, now_ms,
            /* can_enter        */ false,  // already in position; entry-path skipped via LIVE branch
            /* flow_live        */ false,
            /* flow_be_locked   */ false,
            /* flow_trail_stage */ 0,
            bracket_on_close,
            /* book_slope */ 0.0,
            /* vacuum_ask */ g_macro_ctx.gbp_vacuum_ask,
            /* vacuum_bid */ g_macro_ctx.gbp_vacuum_bid,
            /* wall_above */ g_macro_ctx.gbp_wall_above,
            /* wall_below */ g_macro_ctx.gbp_wall_below,
            /* l2_real    */ g_macro_ctx.ctrader_l2_live);
    } else {
        const bool can_enter = tradeable && lat_ok;
        g_gbpusd_london_open.on_tick(
            bid, ask, now_ms,
            can_enter,
            /* flow_live        */ false,
            /* flow_be_locked   */ false,
            /* flow_trail_stage */ 0,
            bracket_on_close,
            /* book_slope */ 0.0,
            /* vacuum_ask */ g_macro_ctx.gbp_vacuum_ask,
            /* vacuum_bid */ g_macro_ctx.gbp_vacuum_bid,
            /* wall_above */ g_macro_ctx.gbp_wall_above,
            /* wall_below */ g_macro_ctx.gbp_wall_below,
            /* l2_real    */ g_macro_ctx.ctrader_l2_live);
    }

    // AtrMeanRevGrid GBPUSD (shadow). Backtest: PF 2.11 @ X=10 (13 trades, 2yr).
    if (g_amr_gbpusd.enabled) g_amr_gbpusd.on_tick(bid, ask, now_ms);
    if (g_fx_ens_gbpusd.enabled)
        g_fx_ens_gbpusd.on_tick(bid, ask, now_ms, write_shadow_csv);

    // EurGbpPairsEngine -- GBP leg (see on_tick_eurusd for EUR leg).
    {
        auto ca_on_close = [](const omega::TradeRecord& tr){ (void)tr; };
        g_eur_gbp_pairs.on_tick_gbp(bid, ask, now_ms, ca_on_close);
    }

    (void)sym; (void)regime; (void)dispatch;
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
    // 2026-05-05 (audit-fixes-40): heartbeat pulse (see on_tick_eurusd).
    g_engine_heartbeat.pulse("UsdjpyAsianOpen");

    // Store USDJPY mid for tick_value_multiplier (live JPY/USD conversion).
    //   Done unconditionally before any engine logic, same pattern as the
    //   eur_mid_price store in on_tick_eurusd.
    g_usdjpy_mid.store((bid + ask) * 0.5, std::memory_order_relaxed);

    // 2026-05-23: FxTurtleH4 USDJPY dispatch (post-S99 rebuild).
    {
        const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
        auto on_close_cb = [](const omega::TradeRecord& tr){ (void)tr; };
        g_usdjpy_turtle_h4.on_tick(bid, ask, now_ms_fx, on_close_cb);
        g_usdjpy_turtle_h4.check_weekend_close(bid, ask, now_ms_fx, on_close_cb);
    }

    // S38d 2026-05-26: FxScalpPyramid_USDJPY dispatch (shadow-mode).
    {
        const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
        const bool fx_can_enter = tradeable && lat_ok;
        g_fx_scalp_usdjpy.on_tick(bid, ask, now_ms_fx,
                                  fx_can_enter,
                                  /*l2_imbalance=*/0.5, /*book_slope=*/0.0,
                                  /*vacuum_ask=*/false, /*vacuum_bid=*/false,
                                  /*wall_above=*/false, /*wall_below=*/false,
                                  /*l2_real=*/false,
                                  nullptr);
    }

    // S37g 2026-05-26 FxEnsembleEngine USDJPY (donchian_20 H2 LONG cell).
    {
        const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
        if (g_fx_ens_usdjpy.enabled)
            g_fx_ens_usdjpy.on_tick(bid, ask, now_ms_fx, write_shadow_csv);
    }

    // S99: FX kill-switch (see on_tick_eurusd comment). UsdjpyAsianOpen disabled.
    (void)sym; (void)regime; (void)dispatch; (void)tradeable; (void)lat_ok;
    return;

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
// 2026-05-02: USDJPY split out to on_tick_usdjpy. This handler now serves
//   AUDUSD (live engine) + NZDUSD (live engine, since 2026-05-04).
// 2026-05-04 (audit-fixes-36 + S57): AUDUSD dispatches to
//   AudusdSydneyOpenEngine using the same two-phase pattern as
//   on_tick_usdjpy (manage-when-open vs entry-when-flat). NZDUSD now
//   dispatches to NzdusdAsianOpenEngine via the same two-phase pattern
//   (NZD branch added 2026-05-04). Both [FX-NO-ENGINE] diag stubs have
//   been removed -- each engine's own [AUD-SYD-OPEN-DIAG] /
//   [NZD-ASN-OPEN-DIAG] warmup logger provides the operator-visible
//   tick-flow signal.
template<typename Dispatch>
static void on_tick_audusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // -- AUDUSD: real engine dispatch --------------------------------------
    if (sym == "AUDUSD") {
        // 2026-05-05 (audit-fixes-40): heartbeat pulse (see on_tick_eurusd).
        g_engine_heartbeat.pulse("AudusdSydneyOpen");

        // 2026-05-23: FxTurtleH4 AUD dispatch (post-S99 rebuild).
        {
            const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
            auto on_close_cb = [](const omega::TradeRecord& tr){ (void)tr; };
            g_audusd_turtle_h4.on_tick(bid, ask, now_ms_fx, on_close_cb);
            g_audusd_turtle_h4.check_weekend_close(bid, ask, now_ms_fx, on_close_cb);
        }

        // S38d 2026-05-26: FxScalpPyramid_AUDUSD dispatch (shadow-mode).
        {
            const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
            const bool fx_can_enter = tradeable && lat_ok;
            g_fx_scalp_audusd.on_tick(bid, ask, now_ms_fx,
                                      fx_can_enter,
                                      /*l2_imbalance=*/0.5, /*book_slope=*/0.0,
                                      /*vacuum_ask=*/false, /*vacuum_bid=*/false,
                                      /*wall_above=*/false, /*wall_below=*/false,
                                      /*l2_real=*/false,
                                      nullptr);
        }

        // S99: FX kill-switch (see on_tick_eurusd comment). AudusdSydneyOpen disabled.
        (void)regime; (void)dispatch; (void)tradeable; (void)lat_ok;
        return;

        const int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;

        // Two-phase dispatch (mirrors on_tick_usdjpy):
        //   1) If position open, manage unconditionally (regardless of
        //      tradeable/lat_ok). This guarantees SL/TP/trail/BE management
        //      never stops mid-trade.
        //   2) If no position, run entry path gated by tradeable && lat_ok.
        //      The engine internally short-circuits on session window, news
        //      blackout, spread, ATR gate, and same-level block.
        //
        // DOM fields plumbed from g_macro_ctx AUD-side fields (vacuum-only,
        // mirrors USDJPY's DOM shape):
        //   - aud_vacuum_ask, aud_vacuum_bid (populated alongside JPY per
        //     SymbolEngines.hpp:111-112).
        //   - NO aud_wall_above / aud_wall_below / aud_slope -- pass false /
        //     0.0; the engine's wall and slope branches naturally inert at
        //     those values.
        //   - l2_real wired to g_macro_ctx.ctrader_l2_live -- safe-fallback
        //     when cTrader depth is offline.
        if (g_audusd_sydney_open.has_open_position()) {
            g_audusd_sydney_open.on_tick(
                bid, ask, now_ms,
                /* can_enter        */ false,  // already in position; entry-path skipped via LIVE branch
                /* flow_live        */ false,
                /* flow_be_locked   */ false,
                /* flow_trail_stage */ 0,
                bracket_on_close,
                /* book_slope */ 0.0,
                /* vacuum_ask */ g_macro_ctx.aud_vacuum_ask,
                /* vacuum_bid */ g_macro_ctx.aud_vacuum_bid,
                /* wall_above */ false,
                /* wall_below */ false,
                /* l2_real    */ g_macro_ctx.ctrader_l2_live);
        } else {
            const bool can_enter = tradeable && lat_ok;
            g_audusd_sydney_open.on_tick(
                bid, ask, now_ms,
                can_enter,
                /* flow_live        */ false,
                /* flow_be_locked   */ false,
                /* flow_trail_stage */ 0,
                bracket_on_close,
                /* book_slope */ 0.0,
                /* vacuum_ask */ g_macro_ctx.aud_vacuum_ask,
                /* vacuum_bid */ g_macro_ctx.aud_vacuum_bid,
                /* wall_above */ false,
                /* wall_below */ false,
                /* l2_real    */ g_macro_ctx.ctrader_l2_live);
        }

        // AtrMeanRevGrid AUDUSD (shadow). Backtest: PF 1.06 (marginal).
        if (g_amr_audusd.enabled) g_amr_audusd.on_tick(bid, ask, now_ms);
        if (g_fx_ens_audusd.enabled)
            g_fx_ens_audusd.on_tick(bid, ask, now_ms, write_shadow_csv);

        (void)regime; (void)dispatch;
        return;
    }

    // -- NZDUSD: real engine dispatch --------------------------------------
    // 2026-05-04 (audit-fixes-36 + S57): NzdusdAsianOpenEngine wired with
    //   the same two-phase manage-when-open / entry-when-flat pattern as
    //   the AUDUSD branch above. DOM fields plumbed from g_macro_ctx
    //   NZD-side fields (vacuum-only, mirrors AUD/JPY DOM shape):
    //     - nzd_vacuum_ask, nzd_vacuum_bid (populated alongside AUD per
    //       SymbolEngines.hpp:113-114).
    //     - NO nzd_wall_above / nzd_wall_below / nzd_slope -- pass false /
    //       0.0; the engine's wall and slope branches naturally inert at
    //       those values.
    //     - l2_real wired to g_macro_ctx.ctrader_l2_live -- safe-fallback
    //       when cTrader depth is offline.
    if (sym == "NZDUSD") {
        // 2026-05-05 (audit-fixes-40): heartbeat pulse (see on_tick_eurusd).
        g_engine_heartbeat.pulse("NzdusdAsianOpen");

        // 2026-05-23: FxTurtleH4 NZD dispatch (post-S99 rebuild).
        {
            const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
            auto on_close_cb = [](const omega::TradeRecord& tr){ (void)tr; };
            g_nzdusd_turtle_h4.on_tick(bid, ask, now_ms_fx, on_close_cb);
            g_nzdusd_turtle_h4.check_weekend_close(bid, ask, now_ms_fx, on_close_cb);
        }

        // S99: FX kill-switch (see on_tick_eurusd comment). NzdusdAsianOpen disabled.
        (void)regime; (void)dispatch; (void)tradeable; (void)lat_ok;
        return;

        const int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;

        if (g_nzdusd_asian_open.has_open_position()) {
            g_nzdusd_asian_open.on_tick(
                bid, ask, now_ms,
                /* can_enter        */ false,  // already in position; entry-path skipped via LIVE branch
                /* flow_live        */ false,
                /* flow_be_locked   */ false,
                /* flow_trail_stage */ 0,
                bracket_on_close,
                /* book_slope */ 0.0,
                /* vacuum_ask */ g_macro_ctx.nzd_vacuum_ask,
                /* vacuum_bid */ g_macro_ctx.nzd_vacuum_bid,
                /* wall_above */ false,
                /* wall_below */ false,
                /* l2_real    */ g_macro_ctx.ctrader_l2_live);
        } else {
            const bool can_enter = tradeable && lat_ok;
            g_nzdusd_asian_open.on_tick(
                bid, ask, now_ms,
                can_enter,
                /* flow_live        */ false,
                /* flow_be_locked   */ false,
                /* flow_trail_stage */ 0,
                bracket_on_close,
                /* book_slope */ 0.0,
                /* vacuum_ask */ g_macro_ctx.nzd_vacuum_ask,
                /* vacuum_bid */ g_macro_ctx.nzd_vacuum_bid,
                /* wall_above */ false,
                /* wall_below */ false,
                /* l2_real    */ g_macro_ctx.ctrader_l2_live);
        }

        // AtrMeanRevGrid NZDUSD (shadow). Backtest: PF 0.92 (negative; keep enabled=false until tuned).
        if (g_amr_nzdusd.enabled) g_amr_nzdusd.on_tick(bid, ask, now_ms);
        if (g_fx_ens_nzdusd.enabled)
            g_fx_ens_nzdusd.on_tick(bid, ask, now_ms, write_shadow_csv);

        (void)regime; (void)dispatch;
        return;
    }

    // Defensive fall-through: if the on_tick.hpp dispatcher ever routes a
    //   non-AUD/non-NZD symbol here, suppress unused-arg warnings cleanly.
    (void)bid; (void)ask; (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
}

// ── USDCAD ──────────────────────────────────────────────────
// S38d 2026-05-26: new handler added for USDCAD to host FxScalpPyramid_USDCAD.
//   No prior FX session-open / Turtle wiring for USDCAD; this handler exists
//   solely to feed M5 ticks to the scalp pyramid engine.
template<typename Dispatch>
static void on_tick_usdcad(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    g_engine_heartbeat.pulse("FxScalpPyramid_USDCAD");

    // S38d: FxScalpPyramid_USDCAD dispatch (shadow-mode).
    {
        const int64_t now_ms_fx = static_cast<int64_t>(std::time(nullptr)) * 1000;
        const bool fx_can_enter = tradeable && lat_ok;
        g_fx_scalp_usdcad.on_tick(bid, ask, now_ms_fx,
                                  fx_can_enter,
                                  /*l2_imbalance=*/0.5, /*book_slope=*/0.0,
                                  /*vacuum_ask=*/false, /*vacuum_bid=*/false,
                                  /*wall_above=*/false, /*wall_below=*/false,
                                  /*l2_real=*/false,
                                  nullptr);

        // S37 audit fix (2026-05-27): wire previously-orphan g_fx_ens_usdcad
        // dispatch. The engine has 4 cells configured + booted in
        // engine_init.hpp:1941-1947 (S37h PF 2.20-2.31 across cells) but
        // was never dispatched -- the booting code was wired without the
        // tick-path call site landing. Add it now under the same gating
        // pattern as g_fx_ens_eurusd / g_fx_ens_gbpusd above.
        if (g_fx_ens_usdcad.enabled)
            g_fx_ens_usdcad.on_tick(bid, ask, now_ms_fx, write_shadow_csv);
    }

    (void)sym; (void)regime; (void)dispatch;
}
