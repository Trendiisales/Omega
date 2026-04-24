#pragma once
// engine_config.hpp -- extracted from main.cpp
// Section: engine_config (original lines 3044-3634)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static bool session_tradeable() noexcept {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti; gmtime_s(&ti, &t);
    const int h = ti.tm_hour;

    // PRIMARY WINDOW -- London + NY (07:00-22:00 UTC by default)
    // Supports wrap-through-midnight: if start > end, range crosses 00:00
    // e.g. start=22 end=5 ? active from 22:00 through to 05:00
    bool in_primary = false;
    if (g_cfg.session_start_utc == g_cfg.session_end_utc) {
        in_primary = true;                                              // 24h explicit mode
    } else if (g_cfg.session_start_utc < g_cfg.session_end_utc) {
        in_primary = (h >= g_cfg.session_start_utc && h < g_cfg.session_end_utc); // normal range
    } else {
        in_primary = (h >= g_cfg.session_start_utc || h < g_cfg.session_end_utc); // wraps midnight
    }

    // ASIA WINDOW -- Tokyo gold market (22:00-05:00 UTC, wraps midnight)
    // Hardcoded range -- independent of primary window config.
    // When session_end_utc=22, primary hands off to Asia with zero gap.
    const bool in_asia = g_cfg.session_asia && (h >= 22 || h < 5);

    // PRE-LONDON WINDOW -- 05:00-07:00 UTC
    // Previously a gap between Asia (ends 05:00) and primary (starts 07:00).
    // RSIReversal trades this session -- clear moves visible.
    // (MicroMomentum removed at Batch 5V §1.2.)
    // Gold trades 24h -- there is no dead period, only thin spread.
    const bool in_pre_london = (h >= 5 && h < 7);

    return in_primary || in_asia || in_pre_london;
}

// ?????????????????????????????????????????????????????????????????????????????
// Apply config to engines -- per-symbol typed overloads
// ?????????????????????????????????????????????????????????????????????????????
// SP -- uses [sp] config section, links macro context
static void apply_engine_config(omega::SpEngine& eng) noexcept {
    eng.VOL_THRESH_PCT          = g_cfg.sp_vol_thresh_pct;
    eng.TP_PCT                  = g_cfg.sp_tp_pct;
    eng.SL_PCT                  = g_cfg.sp_sl_pct;
    eng.MIN_GAP_SEC             = std::max(g_cfg.sp_min_gap_sec, g_cfg.min_entry_gap_sec);
    eng.BASELINE_LOOKBACK       = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK    = g_cfg.compression_lookback;
    eng.MOMENTUM_THRESH_PCT     = g_cfg.sp_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT        = g_cfg.sp_min_breakout_pct;
    eng.MAX_SPREAD_PCT          = g_cfg.sp_max_spread_pct;
    eng.COMPRESSION_THRESHOLD   = g_cfg.sp_compression_threshold;
    eng.vix_panic               = g_cfg.sp_vix_panic;
    eng.div_threshold           = g_cfg.sp_div_threshold;
    eng.MAX_TRADES_PER_MIN      = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC            = g_cfg.max_hold_sec;
    eng.macro                   = &g_macro_ctx;
}
// NQ -- uses [nq] config section, links macro context
static void apply_engine_config(omega::NqEngine& eng) noexcept {
    eng.VOL_THRESH_PCT          = g_cfg.nq_vol_thresh_pct;
    eng.TP_PCT                  = g_cfg.nq_tp_pct;
    eng.SL_PCT                  = g_cfg.nq_sl_pct;
    eng.MIN_GAP_SEC             = std::max(g_cfg.nq_min_gap_sec, g_cfg.min_entry_gap_sec);
    eng.BASELINE_LOOKBACK       = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK    = g_cfg.compression_lookback;
    eng.MOMENTUM_THRESH_PCT     = g_cfg.nq_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT        = g_cfg.nq_min_breakout_pct;
    eng.MAX_SPREAD_PCT          = g_cfg.nq_max_spread_pct;
    eng.COMPRESSION_THRESHOLD   = g_cfg.nq_compression_threshold;
    eng.vix_panic               = g_cfg.nq_vix_panic;
    eng.div_threshold           = g_cfg.nq_div_threshold;
    eng.MAX_TRADES_PER_MIN      = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC            = g_cfg.max_hold_sec;
    eng.macro                   = &g_macro_ctx;
}
// Oil -- uses [oil] config section, inventory window block built into engine
static void apply_engine_config(omega::OilEngine& eng) noexcept {
    eng.VOL_THRESH_PCT          = g_cfg.oil_vol_thresh_pct;
    eng.TP_PCT                  = g_cfg.oil_tp_pct;
    eng.SL_PCT                  = g_cfg.oil_sl_pct;
    eng.MIN_GAP_SEC             = std::max(g_cfg.oil_min_gap_sec, g_cfg.min_entry_gap_sec);
    eng.BASELINE_LOOKBACK       = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK    = g_cfg.compression_lookback;
    eng.MOMENTUM_THRESH_PCT     = g_cfg.oil_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT        = g_cfg.oil_min_breakout_pct;
    eng.MAX_SPREAD_PCT          = g_cfg.oil_max_spread_pct;
    eng.COMPRESSION_THRESHOLD   = g_cfg.oil_compression_threshold;
    eng.vix_panic               = g_cfg.oil_vix_panic;
    eng.MAX_TRADES_PER_MIN      = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC            = std::max(g_cfg.oil_max_hold_sec, 1800);
    eng.macro                   = &g_macro_ctx;
}

