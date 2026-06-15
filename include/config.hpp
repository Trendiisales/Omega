#pragma once
// config.hpp -- extracted from main.cpp
// Section: config (original lines 3635-3981)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static void sanitize_config() noexcept {
    auto clampd = [](double v, double lo, double hi, double fallback) {
        if (!std::isfinite(v)) return fallback;
        return std::max(lo, std::min(v, hi));
    };
    auto clampi = [](int v, int lo, int hi, int fallback) {
        if (v < lo || v > hi) return fallback;
        return v;
    };

    g_cfg.max_open_positions = clampi(g_cfg.max_open_positions, 1, 8, 1);
    g_cfg.max_consec_losses  = clampi(g_cfg.max_consec_losses, 1, 20, 3);
    g_cfg.loss_pause_sec     = clampi(g_cfg.loss_pause_sec, 10, 3600, 300);
    g_cfg.auto_disable_after_trades = clampi(g_cfg.auto_disable_after_trades, 5, 200, 10);
    g_cfg.ustec_pilot_min_gap_sec   = clampi(g_cfg.ustec_pilot_min_gap_sec, 15, 900, 60);

    g_cfg.max_latency_ms     = clampd(g_cfg.max_latency_ms, 0.0, 5000.0, 60.0);
    g_cfg.daily_loss_limit       = clampd(g_cfg.daily_loss_limit,       1.0, 1000000.0, 200.0);
    g_cfg.daily_profit_target    = clampd(g_cfg.daily_profit_target,    0.0, 1000000.0, 0.0);
    g_cfg.max_loss_per_trade_usd     = clampd(g_cfg.max_loss_per_trade_usd,     0.0, 100000.0, 0.0);
    g_cfg.max_portfolio_sl_risk_usd  = clampd(g_cfg.max_portfolio_sl_risk_usd,  0.0, 100000.0, 0.0);
    g_cfg.session_watermark_pct  = clampd(g_cfg.session_watermark_pct,  0.0, 1.0,        0.0);
    g_cfg.hourly_loss_limit      = clampd(g_cfg.hourly_loss_limit,      0.0, 1000000.0, 0.0);
    g_cfg.momentum_thresh_pct = clampd(g_cfg.momentum_thresh_pct, 0.0, 10.0, 0.05);
    g_cfg.min_breakout_pct    = clampd(g_cfg.min_breakout_pct, 0.0, 10.0, 0.25);
    g_cfg.ustec_pilot_size    = clampd(g_cfg.ustec_pilot_size, 0.05, 2.0, 0.35);

    // Risk-based sizing sanitization
    g_cfg.risk_per_trade_usd = clampd(g_cfg.risk_per_trade_usd, 0.0, 10000.0, 0.0);
    g_live_equity.store(std::max(g_cfg.account_equity, 100.0), std::memory_order_relaxed);
    g_cfg.max_lot_gold       = clampd(g_cfg.max_lot_gold,    0.01, 10.0, 0.01);  // FIX 2026-04-22 uniformity: master cap 0.50 -> 0.01 SHADOW-mode
    g_cfg.max_lot_indices    = clampd(g_cfg.max_lot_indices, 0.01, 10.0, 0.20);
    g_cfg.max_lot_oil        = clampd(g_cfg.max_lot_oil,     0.01, 10.0, 0.50);
    g_cfg.max_lot_fx         = clampd(g_cfg.max_lot_fx,      0.01, 50.0, 5.00);
    g_cfg.max_lot_gbpusd     = clampd(g_cfg.max_lot_gbpusd,  0.01, 50.0, 5.00);
    g_cfg.max_lot_audusd     = clampd(g_cfg.max_lot_audusd,  0.01, 50.0, 5.00);
    g_cfg.max_lot_nzdusd     = clampd(g_cfg.max_lot_nzdusd,  0.01, 50.0, 5.00);
    g_cfg.max_lot_usdjpy     = clampd(g_cfg.max_lot_usdjpy,  0.01, 50.0, 5.00);
    // min_lot must never exceed max_lot
    g_cfg.min_lot_gold       = clampd(g_cfg.min_lot_gold,    0.0, g_cfg.max_lot_gold,    0.01);
    g_cfg.min_lot_indices    = clampd(g_cfg.min_lot_indices, 0.0, g_cfg.max_lot_indices, 0.01);
    g_cfg.min_lot_oil        = clampd(g_cfg.min_lot_oil,     0.0, g_cfg.max_lot_oil,     0.01);
    g_cfg.min_lot_fx         = clampd(g_cfg.min_lot_fx,      0.0, g_cfg.max_lot_fx,      0.01);
    g_cfg.min_lot_gbpusd     = clampd(g_cfg.min_lot_gbpusd,  0.0, g_cfg.max_lot_gbpusd,  0.01);
    g_cfg.min_lot_audusd     = clampd(g_cfg.min_lot_audusd,  0.0, g_cfg.max_lot_audusd,  0.01);
    g_cfg.min_lot_nzdusd     = clampd(g_cfg.min_lot_nzdusd,  0.0, g_cfg.max_lot_nzdusd,  0.01);
    g_cfg.min_lot_usdjpy     = clampd(g_cfg.min_lot_usdjpy,  0.0, g_cfg.max_lot_usdjpy,  0.01);

    g_cfg.sp_vol_thresh_pct   = clampd(g_cfg.sp_vol_thresh_pct, 0.0, 10.0, 0.04);
    g_cfg.nq_vol_thresh_pct   = clampd(g_cfg.nq_vol_thresh_pct, 0.0, 10.0, 0.05);
    g_cfg.oil_vol_thresh_pct  = clampd(g_cfg.oil_vol_thresh_pct, 0.0, 10.0, 0.08);

    g_cfg.session_start_utc = clampi(g_cfg.session_start_utc, 0, 23, 7);
    g_cfg.session_end_utc   = clampi(g_cfg.session_end_utc,   0, 23, 21);

    g_cfg.ext_ger30_id   = std::max(0, g_cfg.ext_ger30_id);
    g_cfg.ext_uk100_id   = std::max(0, g_cfg.ext_uk100_id);
    g_cfg.ext_estx50_id  = std::max(0, g_cfg.ext_estx50_id);
    g_cfg.ext_eurusd_id  = std::max(0, g_cfg.ext_eurusd_id);
    g_cfg.ext_ukbrent_id = std::max(0, g_cfg.ext_ukbrent_id);
    g_cfg.ext_gbpusd_id  = std::max(0, g_cfg.ext_gbpusd_id);
    g_cfg.ext_audusd_id  = std::max(0, g_cfg.ext_audusd_id);
    g_cfg.ext_nzdusd_id  = std::max(0, g_cfg.ext_nzdusd_id);
    g_cfg.ext_usdjpy_id  = std::max(0, g_cfg.ext_usdjpy_id);

    g_ext_syms[0].id = g_cfg.ext_ger30_id;
    g_ext_syms[1].id = g_cfg.ext_uk100_id;
    g_ext_syms[2].id = g_cfg.ext_estx50_id;
    g_ext_syms[3].id = 0;  // XAGUSD removed Session 10 — slot retained for array index contract
    g_ext_syms[4].id = g_cfg.ext_eurusd_id;
    g_ext_syms[5].id = g_cfg.ext_ukbrent_id;
    g_ext_syms[6].id = g_cfg.ext_gbpusd_id;
    g_ext_syms[7].id = g_cfg.ext_audusd_id;
    g_ext_syms[8].id = g_cfg.ext_nzdusd_id;
    g_ext_syms[9].id = g_cfg.ext_usdjpy_id;

    std::cout << "[CONFIG] risk max_positions=" << g_cfg.max_open_positions
              << " max_consec_losses=" << g_cfg.max_consec_losses
              << " loss_pause_sec=" << g_cfg.loss_pause_sec << "\n";
    if (g_cfg.risk_per_trade_usd > 0.0) {
        // Print example sizes at current risk level so it's easy to verify on boot
        const double r = g_cfg.risk_per_trade_usd;
        const double oil_size  = std::floor(std::min(r / (0.478 * 1000.0),  g_cfg.max_lot_oil)     * 100 + 0.5) / 100;
        const double gold_size = std::floor(std::min(r / (10.4  * 100.0),   g_cfg.max_lot_gold)    * 100 + 0.5) / 100;
        const double sp_size   = std::floor(std::min(r / (22.3  * 50.0),    g_cfg.max_lot_indices) * 100 + 0.5) / 100;
        const double nq_size   = std::floor(std::min(r / (78.0  * 20.0),    g_cfg.max_lot_indices) * 100 + 0.5) / 100;
        const double dj_size   = std::floor(std::min(r / (40.0  * 5.0),     g_cfg.max_lot_indices) * 100 + 0.5) / 100;
        std::cout << "[CONFIG] RISK-SIZING ENABLED  risk_per_trade=$" << r << "\n"
                  << "[CONFIG]   example sizes at current risk ($" << r << " max loss per trade):\n"
                  << "[CONFIG]     USOIL.F  ~" << oil_size  << " lots  (SL?$0.48, max_loss?$"  << std::round(oil_size  * 0.478 * 1000) << ")\n"
                  << "[CONFIG]     XAUUSD   ~" << gold_size << " lots  (SL?$10.4, max_loss?$"  << std::round(gold_size * 10.4  * 100)  << ")\n"
                  << "[CONFIG]     US500.F  ~" << sp_size   << " lots  (SL?22.3pts@$50/pt, max_loss?$"  << std::round(sp_size   * 22.3  * 50)   << ")\n"
                  << "[CONFIG]     USTEC.F  ~" << nq_size   << " lots  (SL?78.0pts@$20/pt, max_loss?$"  << std::round(nq_size   * 78.0  * 20)   << ")\n"
                  << "[CONFIG]     DJ30.F   ~" << dj_size   << " lots  (SL?40.0pts@$5/pt,  max_loss?$"  << std::round(dj_size   * 40.0  * 5)    << ")\n"
                  << "[CONFIG]   caps(max): gold=" << g_cfg.max_lot_gold
                  << " idx=" << g_cfg.max_lot_indices
                  << " oil=" << g_cfg.max_lot_oil
                  << " fx=" << g_cfg.max_lot_fx << "\n"
                  << "[CONFIG]   floors(min): gold=" << g_cfg.min_lot_gold
                  << " idx=" << g_cfg.min_lot_indices
                  << " oil=" << g_cfg.min_lot_oil
                  << " fx=" << g_cfg.min_lot_fx << "\n";
    } else {
        std::cout << "[CONFIG] RISK-SIZING DISABLED (risk_per_trade_usd=0) -- using fixed fallback size 0.01 lots\n";
    }
}

