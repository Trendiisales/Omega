#pragma once
// tick_indices.hpp -- per-symbol tick handlers for equity indices
// Extracted from on_tick(). Same translation unit -- all static functions visible.
//
// CHANGES vs prior version:
//   1. GoldHybridBracketEngine → IndexHybridBracketEngine wired for all 4 US indices
//      (g_hybrid_sp, g_hybrid_nq, g_hybrid_us30, g_hybrid_nas100).
//      Each engine: receives on_tick every tick, sends pending orders via
//      send_live_order() when transitioning to PENDING phase, and cancels
//      the losing side via send_cancel_order() on fill.
//
//   2. Supervisor block root-cause fix (comment + gate bypass):
//      dispatch_bracket() gates on sdec.allow_bracket which requires
//      stable_bracket >= min_bracket_score (0.35). bracket_score is computed
//      from compression_score which requires in_compression_for_sup=true.
//      in_compression_for_sup depends on ref_eng.phase == COMPRESSION, but
//      g_eng_sp/nq/us30/nas100 are disabled (never build compression).
//      Result: allow_bracket is always false for the old BracketEngine on indices.
//
//      The IndexHybridBracketEngine BYPASSES dispatch_bracket entirely -- it has
//      its own structural compression detector (MIN_RANGE/MAX_RANGE over 30 ticks)
//      and its own entry gate. The supervisor is NOT consulted for hybrid bracket
//      arm decisions. This is correct and intentional: the bracket's own range
//      geometry is the signal quality gate. Session/risk gates apply via can_enter.
//
//      The old g_bracket_sp/nq/us30/nas100 BracketEngines remain untouched in
//      their disabled state (commented out dispatch calls). They are preserved
//      for future re-evaluation but never fire live.

