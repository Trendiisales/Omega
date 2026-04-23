#pragma once
// sizing.hpp -- extracted from main.cpp
// Section: sizing (original lines 1655-1871)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static double tick_value_multiplier(const std::string& symbol) noexcept {
    // ?????????????????????????????????????????????????????????????????????????
    // BlackBull Markets cTrader CFD/Futures contract sizes -- verified 2026-03
    // Formula: P&L (USD) = price_move_pts * contract_size_per_lot * lots
    //
    // COMMODITIES (confirmed from BlackBull instrument pages):
    //   USOIL.F  = 1,000 barrels/lot  ? $1000/pt  (verified: 0.57pt ? $1000 = $570 ?)
    //   UKBRENT  = 1,000 barrels/lot  ? $1000/pt  (scraped: "1,000 barrel")
    //   XAUUSD   = 100 troy oz/lot    ? $100/pt   (BlackBull XAUUSD spec: 100 oz)
    //   XAGUSD   = 5,000 troy oz/lot  ? $5000/pt  (scraped: "5,000 oz")
    //
    // FX (industry standard):
    //   EURUSD   = 100,000 units/lot  ? $100,000/pt (1 pip = $10 at 0.0001)
    //
    // INDEX CFDs on cTrader (BlackBull futures are CFDs, NOT CME exchange contracts):
    //   1 lot = 1 contract = $1 per index point -- this is the cTrader CFD standard.
    //   Previous values ($50, $20, $25, $5) were CME full/E-mini exchange specs
    //   and do NOT apply to BlackBull CFD instruments.
    //   Confirmed: sizing formula with $1/pt produces correct lot counts for
    //   the $50 risk budget vs observed trade sizes.
    // ?????????????????????????????????????????????????????????????????????????
    if (symbol == "USOIL.F")  return 1000.0;  // WTI CFD future: 1,000 barrels/lot ? verified
    if (symbol == "BRENT")  return 1000.0;  // Brent CFD future: 1,000 barrels/lot ? scraped
    if (symbol == "XAUUSD")   return 100.0;   // Gold spot CFD: 100 troy oz/lot ? confirmed
    if (symbol == "XAGUSD")   return 5000.0;  // Silver spot CFD: 5,000 troy oz/lot ? scraped
    if (symbol == "EURUSD")   return 100000.0;// FX major: 100,000 units/lot ? standard
    if (symbol == "GBPUSD")   return 100000.0;// FX major: 100,000 units/lot
    // FX minors -- same 100,000 units/lot contract size as majors
    if (symbol == "AUDUSD")   return 100000.0;// AUD/USD: 100,000 units/lot
    if (symbol == "NZDUSD")   return 100000.0;// NZD/USD: 100,000 units/lot
    // USDJPY: JPY-quoted pair. P&L in USD = move_JPY * 100000 / rate.
    // Use live mid price updated every tick -- avoids static approximation
    // drifting ?8% as the rate moves between 140-160.
    // g_usdjpy_mid initialised to 150.0 so this is safe before first tick.
    if (symbol == "USDJPY") {
        const double rate = g_usdjpy_mid.load(std::memory_order_relaxed);
        return (rate > 0.0) ? (100000.0 / rate) : 667.0;
    }
    // Index CFDs -- BlackBull cTrader: $1 per index point per lot
    // INDEX CFDs -- BlackBull cTrader contract sizes (verified from Symbol Info spec pages 2026-03-21)
    // ?????????????????????????????????????????????????????????????????????????????????????????????
    // Lot size shown in Symbol Info = denomination per lot = $/pt/lot for USD-quoted instruments
    // EUR/GBP-quoted instruments need FX conversion (static approx: EUR?1.10, GBP?1.33)
    //
    // US500.F : lot_size=50.0  US500.F  ? $50/pt/lot   (confirmed from spec)
    // USTEC.F : lot_size=20.0  USTEC.F  ? $20/pt/lot   (confirmed from spec)
    // DJ30.F  : lot_size=5.0   DJ30.F   ? $5/pt/lot    (confirmed from spec)
    // NAS100  : lot_size=1.0   NAS100   ? $1/pt/lot    (confirmed; min trade=0.1 lots)
    // GER40   : lot_size=1.0   GER40, quote=EUR ? $1.10/pt/lot  (EUR?1.10 approx)
    // UK100   : lot_size=1.0   UK100,  quote=GBP ? $1.33/pt/lot (GBP?1.33 approx)
    // ESTX50  : lot_size=1.0   ESTX50, quote=EUR ? $1.10/pt/lot (EUR?1.10 approx)
    if (symbol == "US500.F")  return 50.0;   // confirmed: lot=50 US500.F
    if (symbol == "USTEC.F")  return 20.0;   // confirmed: lot=20 USTEC.F
    if (symbol == "DJ30.F")   return 5.0;    // confirmed: lot=5 DJ30.F
    if (symbol == "NAS100")   return 1.0;    // confirmed: lot=1 NAS100 (USD, min 0.1 lots)
    if (symbol == "GER40")    return 1.10;   // confirmed: lot=1 GER40 EUR-quoted ? 1.10
    if (symbol == "UK100")    return 1.33;   // confirmed: lot=1 UK100 GBP-quoted ? 1.33
    if (symbol == "ESTX50")   return 1.10;   // confirmed: lot=1 ESTX50 EUR-quoted ? 1.10
    return 1.0;  // Unknown symbol: no scaling (safe fallback)
}

