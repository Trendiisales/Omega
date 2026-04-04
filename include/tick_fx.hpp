// tick_fx.hpp — per-symbol tick handlers
// File-scope template functions. dispatch is passed as a template parameter
// from on_tick() so the local lambda type is deduced at the call site.

// ── XAGUSD ─────────────────────────────────────────────────
static void on_tick_silver(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime)
{
    // XAGUSD FULLY DISABLED -- real-tick backtest FAILED (Sharpe=-16.23).
    (void)sym; (void)bid; (void)ask; (void)tradeable; (void)lat_ok; (void)regime;
}

// ── EURUSD ─────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_eurusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    g_macro_ctx.eur_mid_price = (bid + ask) * 0.5;
    const bool base_can_fx = symbol_gate("EURUSD",
        g_eng_eurusd.pos.active             ||
        g_bracket_eurusd.pos.active         ||
        g_vwap_rev_eurusd.has_open_position(), "", tradeable, lat_ok, regime, bid, ask);
    const auto sdec_fx = sup_decision(g_sup_eurusd, g_eng_eurusd, base_can_fx, sym, bid, ask);
    {
        static bool s_eur_was_armed = false;
        const bool eur_armed_now = (g_eng_eurusd.phase == omega::Phase::BREAKOUT_WATCH);
        if (s_eur_was_armed && !eur_armed_now && g_eng_eurusd.pos.active)
            g_ca_fx_cascade.notify_eurusd_signal(g_eng_eurusd.pos.is_long);
        s_eur_was_armed = eur_armed_now;
    }
    if (sdec_fx.allow_breakout && !g_bracket_eurusd.pos.active)
        dispatch(g_eng_eurusd, g_sup_eurusd, base_can_fx, &sdec_fx);
    // SIM: BracketEngine on FX -- no edge. 25 trades WR 20% -$935. Disabled.
    if (g_vwap_rev_eurusd.has_open_position()) {
        auto vwap_eur_cb = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == "TP_HIT") g_vwap_rev_eurusd.notify_tp_hit(tr.side == "LONG");
            ca_on_close(tr);
        };
        g_vwap_rev_eurusd.on_tick(sym, bid, ask, 0.0, vwap_eur_cb);
    }
    if (!g_vwap_rev_eurusd.has_open_position() && base_can_fx) {
        static double  s_eur_london_open     = 0.0;
        static int     s_eur_london_open_day = -1;
        {
            const auto t_eur = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            struct tm ti_eur; gmtime_s(&ti_eur, &t_eur);
            const bool in_london = (ti_eur.tm_hour >= 8);
            if (in_london && ti_eur.tm_yday != s_eur_london_open_day) {
                s_eur_london_open     = (bid + ask) * 0.5;
                s_eur_london_open_day = ti_eur.tm_yday;
                g_vwap_rev_eurusd.reset_ewm_vwap(s_eur_london_open);
            }
        }
        if (s_eur_london_open > 0.0) {
            const auto vr = g_vwap_rev_eurusd.on_tick(sym, bid, ask, s_eur_london_open, ca_on_close,
                g_macro_ctx.vix, g_macro_ctx.eur_l2_imbalance);
            if (vr.valid) {
                g_telemetry.UpdateLastSignal("EURUSD", vr.is_long?"LONG":"SHORT", vr.entry, vr.reason, "VWAP_REV", regime.c_str(), "VWAP_REV", vr.tp, vr.sl);
                const double conf_mult = (vr.confluence_score >= 4) ? 3.0 :
                                        (vr.confluence_score == 3) ? 2.0 :
                                        (vr.confluence_score == 2) ? 1.5 : 1.0;
                if (!enter_directional("EURUSD", vr.is_long, vr.entry, vr.sl, vr.tp, 0.01 * conf_mult, true, bid, ask, sym, regime))
                    g_vwap_rev_eurusd.cancel();
                else g_vwap_rev_eurusd.patch_size(g_last_directional_lot);
            }
        }
    }
    if (base_can_fx) {
        const omega::edges::CVDState eur_cvd = g_edges.cvd.get("EURUSD");
        const auto fix_sig = g_edges.fx_fix.on_tick_london("EURUSD", bid, ask, eur_cvd.normalised(), nowSec());
        if (fix_sig.valid) {
            g_telemetry.UpdateLastSignal("EURUSD", fix_sig.is_long?"LONG":"SHORT", fix_sig.entry, fix_sig.reason, "FX_FIX", regime.c_str(), "LONDON_FIX", fix_sig.tp, fix_sig.sl);
            enter_directional("EURUSD", fix_sig.is_long, fix_sig.entry, fix_sig.sl, fix_sig.tp, 0.01, true, bid, ask, sym, regime);
        }
    }
}