static void apply_generic_index_config(omega::EuIndexEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = std::min(0.06, std::max(0.02, g_cfg.sp_vol_thresh_pct));
    eng.TP_PCT                = g_cfg.sp_tp_pct;
    eng.SL_PCT                = g_cfg.sp_sl_pct;
    eng.MIN_GAP_SEC           = std::max(30, g_cfg.sp_min_gap_sec);
    eng.MOMENTUM_THRESH_PCT   = g_cfg.eu_index_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT      = g_cfg.eu_index_min_breakout_pct;
    eng.MAX_SPREAD_PCT        = g_cfg.eu_index_max_spread_pct;
    eng.COMPRESSION_THRESHOLD = g_cfg.eu_index_compression_threshold;
    eng.BASELINE_LOOKBACK     = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK  = g_cfg.compression_lookback;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    eng.vix_panic             = g_cfg.sp_vix_panic;  // EU indices use same VIX panic as US equity
    eng.macro                 = &g_macro_ctx;         // required for VIX gate in shouldTrade()
}

// Us30 (DJ30.F) -- typed engine, links macro context
static void apply_engine_config(omega::Us30Engine& eng) noexcept {
    eng.VOL_THRESH_PCT          = std::min(0.05, std::max(0.02, g_cfg.sp_vol_thresh_pct));
    eng.TP_PCT                  = g_cfg.sp_tp_pct;
    eng.SL_PCT                  = g_cfg.sp_sl_pct;
    eng.MIN_GAP_SEC             = std::max(g_cfg.sp_min_gap_sec, g_cfg.min_entry_gap_sec);
    eng.BASELINE_LOOKBACK       = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK    = g_cfg.compression_lookback;
    eng.MOMENTUM_THRESH_PCT     = g_cfg.us30_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT        = g_cfg.us30_min_breakout_pct;
    eng.MAX_SPREAD_PCT          = g_cfg.us30_max_spread_pct;
    eng.COMPRESSION_THRESHOLD   = g_cfg.us30_compression_threshold;
    eng.vix_panic               = g_cfg.us30_vix_panic;
    eng.div_threshold           = g_cfg.us30_div_threshold;
    eng.MAX_TRADES_PER_MIN      = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC            = g_cfg.max_hold_sec;
    eng.macro                   = &g_macro_ctx;
}

// Nas100 -- typed engine, links macro context
static void apply_engine_config(omega::Nas100Engine& eng) noexcept {
    eng.VOL_THRESH_PCT          = g_cfg.nq_vol_thresh_pct;
    eng.TP_PCT                  = g_cfg.nq_tp_pct;
    eng.SL_PCT                  = g_cfg.nq_sl_pct;
    eng.MIN_GAP_SEC             = std::max(g_cfg.nq_min_gap_sec, g_cfg.min_entry_gap_sec);
    eng.BASELINE_LOOKBACK       = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK    = g_cfg.compression_lookback;
    eng.MOMENTUM_THRESH_PCT     = g_cfg.nas100_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT        = g_cfg.nas100_min_breakout_pct;
    eng.MAX_SPREAD_PCT          = g_cfg.nas100_max_spread_pct;
    eng.COMPRESSION_THRESHOLD   = g_cfg.nas100_compression_threshold;
    eng.vix_panic               = g_cfg.nas100_vix_panic;
    eng.div_threshold           = g_cfg.nas100_div_threshold;
    eng.MAX_TRADES_PER_MIN      = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC            = g_cfg.max_hold_sec;
    eng.macro                   = &g_macro_ctx;
}

