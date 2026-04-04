// tick_indices.hpp — per-symbol tick handlers
// Extracted from main.cpp on_tick(). Same translation unit -- all globals visible.
// #included into main.cpp before on_tick(). Do NOT include elsewhere.
// IMMUTABLE: never modify core logic here without explicit instruction.

// ──────────────────────────────────────────────────────────────────────
// US500.F tick handler
// ──────────────────────────────────────────────────────────────────────
static void on_tick_us500(const std::string& sym, double bid, double ask) {
    // FIX-tick bar builder for US500.F M1/M5
    {
        static OHLCBar s_sp1{}, s_sp5{};
        static int64_t s_sp1_start = 0, s_sp5_start = 0;
        const double sp_mid = (bid + ask) * 0.5;
        const int64_t now_ms_s = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const int64_t b1 = (now_ms_s /  60000LL) *  60000LL;
        const int64_t b5 = (now_ms_s / 300000LL) * 300000LL;
        if (s_sp1_start == 0) { s_sp1 = {b1/60000LL,sp_mid,sp_mid,sp_mid,sp_mid}; s_sp1_start = b1; }
        else if (b1 != s_sp1_start) { g_bars_sp.m1.add_bar(s_sp1); s_sp1 = {b1/60000LL,sp_mid,sp_mid,sp_mid,sp_mid}; s_sp1_start = b1; }
        else { if(sp_mid>s_sp1.high)s_sp1.high=sp_mid; if(sp_mid<s_sp1.low)s_sp1.low=sp_mid; s_sp1.close=sp_mid; }
        if (s_sp5_start == 0) { s_sp5 = {b5/60000LL,sp_mid,sp_mid,sp_mid,sp_mid}; s_sp5_start = b5; }
        else if (b5 != s_sp5_start) { g_bars_sp.m5.add_bar(s_sp5); s_sp5 = {b5/60000LL,sp_mid,sp_mid,sp_mid,sp_mid}; s_sp5_start = b5; }
        else { if(sp_mid>s_sp5.high)s_sp5.high=sp_mid; if(sp_mid<s_sp5.low)s_sp5.low=sp_mid; s_sp5.close=sp_mid; }
    }
    const bool base_can_sp = symbol_gate("US500.F",
        g_eng_sp.pos.active          ||
        g_bracket_sp.pos.active      ||
        g_orb_us.has_open_position() ||
        g_vwap_rev_sp.has_open_position()  ||
        g_trend_pb_sp.has_open_position()  ||  // TrendPullback SP
        g_nbm_sp.has_open_position())
        // ?? Indices circuit breaker: block new entries for 30min after any US index FORCE_CLOSE
        && (static_cast<int64_t>(std::time(nullptr)) >= g_indices_disconnect_until.load());
    // Log when circuit breaker is blocking -- once every 60s so it's visible but not spammy
    {
        const int64_t now_cb = static_cast<int64_t>(std::time(nullptr));
        const int64_t until_cb = g_indices_disconnect_until.load();
        static int64_t s_cb_log_sp = 0;
        if (until_cb > now_cb && now_cb - s_cb_log_sp >= 60) {
            s_cb_log_sp = now_cb;
            printf("[INDICES-CB] US index entries BLOCKED -- %llds remaining (disconnect cooldown)\n",
                   (long long)(until_cb - now_cb));
            fflush(stdout);
        }
    }
    const auto sdec_sp = sup_decision(g_sup_sp, g_eng_sp, base_can_sp);
    // SIM: SP breakout WR 31.6% -$105. No edge on US500 compression breakout. Disabled.
    // if (sdec_sp.allow_breakout && !g_bracket_sp.pos.active)
    //     dispatch(g_eng_sp, g_sup_sp, base_can_sp, &sdec_sp);
    // SIM: BracketEngine on indices -- no edge. Disabled.
    // if (sdec_sp.allow_bracket && !g_eng_sp.pos.active)
    //     dispatch_bracket(g_bracket_sp, ...);
    // Cross-asset: ES/NQ divergence engine.
    // enabled=false by default -- set esnq_enabled=true in [cross_asset] config once
    // shadow validates signal quality. on_tick() always drains open positions even
    // when disabled; new entries only fire when enabled=true and 3-tick confirmation passes.
    {
        const auto esnq = g_ca_esnq.on_tick(sym, bid, ask, g_macro_ctx.es_nq_div, ca_on_close);
        if (esnq.valid && base_can_sp) {
            g_telemetry.UpdateLastSignal(sym.c_str(), esnq.is_long?"LONG":"SHORT",
                esnq.entry, esnq.reason, "ESNQ_DIV", regime.c_str(), "ESNQ_DIV",
                esnq.tp, esnq.sl);
            if (!enter_directional(sym.c_str(), esnq.is_long, esnq.entry, esnq.sl, esnq.tp))
                g_ca_esnq.cancel();
                else g_ca_esnq.patch_size(g_last_directional_lot);
        }
    }
    // ?? US500.F manage blocks -- ALWAYS run when position open (SL/trail fix) ??
    if (g_orb_us.has_open_position())       { g_orb_us.on_tick(sym, bid, ask, ca_on_close); }
    if (g_vwap_rev_sp.has_open_position())  {
        auto vwap_sp_cb = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == "TP_HIT") g_vwap_rev_sp.notify_tp_hit(tr.side == "LONG");
            ca_on_close(tr);
        };
        g_vwap_rev_sp.on_tick(sym, bid, ask, 0.0, vwap_sp_cb);
    }
    if (g_nbm_sp.has_open_position())       { g_nbm_sp.on_tick(sym, bid, ask, ca_on_close); }
    // Seed TrendPB with real M1 bar EMAs from cTrader trendbar API
    if (g_bars_sp.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
        g_trend_pb_sp.seed_bar_emas(
            g_bars_sp.m1.ind.ema9.load(std::memory_order_relaxed),
            g_bars_sp.m1.ind.ema21.load(std::memory_order_relaxed),
            g_bars_sp.m1.ind.ema50.load(std::memory_order_relaxed),
            g_bars_sp.m1.ind.atr14.load(std::memory_order_relaxed));
        {   // FIX: M1 EMA crossover replaces M5 swing trend_state (15min lag)
            const double sp_s_e9  = g_bars_sp.m1.ind.ema9 .load(std::memory_order_relaxed);
            const double sp_s_e50 = g_bars_sp.m1.ind.ema50.load(std::memory_order_relaxed);
            const int sp_ema_trend = (sp_s_e9 > 0.0 && sp_s_e50 > 0.0)
                ? (sp_s_e9 < sp_s_e50 ? -1 : +1) : 0;
            g_trend_pb_sp.seed_m5_trend(sp_ema_trend);
        }
    }
    if (g_trend_pb_sp.has_open_position())  { g_trend_pb_sp.on_tick(sym, bid, ask, ca_on_close); }

    if (!g_orb_us.has_open_position() && !g_vwap_rev_sp.has_open_position() && base_can_sp) {  // ADDED !vwap check
        const auto orb = g_orb_us.on_tick(sym, bid, ask, ca_on_close);
        if (orb.valid) {
            g_telemetry.UpdateLastSignal("US500.F", orb.is_long?"LONG":"SHORT", orb.entry, orb.reason, "ORB", regime.c_str(), "ORB", orb.tp, orb.sl);
            if (!enter_directional("US500.F", orb.is_long, orb.entry, orb.sl, orb.tp))
                g_orb_us.cancel();
                else g_orb_us.patch_size(g_last_directional_lot);
        }
    }
    // VWAP Reversion: enter when price reverses back toward daily VWAP after over-extension.
    // Reference: first tick of each UTC calendar day as VWAP proxy.
    // Previously used ORB range midpoint ? dead before 13:30 UTC (entire London session blocked).
    // Daily-open anchor matches EURUSD approach and covers full 07:00-22:00 session.
    if (!g_vwap_rev_sp.has_open_position() && !g_orb_us.has_open_position() && base_can_sp) {  // ADDED !orb check
        // ?? SP VWAP anchor: NY session open (13:30 UTC) ???????????????????
        // Using NY open not midnight UTC -- NQ/SP have almost no volume at
        // midnight UTC (NZ midday). NY open is the correct liquidity anchor.
        // Each UTC day the anchor resets at 13:30 UTC (first tick after NY open).
        static double  s_sp_ny_open      = 0.0;
        static int     s_sp_ny_open_day  = -1;
        static bool    s_sp_ny_armed     = false;
        {
            const auto t_sp = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            struct tm ti_sp; gmtime_s(&ti_sp, &t_sp);
            const int hm = ti_sp.tm_hour * 60 + ti_sp.tm_min;
            const bool in_ny_open = (hm >= 13*60+30);
            if (in_ny_open && ti_sp.tm_yday != s_sp_ny_open_day) {
                s_sp_ny_open     = (bid + ask) * 0.5;
                s_sp_ny_open_day = ti_sp.tm_yday;
                s_sp_ny_armed    = true;
                g_vwap_rev_sp.reset_ewm_vwap(s_sp_ny_open);  // re-anchor EWM
            }
        }
        // ?? NY open settle gate -- block VWAPRev for 30min after NY open ??
        // NY open (13:30 UTC) creates momentum window where mean reversion fails.
        // Evidence 2026-04-03 (UTC): VWAPRev entered at 13:31 into a 200pt NQ
        // momentum move and FORCE_CLOSEd at -$55, -$38, -$44, -$49.
        // Block entries until 14:00 UTC (first 30min = pure momentum, not reversion).
        static int64_t s_sp_ny_armed_ts = 0;
        if (s_sp_ny_armed && s_sp_ny_open > 0.0 && s_sp_ny_armed_ts == 0)
            s_sp_ny_armed_ts = static_cast<int64_t>(std::time(nullptr));
        const bool sp_ny_settled = (s_sp_ny_armed_ts > 0)
            && (static_cast<int64_t>(std::time(nullptr)) - s_sp_ny_armed_ts >= 30 * 60);
        if (s_sp_ny_armed && s_sp_ny_open > 0.0 && sp_ny_settled) {
            // ?? Spread anomaly gate for VWAPReversion ????????????????????
            // VWAPReversion is a mean-reversion strategy -- it needs settled price.
            // Wide spread = news/thin liquidity = reversion target unreliable.
            // Track 100-tick rolling avg spread for this symbol.
            static std::deque<double> s_vwap_sp_spread_hist;
            {
                s_vwap_sp_spread_hist.push_back(ask - bid);
                if (s_vwap_sp_spread_hist.size() > 100) s_vwap_sp_spread_hist.pop_front();
            }
            double sp_spread_avg = 0.0;
            for (double sv : s_vwap_sp_spread_hist) sp_spread_avg += sv;
            if (!s_vwap_sp_spread_hist.empty()) sp_spread_avg /= s_vwap_sp_spread_hist.size();
            const bool sp_spread_ok = (sp_spread_avg < 0.5)
                                    || ((ask - bid) <= sp_spread_avg * 1.8);
            if (sp_spread_ok) {
            const auto vr = g_vwap_rev_sp.on_tick(sym, bid, ask, s_sp_ny_open, ca_on_close,
                g_macro_ctx.vix, g_macro_ctx.sp_l2_imbalance);
            if (vr.valid) {
                g_telemetry.UpdateLastSignal("US500.F", vr.is_long?"LONG":"SHORT", vr.entry, vr.reason, "VWAP_REV", regime.c_str(), "VWAP_REV", vr.tp, vr.sl);
                // Confluence size scaling: score 1=1? 2=1.5? 3=2? 4=3?
                const double conf_mult = (vr.confluence_score >= 4) ? 3.0 :
                                        (vr.confluence_score == 3) ? 2.0 :
                                        (vr.confluence_score == 2) ? 1.5 : 1.0;
                if (!enter_directional("US500.F", vr.is_long, vr.entry, vr.sl, vr.tp, 0.01 * conf_mult, true))
                    g_vwap_rev_sp.cancel();
                else g_vwap_rev_sp.patch_size(g_last_directional_lot);
            }
            } // sp_spread_ok
        }
    }
    // NoiseBandMomentum: Zarattini/Maroy intraday momentum (Sharpe 3.0-5.9).
    // Fires when price breaks out of rolling ATR noise band from session open.
    // Primary stop: VWAP crossing. Gated: no other US500.F position open.
    // Asia gate: mirrors TrendPB gate -- NBM needs real US equity session liquidity.
    // Block in slot 6 (Asia), slot 0 (dead zone 05-07 UTC), slot 5 (NY late thin tape)
    // unless M1 bars are seeded AND M5 shows a confirmed trend (same guard as TrendPB).
    {
        const bool sp_nbm_offhours = (g_macro_ctx.session_slot == 6 ||
                                      g_macro_ctx.session_slot == 0 ||
                                      g_macro_ctx.session_slot == 5);
        const bool sp_nbm_bars_ok  = g_bars_sp.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const double sp_nbm_ema9   = g_bars_sp.m1.ind.ema9 .load(std::memory_order_relaxed);
        const double sp_nbm_ema50  = g_bars_sp.m1.ind.ema50.load(std::memory_order_relaxed);
        const int  sp_nbm_m5_trend = (sp_nbm_ema9 > 0.0 && sp_nbm_ema50 > 0.0)
            ? (sp_nbm_ema9 < sp_nbm_ema50 ? -1 : +1) : 0;  // M1 EMA crossover
        const bool sp_nbm_gate_ok  = !sp_nbm_offhours || (sp_nbm_bars_ok && sp_nbm_m5_trend != 0);
        if (!g_nbm_sp.has_open_position() && !g_orb_us.has_open_position() &&
            !g_vwap_rev_sp.has_open_position() && base_can_sp && sp_nbm_gate_ok) {
            const auto nbm = g_nbm_sp.on_tick(sym, bid, ask, ca_on_close);
            if (nbm.valid) {
                g_telemetry.UpdateLastSignal("US500.F", nbm.is_long?"LONG":"SHORT", nbm.entry,
                    nbm.reason, "NBM", regime.c_str(), "NoiseBandMomentum", nbm.tp, nbm.sl);
                if (!enter_directional("US500.F", nbm.is_long, nbm.entry, nbm.sl, nbm.tp))
                    g_nbm_sp.cancel();
                else g_nbm_sp.patch_size(g_last_directional_lot);
            }
        }
    }
    // TrendPullback: EMA9/21/50 stack grind trades -- no timeout, ATR trail.
    // Catches slow trends that VWAPReversion times out on.
    // Only fires when all other US500.F positions are flat.
    // Asia gate: block during Asia/dead-zone unless M5 trend confirmed + bars seeded.
    {
        const bool sp_in_offhours = (g_macro_ctx.session_slot == 6 || g_macro_ctx.session_slot == 0 || g_macro_ctx.session_slot == 5); // slot5=NY late thin tape
        const bool sp_bars_ready  = g_bars_sp.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const bool sp_ema_live    = g_bars_sp.m1.ind.m1_ema_live.load(std::memory_order_relaxed);
        const double sp_tpb_ema9  = sp_ema_live ? g_bars_sp.m1.ind.ema9 .load(std::memory_order_relaxed) : 0.0;
        const double sp_tpb_ema50 = sp_ema_live ? g_bars_sp.m1.ind.ema50.load(std::memory_order_relaxed) : 0.0;
        const int  sp_m5_trend    = (sp_tpb_ema9 > 0.0 && sp_tpb_ema50 > 0.0)
            ? (sp_tpb_ema9 < sp_tpb_ema50 ? -1 : +1) : 0;  // M1 EMA crossover
        // m1_ema_live required always: prevents stale disk EMA firing on restart
        const bool sp_trendpb_ok  = sp_ema_live
            && (!sp_in_offhours || (sp_bars_ready && sp_m5_trend != 0));
        if (!g_trend_pb_sp.has_open_position() && !g_vwap_rev_sp.has_open_position()
            && !g_nbm_sp.has_open_position() && base_can_sp && sp_trendpb_ok) {
            const auto tp_sig = g_trend_pb_sp.on_tick(sym, bid, ask, ca_on_close);
            if (tp_sig.valid) {
                g_telemetry.UpdateLastSignal("US500.F", tp_sig.is_long?"LONG":"SHORT",
                    tp_sig.entry, tp_sig.reason, "TREND_PB", regime.c_str(), "TREND_PB",
                    tp_sig.tp, tp_sig.sl);
                if (!enter_directional("US500.F", tp_sig.is_long, tp_sig.entry,
                                       tp_sig.sl, tp_sig.tp, 0.01, true))
                    g_trend_pb_sp.cancel();
                // patch_size removed -- enter_directional already sized correctly
                // patch_size(g_last_directional_lot) would corrupt size with last ANY-symbol lot
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// USTEC.F tick handler
// ──────────────────────────────────────────────────────────────────────
static void on_tick_ustec(const std::string& sym, double bid, double ask) {
    // FIX-tick bar builder for USTEC.F M1/M5
    {
        static OHLCBar s_nq1{}, s_nq5{};
        static int64_t s_nq1_start = 0, s_nq5_start = 0;
        const double nq_mid = (bid + ask) * 0.5;
        const int64_t now_ms_n = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const int64_t b1 = (now_ms_n /  60000LL) *  60000LL;
        const int64_t b5 = (now_ms_n / 300000LL) * 300000LL;
        if (s_nq1_start == 0) { s_nq1 = {b1/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nq1_start = b1; }
        else if (b1 != s_nq1_start) { g_bars_nq.m1.add_bar(s_nq1); s_nq1 = {b1/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nq1_start = b1; }
        else { if(nq_mid>s_nq1.high)s_nq1.high=nq_mid; if(nq_mid<s_nq1.low)s_nq1.low=nq_mid; s_nq1.close=nq_mid; }
        if (s_nq5_start == 0) { s_nq5 = {b5/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nq5_start = b5; }
        else if (b5 != s_nq5_start) { g_bars_nq.m5.add_bar(s_nq5); s_nq5 = {b5/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nq5_start = b5; }
        else { if(nq_mid>s_nq5.high)s_nq5.high=nq_mid; if(nq_mid<s_nq5.low)s_nq5.low=nq_mid; s_nq5.close=nq_mid; }
    }
    const bool base_can_nq = symbol_gate("USTEC.F",
        g_eng_nq.pos.active                  ||
        g_bracket_nq.pos.active              ||
        g_vwap_rev_nq.has_open_position()    ||
        g_trend_pb_nq.has_open_position()    ||  // TrendPullback NQ
        g_nbm_nq.has_open_position())             // NBM
        // ?? Indices circuit breaker: block new entries for 30min after any US index FORCE_CLOSE
        && (static_cast<int64_t>(std::time(nullptr)) >= g_indices_disconnect_until.load());
    {
        const int64_t now_cb   = static_cast<int64_t>(std::time(nullptr));
        const int64_t until_cb = g_indices_disconnect_until.load();
        static int64_t s_cb_log_nq = 0;
        if (until_cb > now_cb && now_cb - s_cb_log_nq >= 60) {
            s_cb_log_nq = now_cb;
            printf("[INDICES-CB] USTEC.F entries BLOCKED -- %llds remaining (disconnect cooldown)\n",
                   (long long)(until_cb - now_cb));
            fflush(stdout);
        }
    }
    const auto sdec_nq = sup_decision(g_sup_nq, g_eng_nq, base_can_nq);
    // SIM: NQ breakout WR 26.1% -$1167. Worst index performer. Disabled.
    // if (sdec_nq.allow_breakout && !g_bracket_nq.pos.active)
    //     dispatch(g_eng_nq, g_sup_nq, base_can_nq, &sdec_nq);
    // SIM: BracketEngine on indices -- no edge. Disabled.
    // const bool nas100_bracket_open = g_bracket_nas100.has_open_position();
    // if (sdec_nq.allow_bracket && !g_eng_nq.pos.active && !nas100_bracket_open)
    //     dispatch_bracket(g_bracket_nq, ...);
    // ?? USTEC.F manage blocks -- ALWAYS run when position open (SL/trail fix) ??
    if (g_vwap_rev_nq.has_open_position()) {
        auto vwap_nq_cb = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == "TP_HIT") g_vwap_rev_nq.notify_tp_hit(tr.side == "LONG");
            ca_on_close(tr);
        };
        g_vwap_rev_nq.on_tick(sym, bid, ask, 0.0, vwap_nq_cb);
    }
    // Seed TrendPB NQ with real M1 bar EMAs
    if (g_bars_nq.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
        g_trend_pb_nq.seed_bar_emas(
            g_bars_nq.m1.ind.ema9.load(std::memory_order_relaxed),
            g_bars_nq.m1.ind.ema21.load(std::memory_order_relaxed),
            g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed),
            g_bars_nq.m1.ind.atr14.load(std::memory_order_relaxed));
        {   // FIX: M1 EMA crossover replaces M5 swing trend_state (15min lag)
            const double nq_s_e9  = g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed);
            const double nq_s_e50 = g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed);
            const int nq_ema_trend = (nq_s_e9 > 0.0 && nq_s_e50 > 0.0)
                ? (nq_s_e9 < nq_s_e50 ? -1 : +1) : 0;
            g_trend_pb_nq.seed_m5_trend(nq_ema_trend);
        }
    }
    if (g_trend_pb_nq.has_open_position()) { g_trend_pb_nq.on_tick(sym, bid, ask, ca_on_close); }
    if (g_nbm_nq.has_open_position())      { g_nbm_nq.on_tick(sym, bid, ask, ca_on_close); }

    // VWAP Reversion: NQ -- anchored to NY open (13:30 UTC), not midnight.
    // NQ barely trades at midnight UTC (NZ midday). NY open is the correct anchor.
    if (!g_vwap_rev_nq.has_open_position() && base_can_nq) {
        static double  s_nq_ny_open      = 0.0;
        static int     s_nq_ny_open_day  = -1;
        static bool    s_nq_ny_armed     = false;
        {
            const auto t_nq = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            struct tm ti_nq; gmtime_s(&ti_nq, &t_nq);
            const int hm = ti_nq.tm_hour * 60 + ti_nq.tm_min;
            if (hm >= 13*60+30 && ti_nq.tm_yday != s_nq_ny_open_day) {
                s_nq_ny_open     = (bid + ask) * 0.5;
                s_nq_ny_open_day = ti_nq.tm_yday;
                s_nq_ny_armed    = true;
                g_vwap_rev_nq.reset_ewm_vwap(s_nq_ny_open);
            }
        }
        // ?? NY open settle gate (NQ) -- same logic as SP ??????????????????
        static int64_t s_nq_ny_armed_ts = 0;
        if (s_nq_ny_armed && s_nq_ny_open > 0.0 && s_nq_ny_armed_ts == 0)
            s_nq_ny_armed_ts = static_cast<int64_t>(std::time(nullptr));
        const bool nq_ny_settled = (s_nq_ny_armed_ts > 0)
            && (static_cast<int64_t>(std::time(nullptr)) - s_nq_ny_armed_ts >= 30 * 60);
        if (s_nq_ny_armed && s_nq_ny_open > 0.0 && nq_ny_settled) {
            // ?? Spread anomaly gate for VWAPReversion ????????????????????
            static std::deque<double> s_vwap_nq_spread_hist;
            {
                s_vwap_nq_spread_hist.push_back(ask - bid);
                if (s_vwap_nq_spread_hist.size() > 100) s_vwap_nq_spread_hist.pop_front();
            }
            double nq_spread_avg = 0.0;
            for (double sv : s_vwap_nq_spread_hist) nq_spread_avg += sv;
            if (!s_vwap_nq_spread_hist.empty()) nq_spread_avg /= s_vwap_nq_spread_hist.size();
            const bool nq_spread_ok = (nq_spread_avg < 0.5)
                                    || ((ask - bid) <= nq_spread_avg * 1.8);
            if (nq_spread_ok) {
            const auto vr = g_vwap_rev_nq.on_tick(sym, bid, ask, s_nq_ny_open, ca_on_close,
                g_macro_ctx.vix, g_macro_ctx.nq_l2_imbalance);
            if (vr.valid) {
                g_telemetry.UpdateLastSignal("USTEC.F", vr.is_long?"LONG":"SHORT", vr.entry, vr.reason, "VWAP_REV", regime.c_str(), "VWAP_REV", vr.tp, vr.sl);
                const double conf_mult = (vr.confluence_score >= 4) ? 3.0 :
                                        (vr.confluence_score == 3) ? 2.0 :
                                        (vr.confluence_score == 2) ? 1.5 : 1.0;
                if (!enter_directional("USTEC.F", vr.is_long, vr.entry, vr.sl, vr.tp, 0.01 * conf_mult, true))
                    g_vwap_rev_nq.cancel();
                else g_vwap_rev_nq.patch_size(g_last_directional_lot);
            }
            } // nq_spread_ok
        }
    }
    // TrendPullback: EMA9/21/50 stack grind trades -- no timeout, ATR trail.
    // Catches slow trends that VWAPReversion times out on.
    // Only fires when VWAP position is flat -- they share the same direction thesis.
    //
    // Asia/dead-zone gate: block during Asia (slot=6, 22-05 UTC) and dead zone
    // (slot=0, 05-07 UTC) unless M5 trend is clearly established AND M1 bars seeded.
    // Without real bar EMAs the engine uses tick-based EMAs that are meaningless
    // at Asia open -- produces 0-second SL hits from spread noise.
    {
        const int slot_nq = g_macro_ctx.session_slot;
        const bool nq_in_offhours = (slot_nq == 6 || slot_nq == 0 || slot_nq == 5);
        // slot 5 = NY late (17:00-22:00 UTC): US indices thin after NY close.
        // Same gate as Asia -- only trade if M1 bars seeded AND M5 trend confirmed.
        const bool nq_bars_ready  = g_bars_nq.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const bool nq_ema_live    = g_bars_nq.m1.ind.m1_ema_live.load(std::memory_order_relaxed);
        const double nq_tpb_ema9  = nq_ema_live ? g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed) : 0.0;
        const double nq_tpb_ema50 = nq_ema_live ? g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed) : 0.0;
        const int  nq_m5_trend    = (nq_tpb_ema9 > 0.0 && nq_tpb_ema50 > 0.0)
            ? (nq_tpb_ema9 < nq_tpb_ema50 ? -1 : +1) : 0;  // M1 EMA crossover
        // m1_ema_live required always: prevents stale disk EMA firing on restart
        const bool nq_trendpb_ok  = nq_ema_live
            && (!nq_in_offhours || (nq_bars_ready && nq_m5_trend != 0));

        if (!g_trend_pb_nq.has_open_position() && !g_vwap_rev_nq.has_open_position()
            && !g_nbm_nq.has_open_position() && base_can_nq && nq_trendpb_ok) {
            const auto tp_sig = g_trend_pb_nq.on_tick(sym, bid, ask, ca_on_close);
            if (tp_sig.valid) {
                g_telemetry.UpdateLastSignal("USTEC.F", tp_sig.is_long?"LONG":"SHORT",
                    tp_sig.entry, tp_sig.reason, "TREND_PB", regime.c_str(), "TREND_PB",
                    tp_sig.tp, tp_sig.sl);
                if (!enter_directional("USTEC.F", tp_sig.is_long, tp_sig.entry,
                                       tp_sig.sl, tp_sig.tp, 0.01, true))
                    g_trend_pb_nq.cancel();
                // patch_size removed -- enter_directional already sized correctly
            }
        }
    }
    // NoiseBandMomentum: Zarattini/Maroy intraday momentum (Sharpe 3.0-5.9).
    // Same Asia gate -- NBM needs real momentum, not Asia drift noise.
    {
        const int slot_nq2 = g_macro_ctx.session_slot;
        const bool nq_in_offhours2 = (slot_nq2 == 6 || slot_nq2 == 0 || slot_nq2 == 5); // slot5=NY late thin tape
        const bool nq_bars_ready2  = g_bars_nq.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const double nq_nbm_ema9   = g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed);
        const double nq_nbm_ema50  = g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed);
        const int  nq_m5_trend2    = (nq_nbm_ema9 > 0.0 && nq_nbm_ema50 > 0.0)
            ? (nq_nbm_ema9 < nq_nbm_ema50 ? -1 : +1) : 0;  // M1 EMA crossover
        const bool nq_nbm_ok       = !nq_in_offhours2 || (nq_bars_ready2 && nq_m5_trend2 != 0);

        if (!g_nbm_nq.has_open_position() && !g_vwap_rev_nq.has_open_position()
            && base_can_nq && nq_nbm_ok) {
            const auto nbm = g_nbm_nq.on_tick(sym, bid, ask, ca_on_close);
            if (nbm.valid) {
                g_telemetry.UpdateLastSignal("USTEC.F", nbm.is_long?"LONG":"SHORT", nbm.entry,
                    nbm.reason, "NBM", regime.c_str(), "NoiseBandMomentum", nbm.tp, nbm.sl);
                if (!enter_directional("USTEC.F", nbm.is_long, nbm.entry, nbm.sl, nbm.tp))
                    g_nbm_nq.cancel();
                else g_nbm_nq.patch_size(g_last_directional_lot);
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// DJ30.F tick handler
// ──────────────────────────────────────────────────────────────────────
static void on_tick_dj30(const std::string& sym, double bid, double ask) {
    const bool base_can_us30 = symbol_gate("DJ30.F",
        g_eng_us30.pos.active      ||
        g_bracket_us30.pos.active  ||
        g_nbm_us30.has_open_position()) // NBM
        // ?? Indices circuit breaker: block new entries for 30min after any US index FORCE_CLOSE
        && (static_cast<int64_t>(std::time(nullptr)) >= g_indices_disconnect_until.load());
    {
        const int64_t now_cb   = static_cast<int64_t>(std::time(nullptr));
        const int64_t until_cb = g_indices_disconnect_until.load();
        static int64_t s_cb_log_us30 = 0;
        if (until_cb > now_cb && now_cb - s_cb_log_us30 >= 60) {
            s_cb_log_us30 = now_cb;
            printf("[INDICES-CB] DJ30.F entries BLOCKED -- %llds remaining (disconnect cooldown)\n",
                   (long long)(until_cb - now_cb));
            fflush(stdout);
        }
    }
    const auto sdec_us30 = sup_decision(g_sup_us30, g_eng_us30, base_can_us30);
    // SIM: DJ30 breakout WR 23.5% -$736, bracket also negative. Both disabled.
    // if (sdec_us30.allow_breakout && !g_bracket_us30.pos.active)
    //     dispatch(g_eng_us30, g_sup_us30, base_can_us30, &sdec_us30);
    // if (sdec_us30.allow_bracket && !g_eng_us30.pos.active)
    //     dispatch_bracket(g_bracket_us30, ...);
    (void)sdec_us30;
    // ?? DJ30.F manage block -- ALWAYS run when position open (SL/trail fix) ??
    if (g_nbm_us30.has_open_position()) { g_nbm_us30.on_tick(sym, bid, ask, ca_on_close); }

    // NoiseBandMomentum: Zarattini/Maroy intraday momentum (Sharpe 3.0-5.9).
    // Asia gate: DJ30 is a US instrument -- no edge in Asia thin liquidity.
    // Only trade during London(1), core(2), overlap(3), NY open(4), NY late(5).
    {
        const int slot_us30 = g_macro_ctx.session_slot;
        const bool us30_session_ok = (slot_us30 >= 1 && slot_us30 <= 5);
        if (!g_nbm_us30.has_open_position() && base_can_us30 && us30_session_ok) {
            const auto nbm = g_nbm_us30.on_tick(sym, bid, ask, ca_on_close);
            if (nbm.valid) {
                g_telemetry.UpdateLastSignal("DJ30.F", nbm.is_long?"LONG":"SHORT", nbm.entry,
                    nbm.reason, "NBM", regime.c_str(), "NoiseBandMomentum", nbm.tp, nbm.sl);
                if (!enter_directional("DJ30.F", nbm.is_long, nbm.entry, nbm.sl, nbm.tp))
                    g_nbm_us30.cancel();
                else g_nbm_us30.patch_size(g_last_directional_lot);
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// GER40 tick handler
// ──────────────────────────────────────────────────────────────────────
static void on_tick_ger40(const std::string& sym, double bid, double ask) {
    const bool base_can_ger = symbol_gate("GER40",
        g_eng_ger30.pos.active              ||
        g_bracket_ger30.pos.active          ||
        g_orb_ger30.has_open_position()     ||  // ADDED
        g_vwap_rev_ger40.has_open_position() || // ADDED
        g_trend_pb_ger40.has_open_position());  // ADDED
    const auto sdec_ger = sup_decision(g_sup_ger30, g_eng_ger30, base_can_ger);
    // ?? GER40 manage blocks -- ALWAYS run when position open (SL/trail fix) ??
    if (g_orb_ger30.has_open_position())      { g_orb_ger30.on_tick(sym, bid, ask, ca_on_close); }
    if (g_vwap_rev_ger40.has_open_position()) {
        const double ger_vwap_mgmt = (g_orb_ger30.range_high() + g_orb_ger30.range_low()) > 0.0
            ? (g_orb_ger30.range_high() + g_orb_ger30.range_low()) * 0.5 : 0.0;
        auto vwap_ger_cb = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == "TP_HIT") g_vwap_rev_ger40.notify_tp_hit(tr.side == "LONG");
            ca_on_close(tr);
        };
        g_vwap_rev_ger40.on_tick(sym, bid, ask, ger_vwap_mgmt, vwap_ger_cb);
    }
    if (g_trend_pb_ger40.has_open_position()) { g_trend_pb_ger40.on_tick(sym, bid, ask, ca_on_close); }

    // GER40 NEW ENTRIES DISABLED -- taken out of play
    (void)sdec_ger;
}

// ──────────────────────────────────────────────────────────────────────
// UK100 tick handler
// ──────────────────────────────────────────────────────────────────────
static void on_tick_uk100(const std::string& sym, double bid, double ask) {
    const bool base_can_uk = symbol_gate("UK100", g_eng_uk100.pos.active || g_bracket_uk100.pos.active);
    const auto sdec_uk = sup_decision(g_sup_uk100, g_eng_uk100, base_can_uk);
    // SIM: EU index breakout -- no edge. Disabled.
    // if (sdec_uk.allow_breakout && !g_bracket_uk100.pos.active)
    //     dispatch(g_eng_uk100, g_sup_uk100, base_can_uk, &sdec_uk);
    // SIM: BracketEngine on indices -- no edge. Disabled.
    // if (sdec_uk.allow_bracket && !g_eng_uk100.pos.active)
    //     dispatch_bracket(g_bracket_uk100, ...);
    // ?? UK100 manage block -- ALWAYS run when position open (SL/trail fix) ??
    if (g_orb_uk100.has_open_position()) { g_orb_uk100.on_tick(sym, bid, ask, ca_on_close); }

    // Opening range breakout: LSE open 08:00 UTC, 15-min range window
    if (!g_orb_uk100.has_open_position() && base_can_uk) {
        const auto orb = g_orb_uk100.on_tick(sym, bid, ask, ca_on_close);
        if (orb.valid) {
            g_telemetry.UpdateLastSignal("UK100", orb.is_long?"LONG":"SHORT", orb.entry, orb.reason, "ORB", regime.c_str(), "ORB", orb.tp, orb.sl);
            if (!enter_directional("UK100", orb.is_long, orb.entry, orb.sl, orb.tp))
                g_orb_uk100.cancel();
                else g_orb_uk100.patch_size(g_last_directional_lot);
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// ESTX50 tick handler
// ──────────────────────────────────────────────────────────────────────
static void on_tick_estx50(const std::string& sym, double bid, double ask) {
    const bool base_can_estx = symbol_gate("ESTX50", g_eng_estx50.pos.active || g_bracket_estx50.pos.active);
    const auto sdec_estx = sup_decision(g_sup_estx50, g_eng_estx50, base_can_estx);
    // SIM: EU index breakout -- no edge. Disabled.
    // if (sdec_estx.allow_breakout && !g_bracket_estx50.pos.active)
    //     dispatch(g_eng_estx50, g_sup_estx50, base_can_estx, &sdec_estx);
    // SIM: BracketEngine on indices -- no edge. Disabled.
    // if (sdec_estx.allow_bracket && !g_eng_estx50.pos.active)
    //     dispatch_bracket(g_bracket_estx50, ...);
    // ?? ESTX50 manage block -- ALWAYS run when position open (SL/trail fix) ??
    if (g_orb_estx50.has_open_position()) { g_orb_estx50.on_tick(sym, bid, ask, ca_on_close); }

    // Opening range breakout: Euronext open 09:00 UTC, 15-min range window
    if (!g_orb_estx50.has_open_position() && base_can_estx) {
        const auto orb = g_orb_estx50.on_tick(sym, bid, ask, ca_on_close);
        if (orb.valid) {
            g_telemetry.UpdateLastSignal("ESTX50", orb.is_long?"LONG":"SHORT", orb.entry, orb.reason, "ORB", regime.c_str(), "ORB", orb.tp, orb.sl);
            if (!enter_directional("ESTX50", orb.is_long, orb.entry, orb.sl, orb.tp))
                g_orb_estx50.cancel();
                else g_orb_estx50.patch_size(g_last_directional_lot);
        }
    }
}

// ──────────────────────────────────────────────────────────────────────
// NAS100 tick handler
// ──────────────────────────────────────────────────────────────────────
static void on_tick_nas100(const std::string& sym, double bid, double ask) {
    const bool base_can_nas = symbol_gate("NAS100",
        g_eng_nas100.pos.active      ||
        g_bracket_nas100.pos.active  ||
        g_nbm_nas.has_open_position()) // NBM
        // ?? Indices circuit breaker: block new entries for 30min after any US index FORCE_CLOSE
        && (static_cast<int64_t>(std::time(nullptr)) >= g_indices_disconnect_until.load());
    {
        const int64_t now_cb   = static_cast<int64_t>(std::time(nullptr));
        const int64_t until_cb = g_indices_disconnect_until.load();
        static int64_t s_cb_log_nas = 0;
        if (until_cb > now_cb && now_cb - s_cb_log_nas >= 60) {
            s_cb_log_nas = now_cb;
            printf("[INDICES-CB] NAS100 entries BLOCKED -- %llds remaining (disconnect cooldown)\n",
                   (long long)(until_cb - now_cb));
            fflush(stdout);
        }
    }
    const auto sdec_nas = sup_decision(g_sup_nas100, g_eng_nas100, base_can_nas);
    // SIM: NAS100 breakout -- no edge (correlated with NQ which is also disabled). Disabled.
    // if (sdec_nas.allow_breakout && !g_bracket_nas100.pos.active)
    //     dispatch(g_eng_nas100, g_sup_nas100, base_can_nas, &sdec_nas);
    // SIM: BracketEngine on indices -- no edge. Disabled.
    // const bool ustec_bracket_open = g_bracket_nq.has_open_position();
    // if (sdec_nas.allow_bracket && !g_eng_nas100.pos.active && !ustec_bracket_open)
    //     dispatch_bracket(g_bracket_nas100, ...);
    (void)sdec_nas;
    // ?? NAS100 manage block -- ALWAYS run when position open (SL/trail fix) ??
    if (g_nbm_nas.has_open_position()) { g_nbm_nas.on_tick(sym, bid, ask, ca_on_close); }

    // NoiseBandMomentum: Zarattini/Maroy intraday momentum (Sharpe 3.0-5.9).
    // Asia gate: NAS100 is a US cash instrument -- no edge during Asia thin tape.
    // Mirror USTEC.F gate: block slot 6 (Asia), slot 0 (dead zone), slot 5 (NY late)
    // unless M1 bars seeded AND M5 trend confirmed (same bars as USTEC.F / g_bars_nq).
    {
        const bool nas_nbm_offhours = (g_macro_ctx.session_slot == 6 ||
                                       g_macro_ctx.session_slot == 0 ||
                                       g_macro_ctx.session_slot == 5);
        const bool nas_nbm_bars_ok  = g_bars_nq.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const double nas_nbm_ema9   = g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed);
        const double nas_nbm_ema50  = g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed);
        const int  nas_nbm_m5_trend = (nas_nbm_ema9 > 0.0 && nas_nbm_ema50 > 0.0)
            ? (nas_nbm_ema9 < nas_nbm_ema50 ? -1 : +1) : 0;  // M1 EMA crossover
        const bool nas_nbm_gate_ok  = !nas_nbm_offhours || (nas_nbm_bars_ok && nas_nbm_m5_trend != 0);
        if (!g_nbm_nas.has_open_position() && base_can_nas && nas_nbm_gate_ok) {
            const auto nbm = g_nbm_nas.on_tick(sym, bid, ask, ca_on_close);
            if (nbm.valid) {
                g_telemetry.UpdateLastSignal("NAS100", nbm.is_long?"LONG":"SHORT", nbm.entry,
                    nbm.reason, "NBM", regime.c_str(), "NoiseBandMomentum", nbm.tp, nbm.sl);
                if (!enter_directional("NAS100", nbm.is_long, nbm.entry, nbm.sl, nbm.tp))
                    g_nbm_nas.cancel();
                else g_nbm_nas.patch_size(g_last_directional_lot);
            }
        }
    }
}