// ── GBPUSD ─────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_gbpusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    g_macro_ctx.gbp_mid_price = (bid + ask) * 0.5;
    const bool base_can_fx2 = symbol_gate("GBPUSD",
        g_eng_gbpusd.pos.active                    ||
        g_bracket_gbpusd.pos.active                ||
        g_ca_fx_cascade.has_open_gbpusd(), "", tradeable, lat_ok, regime, bid, ask);
    const auto sdec_fx2 = sup_decision(g_sup_gbpusd, g_eng_gbpusd, base_can_fx2, sym, bid, ask);
    if (sdec_fx2.allow_breakout && !g_bracket_gbpusd.pos.active)
        dispatch(g_eng_gbpusd, g_sup_gbpusd, base_can_fx2, &sdec_fx2);
    // SIM: BracketEngine on FX -- no edge. 26 trades WR 15% -$1272. Disabled.
    if (g_ca_fx_cascade.has_open_gbpusd()) { g_ca_fx_cascade.on_tick_gbpusd(bid, ask, ca_on_close); }
    if (!g_ca_fx_cascade.has_open_gbpusd() && base_can_fx2) {
        const auto cas = g_ca_fx_cascade.on_tick_gbpusd(bid, ask, ca_on_close);
        if (cas.valid) {
            g_telemetry.UpdateLastSignal("GBPUSD", cas.is_long?"LONG":"SHORT", cas.entry, cas.reason, "FX_CASCADE", regime.c_str(), "FX_CASCADE", cas.tp, cas.sl);
            if (!enter_directional("GBPUSD", cas.is_long, cas.entry, cas.sl, cas.tp, 0.01, false, bid, ask, sym, regime))
                g_ca_fx_cascade.cancel_gbpusd();
            else g_ca_fx_cascade.patch_size_gbp(g_last_directional_lot);
        }
    }
}