static void apply_shadow_research_profile() noexcept {
    // REMOVED: shadow mode is an exact simulation of live trading.
    // Loosening session hours, throttles, or thresholds in shadow produces
    // results that do not reflect what live would do -- defeating the purpose.
    // This function is a no-op. It remains here to avoid breaking call sites.
    (void)0;
}

// ── Book-wide force-close of EVERY open position across all engine classes.
// Extracted from maybe_reset_daily_ledger 2026-06-13 (S-2026-06-13r) so the same
// proven cascade can fire on BOTH the midnight rollover AND the Friday weekend cut.
// `reason` is the TradeRecord exitReason + log label. Behaviour for the midnight
// caller is byte-identical (reason="MIDNIGHT_ROLLOVER").
// include_multiday=false (midnight): leave the D1/H4/H1 swing engines holding --
//   they carry across midnight BY DESIGN. true (weekend cut): ALSO flatten them.
static void force_close_all_open(const char* reason, bool include_multiday = false) {
    // Snapshot all prices we need under one lock
    std::unordered_map<std::string, std::pair<double,double>> px_snap;
    {
        std::lock_guard<std::mutex> lk(g_book_mtx);
        for (const auto& kv : g_bids) {
            const auto ai = g_asks.find(kv.first);
            if (ai != g_asks.end() && kv.second > 0.0 && ai->second > 0.0)
                px_snap[kv.first] = {kv.second, ai->second};
        }
    }
    auto mpx = [&](const char* sym, double& b, double& a) {
        const auto it = px_snap.find(sym);
        b = a = 0.0;
        if (it != px_snap.end()) { b = it->second.first; a = it->second.second; }
    };
    double xau_b=0, xau_a=0; mpx("XAUUSD", xau_b, xau_a);

    const std::string reason_s = reason;
    auto close_cb = [reason_s](const omega::TradeRecord& tr) {
        omega::TradeRecord t = tr;
        const double mult = tick_value_multiplier(t.symbol);
        t.pnl *= mult; t.mfe *= mult; t.mae *= mult;
        double cps = 0.0;
        { const std::string& s = t.symbol;
          if (s=="XAUUSD"||s=="EURUSD"||s=="GBPUSD"||
              s=="AUDUSD"||s=="NZDUSD"||s=="USDJPY") cps = 3.0; }
        omega::apply_realistic_costs(t, cps, mult);
        g_omegaLedger.record(t);
        g_telemetry.AccumEnginePnl(t.engine.c_str(), t.net_pnl);
        printf("[%s] Closed %s %s pnl=$%.2f reason=%s\n",
               t.exitReason.c_str(), t.symbol.c_str(), t.engine.c_str(),
               t.net_pnl, t.exitReason.c_str());
        fflush(stdout);
    };

    // -- Gold engines --
    if (xau_b > 0.0 && xau_a > 0.0) {
        if (g_trend_pb_gold.has_open_position()) {
            g_trend_pb_gold.force_close(xau_b, xau_a, close_cb, reason);
            printf("[%s] Force-closed TrendPullback gold\n", reason); fflush(stdout);
        }
        if (g_gold_stack.has_open_position()) {
            g_gold_stack.force_close(xau_b, xau_a, g_rtt_last, close_cb);
            printf("[%s] Force-closed GoldStack\n", reason); fflush(stdout);
        }
    } else {
        printf("[%s] WARNING: no XAUUSD price -- gold positions may carry\n", reason); fflush(stdout);
    }
    // -- Breakout engines (all symbols) --
    auto cls_beng = [&](auto& eng, const char* sym) {
        if (!eng.pos.active) return;
        double b=0,a=0; mpx(sym,b,a);
        if (b<=0) { b=eng.pos.entry*0.9999; a=eng.pos.entry*1.0001; }
        eng.forceClose(b, a, reason, g_rtt_last, "", close_cb);
        printf("[%s] Force-closed Breakout %s\n", reason, sym); fflush(stdout);
    };
    cls_beng(g_eng_sp,     "US500.F"); cls_beng(g_eng_nq,     "USTEC.F");
    cls_beng(g_eng_us30,   "DJ30.F");  cls_beng(g_eng_nas100, "NAS100");
    cls_beng(g_eng_ger30,  "GER40");   cls_beng(g_eng_uk100,  "UK100");
    cls_beng(g_eng_estx50, "ESTX50");  cls_beng(g_eng_cl,     "USOIL.F");
    cls_beng(g_eng_brent,  "BRENT");
    cls_beng(g_eng_eurusd, "EURUSD");  cls_beng(g_eng_gbpusd, "GBPUSD");
    cls_beng(g_eng_audusd, "AUDUSD");  cls_beng(g_eng_nzdusd, "NZDUSD");
    cls_beng(g_eng_usdjpy, "USDJPY");
    // -- Bracket engines --
    auto cls_bracket = [&](auto& eng, const char* sym) {
        if (!eng.has_open_position()) return;
        double b=0,a=0; mpx(sym,b,a);
        if (b<=0) { b=1.0; a=1.0; }
        eng.forceClose(b, a, reason, g_rtt_last, "", close_cb);
        printf("[%s] Force-closed Bracket %s\n", reason, sym); fflush(stdout);
    };
    cls_bracket(g_bracket_gold,   "XAUUSD");
    cls_bracket(g_bracket_sp,     "US500.F"); cls_bracket(g_bracket_nq,     "USTEC.F");
    cls_bracket(g_bracket_us30,   "DJ30.F");  cls_bracket(g_bracket_nas100, "NAS100");
    cls_bracket(g_bracket_ger30,  "GER40");   cls_bracket(g_bracket_uk100,  "UK100");
    cls_bracket(g_bracket_estx50, "ESTX50");  cls_bracket(g_bracket_eurusd, "EURUSD");
    cls_bracket(g_bracket_gbpusd, "GBPUSD");  cls_bracket(g_bracket_audusd, "AUDUSD");
    cls_bracket(g_bracket_nzdusd, "NZDUSD");  cls_bracket(g_bracket_usdjpy, "USDJPY");
    cls_bracket(g_bracket_brent,  "BRENT");
    // -- TrendPullback non-gold --
    auto cls_tpb = [&](auto& eng, const char* sym) {
        if (!eng.has_open_position()) return;
        double b=0,a=0; mpx(sym,b,a);
        if (b<=0) return;
        eng.force_close(b, a, close_cb, reason);
        printf("[%s] Force-closed TrendPullback %s\n", reason, sym); fflush(stdout);
    };
    cls_tpb(g_trend_pb_sp,    "US500.F");
    cls_tpb(g_trend_pb_nq,    "USTEC.F");
    cls_tpb(g_trend_pb_ger40, "GER40");
    // -- NBM / ORB / VWAP / CrossAsset --
    auto cls_ca = [&](auto& eng, const char* sym) {
        if (!eng.has_open_position()) return;
        double b=0,a=0; mpx(sym,b,a);
        if (b<=0) return;
        eng.force_close(b, a, close_cb, reason);
        printf("[%s] Force-closed CA/NBM/ORB/VWAP %s\n", reason, sym); fflush(stdout);
    };
    cls_ca(g_nbm_sp,          "US500.F"); cls_ca(g_nbm_nq,          "USTEC.F");
    cls_ca(g_nbm_nas,         "NAS100");  cls_ca(g_nbm_us30,        "DJ30.F");
    cls_ca(g_nbm_gold_london, "XAUUSD");  cls_ca(g_nbm_oil_london,  "USOIL.F");
    cls_ca(g_orb_us,          "US500.F"); cls_ca(g_orb_ger30,       "GER40");
    cls_ca(g_orb_uk100,       "UK100");   cls_ca(g_orb_estx50,      "ESTX50");
    cls_ca(g_vwap_rev_sp,     "US500.F"); cls_ca(g_vwap_rev_nq,     "USTEC.F");
    cls_ca(g_vwap_rev_ger40,  "GER40");   cls_ca(g_vwap_rev_eurusd, "EURUSD");
    cls_ca(g_ca_esnq,         "US500.F"); cls_ca(g_ca_eia_fade,     "USOIL.F");
    cls_ca(g_ca_brent_wti,    "USOIL.F"); cls_ca(g_ca_carry_unwind, "USDJPY");

    // -- Multi-day swing engines (D1/H4/H1) -- WEEKEND ONLY (hold across midnight
    //    by design). Force-close ALL regardless of P&L (the existing per-engine
    //    weekend_close_gate only closed winners -- losers gapped). reason carries
    //    the WEEKEND_CLOSE label through each engine's force_close.
    if (include_multiday) {
        const int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000LL;
        double ger_b=0, ger_a=0; mpx("GER40", ger_b, ger_a);
        if (xau_b > 0.0 && xau_a > 0.0) {
            g_xau_tf_1h.force_close(xau_b, xau_a, now_ms, close_cb, reason);
            g_xau_tf_2h.force_close(xau_b, xau_a, now_ms, close_cb, reason);
            g_xau_tf_4h.force_close(xau_b, xau_a, now_ms, close_cb, reason);
            g_xau_tf_d1.force_close(xau_b, xau_a, now_ms, close_cb, reason);
            g_donchian.force_close_all(xau_b, xau_a, now_ms, close_cb);   // gold cells
            if (g_xau_turtle_d1.has_open_position())                     // D1 turtle (4-arg sig)
                g_xau_turtle_d1.force_close(xau_b, xau_a, close_cb, reason);
            printf("[%s] Force-closed XAU swing engines (TF 1h/2h/4h/D1 + Donchian + Turtle)\n", reason);
            fflush(stdout);
        }
        if (ger_b > 0.0 && ger_a > 0.0) {
            g_ger40_kelt.force_close(ger_b, ger_a, now_ms, close_cb, reason);
            printf("[%s] Force-closed GER40 Keltner\n", reason); fflush(stdout);
        }
    }
    std::cout.flush();
}