static void apply_generic_fx_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.fx_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.fx_tp_pct;
    eng.SL_PCT                = g_cfg.fx_sl_pct;
    eng.MIN_GAP_SEC           = g_cfg.fx_min_gap_sec;
    eng.MAX_SPREAD_PCT        = g_cfg.fx_max_spread_pct;
    eng.COMPRESSION_THRESHOLD = g_cfg.fx_compression_threshold;
    eng.MOMENTUM_THRESH_PCT   = g_cfg.fx_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT      = g_cfg.fx_min_breakout_pct;
    eng.BASELINE_LOOKBACK     = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK  = g_cfg.compression_lookback;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
}

static void apply_generic_gbpusd_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.gbpusd_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.gbpusd_tp_pct;
    eng.SL_PCT                = g_cfg.gbpusd_sl_pct;
    eng.MIN_GAP_SEC           = g_cfg.gbpusd_min_gap_sec;
    eng.MAX_SPREAD_PCT        = g_cfg.gbpusd_max_spread_pct;
    eng.COMPRESSION_THRESHOLD = g_cfg.gbpusd_compression_threshold;
    eng.MOMENTUM_THRESH_PCT   = g_cfg.gbpusd_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT      = g_cfg.gbpusd_min_breakout_pct;
    eng.BASELINE_LOOKBACK     = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK  = g_cfg.compression_lookback;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
}

static void apply_generic_brent_config(omega::BrentEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.brent_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.brent_tp_pct;
    eng.SL_PCT                = g_cfg.brent_sl_pct;
    eng.MIN_GAP_SEC           = g_cfg.brent_min_gap_sec;
    eng.MAX_SPREAD_PCT        = g_cfg.brent_max_spread_pct;
    eng.COMPRESSION_THRESHOLD = g_cfg.brent_compression_threshold;
    eng.MOMENTUM_THRESH_PCT   = g_cfg.brent_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT      = g_cfg.brent_min_breakout_pct;
    eng.BASELINE_LOOKBACK     = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK  = g_cfg.compression_lookback;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    eng.vix_panic             = g_cfg.oil_vix_panic;  // commodity: 50.0 (same as OilEngine)
    eng.macro                 = &g_macro_ctx;          // required for VIX gate in shouldTrade()
}

static void apply_generic_audusd_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.audusd_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.audusd_tp_pct;
    eng.SL_PCT                = g_cfg.audusd_sl_pct;
    eng.MIN_GAP_SEC           = g_cfg.audusd_min_gap_sec;
    eng.MAX_SPREAD_PCT        = g_cfg.audusd_max_spread_pct;
    eng.COMPRESSION_THRESHOLD = g_cfg.audusd_compression_threshold;
    eng.MOMENTUM_THRESH_PCT   = g_cfg.audusd_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT      = g_cfg.audusd_min_breakout_pct;
    eng.BASELINE_LOOKBACK     = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK  = g_cfg.compression_lookback;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
}

static void apply_generic_nzdusd_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.nzdusd_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.nzdusd_tp_pct;
    eng.SL_PCT                = g_cfg.nzdusd_sl_pct;
    eng.MIN_GAP_SEC           = g_cfg.nzdusd_min_gap_sec;
    eng.MAX_SPREAD_PCT        = g_cfg.nzdusd_max_spread_pct;
    eng.COMPRESSION_THRESHOLD = g_cfg.nzdusd_compression_threshold;
    eng.MOMENTUM_THRESH_PCT   = g_cfg.nzdusd_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT      = g_cfg.nzdusd_min_breakout_pct;
    eng.BASELINE_LOOKBACK     = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK  = g_cfg.compression_lookback;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
}

static void apply_generic_usdjpy_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.usdjpy_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.usdjpy_tp_pct;
    eng.SL_PCT                = g_cfg.usdjpy_sl_pct;
    eng.MIN_GAP_SEC           = g_cfg.usdjpy_min_gap_sec;
    eng.MAX_SPREAD_PCT        = g_cfg.usdjpy_max_spread_pct;
    eng.COMPRESSION_THRESHOLD = g_cfg.usdjpy_compression_threshold;
    eng.MOMENTUM_THRESH_PCT   = g_cfg.usdjpy_momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT      = g_cfg.usdjpy_min_breakout_pct;
    eng.BASELINE_LOOKBACK     = g_cfg.baseline_lookback;
    eng.COMPRESSION_LOOKBACK  = g_cfg.compression_lookback;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
}