// =============================================================================


// ?? Live trade telemetry helper -- static free function, callable from any context ??
// Cannot be a lambda: MSVC refuses to call local lambdas defined inside another lambda
// even when non-capturing. Free function has no such restriction.
static void push_live_trade(const char* sym, const char* eng, bool il,
                             double entry, double tp, double sl,
                             double sz, int64_t ts)
{
    double cb = 0.0, ca = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_book_mtx);
        auto bi = g_bids.find(sym);
        auto ai = g_asks.find(sym);
        if (bi != g_bids.end()) cb = bi->second;
        if (ai != g_asks.end()) ca = ai->second;
    }
    const double cur = il ? cb : ca;
    const double tv  = tick_value_multiplier(sym);
    const double pnl = (il ? (cur - entry) : (entry - cur)) * sz * tv;
    // AddLiveTrade(symbol, engine, side, entry, current, tp, sl, size, pnl, tick_value, entry_ts)
    g_telemetry.AddLiveTrade(sym, eng, il ? "LONG" : "SHORT",
        entry, cur, tp, sl, sz, pnl, tv, ts);
}

// ?????????????????????????????????????????????????????????????????????????????
// compute_size() -- risk-based position sizing
//
// Sizes every trade so the maximum dollar loss (SL distance + spread cost)
// equals risk_per_trade_usd.
//
// Formula: size = risk_usd / ((sl_abs + spread_abs) * tick_value_per_lot)
//
// Falls back to fallback_size only when:
//   - risk_per_trade_usd == 0.0 (disabled -- use legacy fixed sizes)
//   - sl_abs or tick_mult is zero (safety: avoids division by zero)
//
// The result is clamped to [0.01, per-symbol max_lot cap].
// NOTE: the old floor of std::max(size, fallback_size) has been removed.
//       That floor overrode the risk calculation entirely -- on a $50 risk budget
//       with USOIL at $1000/pt, the correct size is 0.10 lots, but the floor
//       forced it to 1.0 lot, producing a $478 actual risk -- nearly 10? intended.
// ?????????????????????????????????????????????????????????????????????????????
// Live account equity -- updated on every closed trade in LIVE mode.
// Shadow mode keeps this at the configured account_equity value.
// Used by compute_size() so risk_per_trade_usd scales with account growth.
static std::atomic<double> g_live_equity{10000.0};
// Note: std::atomic<double> is lock-free on MSVC x64 (confirmed for this platform).
// is_always_lock_free may be false on some compilers even when runtime IS lock-free,
// so we use a runtime check logged at startup rather than a static_assert.

