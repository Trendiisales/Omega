#pragma once  // OM-08 (audit 2026-07-13): TU-fragment guard
// tick_fx.hpp -- FX tick handlers (MACRO-CONTEXT ONLY)
//
// S-2026-06-29 FULL FX REMOVAL ("no FX", operator directive):
//   Every FX trading engine has been removed from the tick path. The five
//   session-open engines (Eurusd/Gbpusd/Usdjpy/Audusd/Nzdusd LondonOpen/
//   AsianOpen/SydneyOpen) were already dead behind the S99 kill-switch; the
//   FxTurtleH4 cohort was tombstoned 2026-06-16; TrendLineBreak (GBP/JPY),
//   EurGbpPairs, the FxEnsemble (AtrMeanRevGrid FX) grid, the FxScalpPyramid
//   family, FxCarry/FxCrossRev/FxSeasonal, and the MondayRiskOn FX legs were
//   all neutralized in prior passes. This pass deletes the residual dispatch
//   scaffolding so NO FX engine is referenced from the tick path at all.
//
//   FX symbols remain SUBSCRIBED for macro context only:
//     - EURUSD mid -> g_macro_ctx.eur_mid_price  (gold correlation)
//     - GBPUSD mid -> g_macro_ctx.gbp_mid_price  (GBP correlation + bracket
//                                                  trend bias, on_tick.hpp:1177)
//     - USDJPY mid -> g_usdjpy_mid               (live JPY/USD sizing conversion
//                                                  via tick_value_multiplier)
//   No orders are sent from any FX handler. These stores are the ONLY reason
//   the handlers still exist.
//
// Lineage (why FX was abandoned): 2026-04-06 global FX disable found zero
//   winning trades across all pairs (BreakoutEngine enters after the move;
//   FxCascade/Carry/VWAPReversion/FixWindow all edgeless on this feed). The
//   2026-05 re-enable experiments (session-open + Turtle + Ensemble) were
//   swept to negative expectancy by S99 and retired pair-by-pair through June.

#include <chrono>
#include "FxBeFloorCompanion.hpp"   // omega::fx_befloor_book() (per-pair BE-floor companion)
// (JumpRiderEngine.hpp include REMOVED — engine culled/tombstoned S-2026-07-10)
#include "FxMimicLadderCompanion.hpp" // omega::fx_mimic_ladder_book() (mimic LADDER, needs H1 h/l/c)
#include "OmegaBeCascadeBook.hpp"      // omega::be_cascade_book() (BE-CASCADE companion, S-2026-07-16)

// ── FX chart + companion bar builder (S-2026-07-06) ─────────────────────────
//   FX has no trading engine (2026-06-29 removal stands), but it is subscribed for
//   macro context. Aggregate its tick mids into M1/M5/M15/H1 bars so (a) the desk
//   price chart (/api/predictive_ranges) can show FX ranges, and (b) the FX BE-floor
//   companion gets its H1 close stream. Mirrors the US500 builder in tick_indices.hpp.
//   Observe-only: feeds a GUI chart + a shadow companion, sends no orders.
struct FxBarAgg { OHLCBar m1{}, m5{}, m15{}, h1{}; int64_t s1=0, s5=0, s15=0, sh1=0; };
static inline void fx_feed_bars(FxBarAgg& a, SymBarState& bars, const char* pair, double mid) {
    if (mid <= 0.0) return;
    omega::fx_mimic_ladder_book().set_disp_mid(pair, mid);   // S-2026-07-08d live display mark
    const int64_t now_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto roll = [&](OHLCBar& acc, int64_t& start, int64_t span, OHLCBarEngine& sink, bool drive) {
        const int64_t b = (now_ms / span) * span;
        if (start == 0) { acc = {b/60000LL, mid, mid, mid, mid, 0}; start = b; }
        else if (b != start) {
            sink.add_bar(acc);
            if (drive) {
                omega::fx_befloor_book().on_h1_bar(pair, start / 1000, acc.close);
                // (JumpRider FX feed REMOVED — engine culled/tombstoned S-2026-07-10)
                omega::fx_mimic_ladder_book().on_h1_bar(pair, start / 1000,
                                                         acc.high, acc.low, acc.close, acc.open); // h/l intrabar; open for Layer-3 weekend gap
                // S-2026-07-16: BE-CASCADE companion (EURUSD/GBPUSD/AUDUSD cells; others absent = no-op)
                omega::be_cascade_book().on_bar(pair, start / 1000, acc.high, acc.low, acc.close);
            }
            acc = {b/60000LL, mid, mid, mid, mid, 0}; start = b;
        } else { if (mid > acc.high) acc.high = mid; if (mid < acc.low) acc.low = mid; acc.close = mid; }
    };
    roll(a.m1,  a.s1,   60000LL, bars.m1,  false);
    roll(a.m5,  a.s5,  300000LL, bars.m5,  false);
    roll(a.m15, a.s15, 900000LL, bars.m15, false);
    roll(a.h1,  a.sh1,3600000LL, bars.h1,  true);   // drive the companion on each H1 close
}