// ?????????????????????????????????????????????????????????????????????????????
// Safe config parsing helpers -- malformed values log a warning instead of crashing
// ?????????????????????????????????????????????????????????????????????????????
static int safe_stoi(const std::string& v, const std::string& key, int fallback = 0) {
    try { return std::stoi(v); }
    catch (...) { std::cerr << "[CONFIG-WARN] bad int for '" << key << "': " << v << "\n"; return fallback; }
}
static double safe_stod(const std::string& v, const std::string& key, double fallback = 0.0) {
    try { return std::stod(v); }
    catch (...) { std::cerr << "[CONFIG-WARN] bad double for '" << key << "': " << v << "\n"; return fallback; }
}

// ?????????????????????????????????????????????????????????????????????????????
// Config loader
// ?????????????????????????????????????????????????????????????????????????????
static void load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) { std::cout << "[CONFIG] Using defaults\n"; return; }
    std::string line, section;
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        while (!s.empty() && s.front() == ' ') s = s.substr(1);
    };
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        trim(k); trim(v);
        const auto cm = v.find('#');
        if (cm != std::string::npos) v = v.substr(0, cm);
        trim(v);
        if (v.empty()) continue;

        if (section == "fix") {
            if (k=="host")               g_cfg.host      = v;
            if (k=="port")               g_cfg.port      = safe_stoi(v, k);
            if (k=="trade_port")        g_cfg.trade_port = safe_stoi(v, k);
            if (k=="sender_comp_id")     g_cfg.sender    = v;
            if (k=="target_comp_id")     g_cfg.target    = v;
            if (k=="username")           g_cfg.username  = v;
            if (k=="password")           g_cfg.password  = v;
            if (k=="heartbeat_interval") g_cfg.heartbeat = safe_stoi(v, k);
            if (k=="connection_warmup_sec") g_cfg.connection_warmup_sec = safe_stoi(v, k);
        }
        if (section == "mode"     && k=="mode")         g_cfg.mode           = v;
        if (section == "breakout") {
            if (k=="vol_thresh_pct")        g_cfg.vol_thresh_pct        = safe_stod(v, k);
            if (k=="tp_pct")                g_cfg.tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.sl_pct                = safe_stod(v, k);
            if (k=="compression_lookback")  g_cfg.compression_lookback  = safe_stoi(v, k);
            if (k=="baseline_lookback")     g_cfg.baseline_lookback     = safe_stoi(v, k);
            if (k=="compression_threshold") g_cfg.compression_threshold = safe_stod(v, k);
            if (k=="max_hold_sec")          g_cfg.max_hold_sec          = safe_stoi(v, k);
            if (k=="min_entry_gap_sec")     g_cfg.min_entry_gap_sec     = safe_stoi(v, k);
            if (k=="max_spread_entry_pct")  g_cfg.max_spread_pct        = safe_stod(v, k);
            if (k=="momentum_threshold")    g_cfg.momentum_thresh_pct  = safe_stod(v, k);
            if (k=="min_breakout_move_pct") g_cfg.min_breakout_pct     = safe_stod(v, k);
            if (k=="max_trades_per_minute") g_cfg.max_trades_per_min   = safe_stoi(v, k);
            if (k=="max_trades_per_cycle")  g_cfg.max_trades_per_cycle = safe_stoi(v, k);
        }
        if (section == "risk") {
            if (k=="max_positions")        g_cfg.max_open_positions = safe_stoi(v, k);
            if (k=="reload_trades_on_startup") g_cfg.reload_trades_on_startup = (v=="true"||v=="1");
            if (k=="daily_loss_limit")     g_cfg.daily_loss_limit  = safe_stod(v, k);
            if (k=="daily_profit_target")  g_cfg.daily_profit_target = safe_stod(v, k);
            if (k=="max_loss_per_trade_usd") g_cfg.max_loss_per_trade_usd = safe_stod(v, k);
            if (k=="dollar_stop_usd")         g_cfg.dollar_stop_usd         = safe_stod(v, k);
            if (k=="max_portfolio_sl_risk_usd") g_cfg.max_portfolio_sl_risk_usd = safe_stod(v, k);
            if (k=="session_watermark_pct")  g_cfg.session_watermark_pct  = safe_stod(v, k);
            if (k=="hourly_loss_limit")    g_cfg.hourly_loss_limit   = safe_stod(v, k);
            if (k=="max_consec_losses")    g_cfg.max_consec_losses = safe_stoi(v, k);
            if (k=="loss_pause_sec")       g_cfg.loss_pause_sec    = safe_stoi(v, k);
            if (k=="independent_symbols")  g_cfg.independent_symbols = (v == "true" || v == "1");
            if (k=="auto_disable_after_trades")  g_cfg.auto_disable_after_trades = safe_stoi(v, k);
            if (k=="shadow_ustec_pilot_only")    g_cfg.shadow_ustec_pilot_only = (v == "true" || v == "1");
            if (k=="shadow_research_mode")       g_cfg.shadow_research_mode = (v == "true" || v == "1");
            if (k=="ustec_pilot_size")           g_cfg.ustec_pilot_size = safe_stod(v, k);
            if (k=="ustec_pilot_min_gap_sec")    g_cfg.ustec_pilot_min_gap_sec = safe_stoi(v, k);
            if (k=="ustec_pilot_require_session") g_cfg.ustec_pilot_require_session = (v == "true" || v == "1");
            if (k=="ustec_pilot_require_latency") g_cfg.ustec_pilot_require_latency = (v == "true" || v == "1");
            if (k=="ustec_pilot_block_risk_off")  g_cfg.ustec_pilot_block_risk_off = (v == "true" || v == "1");
            if (k=="enable_extended_symbols")    g_cfg.enable_extended_symbols = (v == "true" || v == "1");
            if (k=="goldflow_enabled")           g_cfg.goldflow_enabled        = (v == "true" || v == "1");
        if (k=="indices_enabled")            g_cfg.indices_enabled         = (v == "true" || v == "1");
            if (k=="min_entry_gap_sec")    g_cfg.min_entry_gap_sec = safe_stoi(v, k);
            if (k=="max_spread_entry_pct") g_cfg.max_spread_pct    = safe_stod(v, k);
            if (k=="max_latency_ms")       g_cfg.max_latency_ms    = safe_stod(v, k);
            // Risk-based position sizing
            if (k=="risk_per_trade_usd")   g_cfg.risk_per_trade_usd = safe_stod(v, k);
            if (k=="account_equity")       g_cfg.account_equity     = safe_stod(v, k);
            if (k=="max_lot_gold")         g_cfg.max_lot_gold       = safe_stod(v, k);
            if (k=="max_lot_indices")      g_cfg.max_lot_indices    = safe_stod(v, k);
            if (k=="max_lot_oil")          g_cfg.max_lot_oil        = safe_stod(v, k);
            if (k=="max_lot_fx")           g_cfg.max_lot_fx         = safe_stod(v, k);
            if (k=="max_lot_gbpusd")       g_cfg.max_lot_gbpusd     = safe_stod(v, k);
            if (k=="max_lot_audusd")       g_cfg.max_lot_audusd     = safe_stod(v, k);
            if (k=="max_lot_nzdusd")       g_cfg.max_lot_nzdusd     = safe_stod(v, k);
            if (k=="max_lot_usdjpy")       g_cfg.max_lot_usdjpy     = safe_stod(v, k);
            if (k=="min_lot_gold")         g_cfg.min_lot_gold       = safe_stod(v, k);
            if (k=="min_lot_indices")      g_cfg.min_lot_indices    = safe_stod(v, k);
            if (k=="min_lot_oil")          g_cfg.min_lot_oil        = safe_stod(v, k);
            if (k=="min_lot_fx")           g_cfg.min_lot_fx         = safe_stod(v, k);
            if (k=="gf_compression_vol_floor")      g_cfg.gf_compression_vol_floor      = safe_stod(v, k);
            if (k=="gf_compression_vol_floor_asia") g_cfg.gf_compression_vol_floor_asia = safe_stod(v, k);
            if (k=="min_lot_gbpusd")       g_cfg.min_lot_gbpusd     = safe_stod(v, k);
            if (k=="min_lot_audusd")       g_cfg.min_lot_audusd     = safe_stod(v, k);
            if (k=="min_lot_nzdusd")       g_cfg.min_lot_nzdusd     = safe_stod(v, k);
            if (k=="min_lot_usdjpy")       g_cfg.min_lot_usdjpy     = safe_stod(v, k);
            // Backward-compat: older configs place breakout keys under [risk].
            // Parse them here too so tuned values are not silently ignored.
            if (k=="momentum_threshold")    g_cfg.momentum_thresh_pct = safe_stod(v, k);
            if (k=="min_breakout_move_pct") g_cfg.min_breakout_pct    = safe_stod(v, k);
            if (k=="max_trades_per_minute") g_cfg.max_trades_per_min  = safe_stoi(v, k);
            if (k=="max_trades_per_cycle")  g_cfg.max_trades_per_cycle = safe_stoi(v, k);
        }
        if (section == "session") {
            if (k=="session_start_utc") g_cfg.session_start_utc = safe_stoi(v, k);
            if (k=="session_end_utc")   g_cfg.session_end_utc   = safe_stoi(v, k);
            if (k=="session_asia")      g_cfg.session_asia      = (v == "true" || v == "1");
        }
        if (section == "ctrader_api") {
            if (k=="access_token")           g_cfg.ctrader_access_token     = v;
            if (k=="refresh_token")          g_cfg.ctrader_refresh_token    = v;
            if (k=="ctid_trader_account_id") g_cfg.ctrader_ctid_account_id  = std::stoll(v);
            if (k=="enabled")                g_cfg.ctrader_depth_enabled    = (v == "true" || v == "1");
        }
        if (section == "telemetry") {
            if (k=="gui_port")   g_cfg.gui_port   = safe_stoi(v, k);
            if (k=="ws_port")    g_cfg.ws_port     = safe_stoi(v, k);
            if (k=="shadow_csv") g_cfg.shadow_csv  = v;
            if (k=="log_file")   g_cfg.log_file    = v;
        }
        if (section == "extended_ids") {
            if (k=="ger30_id")   g_cfg.ext_ger30_id   = safe_stoi(v, k);
            if (k=="uk100_id")   g_cfg.ext_uk100_id   = safe_stoi(v, k);
            if (k=="estx50_id")  g_cfg.ext_estx50_id  = safe_stoi(v, k);
            if (k=="eurusd_id")  g_cfg.ext_eurusd_id  = safe_stoi(v, k);
            if (k=="ukbrent_id") g_cfg.ext_ukbrent_id = safe_stoi(v, k);
            if (k=="gbpusd_id")  g_cfg.ext_gbpusd_id  = safe_stoi(v, k);
            if (k=="audusd_id")  g_cfg.ext_audusd_id  = safe_stoi(v, k);
            if (k=="nzdusd_id")  g_cfg.ext_nzdusd_id  = safe_stoi(v, k);
            if (k=="usdjpy_id")  g_cfg.ext_usdjpy_id  = safe_stoi(v, k);
        }
        if (section == "sp") {
            if (k=="tp_pct")                g_cfg.sp_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.sp_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.sp_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.sp_min_gap_sec           = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.sp_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.sp_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.sp_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.sp_compression_threshold = safe_stod(v, k);
            if (k=="vix_panic")             g_cfg.sp_vix_panic             = safe_stod(v, k);
            if (k=="div_threshold")         g_cfg.sp_div_threshold         = safe_stod(v, k);
        }
        if (section == "nq") {
            if (k=="tp_pct")                g_cfg.nq_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.nq_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.nq_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.nq_min_gap_sec           = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.nq_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.nq_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.nq_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.nq_compression_threshold = safe_stod(v, k);
            if (k=="vix_panic")             g_cfg.nq_vix_panic             = safe_stod(v, k);
            if (k=="div_threshold")         g_cfg.nq_div_threshold         = safe_stod(v, k);
        }
        if (section == "us30") {
            if (k=="momentum_thresh_pct")   g_cfg.us30_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.us30_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.us30_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.us30_compression_threshold = safe_stod(v, k);
            if (k=="vix_panic")             g_cfg.us30_vix_panic             = safe_stod(v, k);
            if (k=="div_threshold")         g_cfg.us30_div_threshold         = safe_stod(v, k);
        }
        if (section == "nas100") {
            if (k=="momentum_thresh_pct")   g_cfg.nas100_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.nas100_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.nas100_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.nas100_compression_threshold = safe_stod(v, k);
            if (k=="vix_panic")             g_cfg.nas100_vix_panic             = safe_stod(v, k);
            if (k=="div_threshold")         g_cfg.nas100_div_threshold         = safe_stod(v, k);
        }
        if (section == "oil") {
            if (k=="tp_pct")                g_cfg.oil_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.oil_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.oil_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.oil_min_gap_sec           = safe_stoi(v, k);
            if (k=="max_hold_sec")          g_cfg.oil_max_hold_sec          = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.oil_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.oil_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.oil_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.oil_compression_threshold = safe_stod(v, k);
            if (k=="vix_panic")             g_cfg.oil_vix_panic             = safe_stod(v, k);
        }
        if (section == "brent") {
            if (k=="tp_pct")                g_cfg.brent_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.brent_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.brent_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.brent_min_gap_sec           = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.brent_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.brent_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.brent_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.brent_compression_threshold = safe_stod(v, k);
        }
        if (section == "eu_index") {
            if (k=="momentum_thresh_pct")   g_cfg.eu_index_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.eu_index_min_breakout_pct      = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.eu_index_compression_threshold = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.eu_index_max_spread_pct        = safe_stod(v, k);
        }
        if (section == "fx") {
            if (k=="tp_pct")                g_cfg.fx_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.fx_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.fx_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.fx_min_gap_sec           = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.fx_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.fx_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.fx_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.fx_compression_threshold = safe_stod(v, k);
        }
        if (section == "gbpusd") {
            if (k=="tp_pct")                g_cfg.gbpusd_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.gbpusd_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.gbpusd_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.gbpusd_min_gap_sec           = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.gbpusd_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.gbpusd_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.gbpusd_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.gbpusd_compression_threshold = safe_stod(v, k);
        }
        if (section == "audusd") {
            if (k=="tp_pct")                g_cfg.audusd_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.audusd_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.audusd_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.audusd_min_gap_sec           = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.audusd_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.audusd_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.audusd_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.audusd_compression_threshold = safe_stod(v, k);
        }
        if (section == "nzdusd") {
            if (k=="tp_pct")                g_cfg.nzdusd_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.nzdusd_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.nzdusd_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.nzdusd_min_gap_sec           = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.nzdusd_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.nzdusd_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.nzdusd_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.nzdusd_compression_threshold = safe_stod(v, k);
        }
        if (section == "usdjpy") {
            if (k=="tp_pct")                g_cfg.usdjpy_tp_pct                = safe_stod(v, k);
            if (k=="sl_pct")                g_cfg.usdjpy_sl_pct                = safe_stod(v, k);
            if (k=="vol_thresh_pct")        g_cfg.usdjpy_vol_thresh_pct        = safe_stod(v, k);
            if (k=="min_gap_sec")           g_cfg.usdjpy_min_gap_sec           = safe_stoi(v, k);
            if (k=="momentum_thresh_pct")   g_cfg.usdjpy_momentum_thresh_pct   = safe_stod(v, k);
            if (k=="min_breakout_pct")      g_cfg.usdjpy_min_breakout_pct      = safe_stod(v, k);
            if (k=="max_spread_pct")        g_cfg.usdjpy_max_spread_pct        = safe_stod(v, k);
            if (k=="compression_threshold") g_cfg.usdjpy_compression_threshold = safe_stod(v, k);
        }
        if (section == "gold_stack") {
            auto& gs = g_cfg.gs_cfg;
            // Orchestrator gates
            if (k=="hard_sl_cooldown_sec")    gs.hard_sl_cooldown_sec     = safe_stoi(v, k);
            if (k=="side_chop_window_sec")    gs.side_chop_window_sec     = safe_stoi(v, k);
            if (k=="side_chop_pause_sec")     gs.side_chop_pause_sec      = safe_stoi(v, k);
            if (k=="same_level_reentry_sec")  gs.same_level_reentry_sec   = safe_stoi(v, k);
            if (k=="same_level_reentry_band") gs.same_level_reentry_band  = safe_stod(v, k);
            if (k=="min_vwap_dislocation")    gs.min_vwap_dislocation     = safe_stod(v, k);
            if (k=="max_entry_spread")        gs.max_entry_spread         = safe_stod(v, k);
            if (k=="min_entry_gap_sec")       gs.min_entry_gap_sec        = safe_stoi(v, k);
            // Position manager
            if (k=="max_hold_sec")            gs.max_hold_sec             = safe_stoi(v, k);
            if (k=="lock_arm_move")           gs.lock_arm_move            = safe_stod(v, k);
            if (k=="lock_gain")               gs.lock_gain                = safe_stod(v, k);
            if (k=="trail_arm_1")             gs.trail_arm_1              = safe_stod(v, k);
            if (k=="trail_dist_1")            gs.trail_dist_1             = safe_stod(v, k);
            if (k=="trail_arm_2")             gs.trail_arm_2              = safe_stod(v, k);
            if (k=="trail_dist_2")            gs.trail_dist_2             = safe_stod(v, k);
            if (k=="min_locked_profit")       gs.min_locked_profit        = safe_stod(v, k);
            if (k=="max_base_sl_ticks")       gs.max_base_sl_ticks        = safe_stod(v, k);
            // LiquiditySweepPro
            if (k=="sweep_pro_max_spread")    gs.sweep_pro_max_spread     = safe_stod(v, k);
            if (k=="sweep_pro_sl_ticks")      gs.sweep_pro_sl_ticks       = safe_stoi(v, k);
            if (k=="sweep_pro_base_size")     gs.sweep_pro_base_size      = safe_stod(v, k);
            // LiquiditySweepPressure
            if (k=="sweep_pres_max_spread")   gs.sweep_pres_max_spread    = safe_stod(v, k);
            if (k=="sweep_pres_sl_ticks")     gs.sweep_pres_sl_ticks      = safe_stoi(v, k);
            if (k=="sweep_pres_base_size")    gs.sweep_pres_base_size     = safe_stod(v, k);
        }
        if (section == "cross_asset") {
            if (k=="esnq_enabled")       g_ca_esnq.enabled       = (v == "true" || v == "1");
            if (k=="esnq_confirm_ticks") g_ca_esnq.CONFIRM_TICKS = safe_stoi(v, k);
            if (k=="esnq_cooldown_sec")  g_ca_esnq.COOLDOWN_SEC  = safe_stoi(v, k);
            if (k=="esnq_tp_pct")        g_ca_esnq.TP_PCT        = safe_stod(v, k);
            if (k=="esnq_sl_pct")        g_ca_esnq.SL_PCT        = safe_stod(v, k);
        }
        if (section == "minimal_h4") {
            auto& mp = g_minimal_h4_gold.p;
            if (k=="enabled")            g_minimal_h4_gold.enabled     = (v == "true" || v == "1");
            if (k=="shadow_mode")        g_minimal_h4_gold.shadow_mode = (v == "true" || v == "1");
            if (k=="donchian_bars")      mp.donchian_bars       = safe_stoi(v, k);
            if (k=="sl_mult")            mp.sl_mult             = safe_stod(v, k);
            if (k=="tp_mult")            mp.tp_mult             = safe_stod(v, k);
            if (k=="risk_dollars")       mp.risk_dollars        = safe_stod(v, k);
            if (k=="max_lot")            mp.max_lot             = safe_stod(v, k);
            if (k=="max_spread")         mp.max_spread          = safe_stod(v, k);
            if (k=="timeout_h4_bars")    mp.timeout_h4_bars     = safe_stoi(v, k);
            if (k=="cooldown_h4_bars")   mp.cooldown_h4_bars    = safe_stoi(v, k);
            if (k=="weekend_close_gate") mp.weekend_close_gate  = (v == "true" || v == "1");
        }
        // [latency_edge] section parser REMOVED at S13 Finding B 2026-04-24 — engine culled.
        // Any [latency_edge] keys in omega_config.ini are now silently ignored.
    }
    std::cout << "[CONFIG] mode=" << g_cfg.mode
              << " vol=" << g_cfg.vol_thresh_pct
              << "% tp=" << g_cfg.tp_pct
              << "% sl=" << g_cfg.sl_pct
              << "% maxhold=" << g_cfg.max_hold_sec << "s\n"
              << "[CONFIG] SP   tp=" << g_cfg.sp_tp_pct   << "% sl=" << g_cfg.sp_sl_pct   << "% vol=" << g_cfg.sp_vol_thresh_pct  << "%\n"
              << "[CONFIG] NQ   tp=" << g_cfg.nq_tp_pct   << "% sl=" << g_cfg.nq_sl_pct   << "% vol=" << g_cfg.nq_vol_thresh_pct  << "%\n"
              << "[CONFIG] OIL  tp=" << g_cfg.oil_tp_pct  << "% sl=" << g_cfg.oil_sl_pct  << "% vol=" << g_cfg.oil_vol_thresh_pct << "%\n"
              << "[CONFIG] USTEC pilot only=" << (g_cfg.shadow_ustec_pilot_only ? "true" : "false")
              << " shadow_research=" << (g_cfg.shadow_research_mode ? "true" : "false")
              << " size=" << g_cfg.ustec_pilot_size
              << " min_gap=" << g_cfg.ustec_pilot_min_gap_sec << "s\n"
              << "[CONFIG] latency_cap=" << g_cfg.max_latency_ms << "ms spread_cap=" << g_cfg.max_spread_pct << "%\n";
}