// ── AUDUSD/NZDUSD/USDJPY ───────────────────────────────────
template<typename Dispatch>
static void on_tick_audusd(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    if (sym == "USDJPY") g_usdjpy_mid.store((bid + ask) * 0.5, std::memory_order_relaxed);
    const bool any_fx_bracket_active =
        g_bracket_gbpusd.has_open_position() ||
        g_bracket_audusd.has_open_position() ||
        g_bracket_nzdusd.has_open_position() ||
        g_bracket_usdjpy.has_open_position();
    const auto t2 = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti2; gmtime_s(&ti2, &t2);
    const int h2 = ti2.tm_hour;
    const bool asia_ok = !g_cfg.asia_fx_asia_only || (h2 >= 22 || h2 < 7);
    if (asia_ok) {
        if (sym == "AUDUSD") {
            const bool bc_aud = symbol_gate("AUDUSD", g_eng_audusd.pos.active || g_bracket_audusd.pos.active, "", tradeable, lat_ok, regime, bid, ask);
            const auto sd_aud = sup_decision(g_sup_audusd, g_eng_audusd, bc_aud, sym, bid, ask);
            if (sd_aud.allow_breakout && !g_bracket_audusd.pos.active) dispatch(g_eng_audusd, g_sup_audusd, bc_aud, &sd_aud);
            // SIM: BracketEngine on FX -- no edge. Disabled.
            // if (sd_aud.allow_bracket && !g_eng_audusd.pos.active && !any_fx_bracket_active)
            //     dispatch_bracket(g_bracket_audusd, ...);
            if (g_ca_fx_cascade.has_open_audusd()) { g_ca_fx_cascade.on_tick_audusd(bid, ask, ca_on_close); }
            if (!g_ca_fx_cascade.has_open_audusd() && bc_aud) {
                const auto cas = g_ca_fx_cascade.on_tick_audusd(bid, ask, ca_on_close);
                if (cas.valid) {
                    g_telemetry.UpdateLastSignal("AUDUSD", cas.is_long?"LONG":"SHORT", cas.entry, cas.reason, "FX_CASCADE", regime.c_str(), "FX_CASCADE", cas.tp, cas.sl);
                    if (!enter_directional("AUDUSD", cas.is_long, cas.entry, cas.sl, cas.tp, 0.01, false, bid, ask, sym, regime))
                        g_ca_fx_cascade.cancel_audusd();
                    else g_ca_fx_cascade.patch_size_aud(g_last_directional_lot);
                }
            }
        }
        if (sym == "NZDUSD") {
            const bool bc_nzd = symbol_gate("NZDUSD", g_eng_nzdusd.pos.active || g_bracket_nzdusd.pos.active, "", tradeable, lat_ok, regime, bid, ask);
            const auto sd_nzd = sup_decision(g_sup_nzdusd, g_eng_nzdusd, bc_nzd, sym, bid, ask);
            if (sd_nzd.allow_breakout && !g_bracket_nzdusd.pos.active) dispatch(g_eng_nzdusd, g_sup_nzdusd, bc_nzd, &sd_nzd);
            // SIM: BracketEngine on FX -- no edge. Disabled.
            if (g_ca_fx_cascade.has_open_nzdusd()) { g_ca_fx_cascade.on_tick_nzdusd(bid, ask, ca_on_close); }
            if (!g_ca_fx_cascade.has_open_nzdusd() && bc_nzd) {
                const auto cas = g_ca_fx_cascade.on_tick_nzdusd(bid, ask, ca_on_close);
                if (cas.valid) {
                    g_telemetry.UpdateLastSignal("NZDUSD", cas.is_long?"LONG":"SHORT", cas.entry, cas.reason, "FX_CASCADE", regime.c_str(), "FX_CASCADE", cas.tp, cas.sl);
                    if (!enter_directional("NZDUSD", cas.is_long, cas.entry, cas.sl, cas.tp, 0.01, false, bid, ask, sym, regime))
                        g_ca_fx_cascade.cancel_nzdusd();
                    else g_ca_fx_cascade.patch_size_nzd(g_last_directional_lot);
                }
            }
        }
        if (sym == "USDJPY") {
            const bool bc_jpy = symbol_gate("USDJPY", g_eng_usdjpy.pos.active || g_bracket_usdjpy.pos.active, "", tradeable, lat_ok, regime, bid, ask);
            const auto sd_jpy = sup_decision(g_sup_usdjpy, g_eng_usdjpy, bc_jpy, sym, bid, ask);
            if (sd_jpy.allow_breakout && !g_bracket_usdjpy.pos.active) dispatch(g_eng_usdjpy, g_sup_usdjpy, bc_jpy, &sd_jpy);
            // SIM: BracketEngine on FX -- no edge. Disabled.
            if (g_ca_carry_unwind.has_open_position()) { g_ca_carry_unwind.on_tick(bid, ask, g_macro_ctx.vix, ca_on_close); }
            if (!g_ca_carry_unwind.has_open_position() && bc_jpy) {
                const auto cu = g_ca_carry_unwind.on_tick(bid, ask, g_macro_ctx.vix, ca_on_close);
                if (cu.valid) {
                    g_telemetry.UpdateLastSignal("USDJPY", cu.is_long?"LONG":"SHORT", cu.entry, cu.reason, "CARRY_UNWIND", regime.c_str(), "CARRY_UNWIND", cu.tp, cu.sl);
                    if (!enter_directional("USDJPY", cu.is_long, cu.entry, cu.sl, cu.tp, 0.01, true, bid, ask, sym, regime))
                        g_ca_carry_unwind.cancel();
                    else g_ca_carry_unwind.patch_size(g_last_directional_lot);
                }
            }
            if (bc_jpy) {
                const omega::edges::CVDState jpy_cvd = g_edges.cvd.get("USDJPY");
                const auto fix_sig = g_edges.fx_fix.on_tick_tokyo(bid, ask, jpy_cvd.normalised(), nowSec());
                if (fix_sig.valid) {
                    g_telemetry.UpdateLastSignal("USDJPY", fix_sig.is_long?"LONG":"SHORT", fix_sig.entry, fix_sig.reason, "FX_FIX", regime.c_str(), "TOKYO_FIX", fix_sig.tp, fix_sig.sl);
                    enter_directional("USDJPY", fix_sig.is_long, fix_sig.entry, fix_sig.sl, fix_sig.tp, 0.01, true, bid, ask, sym, regime);
                }
            }
        }
        (void)any_fx_bracket_active;
    }
}
