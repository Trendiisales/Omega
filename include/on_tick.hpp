#pragma once
// on_tick.hpp -- extracted from main.cpp
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static void on_tick(const std::string& sym, double bid, double ask) {
    // ?? Tick spike filter ???????????????????????????????????????????????
    // Reject ticks where mid moves > 5x slow ATR in a single step.
    // Broker bad ticks (gold at 46420 instead of 4642) distort ATR, VWAP,
    // and can trigger entries on garbage data. Hard reject -- drops tick
    // entirely before any engine state is touched.
    // Threshold: 5 * atr_slow. Gold ATR_SLOW~10pts -> threshold=50pts.
    // A real $50 gold move in a single tick is physically impossible.
    // Warmup: skip filter until atr_slow is populated (~100 ticks/symbol).
    {
        static std::mutex                              s_spike_mtx;
        static std::unordered_map<std::string, double> s_prev_mid;

        if (bid <= 0.0 || ask <= 0.0 || bid >= ask) return;  // bad quote

        const double mid = (bid + ask) * 0.5;
        double prev = 0.0;
        {
            std::lock_guard<std::mutex> lk(s_spike_mtx);
            auto it = s_prev_mid.find(sym);
            if (it != s_prev_mid.end()) prev = it->second;
            s_prev_mid[sym] = mid;
        }

        if (prev > 0.0) {
            const double atr_s = g_adaptive_risk.vol_scaler.atr_slow(sym);
            if (atr_s > 0.0) {
                const double move      = std::fabs(mid - prev);
                // Per-symbol minimum threshold floor -- prevents near-zero atr_slow
                // (during warmup or quiet tape) from rejecting valid ticks.
                // Root cause: atr_slow for XAUUSD was 0.11pts -> threshold=0.54pts
                // -> every normal gold tick-to-tick move rejected -> bars/RSI starved.
                // Gold moves 0.5-2pts per tick normally. Floor = 5pts (50x normal tick).
                // A real bad-tick on gold (e.g. 46420 instead of 4642) moves 800pts.
                // FX floor = 0.002 (20 pips). Index floor = 5pts. Oil floor = 0.5pts.
                const double min_threshold =
                    (sym == "XAUUSD" || sym == "XAGUSD") ? 5.0   :  // gold/silver: 5pt floor
                    (sym == "US500.F" || sym == "USTEC.F" || sym == "DJ30.F" ||
                     sym == "NAS100"  || sym == "GER40"   || sym == "UK100"  ||
                     sym == "ESTX50")                     ? 5.0   :  // indices: 5pt floor
                    (sym == "USOIL.F" || sym == "BRENT")  ? 0.5   :  // oil: 0.5pt floor
                                                            0.002;   // FX: 20 pip floor
                const double threshold = std::max(5.0 * atr_s, min_threshold);
                if (move > threshold) {
                    printf("[TICK-SPIKE] %s REJECTED mid=%.5f prev=%.5f move=%.5f threshold=%.5f (5xATR_SLOW)\n",
                           sym.c_str(), mid, prev, move, threshold);
                    fflush(stdout);
                    return;  // drop tick -- no engine state updated
                }
            }
        }
    }

    { std::lock_guard<std::mutex> lk(g_book_mtx); g_bids[sym] = bid; g_asks[sym] = ask; }
    stale_watchdog_ping(sym, bid);  // record tick + price for frozen-feed detection

    // ?? Edge system updates (every tick, every symbol) ????????????????????????
    {
        const double mid = (bid + ask) * 0.5;
        g_edges.cvd.update(sym, bid, ask);
        g_edges.spread_gate.update(sym, ask - bid);
        g_edges.prev_day.update(sym, mid, nowSec());
        // Edge 8: Volume profile -- track time-at-price every tick
        g_edges.vol_profile.update(sym, mid);
        // Edge 9: Order flow absorption -- detect institutional fading of moves
        // Use the per-symbol L2 imbalance from MacroContext (updated just above this)
        {
            double l2_imb = 0.5;  // neutral fallback
            if      (sym == "XAUUSD")  l2_imb = g_macro_ctx.gold_l2_imbalance;
            else if (sym == "US500.F") l2_imb = g_macro_ctx.sp_l2_imbalance;
            else if (sym == "USTEC.F") l2_imb = g_macro_ctx.nq_l2_imbalance;
            else if (sym == "XAGUSD")  l2_imb = g_macro_ctx.xag_l2_imbalance;
            else if (sym == "USOIL.F") l2_imb = g_macro_ctx.cl_l2_imbalance;
            else if (sym == "BRENT")   l2_imb = g_macro_ctx.brent_l2_imbalance;
            else if (sym == "EURUSD")  l2_imb = g_macro_ctx.eur_l2_imbalance;
            else if (sym == "GBPUSD")  l2_imb = g_macro_ctx.gbp_l2_imbalance;
            else if (sym == "AUDUSD")  l2_imb = g_macro_ctx.aud_l2_imbalance;
            else if (sym == "NZDUSD")  l2_imb = g_macro_ctx.nzd_l2_imbalance;
            else if (sym == "USDJPY")  l2_imb = g_macro_ctx.jpy_l2_imbalance;
            else if (sym == "GER40")   l2_imb = g_macro_ctx.ger40_l2_imbalance;
            else if (sym == "UK100")   l2_imb = g_macro_ctx.uk100_l2_imbalance;
            else if (sym == "ESTX50")  l2_imb = g_macro_ctx.estx50_l2_imbalance;
            else if (sym == "DJ30.F")  l2_imb = g_macro_ctx.us30_l2_imbalance;
            else if (sym == "NAS100")  l2_imb = g_macro_ctx.nas_l2_imbalance;
            g_edges.absorption.update(sym, mid, l2_imb);
        }
        // Edge 10: VPIN -- volume-synchronised informed flow toxicity
        // Detects institutional flow that doesn't show as L2 wall/vacuum.
        // Bucket-based: classifies each 50-tick window as buy/sell imbalance.
        g_edges.vpin.update(sym, mid);
        // HTF bias filter -- tracks daily + intraday momentum per symbol.
        // Used in lot sizing: 0.5? when trade opposes both TF trends.
        g_htf_filter.update(sym, mid);
    }  // end edge system updates

    // Seed vol history on first tick after reconnect -- avoids warmup dead zone.
    // seed() is a no-op if already warmed up.
    {
        const double mid = (bid + ask) * 0.5;
        if      (sym == "US500.F") g_eng_sp.seed(mid);
        else if (sym == "USTEC.F") g_eng_nq.seed(mid);
        else if (sym == "USOIL.F") g_eng_cl.seed(mid);
        else if (sym == "DJ30.F")  g_eng_us30.seed(mid);
        else if (sym == "NAS100")  g_eng_nas100.seed(mid);
        else if (sym == "GER40")   g_eng_ger30.seed(mid);
        else if (sym == "UK100")   g_eng_uk100.seed(mid);
        else if (sym == "ESTX50")  g_eng_estx50.seed(mid);
        else if (sym == "XAGUSD")  g_eng_xag.seed(mid);
        else if (sym == "EURUSD")  g_eng_eurusd.seed(mid);
        else if (sym == "GBPUSD")  g_eng_gbpusd.seed(mid);
        else if (sym == "AUDUSD")  g_eng_audusd.seed(mid);
        else if (sym == "NZDUSD")  g_eng_nzdusd.seed(mid);
        else if (sym == "USDJPY")  g_eng_usdjpy.seed(mid);
        else if (sym == "BRENT")   g_eng_brent.seed(mid);
        else if (sym == "XAUUSD") {
            // Pass VIX level so seed ATR scales to actual volatility regime.
            // VIX 27 day ? seed_atr=18pts ? SL=18pts -- survives real moves.
            // VIX 15 day ? seed_atr=5pts  ? SL=5pts  -- appropriate for quiet tape.
            g_gold_flow.seed(mid, g_macro_ctx.vix);
        }
    }

    // Rate-limit tick logging -- max 1 line per symbol per 30s to keep logs readable.
    // Previously logged every tick: thousands of lines/minute drowning signal output.
    {
        static std::mutex s_tick_log_mtx;
        static std::unordered_map<std::string, int64_t> s_last_tick_log;
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(s_tick_log_mtx);
        auto& last = s_last_tick_log[sym];
        if (now_ms - last >= 30000) {
            last = now_ms;
            std::cout << "[TICK] " << sym << " "
                      << std::fixed << std::setprecision(2) << bid << "/"
                      << std::fixed << std::setprecision(2) << ask << "\n";
            std::cout.unsetf(std::ios::fixed);
            std::cout << std::setprecision(6);
            std::cout.flush();
        }
    }

    maybe_reset_daily_ledger();

    const double mid = (bid + ask) * 0.5;
    if (sym == "VIX.F")   g_macroDetector.updateVIX(mid);
    if (sym == "DX.F")    g_macroDetector.updateDXY(mid);  // Dollar Index futures -- DXY momentum
    if (sym == "US500.F") g_macroDetector.updateES(mid);   // use traded futures, not cash ES
    if (sym == "USTEC.F") g_macroDetector.updateNQ(mid);   // use traded futures, not cash NAS100

    g_telemetry.UpdatePrice(sym.c_str(), bid, ask);

    const std::string regime = g_macroDetector.regime();
    g_telemetry.UpdateMacroRegime(
        g_macroDetector.vixLevel(), regime.c_str(), g_macroDetector.esNqDivergence());

    // Update shared MacroContext -- read by SP/NQ shouldTrade() overrides
    g_macro_ctx.regime     = regime;
    g_macro_ctx.vix        = g_macroDetector.vixLevel();
    g_macro_ctx.es_nq_div  = g_macroDetector.esNqDivergence();
    g_macro_ctx.sp_open    = g_eng_sp.pos.active;
    g_macro_ctx.nq_open    = g_eng_nq.pos.active;
    g_macro_ctx.oil_open   = g_eng_cl.pos.active;

    // ?? Feed adaptive intelligence layer ?????????????????????????????????????
    // Regime adaptor: track macro regime changes + per-symbol vol regime
    g_regime_adaptor.update(regime, g_macroDetector.vixLevel(), nowSec());
    // Vol update: use half-spread as proxy for tick vol (avoids needing prev_mid)
    // update_vol needs actual mid price (for bucket high/low range), not half-spread.
    // Passing half-spread was causing vol regime to track spread width, not price vol.
    g_regime_adaptor.update_vol(sym, (bid + ask) * 0.5, nowSec());
    g_adaptive_risk.update_vol(sym, (ask - bid) * 0.5);

    // Pending limit order cancel fallback -- runs every tick
    check_pending_limits();

    // Correlation cluster counts -- updated every tick for heat guard
    // Count open positions per cluster for CorrelationHeatGuard
    {
        const int us_eq = static_cast<int>(g_eng_sp.pos.active)
                        + static_cast<int>(g_eng_nq.pos.active)
                        + static_cast<int>(g_eng_us30.pos.active)
                        + static_cast<int>(g_eng_nas100.pos.active)
                        + static_cast<int>(g_bracket_sp.pos.active)
                        + static_cast<int>(g_bracket_nq.pos.active)
                        + static_cast<int>(g_bracket_us30.pos.active)
                        + static_cast<int>(g_bracket_nas100.pos.active)
                        + static_cast<int>(g_ca_esnq.has_open_position())
                        + static_cast<int>(g_vwap_rev_sp.has_open_position())
                        + static_cast<int>(g_vwap_rev_nq.has_open_position())
                        + static_cast<int>(g_orb_us.has_open_position())
                        + static_cast<int>(g_nbm_sp.has_open_position())
                        + static_cast<int>(g_nbm_nq.has_open_position())
                        + static_cast<int>(g_nbm_nas.has_open_position())
                        + static_cast<int>(g_nbm_us30.has_open_position());
                        // NOTE: g_nbm_gold_london and g_nbm_oil_london are NOT US equity --
                        // they were incorrectly counted here, inflating us_eq heat and
                        // blocking legitimate US equity entries when gold/oil NBM was open.
                        // g_nbm_gold_london is counted in `metals` below.
                        // g_nbm_oil_london  is counted in `oil`    below.
        const int eu_eq = static_cast<int>(g_eng_ger30.pos.active)
                        + static_cast<int>(g_eng_uk100.pos.active)
                        + static_cast<int>(g_eng_estx50.pos.active)
                        + static_cast<int>(g_bracket_ger30.pos.active)
                        + static_cast<int>(g_bracket_uk100.pos.active)
                        + static_cast<int>(g_bracket_estx50.pos.active)
                        + static_cast<int>(g_vwap_rev_ger40.has_open_position())
                        + static_cast<int>(g_orb_ger30.has_open_position())
                        + static_cast<int>(g_orb_uk100.has_open_position())
                        + static_cast<int>(g_orb_estx50.has_open_position());
        const int oil   = static_cast<int>(g_eng_cl.pos.active)
                        + static_cast<int>(g_eng_brent.pos.active)
                        + static_cast<int>(g_bracket_brent.pos.active)
                        + static_cast<int>(g_ca_eia_fade.has_open_position())
                        + static_cast<int>(g_ca_brent_wti.has_open_position())
                        + static_cast<int>(g_nbm_oil_london.has_open_position()); // was incorrectly in us_eq
        const int metals = static_cast<int>(g_gold_stack.has_open_position())
                         + static_cast<int>(g_bracket_gold.pos.active)
                         + static_cast<int>(g_gold_flow.has_open_position())
                         + static_cast<int>(g_gold_flow_reload.has_open_position())
                         + static_cast<int>(g_eng_xag.pos.active)
                         + static_cast<int>(g_bracket_xag.pos.active)
                         + static_cast<int>(g_orb_silver.has_open_position())
                         + static_cast<int>(g_nbm_gold_london.has_open_position()); // was incorrectly in us_eq
        const int jpy   = static_cast<int>(g_eng_usdjpy.pos.active)
                        + static_cast<int>(g_eng_audusd.pos.active)
                        + static_cast<int>(g_eng_nzdusd.pos.active)
                        + static_cast<int>(g_ca_carry_unwind.has_open_position());
        const int eur_gbp = static_cast<int>(g_eng_eurusd.pos.active)
                          + static_cast<int>(g_eng_gbpusd.pos.active)
                          + static_cast<int>(g_bracket_eurusd.pos.active)
                          + static_cast<int>(g_bracket_gbpusd.pos.active)
                          + static_cast<int>(g_vwap_rev_eurusd.has_open_position())
                          + static_cast<int>(g_ca_fx_cascade.has_open_gbpusd());
        g_adaptive_risk.update_cluster_counts(us_eq, eu_eq, oil, metals, jpy, eur_gbp);

        // Portfolio VaR: update dollar-risk estimates per cluster.
        // Dollar risk proxy = cluster_open_count ? risk_per_trade_usd (configured).
        // Uses risk_per_trade_usd if set, else falls back to daily_loss_limit / 4.
        // This gives a conservative estimate of simultaneous loss if all positions
        // move adversely together -- the correlation-adjustment (beta) in PortfolioVaR
        // then weights by DXY sensitivity to catch correlated drawdowns.
        {
            const double rpt = (g_cfg.risk_per_trade_usd > 0.0)
                ? g_cfg.risk_per_trade_usd
                : g_cfg.daily_loss_limit / 4.0;
            g_portfolio_var.update("US_EQUITY", us_eq    * rpt);
            g_portfolio_var.update("EU_EQUITY", eu_eq    * rpt);
            g_portfolio_var.update("OIL",       oil      * rpt);
            g_portfolio_var.update("METALS",    metals   * rpt);
            g_portfolio_var.update("JPY_RISK",  jpy      * rpt);
            g_portfolio_var.update("EUR_GBP",   eur_gbp  * rpt);
        }
    }

    // ?? Correlation matrix -- feed current symbol mid price each tick ????????
    // Each symbol passes through the macro-tick dispatch once per tick.
    // XAUUSD is also fed in the gold tick handler (higher frequency is fine --
    // duplicate feeds are idempotent: EWM converges regardless of update rate).
    if (bid > 0.0 && ask > 0.0)
        g_corr_matrix.on_price(sym, (bid + ask) * 0.5);

    // Session slot -- updated every tick
    {
        const auto t_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti_now = {};
#ifdef _WIN32
        gmtime_s(&ti_now, &t_now);
#else
        gmtime_r(&t_now, &ti_now);
#endif
        const int h = ti_now.tm_hour;
        if      (h >= 7  && h < 9)  g_macro_ctx.session_slot = 1; // London open
        else if (h >= 9  && h < 12) g_macro_ctx.session_slot = 2; // London core
        else if (h >= 12 && h < 14) g_macro_ctx.session_slot = 3; // Overlap
        else if (h >= 14 && h < 17) g_macro_ctx.session_slot = 4; // NY open
        else if (h >= 17 && h < 22) g_macro_ctx.session_slot = 5; // NY late
        else if (h >= 22 || h < 5)  g_macro_ctx.session_slot = 6; // Asia
        else                         g_macro_ctx.session_slot = 1; // 05-07 UTC: pre-London (was dead zone/slot=0, now slot=1)
        // [BUG-1 NOTE] session_slot=0 is INTENTIONALLY NEVER ASSIGNED. The old dead zone (05-07 UTC)
        // is now treated as London pre-open (slot=1). Any code checking slot==0 (in_dead_zone_slot)
        // will NEVER fire because slot=0 cannot occur. This is correct and intentional --
        // the dead zone gate was removed. Internal GoldEngineStack engines that check slot==0
        // ("UNKNOWN") will also never fire their dead zone blocks, which is correct behaviour.
        // DO NOT restore slot=0 without auditing all dead-zone gates in GoldEngineStack.hpp.
    }

    // Cross-symbol compression state -- engine is in COMPRESSION or BREAKOUT_WATCH
    // NAS100 and USTEC.F are the same underlying -- either one counts as "nq compressing"
    g_macro_ctx.sp_compressing    = (g_eng_sp.phase     == omega::Phase::COMPRESSION
                                  || g_eng_sp.phase     == omega::Phase::BREAKOUT_WATCH);
    g_macro_ctx.nq_compressing    = (g_eng_nq.phase     == omega::Phase::COMPRESSION
                                  || g_eng_nq.phase     == omega::Phase::BREAKOUT_WATCH
                                  || g_eng_nas100.phase == omega::Phase::COMPRESSION
                                  || g_eng_nas100.phase == omega::Phase::BREAKOUT_WATCH);
    g_macro_ctx.us30_compressing  = (g_eng_us30.phase   == omega::Phase::COMPRESSION
                                  || g_eng_us30.phase   == omega::Phase::BREAKOUT_WATCH);
    g_macro_ctx.ger30_compressing = (g_eng_ger30.phase  == omega::Phase::COMPRESSION
                                  || g_eng_ger30.phase  == omega::Phase::BREAKOUT_WATCH);
    g_macro_ctx.uk100_compressing = (g_eng_uk100.phase  == omega::Phase::COMPRESSION
                                  || g_eng_uk100.phase  == omega::Phase::BREAKOUT_WATCH);

    // ── GOLD L2 IMBALANCE -- cTrader Open API ONLY (ctid=43014358) ────────────
    // cTrader is the ONLY source of L2 data. FIX does not provide L2.
    // No BlackBull assumptions. No has_data checks. No bid/ask side requirements.
    // Rule: if cTrader depth events are flowing for XAUUSD, L2 is live.
    // Imbalance is derived from price level structure (level count + book slope).
    {
        const int64_t l2_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto rd = [&](const AtomicL2& al) -> double {
            return al.fresh(l2_now_ms) ? al.imbalance.load(std::memory_order_relaxed) : 0.5;
        };

        {
            // GOLD L2 IMBALANCE -- cTrader Open API (ctid=43014358) ONLY
            // Use g_l2_gold.last_update_ms to check if cTrader is delivering
            // XAUUSD depth events specifically (not just any symbol).
            // atomic_l2_write_fn sets last_update_ms on every XAUUSD depth event.
            const bool l2_live = g_l2_gold.fresh(l2_now_ms, 10000);  // raised 3000->10000ms: Asia tape batches depth events, gaps up to 5s are normal at 250 events/min
            g_macro_ctx.gold_l2_real = l2_live;

            if (l2_live) {
                // g_l2_gold.imbalance = book.raw_imbalance() from CTDepthBook.
                // = raw_bid_count / (raw_bid_count + raw_ask_count) across ALL DOM quotes.
                // cTrader sends ≥5 levels per side always for XAUUSD. to_l2book() caps at 5,
                // making imbalance_level() = 5/10 = 0.500 permanently.
                // raw_imbalance() counts all quotes before the 5-level cap -- real signal.
                const double raw_imb = g_l2_gold.imbalance.load(std::memory_order_relaxed);
                // Use GoldMicrostructureAnalyzer delta-based signal as the canonical
                // gold L2 imbalance for ALL gold engines. This replaces the raw
                // bid_count/(bid+ask) ratio which was perpetually ~0.5 (dead signal).
                // micro_edge is computed in CTraderDepthClient on every DOM event
                // from 5 delta features: imbalance + consumption + pull + absorption + queue.
                // Range 0..1 -- same as raw_imb, compatible with all existing thresholds.
                // raw_imb retained for logging/diagnostics only.
                g_macro_ctx.gold_l2_imbalance = g_l2_gold.micro_edge.load(std::memory_order_relaxed);

                // ── Real DOM override (OmegaDomStreamer cBot on port 8765) ──────────
                // If real DOM data is fresh (<2s), use volume-weighted imbalance
                // from actual cTrader platform sizes instead of level-count proxy.
                // Falls back to micro_edge automatically if cBot disconnects.
                {
                    const int64_t REAL_DOM_MAX_STALE_MS = 2000;
                    const int64_t real_dom_age = l2_now_ms - g_real_dom_last_ms.load(std::memory_order_relaxed);
                    if (real_dom_age < REAL_DOM_MAX_STALE_MS) {
                        const double real_imb = real_dom_imbalance(5);
                        if (real_imb > 0.0 && real_imb < 1.0) {
                            g_macro_ctx.gold_l2_imbalance = real_imb;
                            g_macro_ctx.gold_l2_real = true;  // confirm real data present
                        }
                    }
                }

                static int64_t s_l2_log_ms = 0;
                if (l2_now_ms - s_l2_log_ms > 10000) {
                    s_l2_log_ms = l2_now_ms;
                    printf("[GOLD-L2-LIVE] imb=%.3f age_ms=%lld\n",
                           raw_imb,
                           (long long)(l2_now_ms - g_l2_gold.last_update_ms.load(std::memory_order_relaxed)));
                    fflush(stdout);
                }
            } else {
                // cTrader XAUUSD events stale. Hold last known imbalance.
                static int64_t s_fallback_log_ms = 0;
                if (l2_now_ms - s_fallback_log_ms > 10000) {
                    s_fallback_log_ms = l2_now_ms;
                    printf("[GOLD-L2-WAIT] cTrader stale -- holding imb=%.3f\n",
                           g_macro_ctx.gold_l2_imbalance);
                    fflush(stdout);
                }
            }
        }

        g_macro_ctx.sp_l2_imbalance     = rd(g_l2_sp);
        g_macro_ctx.nq_l2_imbalance     = rd(g_l2_nq);
        g_macro_ctx.cl_l2_imbalance     = rd(g_l2_cl);
        g_macro_ctx.xag_l2_imbalance    = rd(g_l2_xag);
        g_macro_ctx.eur_l2_imbalance    = rd(g_l2_eur);
        g_macro_ctx.gbp_l2_imbalance    = rd(g_l2_gbp);
        g_macro_ctx.aud_l2_imbalance    = rd(g_l2_aud);
        g_macro_ctx.nzd_l2_imbalance    = rd(g_l2_nzd);
        g_macro_ctx.jpy_l2_imbalance    = rd(g_l2_jpy);
        g_macro_ctx.ger40_l2_imbalance  = rd(g_l2_ger40);
        g_macro_ctx.uk100_l2_imbalance  = rd(g_l2_uk100);
        g_macro_ctx.estx50_l2_imbalance = rd(g_l2_estx50);
        g_macro_ctx.brent_l2_imbalance  = rd(g_l2_brent);
        g_macro_ctx.nas_l2_imbalance    = rd(g_l2_nas);
        g_macro_ctx.us30_l2_imbalance   = rd(g_l2_us30);
    }
    // Microprice bias -- still from cTrader atomics (FIX doesn't compute this)
    g_macro_ctx.gold_microprice_bias = g_l2_gold.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.sp_microprice_bias   = g_l2_sp.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.xag_microprice_bias  = g_l2_xag.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.cl_microprice_bias   = g_l2_cl.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.eur_microprice_bias  = g_l2_eur.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.gbp_microprice_bias  = g_l2_gbp.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.aud_microprice_bias  = g_l2_aud.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.nzd_microprice_bias  = g_l2_nzd.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.jpy_microprice_bias  = g_l2_jpy.microprice_bias.load(std::memory_order_relaxed);
    g_macro_ctx.ger40_microprice_bias= g_l2_ger40.microprice_bias.load(std::memory_order_relaxed);

    // ?? L2 quality flags -- lock-free reads ????????????????????????????????
    g_macro_ctx.ctrader_l2_live = (g_ctrader_depth.depth_events_total.load() > 0);
    // gold_l2_real now set from g_l2_books in the mutex block above

    // ?? Midnight log rotation -- every tick check, guaranteed rotation ???????????
    // force_rotate_check runs every 60s in diagnostic loop but if stdout is
    // quiet it may not fire. Check every tick so rotation is never missed.
    if (g_tee_buf) {
        static int64_t s_last_rotate_check = 0;
        const int64_t now_rot = nowSec();
        if (now_rot - s_last_rotate_check >= 5) {  // check every 5s max
            s_last_rotate_check = now_rot;
            g_tee_buf->force_rotate_check();
        }
    }

    // ?? Periodic bar indicator auto-save every 10 minutes ????????????????
    // Prevents cold-start on crash/kill: indicators saved to .dat every 10min
    // so a restart within 12h loads them instantly (m1_ready=true immediately).
    // Previously only saved at midnight + shutdown -- a crash between saves
    // meant cold start, m1_ready=false, GoldFlow blocked for 15min+ every restart.
    {
        static int64_t s_last_bar_save = 0;
        const int64_t now_bs = nowSec();
        if (now_bs - s_last_bar_save >= 60) {  // every 60s -- ensures warm restart even after short session
            s_last_bar_save = now_bs;
            if (g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
                const std::string base = log_root_dir();
                g_bars_gold.m1 .save_indicators(base + "/bars_gold_m1.dat");
                g_bars_gold.m5 .save_indicators(base + "/bars_gold_m5.dat");
                g_bars_gold.m15.save_indicators(base + "/bars_gold_m15.dat");
                g_bars_gold.h1 .save_indicators(base + "/bars_gold_h1.dat");
                g_bars_gold.h4 .save_indicators(base + "/bars_gold_h4.dat");
                // H1 bars for SP/NQ swing engine warm restart
                g_bars_sp.h1.save_indicators(base + "/bars_sp_h1.dat");
                g_bars_nq.h1.save_indicators(base + "/bars_nq_h1.dat");
                printf("[BAR-SAVE] Periodic save -- bars_gold_m1/m5/m15/h1/h4 + sp_h1/nq_h1.dat updated\n");
                fflush(stdout);
            }
        }
    }


    // Log L2 status once per minute
    {
        static int64_t s_l2_log = 0;
        const int64_t now_l2 = nowSec();
        if (now_l2 - s_l2_log >= 60) {
            s_l2_log = now_l2;
            printf("[L2-STATUS] ctrader_live=%d events=%llu gold_real=%d gold_imb=%.3f "
                   "gold_mp=%.3f sp_real=%d cl_real=%d\n",
                   (int)g_macro_ctx.ctrader_l2_live,
                   (unsigned long long)g_ctrader_depth.depth_events_total.load(),
                   (int)g_macro_ctx.gold_l2_real,
                   g_macro_ctx.gold_l2_imbalance,
                   g_macro_ctx.gold_microprice_bias,
                   (int)g_macro_ctx.sp_l2_real,
                   (int)g_macro_ctx.cl_l2_real);
            fflush(stdout);
        }
    }

    // ?? Cold path: snapshot all needed books under ONE lock ?????????????????
    // Previously called getBook() per-symbol with a lock per call -- 12 locks/tick.
    // Now: one lock, copy all books, release, process outside -- zero contention window.
    // Only walls/vacuums/book_slope/GUI push need the full book; microprice_bias
    // already comes from atomics above.
    struct ColdSnap { L2Book book; bool valid = false; };
    std::unordered_map<std::string, ColdSnap> cold_snap;
    {
        static constexpr const char* COLD_SYMS[] = {
            "XAUUSD","US500.F","XAGUSD","USOIL.F","EURUSD","GBPUSD",
            "AUDUSD","NZDUSD","USDJPY","GER40","UK100","ESTX50","BRENT"
        };
        std::lock_guard<std::mutex> lk(g_l2_mtx);
        for (const char* cold_sym : COLD_SYMS) {
            auto it = g_l2_books.find(cold_sym);
            // Use bid_count OR ask_count > 0 -- cTrader sends incremental one-sided
            // updates so ask_count may be 0 when bids arrive first and vice versa.
            // has_data() requires BOTH sides which causes empty book on startup.
            if (it != g_l2_books.end() &&
                (it->second.bid_count > 0 || it->second.ask_count > 0))
                cold_snap[cold_sym] = {it->second, true};
        }
    }
    auto getBook = [&](const std::string& s) -> const L2Book* {
        auto it = cold_snap.find(s);
        return (it != cold_snap.end() && it->second.valid) ? &it->second.book : nullptr;
    };
        // Push L2 book levels to telemetry for GUI depth panel.
        // Always push exactly 5 levels -- pad with price=0/size=0 if book has fewer.
        // This prevents the JS depth panel from showing/hiding rows as cTrader sends
        // partial incremental updates (bid_count fluctuates 1-5 between ticks).
        // Zero-size levels render as invisible rows (zero-width bar, empty size) -- stable.
        auto pushL2 = [&](const char* sym, const L2Book* b) {
            if (!b) return;
            double bp[5]{}, bs[5]{}, ap[5]{}, as_[5]{};
            const int nb = std::min(b->bid_count, 5);
            const int na = std::min(b->ask_count, 5);
            for (int i=0;i<nb;++i){bp[i]=b->bids[i].price;bs[i]=b->bids[i].size;}
            for (int i=0;i<na;++i){ap[i]=b->asks[i].price;as_[i]=b->asks[i].size;}
            g_telemetry.UpdateL2Book(sym, bp, bs, nb, ap, as_, na);
        };
        if (const L2Book* b = getBook("XAUUSD")) {
            g_macro_ctx.gold_book_slope      = b->book_slope();
            g_macro_ctx.gold_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.gold_slope           = b->book_slope();  // weighted bid-ask pressure primer for MCE
            g_macro_ctx.gold_vacuum_bid      = b->liquidity_vacuum_bid();
            g_macro_ctx.gold_wall_above      = b->wall_above(g_macro_ctx.gold_mid_price);
            g_macro_ctx.gold_wall_below      = b->wall_below(g_macro_ctx.gold_mid_price);
            pushL2("XAUUSD", b);
        }
        if (const L2Book* b = getBook("US500.F")) {
            g_macro_ctx.sp_book_slope        = b->book_slope();
            g_macro_ctx.sp_vacuum_ask        = b->liquidity_vacuum_ask();
            g_macro_ctx.sp_vacuum_bid        = b->liquidity_vacuum_bid();
            g_macro_ctx.sp_wall_above        = b->wall_above(b->bid_count > 0 ? b->bids[0].price : 0.0);
            g_macro_ctx.sp_wall_below        = b->wall_below(b->ask_count > 0 ? b->asks[0].price : 0.0);
            pushL2("US500.F", b);
        }
        if (const L2Book* b = getBook("XAGUSD"))  { pushL2("XAGUSD", b); }
        if (const L2Book* b = getBook("USOIL.F")) {
            g_macro_ctx.cl_vacuum_ask        = b->liquidity_vacuum_ask();
            g_macro_ctx.cl_vacuum_bid        = b->liquidity_vacuum_bid();
        }
        // ?? FX pairs -- Priority 6 backlog now complete ????????????????????????????????
        // These previously used L2 imbalance < 0.30 as a vacuum proxy.
        // Now populated from real book data for full L2 scoring parity with GOLD/SP.
        if (const L2Book* b = getBook("EURUSD")) {
            g_macro_ctx.eur_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.eur_vacuum_bid      = b->liquidity_vacuum_bid();
            g_macro_ctx.eur_wall_above      = b->wall_above(g_macro_ctx.eur_mid_price);
            g_macro_ctx.eur_wall_below      = b->wall_below(g_macro_ctx.eur_mid_price);
            pushL2("EURUSD", b);
        }
        if (const L2Book* b = getBook("GBPUSD")) {
            g_macro_ctx.gbp_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.gbp_vacuum_bid      = b->liquidity_vacuum_bid();
            g_macro_ctx.gbp_wall_above      = b->wall_above(g_macro_ctx.gbp_mid_price);
            g_macro_ctx.gbp_wall_below      = b->wall_below(g_macro_ctx.gbp_mid_price);
        }
        if (const L2Book* b = getBook("AUDUSD")) {
            g_macro_ctx.aud_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.aud_vacuum_bid      = b->liquidity_vacuum_bid();
        }
        if (const L2Book* b = getBook("NZDUSD")) {
            g_macro_ctx.nzd_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.nzd_vacuum_bid      = b->liquidity_vacuum_bid();
        }
        if (const L2Book* b = getBook("USDJPY")) {
            g_macro_ctx.jpy_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.jpy_vacuum_bid      = b->liquidity_vacuum_bid();
        }
        // EU equity vacuum (for bracket L2 gate)
        if (const L2Book* b = getBook("GER40")) {
            g_macro_ctx.ger40_vacuum_ask      = b->liquidity_vacuum_ask();
            g_macro_ctx.ger40_vacuum_bid      = b->liquidity_vacuum_bid();
            g_macro_ctx.ger40_wall_above      = b->wall_above(
                b->bid_count > 0 ? b->bids[0].price : 0.0);
            g_macro_ctx.ger40_wall_below      = b->wall_below(
                b->ask_count > 0 ? b->asks[0].price : 0.0);
        }
        if (const L2Book* b = getBook("UK100")) {
            g_macro_ctx.uk100_vacuum_ask = b->liquidity_vacuum_ask();
            g_macro_ctx.uk100_vacuum_bid = b->liquidity_vacuum_bid();
        }
        if (const L2Book* b = getBook("ESTX50")) {
            g_macro_ctx.estx50_vacuum_ask = b->liquidity_vacuum_ask();
            g_macro_ctx.estx50_vacuum_bid = b->liquidity_vacuum_bid();
        }

    // ?? CVD direction ? MacroContext ??????????????????????????????????????????
    // Push CVD direction and divergence flags into MacroContext so all engines
    // can use them as entry confirmation without accessing g_edges directly.
    {
        auto upd_cvd = [&](int& dir, bool& bull, bool& bear, const char* s) {
            const omega::edges::CVDState cs = g_edges.cvd.get(s);
            dir  = cs.direction();
            bull = cs.bullish_divergence();
            bear = cs.bearish_divergence();
        };
        bool dummy_b = false, dummy_b2 = false;
        upd_cvd(g_macro_ctx.gold_cvd_dir,   g_macro_ctx.gold_cvd_bull_div, g_macro_ctx.gold_cvd_bear_div, "XAUUSD");
        upd_cvd(g_macro_ctx.sp_cvd_dir,     g_macro_ctx.sp_cvd_bull_div,   g_macro_ctx.sp_cvd_bear_div,   "US500.F");
        upd_cvd(g_macro_ctx.nq_cvd_dir,     dummy_b,  dummy_b2,  "USTEC.F");
        upd_cvd(g_macro_ctx.eurusd_cvd_dir, dummy_b,  dummy_b2,  "EURUSD");
        upd_cvd(g_macro_ctx.usdjpy_cvd_dir, dummy_b,  dummy_b2,  "USDJPY");
        upd_cvd(g_macro_ctx.xagusd_cvd_dir, dummy_b,  dummy_b2,  "XAGUSD");
    }

    // Push L2 imbalance snapshot to telemetry
    g_telemetry.UpdateL2(
        g_macro_ctx.sp_l2_imbalance,  g_macro_ctx.nq_l2_imbalance,
        g_macro_ctx.us30_l2_imbalance,g_macro_ctx.nas_l2_imbalance,
        g_macro_ctx.cl_l2_imbalance,  g_macro_ctx.brent_l2_imbalance,
        g_macro_ctx.gold_l2_imbalance,g_macro_ctx.xag_l2_imbalance,
        g_macro_ctx.ger40_l2_imbalance,g_macro_ctx.uk100_l2_imbalance,
        g_macro_ctx.estx50_l2_imbalance,
        g_macro_ctx.eur_l2_imbalance, g_macro_ctx.gbp_l2_imbalance,
        g_macro_ctx.aud_l2_imbalance, g_macro_ctx.nzd_l2_imbalance,
        g_macro_ctx.jpy_l2_imbalance,
        [&]() -> int {
            // L2 active: TCP connected AND depth events received within last 30s.
            // Without the age check, a silent feed stall (connection alive, broker
            // stopped sending quotes -- common in thin Asian session) shows the badge
            // green while all imbalance values are frozen stale data.
            if (!g_ctrader_depth.depth_active.load()) return 0;
            const int64_t last_ev = g_ctrader_depth.last_depth_event_ms.load();
            if (last_ev == 0) return 0;  // connected but no events yet
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return ((now_ms - last_ev) < 30000) ? 1 : 0;  // stale after 30s
        }());

    const bool tradeable = session_tradeable();
    g_telemetry.UpdateSession(tradeable ? "ACTIVE" : "CLOSED", tradeable ? 1 : 0);

    // Base gate flags -- passed into dispatch, checked before entry (not before warmup)
    // Use p95 RTT (not last) -- a single spike in g_rtt_last was permanently blocking
    // entries until the next 5s ping. p95 over 200 samples is stable and representative.
    const double rtt_check = (g_rtt_p95 > 0.0) ? g_rtt_p95 : g_rtt_last;
    const bool lat_ok = (rtt_check <= 0.0 || g_governor.checkLatency(rtt_check, g_cfg.max_latency_ms));
    if (!lat_ok) ++g_gov_lat;
    // Spread governor counter -- incremented only when bracket_spread_blocked fires
    // (i.e. a real signal was ready but spread was too wide). Not per-tick noise.
    // See bracket_spread_blocked lambda below which calls ++g_gov_spread directly.

    // ?? Open unrealised P&L accumulator ?????????????????????????????????????
    // Sums floating P&L across ALL open positions each tick.
    // Used by symbol_gate to enforce daily_loss_limit on combined
    // closed + unrealised loss -- prevents limit breach before any close fires.
    // open_unrealised_pnl -- converted to static function above on_tick

    // ?? Push unrealised P&L to global atomic every 250ms ????????????????????
    // This feeds the GUI daily_pnl display (closed + floating) and persists
    // across the tick so handle_closed_trade can read it without recomputing.
    {
        static thread_local int64_t s_last_unr_push = 0;
        const int64_t now_ms_unr = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ms_unr - s_last_unr_push >= 250) {
            s_last_unr_push = now_ms_unr;
            const double unr = open_unrealised_pnl();
            g_open_unrealised_cents.store(static_cast<int64_t>(unr * 100.0));
            // Push combined (closed + unrealised) to GUI so it shows live floating P&L
            const double closed = g_omegaLedger.dailyPnl();
            g_telemetry.UpdateStats(
                closed + unr,
                g_omegaLedger.grossDailyPnl() + unr,
                g_omegaLedger.maxDD(),
                g_omegaLedger.total(), g_omegaLedger.wins(), g_omegaLedger.losses(),
                g_omegaLedger.winRate(), g_omegaLedger.avgWin(), g_omegaLedger.avgLoss(), 0, 0,
                closed, unr);

            // ?? Per-trade live P&L update ?????????????????????????????????????
            // Rebuilds live_trades[] from scratch every 250ms.
            // Each open position contributes one entry with current floating P&L.
            g_telemetry.ClearLiveTrades();
            // Per-trade live P&L -- push_live_trade(sym, eng, is_long, entry, tp, sl, size, ts)
            // ?? Gold engines ????????????????????????????????????????????????
            if (g_gold_flow.pos.active) {
                // TP for GoldFlow = next stage trigger price (there is no fixed TP --
                // the engine trails. The next stage price is the meaningful target).
                // Stage triggers: 1x/2x/8x/15x ATR profit from entry.
                static constexpr double GF_STAGE_MULTS[] = {
                    GFE_BE_ATR_MULT,     // stage 0 ? 1: 1x ATR
                    GFE_STAGE2_ATR_MULT, // stage 1 ? 2: 2x ATR
                    GFE_STAGE3_ATR_MULT, // stage 2 ? 3: 8x ATR
                    GFE_STAGE4_ATR_MULT, // stage 3 ? 4: 15x ATR
                };
                const int   cur_stage = g_gold_flow.pos.trail_stage;
                const double atr_e    = g_gold_flow.pos.atr_at_entry;
                double gf_tp = 0.0;
                if (atr_e > 0.0 && cur_stage < 4) {
                    const double next_mult = GF_STAGE_MULTS[cur_stage];
                    gf_tp = g_gold_flow.pos.is_long
                        ? g_gold_flow.pos.entry + atr_e * next_mult
                        : g_gold_flow.pos.entry - atr_e * next_mult;
                }
                push_live_trade("XAUUSD", "GoldFlow",
                    g_gold_flow.pos.is_long, g_gold_flow.pos.entry,
                    gf_tp, g_gold_flow.pos.sl,
                    g_gold_flow.pos.size, g_gold_flow.pos.entry_ts);
            }
            if (g_gold_stack.has_open_position())
                push_live_trade("XAUUSD", g_gold_stack.live_engine(),
                    g_gold_stack.live_is_long(), g_gold_stack.live_entry(),
                    g_gold_stack.live_tp(),       g_gold_stack.live_sl(),
                    g_gold_stack.live_size(),     (int64_t)std::time(nullptr));
            if (g_bracket_gold.pos.active)
                push_live_trade("XAUUSD","Bracket",
                    g_bracket_gold.pos.is_long, g_bracket_gold.pos.entry,
                    g_bracket_gold.pos.tp,      g_bracket_gold.pos.sl,
                    g_bracket_gold.pos.size,    g_bracket_gold.pos.entry_ts);
            if (g_trend_pb_gold.has_open_position())
                push_live_trade("XAUUSD","TrendPB",
                    g_trend_pb_gold.open_is_long(), g_trend_pb_gold.open_entry(),
                    0.0, g_trend_pb_gold.open_sl(),
                    g_trend_pb_gold.open_size(), (int64_t)std::time(nullptr));
            if (g_nbm_gold_london.has_open_position())
                push_live_trade("XAUUSD","NBM-London",
                    g_nbm_gold_london.open_is_long(), g_nbm_gold_london.open_entry(),
                    0.0, 0.0, g_nbm_gold_london.open_size(), (int64_t)std::time(nullptr));
            if (g_candle_flow.has_open_position())
                push_live_trade("XAUUSD", "CandleFlow",
                    g_candle_flow.pos.is_long, g_candle_flow.pos.entry,
                    0.0, g_candle_flow.pos.sl,
                    g_candle_flow.pos.size, g_candle_flow.pos.entry_ts_ms / 1000);
            if (g_h1_swing_gold.has_open_position())
                push_live_trade("XAUUSD", "H1Swing",
                    g_h1_swing_gold.pos_.is_long, g_h1_swing_gold.pos_.entry,
                    g_h1_swing_gold.pos_.tp1,     g_h1_swing_gold.pos_.sl,
                    g_h1_swing_gold.pos_.size_remaining,
                    g_h1_swing_gold.pos_.entry_ts_ms / 1000);
            if (g_h4_regime_gold.has_open_position())
                push_live_trade("XAUUSD", "H4Regime",
                    g_h4_regime_gold.pos_.is_long, g_h4_regime_gold.pos_.entry,
                    g_h4_regime_gold.pos_.tp,      g_h4_regime_gold.pos_.sl,
                    g_h4_regime_gold.pos_.size,
                    g_h4_regime_gold.pos_.entry_ts_ms / 1000);
            // ?? US indices ??????????????????????????????????????????????????
            if (g_eng_sp.pos.active)
                push_live_trade("US500.F","BE", g_eng_sp.pos.is_long,
                    g_eng_sp.pos.entry, g_eng_sp.pos.tp, g_eng_sp.pos.sl,
                    g_eng_sp.pos.size, g_eng_sp.pos.entry_ts);
            if (g_eng_nq.pos.active)
                push_live_trade("USTEC.F","BE", g_eng_nq.pos.is_long,
                    g_eng_nq.pos.entry, g_eng_nq.pos.tp, g_eng_nq.pos.sl,
                    g_eng_nq.pos.size, g_eng_nq.pos.entry_ts);
            if (g_eng_us30.pos.active)
                push_live_trade("DJ30.F","BE", g_eng_us30.pos.is_long,
                    g_eng_us30.pos.entry, g_eng_us30.pos.tp, g_eng_us30.pos.sl,
                    g_eng_us30.pos.size, g_eng_us30.pos.entry_ts);
            if (g_eng_nas100.pos.active)
                push_live_trade("NAS100","BE", g_eng_nas100.pos.is_long,
                    g_eng_nas100.pos.entry, g_eng_nas100.pos.tp, g_eng_nas100.pos.sl,
                    g_eng_nas100.pos.size, g_eng_nas100.pos.entry_ts);
            if (g_bracket_sp.pos.active)
                push_live_trade("US500.F","Bracket", g_bracket_sp.pos.is_long,
                    g_bracket_sp.pos.entry, g_bracket_sp.pos.tp, g_bracket_sp.pos.sl,
                    g_bracket_sp.pos.size, g_bracket_sp.pos.entry_ts);
            if (g_bracket_nq.pos.active)
                push_live_trade("USTEC.F","Bracket", g_bracket_nq.pos.is_long,
                    g_bracket_nq.pos.entry, g_bracket_nq.pos.tp, g_bracket_nq.pos.sl,
                    g_bracket_nq.pos.size, g_bracket_nq.pos.entry_ts);
            if (g_bracket_us30.pos.active)
                push_live_trade("DJ30.F","Bracket", g_bracket_us30.pos.is_long,
                    g_bracket_us30.pos.entry, g_bracket_us30.pos.tp, g_bracket_us30.pos.sl,
                    g_bracket_us30.pos.size, g_bracket_us30.pos.entry_ts);
            if (g_bracket_nas100.pos.active)
                push_live_trade("NAS100","Bracket", g_bracket_nas100.pos.is_long,
                    g_bracket_nas100.pos.entry, g_bracket_nas100.pos.tp, g_bracket_nas100.pos.sl,
                    g_bracket_nas100.pos.size, g_bracket_nas100.pos.entry_ts);
            if (g_nbm_sp.has_open_position())
                push_live_trade("US500.F","NBM", g_nbm_sp.open_is_long(),
                    g_nbm_sp.open_entry(), 0.0, 0.0, g_nbm_sp.open_size(), (int64_t)std::time(nullptr));
            if (g_nbm_nq.has_open_position())
                push_live_trade("USTEC.F","NBM", g_nbm_nq.open_is_long(),
                    g_nbm_nq.open_entry(), 0.0, 0.0, g_nbm_nq.open_size(), (int64_t)std::time(nullptr));
            if (g_nbm_nas.has_open_position())
                push_live_trade("NAS100","NBM", g_nbm_nas.open_is_long(),
                    g_nbm_nas.open_entry(), 0.0, 0.0, g_nbm_nas.open_size(), (int64_t)std::time(nullptr));
            if (g_nbm_us30.has_open_position())
                push_live_trade("DJ30.F","NBM", g_nbm_us30.open_is_long(),
                    g_nbm_us30.open_entry(), 0.0, 0.0, g_nbm_us30.open_size(), (int64_t)std::time(nullptr));
            // ?? EU indices ??????????????????????????????????????????????????
            if (g_eng_ger30.pos.active)
                push_live_trade("GER40","BE", g_eng_ger30.pos.is_long,
                    g_eng_ger30.pos.entry, g_eng_ger30.pos.tp, g_eng_ger30.pos.sl,
                    g_eng_ger30.pos.size, g_eng_ger30.pos.entry_ts);
            if (g_eng_uk100.pos.active)
                push_live_trade("UK100","BE", g_eng_uk100.pos.is_long,
                    g_eng_uk100.pos.entry, g_eng_uk100.pos.tp, g_eng_uk100.pos.sl,
                    g_eng_uk100.pos.size, g_eng_uk100.pos.entry_ts);
            if (g_eng_estx50.pos.active)
                push_live_trade("ESTX50","BE", g_eng_estx50.pos.is_long,
                    g_eng_estx50.pos.entry, g_eng_estx50.pos.tp, g_eng_estx50.pos.sl,
                    g_eng_estx50.pos.size, g_eng_estx50.pos.entry_ts);
            if (g_bracket_ger30.pos.active)
                push_live_trade("GER40","Bracket", g_bracket_ger30.pos.is_long,
                    g_bracket_ger30.pos.entry, g_bracket_ger30.pos.tp, g_bracket_ger30.pos.sl,
                    g_bracket_ger30.pos.size, g_bracket_ger30.pos.entry_ts);
            if (g_bracket_uk100.pos.active)
                push_live_trade("UK100","Bracket", g_bracket_uk100.pos.is_long,
                    g_bracket_uk100.pos.entry, g_bracket_uk100.pos.tp, g_bracket_uk100.pos.sl,
                    g_bracket_uk100.pos.size, g_bracket_uk100.pos.entry_ts);
            if (g_bracket_estx50.pos.active)
                push_live_trade("ESTX50","Bracket", g_bracket_estx50.pos.is_long,
                    g_bracket_estx50.pos.entry, g_bracket_estx50.pos.tp, g_bracket_estx50.pos.sl,
                    g_bracket_estx50.pos.size, g_bracket_estx50.pos.entry_ts);
            if (g_trend_pb_ger40.has_open_position())
                push_live_trade("GER40","TrendPB", g_trend_pb_ger40.open_is_long(),
                    g_trend_pb_ger40.open_entry(), 0.0, g_trend_pb_ger40.open_sl(),
                    g_trend_pb_ger40.open_size(), (int64_t)std::time(nullptr));
            // ?? Oil/commodities ?????????????????????????????????????????????
            if (g_eng_cl.pos.active)
                push_live_trade("USOIL.F","BE", g_eng_cl.pos.is_long,
                    g_eng_cl.pos.entry, g_eng_cl.pos.tp, g_eng_cl.pos.sl,
                    g_eng_cl.pos.size, g_eng_cl.pos.entry_ts);
            if (g_eng_brent.pos.active)
                push_live_trade("BRENT","BE", g_eng_brent.pos.is_long,
                    g_eng_brent.pos.entry, g_eng_brent.pos.tp, g_eng_brent.pos.sl,
                    g_eng_brent.pos.size, g_eng_brent.pos.entry_ts);
            if (g_bracket_brent.pos.active)
                push_live_trade("BRENT","Bracket", g_bracket_brent.pos.is_long,
                    g_bracket_brent.pos.entry, g_bracket_brent.pos.tp, g_bracket_brent.pos.sl,
                    g_bracket_brent.pos.size, g_bracket_brent.pos.entry_ts);
            if (g_eng_xag.pos.active)
                push_live_trade("XAGUSD","BE", g_eng_xag.pos.is_long,
                    g_eng_xag.pos.entry, g_eng_xag.pos.tp, g_eng_xag.pos.sl,
                    g_eng_xag.pos.size, g_eng_xag.pos.entry_ts);
            if (g_nbm_oil_london.has_open_position())
                push_live_trade("USOIL.F","NBM-London", g_nbm_oil_london.open_is_long(),
                    g_nbm_oil_london.open_entry(), 0.0, 0.0, g_nbm_oil_london.open_size(), (int64_t)std::time(nullptr));
            // ?? FX ??????????????????????????????????????????????????????????
            if (g_eng_eurusd.pos.active)
                push_live_trade("EURUSD","BE", g_eng_eurusd.pos.is_long,
                    g_eng_eurusd.pos.entry, g_eng_eurusd.pos.tp, g_eng_eurusd.pos.sl,
                    g_eng_eurusd.pos.size, g_eng_eurusd.pos.entry_ts);
            if (g_eng_gbpusd.pos.active)
                push_live_trade("GBPUSD","BE", g_eng_gbpusd.pos.is_long,
                    g_eng_gbpusd.pos.entry, g_eng_gbpusd.pos.tp, g_eng_gbpusd.pos.sl,
                    g_eng_gbpusd.pos.size, g_eng_gbpusd.pos.entry_ts);
            if (g_eng_audusd.pos.active)
                push_live_trade("AUDUSD","BE", g_eng_audusd.pos.is_long,
                    g_eng_audusd.pos.entry, g_eng_audusd.pos.tp, g_eng_audusd.pos.sl,
                    g_eng_audusd.pos.size, g_eng_audusd.pos.entry_ts);
            if (g_eng_nzdusd.pos.active)
                push_live_trade("NZDUSD","BE", g_eng_nzdusd.pos.is_long,
                    g_eng_nzdusd.pos.entry, g_eng_nzdusd.pos.tp, g_eng_nzdusd.pos.sl,
                    g_eng_nzdusd.pos.size, g_eng_nzdusd.pos.entry_ts);
            if (g_eng_usdjpy.pos.active)
                push_live_trade("USDJPY","BE", g_eng_usdjpy.pos.is_long,
                    g_eng_usdjpy.pos.entry, g_eng_usdjpy.pos.tp, g_eng_usdjpy.pos.sl,
                    g_eng_usdjpy.pos.size, g_eng_usdjpy.pos.entry_ts);
            if (g_bracket_eurusd.pos.active)
                push_live_trade("EURUSD","Bracket", g_bracket_eurusd.pos.is_long,
                    g_bracket_eurusd.pos.entry, g_bracket_eurusd.pos.tp, g_bracket_eurusd.pos.sl,
                    g_bracket_eurusd.pos.size, g_bracket_eurusd.pos.entry_ts);
            if (g_bracket_gbpusd.pos.active)
                push_live_trade("GBPUSD","Bracket", g_bracket_gbpusd.pos.is_long,
                    g_bracket_gbpusd.pos.entry, g_bracket_gbpusd.pos.tp, g_bracket_gbpusd.pos.sl,
                    g_bracket_gbpusd.pos.size, g_bracket_gbpusd.pos.entry_ts);
            if (g_bracket_audusd.pos.active)
                push_live_trade("AUDUSD","Bracket", g_bracket_audusd.pos.is_long,
                    g_bracket_audusd.pos.entry, g_bracket_audusd.pos.tp, g_bracket_audusd.pos.sl,
                    g_bracket_audusd.pos.size, g_bracket_audusd.pos.entry_ts);
            if (g_bracket_nzdusd.pos.active)
                push_live_trade("NZDUSD","Bracket", g_bracket_nzdusd.pos.is_long,
                    g_bracket_nzdusd.pos.entry, g_bracket_nzdusd.pos.tp, g_bracket_nzdusd.pos.sl,
                    g_bracket_nzdusd.pos.size, g_bracket_nzdusd.pos.entry_ts);
            if (g_bracket_usdjpy.pos.active)
                push_live_trade("USDJPY","Bracket", g_bracket_usdjpy.pos.is_long,
                    g_bracket_usdjpy.pos.entry, g_bracket_usdjpy.pos.tp, g_bracket_usdjpy.pos.sl,
                    g_bracket_usdjpy.pos.size, g_bracket_usdjpy.pos.entry_ts);
            if (g_ca_fx_cascade.has_open_position())
                push_live_trade("GBPUSD","FxCascade", g_ca_fx_cascade.open_is_long(),
                    g_ca_fx_cascade.open_entry(), 0.0, 0.0, g_ca_fx_cascade.open_size(), (int64_t)std::time(nullptr));
            if (g_ca_carry_unwind.has_open_position())
                push_live_trade("USDJPY","CarryUnw", g_ca_carry_unwind.open_is_long(),
                    g_ca_carry_unwind.open_entry(), 0.0, 0.0, g_ca_carry_unwind.open_size(), (int64_t)std::time(nullptr));
        }

        // ── DOLLAR STOP: emergency per-trade runtime cut ──────────────────
        // Fires every 250ms (inside the existing 250ms push block).
        // Closes any position whose unrealised USD loss exceeds dollar_stop_usd.
        // Independent of SL -- fires on live floating P&L regardless of broker order state.
        // Uses last-seen XAUUSD bid/ask stored each tick so this works on any symbol tick.
        // Set dollar_stop_usd=0 in omega_config.ini to disable.
        if (g_cfg.dollar_stop_usd > 0.0) {
            // Cache XAUUSD prices from this tick or prior ticks
            static thread_local double s_xau_bid = 0.0;
            static thread_local double s_xau_ask = 0.0;
            if (sym == "XAUUSD" && bid > 0.0 && ask > 0.0) {
                s_xau_bid = bid;
                s_xau_ask = ask;
            }
            // Only check when we have a valid XAUUSD price
            if (s_xau_bid > 0.0 && s_xau_ask > 0.0) {
                const double ds_lim   = g_cfg.dollar_stop_usd;
                const double xau_mid  = (s_xau_bid + s_xau_ask) * 0.5;
                const int64_t ds_now     = static_cast<int64_t>(std::time(nullptr));
                const int64_t ds_now_ms  = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

                // Compute unrealised USD P&L: pts * size * 100 (XAUUSD = $100/pt/lot)
                auto xau_unr = [&](bool is_long, double entry, double size) -> double {
                    const double pts = is_long ? (xau_mid - entry) : (entry - xau_mid);
                    return pts * size * 100.0;
                };

                // GoldFlow
                if (g_gold_flow.pos.active) {
                    const double unr = xau_unr(g_gold_flow.pos.is_long,
                                               g_gold_flow.pos.entry,
                                               g_gold_flow.pos.size);
                    if (unr < -ds_lim) {
                        printf("[DOLLAR-STOP] GoldFlow %s entry=%.2f unr=$%.2f limit=$%.0f -- CLOSING\n",
                               g_gold_flow.pos.is_long?"LONG":"SHORT",
                               g_gold_flow.pos.entry, unr, ds_lim);
                        fflush(stdout);
                        g_gold_flow.force_close(s_xau_bid, s_xau_ask, ds_now,
                            [&](const omega::TradeRecord& tr) {
                                handle_closed_trade(tr);
                                send_live_order("XAUUSD", tr.side=="SHORT", tr.size, tr.exitPrice);
                            });
                    }
                }
                // CandleFlow
                // DOLLAR-STOP for CFE: only fire if price has already passed the
                // engine's own SL level. This prevents the dollar stop firing BEFORE
                // the SL when lot size is large relative to the $50 limit.
                // Example of old bug: sl=4719.13 (1.90pts away), dollar stop fires
                // at 4719.68 ($52 loss) -- $0.55 before the SL. The SL never fires.
                // Fix: check sl_breached first. Dollar stop is now a gap-fill safety
                // net (fires when price skips past SL) not a premature position killer.
                if (g_candle_flow.has_open_position()) {
                    const double unr = xau_unr(g_candle_flow.pos.is_long,
                                               g_candle_flow.pos.entry,
                                               g_candle_flow.pos.size);
                    // Effective SL: use trail SL if active, otherwise hard SL
                    const double cfe_eff_sl = g_candle_flow.pos.trail_active
                        ? g_candle_flow.pos.trail_sl
                        : g_candle_flow.pos.sl;
                    const bool cfe_sl_breached = g_candle_flow.pos.is_long
                        ? (s_xau_bid <= cfe_eff_sl)
                        : (s_xau_ask >= cfe_eff_sl);
                    // Only fire dollar stop if:
                    //   a) SL has already been breached (gap scenario), OR
                    //   b) Loss exceeds 2x dollar_stop_usd (catastrophic runaway)
                    const bool cfe_dollar_stop_ok = cfe_sl_breached
                        || (unr < -(ds_lim * 2.0));
                    if (unr < -ds_lim && cfe_dollar_stop_ok) {
                        printf("[DOLLAR-STOP] CandleFlow %s entry=%.2f unr=$%.2f limit=$%.0f sl=%.2f sl_breached=%d -- CLOSING\n",
                               g_candle_flow.pos.is_long?"LONG":"SHORT",
                               g_candle_flow.pos.entry, unr, ds_lim,
                               cfe_eff_sl, (int)cfe_sl_breached);
                        fflush(stdout);
                        g_candle_flow.force_close(s_xau_bid, s_xau_ask, ds_now_ms,
                            [&](const omega::TradeRecord& tr) {
                                handle_closed_trade(tr);
                                if (!g_candle_flow.shadow_mode)
                                    send_live_order("XAUUSD", tr.side=="SHORT", tr.size, tr.exitPrice);
                            });
                    } else if (unr < -ds_lim) {
                        // Dollar stop wanted to fire but SL not yet breached -- log only
                        static int64_t s_ds_skip_log = 0;
                        if (ds_now - s_ds_skip_log >= 10) {
                            s_ds_skip_log = ds_now;
                            printf("[DOLLAR-STOP-SKIP] CandleFlow %s unr=$%.2f > limit=$%.0f but sl=%.2f not breached (bid=%.2f ask=%.2f) -- letting SL handle\n",
                                   g_candle_flow.pos.is_long?"LONG":"SHORT",
                                   unr, ds_lim, cfe_eff_sl, s_xau_bid, s_xau_ask);
                            fflush(stdout);
                        }
                    }
                }
                // MacroCrash
                if (g_macro_crash.has_open_position()) {
                    const double unr = xau_unr(g_macro_crash.pos.is_long,
                                               g_macro_crash.pos.entry,
                                               g_macro_crash.pos.size);
                    if (unr < -ds_lim) {
                        printf("[DOLLAR-STOP] MacroCrash %s entry=%.2f unr=$%.2f limit=$%.0f -- CLOSING\n",
                               g_macro_crash.pos.is_long?"LONG":"SHORT",
                               g_macro_crash.pos.entry, unr, ds_lim);
                        fflush(stdout);
                        // CRITICAL FIX: pass ds_now_ms (milliseconds) not ds_now (seconds).
                        // ds_now in seconds caused cooldown to be set ~1700s in the past
                        // (seconds treated as ms), allowing immediate re-entry loop.
                        g_macro_crash.force_close(s_xau_bid, s_xau_ask, ds_now_ms);
                    }
                }
                // GoldStack (MeanReversion / CompressionBreakout etc)
                if (g_gold_stack.has_open_position()) {
                    const double unr = xau_unr(g_gold_stack.live_is_long(),
                                               g_gold_stack.live_entry(),
                                               g_gold_stack.live_size());
                    if (unr < -ds_lim) {
                        printf("[DOLLAR-STOP] GoldStack %s entry=%.2f unr=$%.2f limit=$%.0f -- CLOSING\n",
                               g_gold_stack.live_is_long()?"LONG":"SHORT",
                               g_gold_stack.live_entry(), unr, ds_lim);
                        fflush(stdout);
                        g_gold_stack.force_close(s_xau_bid, s_xau_ask, 0.0,
                            [&](const omega::TradeRecord& tr) {
                                handle_closed_trade(tr);
                                send_live_order("XAUUSD", tr.side=="SHORT", tr.size, tr.exitPrice);
                            });
                    }
                }
            }  // s_xau_bid valid
        }  // dollar_stop enabled
    }

    // symbol_risk_blocked -- converted to static function (see above on_tick)

    // symbol_gate -- converted to static function (see above on_tick)

    // on_close -- called by BreakoutEngine (CRTP) when a position closes.
    // BreakoutEngine positions are closed by the BROKER via SL/TP orders submitted
    // at entry. The broker sends an ExecutionReport fill -- on_close is called from
    // the fill handler. No market close order needed here; broker already closed it.
    //
    // Cross-asset engines (EsNqDiv, EIAFade, BrentWTI, FxCascade, CarryUnwind, ORB)
    // manage TP/SL in software -- they enter with a market order but place no broker
    // TP/SL orders. When they close, a market close order IS needed.
    // Those engines receive their own dedicated callback below (ca_on_close).
    auto on_close = [&](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);
    };

    // ca_on_close -- for cross-asset and ORB engines. These manage TP/SL in software
    // with no broker-side orders, so closing requires an explicit market order.
    // ca_on_close -- converted to static function above on_tick
    // bracket_on_close -- used exclusively by g_bracket_gold and g_bracket_xag.
    // When the engine closes a position (TP/SL/timeout/force), it calls this with
    // the filled TradeRecord. We:
    //   1. Record/ledger the trade (same as on_close)
    //   2. Send a closing market order (opposite side, same size) in LIVE mode.
    //
    // The bracket engines manage TP/SL/trailing purely in software -- we do NOT
    // submit bracket orders to BlackBull. When the engine decides to exit, we
    // fire a market order here to close the position at the broker.
    //
    // tr.side is the ENTRY side ("LONG"/"SHORT"); to close we flip it.
    // tr.size is the lot size originally submitted at entry.
    // bracket_on_close -- converted to static function above on_tick
    // ?? Global ranking: collect candidates across all symbols ????????????????
    // on_tick fires once per symbol per tick -- not all symbols in one call.
    // We use a static buffer with a time window: collect all candidates that
    // fire within RANKING_WINDOW_MS of each other, then pick the best one.
    // Any candidate older than the window is flushed before adding new ones.
    // ?? Global ranking state ??????????????????????????????????????????????????
    static omega::RankingConfig g_ranking_cfg;
    g_ranking_cfg.max_trades_per_cycle = g_cfg.max_trades_per_cycle;
    static std::vector<omega::TradeCandidate> g_cycle_candidates;
    static int64_t g_cycle_window_start_ms = 0;
    constexpr int64_t RANKING_WINDOW_MS = 500;

    // ?? Supervisor helper -- run supervisor for a given symbol/engine/supervisor ?
    // Returns the decision. Also always ticks the engine for position management
    // regardless of whether new entries are allowed.
    // sup_decision -- converted to static function above on_tick
    // ?? cost_ok() -- mandatory gate for all direct send_live_order calls ?????????
    // Defined BEFORE dispatch/dispatch_bracket (generic lambdas) so MSVC can
    // resolve it at template definition time, not just instantiation time.
    auto cost_ok = [&](const char* csym, double sl_abs, double lot) -> bool {
        const double tp_dist = sl_abs * 1.5;  // conservative TP estimate at 1.5R
        if (!ExecutionCostGuard::is_viable(csym, ask - bid, tp_dist, lot)) {
            g_telemetry.IncrCostBlocked();
            std::cout << "[COST-BLOCKED] " << csym
                      << " spread=" << std::fixed << std::setprecision(5) << (ask - bid)
                      << " tp_dist=" << tp_dist
                      << " lot=" << lot << "\n";
            std::cout.flush();
            return false;
        }
        return true;
    };

    // ?? dispatch -- breakout engine + supervisor gated ?????????????????????????
    // Calls supervisor, gates new entries on allow_breakout, always ticks for
    // position management. Feeds valid signals into global ranking buffer.
    // Accepts optional pre-computed sdec to avoid double-calling sup.update()
    // when dispatch and dispatch_bracket are both called for the same symbol.
    auto dispatch = [&](auto& eng, omega::SymbolSupervisor& sup, bool base_can_enter,
                        const omega::SupervisorDecision* precomputed_sdec = nullptr) {
        // Supervisor always runs every tick -- for regime classification and telemetry.
        // If a pre-computed decision is provided, reuse it -- don't call update() again.
        const auto sdec = precomputed_sdec ? *precomputed_sdec
                                           : sup_decision(sup, eng, base_can_enter, sym, bid, ask);

        // ?? can_enter construction ????????????????????????????????????????????
        // FLAT:            supervisor must allow + base gates must pass
        // COMPRESSION:     supervisor must allow + base gates must pass
        //                  (BreakoutEngine Leak2 fix aborts to FLAT if can_enter=false)
        // BREAKOUT_WATCH:  ARMED -- bypass supervisor, only base gates apply.
        //                  Once armed at breakout-watch stage a supervisor flip must
        //                  not kill it mid-execution. Final re-check below is last defence.
        const bool eng_armed = (eng.phase == omega::Phase::BREAKOUT_WATCH);
        const bool can_enter = eng_armed
            ? base_can_enter
            : (base_can_enter && sdec.allow_breakout);

        if (eng_armed && !sdec.allow_breakout && base_can_enter) {
            static int64_t s_last_armed_log = 0;
            const int64_t now_log = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now_log - s_last_armed_log >= 5) {
                s_last_armed_log = now_log;
                std::cout << "[ARMED-BYPASS] " << eng.symbol
                          << " supervisor allow=0 ignored -- engine in BREAKOUT_WATCH\n";
                std::cout.flush();
            }
        }
        // Session-slot scaling on MIN_BREAKOUT_PCT -- only when idle (FLAT), not mid-setup
        const bool eng_mid_cycle = (eng.phase == omega::Phase::COMPRESSION
                                 || eng.phase == omega::Phase::BREAKOUT_WATCH);
        if (!eng_mid_cycle) {
            static std::unordered_map<std::string, double> s_base_breakout;
            const std::string sym_key = eng.symbol;
            auto it = s_base_breakout.find(sym_key);
            if (it == s_base_breakout.end()) {
                s_base_breakout[sym_key] = eng.MIN_BREAKOUT_PCT;
                it = s_base_breakout.find(sym_key);
            }
            const double base = it->second;
            const double mult = omega::session_breakout_mult(g_macro_ctx.session_slot);
            eng.MIN_BREAKOUT_PCT = base * std::max(0.70, std::min(1.30, mult));
        }
        // Pass supervisor regime name (EXPANSION_BREAKOUT/QUIET_COMPRESSION etc) not macro
        // regime (RISK_ON/NEUTRAL). The pyramid logic in BreakoutEngine checks for
        // EXPANSION_BREAKOUT/TREND_CONTINUATION -- those are supervisor regime names.
        const char* eng_regime = omega::regime_name(sdec.regime);
        // Adaptive TP: compress target in LOW/CRUSH vol so it actually fills.
        // CRUSH=0.70x, LOW=0.85x, NORMAL=1.00x, HIGH=1.15x -- set each tick.
        eng.EDGE_CFG.tp_vol_mult = static_cast<double>(g_regime_adaptor.tp_vol_mult(sym));
        const auto sig = eng.update(bid, ask, rtt_check, eng_regime, on_close, can_enter);
        g_telemetry.UpdateEngineState(sym.c_str(),
            static_cast<int>(eng.phase), eng.comp_high, eng.comp_low,
            eng.recent_vol_pct, eng.base_vol_pct, eng.signal_count);

        // ?? Pyramid add-on dispatch ???????????????????????????????????????????
        // engine sets pyramid_pending=true when trail1 arm + expansion regime met.
        // We send the add-on order here and clear the flag so it fires exactly once.
        if (eng.pos.pyramid_pending && eng.pos.pyramid_entry > 0.0) {
            eng.pos.pyramid_pending = false;
            const double pyr_sl_abs_raw = std::fabs(eng.pos.pyramid_entry - eng.pos.pyramid_sl);
            const double pyr_sl_abs = std::min(pyr_sl_abs_raw, 3.0);  // cap pyramid SL at $3
            // Fix: use half of the actual patched base lot (eng.pos.size) not a fresh
            // compute_size() which recalculates from eng.ENTRY_SIZE and can mismatch
            // the lot that was actually sent to the broker for the base trade.
            const double pyr_lot    = std::min(std::max(0.01, eng.pos.size * 0.5), 0.20);
            std::cout << "\033[1;36m[PYRAMID] " << sym
                      << " " << (eng.pos.is_long ? "LONG" : "SHORT")
                      << " add-on entry=" << eng.pos.pyramid_entry
                      << " tp=" << eng.pos.pyramid_tp
                      << " sl=" << eng.pos.pyramid_sl
                      << " size=" << pyr_lot << "\033[0m\n";
            std::cout.flush();
            // ?? Pyramid L2 gate ???????????????????????????????????????????
            // Don't add-on into absorption (institutional fading) or a wall
            // directly above/below the pyramid entry toward TP.
            bool pyr_l2_ok = true;
            {
                const bool absorbing = g_edges.absorption.is_absorbing(
                    sym, eng.pos.is_long);
                bool wall_in_dir = false;
                const std::string_view psv(sym);
                if      (psv == "XAUUSD")  wall_in_dir = eng.pos.is_long
                    ? g_macro_ctx.gold_wall_above : g_macro_ctx.gold_wall_below;
                else if (psv == "US500.F" || psv == "USTEC.F" ||
                         psv == "DJ30.F"  || psv == "NAS100")
                    wall_in_dir = eng.pos.is_long
                        ? g_macro_ctx.sp_wall_above : g_macro_ctx.sp_wall_below;
                else if (psv == "EURUSD") wall_in_dir = eng.pos.is_long
                    ? g_macro_ctx.eur_wall_above : g_macro_ctx.eur_wall_below;
                else if (psv == "GBPUSD") wall_in_dir = eng.pos.is_long
                    ? g_macro_ctx.gbp_wall_above : g_macro_ctx.gbp_wall_below;
                if (absorbing || wall_in_dir) {
                    pyr_l2_ok = false;
                    printf("[PYRAMID-L2-BLOCK] %s %s absorb=%d wall=%d -- pyramid suppressed\n",
                           sym.c_str(), eng.pos.is_long?"LONG":"SHORT",
                           absorbing?1:0, wall_in_dir?1:0);
                }
            }
            // Also block pyramid during session transition noise windows
            {
                struct tm ti_pyr{}; const auto t_pyr = std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now());
                gmtime_s(&ti_pyr, &t_pyr);
                const int mins_pyr = ti_pyr.tm_hour * 60 + ti_pyr.tm_min;
                const bool in_transition = (mins_pyr >= 1320 && mins_pyr < 1330)
                                        || (mins_pyr >= 0    && mins_pyr < 15);
                if (in_transition) pyr_l2_ok = false;
            }
            if (pyr_l2_ok && cost_ok(sym.c_str(), pyr_sl_abs, pyr_lot))
                send_live_order(sym, eng.pos.is_long, pyr_lot, eng.pos.pyramid_entry);
        }

        if (!sig.valid) return;

        const double sl_abs_raw = sig.entry * eng.SL_PCT / 100.0;
        // ATR-normalised SL floor: prevent oversized lots when comp_range is tiny.
        // Never size from an SL smaller than half the slow ATR baseline.
        const double sl_abs = g_adaptive_risk.vol_scaler.atr_sl_floor(sym, sl_abs_raw);
        if (sl_abs > sl_abs_raw) {
            static thread_local int64_t s_atr_log = 0;
            if (nowSec() - s_atr_log > 30) {
                s_atr_log = nowSec();
                printf("[ATR-SL-FLOOR] %s sl_raw=%.5f ? sl_floor=%.5f (ATR slow=%.5f)\n",
                       sym.c_str(), sl_abs_raw, sl_abs,
                       g_adaptive_risk.vol_scaler.atr_slow(sym));
            }
        }
        // Compute lot_size but do NOT write back to eng.ENTRY_SIZE yet.
        // eng.ENTRY_SIZE must only be updated if the trade actually executes.
        const double lot_size_base = compute_size(sym, sl_abs, ask - bid, eng.ENTRY_SIZE);
        // Vol-regime size scale from RegimeAdaptor -- CRUSH=1.10, HIGH=0.75, NORMAL=1.0
        // Previously computed but never applied -- was dead code
        const double vol_mult = static_cast<double>(
            g_regime_adaptor.vol_size_scale(sym));
        // ?? Adaptive risk adjustment: Kelly + DD throttle + vol regime ????????
        // Pull current daily loss for this symbol from SymbolRiskState
        double sym_daily_loss = 0.0;
        int    sym_consec     = 0;
        {
            std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
            auto it = g_sym_risk.find(sym);
            if (it != g_sym_risk.end()) {
                sym_daily_loss = std::max(0.0, -it->second.daily_pnl); // positive = loss
                sym_consec     = it->second.consec_losses;
            }
        }
        double lot_size = g_adaptive_risk.adjusted_lot(
            sym, lot_size_base * vol_mult, sym_daily_loss, g_cfg.daily_loss_limit, sym_consec);

        // ?? TOD-weighted lot scaling ??????????????????????????????????????
        // Scale down on marginal time-of-day buckets (WR < 55%) instead of
        // binary block/allow. Reduces size 10-40% on borderline windows.
        {
            const double tod_mult = g_edges.tod.size_scale(sym, "ALL", nowSec());
            if (tod_mult < 1.0) {
                printf("[TOD-SCALE] %s lot %.4f ? %.4f (tod_mult=%.2f)\n",
                       sym.c_str(), lot_size, lot_size * tod_mult, tod_mult);
                lot_size *= tod_mult;
                lot_size = std::max(0.01, std::floor(lot_size * 100.0 + 0.5) / 100.0);
            }
        }

        // ?? HTF bias size scale ???????????????????????????????????????????
        // 1.0? when daily+intraday both agree with direction (Jane Street 2/2 rule).
        // 0.5? when both TFs oppose direction -- trade is counter-trend on all TFs.
        // 0.75? when TFs are mixed/neutral -- modest size reduction for uncertainty.
        {
            const double htf_mult = g_htf_filter.size_scale(sym, sig.is_long);
            if (htf_mult < 1.0) {
                static thread_local int64_t s_htf_log = 0;
                if (nowSec() - s_htf_log > 30) {
                    s_htf_log = nowSec();
                    printf("[HTF-BIAS] %s %s bias=%s ? lot %.4f ? %.2f\n",
                           sym.c_str(), sig.is_long ? "LONG" : "SHORT",
                           g_htf_filter.bias_name(sym),
                           lot_size, htf_mult);
                }
                lot_size *= htf_mult;
                lot_size = std::max(0.01, std::floor(lot_size * 100.0 + 0.5) / 100.0);
            }
        }

        omega::TradeCandidate cand = omega::build_candidate(
            omega::EdgeResult{
                sig.net_edge > 0 ? sig.net_edge : 0.0,
                0.0, sig.net_edge,
                sig.tp, sig.sl, static_cast<double>(lot_size),
                sig.breakout_strength, sig.momentum_score, sig.vol_score,
                true
            },
            sig.is_long, sig.entry, sym.c_str());

        const int64_t now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_ms - g_cycle_window_start_ms > RANKING_WINDOW_MS) {
            g_cycle_candidates.clear();
            g_cycle_window_start_ms = now_ms;
        }

        // Per-symbol duplicate guard: if this symbol already has a candidate in the
        // current window, replace it only if the new score is better -- never double-enter.
        {
            auto existing = std::find_if(g_cycle_candidates.begin(), g_cycle_candidates.end(),
                [&sym](const omega::TradeCandidate& c) {
                    return std::string(c.symbol) == sym;
                });
            if (existing != g_cycle_candidates.end()) {
                if (cand.score > existing->score) {
                    *existing = cand;  // replace with higher-scoring candidate
                }
                // Either way, don't add a second entry for same symbol
            } else {
                g_cycle_candidates.push_back(cand);
            }
        }

        auto selected = omega::select_best_trades(g_cycle_candidates, g_ranking_cfg);
        if (selected.empty()) {
            std::cout << "[RANKED OUT] " << sym << " no valid candidates\n";
            std::cout.flush();
            g_cycle_candidates.clear();  // don't let invalid candidates persist into next window
            return;
        }
        const omega::TradeCandidate& best = selected[0];
        if (std::string(best.symbol) != sym) {
            std::cout << "[RANKED OUT] " << sym
                      << " outranked by " << best.symbol
                      << " score=" << best.score << "\n";
            std::cout.flush();
            // Do NOT clear g_cycle_candidates -- the winner stays live for execution
            // on its own dispatch call this same tick. But remove this symbol's
            // losing candidate so it cannot re-compete on the next tick.
            g_cycle_candidates.erase(
                std::remove_if(g_cycle_candidates.begin(), g_cycle_candidates.end(),
                    [&sym](const omega::TradeCandidate& c) {
                        return std::string(c.symbol) == sym;
                    }),
                g_cycle_candidates.end());
            return;
        }

        // Final supervisor re-check before execution.
        // eng.phase is FLAT at this point (set inside update() when signal fires).
        // If supervisor flipped to NO_TRADE between arming and breakout, block here.
        if (!sdec.allow_breakout) {
            std::cout << "[ENG-" << sym << "] BLOCKED: supervisor_recheck allow=0 at execution\n";
            std::cout.flush();
            g_cycle_candidates.clear();
            return;
        }

        // Cost guard: block if spread+commission+slippage > expected gross ? 1.5
        {
            const double tp_dist = std::fabs(sig.tp - sig.entry);
            if (!ExecutionCostGuard::is_viable(sym.c_str(), ask - bid, tp_dist, lot_size)) {
                g_telemetry.IncrCostBlocked();
                g_cycle_candidates.clear();
                return;
            }
        }

        // All gates passed -- commit sizing and execute.
        // eng.ENTRY_SIZE and telemetry written here only, after all gates cleared.
        eng.ENTRY_SIZE  = lot_size;
        // CRITICAL: patch pos.size with the correct risk-based lot size.
        // pos.size was set to edge.size inside compute_edge_and_execution which uses
        // account_equity*0.002/sl_dist -- an internal formula that bypasses risk_per_trade_usd
        // and the per-symbol lot caps. Without this patch, tr.pnl = move * edge.size
        // (potentially 5-10 lots) instead of move * lot_size (0.01-0.10 lots).
        eng.pos.size    = lot_size;
        g_telemetry.UpdateLastSignal(sym.c_str(),
            sig.is_long ? "LONG" : "SHORT", sig.entry, sig.reason,
            omega::regime_name(sdec.regime), regime.c_str(), "BREAKOUT",
            sig.tp, sig.sl);
        std::cout << "\033[1;" << (sig.is_long ? "32" : "31") << "m"
                  << "[OMEGA] " << sym << " " << (sig.is_long ? "LONG" : "SHORT")
                  << " entry=" << sig.entry << " tp=" << sig.tp << " sl=" << sig.sl
                  << " size=" << lot_size << " score=" << best.score
                  << " sup_regime=" << omega::regime_name(sdec.regime)
                  << " regime=" << regime << "\033[0m\n";
        // ?? Arm partial exit (split TP: 50% at 1R, trail remainder) ??????????
        g_partial_exit.arm(sym, sig.is_long, sig.entry, sig.tp, sig.sl, lot_size,
                           g_adaptive_risk.vol_scaler.atr_fast(sym));
        send_live_order(sym, sig.is_long, lot_size, sig.entry);
        g_cycle_candidates.clear();
    };

    // ?? dispatch_bracket -- bracket engine + supervisor gated ?????????????????
    // Runs supervisor (shared with breakout for the symbol), gates arming on
    // allow_bracket. Position management always runs regardless.
    auto dispatch_bracket = [&](auto& bracket_eng,
                                 omega::SymbolSupervisor& sup,
                                 auto& ref_eng,         // breakout eng for vol/phase data
                                 bool base_can_enter,
                                 double vwap_val,
                                 int& trades_this_min,
                                 int64_t& min_start,
                                 double l2_imb = 0.5,
                                 const omega::SupervisorDecision* precomputed_sdec = nullptr) {
        int fb = 0;
        { std::lock_guard<std::mutex> lk(g_false_break_mtx);
          auto it = g_false_break_counts.find(sym); if (it != g_false_break_counts.end()) fb = it->second; }
        const double bkt_momentum = (ref_eng.base_vol_pct > 0.0)
            ? ((ref_eng.recent_vol_pct - ref_eng.base_vol_pct) / ref_eng.base_vol_pct * 100.0)
            : 0.0;
        // in_compression: true when breakout engine is in COMPRESSION/WATCH,
        // OR when the bracket engine itself has ARMED (it detected compression independently).
        // Without this, bracket ARMED state is invisible to the supervisor -- it scores
        // the symbol as HIGH_RISK/no_dominant and blocks the bracket it should allow.
        const bool bracket_detected_compression =
            (bracket_eng.phase == omega::BracketPhase::ARMED ||
             bracket_eng.phase == omega::BracketPhase::PENDING);
        const bool in_compression_for_sup =
            (ref_eng.phase == omega::Phase::COMPRESSION) || bracket_detected_compression;
        // Reuse pre-computed decision if provided -- avoids double-calling sup.update()
        // which causes allow=1/allow=0 flicker when dispatch + dispatch_bracket both
        // run for the same symbol on the same tick.
        const auto sdec = precomputed_sdec ? *precomputed_sdec
                                           : sup.update(
                                               bid, ask,
                                               ref_eng.recent_vol_pct, ref_eng.base_vol_pct,
                                               bkt_momentum,
                                               ref_eng.comp_high, ref_eng.comp_low,
                                               in_compression_for_sup,
                                               fb);

        // Frequency limit
        const int64_t now_ms = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_ms - min_start >= 60000) { min_start = now_ms; trades_this_min = 0; }
        const bool freq_ok = (trades_this_min < 2);

        const bool bracket_open    = bracket_eng.has_open_position();
        const bool bracket_armed   = (bracket_eng.phase == omega::BracketPhase::ARMED);
        const bool bracket_pending = (bracket_eng.phase == omega::BracketPhase::PENDING);

        // ?? Direct spread guard -- belt-and-suspenders beyond supervisor ???????
        // Supervisor spread gate was broken (max_spread_pct defaulted to 0.10=10%,
        // never overridden). Now fixed via apply_supervisor, but also guard here
        // as a second layer. Only blocks NEW arming (IDLE phase) -- never cancels
        // an already ARMED/PENDING/LIVE position.
        // London open guard (07:00-07:15 UTC): spreads blow out on all instruments
        // at session open. Same pattern as gold's in_london_open_noise.
        const bool bracket_spread_blocked = [&]() -> bool {
            if (bracket_open || bracket_armed || bracket_pending) return false; // managing existing
            const double mid_price = (bid + ask) * 0.5;
            if (mid_price <= 0.0) return false;
            const double spread_pct = (ask - bid) / mid_price * 100.0;
            // Use >= with a small epsilon to handle floating-point equality at limit
            if (spread_pct >= sup.cfg.max_spread_pct * 1.001) {
                static thread_local int64_t s_last_log = 0;
                if (nowSec() - s_last_log > 30) {
                    s_last_log = nowSec();
                    printf("[BRACKET-SPREAD-BLOCK] %s spread_pct=%.3f%% >= max=%.3f%%\n",
                           sym.c_str(), spread_pct, sup.cfg.max_spread_pct);
                }
                ++g_gov_spread;
                return true;
            }
            return false;
        }();
        // London open guard: 07:00-07:15 UTC -- violent liquidity sweeps on all instruments
        const bool bracket_london_noise = [&]() -> bool {
            if (bracket_open || bracket_armed || bracket_pending) return false;
            const auto t_bn = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            struct tm ti_bn{}; gmtime_s(&ti_bn, &t_bn);
            const int mins_utc = ti_bn.tm_hour * 60 + ti_bn.tm_min;
            return (mins_utc >= 420 && mins_utc < 435); // 07:00-07:15 UTC
        }();

        // ?? Trend bias: L2-aware counter-trend suppression + pyramiding ???????
        // Update L2 adjustment on the trend state every tick (throttled internally to 5s)
        BracketTrendState& trend = g_bracket_trend[sym];
        trend.update_l2(l2_imb, now_ms);
        const bool trend_blocked   = trend.counter_trend_blocked(now_ms);
        const bool pyramid_ok      = trend.pyramid_allowed(l2_imb, now_ms);

        // Pyramiding: when trend bias active + L2 strongly confirms + position already open
        // in trend direction ? allow a second bracket arm (base_can_enter bypasses !bracket_open).
        // The second arm uses PYRAMID_SIZE_MULT of normal size.
        const bool is_pyramiding   = pyramid_ok && bracket_open &&
                                     ((trend.bias == 1  && bracket_eng.pos.is_long) ||
                                      (trend.bias == -1 && !bracket_eng.pos.is_long));

        const bool can_arm         = (base_can_enter && sdec.allow_bracket && freq_ok
                                      && (!bracket_open || is_pyramiding)
                                      && !trend_blocked
                                      && !bracket_spread_blocked
                                      && !bracket_london_noise)
                                   // ?? Supervisor chop_detected price-break override ???????
                                   // Same logic as gold: if ARMED and price breaks the bracket
                                   // level by >= 2pt, bypass supervisor allow_bracket=0.
                                   // Hard gates (base_can_enter, freq_ok, no open pos) still apply.
                                   || ([&]() -> bool {
                                       if (!bracket_armed) return false;
                                       if (!base_can_enter || !freq_ok) return false;
                                       if (bracket_open && !is_pyramiding) return false;
                                       if (trend_blocked || bracket_spread_blocked || bracket_london_noise) return false;
                                       const double bhi = bracket_eng.bracket_high;
                                       const double blo = bracket_eng.bracket_low;
                                       const bool brk_hi_hit = (bhi > 0.0) && (ask >= bhi + 2.0);
                                       const bool brk_lo_hit = (blo > 0.0) && (bid <= blo - 2.0);
                                       if (!brk_hi_hit && !brk_lo_hit) return false;
                                       static thread_local int64_t s_disp_ovr_log = 0;
                                       const int64_t now_do = static_cast<int64_t>(std::time(nullptr));
                                       if (now_do - s_disp_ovr_log >= 5) {
                                           s_disp_ovr_log = now_do;
                                           printf("[BRK-CHOP-OVERRIDE] %s supervisor chop bypassed -- "
                                                  "price broke %s by %.1fpt\n",
                                                  sym.c_str(),
                                                  brk_hi_hit ? "brk_hi" : "brk_lo",
                                                  brk_hi_hit ? (ask - bhi) : (blo - bid));
                                           fflush(stdout);
                                       }
                                       return true;
                                   }());

        // Gate logic by phase:
        //   IDLE    ? can_arm: supervisor + session + freq + trend bias all required
        //   ARMED   ? true: structure qualified, timer must run uninterrupted
        //   PENDING ? true: orders at broker, only timeout should cancel
        //   LIVE    ? can_manage: allow force-close on session/risk gate
        const bool can_manage      = (bracket_armed || bracket_pending) ? true
                                                     : (base_can_enter && sdec.allow_bracket);

        bracket_eng.on_tick(bid, ask, now_ms,
            (bracket_open || bracket_armed) ? can_manage : can_arm,
            regime.c_str(), bracket_on_close, vwap_val, l2_imb);

        // Update bracket state in telemetry snapshot every tick so GUI shows live levels
        g_telemetry.UpdateBracketState(sym.c_str(),
            static_cast<int>(bracket_eng.phase),
            bracket_eng.bracket_high,
            bracket_eng.bracket_low);

        const auto bsigs = bracket_eng.get_signals();
        if (bsigs.valid) {
            const double raw_sl_dist = std::fabs(bsigs.long_entry - bsigs.long_sl);
            // Pyramid: cap SL and lot to limit add-on risk
            const double eff_sl_dist = is_pyramiding
                ? std::min(raw_sl_dist, raw_sl_dist * 0.5)  // 50% of natural SL for pyramids
                : raw_sl_dist;
            const double base_lot = compute_size(sym, eff_sl_dist, ask - bid,
                bracket_eng.ENTRY_SIZE);
            const double raw_lot = is_pyramiding ? (base_lot * PYRAMID_SIZE_MULT) : base_lot;
            // Cap pyramid lot at 50% of normal max lot for this symbol
            const double max_pyr_lot = is_pyramiding ? 0.10 : 1.0;  // indices: max 0.10 lots pyramid
            const double lot = std::min(raw_lot, max_pyr_lot);
            // Cost guard: ensure spread+cost is covered by bracket TP distance
            {
                const double tp_dist = raw_sl_dist *
                    (bracket_eng.RR > 0.0 ? bracket_eng.RR : 1.5);
                if (!ExecutionCostGuard::is_viable(sym.c_str(), ask - bid, tp_dist, lot)) {
                    g_telemetry.IncrCostBlocked();
                    return;
                }
            }

            // ?? Bracket R:R floor ?????????????????????????????????????????
            // RR config is the minimum acceptable R:R for bracket signals.
            // Enforce 1.5 as a hard floor regardless of config.
            {
                const double bkt_rr = bracket_eng.RR > 0.0 ? bracket_eng.RR : 1.5;
                if (bkt_rr < 1.5) {
                    printf("[RR-FLOOR] %s BRACKET blocked: R:R=%.2f < 1.5 floor\n",
                           sym.c_str(), bkt_rr);
                    return;
                }
            }

            // ?? Bracket L2 microstructure gate ????????????????????????????
            // Brackets bypass enter_directional so they previously skipped the
            // full L2 scoring layer. Apply entry_score_l2 to both directions;
            // if BOTH legs score <= -3, the setup is structurally blocked.
            // If only one leg is blocked, proceed with the other at half size.
            {
                double bkt_microprice = 0.0, bkt_l2_imb = l2_imb;
                bool bkt_vac_ask = false, bkt_vac_bid = false;
                bool bkt_wall_above = false, bkt_wall_below = false;
                const std::string_view sv_bkt(sym);
                if (sv_bkt == "XAUUSD") {
                    bkt_microprice  = g_macro_ctx.gold_microprice_bias;
                    bkt_vac_ask     = g_macro_ctx.gold_vacuum_ask;
                    bkt_vac_bid     = g_macro_ctx.gold_vacuum_bid;
                    bkt_wall_above  = g_macro_ctx.gold_wall_above;
                    bkt_wall_below  = g_macro_ctx.gold_wall_below;
                } else if (sv_bkt == "US500.F" || sv_bkt == "USTEC.F" ||
                           sv_bkt == "DJ30.F"  || sv_bkt == "NAS100") {
                    bkt_microprice  = g_macro_ctx.sp_microprice_bias;
                    bkt_vac_ask     = g_macro_ctx.sp_vacuum_ask;
                    bkt_vac_bid     = g_macro_ctx.sp_vacuum_bid;
                    bkt_wall_above  = g_macro_ctx.sp_wall_above;
                    bkt_wall_below  = g_macro_ctx.sp_wall_below;
                } else if (sv_bkt == "USOIL.F" || sv_bkt == "BRENT") {
                    bkt_microprice  = g_macro_ctx.cl_microprice_bias;
                    bkt_vac_ask     = g_macro_ctx.cl_vacuum_ask;
                    bkt_vac_bid     = g_macro_ctx.cl_vacuum_bid;
                } else if (sv_bkt == "XAGUSD") {
                    bkt_microprice  = g_macro_ctx.xag_microprice_bias;
                    bkt_vac_ask     = bkt_l2_imb < 0.30;
                    bkt_vac_bid     = bkt_l2_imb > 0.70;
                } else if (sv_bkt == "EURUSD") {
                    bkt_microprice = g_macro_ctx.eur_microprice_bias;
                    bkt_vac_ask    = g_macro_ctx.eur_vacuum_ask;
                    bkt_vac_bid    = g_macro_ctx.eur_vacuum_bid;
                    bkt_wall_above = g_macro_ctx.eur_wall_above;
                    bkt_wall_below = g_macro_ctx.eur_wall_below;
                } else if (sv_bkt == "GBPUSD") {
                    bkt_microprice = g_macro_ctx.gbp_microprice_bias;
                    bkt_vac_ask    = g_macro_ctx.gbp_vacuum_ask;
                    bkt_vac_bid    = g_macro_ctx.gbp_vacuum_bid;
                    bkt_wall_above = g_macro_ctx.gbp_wall_above;
                    bkt_wall_below = g_macro_ctx.gbp_wall_below;
                } else if (sv_bkt == "AUDUSD") {
                    bkt_vac_ask = g_macro_ctx.aud_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.aud_vacuum_bid;
                } else if (sv_bkt == "NZDUSD") {
                    bkt_vac_ask = g_macro_ctx.nzd_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.nzd_vacuum_bid;
                } else if (sv_bkt == "USDJPY") {
                    bkt_vac_ask = g_macro_ctx.jpy_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.jpy_vacuum_bid;
                } else if (sv_bkt == "GER40") {
                    bkt_microprice = g_macro_ctx.ger40_microprice_bias;
                    bkt_vac_ask    = g_macro_ctx.ger40_vacuum_ask;
                    bkt_vac_bid    = g_macro_ctx.ger40_vacuum_bid;
                    bkt_wall_above = g_macro_ctx.ger40_wall_above;
                    bkt_wall_below = g_macro_ctx.ger40_wall_below;
                } else if (sv_bkt == "UK100") {
                    bkt_vac_ask = g_macro_ctx.uk100_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.uk100_vacuum_bid;
                } else if (sv_bkt == "ESTX50") {
                    bkt_vac_ask = g_macro_ctx.estx50_vacuum_ask;
                    bkt_vac_bid = g_macro_ctx.estx50_vacuum_bid;
                } else {
                    // Fallback: L2 imbalance proxy for any uncovered symbol
                    bkt_vac_ask = bkt_l2_imb < 0.30;
                    bkt_vac_bid = bkt_l2_imb > 0.70;
                }
                const int score_long  = g_edges.entry_score_l2(
                    sym, bsigs.long_entry,  true,  bsigs.long_tp,
                    nowSec(), bkt_microprice, bkt_l2_imb, bkt_vac_ask, bkt_wall_above);
                const int score_short = g_edges.entry_score_l2(
                    sym, bsigs.short_entry, false, bsigs.short_tp,
                    nowSec(), bkt_microprice, bkt_l2_imb, bkt_vac_bid, bkt_wall_below);
                if (score_long <= -3 && score_short <= -3) {
                    printf("[EDGE-BLOCK-BKT] %s BRACKET both legs blocked: L=%d S=%d\n",
                           sym.c_str(), score_long, score_short);
                    return;
                }
            }

            // ?? Cross-engine dedup (inline -- lambda not yet in scope here) ?
            {
                std::lock_guard<std::mutex> _lk(g_dedup_mtx);
                auto _it = g_last_cross_entry.find(sym);
                if (_it != g_last_cross_entry.end() &&
                    (nowSec() - _it->second) < CROSS_ENG_DEDUP_SEC) {
                    printf("[CROSS-DEDUP] %s BRACKET blocked -- another engine entered %.0fs ago\n",
                           sym.c_str(), static_cast<double>(nowSec() - _it->second));
                    return;
                }
                g_last_cross_entry[sym] = nowSec();
            }

            // Encode bracket levels + trend state in reason for GUI
            char bracket_reason[80];
            snprintf(bracket_reason, sizeof(bracket_reason), "HI:%.2f LO:%.2f bias:%d l2:%.2f",
                     bsigs.long_entry, bsigs.short_entry, trend.bias, l2_imb);
            g_telemetry.UpdateLastSignal(sym.c_str(), "BRACKET", bsigs.long_entry, bracket_reason,
                omega::regime_name(sdec.regime), regime.c_str(), "BRACKET",
                bsigs.long_tp, bsigs.long_sl);
            std::cout << "\033[1;33m[BRACKET] " << sym
                      << " sup_regime=" << omega::regime_name(sdec.regime)
                      << " bracket_score=" << sdec.bracket_score
                      << " winner=" << sdec.winner
                      << " bias=" << trend.bias
                      << " l2=" << std::fixed << std::setprecision(2) << l2_imb
                      << (is_pyramiding ? " PYRAMID" : "")
                      << "\033[0m\n";

            // L2-aware order sizing:
            //   When trend bias active: send trend-direction leg at full size,
            //   counter-trend leg at half size (hedge leg, expect cancellation).
            //   When no bias: both legs at full size (standard bracket).
            std::string long_id, short_id;
            if (trend.bias != 0) {
                const bool long_is_trend  = (trend.bias == 1);
                const double trend_lot    = lot;
                const double counter_lot  = lot * 0.5;
                long_id  = send_live_order(sym, true,  long_is_trend  ? trend_lot : counter_lot, bsigs.long_entry);
                short_id = send_live_order(sym, false, !long_is_trend ? trend_lot : counter_lot, bsigs.short_entry);
                printf("[BRACKET-L2] %s bias=%d trend_lot=%.4f counter_lot=%.4f l2=%.3f\n",
                       sym.c_str(), trend.bias, trend_lot, counter_lot, l2_imb);
            } else {
                long_id  = send_live_order(sym, true,  lot, bsigs.long_entry);
                short_id = send_live_order(sym, false, lot, bsigs.short_entry);
            }
            bracket_eng.pending_long_clOrdId  = long_id;
            bracket_eng.pending_short_clOrdId = short_id;
            // Tag pyramid arms so bracket_on_close can enforce SL cooldown
            if (is_pyramiding) {
                g_pyramid_clordids.insert(sym);
            }
            ++trades_this_min;
        }
    };

    // ?? cost_ok() -- mandatory gate for ALL direct send_live_order calls ???????
    // Every engine signal that bypasses dispatch()/dispatch_bracket() must call
    // this before executing. Blocks the trade and increments the cost counter
    // if spread + commission + slippage exceeds expected gross ? EDGE_MULTIPLIER.
    // Params: symbol, sl_abs (SL distance in price points), lot.
    // Uses the same ExecutionCostGuard that dispatch() and dispatch_bracket() use,
    // with RR=1.5 as the TP estimate (conservative: actual RR is often 2.0+).
    // ?? Cross-engine deduplication ????????????????????????????????????????????
    // Per-symbol 30s lockout across all engine types -- statics live at file scope.
    // cross_engine_dedup_ok -- converted to static function above on_tick
    // Stamp dedup timestamp -- called only after all gates pass and trade executes
    // cross_engine_dedup_stamp -- converted to static function above on_tick
    // ?? enter_directional: unified entry helper for all cross-asset engines ???
    // Replaces the repeated pattern: compute_size ? cost_ok ? send_live_order
    // Also applies adaptive risk (adjusted_lot) and arms partial exit.
    // sig: must have .valid, .is_long, .entry, .sl, .tp (or computed tp_dist)
    // fallback_lot: default size when risk sizing disabled (0.01 for most CA engines)
    // sym_override: use this symbol for sizing/corr if different from outer sym
    // enter_directional -- converted to static function above on_tick
    // ?? Partial exit tick check ???????????????????????????????????????????????
    // Runs every tick for every symbol. No-op when no partial state is active.
    // When TP1 is hit: sends a market close for the first half and moves SL to BE.
    // When TP2/trailing SL is hit: sends final close for the remainder.
    // Applies in all modes -- shadow simulates the close without sending a real order.
    // XAUUSD + GFE open: GoldFlowEngine manages its own partial internally via
    // manage_position() ? PARTIAL_1R callback. Skip here to prevent duplicate orders.
    const bool gfe_owns_partial = (sym == "XAUUSD" && g_gold_flow.has_open_position());
    if (g_partial_exit.active(sym) && !gfe_owns_partial) {
        double pe_price = 0.0, pe_lot = 0.0;
        using PE = omega::partial::CloseAction;
        const PE act = g_partial_exit.tick(sym, mid, bid, ask, pe_price, pe_lot);
        if (act == PE::PARTIAL || act == PE::FULL) {
            const bool close_is_long = !g_partial_exit.entry_is_long(sym);
            // In LIVE mode send the actual order; in SHADOW simulate the fill at current mid.
            if (g_cfg.mode == "LIVE") {
                send_live_order(sym, close_is_long, pe_lot, pe_price);
            }
            // Log in both modes -- shadow records the simulated partial as if it were real.
            std::printf("[PARTIAL-EXIT]%s %s %s %.2f lots @ %.5f  entry_long=%d\n",
                        g_cfg.mode == "LIVE" ? "" : "[SHADOW]",
                        sym.c_str(),
                        act == PE::PARTIAL ? "TP1-HIT" : "TP2/TRAIL-HIT",
                        pe_lot, pe_price,
                        g_partial_exit.entry_is_long(sym) ? 1 : 0);
            if (act == PE::FULL) {
                g_partial_exit.reset(sym);
            }
        }
    }

    // ?? ACTIVE SYMBOLS GATE ???????????????????????????????????????????????????
    // SIM-VALIDATED: XAUUSD and USOIL.F have proven compression edge.
    // US indices (US500.F, USTEC.F, NAS100, DJ30.F) now routed for
    // NoiseBandMomentumEngine -- Zarattini/Maroy research (Sharpe 3.0-5.9).
    // All other symbols remain hard-blocked until re-validated.
    {
        // All symbols active -- bars now built from FIX ticks directly, no broker bar API needed
        const bool is_active_sym = (sym == "XAUUSD"  || sym == "USOIL.F"  ||
                                    sym == "US500.F" || sym == "USTEC.F"  ||
                                    sym == "NAS100"  || sym == "DJ30.F");
        // XAGUSD hard-blocked: SilverTurtleTick real-tick backtest result:
        // Sharpe=-16.23, MaxDD=$18,381, 0 positive months across 24 months.
        // Root cause: 65% timeout rate, TP=$0.30 requires 49x the actual
        // avg 45-min move. Silver reverts too fast for Turtle architecture.
        // All 12 silver strategies were audited -- none viable. DROP silver.
        if (!is_active_sym) return;
    }

    // ?? Routing -- every symbol goes through supervisor ????????????????????????
    // ── Symbol dispatch ────────────────────────────────────────────────────────
    if      (sym == "US500.F")                          on_tick_us500(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "USTEC.F")                          on_tick_ustec(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "USOIL.F")                          on_tick_oil(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "DJ30.F")                           on_tick_dj30(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "GER40")                            on_tick_ger40(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "UK100")                            on_tick_uk100(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "ESTX50")                           on_tick_estx50(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "XAGUSD")                           on_tick_silver(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "EURUSD")                           on_tick_eurusd(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "GBPUSD")                           on_tick_gbpusd(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "AUDUSD" || sym == "NZDUSD" || sym == "USDJPY")
                                                        on_tick_audusd(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "BRENT")                            on_tick_brent(sym, bid, ask, tradeable, lat_ok, regime, dispatch);
    else if (sym == "NAS100")                           on_tick_nas100(sym, bid, ask, tradeable, lat_ok, regime);
    else if (sym == "XAUUSD")                           on_tick_gold(sym, bid, ask, tradeable, lat_ok, regime, rtt_check);
    else {
        // Confirmation-only symbol (VIX, ES, NAS100, DX etc) -- no engine dispatch
        g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, g_gov_pos, g_gov_consec);
        return;
    }

    g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, g_gov_pos, g_gov_consec);

    // ?? SL cooldown telemetry -- collect active cooldowns across all engines ??
    {
        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));
        std::vector<std::pair<std::string,int>> cooldowns;
        auto chk = [&](auto& eng, const char* name) {
            const int64_t rem = eng.sl_cooldown_until() - now_s;
            if (rem > 0) cooldowns.push_back({name, static_cast<int>(rem)});
        };
        chk(g_eng_sp,     "US500.F");
        chk(g_eng_nq,     "USTEC.F");
        chk(g_eng_cl,     "USOIL.F");
        chk(g_eng_us30,   "DJ30.F");
        chk(g_eng_nas100, "NAS100");
        chk(g_eng_ger30,  "GER40");
        chk(g_eng_uk100,  "UK100");
        chk(g_eng_estx50, "ESTX50");
        chk(g_eng_xag,    "XAGUSD");
        chk(g_eng_eurusd, "EURUSD");
        chk(g_eng_gbpusd, "GBPUSD");
        chk(g_eng_audusd, "AUDUSD");
        chk(g_eng_nzdusd, "NZDUSD");
        chk(g_eng_usdjpy, "USDJPY");
        chk(g_eng_brent,  "BRENT");
        g_telemetry.UpdateSLCooldown(cooldowns);
    }

    // ?? Asia gate + config snapshot ??????????????????????????????????????????
    {
        const auto t_asia = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti_asia; gmtime_s(&ti_asia, &t_asia);
        const int h2 = ti_asia.tm_hour;
        const int asia_open = (!g_cfg.asia_fx_asia_only || (h2 >= 22 || h2 < 7)) ? 1 : 0;
        g_telemetry.UpdateAsiaCfg(asia_open, g_cfg.max_trades_per_cycle, g_cfg.max_open_positions);
        // Push L2 quality flags so GUI shows cTrader status and gold_l2_real indicator
        if (g_telemetry.snap()) {
            g_telemetry.snap()->ctrader_l2_live = g_macro_ctx.ctrader_l2_live ? 1 : 0;
            g_telemetry.snap()->gold_l2_real     = g_macro_ctx.gold_l2_real     ? 1 : 0;
        }
    }

    // ?? Cross-asset engine live state snapshot ???????????????????????????????
    // Written every tick so the GUI always has fresh active/position data.
    // ref_price carries the most meaningful context value per engine type:
    //   ORB      ? range midpoint (0 until range is built)
    //   VWAP_REV ? VWAP proxy used for this instrument
    //   TREND_PB ? EMA50 level (dynamic SL reference)
    //   FX_CASCADE ? 0 (armed state not visible here; signal fires fast)
    {
        // ORB instances
        const double orb_us_mid    = (g_orb_us.range_high()    + g_orb_us.range_low())    > 0.0 ? (g_orb_us.range_high()    + g_orb_us.range_low())    * 0.5 : 0.0;
        const double orb_ger_mid   = (g_orb_ger30.range_high() + g_orb_ger30.range_low())  > 0.0 ? (g_orb_ger30.range_high() + g_orb_ger30.range_low())  * 0.5 : 0.0;
        const double orb_uk_mid    = (g_orb_uk100.range_high()  + g_orb_uk100.range_low())  > 0.0 ? (g_orb_uk100.range_high()  + g_orb_uk100.range_low())  * 0.5 : 0.0;
        const double orb_estx_mid  = (g_orb_estx50.range_high() + g_orb_estx50.range_low()) > 0.0 ? (g_orb_estx50.range_high() + g_orb_estx50.range_low()) * 0.5 : 0.0;
        const double orb_xag_mid   = (g_orb_silver.range_high() + g_orb_silver.range_low()) > 0.0 ? (g_orb_silver.range_high() + g_orb_silver.range_low()) * 0.5 : 0.0;

        auto ca = [&](const char* nm, const char* sym, bool act, bool lng,
                      double ent, double tp, double sl, double ref, int sigs) {
            g_telemetry.UpdateCrossAsset(nm, sym, act?1:0, lng?1:0, ent, tp, sl, ref, sigs);
        };

        // ORB
        ca("ORB_US",    "US500.F", g_orb_us.has_open_position(),    false, 0,0,0, orb_us_mid,   0);
        ca("ORB_GER40", "GER40",   g_orb_ger30.has_open_position(),  false, 0,0,0, orb_ger_mid,  0);
        ca("ORB_UK100", "UK100",   g_orb_uk100.has_open_position(),  false, 0,0,0, orb_uk_mid,   0);
        ca("ORB_ESTX50","ESTX50",  g_orb_estx50.has_open_position(), false, 0,0,0, orb_estx_mid, 0);
        ca("ORB_XAG",   "XAGUSD",  g_orb_silver.has_open_position(), false, 0,0,0, orb_xag_mid,  0);

        // VWAP Reversion -- capture VWAP proxy per instrument
        // SP/NQ share the US ORB range mid; GER40 uses Xetra ORB; EUR uses daily open static
        ca("VWAP_SP",   "US500.F", g_vwap_rev_sp.has_open_position(),     false, 0,0,0, orb_us_mid,  0);
        ca("VWAP_NQ",   "USTEC.F", g_vwap_rev_nq.has_open_position(),     false, 0,0,0, orb_us_mid,  0);
        ca("VWAP_GER40","GER40",   g_vwap_rev_ger40.has_open_position(),  false, 0,0,0, orb_ger_mid, 0);
        ca("VWAP_EUR",  "EURUSD",  g_vwap_rev_eurusd.has_open_position(), false, 0,0,0, 0.0,         0);

        // Trend Pullback -- ref_price = EMA50 (dynamic SL)
        ca("TRENDPB_GOLD","XAUUSD", g_trend_pb_gold.has_open_position(),  false, 0,0,0, g_trend_pb_gold.ema50(),  0);
        ca("TRENDPB_GER", "GER40",  g_trend_pb_ger40.has_open_position(), false, 0,0,0, g_trend_pb_ger40.ema50(), 0);

        // FX Cascade -- one entry covers all three legs; armed state not stored here
        ca("FXCASC_GBP", "GBPUSD", g_ca_fx_cascade.has_open_position(), false, 0,0,0, 0.0, 0);

        // Other cross-asset (EsNq, OilFade, BrentWTI, CarryUnwind)
        ca("ESNQ_DIV",   "US500.F", g_ca_esnq.has_open_position(),        false, 0,0,0, 0.0, 0);
        ca("CARRY_UNW",  "USDJPY",  g_ca_carry_unwind.has_open_position(), false, 0,0,0, 0.0, 0);
    }

    // ?? Real-time cluster dollar exposure ?????????????????????????????????????????
    // Computed every tick from phase==IN_TRADE (3) and open ca_engine positions.
    // Dollar notional = lot_size * tick_value_multiplier(symbol) * direction (1=long, -1=short).
    // For breakout/bracket engines, lot_size is not directly in the snapshot, so we
    // use a fixed 1.0 sentinel scaled by tick_value_multiplier to give relative cluster weight.
    // Exact lot sizing is in the trade ledger; this is for real-time directional exposure.
    {
        // Helper: net dollar exposure for one open position slot
        // phase==3 (IN_TRADE for breakout, LIVE for bracket) = position open
        // For engines that have ca_engine active flags, use those (more precise).
        auto eng_exposure = [](int phase, double lot, double tick_mult, int is_long_hint=1) -> double {
            if (phase != 3) return 0.0;
            return lot * tick_mult * (is_long_hint >= 0 ? 1.0 : -1.0);
        };

        double exp_us = 0.0, exp_eu = 0.0, exp_oil = 0.0;
        double exp_metals = 0.0, exp_jpy = 0.0, exp_egbp = 0.0;

        const auto* sn = g_telemetry.snap();
        if (sn) {
            // Use cross-asset engine live states (have direction) for all cluster symbols
            for (int i = 0; i < sn->ca_engine_count; ++i) {
                const auto& e = sn->ca_engines[i];
                if (!e.active) continue;
                const double tm = tick_value_multiplier(e.symbol);
                // We use 1.0 lot as a relative unit; actual lot unknown from snapshot
                const double dir = e.is_long ? 1.0 : -1.0;
                const double notional = tm * dir;
                const std::string ca_sym(e.symbol);
                using CL = omega::risk::CorrCluster;
                switch (omega::risk::symbol_to_cluster(ca_sym)) {
                    case CL::US_EQUITY: exp_us    += notional; break;
                    case CL::EU_EQUITY: exp_eu    += notional; break;
                    case CL::OIL:       exp_oil   += notional; break;
                    case CL::METALS:    exp_metals+= notional; break;
                    case CL::JPY_RISK:  exp_jpy   += notional; break;
                    case CL::EUR_GBP:   exp_egbp  += notional; break;
                    default: break;
                }
            }
            // Supplement with breakout/bracket phase==3 for symbols not covered by ca_engines
            // (breakout engines don't store direction in snapshot; use +1 as unsigned exposure)
            auto add_bkt = [&](int phase, const char* sym) {
                if (phase != 3) return;
                const double tm = tick_value_multiplier(sym);
                const std::string s(sym);
                using CL = omega::risk::CorrCluster;
                switch (omega::risk::symbol_to_cluster(s)) {
                    case CL::US_EQUITY: exp_us    += tm; break;
                    case CL::EU_EQUITY: exp_eu    += tm; break;
                    case CL::OIL:       exp_oil   += tm; break;
                    case CL::METALS:    exp_metals+= tm; break;
                    case CL::JPY_RISK:  exp_jpy   += tm; break;
                    case CL::EUR_GBP:   exp_egbp  += tm; break;
                    default: break;
                }
            };
            add_bkt(sn->sp_phase,     "US500.F");
            add_bkt(sn->nq_phase,     "USTEC.F");
            add_bkt(sn->cl_phase,     "USOIL.F");
            add_bkt(sn->xau_phase,    "XAUUSD");
            add_bkt(sn->brent_phase,  "BRENT");
            add_bkt(sn->xag_phase,    "XAGUSD");
            add_bkt(sn->eurusd_phase, "EURUSD");
            add_bkt(sn->gbpusd_phase, "GBPUSD");
            add_bkt(sn->audusd_phase, "AUDUSD");
            add_bkt(sn->nzdusd_phase, "NZDUSD");
            add_bkt(sn->usdjpy_phase, "USDJPY");
            add_bkt(sn->bkt_sp.phase,    "US500.F");
            add_bkt(sn->bkt_nq.phase,    "USTEC.F");
            add_bkt(sn->bkt_us30.phase,  "DJ30.F");
            add_bkt(sn->bkt_nas.phase,   "NAS100");
            add_bkt(sn->bkt_ger.phase,   "GER40");
            add_bkt(sn->bkt_uk.phase,    "UK100");
            add_bkt(sn->bkt_estx.phase,  "ESTX50");
            add_bkt(sn->bkt_xag.phase,   "XAGUSD");
            add_bkt(sn->bkt_gold.phase,  "XAUUSD");
            add_bkt(sn->bkt_eur.phase,   "EURUSD");
            add_bkt(sn->bkt_gbp.phase,   "GBPUSD");
            add_bkt(sn->bkt_brent.phase, "BRENT");
        }
        g_telemetry.UpdateExposure(exp_us, exp_eu, exp_oil, exp_metals, exp_jpy, exp_egbp);
    }

    if (g_telemetry.snap()) g_telemetry.snap()->uptime_sec =
        static_cast<int64_t>(std::time(nullptr)) - g_start_time;
}  // ? on_tick
// ?????????????????????????????????????????????????????????????????????????????
static std::vector<std::string> extract_messages(const char* data, int n, bool reset = false) {
    static std::string recv_buf;  // local static -- no global sharing
    if (reset) { recv_buf.clear(); return {}; }
    recv_buf.append(data, static_cast<size_t>(n));
    std::vector<std::string> msgs;
    while (true) {
        const size_t bs = recv_buf.find("8=FIX");
        if (bs == std::string::npos) { recv_buf.clear(); break; }
        if (bs > 0u) recv_buf = recv_buf.substr(bs);
        const size_t bl_pos = recv_buf.find("\x01" "9=");
        if (bl_pos == std::string::npos) break;
        const size_t bl_start = bl_pos + 3u;
        const size_t bl_end   = recv_buf.find('\x01', bl_start);
        if (bl_end == std::string::npos) break;
        int body_len = 0;
        try { body_len = std::stoi(recv_buf.substr(bl_start, bl_end - bl_start)); }
        catch (...) { recv_buf = recv_buf.substr(bl_end); continue; } // malformed -- skip
        const size_t hdr_end  = bl_end + 1u;
        const size_t msg_end  = hdr_end + static_cast<size_t>(body_len) + 7u;
        if (msg_end > recv_buf.size()) break;
        msgs.push_back(recv_buf.substr(0u, msg_end));
        recv_buf = recv_buf.substr(msg_end);
    }
    return msgs;
}

// ?????????????????????????????????????????????????????????????????????????????
// FIX dispatch
// ?????????????????????????????????????????????????????????????????????????????
#include "fix_dispatch.hpp"

