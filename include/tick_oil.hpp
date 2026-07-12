// tick_oil.hpp — per-symbol tick handlers
// File-scope template functions. dispatch is passed as a template parameter
// from on_tick() so the local lambda type is deduced at the call site.

// ── USOIL.F ────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_oil(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // 2026-05-05 (audit-fixes-40): heartbeat pulse for USOIL-driven engine.
    g_engine_heartbeat.pulse("UsoilEngine");

    // Session gate: London/NY only (07:00-22:00 UTC)
    const auto t_cl = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti_cl; gmtime_s(&ti_cl, &t_cl);
    if (ti_cl.tm_hour >= 7 && ti_cl.tm_hour < 22) {
        const bool base_can = symbol_gate("USOIL.F",
            g_eng_cl.pos.active, "", tradeable, lat_ok, regime, bid, ask);
        const auto sdec = sup_decision(g_sup_cl, g_eng_cl, base_can, sym, bid, ask);
        if (sdec.allow_breakout)
            dispatch(g_eng_cl, g_sup_cl, base_can);
    }
}

// ── BRENT ──────────────────────────────────────────────────
template<typename Dispatch>
static void on_tick_brent(
    const std::string& sym, double bid, double ask,
    bool tradeable, bool lat_ok, const std::string& regime,
    Dispatch& dispatch)
{
    // 2026-05-05 (audit-fixes-40): heartbeat pulse for BRENT-driven engine.
    g_engine_heartbeat.pulse("BrentEngine");

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
