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

// ── EURUSD ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_eurusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // Macro context price -- needed by gold correlation logic.
    g_macro_ctx.eur_mid_price = (bid + ask) * 0.5;
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
    (void)sym; (void)bid; (void)ask; (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
}

// ── USDCAD ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_usdcad(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    (void)sym; (void)bid; (void)ask; (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
}