// ── US500.F ────────────────────────────────────────────────
static void on_tick_us500(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
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
        g_nbm_sp.has_open_position(), "", tradeable, lat_ok, regime, bid, ask)
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
    const auto sdec_sp = sup_decision(g_sup_sp, g_eng_sp, base_can_sp, sym, bid, ask);
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
            if (!enter_directional(sym.c_str(), esnq.is_long, esnq.entry, esnq.sl, esnq.tp, 0.01, false, bid, ask, sym, regime))
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

    if (!g_orb_us.has_open_position() && !g_vwap_rev_sp.has_open_position() && base_can_sp) {
        const auto orb = g_orb_us.on_tick(sym, bid, ask, ca_on_close);
        if (orb.valid) {
            g_telemetry.UpdateLastSignal("US500.F", orb.is_long?"LONG":"SHORT", orb.entry, orb.reason, "ORB", regime.c_str(), "ORB", orb.tp, orb.sl);
            if (!enter_directional("US500.F", orb.is_long, orb.entry, orb.sl, orb.tp, 0.01, false, bid, ask, sym, regime))
                g_orb_us.cancel();
                else g_orb_us.patch_size(g_last_directional_lot);
        }
    }
    // VWAP Reversion: enter when price reverses back toward daily VWAP after over-extension.
    if (!g_vwap_rev_sp.has_open_position() && !g_orb_us.has_open_position() && base_can_sp) {
        // ?? SP VWAP anchor: NY session open (13:30 UTC)
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
                g_vwap_rev_sp.reset_ewm_vwap(s_sp_ny_open);
            }
        }
        // NY open settle gate -- block VWAPRev for 30min after NY open
        static int64_t s_sp_ny_armed_ts = 0;
        if (s_sp_ny_armed && s_sp_ny_open > 0.0 && s_sp_ny_armed_ts == 0)
            s_sp_ny_armed_ts = static_cast<int64_t>(std::time(nullptr));
        const bool sp_ny_settled = (s_sp_ny_armed_ts > 0)
            && (static_cast<int64_t>(std::time(nullptr)) - s_sp_ny_armed_ts >= 30 * 60);
        if (s_sp_ny_armed && s_sp_ny_open > 0.0 && sp_ny_settled) {
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
                const double conf_mult = (vr.confluence_score >= 4) ? 3.0 :
                                        (vr.confluence_score == 3) ? 2.0 :
                                        (vr.confluence_score == 2) ? 1.5 : 1.0;
                if (!enter_directional("US500.F", vr.is_long, vr.entry, vr.sl, vr.tp, 0.01 * conf_mult, true, bid, ask, sym, regime))
                    g_vwap_rev_sp.cancel();
                else g_vwap_rev_sp.patch_size(g_last_directional_lot);
            }
            } // sp_spread_ok
        }
    }
    // NoiseBandMomentum
    {
        const bool sp_nbm_offhours = (g_macro_ctx.session_slot == 6 ||
                                      g_macro_ctx.session_slot == 0 ||
                                      g_macro_ctx.session_slot == 5);
        const bool sp_nbm_bars_ok  = g_bars_sp.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const double sp_nbm_ema9   = g_bars_sp.m1.ind.ema9 .load(std::memory_order_relaxed);
        const double sp_nbm_ema50  = g_bars_sp.m1.ind.ema50.load(std::memory_order_relaxed);
        const int  sp_nbm_m5_trend = (sp_nbm_ema9 > 0.0 && sp_nbm_ema50 > 0.0)
            ? (sp_nbm_ema9 < sp_nbm_ema50 ? -1 : +1) : 0;
        const bool sp_nbm_gate_ok  = !sp_nbm_offhours || (sp_nbm_bars_ok && sp_nbm_m5_trend != 0);
        if (!g_nbm_sp.has_open_position() && !g_orb_us.has_open_position() &&
            !g_vwap_rev_sp.has_open_position() && base_can_sp && sp_nbm_gate_ok) {
            const auto nbm = g_nbm_sp.on_tick(sym, bid, ask, ca_on_close);
            if (nbm.valid) {
                g_telemetry.UpdateLastSignal("US500.F", nbm.is_long?"LONG":"SHORT", nbm.entry,
                    nbm.reason, "NBM", regime.c_str(), "NoiseBandMomentum", nbm.tp, nbm.sl);
                if (!enter_directional("US500.F", nbm.is_long, nbm.entry, nbm.sl, nbm.tp, 0.01, false, bid, ask, sym, regime))
                    g_nbm_sp.cancel();
                else g_nbm_sp.patch_size(g_last_directional_lot);
            }
        }
    }
    // TrendPullback
    {
        const bool sp_in_offhours = (g_macro_ctx.session_slot == 6 || g_macro_ctx.session_slot == 0 || g_macro_ctx.session_slot == 5);
        const bool sp_bars_ready  = g_bars_sp.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const bool sp_ema_live    = g_bars_sp.m1.ind.m1_ema_live.load(std::memory_order_relaxed);
        const double sp_tpb_ema9  = sp_ema_live ? g_bars_sp.m1.ind.ema9 .load(std::memory_order_relaxed) : 0.0;
        const double sp_tpb_ema50 = sp_ema_live ? g_bars_sp.m1.ind.ema50.load(std::memory_order_relaxed) : 0.0;
        const int  sp_m5_trend    = (sp_tpb_ema9 > 0.0 && sp_tpb_ema50 > 0.0)
            ? (sp_tpb_ema9 < sp_tpb_ema50 ? -1 : +1) : 0;
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
                                       tp_sig.sl, tp_sig.tp, 0.01, true, bid, ask, sym, regime))
                    g_trend_pb_sp.cancel();
            }
        }
    }

    // ?? IndexHybridBracketEngine -- US500.F ????????????????????????????????????
    // Manages open position unconditionally, then checks for new entry.
    // Gate: base_can_sp (risk/session/latency/circuit-breaker) AND no other US500.F
    // position open. The hybrid engine has its own structural compression detector
    // (MIN_RANGE=8pt, MAX_RANGE=40pt over 30 ticks) -- supervisor is NOT consulted.
    // shadow_mode=true by default -- no real orders until shadow validates.
    {
        const int64_t now_ms_h = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        // Position management -- always runs when PENDING or LIVE
        if (g_hybrid_sp.has_open_position()) {
            g_hybrid_sp.on_tick(bid, ask, now_ms_h, base_can_sp,
                                false, false, 0, ca_on_close);
        }

        // New entry gate: no other US500.F position open
        // US indices only trade London + NY sessions (slots 1-5, 07:00-22:00 UTC).
        // Asia (slot 6, 22:00-05:00 UTC) has no US index price discovery.
        // Evidence: NAS100 LONG 00:16 UTC SL -$24.78 (Asia drift noise armed bracket).
        const bool idx_session_ok = (g_macro_ctx.session_slot >= 1 &&
                                     g_macro_ctx.session_slot <= 5);
        const bool hybrid_sp_can_enter =
            base_can_sp
            && idx_session_ok
            && !g_eng_sp.pos.active
            && !g_bracket_sp.pos.active
            && !g_orb_us.has_open_position()
            && !g_vwap_rev_sp.has_open_position()
            && !g_trend_pb_sp.has_open_position()
            && !g_nbm_sp.has_open_position()
            && !g_iflow_sp.has_open_position();

        // FIX 2026-04-07: call on_tick unconditionally to feed structure window.
        // Window was starved when hybrid_sp_can_enter=false -- range stayed 0.00 permanently.
        if (!g_hybrid_sp.has_open_position()) {
            g_hybrid_sp.on_tick(bid, ask, now_ms_h, hybrid_sp_can_enter,
                                false, false, 0, ca_on_close);
        }

        // When hybrid transitions to PENDING, send both stop orders
        if (g_hybrid_sp.phase == omega::idx::IndexHybridBracketEngine::Phase::PENDING
            && g_hybrid_sp.pending_long_clOrdId.empty()
            && g_hybrid_sp.pending_short_clOrdId.empty()) {
            const double h_hi  = g_hybrid_sp.bracket_high;
            const double h_lo  = g_hybrid_sp.bracket_low;
            const double h_lot = g_hybrid_sp.pending_lot;
            if (h_hi > 0.0 && h_lo > 0.0 && h_lot >= 0.01) {
                g_hybrid_sp.cancel_fn = [](const std::string& id) { send_cancel_order(id); };
                const std::string h_long_id  = send_live_order("US500.F", true,  h_lot, h_hi);
                const std::string h_short_id = send_live_order("US500.F", false, h_lot, h_lo);
                g_hybrid_sp.pending_long_clOrdId  = h_long_id;
                g_hybrid_sp.pending_short_clOrdId = h_short_id;
                printf("[HYBRID-SP] ORDERS SENT long_id=%s short_id=%s "
                       "hi=%.2f lo=%.2f range=%.2f lot=%.3f\n",
                       h_long_id.c_str(), h_short_id.c_str(),
                       h_hi, h_lo, g_hybrid_sp.range, h_lot);
                fflush(stdout);
            }
        }
    }
    // ?? IndexFlowEngine -- US500.F ?????????????????????????????????????????????
    // L2 order-flow + EWM drift engine. Runs when no other US500.F position is open.
    {
        const double sp_l2_imb = g_macro_ctx.sp_l2_imbalance;
        if (g_iflow_sp.has_open_position()) {
            g_iflow_sp.on_tick(sym, bid, ask, sp_l2_imb, ca_on_close, false);
        } else if (base_can_sp
                   && !g_eng_sp.pos.active
                   && !g_orb_us.has_open_position()
                   && !g_vwap_rev_sp.has_open_position()
                   && !g_trend_pb_sp.has_open_position()
                   && !g_nbm_sp.has_open_position()
                   && !g_hybrid_sp.has_open_position()) {
            const auto isig = g_iflow_sp.on_tick(sym, bid, ask, sp_l2_imb, ca_on_close, true);
            if (isig.valid) {
                g_telemetry.UpdateLastSignal("US500.F", isig.is_long?"LONG":"SHORT",
                    isig.entry, isig.reason, "IFLOW", regime.c_str(), "IndexFlow",
                    isig.tp, isig.sl);
                if (!enter_directional("US500.F", isig.is_long, isig.entry,
                                       isig.sl, isig.tp, isig.size, true, bid, ask, sym, regime))
                    g_iflow_sp.patch_size(0.0);
                else g_iflow_sp.patch_size(g_last_directional_lot);
            }
        }
    }
}

