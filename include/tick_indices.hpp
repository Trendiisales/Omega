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

#include <chrono>
#include "IndexBeFloorCompanion.hpp"   // omega::index_befloor_book() (per-symbol index BE-floor companion)
// (JumpRiderEngine.hpp include REMOVED — engine culled/tombstoned S-2026-07-10)
#include "FxUpJumpLadderCompanion.hpp" // omega::index_upjump_ladder_book() (upjump LADDER, needs H1 h/l)

// ── index BE-floor companion H1 feed (S-2026-07-06) ─────────────────────────
//   Indices are traded live, and separately the index BE-floor companion needs each
//   symbol's H1 close stream (shadow, additive). Self-contained wall-clock H1 aggregator
//   (mirrors fx_feed_bars in tick_fx.hpp): roll a 1h bucket from the tick mid and drive
//   index_befloor_book().on_h1_bar(tag, ts, close) on each H1 close. Observe-only; sends
//   no orders itself (the companion's own order path is mode-gated SHADOW->live-on-flip).
struct IdxH1Agg { int64_t start = 0; double close = 0.0; double high = 0.0, low = 0.0; double open = 0.0; };
static inline void index_feed_h1(IdxH1Agg& a, const char* tag, double bid, double ask) {
    const double mid = (bid + ask) * 0.5;
    if (mid <= 0.0) return;
    omega::index_upjump_ladder_book().set_disp_mid(tag, mid);   // S-2026-07-08d live display mark
    const int64_t now_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const int64_t b = (now_ms / 3600000LL) * 3600000LL;
    if (a.start == 0) { a.start = b; a.open = mid; a.close = mid; a.high = mid; a.low = mid; }
    else if (b != a.start) {
        omega::index_befloor_book().on_h1_bar(tag, a.start / 1000, a.close);
        // (JumpRider index feed REMOVED — engine culled/tombstoned S-2026-07-10)
        omega::index_upjump_ladder_book().on_h1_bar(tag, a.start / 1000,
                                                    a.high, a.low, a.close, a.open); // h/l intrabar; open for Layer-3 weekend gap
        a.start = b; a.open = mid; a.close = mid; a.high = mid; a.low = mid;
    } else { a.close = mid; if (mid > a.high) a.high = mid; if (mid < a.low) a.low = mid; }
    // S-2026-07-07 real-fill fix: every tick also drives the BE-floor companion INTRABAR
    // catastrophe cap (resting stop under the model floor). Close-only exit eval booked
    // worse-of(floor,close) -- one -5pt US500 hourly close through the floor booked -$273
    // on all 5 pre-progress tiers (2026-07-06). Cheap no-op when no leg is open.
    omega::index_befloor_book().on_tick(tag, now_ms / 1000, bid, ask);
}

// ── M2K (micro E-mini Russell 2000, CME) ───────────────────
// 2026-07-09: NEW underlying (not covered by US500/NAS100/DJ30). IBKR-only L1 feed
// via the bridge (M2K->M2K). MINIMAL handler by design -- M2K drives ONLY the index
// up-jump ladder SHADOW book (validated W24/thr1.5 + BE-entry0.08 = +76.5% WF both
// halves). index_feed_h1 aggregates ticks -> H1 -> index_upjump_ladder_book().on_h1_bar.
// The BE-floor book is culled (family retired) so its on_h1_bar/on_tick calls there are
// cheap no-ops for M2K (no leg ever opens). No other engine family runs on M2K.
static void on_tick_m2k(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    (void)sym; (void)tradeable; (void)lat_ok; (void)regime;
    { static IdxH1Agg agg; index_feed_h1(agg, "M2K", bid, ask); }  // index up-jump ladder H1 feed
    g_engine_heartbeat.pulse("M2KUpJumpLadder");
}

