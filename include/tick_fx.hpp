// tick_fx.hpp -- FX tick handlers
// ALL FX ENGINES DISABLED 2026-04-06
// Zero winning trades across all FX pairs. Root causes analysed:
//   BreakoutEngine: enters after move is done (0.12% = 25-40% of daily range)
//   FxCascadeEngine: cascades from an edgeless EUR signal, 8pip TP barely covers spread
//   CarryUnwindEngine: VIX.F too sparse on cTrader for timing to work
//   VWAPReversionEngine: 2hr EWM VWAP chases price, enters INTO trends
//   FxFixWindowEngine: DST timing wrong in summer, CVD needs volume data we don't have
//
// FX symbols remain SUBSCRIBED for macro context:
//   EUR/GBP/JPY/AUD/NZD mid prices feed into g_macro_ctx for gold correlation
//   No orders are sent from any FX handler
//
// Will be rebuilt as a separate system targeting news-event momentum
// (MacroCrash pattern applied to FX: compression -> bracket -> breakout on NFP/CPI/FOMC)

// ── EURUSD ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_eurusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // Update macro context price -- needed by gold correlation logic
    g_macro_ctx.eur_mid_price = (bid + ask) * 0.5;
    // All trading engines disabled -- no orders
    (void)sym; (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
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

// ── AUDUSD/NZDUSD/USDJPY ────────────────────────────────────
template<typename Dispatch>
static void on_tick_audusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // Store USDJPY mid for macro context (carry trade indicator)
    if (sym == "USDJPY") g_usdjpy_mid.store((bid + ask) * 0.5, std::memory_order_relaxed);
    (void)tradeable; (void)lat_ok; (void)regime; (void)dispatch;
}