// ── USTEC.F ────────────────────────────────────────────────
static void on_tick_ustec(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
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
        g_trend_pb_nq.has_open_position()    ||
        g_nbm_nq.has_open_position(), "", tradeable, lat_ok, regime, bid, ask)
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
    const auto sdec_nq = sup_decision(g_sup_nq, g_eng_nq, base_can_nq, sym, bid, ask);
    // SIM: NQ breakout WR 26.1% -$1167. Disabled.
    // if (sdec_nq.allow_breakout && !g_bracket_nq.pos.active)
    //     dispatch(g_eng_nq, g_sup_nq, base_can_nq, &sdec_nq);
    // SIM: BracketEngine on indices -- no edge. Disabled.
    // if (sdec_nq.allow_bracket && !g_eng_nq.pos.active)
    //     dispatch_bracket(g_bracket_nq, ...);
    // ?? USTEC.F manage blocks -- ALWAYS run when position open ??
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
        {
            const double nq_s_e9  = g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed);
            const double nq_s_e50 = g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed);
            const int nq_ema_trend = (nq_s_e9 > 0.0 && nq_s_e50 > 0.0)
                ? (nq_s_e9 < nq_s_e50 ? -1 : +1) : 0;
            g_trend_pb_nq.seed_m5_trend(nq_ema_trend);
        }
    }
    if (g_trend_pb_nq.has_open_position()) { g_trend_pb_nq.on_tick(sym, bid, ask, ca_on_close); }
    if (g_nbm_nq.has_open_position())      { g_nbm_nq.on_tick(sym, bid, ask, ca_on_close); }

    // VWAP Reversion NQ -- anchored to NY open (13:30 UTC)
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
        static int64_t s_nq_ny_armed_ts = 0;
        if (s_nq_ny_armed && s_nq_ny_open > 0.0 && s_nq_ny_armed_ts == 0)
            s_nq_ny_armed_ts = static_cast<int64_t>(std::time(nullptr));
        const bool nq_ny_settled = (s_nq_ny_armed_ts > 0)
            && (static_cast<int64_t>(std::time(nullptr)) - s_nq_ny_armed_ts >= 30 * 60);
        if (s_nq_ny_armed && s_nq_ny_open > 0.0 && nq_ny_settled) {
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
                if (!enter_directional("USTEC.F", vr.is_long, vr.entry, vr.sl, vr.tp, 0.01 * conf_mult, true, bid, ask, sym, regime))
                    g_vwap_rev_nq.cancel();
                else g_vwap_rev_nq.patch_size(g_last_directional_lot);
            }
            } // nq_spread_ok
        }
    }
    // TrendPullback NQ
    {
        const int slot_nq = g_macro_ctx.session_slot;
        const bool nq_in_offhours = (slot_nq == 6 || slot_nq == 0 || slot_nq == 5);
        const bool nq_bars_ready  = g_bars_nq.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const bool nq_ema_live    = g_bars_nq.m1.ind.m1_ema_live.load(std::memory_order_relaxed);
        const double nq_tpb_ema9  = nq_ema_live ? g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed) : 0.0;
        const double nq_tpb_ema50 = nq_ema_live ? g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed) : 0.0;
        const int  nq_m5_trend    = (nq_tpb_ema9 > 0.0 && nq_tpb_ema50 > 0.0)
            ? (nq_tpb_ema9 < nq_tpb_ema50 ? -1 : +1) : 0;
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
                                       tp_sig.sl, tp_sig.tp, 0.01, true, bid, ask, sym, regime))
                    g_trend_pb_nq.cancel();
            }
        }
    }
    // NoiseBandMomentum NQ
    {
        const int slot_nq2 = g_macro_ctx.session_slot;
        const bool nq_in_offhours2 = (slot_nq2 == 6 || slot_nq2 == 0 || slot_nq2 == 5);
        const bool nq_bars_ready2  = g_bars_nq.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const double nq_nbm_ema9   = g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed);
        const double nq_nbm_ema50  = g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed);
        const int  nq_m5_trend2    = (nq_nbm_ema9 > 0.0 && nq_nbm_ema50 > 0.0)
            ? (nq_nbm_ema9 < nq_nbm_ema50 ? -1 : +1) : 0;
        const bool nq_nbm_ok       = !nq_in_offhours2 || (nq_bars_ready2 && nq_m5_trend2 != 0);
        if (!g_nbm_nq.has_open_position() && !g_vwap_rev_nq.has_open_position()
            && base_can_nq && nq_nbm_ok) {
            const auto nbm = g_nbm_nq.on_tick(sym, bid, ask, ca_on_close);
            if (nbm.valid) {
                g_telemetry.UpdateLastSignal("USTEC.F", nbm.is_long?"LONG":"SHORT", nbm.entry,
                    nbm.reason, "NBM", regime.c_str(), "NoiseBandMomentum", nbm.tp, nbm.sl);
                if (!enter_directional("USTEC.F", nbm.is_long, nbm.entry, nbm.sl, nbm.tp, 0.01, false, bid, ask, sym, regime))
                    g_nbm_nq.cancel();
                else g_nbm_nq.patch_size(g_last_directional_lot);
            }
        }
    }

    // ?? IndexHybridBracketEngine -- USTEC.F ????????????????????????????????????
    {
        const int64_t now_ms_h = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        if (g_hybrid_nq.has_open_position()) {
            g_hybrid_nq.on_tick(bid, ask, now_ms_h, base_can_nq,
                                false, false, 0, ca_on_close);
        }

        const bool idx_session_ok = (g_macro_ctx.session_slot >= 1 &&
                                     g_macro_ctx.session_slot <= 5);
        const bool hybrid_nq_can_enter =
            base_can_nq
            && idx_session_ok
            && !g_eng_nq.pos.active
            && !g_bracket_nq.pos.active
            && !g_vwap_rev_nq.has_open_position()
            && !g_trend_pb_nq.has_open_position()
            && !g_nbm_nq.has_open_position()
            && !g_iflow_nq.has_open_position();

        // FIX 2026-04-07: call on_tick unconditionally to feed structure window.
        // Window was starved when hybrid_nq_can_enter=false -- range stayed 0.00 permanently.
        if (!g_hybrid_nq.has_open_position()) {
            g_hybrid_nq.on_tick(bid, ask, now_ms_h, hybrid_nq_can_enter,
                                false, false, 0, ca_on_close);
        }

        if (g_hybrid_nq.phase == omega::idx::IndexHybridBracketEngine::Phase::PENDING
            && g_hybrid_nq.pending_long_clOrdId.empty()
            && g_hybrid_nq.pending_short_clOrdId.empty()) {
            const double h_hi  = g_hybrid_nq.bracket_high;
            const double h_lo  = g_hybrid_nq.bracket_low;
            const double h_lot = g_hybrid_nq.pending_lot;
            if (h_hi > 0.0 && h_lo > 0.0 && h_lot >= 0.01) {
                g_hybrid_nq.cancel_fn = [](const std::string& id) { send_cancel_order(id); };
                const std::string h_long_id  = send_live_order("USTEC.F", true,  h_lot, h_hi);
                const std::string h_short_id = send_live_order("USTEC.F", false, h_lot, h_lo);
                g_hybrid_nq.pending_long_clOrdId  = h_long_id;
                g_hybrid_nq.pending_short_clOrdId = h_short_id;
                printf("[HYBRID-NQ] ORDERS SENT long_id=%s short_id=%s "
                       "hi=%.2f lo=%.2f range=%.2f lot=%.3f\n",
                       h_long_id.c_str(), h_short_id.c_str(),
                       h_hi, h_lo, g_hybrid_nq.range, h_lot);
                fflush(stdout);
            }
        }
    }
    // ?? IndexFlowEngine -- USTEC.F
    {
        const double nq_l2_imb = g_macro_ctx.nq_l2_imbalance;
        if (g_iflow_nq.has_open_position()) {
            g_iflow_nq.on_tick(sym, bid, ask, nq_l2_imb, ca_on_close, false);
        } else if (base_can_nq
                   && !g_eng_nq.pos.active
                   && !g_vwap_rev_nq.has_open_position()
                   && !g_trend_pb_nq.has_open_position()
                   && !g_nbm_nq.has_open_position()
                   && !g_hybrid_nq.has_open_position()) {
            const auto isig = g_iflow_nq.on_tick(sym, bid, ask, nq_l2_imb, ca_on_close, true);
            if (isig.valid) {
                g_telemetry.UpdateLastSignal("USTEC.F", isig.is_long?"LONG":"SHORT",
                    isig.entry, isig.reason, "IFLOW", regime.c_str(), "IndexFlow",
                    isig.tp, isig.sl);
                if (!enter_directional("USTEC.F", isig.is_long, isig.entry,
                                       isig.sl, isig.tp, isig.size, true, bid, ask, sym, regime))
                    g_iflow_nq.patch_size(0.0);
                else g_iflow_nq.patch_size(g_last_directional_lot);
            }
        }
    }
}