// ── US500.F ────────────────────────────────────────────────
static void on_tick_us500(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    { static IdxH1Agg agg; index_feed_h1(agg, "US500", bid, ask); }  // BE-floor companion H1 feed
    {   // S-2026-07-12b BE-cascade port (shadow)
        const int64_t bc_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
        g_xsbec_us500.on_tick(bid, ask, bc_ms); g_engine_heartbeat.pulse("XsBeCascade_US500.F");
        // S-2026-07-12e: SPX regime brain + gated bracket-cascade H1/H4 (shadow)
        g_regime_spx.on_tick(bid, ask, bc_ms);
        g_brc_sp_h1.on_tick(bid, ask, bc_ms); g_engine_heartbeat.pulse("BrkCascade_US500_H1");
        g_brc_sp_h4.on_tick(bid, ask, bc_ms); g_engine_heartbeat.pulse("BrkCascade_US500_H4");
    }
    // 2026-05-05 (audit-fixes-40): heartbeat pulses for every US500-driven engine.
    // S11 P3b: HybridSP pulse removed (engine culled in P3a + globals/init removed in P3b).
    g_engine_heartbeat.pulse("IFlowSP");
    g_engine_heartbeat.pulse("IMacroSP");
    g_engine_heartbeat.pulse("TrendPullbackSP");
    g_engine_heartbeat.pulse("AmrUs500");  // 2026-05-26 (Stage 4)
    // S-2026-06-19 v3 MR family (US500): STREAK / DOUBLE dip-buy (shadow).
    {
        const int64_t conn_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
        g_streak_spx.on_tick(bid, ask, conn_ms); g_engine_heartbeat.pulse("ConnorsStreak_SPX");
        g_dbl_spx.on_tick(bid, ask, conn_ms);    g_engine_heartbeat.pulse("ConnorsDouble_SPX");
        g_ibs_spx.on_tick(bid, ask, conn_ms);    g_engine_heartbeat.pulse("ConnorsIBS_SPX");   // S-2026-06-20 breadth
        g_rsi2_spx.on_tick(bid, ask, conn_ms);   g_engine_heartbeat.pulse("ConnorsRSI2_SPX");  // S-2026-06-20 breadth
    }

    // AtrMeanRevGrid US500 (shadow). H1 X=8 SL_Y=6 ATR_FROM_WAP, PF 1.75 sweep.
    // Engine aggregates H1 bars from tick mids internally.

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
        // H1 -- HTF swing context for IndexSwingEngine
        static OHLCBar s_sph1{};
        static int64_t s_sph1_start = 0;
        const int64_t bh1_s = (now_ms_s / 3600000LL) * 3600000LL;
        if (s_sph1_start == 0) { s_sph1 = {bh1_s/60000LL,sp_mid,sp_mid,sp_mid,sp_mid}; s_sph1_start = bh1_s; }
        else if (bh1_s != s_sph1_start) { g_bars_sp.h1.add_bar(s_sph1); s_sph1 = {bh1_s/60000LL,sp_mid,sp_mid,sp_mid,sp_mid}; s_sph1_start = bh1_s; }
        else { if(sp_mid>s_sph1.high)s_sph1.high=sp_mid; if(sp_mid<s_sph1.low)s_sph1.low=sp_mid; s_sph1.close=sp_mid; }
        // NOTE: L2 tick CSV logger for US500 is at on_tick.hpp router level
        // (before indices_enabled gate), not here -- we must capture tick data
        // for hydrate_from_csv() regardless of whether index trading is enabled.
    }
    const bool base_can_sp = symbol_gate("US500.F",
        g_eng_sp.pos.active          ||
        g_bracket_sp.pos.active, "", tradeable, lat_ok, regime, bid, ask)
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
    (void)sdec_sp;
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
    // ?? US500.F manage blocks -- ALWAYS run when position open (SL/trail fix) ??
    // Seed TrendPB with real M1 bar EMAs from cTrader trendbar API
    if (g_bars_sp.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
        {   // FIX: M1 EMA crossover replaces M5 swing trend_state (15min lag)
            const double sp_s_e9  = g_bars_sp.m1.ind.ema9 .load(std::memory_order_relaxed);
            const double sp_s_e50 = g_bars_sp.m1.ind.ema50.load(std::memory_order_relaxed);
            const int sp_ema_trend = (sp_s_e9 > 0.0 && sp_s_e50 > 0.0)
                ? (sp_s_e9 < sp_s_e50 ? -1 : +1) : 0;
            (void)sp_ema_trend;
        }
    }

    // VWAP Reversion: enter when price reverses back toward daily VWAP after over-extension.
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
        (void)sp_nbm_gate_ok;
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
        (void)sp_trendpb_ok;
    }

    // ----------------------------------------------------------------------
    // HBI-SP fully retired S12 P3c (2026-05-07): IndexHybridBracketEngine[US500.F]
    //   - Dispatch removed S10 P3a (commit ba5f0e9)
    //   - Globals decl + init/heartbeat/gate-reads removed S11 P3b (commit a6e3403)
    //   - Header file include/IndexHybridBracketEngine.hpp DELETED + #include
    //     refs removed S12 P3c (this commit)
    //   Original dispatch (compression range -> dual stop entry, slot 1-5 only,
    //   shadow by default) was lines ~246-310 of tick_indices.hpp pre-S10.
    // ----------------------------------------------------------------------
    // ?? IndexFlowEngine -- US500.F ?????????????????????????????????????????????
    // L2 order-flow + EWM drift engine. Runs when no other US500.F position is open.
    {
        const double sp_l2_imb = g_macro_ctx.sp_l2_imbalance;
        if (g_iflow_sp.has_open_position()) {
            g_iflow_sp.on_tick(sym, bid, ask, sp_l2_imb, ca_on_close, false);
        } else if (!g_disable_index_flow  // 2026-04-30 audit-disable
                   && base_can_sp
                   && !g_eng_sp.pos.active
                   // S11 P3b: g_hybrid_sp gate removed -- engine culled in P3a, dispatch dormant.
                   // Bug #3 (KNOWN_BUGS.md): cross-symbol concurrent block + post-close gap.
                   && !index_any_open()
                   && !omega::idx::idx_recent_close_block()) {
            const auto isig = g_iflow_sp.on_tick(sym, bid, ask, sp_l2_imb, ca_on_close, true);
            if (isig.valid) {
                g_telemetry.UpdateLastSignal("US500.F", isig.is_long?"LONG":"SHORT",
                    isig.entry, isig.reason, "IFLOW", regime.c_str(), "IndexFlow",
                    isig.tp, isig.sl);
                if (!enter_directional("US500.F", isig.is_long, isig.entry,
                                       isig.sl, isig.tp, isig.size, true, bid, ask, sym, regime, "IndexFlow"))
                    g_iflow_sp.patch_size(0.0);
                else g_iflow_sp.patch_size(g_last_directional_lot);
            }
        }
    }
    // ── IndexMacroCrash US500.F (shadow only) ───────────────────────────────
    // vol_ratio = cur_atr / slow-EWM baseline (alpha=0.001).
    {
        static double s_atr_base_sp = 0.0;
        const double cur_atr_sp = g_iflow_sp.atr();
        if (s_atr_base_sp <= 0.0) {
            if (cur_atr_sp > 0.0) s_atr_base_sp = cur_atr_sp;
        } else {
            s_atr_base_sp = 0.999 * s_atr_base_sp + 0.001 * cur_atr_sp;
        }
        const double sp_vol_ratio = (s_atr_base_sp > 0.0)
            ? (cur_atr_sp / s_atr_base_sp) : 1.0;
        const bool sp_trend_regime = g_iflow_sp.is_trending();
        g_imacro_sp.on_tick(bid, ask, cur_atr_sp, g_iflow_sp.drift(),
                            sp_vol_ratio, sp_trend_regime, ca_on_close);
    }
    // ?? IndexSwingEngine -- US500.F H1+H4 swing entries (shadow mode) ????????
    {
        auto swing_sp_cb = [&](const omega::TradeRecord& tr) {
            // Shadow mode: call handle_closed_trade directly -- NOT ca_on_close.
            // ca_on_close sends a live market order to close; this engine never
            // opened a broker position so sending a close order would be a rogue order.
            handle_closed_trade(tr);
            printf("[ISWING-CB] US500.F %s pnl=%.2f why=%s\n",
                   tr.side.c_str(), tr.pnl, tr.exitReason.c_str());
            fflush(stdout);
        };
        const bool sw_sp_entered = g_iswing_sp.on_tick(bid, ask, g_bars_sp.h1, g_bars_sp.h4,
                            g_iflow_sp.drift(), swing_sp_cb);
        if (sw_sp_entered)
            g_telemetry.UpdateLastSignal("US500.F",
                g_iswing_sp.is_long_at_entry() ? "LONG" : "SHORT",
                g_iswing_sp.entry_price(), "H1+H4 EMA swing",
                "ISWING", regime.c_str(), "IndexSwing",
                0.0, g_iswing_sp.sl_price());
    }

    // -- IndexIntradayDriftEngine (S37-Z 2026-05-28) -------------------------
    // BUY at first H1 of UTC day, SELL at last H1 of UTC day. Audit verdict
    // SPXUSD Sharpe +0.77 net, walk-fwd both halves positive.
    {
        const int64_t now_ms_idd = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        (void)now_ms_idd;
    }
    // IndexSessionEngine US500/SPX (14-22 UTC LONG, flat overnight, risk-off gated).
    {
        const int64_t now_ms_isp = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_idxsess_sp.set_risk_off(omega::index_risk_off());
        g_idxsess_sp.on_tick(bid, ask, now_ms_isp);
        g_idx_bear_short_sp.on_tick(bid, ask, now_ms_isp); // 2026-06-22 SPX risk-off SHORT breakdown (shadow); real-engine SPX2022 PF1.59 both-halves+
        g_engine_heartbeat.pulse("IdxBearShortSp");  // S-2026-06-29 ENABLED+NO_PULSE fix
        g_engine_heartbeat.pulse("IndexSession_US500");
    }

    // SPX D1 turtle (2026-06-15) -- NasTurtleD1 chassis, Yahoo-daily xregime
    // PF2.49 both-halves+ (incl 2022 bear), cost-incl. Self-aggregates D1; shadow.
    {
        const int64_t now_ms_st = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_engine_heartbeat.pulse("SpxTurtleD1");  // S-2026-06-29 ENABLED+NO_PULSE fix
        const auto stsig = g_spx_turtle_d1.on_tick(bid, ask, now_ms_st, ca_on_close);
        if (stsig.valid) {
            g_telemetry.UpdateLastSignal("US500.F", "LONG", stsig.entry, stsig.reason,
                "SPX_TURTLE_D1", "", "SPX_TURTLE_D1", stsig.tp, stsig.sl);
        }
        if (g_spx_turtle_d1.has_open_position())
            g_spx_turtle_d1.check_weekend_close(bid, ask, now_ms_st, ca_on_close);
    }
}

