// tick_oil.hpp — per-symbol tick handlers
// Extracted from on_tick(). Same translation unit — all static functions visible.

// ── USOIL.F ────────────────────────────────────────────────
static void on_tick_oil(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    // Session gate: London/NY only (07:00-22:00 UTC)
    const auto t_cl = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti_cl; gmtime_s(&ti_cl, &t_cl);
    if (ti_cl.tm_hour >= 7 && ti_cl.tm_hour < 22) {
        const bool base_can = symbol_gate("USOIL.F",
            g_eng_cl.pos.active                 ||
            g_ca_eia_fade.has_open_position()   ||  // ADDED
            g_ca_brent_wti.has_open_position(), "", tradeable, lat_ok, regime, bid, ask);    // ADDED
        // NOTE: USOIL.F bracket is disabled -- it was incorrectly sharing
        // g_bracket_gold (XAUUSD's GoldBracketEngine). That engine's confirm_fill,
        // on_reject, and pending order IDs are wired to XAUUSD fills only.
        // An oil bracket position would corrupt XAUUSD bracket state.
        // Oil needs its own dedicated BracketEngine before bracket can be enabled.
        const auto sdec = sup_decision(g_sup_cl, g_eng_cl, base_can, sym, bid, ask);
        if (sdec.allow_breakout)
            dispatch(g_eng_cl, g_sup_cl, base_can);
        // ?? USOIL.F manage blocks -- ALWAYS run when position open (SL/trail fix) ??
        if (g_ca_eia_fade.has_open_position())   { g_ca_eia_fade.on_tick(sym, bid, ask, ca_on_close); }
        if (g_ca_brent_wti.has_open_position())  {
            double brent_b2 = 0.0, brent_a2 = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi2 = g_bids.find("BRENT"); if (bi2 != g_bids.end()) brent_b2 = bi2->second;
              const auto ai2 = g_asks.find("BRENT"); if (ai2 != g_asks.end()) brent_a2 = ai2->second; }
            const double brent_mid2 = (brent_b2 > 0 && brent_a2 > 0) ? (brent_b2+brent_a2)*0.5 : 0.0;
            if (brent_mid2 > 0) g_ca_brent_wti.on_tick_wti(bid, ask, brent_mid2, ca_on_close);
        }
        if (g_nbm_oil_london.has_open_position()) { g_nbm_oil_london.on_tick(sym, bid, ask, ca_on_close); }

        // EIA fade engine -- only when BrentWTI not already open
        if (!g_ca_eia_fade.has_open_position() && !g_ca_brent_wti.has_open_position() && base_can) {  // ADDED !brent check
            const auto ef = g_ca_eia_fade.on_tick(sym, bid, ask, ca_on_close);
            if (ef.valid) { if (!enter_directional(sym.c_str(), ef.is_long, ef.entry, ef.sl, ef.tp, 0.01, false, bid, ask, sym, regime)) g_ca_eia_fade.cancel();
                else g_ca_eia_fade.patch_size(g_last_directional_lot); }
        }
        // Brent/WTI spread engine -- only when EIA not already open
        if (!g_ca_brent_wti.has_open_position() && !g_ca_eia_fade.has_open_position() && base_can) {  // ADDED !eia check
            double brent_b = 0.0, brent_a = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find("BRENT"); if (bi != g_bids.end()) brent_b = bi->second;
              const auto ai = g_asks.find("BRENT"); if (ai != g_asks.end()) brent_a = ai->second; }
            const double brent_mid = (brent_b > 0 && brent_a > 0) ? (brent_b+brent_a)*0.5 : 0.0;
            if (brent_mid > 0) {
                const auto bw = g_ca_brent_wti.on_tick_wti(bid, ask, brent_mid, ca_on_close);
                if (bw.valid) { if (!enter_directional(sym.c_str(), bw.is_long, bw.entry, bw.sl, bw.tp, 0.01, false, bid, ask, sym, regime)) g_ca_brent_wti.cancel();
                else g_ca_brent_wti.patch_size(g_last_directional_lot); }
            }
        }
        // NBM London session (07:00-13:30 UTC) on USOIL.F
        if (!g_nbm_oil_london.has_open_position()
            && !g_ca_eia_fade.has_open_position()
            && !g_ca_brent_wti.has_open_position()
            && base_can) {
            const auto nbm_oil = g_nbm_oil_london.on_tick(sym, bid, ask, ca_on_close);
            if (nbm_oil.valid) {
                g_telemetry.UpdateLastSignal("USOIL.F",
                    nbm_oil.is_long ? "LONG" : "SHORT", nbm_oil.entry,
                    nbm_oil.reason, "NBM_LONDON", regime.c_str(), "NBM_LONDON",
                    nbm_oil.tp, nbm_oil.sl);
                if (!enter_directional("USOIL.F", nbm_oil.is_long, nbm_oil.entry,
                                       nbm_oil.sl, nbm_oil.tp, 0.01, false, bid, ask, sym, regime))
                    g_nbm_oil_london.cancel();
                else g_nbm_oil_london.patch_size(g_last_directional_lot);
            }
        }
    }
}

// ── BRENT ──────────────────────────────────────────────────
static void on_tick_brent(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    const auto t_br = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti_br; gmtime_s(&ti_br, &t_br);
    if (ti_br.tm_hour >= 7) {
        const bool base_can_brent = symbol_gate("BRENT", g_eng_brent.pos.active || g_bracket_brent.pos.active, "", tradeable, lat_ok, regime, bid, ask);
        const auto sdec_brent = sup_decision(g_sup_brent, g_eng_brent, base_can_brent, sym, bid, ask);
        if (sdec_brent.allow_breakout && !g_bracket_brent.pos.active)
            dispatch(g_eng_brent, g_sup_brent, base_can_brent, &sdec_brent);
        // SIM: BracketEngine on commodities -- no edge. Disabled.
        // if (sdec_brent.allow_bracket && !g_eng_brent.pos.active)
        //     dispatch_bracket(g_bracket_brent, ...);
    }
}