// ── DJ30.F ─────────────────────────────────────────────────
static void on_tick_dj30(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    const bool base_can_us30 = symbol_gate("DJ30.F",
        g_eng_us30.pos.active      ||
        g_bracket_us30.pos.active  ||
        g_nbm_us30.has_open_position(), "", tradeable, lat_ok, regime, bid, ask)
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
    const auto sdec_us30 = sup_decision(g_sup_us30, g_eng_us30, base_can_us30, sym, bid, ask);
    // SIM: DJ30 breakout WR 23.5% -$736, bracket also negative. Both disabled.
    // if (sdec_us30.allow_breakout && !g_bracket_us30.pos.active)
    //     dispatch(g_eng_us30, g_sup_us30, base_can_us30, &sdec_us30);
    // if (sdec_us30.allow_bracket && !g_eng_us30.pos.active)
    //     dispatch_bracket(g_bracket_us30, ...);
    (void)sdec_us30;
    // ?? DJ30.F manage block -- ALWAYS run when position open ??
    if (g_nbm_us30.has_open_position()) { g_nbm_us30.on_tick(sym, bid, ask, ca_on_close); }

    // NoiseBandMomentum DJ30
    {
        const int slot_us30 = g_macro_ctx.session_slot;
        const bool us30_session_ok = (slot_us30 >= 1 && slot_us30 <= 5);
        if (!g_nbm_us30.has_open_position() && base_can_us30 && us30_session_ok) {
            const auto nbm = g_nbm_us30.on_tick(sym, bid, ask, ca_on_close);
            if (nbm.valid) {
                g_telemetry.UpdateLastSignal("DJ30.F", nbm.is_long?"LONG":"SHORT", nbm.entry,
                    nbm.reason, "NBM", regime.c_str(), "NoiseBandMomentum", nbm.tp, nbm.sl);
                if (!enter_directional("DJ30.F", nbm.is_long, nbm.entry, nbm.sl, nbm.tp, 0.01, false, bid, ask, sym, regime))
                    g_nbm_us30.cancel();
                else g_nbm_us30.patch_size(g_last_directional_lot);
            }
        }
    }

    // ?? IndexHybridBracketEngine -- DJ30.F ?????????????????????????????????????
    {
        const int64_t now_ms_h = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        if (g_hybrid_us30.has_open_position()) {
            g_hybrid_us30.on_tick(bid, ask, now_ms_h, base_can_us30,
                                  false, false, 0, ca_on_close);
        }

        const bool idx_session_ok = (g_macro_ctx.session_slot >= 1 &&
                                     g_macro_ctx.session_slot <= 5);
        const bool hybrid_us30_can_enter =
            base_can_us30
            && idx_session_ok
            && !g_eng_us30.pos.active
            && !g_bracket_us30.pos.active
            && !g_nbm_us30.has_open_position()
            && !g_iflow_us30.has_open_position();

        // FIX 2026-04-07: call on_tick unconditionally to feed structure window.
        // Window was starved when hybrid_us30_can_enter=false -- range stayed 0.00 permanently.
        if (!g_hybrid_us30.has_open_position()) {
            g_hybrid_us30.on_tick(bid, ask, now_ms_h, hybrid_us30_can_enter,
                                  false, false, 0, ca_on_close);
        }

        if (g_hybrid_us30.phase == omega::idx::IndexHybridBracketEngine::Phase::PENDING
            && g_hybrid_us30.pending_long_clOrdId.empty()
            && g_hybrid_us30.pending_short_clOrdId.empty()) {
            const double h_hi  = g_hybrid_us30.bracket_high;
            const double h_lo  = g_hybrid_us30.bracket_low;
            const double h_lot = g_hybrid_us30.pending_lot;
            if (h_hi > 0.0 && h_lo > 0.0 && h_lot >= 0.01) {
                g_hybrid_us30.cancel_fn = [](const std::string& id) { send_cancel_order(id); };
                const std::string h_long_id  = send_live_order("DJ30.F", true,  h_lot, h_hi);
                const std::string h_short_id = send_live_order("DJ30.F", false, h_lot, h_lo);
                g_hybrid_us30.pending_long_clOrdId  = h_long_id;
                g_hybrid_us30.pending_short_clOrdId = h_short_id;
                printf("[HYBRID-US30] ORDERS SENT long_id=%s short_id=%s "
                       "hi=%.2f lo=%.2f range=%.2f lot=%.3f\n",
                       h_long_id.c_str(), h_short_id.c_str(),
                       h_hi, h_lo, g_hybrid_us30.range, h_lot);
                fflush(stdout);
            }
        }
    }
    // ?? IndexFlowEngine -- DJ30.F
    {
        const double us30_l2_imb = g_macro_ctx.us30_l2_imbalance;
        if (g_iflow_us30.has_open_position()) {
            g_iflow_us30.on_tick(sym, bid, ask, us30_l2_imb, ca_on_close, false);
        } else if (base_can_us30
                   && !g_eng_us30.pos.active
                   && !g_nbm_us30.has_open_position()
                   && !g_hybrid_us30.has_open_position()) {
            const auto isig = g_iflow_us30.on_tick(sym, bid, ask, us30_l2_imb, ca_on_close, true);
            if (isig.valid) {
                g_telemetry.UpdateLastSignal("DJ30.F", isig.is_long?"LONG":"SHORT",
                    isig.entry, isig.reason, "IFLOW", regime.c_str(), "IndexFlow",
                    isig.tp, isig.sl);
                if (!enter_directional("DJ30.F", isig.is_long, isig.entry,
                                       isig.sl, isig.tp, isig.size, true, bid, ask, sym, regime))
                    g_iflow_us30.patch_size(0.0);
                else g_iflow_us30.patch_size(g_last_directional_lot);
            }
        }
    }
}