// ── EURUSD ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_eurusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // Macro context price -- needed by gold correlation logic.
    g_macro_ctx.eur_mid_price = (bid + ask) * 0.5;
    { static FxBarAgg agg; fx_feed_bars(agg, g_bars_eurusd, "EURUSD", (bid + ask) * 0.5); }
    (void)sym; (void)regime; (void)dispatch; (void)tradeable; (void)lat_ok;
}

// ── GBPUSD ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_gbpusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // Macro context price -- GBP correlation + bracket trend bias (on_tick.hpp:1177).
    g_macro_ctx.gbp_mid_price = (bid + ask) * 0.5;
    { static FxBarAgg agg; fx_feed_bars(agg, g_bars_gbpusd, "GBPUSD", (bid + ask) * 0.5); }
    (void)sym; (void)regime; (void)dispatch; (void)tradeable; (void)lat_ok;
}

// ── USDJPY ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_usdjpy(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // USDJPY mid for tick_value_multiplier (live JPY/USD conversion).
    g_usdjpy_mid.store((bid + ask) * 0.5, std::memory_order_relaxed);
    { static FxBarAgg agg; fx_feed_bars(agg, g_bars_usdjpy, "USDJPY", (bid + ask) * 0.5); }
    (void)sym; (void)regime; (void)dispatch; (void)tradeable; (void)lat_ok;
}

// ── AUDUSD/NZDUSD ──────────────────────────────────────────
// Serves both AUDUSD and NZDUSD. No macro-context store for either pair --
//   nothing reads aud/nzd mids -- so these are now no-op handlers retained
//   only so the on_tick.hpp dispatcher has a valid routing target.
template<typename Dispatch>
static void on_tick_audusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    const double mid = (bid + ask) * 0.5;
    if      (sym == "AUDUSD") { static FxBarAgg agg; fx_feed_bars(agg, g_bars_audusd, "AUDUSD", mid); }
    else if (sym == "NZDUSD") { static FxBarAgg agg; fx_feed_bars(agg, g_bars_nzdusd, "NZDUSD", mid); }
    (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
}

// ── USDCAD ──────────────────────────────────────────────────
// S-2026-07-08c: USDCAD re-activated as a BAR SOURCE ONLY for the DOWN-JUMP
// SHORT ladder companion (FxMimicLadderCompanion short_downjump=true; sweep
// outputs/FX_BOTHWAYS_SWEEP_2026-07-08.md row 5, W96/0.5 +2241bp PF1.58).
// Quotes arrive via the IBKR IDEALPRO L1 bridge (same path as the other 5
// pairs; USDCAD is NOT on the BlackBull ext-symbol list, IBKR is its only
// source). Observe-only: feeds bars to a shadow companion, sends no orders —
// the 2026-06-29 FX execution removal stands.
template<typename Dispatch>
static void on_tick_usdcad(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    { static FxBarAgg agg; fx_feed_bars(agg, g_bars_usdcad, "USDCAD", (bid + ask) * 0.5); }
    (void)sym; (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
}