// ── USTEC.F ────────────────────────────────────────────────
static void on_tick_ustec(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    {   // S-2026-07-12b BE-cascade port (shadow)
        const int64_t bc_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
        g_xsbec_ustec.on_tick(bid, ask, bc_ms); g_engine_heartbeat.pulse("XsBeCascade_USTEC.F");
        // S-2026-07-12e: NQ regime brain + gated bracket-cascade H1/H4 (shadow)
        g_regime_ndx.on_tick(bid, ask, bc_ms);
        g_brc_nq_h1.on_tick(bid, ask, bc_ms); g_engine_heartbeat.pulse("BrkCascade_USTEC_H1");
        g_brc_nq_h4.on_tick(bid, ask, bc_ms); g_engine_heartbeat.pulse("BrkCascade_USTEC_H4");
    }
    // 2026-05-05 (audit-fixes-40): heartbeat pulses for every USTEC-driven engine.
    // S11 P3b: HybridNQ pulse removed (engine culled in P3a + globals/init removed in P3b).
    g_engine_heartbeat.pulse("IFlowNQ");
    g_engine_heartbeat.pulse("IMacroNQ");
    g_engine_heartbeat.pulse("TrendPullbackNQ");

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
        else if (b5 != s_nq5_start) {
            g_bars_nq.m5.add_bar(s_nq5);
            // ── UstecTrendFollow5mEngine 5m-close dispatch (S33d 2026-05-11) ──
            // Donchian N=20 trend-follow on USTEC 5m bars. Shadow-only.
            {
                omega::UstecTfBar tf5m{};
                tf5m.bar_start_ms = s_nq5_start;
                tf5m.open  = s_nq5.open;
                tf5m.high  = s_nq5.high;
                tf5m.low   = s_nq5.low;
                tf5m.close = s_nq5.close;
            }
            s_nq5 = {b5/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nq5_start = b5;
        }
        else { if(nq_mid>s_nq5.high)s_nq5.high=nq_mid; if(nq_mid<s_nq5.low)s_nq5.low=nq_mid; s_nq5.close=nq_mid; }
        // M15 -- feeds UstecTrendFollowHtfEngine (S36-P4 2026-05-12)
        // 3-cell H1/H2/H4 ensemble (InsideBar2h + AtrMom1h + Stoch4h).
        // Engine synthesises H1/H2/H4 internally from the M15 stream.
        // Shadow-only by default. ATR computed locally inside the engine
        // (g_bars_nq has no m15 indicator pipeline populated for indices).
        static OHLCBar s_nq15{};
        static int64_t s_nq15_start = 0;
        const int64_t b15_n = (now_ms_n / 900000LL) * 900000LL;  // 15min
        if (s_nq15_start == 0) { s_nq15 = {b15_n/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nq15_start = b15_n; }
        else if (b15_n != s_nq15_start) {
            // -- UstecTrendFollowHtfEngine 15m-close dispatch (S36-P4 2026-05-12) --
            {
                omega::UstecTfHtfBar tf15m{};
                tf15m.bar_start_ms = s_nq15_start;
                tf15m.open  = s_nq15.open;
                tf15m.high  = s_nq15.high;
                tf15m.low   = s_nq15.low;
                tf15m.close = s_nq15.close;
            }
            s_nq15 = {b15_n/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nq15_start = b15_n;
        }
        else { if(nq_mid>s_nq15.high)s_nq15.high=nq_mid; if(nq_mid<s_nq15.low)s_nq15.low=nq_mid; s_nq15.close=nq_mid; }
        // H1 -- HTF swing context for IndexSwingEngine
        static OHLCBar s_nqh1{};
        static int64_t s_nqh1_start = 0;
        const int64_t bh1_n = (now_ms_n / 3600000LL) * 3600000LL;
        if (s_nqh1_start == 0) { s_nqh1 = {bh1_n/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nqh1_start = bh1_n; }
        else if (bh1_n != s_nqh1_start) {
            g_bars_nq.h1.add_bar(s_nqh1);
            // ── S136 2026-05-24: NasBbRevLongH1Engine H1-close dispatch ────────
            // Engine uses its own internal aggregator (Bollinger / RSI / ATR);
            // we hand it the just-closed H1 bar. ts_ms = bar START (close = +1h).
            g_nas_bbrev_long_h1.on_h1_bar(s_nqh1.high, s_nqh1.low, s_nqh1.close,
                                           bid, ask, s_nqh1_start, ca_on_close);
            s_nqh1 = {bh1_n/60000LL,nq_mid,nq_mid,nq_mid,nq_mid}; s_nqh1_start = bh1_n;
        }
        else { if(nq_mid>s_nqh1.high)s_nqh1.high=nq_mid; if(nq_mid<s_nqh1.low)s_nqh1.low=nq_mid; s_nqh1.close=nq_mid; }
        // NOTE: L2 tick CSV logger for USTEC is at on_tick.hpp router level
        // (before indices_enabled gate), not here -- see US500 block above.
    }
    const bool base_can_nq = symbol_gate("USTEC.F",
        g_eng_nq.pos.active                  ||
        g_bracket_nq.pos.active, "", tradeable, lat_ok, regime, bid, ask)
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
    (void)sdec_nq;
    // SIM: NQ breakout WR 26.1% -$1167. Disabled.
    // if (sdec_nq.allow_breakout && !g_bracket_nq.pos.active)
    //     dispatch(g_eng_nq, g_sup_nq, base_can_nq, &sdec_nq);
    // SIM: BracketEngine on indices -- no edge. Disabled.
    // if (sdec_nq.allow_bracket && !g_eng_nq.pos.active)
    //     dispatch_bracket(g_bracket_nq, ...);
    // ?? USTEC.F manage blocks -- ALWAYS run when position open ??
    // Seed TrendPB NQ with real M1 bar EMAs
    if (g_bars_nq.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
        {
            const double nq_s_e9  = g_bars_nq.m1.ind.ema9 .load(std::memory_order_relaxed);
            const double nq_s_e50 = g_bars_nq.m1.ind.ema50.load(std::memory_order_relaxed);
            const int nq_ema_trend = (nq_s_e9 > 0.0 && nq_s_e50 > 0.0)
                ? (nq_s_e9 < nq_s_e50 ? -1 : +1) : 0;
            (void)nq_ema_trend;
        }
    }
    // UstecTrendFollow5mEngine -- Donchian N=20 trend-follow on 5m bars (S33d).
    // Shadow-only by default. Position management every tick.
    {
        const int64_t tf_now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        (void)tf_now_ms;
    }
    // UstecTrendFollowHtfEngine -- 3-cell M15/H1/H2/H4 ensemble (S36-P4 2026-05-12).
    // Shadow-only by default. Intra-bar SL/TP/BE/trail management every tick.
    {
        const int64_t tf_htf_now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        (void)tf_htf_now_ms;
    }

    // VWAP Reversion NQ -- anchored to NY open (13:30 UTC)
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
        (void)nq_trendpb_ok;
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
        (void)nq_nbm_ok;
    }

    // ----------------------------------------------------------------------
    // HBI-NQ fully retired S12 P3c (2026-05-07): IndexHybridBracketEngine[USTEC.F]
    //   - Dispatch removed S10 P3a (commit ba5f0e9)
    //   - Globals decl + init/heartbeat/gate-reads removed S11 P3b (commit a6e3403)
    //   - Header file include/IndexHybridBracketEngine.hpp DELETED + #include
    //     refs removed S12 P3c (this commit)
    //   Original dispatch was lines ~562-614 of tick_indices.hpp pre-S10.
    // ----------------------------------------------------------------------
    // ?? IndexFlowEngine -- USTEC.F
    {
        const double nq_l2_imb = g_macro_ctx.nq_l2_imbalance;
        if (g_iflow_nq.has_open_position()) {
            g_iflow_nq.on_tick(sym, bid, ask, nq_l2_imb, ca_on_close, false);
        } else if (!g_disable_index_flow  // 2026-04-30 audit-disable
                   && base_can_nq
                   && !g_eng_nq.pos.active
                   // S11 P3b: g_hybrid_nq gate removed -- engine culled in P3a, dispatch dormant.
                   // Bug #3 (KNOWN_BUGS.md): cross-symbol concurrent block + post-close gap.
                   && !index_any_open()
                   && !omega::idx::idx_recent_close_block()) {
            const auto isig = g_iflow_nq.on_tick(sym, bid, ask, nq_l2_imb, ca_on_close, true);
            if (isig.valid) {
                g_telemetry.UpdateLastSignal("USTEC.F", isig.is_long?"LONG":"SHORT",
                    isig.entry, isig.reason, "IFLOW", regime.c_str(), "IndexFlow",
                    isig.tp, isig.sl);
                if (!enter_directional("USTEC.F", isig.is_long, isig.entry,
                                       isig.sl, isig.tp, isig.size, true, bid, ask, sym, regime, "IndexFlow"))
                    g_iflow_nq.patch_size(0.0);
                else g_iflow_nq.patch_size(g_last_directional_lot);
            }
        }
    }
    // ── IndexMacroCrash USTEC.F (shadow only) ───────────────────────────────
    {
        static double s_atr_base_nq = 0.0;
        const double cur_atr_nq = g_iflow_nq.atr();
        if (s_atr_base_nq <= 0.0) {
            if (cur_atr_nq > 0.0) s_atr_base_nq = cur_atr_nq;
        } else {
            s_atr_base_nq = 0.999 * s_atr_base_nq + 0.001 * cur_atr_nq;
        }
        const double nq_vol_ratio = (s_atr_base_nq > 0.0)
            ? (cur_atr_nq / s_atr_base_nq) : 1.0;
        const bool nq_trend_regime = g_iflow_nq.is_trending();
        g_imacro_nq.on_tick(bid, ask, cur_atr_nq, g_iflow_nq.drift(),
                            nq_vol_ratio, nq_trend_regime, ca_on_close);
    }
    // ── IndexSwingEngine -- USTEC.F H1+H4 swing entries (shadow mode) ───────
    // 2026-05-30 BUGFIX: this block was previously inside on_tick_nas100,
    // feeding NAS100 bid/ask into g_iswing_nq (the USTEC.F instance).
    // Symptom: USTEC.F entries logged at NAS100 prices (e.g. 30237.50 when
    // USTEC was 30315). Discovered via iswing_replay trade-9 anomaly.
    {
        auto swing_nq_cb = [&](const omega::TradeRecord& tr) {
            // Shadow mode: call handle_closed_trade directly -- NOT ca_on_close.
            // ca_on_close sends a live market order to close; this engine never
            // opened a broker position so sending a close order would be a rogue order.
            handle_closed_trade(tr);
            printf("[ISWING-CB] USTEC.F %s pnl=%.2f why=%s\n",
                   tr.side.c_str(), tr.pnl, tr.exitReason.c_str());
            fflush(stdout);
        };
        const bool sw_nq_entered = g_iswing_nq.on_tick(bid, ask, g_bars_nq.h1, g_bars_nq.h4,
                            g_iflow_nq.drift(), swing_nq_cb);
        if (sw_nq_entered)
            g_telemetry.UpdateLastSignal("USTEC.F",
                g_iswing_nq.is_long_at_entry() ? "LONG" : "SHORT",
                g_iswing_nq.entry_price(), "H1+H4 EMA swing",
                "ISWING", regime.c_str(), "IndexSwing",
                0.0, g_iswing_nq.sl_price());
    }
}

// ── DJ30.F ─────────────────────────────────────────────────
static void on_tick_dj30(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    { static IdxH1Agg agg; index_feed_h1(agg, "DJ30", bid, ask); }  // BE-floor companion H1 feed
    {   // S-2026-07-12b BE-cascade port (shadow)
        const int64_t bc_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
        g_xsbec_dj30.on_tick(bid, ask, bc_ms); g_engine_heartbeat.pulse("XsBeCascade_DJ30.F");
    }
    // 2026-05-05 (audit-fixes-40): heartbeat pulses for every DJ30-driven engine.
    // S11 P3b: HybridUS30 pulse removed (engine culled in P3a + globals/init removed in P3b).
    g_engine_heartbeat.pulse("IFlowUS30");
    g_engine_heartbeat.pulse("IMacroUS30");
    g_engine_heartbeat.pulse("MinimalH4US30");
    // S-2026-06-20 Connors MR breadth book on DJ30 (IBS/RSI2/DOUBLE, close>SMA200, shadow).
    {
        const int64_t conn_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
        g_ibs_dj.on_tick(bid, ask, conn_ms);   g_engine_heartbeat.pulse("ConnorsIBS_DJ");
        g_rsi2_dj.on_tick(bid, ask, conn_ms);  g_engine_heartbeat.pulse("ConnorsRSI2_DJ");
        g_dbl_dj.on_tick(bid, ask, conn_ms);   g_engine_heartbeat.pulse("ConnorsDouble_DJ");
    }
    g_engine_heartbeat.pulse("Us30Ensemble");        // 2026-05-26 (Stage 4)
    g_engine_heartbeat.pulse("Us30_3BarMomH1");      // 2026-05-26 (Stage 4)
    g_engine_heartbeat.pulse("OrbDj30");             // 2026-05-26 (Stage 4)

    const bool base_can_us30 = symbol_gate("DJ30.F",
        g_eng_us30.pos.active      ||
        g_bracket_us30.pos.active, "", tradeable, lat_ok, regime, bid, ask)
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

    // 2026-05-23: ORB-Swing DJ30 (NY 13:30 UTC, 30-min range, ~0.55% TP,
    //   6h hold). See on_tick_nas100 comment for design rationale.

    // NoiseBandMomentum DJ30
    {
        const int slot_us30 = g_macro_ctx.session_slot;
        const bool us30_session_ok = (slot_us30 >= 1 && slot_us30 <= 5);
        (void)us30_session_ok;
    }

    // ----------------------------------------------------------------------
    // HBI-US30 fully retired S12 P3c (2026-05-07): IndexHybridBracketEngine[DJ30.F]
    //   - Dispatch removed S10 P3a (commit ba5f0e9)
    //   - Globals decl + init/heartbeat/gate-reads removed S11 P3b (commit a6e3403)
    //   - Header file include/IndexHybridBracketEngine.hpp DELETED + #include
    //     refs removed S12 P3c (this commit)
    //   Original dispatch was lines ~712-762 of tick_indices.hpp pre-S10.
    // ----------------------------------------------------------------------
    // ?? IndexFlowEngine -- DJ30.F
    {
        const double us30_l2_imb = g_macro_ctx.us30_l2_imbalance;
        if (g_iflow_us30.has_open_position()) {
            g_iflow_us30.on_tick(sym, bid, ask, us30_l2_imb, ca_on_close, false);
        } else if (!g_disable_index_flow  // 2026-04-30 audit-disable
                   && base_can_us30
                   && !g_eng_us30.pos.active
                   // S11 P3b: g_hybrid_us30 gate removed -- engine culled in P3a, dispatch dormant.
                   // Bug #3 (KNOWN_BUGS.md): cross-symbol concurrent block + post-close gap.
                   && !index_any_open()
                   && !omega::idx::idx_recent_close_block()) {
            const auto isig = g_iflow_us30.on_tick(sym, bid, ask, us30_l2_imb, ca_on_close, true);
            if (isig.valid) {
                g_telemetry.UpdateLastSignal("DJ30.F", isig.is_long?"LONG":"SHORT",
                    isig.entry, isig.reason, "IFLOW", regime.c_str(), "IndexFlow",
                    isig.tp, isig.sl);
                if (!enter_directional("DJ30.F", isig.is_long, isig.entry,
                                       isig.sl, isig.tp, isig.size, true, bid, ask, sym, regime, "IndexFlow"))
                    g_iflow_us30.patch_size(0.0);
                else g_iflow_us30.patch_size(g_last_directional_lot);
            }
        }
    }
    // ── IndexMacroCrash DJ30.F (shadow only) ────────────────────────────────
    {
        static double s_atr_base_us30 = 0.0;
        const double cur_atr_us30 = g_iflow_us30.atr();
        if (s_atr_base_us30 <= 0.0) {
            if (cur_atr_us30 > 0.0) s_atr_base_us30 = cur_atr_us30;
        } else {
            s_atr_base_us30 = 0.999 * s_atr_base_us30 + 0.001 * cur_atr_us30;
        }
        const double us30_vol_ratio = (s_atr_base_us30 > 0.0)
            ? (cur_atr_us30 / s_atr_base_us30) : 1.0;
        const bool us30_trend_regime = g_iflow_us30.is_trending();
        g_imacro_us30.on_tick(bid, ask, cur_atr_us30, g_iflow_us30.drift(),
                              us30_vol_ratio, us30_trend_regime, ca_on_close);
    }

    // ?? MinimalH4US30Breakout -- pure H4 Donchian breakout (shadow mode) ???????
    // Self-contained: engine builds its own H4 OHLC bars + ATR14 from ticks.
    // Independent of all other DJ30.F engines (shadow only, no broker orders).
    // Validated 27/27 profitable on 2yr Tickstory sweep. No mutex with other
    // engines because shadow_mode trades don't touch broker positions.
    // When promoted to live, add an enter_directional() call mirroring the
    // IFLOW pattern above, gated by base_can_us30 + position-mutex check.

    // ── S136 2026-05-24: Us303BarMomH1Engine ─────────────────────────────────
    // Build US30 H1 bars from ticks here (no shared g_bars_us30.h1 stream),
    // dispatch to engine on bar close, and run per-tick management every call.
    {
        const double us30_mid = (bid + ask) * 0.5;
        const int64_t now_ms_u = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const int64_t bh1_u = (now_ms_u / 3600000LL) * 3600000LL;
        static double s_us30_o = 0, s_us30_h = 0, s_us30_l = 0, s_us30_c = 0;
        static int64_t s_us30h1_start = 0;
        if (s_us30h1_start == 0) {
            s_us30_o = s_us30_h = s_us30_l = s_us30_c = us30_mid;
            s_us30h1_start = bh1_u;
        } else if (bh1_u != s_us30h1_start) {
            // bar closed at s_us30h1_start, new bar starting at bh1_u
            s_us30_o = s_us30_h = s_us30_l = s_us30_c = us30_mid;
            s_us30h1_start = bh1_u;
        } else {
            if (us30_mid > s_us30_h) s_us30_h = us30_mid;
            if (us30_mid < s_us30_l) s_us30_l = us30_mid;
            s_us30_c = us30_mid;
        }
    }

    // ── S37 2026-05-26: Us30EnsembleEngine ────────────────────────────────────
    // Build US30 M15 bars from ticks; engine synthesizes M30/H1/H4 internally.
    // Feeds 4-cell ensemble (atr_exp H1, inside_brk H1, atr_exp M30, ema_pb H4).
    // Shadow-only by default. Intra-bar SL/TP management runs every tick.
    {
        const double us30_mid_e = (bid + ask) * 0.5;
        const int64_t now_ms_e = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const int64_t b15_e = (now_ms_e / 900000LL) * 900000LL;  // 15min
        static omega::Us30EnsembleBar s_us30_15{};
        static int64_t s_us30_15_start = 0;
        if (s_us30_15_start == 0) {
            s_us30_15.bar_start_ms = b15_e;
            s_us30_15.open = s_us30_15.high = s_us30_15.low = s_us30_15.close = us30_mid_e;
            s_us30_15_start = b15_e;
        } else if (b15_e != s_us30_15_start) {
            // bar just closed at s_us30_15_start; dispatch then start fresh
            s_us30_15.bar_start_ms = b15_e;
            s_us30_15.open = s_us30_15.high = s_us30_15.low = s_us30_15.close = us30_mid_e;
            s_us30_15_start = b15_e;
        } else {
            if (us30_mid_e > s_us30_15.high) s_us30_15.high = us30_mid_e;
            if (us30_mid_e < s_us30_15.low)  s_us30_15.low  = us30_mid_e;
            s_us30_15.close = us30_mid_e;
        }
    }

    // -- IndexIntradayDriftEngine (S37-Z 2026-05-28) -------------------------
    // Audit verdict on USA30 corpus Oct-2025..Apr-2026:
    //   Sharpe +1.31 net, WF1 +1.04, WF2 +1.53 -- strongest of basket.
    {
        const int64_t now_ms_idd = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        (void)now_ms_idd;
    }

    // DJ30 D1 turtle (2026-06-15) -- NasTurtleD1 chassis, Yahoo-daily xregime
    // PF2.09 both-halves+ (incl 2022 bear), cost-incl. Self-aggregates D1; shadow.
    {
        const int64_t now_ms_dt = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_engine_heartbeat.pulse("Dj30TurtleD1");  // S-2026-06-29 ENABLED+NO_PULSE fix
        const auto dtsig = g_dj30_turtle_d1.on_tick(bid, ask, now_ms_dt, ca_on_close);
        if (dtsig.valid) {
            g_telemetry.UpdateLastSignal("DJ30.F", "LONG", dtsig.entry, dtsig.reason,
                "DJ30_TURTLE_D1", "", "DJ30_TURTLE_D1", dtsig.tp, dtsig.sl);
        }
        if (g_dj30_turtle_d1.has_open_position())
            g_dj30_turtle_d1.check_weekend_close(bid, ask, now_ms_dt, ca_on_close);
    }
}

// ── GER40 ──────────────────────────────────────────────────
static void on_tick_ger40(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    { static IdxH1Agg agg; index_feed_h1(agg, "GER40", bid, ask); }  // BE-floor companion H1 feed
    // 2026-05-05 (audit-fixes-40): heartbeat pulse for GER40-driven engines.
    g_engine_heartbeat.pulse("Ger40");
    g_engine_heartbeat.pulse("AmrGer40");            // 2026-05-26 (Stage 4)
    g_engine_heartbeat.pulse("VwapRevGer40");        // 2026-05-26 (Stage 4)
    g_engine_heartbeat.pulse("Ger40LondonBrk");      // 2026-05-26 (Stage 4)
    g_engine_heartbeat.pulse("Ger40TurtleH4");       // 2026-05-26 (Stage 4)
    g_engine_heartbeat.pulse("MinimalH4Ger40");      // 2026-05-26 (Stage 4)
    // S-2026-06-19 v2: ConnorsRSI2 GER40 daily mean-reversion (shadow, CET session).
    g_engine_heartbeat.pulse("ConnorsRSI2_GER");
    g_connors_ger.on_tick(bid, ask, static_cast<int64_t>(std::time(nullptr)) * 1000);

    {   // S-2026-06-02 index straddle cells (self-aggregating M30/M15, shadow)
        const int64_t now_ms_str = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_idx_straddle_ger40_m30.on_tick_agg(bid, ask, now_ms_str, bracket_on_close);
        g_idx_straddle_ger40_m15.on_tick_agg(bid, ask, now_ms_str, bracket_on_close);
        g_engine_heartbeat.pulse("IdxStraddleGER40");
        // PeachyOrb GER40 removed 2026-06-10 (failed OOS, net-negative — see engine_init).
    }

    // AtrMeanRevGrid GER40 (shadow). M15 X=14 SL_Y=6 ATR_FROM_WAP, PF 1.86 stage-4.

    const bool base_can_ger = symbol_gate("GER40",
        g_eng_ger30.pos.active              ||
        g_bracket_ger30.pos.active, "", tradeable, lat_ok, regime, bid, ask);
    const auto sdec_ger = sup_decision(g_sup_ger30, g_eng_ger30, base_can_ger, sym, bid, ask);
    // ?? GER40 manage blocks -- ALWAYS run when position open ??

    // GER40 NEW ENTRIES DISABLED -- taken out of play (ORB/TrendPullback/breakout)
    //
    // P1-6 (S18): g_trend_pb_ger40 is intentionally NOT wired with
    // seed_bar_emas() or seed_m5_trend() here because new entries are blocked
    // (engine_init.hpp:824 sets enabled=false; "GER40 NEW ENTRIES DISABLED"
    // above also gates the dispatch). The manage-only path at line above runs
    // unconditionally for any open position. If g_trend_pb_ger40.enabled is
    // ever flipped to true (i.e., GER40 re-validated for live trading), this
    // handler MUST be extended to mirror the SP/NQ pattern at lines ~122-135
    // (call seed_bar_emas + seed_m5_trend from g_bars_ger40.m1 indicators
    // gated on g_bars_ger40.m1.ind.m1_ready). Without that wiring,
    // m_using_bar_emas_ stays false → tick EMAs are used instead of bar EMAs,
    // and m5_trend_state_ stays 0 → the M5 trend gate is permissive (allows
    // any direction). Both effects make the engine fire more loosely than SP/NQ
    // do, which is fine for shadow but not for live without re-validation.
    (void)sdec_ger;

    // ── Ger40LondonBreakoutEngine -- self-managing, independent position ─────
    // Asian range break-below at London open. SHORT only. Does NOT compete with
    // other GER40 engines for the shared position slot -- manages its own pos.
    {
        const int64_t now_ms_glb = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        (void)now_ms_glb;
    }
    // IndexSessionEngine GER40 (09-20 UTC LONG, flat overnight, risk-off gated).
    {
        const int64_t now_ms_isg = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_idxsess_ger40.set_risk_off(omega::index_risk_off());
        g_idxsess_ger40.on_tick(bid, ask, now_ms_isg);
        g_engine_heartbeat.pulse("IndexSession_GER40");
    }

    // MinimalH4GER40Breakout -- pure H4 Donchian (shadow mode).
    // Self-contained: builds own H4 OHLC + ATR14 from tick stream.
    // Independent of other GER40 engines (shadow only, no broker orders).
    // Ger40TurtleH4Engine (2026-05-20) -- 20-bar Donchian breakout, shadow.
    // Self-contained: own H4 OHLC + ATR14 from ticks. Distinct lookback (20)
    // from MinimalH4GER40 (8) and longer hold (20 H4 bars). Independent pos.
    // Ger40KeltnerH1Engine (S41 2026-05-30) -- H1 Keltner EMA20 break, LB200
    // bull gate, shadow. Self-aggregates H1 from ticks via feed_tick() (no
    // g_bars_ger40 in the codebase, same as the turtle above). Independent pos.
    {
        const int64_t now_ms_gk = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        (void)now_ms_gk;
        // S42 fix: pulse the heartbeat (registered live_required in engine_init
        // as "Ger40KeltnerH1" but never pulsed -> false HEARTBEAT-MISS spam).
        g_engine_heartbeat.pulse("Ger40KeltnerH1");
    }
}

// ── UK100 ──────────────────────────────────────────────────
static void on_tick_uk100(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    // 2026-05-05 (audit-fixes-40): heartbeat pulse for UK100-driven engines.
    g_engine_heartbeat.pulse("Uk100");

    {   // S-2026-06-02 index straddle cells (self-aggregating M30/M240, shadow)
        const int64_t now_ms_str = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_idx_straddle_uk100_m30.on_tick_agg(bid, ask, now_ms_str, bracket_on_close);
        g_idx_straddle_uk100_m240.on_tick_agg(bid, ask, now_ms_str, bracket_on_close);
        g_engine_heartbeat.pulse("IdxStraddleUK100");
    }

    const bool base_can_uk = symbol_gate("UK100", g_eng_uk100.pos.active || g_bracket_uk100.pos.active, "", tradeable, lat_ok, regime, bid, ask);
    const auto sdec_uk = sup_decision(g_sup_uk100, g_eng_uk100, base_can_uk, sym, bid, ask);
    // SIM: EU index breakout -- no edge. Disabled.
    // if (sdec_uk.allow_breakout && !g_bracket_uk100.pos.active)
    //     dispatch(g_eng_uk100, g_sup_uk100, base_can_uk, &sdec_uk);
    // ?? UK100 manage block -- ALWAYS run when position open ??

    // Opening range breakout: LSE open 08:00 UTC, 15-min range window
    (void)sdec_uk;

    // -- IndexIntradayDriftEngine (S37-Z 2026-05-28) -------------------------
    // Audit verdict on GBRIDXGBP corpus 2025-01..2025-12:
    //   Sharpe +1.12 net, WF1 +1.01, WF2 +1.46 -- both halves >+1.0.
    {
        const int64_t now_ms_idd = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        (void)now_ms_idd;
    }
    // IndexSessionEngine UK100/FTSE (09-20 UTC LONG, dip-buy, risk-off gated).
    {
        const int64_t now_ms_isu = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_idxsess_uk100.set_risk_off(omega::index_risk_off());
        g_idxsess_uk100.on_tick(bid, ask, now_ms_isu);
        g_engine_heartbeat.pulse("IndexSession_UK100");
    }
}

// ── ESTX50 ─────────────────────────────────────────────────
static void on_tick_estx50(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    // 2026-05-05 (audit-fixes-40): heartbeat pulse for ESTX50-driven engines.
    g_engine_heartbeat.pulse("Estx50");

    const bool base_can_estx = symbol_gate("ESTX50", g_eng_estx50.pos.active || g_bracket_estx50.pos.active, "", tradeable, lat_ok, regime, bid, ask);
    const auto sdec_estx = sup_decision(g_sup_estx50, g_eng_estx50, base_can_estx, sym, bid, ask);
    // SIM: EU index breakout -- no edge. Disabled.
    // if (sdec_estx.allow_breakout && !g_bracket_estx50.pos.active)
    //     dispatch(g_eng_estx50, g_sup_estx50, base_can_estx, &sdec_estx);
    // ?? ESTX50 manage block -- ALWAYS run when position open ??

    // Opening range breakout: Euronext open 09:00 UTC, 15-min range window
    (void)sdec_estx;

    // S-2026-06-02: faithful ESTX50 long-only ORB (g_orb_estx50_v2) -- the one
    // OOS-robust survivor of the multi-symbol ORB sweep. Self-aggregating m5
    // SHADOW cell (reports closed trades via bracket_on_close -> ledger/gate,
    // never the live order path). Always run so it builds its own bars + manages.
    {
        const int64_t now_ms_orb = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        (void)now_ms_orb;
        g_engine_heartbeat.pulse("OrbEstx50");
    }

    // IndexSessionEngine ESTX50/Euro Stoxx 50 (09-20 UTC LONG, dip-buy, risk-off).
    {
        const int64_t now_ms_ise = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_idxsess_estx50.set_risk_off(omega::index_risk_off());
        g_idxsess_estx50.on_tick(bid, ask, now_ms_ise);
        g_engine_heartbeat.pulse("IndexSession_ESTX50");
    }
}

// ── NAS100 ─────────────────────────────────────────────────
static void on_tick_nas100(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime)
{
    { static IdxH1Agg agg; index_feed_h1(agg, "NAS100", bid, ask); }  // BE-floor companion H1 feed
    // 2026-05-05 (audit-fixes-40): heartbeat pulses for every NAS100-driven engine.
    // S11 P3b: HybridNAS100 pulse removed (engine culled in P3a + globals/init removed in P3b).
    g_engine_heartbeat.pulse("IFlowNAS100");
    g_engine_heartbeat.pulse("IMacroNAS");
    g_engine_heartbeat.pulse("AmrNas100");           // 2026-05-26 (Stage 4)
    g_engine_heartbeat.pulse("OrbNas100");           // 2026-05-26 (Stage 4)
    // S-2026-06-19: ConnorsRSI2 NAS100 daily mean-reversion (shadow). Self-detects the
    // cash-close transition (ET RTH) internally; just feed every NAS100 tick.
    g_engine_heartbeat.pulse("ConnorsRSI2");
    g_engine_heartbeat.pulse("ConnorsNas");   // S-2026-06-29 ENABLED+NO_PULSE fix (handle-name match for contract check)
    {
        const int64_t conn_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
        g_connors_nas.on_tick(bid, ask, conn_ms);
        // S-2026-06-19 v3 MR family (NAS100): IBS / STREAK / DOUBLE (shadow).
        g_ibs_nas.on_tick(bid, ask, conn_ms);    g_engine_heartbeat.pulse("ConnorsIBS_NAS");
        g_streak_nas.on_tick(bid, ask, conn_ms); g_engine_heartbeat.pulse("ConnorsStreak_NAS");
        g_dbl_nas.on_tick(bid, ask, conn_ms);    g_engine_heartbeat.pulse("ConnorsDouble_NAS");
        g_rsi3_nas.on_tick(bid, ask, conn_ms);   g_engine_heartbeat.pulse("ConnorsRSI3_NAS");  // S-2026-06-20 breadth
    }

    // 2026-06-12: feed the market-bear PROXY (NAS = bellwether). IndexRiskGate uses
    //   it as a price-based FALLBACK when the macro VIX/credit/dollar feed is dead,
    //   so index long engines stay protected in a real bear with no feed. Price-only
    //   => can't silently degrade to all-clear. See RegimeState.hpp / IndexRiskGate.hpp.
    omega::index_market_regime().on_tick(bid, ask, omega::pg::_pg_now_ms());

    {   // S-2026-06-02 index straddle cells (self-aggregating M15/M30, shadow)
        const int64_t now_ms_str = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_idx_straddle_nas_m15.on_tick_agg(bid, ask, now_ms_str, bracket_on_close);
        g_idx_straddle_nas_m30.on_tick_agg(bid, ask, now_ms_str, bracket_on_close);
        g_engine_heartbeat.pulse("IdxStraddleNAS100");

        // 2026-06-18 NqMomentumEngine (regime-gated momentum-continuation, NAS100/NQ).
        //   Self-aggregating 5m bars from this tick path; BigCapMomo exit chassis on a
        //   liquid single instrument. Shadow. Faithful BT: gated positive both regimes.
    }
    g_engine_heartbeat.pulse("NasBbRevLongH1");      // 2026-05-26 (Stage 4)

    // AtrMeanRevGrid NAS100 (shadow). M15 X=14 SL_Y=4 RSI_OR_MA, PF 1.55 sweep.

    const bool base_can_nas = symbol_gate("NAS100",
        g_eng_nas100.pos.active      ||
        g_bracket_nas100.pos.active, "", tradeable, lat_ok, regime, bid, ask)
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

    // 2026-05-23: ORB-Swing NAS100 (NY 13:30 UTC, 30-min range, ~0.50% TP,
    //   6h hold via CrossPosition trail). Manage when open; entry path
    //   short-circuits internally on session window + armed flag. Shadow
    //   only -- standard cross-asset enter_directional() pathway not used
    //   here since the engine writes pos_ directly inside on_tick().

    // ── S136 2026-05-24: NasBbRevLongH1Engine per-tick management ─────────
    // Manages SL/TP/BE/trail on the engine's own open position. Engine entry
    // happens at H1 close (see g_nas_bbrev_long_h1.on_h1_bar dispatch above).
    {
        const int64_t now_ms_n_ll = static_cast<int64_t>(std::time(nullptr)) * 1000LL;
        g_nas_bbrev_long_h1.on_tick(bid, ask, now_ms_n_ll, ca_on_close);
    }

    // NasTurtleD1Engine (2026-06-14) -- 20-day Donchian breakout on NAS100,
    // long-only, shadow. Self-contained: own D1 OHLC + ATR14 from ticks
    // (warm-seeded). Seykota/Donchian D1 archetype; NAS validated as one of only
    // two trend horses (with XAU) in the Omega universe. Independent pos.
    {
        const int64_t now_ms_nt = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const auto ntsig = g_nas_turtle_d1.on_tick(bid, ask, now_ms_nt, ca_on_close);
        if (ntsig.valid) {
            g_telemetry.UpdateLastSignal("NAS100",
                "LONG", ntsig.entry, ntsig.reason,
                "NAS_TURTLE_D1", regime.c_str(), "NAS_TURTLE_D1",
                ntsig.tp, ntsig.sl);
        }
        if (g_nas_turtle_d1.has_open_position()) {
            g_nas_turtle_d1.check_weekend_close(bid, ask, now_ms_nt, ca_on_close);
        }
    }

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
        (void)nas_nbm_gate_ok;
    }

    // ----------------------------------------------------------------------
    // HBI-NAS fully retired S12 P3c (2026-05-07): IndexHybridBracketEngine[NAS100]
    //   - Dispatch removed S10 P3a (commit ba5f0e9)
    //   - Globals decl + init/heartbeat/gate-reads removed S11 P3b (commit a6e3403)
    //   - Header file include/IndexHybridBracketEngine.hpp DELETED + #include
    //     refs removed S12 P3c (this commit)
    //   Original dispatch (slot 3-4 tightened per 2026-04-30 audit,
    //   $154.96/32trd) was lines ~976-1048 of tick_indices.hpp pre-S10.
    // ----------------------------------------------------------------------
    // ?? IndexFlowEngine -- NAS100
    {
        const double nas_l2_imb = g_macro_ctx.nas_l2_imbalance;
        if (g_iflow_nas.has_open_position()) {
            g_iflow_nas.on_tick(sym, bid, ask, nas_l2_imb, ca_on_close, false);
        } else if (!g_disable_index_flow  // 2026-04-30 audit-disable
                   && base_can_nas
                   && !g_eng_nas100.pos.active
                   // S11 P3b: g_hybrid_nas100 gate removed -- engine culled in P3a, dispatch dormant.
                   // Bug #3 (KNOWN_BUGS.md): cross-symbol concurrent block + post-close gap.
                   && !index_any_open()
                   && !omega::idx::idx_recent_close_block()) {
            const auto isig = g_iflow_nas.on_tick(sym, bid, ask, nas_l2_imb, ca_on_close, true);
            if (isig.valid) {
                g_telemetry.UpdateLastSignal("NAS100", isig.is_long?"LONG":"SHORT",
                    isig.entry, isig.reason, "IFLOW", regime.c_str(), "IndexFlow",
                    isig.tp, isig.sl);
                if (!enter_directional("NAS100", isig.is_long, isig.entry,
                                       isig.sl, isig.tp, isig.size, true, bid, ask, sym, regime, "IndexFlow"))
                    g_iflow_nas.patch_size(0.0);
                else g_iflow_nas.patch_size(g_last_directional_lot);
            }
        }
    }
    // ── IndexMacroCrash NAS100 (shadow only) ────────────────────────────────
    {
        static double s_atr_base_nas = 0.0;
        const double cur_atr_nas = g_iflow_nas.atr();
        if (s_atr_base_nas <= 0.0) {
            if (cur_atr_nas > 0.0) s_atr_base_nas = cur_atr_nas;
        } else {
            s_atr_base_nas = 0.999 * s_atr_base_nas + 0.001 * cur_atr_nas;
        }
        const double nas_vol_ratio = (s_atr_base_nas > 0.0)
            ? (cur_atr_nas / s_atr_base_nas) : 1.0;
        const bool nas_trend_regime = g_iflow_nas.is_trending();
        g_imacro_nas.on_tick(bid, ask, cur_atr_nas, g_iflow_nas.drift(),
                             nas_vol_ratio, nas_trend_regime, ca_on_close);
    }
    // (2026-05-30 BUGFIX: g_iswing_nq block REMOVED from here -- it was
    // feeding NAS100 bid/ask into the USTEC.F IndexSwingEngine. Block
    // moved to on_tick_ustec where it belongs.)
    // IndexSessionEngine NAS100 (14-22 UTC LONG, flat overnight, risk-off gated).
    {
        const int64_t now_ms_isn = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_idxsess_nas.set_risk_off(omega::index_risk_off());
        g_idxsess_nas.on_tick(bid, ask, now_ms_isn);
        g_idx_bear_short_nas.on_tick(bid, ask, now_ms_isn); // 2026-06-12 risk-off SHORT breakdown on bad days (shadow); callback via on_close_cb
        g_engine_heartbeat.pulse("IdxBearShortNas");  // S-2026-06-29 ENABLED+NO_PULSE fix
        g_monday_nas.on_tick(bid, ask, now_ms_isn);      // 2026-06-07 Monday risk-on calendar (shadow)
        g_engine_heartbeat.pulse("IndexSession_NAS100");
    }
}