// ── Weekend flat (S-2026-06-13r, operator: "Friday close all pending trades, no
// weekend holds"). Window = Fri 20:45 UTC through Sun 22:00 UTC. Entry-blocked via
// symbol_gate; open positions force-closed ONCE on crossing into the window.
static inline bool weekend_flat_window() noexcept {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti{}; gmtime_s(&ti, &t);
    const int dow = ti.tm_wday;                       // 0=Sun..6=Sat
    const int mod = ti.tm_hour * 60 + ti.tm_min;      // minute-of-day UTC
    if (dow == 5 && mod >= 20*60 + 45) return true;    // Fri >= 20:45
    if (dow == 6) return true;                          // all Saturday
    if (dow == 0 && mod <  22*60)      return true;    // Sun < 22:00
    return false;
}
static int g_weekend_flat_done = -1;   // tm_yday of the Friday we last flattened

// Weekend HOLD-exemption (operator directive 2026-06-15, S-2026-06-15a): the
// longer-timeframe trend engines (H4 and slower) may carry across the weekend
// BY DESIGN -- their edge is the multi-day hold. The shorter/intraday engines
// (H1 and faster, scalps, straddles, ORB) are riskier over the gap, so they are
// still force-closed. Decided by the TF token in the per-position engine name
// (e.g. "XAU_4h_DonchN20"/"XauTrendFollowD1" hold; "GER_1h_DonchN100"/
// "Ger40KeltnerH1"/"XauStraddleM30" close). Unknown/no-TF names default to
// CLOSE (risk-eliminate). Threshold = H4: raise/lower by editing the token set.
static bool weekend_hold_exempt(const std::string& e) {
    auto has = [&](const char* s){ return e.find(s) != std::string::npos; };
    // SurvivorPortfolio tactical cells are tagged "<SYM>_<tf>_<family>" (e.g.
    // "XAU_4h_DonchN20") -- the UNDERSCORE-TF naming distinguishes them from the
    // deliberate trend engines (XauTrendFollow4h / XauTurtleD1, no "_4h"). They
    // are NOT meant to carry the weekend gap (a survivor XAU_4h_Donch SHORT held
    // 70h over a weekend for -$111). Never exempt an underscore-TF cell.
    if (has("_4h")||has("_4H")||has("_6h")||has("_8h")||has("_12h")||has("_d1")||has("_D1")) return false;
    return has("D1") || has("d1") || has("Daily") || has("daily") || has("Turtle")
        || has("4h") || has("4H") || has("H4")
        || has("6h") || has("H6") || has("8h") || has("H8")
        || has("12h") || has("H12") || has("W1");
}
static void maybe_weekend_flat() {
    if (!weekend_flat_window()) return;
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti{}; gmtime_s(&ti, &t);

    // (1) One-shot HEAVY cascade at the Friday 20:45 cut -- force-closes the
    //     hardcoded SHORT-TERM engine set (breakout/bracket/trendpb/CA). The
    //     longer-TF swing engines (H4/D1) are NOT bulk-closed here (S-2026-06-15a,
    //     include_multiday=false) -- they hold across the weekend by design; the
    //     SHORTER swing cells (H1 Donchian/Keltner/1h TF) are caught + closed by
    //     the per-name registry sweep below (weekend_hold_exempt gate).
    if (ti.tm_wday == 5 && g_weekend_flat_done != ti.tm_yday) {
        g_weekend_flat_done = ti.tm_yday;
        printf("[WEEKEND-FLAT] Fri 20:45 UTC cut -- force-closing short-term positions; "
               "H4+ trend engines hold; blocking new entries until Sun 22:00 UTC\n"); fflush(stdout);
        force_close_all_open("WEEKEND_CLOSE", /*include_multiday=*/false);
    }

    // (2) CONTINUOUS registry sweep (throttled 60 s) through the ENTIRE weekend
    //     window -- guarantees NOTHING is ever held over the weekend, catching any
    //     straggler the hardcoded cascade cannot reach (straddles, scalpers, etc.).
    //     Closes each open position via its engine's registered closer; loudly logs
    //     anything that has NO closer wired so the gap is never silent. Operator
    //     directive: weekend holds are NOT allowed.
    static int64_t s_last_sweep = 0;
    const int64_t now_s = (int64_t)std::time(nullptr);
    if (now_s - s_last_sweep < 60) return;
    s_last_sweep = now_s;
    const auto open = g_open_positions.snapshot_all();
    for (const auto& ps : open) {
        if (weekend_hold_exempt(ps.engine)) {       // S-2026-06-15a: H4+ trend engines hold
            printf("[WEEKEND-FLAT] HELD over weekend (longer-TF trend): %s %s\n",
                   ps.engine.c_str(), ps.symbol.c_str());
            fflush(stdout);
            continue;
        }
        if (g_open_positions.close_matching(ps, "WEEKEND_CLOSE"))
            printf("[WEEKEND-FLAT] swept %s %s (registry)\n",
                   ps.engine.c_str(), ps.symbol.c_str());
        else
            printf("[WEEKEND-FLAT] WARN uncloseable over weekend: %s %s -- no closer wired\n",
                   ps.engine.c_str(), ps.symbol.c_str());
        fflush(stdout);
    }
}