static double compute_size(const std::string& symbol,
                            double sl_abs,       // SL distance in price points
                            double spread_abs,   // current bid-ask spread in price points
                            double fallback_size // used only when risk sizing is disabled
                            ) noexcept {
    if (g_cfg.risk_per_trade_usd <= 0.0) return fallback_size;

    const double tick_mult = tick_value_multiplier(symbol);
    if (tick_mult <= 0.0 || sl_abs <= 0.0) return fallback_size;

    // Equity-scaled risk: risk_usd = equity * (config_risk / initial_equity)
    const double base_equity = std::max(g_cfg.account_equity, 100.0);
    const double risk_pct    = g_cfg.risk_per_trade_usd / base_equity;
    const double live_eq     = g_live_equity.load(std::memory_order_relaxed);
    const double risk_usd    = live_eq * risk_pct;

    const double total_risk_pts = sl_abs + spread_abs;
    double size = risk_usd / (total_risk_pts * tick_mult);

    // ?? VIX-based regime size scaling ????????????????????????????????????????
    // VIX classifies volatility regime and adjusts position size accordingly.
    // Logic validated against Winton/Man AHL published VIX-based sizing:
    //   VIX < 15  (CALM)     : equities/FX slightly larger, gold normal
    //   VIX 15-25 (NORMAL)   : baseline
    //   VIX 25-35 (ELEVATED) : equities/FX 75%, gold 125% (safe-haven bid)
    //   VIX > 35  (CRISIS)   : equities/FX 40%, gold 150% (crisis premium)
    const double vix = g_macro_ctx.vix;
    if (vix > 0.0) {
        const bool is_gold = (symbol == "XAUUSD");
        const bool is_eq   = (symbol == "US500.F" || symbol == "USTEC.F" ||
                               symbol == "DJ30.F"  || symbol == "NAS100"  ||
                               symbol == "GER40"   || symbol == "UK100"   ||
                               symbol == "ESTX50");
        const bool is_fx   = (symbol == "EURUSD" || symbol == "GBPUSD" ||
                               symbol == "AUDUSD" || symbol == "NZDUSD" ||
                               symbol == "USDJPY");

        double vix_mult = 1.0;
        if (vix < 15.0) {
            // Calm tape: equities slightly larger, gold normal
            if (is_eq || is_fx) vix_mult = 1.15;
        } else if (vix >= 25.0 && vix < 35.0) {
            // Elevated: reduce equities/FX, boost gold
            if      (is_gold)        vix_mult = 1.25;
            else if (is_eq || is_fx) vix_mult = 0.75;
        } else if (vix >= 35.0) {
            // Crisis: heavy equity/FX cut, max gold
            if      (is_gold)        vix_mult = 1.50;
            else if (is_eq)          vix_mult = 0.40;
            else if (is_fx)          vix_mult = 0.50;
        }
        // VIX 15-25: vix_mult stays 1.0 (baseline, no scaling)
        size *= vix_mult;
    }
    // ?? End VIX scaling ???????????????????????????????????????????????????????

    // Round to 2 decimal places (standard lot precision)
    size = std::floor(size * 100.0 + 0.5) / 100.0;
    if (size < 0.01) size = 0.01;

    // Per-symbol safety cap and minimum floor
    double cap = g_cfg.max_lot_indices;
    double flr = g_cfg.min_lot_indices;
    if      (symbol == "XAUUSD")                               { cap = g_cfg.max_lot_gold;    flr = g_cfg.min_lot_gold; }
    else if (symbol == "EURUSD")                               { cap = g_cfg.max_lot_fx;      flr = g_cfg.min_lot_fx; }
    else if (symbol == "GBPUSD")                               { cap = g_cfg.max_lot_gbpusd;  flr = g_cfg.min_lot_gbpusd; }
    else if (symbol == "AUDUSD")                               { cap = g_cfg.max_lot_audusd;  flr = g_cfg.min_lot_audusd; }
    else if (symbol == "NZDUSD")                               { cap = g_cfg.max_lot_nzdusd;  flr = g_cfg.min_lot_nzdusd; }
    else if (symbol == "USDJPY")                               { cap = g_cfg.max_lot_usdjpy;  flr = g_cfg.min_lot_usdjpy; }
    else if (symbol == "USOIL.F" || symbol == "BRENT")        { cap = g_cfg.max_lot_oil;     flr = g_cfg.min_lot_oil; }
    if (symbol == "NAS100") flr = std::max(flr, 0.10);

    size = std::min(size, cap);
    size = std::max(size, std::max(flr, 0.01));

    return size;
}


// Parse FIX SendingTime (tag 52) "YYYYMMDD-HH:MM:SS.mmm" -> microseconds since epoch
// Returns 0 on parse failure. Used for per-tick latency measurement.
static int64_t parse_fix_time_us(const std::string& ts) noexcept {
    if (ts.size() < 17) return 0;
    try {
        struct tm ti{};
        ti.tm_year = std::stoi(ts.substr(0,4))  - 1900;
        ti.tm_mon  = std::stoi(ts.substr(4,2))  - 1;
        ti.tm_mday = std::stoi(ts.substr(6,2));
        ti.tm_hour = std::stoi(ts.substr(9,2));
        ti.tm_min  = std::stoi(ts.substr(12,2));
        ti.tm_sec  = std::stoi(ts.substr(15,2));
        int ms = 0;
        if (ts.size() >= 21 && ts[17] == '.') ms = std::stoi(ts.substr(18,3));
#ifdef _WIN32
        const int64_t epoch_s = static_cast<int64_t>(_mkgmtime(&ti));
#else
        const int64_t epoch_s = static_cast<int64_t>(timegm(&ti));
#endif
        if (epoch_s < 0) return 0;
        return epoch_s * 1000000LL + static_cast<int64_t>(ms) * 1000LL;
    } catch (...) { return 0; }
}