// ── GER40 ──────────────────────────────────────────────────
static void on_tick_ger40(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    const bool base_can_ger = symbol_gate("GER40",
        g_eng_ger30.pos.active              ||
        g_bracket_ger30.pos.active          ||
        g_orb_ger30.has_open_position()     ||
        g_vwap_rev_ger40.has_open_position() ||
        g_trend_pb_ger40.has_open_position(), "", tradeable, lat_ok, regime, bid, ask);
    const auto sdec_ger = sup_decision(g_sup_ger30, g_eng_ger30, base_can_ger, sym, bid, ask);
    // ?? GER40 manage blocks -- ALWAYS run when position open ??
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

// ── UK100 ──────────────────────────────────────────────────
static void on_tick_uk100(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    const bool base_can_uk = symbol_gate("UK100", g_eng_uk100.pos.active || g_bracket_uk100.pos.active, "", tradeable, lat_ok, regime, bid, ask);
    const auto sdec_uk = sup_decision(g_sup_uk100, g_eng_uk100, base_can_uk, sym, bid, ask);
    // SIM: EU index breakout -- no edge. Disabled.
    // if (sdec_uk.allow_breakout && !g_bracket_uk100.pos.active)
    //     dispatch(g_eng_uk100, g_sup_uk100, base_can_uk, &sdec_uk);
    // ?? UK100 manage block -- ALWAYS run when position open ??
    if (g_orb_uk100.has_open_position()) { g_orb_uk100.on_tick(sym, bid, ask, ca_on_close); }

    // Opening range breakout: LSE open 08:00 UTC, 15-min range window
    if (!g_orb_uk100.has_open_position() && base_can_uk) {
        const auto orb = g_orb_uk100.on_tick(sym, bid, ask, ca_on_close);
        if (orb.valid) {
            g_telemetry.UpdateLastSignal("UK100", orb.is_long?"LONG":"SHORT", orb.entry, orb.reason, "ORB", regime.c_str(), "ORB", orb.tp, orb.sl);
            if (!enter_directional("UK100", orb.is_long, orb.entry, orb.sl, orb.tp, 0.01, false, bid, ask, sym, regime))
                g_orb_uk100.cancel();
                else g_orb_uk100.patch_size(g_last_directional_lot);
        }
    }
    (void)sdec_uk;
}

// ── ESTX50 ─────────────────────────────────────────────────
static void on_tick_estx50(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    const bool base_can_estx = symbol_gate("ESTX50", g_eng_estx50.pos.active || g_bracket_estx50.pos.active, "", tradeable, lat_ok, regime, bid, ask);
    const auto sdec_estx = sup_decision(g_sup_estx50, g_eng_estx50, base_can_estx, sym, bid, ask);
    // SIM: EU index breakout -- no edge. Disabled.
    // if (sdec_estx.allow_breakout && !g_bracket_estx50.pos.active)
    //     dispatch(g_eng_estx50, g_sup_estx50, base_can_estx, &sdec_estx);
    // ?? ESTX50 manage block -- ALWAYS run when position open ??
    if (g_orb_estx50.has_open_position()) { g_orb_estx50.on_tick(sym, bid, ask, ca_on_close); }

    // Opening range breakout: Euronext open 09:00 UTC, 15-min range window
    if (!g_orb_estx50.has_open_position() && base_can_estx) {
        const auto orb = g_orb_estx50.on_tick(sym, bid, ask, ca_on_close);
        if (orb.valid) {
            g_telemetry.UpdateLastSignal("ESTX50", orb.is_long?"LONG":"SHORT", orb.entry, orb.reason, "ORB", regime.c_str(), "ORB", orb.tp, orb.sl);
            if (!enter_directional("ESTX50", orb.is_long, orb.entry, orb.sl, orb.tp, 0.01, false, bid, ask, sym, regime))
                g_orb_estx50.cancel();
                else g_orb_estx50.patch_size(g_last_directional_lot);
        }
    }
    (void)sdec_estx;
}

// ── NAS100 ─────────────────────────────────────────────────
static void on_tick_nas100(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    const bool base_can_nas = symbol_gate("NAS100",
        g_eng_nas100.pos.active      ||
        g_bracket_nas100.pos.active  ||
        g_nbm_nas.has_open_position(), "", tradeable, lat_ok, regime, bid, ask)
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
    const auto sdec_nas = sup_decision(g_sup_nas100, g_eng_nas100, base_can_nas, sym, bid, ask);
    // SIM: NAS100 breakout -- no edge. Disabled.
    // if (sdec_nas.allow_breakout && !g_bracket_nas100.pos.active)
    //     dispatch(g_eng_nas100, g_sup_nas100, base_can_nas, &sdec_nas);
    (void)sdec_nas;
    // ?? NAS100 manage block -- ALWAYS run when position open ??
    if (g_nbm_nas.has_open_position()) { g_nbm_nas.on_tick(sym, bid, ask, ca_on_close); }

    // NoiseBandMomentum NAS100
    {
        const bool nas_nbm_offhours = (g_macro_ctx.session_slot == 6 ||
                                       g_macro_ctx.session_slot == 0 ||
                                       g_macro_ctx.session_slot == 5);
        const bool nas_nbm_bars_ok  = g_bars_nq.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const double nas_nbm_ema9   = g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed);
        const double nas_nbm_ema50  = g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed);
        const int  nas_nbm_m5_trend = (nas_nbm_ema9 > 0.0 && nas_nbm_ema50 > 0.0)
            ? (nas_nbm_ema9 < nas_nbm_ema50 ? -1 : +1) : 0;
        const bool nas_nbm_gate_ok  = !nas_nbm_offhours || (nas_nbm_bars_ok && nas_nbm_m5_trend != 0);
        if (!g_nbm_nas.has_open_position() && base_can_nas && nas_nbm_gate_ok) {
            const auto nbm = g_nbm_nas.on_tick(sym, bid, ask, ca_on_close);
            if (nbm.valid) {
                g_telemetry.UpdateLastSignal("NAS100", nbm.is_long?"LONG":"SHORT", nbm.entry,
                    nbm.reason, "NBM", regime.c_str(), "NoiseBandMomentum", nbm.tp, nbm.sl);
                if (!enter_directional("NAS100", nbm.is_long, nbm.entry, nbm.sl, nbm.tp, 0.01, false, bid, ask, sym, regime))
                    g_nbm_nas.cancel();
                else g_nbm_nas.patch_size(g_last_directional_lot);
            }
        }
    }

    // ?? IndexHybridBracketEngine -- NAS100 ????????????????????????????????????
    {
        const int64_t now_ms_h = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        if (g_hybrid_nas100.has_open_position()) {
            g_hybrid_nas100.on_tick(bid, ask, now_ms_h, base_can_nas,
                                    false, false, 0, ca_on_close);
        }

        const bool idx_session_ok = (g_macro_ctx.session_slot >= 1 &&
                                     g_macro_ctx.session_slot <= 5);
        const bool hybrid_nas_can_enter =
            base_can_nas
            && idx_session_ok
            && !g_eng_nas100.pos.active
            && !g_bracket_nas100.pos.active
            && !g_nbm_nas.has_open_position()
            && !g_iflow_nas.has_open_position();

        // FIX 2026-04-07: call on_tick unconditionally to feed structure window.
        // Window was starved when hybrid_nas_can_enter=false -- range stayed 0.00 permanently.
        if (!g_hybrid_nas100.has_open_position()) {
            g_hybrid_nas100.on_tick(bid, ask, now_ms_h, hybrid_nas_can_enter,
                                    false, false, 0, ca_on_close);
        }

        if (g_hybrid_nas100.phase == omega::idx::IndexHybridBracketEngine::Phase::PENDING
            && g_hybrid_nas100.pending_long_clOrdId.empty()
            && g_hybrid_nas100.pending_short_clOrdId.empty()) {
            const double h_hi  = g_hybrid_nas100.bracket_high;
            const double h_lo  = g_hybrid_nas100.bracket_low;
            const double h_lot = g_hybrid_nas100.pending_lot;
            if (h_hi > 0.0 && h_lo > 0.0 && h_lot >= 0.01) {
                g_hybrid_nas100.cancel_fn = [](const std::string& id) { send_cancel_order(id); };
                const std::string h_long_id  = send_live_order("NAS100", true,  h_lot, h_hi);
                const std::string h_short_id = send_live_order("NAS100", false, h_lot, h_lo);
                g_hybrid_nas100.pending_long_clOrdId  = h_long_id;
                g_hybrid_nas100.pending_short_clOrdId = h_short_id;
                printf("[HYBRID-NAS100] ORDERS SENT long_id=%s short_id=%s "
                       "hi=%.2f lo=%.2f range=%.2f lot=%.3f\n",
                       h_long_id.c_str(), h_short_id.c_str(),
                       h_hi, h_lo, g_hybrid_nas100.range, h_lot);
                fflush(stdout);
            }
        }
    }
    // ?? IndexFlowEngine -- NAS100
    {
        const double nas_l2_imb = g_macro_ctx.nas_l2_imbalance;
        if (g_iflow_nas.has_open_position()) {
            g_iflow_nas.on_tick(sym, bid, ask, nas_l2_imb, ca_on_close, false);
        } else if (base_can_nas
                   && !g_eng_nas100.pos.active
                   && !g_nbm_nas.has_open_position()
                   && !g_hybrid_nas100.has_open_position()) {
            const auto isig = g_iflow_nas.on_tick(sym, bid, ask, nas_l2_imb, ca_on_close, true);
            if (isig.valid) {
                g_telemetry.UpdateLastSignal("NAS100", isig.is_long?"LONG":"SHORT",
                    isig.entry, isig.reason, "IFLOW", regime.c_str(), "IndexFlow",
                    isig.tp, isig.sl);
                if (!enter_directional("NAS100", isig.is_long, isig.entry,
                                       isig.sl, isig.tp, isig.size, true, bid, ask, sym, regime))
                    g_iflow_nas.patch_size(0.0);
                else g_iflow_nas.patch_size(g_last_directional_lot);
            }
        }
    }
}