static void maybe_reset_daily_ledger() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti{};
    gmtime_s(&ti, &t);
    if (g_last_ledger_utc_day < 0) {
        g_last_ledger_utc_day = ti.tm_yday;
        return;
    }
    if (ti.tm_yday == g_last_ledger_utc_day) return;
    g_last_ledger_utc_day = ti.tm_yday;

    // MIDNIGHT FORCE-CLOSE: close ALL open positions before ledger reset. ROOT CAUSE
    // of the recurring $844 bug -- a position surviving midnight in engine memory
    // closes hours into the new day and posts phantom PnL into the fresh ledger.
    // Now delegates to the shared cascade (also used by the Friday weekend cut).
    force_close_all_open("MIDNIGHT_ROLLOVER");

    // Snapshot session PnL BEFORE reset -- multiday throttle needs this.
    // resetDaily() zeroes the ledger. We must capture the final value first.
    const double session_final_pnl = g_omegaLedger.dailyPnl();

    g_omegaLedger.resetDaily();
    g_open_unrealised_cents.store(0);  // FIX: reset floating P&L at midnight
    {
        std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
        g_sym_risk.clear();
        g_shadow_quality.clear();
    }
    // (g_gf_engine_culled midnight reset removed S19 Stage 1B — atomic removed)
    {
        std::lock_guard<std::mutex> lk(g_perf_mtx);
        g_perf.clear();
    }
    g_disable_gold_stack = false;
    g_gov_spread = g_gov_lat = g_gov_pnl = g_gov_pos = g_gov_consec = 0;
    // Refresh live economic calendar once per day
    g_live_calendar.check_and_refresh(g_news_blackout, static_cast<int64_t>(std::time(nullptr)));
    // Prune expired hourly P&L records (belt-and-suspenders; also pruned on insert)
    { std::lock_guard<std::mutex> lk(g_hourly_pnl_mtx);
      const int64_t cutoff = static_cast<int64_t>(std::time(nullptr)) - HOURLY_WINDOW_SEC;
      while (!g_hourly_pnl_records.empty() && g_hourly_pnl_records.front().ts_sec < cutoff)
          g_hourly_pnl_records.pop_front(); }
    // Save TOD gate data and reset CVD session accumulators
    g_edges.tod.save_csv(log_root_dir() + "/omega_tod_buckets.csv");
    g_edges.fill_quality.save_csv(log_root_dir() + "/fill_quality.csv");
    g_edges.fill_quality.print_summary();
    g_walk_forward.log_all();   // RenTec #6 -- WFO final state summary
    g_param_gate.log_all();     // RenTec #7 -- adaptive param gate final state
    g_adaptive_risk.save_perf(state_root_dir() + "/kelly");
    g_corr_matrix.save_state(state_root_dir() + "/corr_matrix.dat");  // persist EWM running stats
    g_gold_stack.save_atr_state(state_root_dir() + "/gold_stack_state.dat");
    // ── Reset GoldStack RegimeGovernor drift EWMs on UTC day rollover ────────
    // FIX 2026-04-22 (Apr 17 drift diagnostic, second bug):
    // Without this call ewm_fast_/ewm_slow_ accumulate continuously from
    // process start, carrying yesterday's bias into today's first ticks.
    // Replay analysis of Apr 17 showed drift=+1.32 at 00:00:00 before any
    // Apr 17 ticks influenced a fresh EWM -- priming directional bias
    // generator LONG from session start and contributing to the 169:9
    // LONG:SHORT Asian-session attempt skew. Save happens on the line above
    // BEFORE this reset so the prior-day baseline is preserved to disk for
    // warm-restart scenarios. This reset only fires on continuous-process
    // day rollover; process restart uses load_atr_state()'s 4-hour staleness
    // check path instead.
    g_gold_stack.reset_drift_on_day_rollover();
    g_trend_pb_gold.save_state(state_root_dir()  + "/trend_pb_gold.dat");
    g_trend_pb_ger40.save_state(state_root_dir() + "/trend_pb_ger40.dat");
    g_trend_pb_nq.save_state(state_root_dir()    + "/trend_pb_nq.dat");
    g_trend_pb_sp.save_state(state_root_dir()    + "/trend_pb_sp.dat");
    // Save OHLCBarEngine indicator state -- eliminates tick data request on restart
    g_bars_gold.m1 .save_indicators(state_root_dir() + "/bars_gold_m1.dat");
    g_bars_gold.m5 .save_indicators(state_root_dir() + "/bars_gold_m5.dat");
    g_bars_gold.m15.save_indicators(state_root_dir() + "/bars_gold_m15.dat");
    g_bars_gold.h4 .save_indicators(state_root_dir() + "/bars_gold_h4.dat");
    // MinimalH4US30Breakout warm-restart state (S26 2026-04-25)
    g_minimal_h4_us30.save_state(state_root_dir() + "/bars_us30_h4.dat");
    g_edges.reset_daily();  // resets CVD session hi/lo; prev_day updates via on_tick

    // Multi-day throttle: record this session's final PnL before resetting the ledger.
    // Must happen BEFORE g_omegaLedger.resetDaily() would clear it -- but resetDaily()
    // already ran above, so we use dailyPnl() which reads the just-reset value.
    // Instead, snapshot net pnl from g_telem which holds the last written value.
    {
        // Format date string for the day that just ended
        char date_buf[16];
        // ti was computed above from system_clock; use it for the closing date
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        // Use session_final_pnl captured before resetDaily() -- guaranteed accurate
        const std::string md_path = log_root_dir() + "/day_results.csv";
        g_adaptive_risk.multiday.record_day(std::string(date_buf), session_final_pnl, md_path);
        const int streak      = g_adaptive_risk.multiday.consecutive_losing_days();
        const double md_scale = g_adaptive_risk.multiday.size_scale();
        g_telemetry.UpdateMultiDayThrottle(streak, md_scale,
                                           g_adaptive_risk.multiday.is_active() ? 1 : 0);
    }

    std::cout << "[OMEGA-RISK] UTC day rollover -- per-symbol risk state reset\n";
}

