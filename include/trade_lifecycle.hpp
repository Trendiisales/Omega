#pragma once
// trade_lifecycle.hpp -- extracted from main.cpp
// Section: trade_lifecycle (original lines 3982-5499)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static void handle_closed_trade(const omega::TradeRecord& tr_in) {
    omega::TradeRecord tr = tr_in;

    // ?? L2 liveness stamp -- set on every close so CSV audit is accurate ??????
    // tr.l2_live defaults false. Stamp from live MacroContext flags here so
    // every CSV row reflects actual L2 state at time of close.
    // XAUUSD/XAGUSD: use gold_l2_real (XAUUSD DOM specifically via ctid=43014358).
    // All other symbols: use ctrader_l2_live (any cTrader depth event received).
    {
        const std::string& sym = tr.symbol;
        if (sym == "XAUUSD" || sym == "XAGUSD") {
            tr.l2_live       = g_macro_ctx.gold_l2_real;
            tr.l2_imbalance  = g_macro_ctx.gold_l2_imbalance;
        } else {
            tr.l2_live = g_macro_ctx.ctrader_l2_live;
            // l2_imbalance already on tr from enter_directional snapshot if set;
            // only overwrite if still at default 0.5 (was never populated)
            if (tr.l2_imbalance == 0.5) {
                if      (sym == "US500.F")                                tr.l2_imbalance = g_macro_ctx.sp_l2_imbalance;
                else if (sym == "USTEC.F" || sym == "NAS100")             tr.l2_imbalance = g_macro_ctx.nq_l2_imbalance;
                else if (sym == "DJ30.F")                                 tr.l2_imbalance = g_macro_ctx.us30_l2_imbalance;
                else if (sym == "USOIL.F" || sym == "BRENT")              tr.l2_imbalance = g_macro_ctx.cl_l2_imbalance;
                else if (sym == "EURUSD")                                 tr.l2_imbalance = g_macro_ctx.eur_l2_imbalance;
                else if (sym == "GBPUSD")                                 tr.l2_imbalance = g_macro_ctx.gbp_l2_imbalance;
                else if (sym == "AUDUSD")                                 tr.l2_imbalance = g_macro_ctx.aud_l2_imbalance;
                else if (sym == "NZDUSD")                                 tr.l2_imbalance = g_macro_ctx.nzd_l2_imbalance;
                else if (sym == "USDJPY")                                 tr.l2_imbalance = g_macro_ctx.jpy_l2_imbalance;
                else if (sym == "GER40")                                  tr.l2_imbalance = g_macro_ctx.ger40_l2_imbalance;
            }
        }
    }

    // ?? PNL SANITY CAP ???????????????????????????????????????????????????????????
    // Catches inflated PnL from stale carry-over positions opened in a prior session
    // that close during the current session after repeated reconnects.
    //
    // The cap is based on max realistic single-trade PnL for each instrument:
    //   XAUUSD: max lot=0.30, max move=150pts (3x ATR on a crash day) -> $4500 raw
    //           but 0.10 lot * 150pts * 100 = $1500. Cap at $1500 for safety.
    //   XAGUSD: max lot=0.10, max move=500pts -> $500. Cap at $600.
    //   US500.F/USTEC.F: max lot=0.10, max move=200pts -> $200. Cap at $300.
    //   Others: cap at $500 gross.
    //
    // If gross |pnl| exceeds cap BEFORE tick_val multiply (raw price pts * size),
    // this fires before scaling. We check the post-scale value after mult is applied.
    // We do NOT block -- we log loudly and let it through so CSV audit is preserved.
    // The LEDGER dedup guard (OmegaTradeLedger::record) is the hard block.
    //
    // Threshold is deliberately generous (3x normal max) to only catch true anomalies.
    {
        // Compute expected max gross USD for this symbol+size combination
        const double sz = tr.size;
        const std::string& sym = tr.symbol;
        double max_gross_usd = 500.0;  // default
        if      (sym == "XAUUSD")  max_gross_usd = std::max(1500.0, sz * 150.0 * 100.0);
        else if (sym == "XAGUSD")  max_gross_usd = std::max( 600.0, sz * 500.0 *   5.0);
        else if (sym == "US500.F") max_gross_usd = std::max( 300.0, sz * 200.0 *  50.0);
        else if (sym == "USTEC.F") max_gross_usd = std::max( 300.0, sz * 300.0 *  20.0);
        else if (sym == "DJ30.F")  max_gross_usd = std::max( 300.0, sz * 500.0 *   5.0);
        // raw pnl (pre-tick-mult) sanity: pnl = move_pts * size
        // For XAUUSD: move_pts * 0.10 lots, so if move > 150pts raw pnl > 15 (pre-mult)
        const double raw_pnl_abs = std::fabs(tr.pnl);
        const double mult_preview = tick_value_multiplier(sym);
        const double gross_usd_preview = raw_pnl_abs * mult_preview;
        if (gross_usd_preview > max_gross_usd) {
            printf("[PNL-SANITY] WARN %s %s gross_usd=%.2f exceeds cap=%.2f "
                   "raw_pnl=%.4f size=%.4f entryTs=%lld reason=%s -- "
                   "possible stale session carry-over. Ledger dedup will block if replay.\n",
                   sym.c_str(), tr.engine.c_str(), gross_usd_preview, max_gross_usd,
                   tr.pnl, sz, (long long)tr.entryTs, tr.exitReason.c_str());
            fflush(stdout);
        }
    }

    // ?? Indices FORCE_CLOSE circuit breaker -- stamp cooldown on disconnect ?????
    // When a US index position is FORCE_CLOSEd (disconnect/reconnect), stamp a 30-min
    // cooldown to prevent immediate re-entry into the same losing conditions.
    // Only fires on FORCE_CLOSE -- not SL_HIT, TRAIL_HIT, or other normal exits.
    // SHADOW mode included: we want to see the cooldown fire in shadow logs too
    // so we can verify it works before going live.
    if (tr_in.exitReason == "FORCE_CLOSE") {
        const std::string& fc_sym = tr_in.symbol;
        const bool is_us_index = (fc_sym == "US500.F" || fc_sym == "USTEC.F" ||
                                  fc_sym == "DJ30.F"  || fc_sym == "NAS100");
        if (is_us_index) {
            const int64_t block_until = static_cast<int64_t>(std::time(nullptr))
                                       + INDICES_DISCONNECT_COOLDOWN_SEC;
            // Only extend -- never shorten an existing cooldown
            int64_t prev = g_indices_disconnect_until.load();
            while (block_until > prev) {
                if (g_indices_disconnect_until.compare_exchange_weak(prev, block_until)) break;
            }
            printf("[INDICES-CB] FORCE_CLOSE on %s -- blocking US index new entries for 30min until %lld\n",
                   fc_sym.c_str(), (long long)block_until);
            fflush(stdout);
        }
    }

    // ?? PARTIAL_1R fast path ??????????????????????????????????????????????????
    // A PARTIAL_1R record means 50% of the position was closed at 1R profit.
    // The trade is NOT fully closed -- the remaining half is still live.
    // MUST NOT update: ledger, P&L, consecutive loss counter, fast-loss streak,
    // perf tracker, adaptive risk, equity, TOD gate, or partial_exit.reset().
    // All of those assume a fully closed trade and corrupt state if called here.
    // Only log the partial close for audit/CSV purposes.
    if (tr.exitReason == "PARTIAL_1R") {
        const double mult = tick_value_multiplier(tr.symbol);
        tr.pnl *= mult; tr.mfe *= mult; tr.mae *= mult;
        double cps = 0.0;
        { const std::string& s = tr.symbol;
          if (s=="EURUSD"||s=="GBPUSD"||s=="AUDUSD"||s=="NZDUSD"||
              s=="USDJPY"||s=="XAUUSD"||s=="XAGUSD") cps = 3.0; }
        omega::apply_realistic_costs(tr, cps, mult);
        std::cout << "[PARTIAL-CLOSE] " << tr.symbol
                  << " gross=$" << std::fixed << std::setprecision(2) << tr.pnl
                  << " net=$"   << tr.net_pnl
                  << " size="   << tr.size
                  << " @ "      << tr.exitPrice << "\n";
        std::cout.flush();
        write_trade_close_logs(tr);   // CSV audit trail only -- no risk state change
        // Add to ledger so GUI shows partial close and bell fires correctly (win sound)
        g_omegaLedger.record(tr);
        g_telemetry.AccumEnginePnl(tr.engine.c_str(), tr.net_pnl);
        return;
    }

    // Step 1: Scale raw price-point P&L to USD using per-instrument contract size.
    // This MUST happen before apply_realistic_costs so slippage is computed in USD.
    // Previously apply_realistic_costs ran first with raw price values, causing
    // slippage to be ~1000? too small (e.g. $0.002 instead of $8.10 for XAGUSD).
    {
        const double mult = tick_value_multiplier(tr.symbol);
        tr.pnl  *= mult;   // gross USD
        tr.mfe  *= mult;   // max favourable excursion USD
        tr.mae  *= mult;   // max adverse excursion USD
        // net_pnl, slippage_entry/exit, commission set below by apply_realistic_costs

        // Step 2: Apply realistic shadow costs (slippage + commission) in USD.
        // Commission: FX and metals carry $6/lot round-trip on BlackBull ECN.
        // Indices and oil are commission-free (cost embedded in spread).
        // Per-side = $3.0 for FX/metals, $0 for indices/oil.
        double comm_per_side = 0.0;
        {
            const std::string& s = tr.symbol;
            if (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" ||
                s == "NZDUSD" || s == "USDJPY" ||
                s == "XAUUSD" || s == "XAGUSD")
                comm_per_side = 3.0;  // $3/side = $6 round-trip per lot
        }
        omega::apply_realistic_costs(tr, comm_per_side, mult);
    }

    // Step 3: Log -- all values are now in USD
    std::cout << "[TRADE-COST] " << tr.symbol
              << " gross=$" << std::fixed << std::setprecision(2) << tr.pnl
              << " slip_in=$" << tr.slippage_entry
              << " slip_out=$" << tr.slippage_exit
              << " net=$" << tr.net_pnl
              << " exit=" << tr.exitReason << "\n";
    std::cout.flush();
    // ?? Decrement portfolio open SL risk on close ????????????????????????????????????
    // sl_abs is not directly in TradeRecord -- approximate from entryPrice and SL price.
    // tr.sl is the SL price stored at entry; not always populated in all engines.
    // Use a conservative fixed fallback per symbol if sl is zero.
    {
        const double sl_pts = (tr.sl > 0.0 && tr.entryPrice > 0.0)
            ? std::fabs(tr.entryPrice - tr.sl)
            : 0.0;
        const double tick_val = tick_value_multiplier(tr.symbol);
        portfolio_sl_risk_sub(sl_pts, tr.size, tick_val);
    }

    g_omegaLedger.record(tr);
    // Accumulate per-engine session P&L for GUI live attribution panel
    g_telemetry.AccumEnginePnl(tr.engine.c_str(), tr.net_pnl);
    // Shadow CSV only written in SHADOW mode -- prevents LIVE trades contaminating shadow analysis
    if (g_cfg.mode == "SHADOW") write_shadow_csv(tr);
    write_trade_close_logs(tr);

    // ?? Crowding guard -- update directional window on every close (RenTec #4) ????????
    // Tracks last 10 trades per symbol to detect directional crowding.
    // Penalty is applied at entry scoring time, not here.
    g_crowding_guard.update(tr.symbol, tr.side == "LONG");

    // ?? Walk-forward OOS validation -- update per-symbol pnl history (RenTec #6) ?????
    // Rebuilds pnl vector from ledger snapshot and re-runs 5-fold WFO every 20 trades.
    // Scale result is read by AdaptiveRiskManager::adjusted_lot() step 11.
    {
        const auto trades = g_omegaLedger.snapshot();
        std::vector<double> sym_pnl;
        sym_pnl.reserve(trades.size());
        for (const auto& t : trades) {
            if (t.symbol == tr.symbol &&
                t.exitReason != "PARTIAL_1R" && t.exitReason != "PARTIAL_2R")
                sym_pnl.push_back(t.net_pnl != 0.0 ? t.net_pnl : t.pnl);
        }
        g_walk_forward.update(tr.symbol, sym_pnl);
    }

    // ?? Adaptive parameter gate -- update score threshold from rolling edge (RenTec #7) ??
    // Reads win_rate and expectancy from g_adaptive_risk.perf for this symbol.
    // Re-evaluates every 10 trades; applies hysteresis so threshold moves ±1 per step.
    {
        auto it = g_adaptive_risk.perf.find(tr.symbol);
        if (it != g_adaptive_risk.perf.end()) {
            const auto& perf = it->second;
            g_param_gate.update(tr.symbol,
                                perf.win_rate(20),
                                perf.expectancy(20),
                                perf.trade_count());
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_hourly_pnl_mtx);
        g_hourly_pnl_records.push_back({nowSec(), tr.net_pnl});
        // Prune records older than 2h
        const int64_t cutoff = nowSec() - HOURLY_WINDOW_SEC;
        while (!g_hourly_pnl_records.empty() && g_hourly_pnl_records.front().ts_sec < cutoff)
            g_hourly_pnl_records.pop_front();
    }

    // ?? Time-of-day gate -- record outcome per 30-min bucket ??????????????????
    g_edges.tod.record(tr.symbol, tr.engine, tr.entryTs, tr.net_pnl);

    const std::string perf_key = perf_key_from_trade(tr);
    {
        const std::string risk_key = g_cfg.independent_symbols ? tr.symbol : "GLOBAL";
        std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
        auto& st = g_sym_risk[risk_key];
        st.daily_pnl += tr.net_pnl;   // track net (after costs) for stats only
        if (tr.net_pnl <= 0.0) {
            const int loss_limit = g_cfg.max_consec_losses;
            const int pause_sec  = g_cfg.loss_pause_sec;
            if (++st.consec_losses >= loss_limit) {
                st.pause_until = nowSec() + pause_sec;
                g_engine_pause[risk_key] = st.pause_until;  // expose to health watchdog
                std::cout << "[OMEGA-RISK] " << risk_key << " "
                          << loss_limit << " consecutive losses -- pause "
                          << pause_sec << "s\n";
            }
        } else {
            st.consec_losses = 0;
        }
        // Fast-loss streak gate: if 3+ consecutive fast bad losses (SL/scratch/timeout
        // within 120s), apply the same loss_pause_sec cooldown as the main consec-loss gate.
        // Applies in all modes -- shadow is a simulation.
        {
            auto& qs = g_shadow_quality[tr.symbol];
            const int64_t held = std::max<int64_t>(0, tr.exitTs - tr.entryTs);
            const bool fast_bad_loss =
                (tr.net_pnl <= 0.0) &&
                (held <= 120) &&
                (tr.exitReason == "SL_HIT" || tr.exitReason == "SCRATCH" || tr.exitReason == "TIMEOUT");
            if (fast_bad_loss) {
                ++qs.fast_loss_streak;
                if (qs.fast_loss_streak >= 3) {
                    qs.pause_until = nowSec() + g_cfg.loss_pause_sec;
                    std::cout << "[OMEGA-QUALITY] " << tr.symbol
                              << " fast-loss streak=" << qs.fast_loss_streak
                              << " -- pausing " << g_cfg.loss_pause_sec << "s\n";
                } else {
                    std::cout << "[OMEGA-QUALITY] " << tr.symbol
                              << " fast-loss streak=" << qs.fast_loss_streak << "/3\n";
                }
            } else if (tr.net_pnl > 0.0) {
                qs.fast_loss_streak = 0;
            } else if (qs.fast_loss_streak > 0) {
                --qs.fast_loss_streak;
            }
        }
        // Per-engine consecutive SL culling -- 4 SL_HIT in a row disables engine for session
        if (tr.exitReason == "SL_HIT") {
            const std::string eng_key = tr.symbol + ":" + tr.engine;
            auto& eq = g_shadow_quality[eng_key];
            if (!eq.engine_culled) {
                if (++eq.engine_consec_sl >= 4) {
                    eq.engine_culled = true;
                    // Mirror to cross-thread atomic so quote thread gate can read it safely
                    if (eng_key == "XAUUSD:GoldFlowEngine")
                        g_gf_engine_culled.store(true, std::memory_order_relaxed);
                    std::cout << "\033[1;31m[ENGINE-CULLED] " << eng_key
                              << " -- 4 consecutive SL hits. Disabled until midnight.\033[0m\n";
                    std::cout.flush();
                } else {
                    printf("[ENGINE-SL-STREAK] %s consec_sl=%d/4\n",
                           eng_key.c_str(), eq.engine_consec_sl);
                }
            }
        } else if (tr.net_pnl > 0.0 && tr.exitReason != "FORCE_CLOSE") {
            const std::string eng_key = tr.symbol + ":" + tr.engine;
            g_shadow_quality[eng_key].engine_consec_sl = 0;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_perf_mtx);
        auto& ps = g_perf[perf_key];
        ps.live_trades++;
        ps.live_pnl += tr.net_pnl;   // net pnl for performance tracking
        if (tr.net_pnl > 0) ps.live_wins++; else ps.live_losses++;
        if (!ps.disabled &&
            ps.live_trades >= g_cfg.auto_disable_after_trades &&
            ps.live_pnl < 0.0) {
            ps.disabled = true;
            if (perf_key == "GOLD_STACK") g_disable_gold_stack = true;
            std::cout << "[OMEGA-AUTO-DISABLE] " << perf_key
                      << " live_trades=" << ps.live_trades
                      << " pnl=" << ps.live_pnl << "\n";
        }
    }
    g_telemetry.UpdateStats(
        g_omegaLedger.dailyPnl() + (g_open_unrealised_cents.load() / 100.0),
        g_omegaLedger.grossDailyPnl() + (g_open_unrealised_cents.load() / 100.0),
        g_omegaLedger.maxDD(),
        g_omegaLedger.total(), g_omegaLedger.wins(), g_omegaLedger.losses(),
        g_omegaLedger.winRate(), g_omegaLedger.avgWin(), g_omegaLedger.avgLoss(), 0, 0,
        g_omegaLedger.dailyPnl(),                     // closed only
        g_open_unrealised_cents.load() / 100.0);      // floating only
    // NOTE: do NOT call UpdateLastSignal("CLOSED") here -- the GUI treats "CLOSED"
    // identically to "NONE" and resets the last-signal panel to "Waiting for first signal..."
    // on every single trade close. The last_signal panel should show the last ENTRY signal,
    // not be blanked by every exit.

    // Track false breaks per symbol -- supervisor uses this for CHOP_REVERSAL detection
    {
        std::lock_guard<std::mutex> lk(g_false_break_mtx);
        auto& fb = g_false_break_counts[tr.symbol];
        if (tr.exitReason == "BREAKOUT_FAIL" || tr.exitReason == "SL_HIT") {
            ++fb;
        } else if (tr.net_pnl > 0.0 && fb > 0) {
            --fb;
        }
    }

    // Notify supervisor of winning trade -- decays consecutive-block counter
    if (tr.net_pnl > 0.0) {
        auto notify = [&](omega::SymbolSupervisor& sup, const std::string& s) {
            if (tr.symbol == s) sup.on_trade_success();
        };
        notify(g_sup_sp,     "US500.F"); notify(g_sup_nq,     "USTEC.F");
        notify(g_sup_cl,     "USOIL.F"); notify(g_sup_us30,   "DJ30.F");
        notify(g_sup_nas100, "NAS100");  notify(g_sup_ger30,   "GER40");
        notify(g_sup_uk100,  "UK100");   notify(g_sup_estx50,  "ESTX50");
        notify(g_sup_xag,    "XAGUSD");  notify(g_sup_gold,    "XAUUSD");
        notify(g_sup_eurusd, "EURUSD");  notify(g_sup_gbpusd,  "GBPUSD");
        notify(g_sup_audusd, "AUDUSD");  notify(g_sup_nzdusd,  "NZDUSD");
        notify(g_sup_usdjpy, "USDJPY");  notify(g_sup_brent,   "BRENT");
    }

    // Update live equity in both LIVE and SHADOW modes.
    // LIVE: real money compounds. SHADOW: paper PnL compounding makes shadow results
    // comparable to what live would produce -- without this, shadow always sizes from
    // initial $10k even after profitable paper sessions.
    {
        const double updated_equity = g_cfg.account_equity + g_omegaLedger.cumulativePnl();
        const double eq = std::max(updated_equity, 100.0);
        g_live_equity.store(eq, std::memory_order_relaxed);
        g_gold_flow.risk_dollars        = eq * (g_cfg.risk_per_trade_usd / std::max(g_cfg.account_equity, 100.0));
        g_gold_flow_reload.risk_dollars = g_gold_flow.risk_dollars;  // keep reload instance in sync -- was missing, causing undersized reload trades
    }
    if (g_cfg.mode == "LIVE") {
        const double updated_equity = g_cfg.account_equity + g_omegaLedger.cumulativePnl();
        const double eq = std::max(updated_equity, 100.0);
        g_eng_sp.ACCOUNT_EQUITY     = eq;
        g_eng_nq.ACCOUNT_EQUITY     = eq;
        g_eng_cl.ACCOUNT_EQUITY     = eq;
        g_eng_us30.ACCOUNT_EQUITY   = eq;
        g_eng_nas100.ACCOUNT_EQUITY = eq;
        g_eng_ger30.ACCOUNT_EQUITY  = eq;
        g_eng_uk100.ACCOUNT_EQUITY  = eq;
        g_eng_estx50.ACCOUNT_EQUITY = eq;
        g_eng_xag.ACCOUNT_EQUITY    = eq;
        g_eng_eurusd.ACCOUNT_EQUITY = eq;
        g_eng_gbpusd.ACCOUNT_EQUITY = eq;
        g_eng_audusd.ACCOUNT_EQUITY = eq;
        g_eng_nzdusd.ACCOUNT_EQUITY = eq;
        g_eng_usdjpy.ACCOUNT_EQUITY = eq;
        g_eng_brent.ACCOUNT_EQUITY  = eq;
    }

    // ?? Feed closed trade into adaptive risk tracker ??????????????????????????
    // FORCE_CLOSE exits excluded from Kelly/Sharpe -- they reflect restart timing
    // not trade outcome. Only SL_HIT, TP_HIT, TRAIL_HIT, BE_HIT, TIMEOUT, SCRATCH counted.
    {
        const double hold_sec = static_cast<double>(
            tr.exitTs > tr.entryTs ? tr.exitTs - tr.entryTs : 1);
        const bool is_clean_exit = (tr.exitReason != "FORCE_CLOSE" &&
                                    tr.exitReason != "SHUTDOWN"    &&
                                    tr.exitReason != "DISCONNECT");
        if (is_clean_exit) {
            g_adaptive_risk.record_trade(tr.symbol, tr.net_pnl, hold_sec);
        } else {
            printf("[PERF-SKIP] %s %s exit=%s pnl=%.2f -- excluded from Kelly/Sharpe\n",
                   tr.symbol.c_str(), tr.engine.c_str(),
                   tr.exitReason.c_str(), tr.net_pnl);
            fflush(stdout);
        }
        // Drawdown velocity always tracked -- forced closes are real losses
        g_adaptive_risk.dd_velocity.record_trade(nowSec(), tr.net_pnl);
    }

    // ?? Reset partial exit state when broker closes position ?????????????????
    // CRTP engine positions are closed by broker SL/TP -- partial exit must be
    // reset here so the next entry can re-arm cleanly.
    g_partial_exit.reset(tr.symbol);
}

// ?????????????????????????????????????????????????????????????????????????????
// Signal handler
// ?????????????????????????????????????????????????????????????????????????????
static void sig_handler(int) noexcept { g_running.store(false); }

// Windows console close handler -- handles X button, CTRL+C, CTRL+BREAK, logoff,
// shutdown. std::signal(SIGINT) is NOT called when the console window is closed
// or the process is terminated by Windows -- this handler covers all those paths.
// Without it, g_running stays true ? no FIX Logout sent ? ghost session left on
// server ? next connect gets ALREADY_SUBSCRIBED.
static BOOL WINAPI console_ctrl_handler(DWORD event) noexcept {
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_running.store(false);
            // Wait for main() to finish cleanup (logout sent, files closed).
            // Worst case: SO_RCVTIMEO(200ms) + quote teardown(~300ms) +
            //             trade teardown(~300ms) + join wait(up to 1.5s) = ~2.3s.
            // We wait up to 4s to cover this comfortably.
            // If still not done after 4s, return FALSE -- this lets Windows apply
            // the default handler (process termination) rather than suppressing
            // the signal and leaving a zombie process. The user has to press
            // Ctrl+C multiple times if we return TRUE and haven't finished.
            for (int i = 0; i < 40 && !g_shutdown_done.load(); ++i) Sleep(100);
            // Return FALSE so Windows terminates us if g_shutdown_done never set.
            // Return TRUE only if we completed cleanly -- prevents Windows from
            // also running the default kill on top of our clean exit.
            return g_shutdown_done.load() ? TRUE : FALSE;
        default:
            return FALSE;
    }
}

// ?????????????????????????????????????????????????????????????????????????????
// Tick handler -- called for every bid/ask update
// ?????????????????????????????????????????????????????????????????????????????
// ── symbol_risk_blocked ──────────────────────────────────────────────
// ── symbol_gate ──────────────────────────────────────────────────────
// tradeable/lat_ok/regime/bid/ask passed explicitly from on_tick.
// ── open_unrealised_pnl ──────────────────────────────────────────────────────
static double open_unrealised_pnl() {
    double total = 0.0;

    // Snapshot all mid prices in ONE lock acquisition to avoid 30+ per-engine
    // g_book_mtx acquisitions at high tick rates (was lock contention risk).
    std::unordered_map<std::string, double> mids;
    {
        std::lock_guard<std::mutex> lk(g_book_mtx);
        for (const auto& kv : g_bids) {
            auto ai = g_asks.find(kv.first);
            if (ai != g_asks.end() && kv.second > 0 && ai->second > 0)
                mids[kv.first] = (kv.second + ai->second) * 0.5;
        }
    }

    // Helper: unrealised PnL -- generic lambda accepts any OpenPos-like type
    auto be_pnl = [&](const auto& p, const std::string& sym) -> double {
        if (!p.active) return 0.0;
        auto it = mids.find(sym);
        if (it == mids.end() || it->second <= 0.0) return 0.0;
        const double move = p.is_long ? (it->second - p.entry) : (p.entry - it->second);
        return move * p.size * tick_value_multiplier(sym);
    };

    // Helper: unrealised PnL for a CrossPosition (uses same snapshot)
    auto cp_pnl = [&](bool active, bool is_long, double entry, double size,
                      const std::string& sym) -> double {
        if (!active || entry <= 0.0) return 0.0;
        auto it = mids.find(sym);
        if (it == mids.end() || it->second <= 0.0) return 0.0;
        const double move = is_long ? (it->second - entry) : (entry - it->second);
        return move * size * tick_value_multiplier(sym);
    };

    // Breakout engines
    total += be_pnl(g_eng_sp.pos,     "US500.F");
    total += be_pnl(g_eng_nq.pos,     "USTEC.F");
    total += be_pnl(g_eng_cl.pos,     "USOIL.F");
    total += be_pnl(g_eng_us30.pos,   "DJ30.F");
    total += be_pnl(g_eng_nas100.pos, "NAS100");
    total += be_pnl(g_eng_ger30.pos,  "GER40");
    total += be_pnl(g_eng_uk100.pos,  "UK100");
    total += be_pnl(g_eng_estx50.pos, "ESTX50");
    total += be_pnl(g_eng_xag.pos,    "XAGUSD");
    total += be_pnl(g_eng_eurusd.pos, "EURUSD");
    total += be_pnl(g_eng_gbpusd.pos, "GBPUSD");
    total += be_pnl(g_eng_audusd.pos, "AUDUSD");
    total += be_pnl(g_eng_nzdusd.pos, "NZDUSD");
    total += be_pnl(g_eng_usdjpy.pos, "USDJPY");
    total += be_pnl(g_eng_brent.pos,  "BRENT");

    // Bracket engines -- pos.entry/size/is_long same struct
    total += be_pnl(g_bracket_sp.pos,     "US500.F");
    total += be_pnl(g_bracket_nq.pos,     "USTEC.F");
    total += be_pnl(g_bracket_us30.pos,   "DJ30.F");
    total += be_pnl(g_bracket_nas100.pos, "NAS100");
    total += be_pnl(g_bracket_ger30.pos,  "GER40");
    total += be_pnl(g_bracket_uk100.pos,  "UK100");
    total += be_pnl(g_bracket_estx50.pos, "ESTX50");
    total += be_pnl(g_bracket_xag.pos,    "XAGUSD");
    total += be_pnl(g_bracket_gold.pos,   "XAUUSD");
    total += be_pnl(g_bracket_brent.pos,  "BRENT");
    total += be_pnl(g_bracket_eurusd.pos, "EURUSD");
    total += be_pnl(g_bracket_gbpusd.pos, "GBPUSD");
    total += be_pnl(g_bracket_audusd.pos, "AUDUSD");
    total += be_pnl(g_bracket_nzdusd.pos, "NZDUSD");
    total += be_pnl(g_bracket_usdjpy.pos, "USDJPY");

    // Cross-asset engines -- use public accessors (pos_ is private)
    // open_unrealised() calls the engine's open_pos_pnl(bid, ask) method.
    // If an engine has a position, mid-price PnL is estimated from the book.
    auto ca_pnl = [&](bool has_pos, double open_entry, bool open_long,
                      double open_size, const std::string& sym) -> double {
        if (!has_pos || open_entry <= 0.0) return 0.0;
        return cp_pnl(true, open_long, open_entry, open_size, sym);
    };
    total += ca_pnl(g_ca_esnq.has_open_position(),
                    g_ca_esnq.open_entry(), g_ca_esnq.open_is_long(), g_ca_esnq.open_size(), "US500.F");
    total += ca_pnl(g_ca_eia_fade.has_open_position(),
                    g_ca_eia_fade.open_entry(), g_ca_eia_fade.open_is_long(), g_ca_eia_fade.open_size(), "USOIL.F");
    total += ca_pnl(g_ca_brent_wti.has_open_position(),
                    g_ca_brent_wti.open_entry(), g_ca_brent_wti.open_is_long(), g_ca_brent_wti.open_size(), "BRENT");
    total += ca_pnl(g_ca_fx_cascade.has_open_position(),
                    g_ca_fx_cascade.open_entry(), g_ca_fx_cascade.open_is_long(), g_ca_fx_cascade.open_size(), "GBPUSD");
    total += ca_pnl(g_ca_carry_unwind.has_open_position(),
                    g_ca_carry_unwind.open_entry(), g_ca_carry_unwind.open_is_long(), g_ca_carry_unwind.open_size(), "USDJPY");

    // Gold flow -- covered by mids map above (XAUUSD included in g_bids loop)
    if (g_gold_flow.pos.active) {
        auto it = mids.find("XAUUSD");
        if (it != mids.end() && it->second > 0.0) {
            const double move = g_gold_flow.pos.is_long
                ? (it->second - g_gold_flow.pos.entry)
                : (g_gold_flow.pos.entry - it->second);
            total += move * g_gold_flow.pos.size * 100.0;  // XAUUSD tick mult
        }
    }
    return total;
}

static bool symbol_risk_blocked(const std::string& symbol) {
    // SHADOW MODE: bypass ALL daily loss limits and consecutive loss pauses.
    // Testing must never be blocked by risk caps -- no real money at risk.
    // The only gates that apply in shadow are session/spread/latency (structural).
    if (g_cfg.mode == "SHADOW") return false;

    // LIVE MODE only: enforce all risk gates below.
    std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
    auto& st = g_sym_risk[symbol];
    if (st.daily_pnl < -g_cfg.daily_loss_limit) {
        ++g_gov_pnl;
        return true;
    }
    // Also block when closed P&L + floating unrealised already breaches limit.
    // Prevents opening new positions into a limit breach before any close fires.
    {
        const double closed_pnl = g_omegaLedger.dailyPnl();
        const double unrealised  = open_unrealised_pnl();
        if (closed_pnl + unrealised < -g_cfg.daily_loss_limit) {
            static thread_local int64_t s_unr_log = 0;
            if (nowSec() - s_unr_log > 30) {
                s_unr_log = nowSec();
                printf("[OMEGA-RISK] Unrealised loss gate: closed=$%.2f unreal=$%.2f total=$%.2f limit=$%.0f\n",
                       closed_pnl, unrealised, closed_pnl + unrealised, g_cfg.daily_loss_limit);
            }
            ++g_gov_pnl;
            return true;
        }
    }
    const int64_t now = nowSec();
    if (st.pause_until > now) {
        ++g_gov_consec;
        return true;
    }
    if (st.pause_until != 0 && st.pause_until <= now) {
        st.pause_until = 0;
        st.consec_losses = 0;
        std::cout << "[OMEGA-RISK] " << symbol << " loss pause cleared\n";
    }
    return false;
}

static bool symbol_gate(
    const std::string& symbol,
    bool symbol_has_open_position,
    const std::string& engine_name,
    bool tradeable,
    bool lat_ok,
    const std::string& regime,
    double bid,
    double ask)
{
    const bool shadow_mode = (g_cfg.mode == "SHADOW");
    if (symbol == "XAUUSD" && g_disable_gold_stack) return false;
    // Connection stability gate: block new entries for N seconds after reconnect.
    // Prevents opening positions on the first tick after logon when the FIX session
    // may be unstable -- avoids the open?immediate-FORCE_CLOSE pattern seen in logs.
    {
        const int64_t cs = g_connected_since.load();
        if (cs > 0 && (nowSec() - cs) < g_cfg.connection_warmup_sec) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_perf_mtx);
        auto it = g_perf.find(symbol);
        if (it != g_perf.end() && it->second.disabled) return false;
    }
    if (!tradeable) return false;  // session gate applies in all modes -- shadow is a simulation
    if (!shadow_mode && !lat_ok) return false;  // latency gate: LIVE only (VPS can't simulate RTT)
    if (shadow_mode) {
        // Optional shadow pilot mode: keep GOLD stack live and run USTEC pilot only.
        if (g_cfg.shadow_ustec_pilot_only &&
            symbol != "XAUUSD" && symbol != "USTEC.F") {
            return false;
        }
        if (g_cfg.shadow_ustec_pilot_only && symbol == "USTEC.F") {
            if (g_cfg.ustec_pilot_require_session && !tradeable) return false;
            if (g_cfg.ustec_pilot_require_latency && !lat_ok) return false;
            if (g_cfg.ustec_pilot_block_risk_off && regime == "RISK_OFF") return false;
        }
        // SHADOW pilot mode filters
    }
    // ?? Fast-loss streak gate -- ALL modes (LIVE + SHADOW) ????????????????
    // BUG FIX: was inside if(shadow_mode) block -- never fired in LIVE mode.
    // Comment said "applies in all modes" but code contradicted it.
    // Evidence: 01:57 SHORT SL_HIT + 01:58 SHORT SL_HIT both allowed through
    // in LIVE -- fast_loss_streak never reached 3 because gate never checked.
    // Fix: moved outside shadow_mode block. Now fires in both modes.
    // 3 consecutive fast bad losses (SL/scratch/timeout <=120s) triggers
    // loss_pause_sec cooldown on that symbol.
    {
        std::lock_guard<std::mutex> lk2(g_sym_risk_mtx);
        auto it = g_shadow_quality.find(symbol);
        if (it != g_shadow_quality.end() && it->second.pause_until > nowSec()) {
            static std::unordered_map<std::string,int64_t> s_fls_log;
            const int64_t now_fls = nowSec();
            if (now_fls - s_fls_log[symbol] >= 30) {
                s_fls_log[symbol] = now_fls;
                printf("[FAST-LOSS-BLOCK] %s fast-loss pause active -- blocking entry\n",
                       symbol.c_str());
                fflush(stdout);
            }
            return false;
        }
    }
    if (symbol_has_open_position) {
        // Do NOT increment g_gov_pos here -- this fires every tick while any
        // position is open and would make the counter meaningless (21k+/day).
        // g_gov_pos is reserved for position CAP blocks -- meaningful signal.
        return false;
    }
    if (g_cfg.independent_symbols) {
        // Per-symbol risk is independent, but enforce a global open-position cap.
        // Without this, all 12 symbols could open simultaneously ignoring max_positions.
        const int open_positions =
            static_cast<int>(g_eng_sp.pos.active) +
            static_cast<int>(g_eng_nq.pos.active) +
            static_cast<int>(g_eng_cl.pos.active) +
            static_cast<int>(g_eng_us30.pos.active) +
            static_cast<int>(g_eng_nas100.pos.active) +
            static_cast<int>(g_eng_ger30.pos.active) +
            static_cast<int>(g_eng_uk100.pos.active) +
            static_cast<int>(g_eng_estx50.pos.active) +
            static_cast<int>(g_eng_xag.pos.active) +
            static_cast<int>(g_bracket_xag.pos.active) +
            static_cast<int>(g_bracket_gold.pos.active) +
            static_cast<int>(g_bracket_sp.pos.active) +
            static_cast<int>(g_bracket_nq.pos.active) +
            static_cast<int>(g_bracket_us30.pos.active) +
            static_cast<int>(g_bracket_nas100.pos.active) +
            static_cast<int>(g_bracket_ger30.pos.active) +
            static_cast<int>(g_bracket_uk100.pos.active) +
            static_cast<int>(g_bracket_estx50.pos.active) +
            static_cast<int>(g_bracket_brent.pos.active) +
            static_cast<int>(g_bracket_eurusd.pos.active) +
            static_cast<int>(g_bracket_gbpusd.pos.active) +
            static_cast<int>(g_bracket_audusd.pos.active) +
            static_cast<int>(g_bracket_nzdusd.pos.active) +
            static_cast<int>(g_bracket_usdjpy.pos.active) +
            static_cast<int>(g_ca_esnq.has_open_position()) +
            static_cast<int>(g_ca_eia_fade.has_open_position()) +
            static_cast<int>(g_ca_brent_wti.has_open_position()) +
            static_cast<int>(g_ca_fx_cascade.has_open_position()) +
            static_cast<int>(g_ca_carry_unwind.has_open_position()) +
            static_cast<int>(g_orb_us.has_open_position()) +
            static_cast<int>(g_orb_ger30.has_open_position()) +
            static_cast<int>(g_orb_silver.has_open_position()) +
            static_cast<int>(g_orb_uk100.has_open_position()) +
            static_cast<int>(g_orb_estx50.has_open_position()) +
            static_cast<int>(g_vwap_rev_sp.has_open_position()) +
            static_cast<int>(g_vwap_rev_nq.has_open_position()) +
            static_cast<int>(g_vwap_rev_ger40.has_open_position()) +
            static_cast<int>(g_vwap_rev_eurusd.has_open_position()) +
            static_cast<int>(g_trend_pb_gold.has_open_position()) +
            static_cast<int>(g_trend_pb_ger40.has_open_position()) +
            static_cast<int>(g_trend_pb_nq.has_open_position()) +    // TrendPB USTEC
            static_cast<int>(g_trend_pb_sp.has_open_position()) +    // TrendPB US500
            static_cast<int>(g_eng_eurusd.pos.active) +
            static_cast<int>(g_eng_gbpusd.pos.active) +
            static_cast<int>(g_eng_audusd.pos.active) +
            static_cast<int>(g_eng_nzdusd.pos.active) +
            static_cast<int>(g_eng_usdjpy.pos.active) +
            static_cast<int>(g_eng_brent.pos.active) +
            static_cast<int>(g_gold_stack.has_open_position()) +
            static_cast<int>(g_le_stack.has_open_position()) +
            static_cast<int>(g_gold_flow.has_open_position()) +        // gold flow
            static_cast<int>(g_gold_flow_reload.has_open_position()) + // reload instance
            static_cast<int>(g_nbm_sp.has_open_position()) +
            static_cast<int>(g_nbm_nq.has_open_position()) +
            static_cast<int>(g_nbm_nas.has_open_position()) +
            static_cast<int>(g_nbm_us30.has_open_position()) +
            static_cast<int>(g_nbm_gold_london.has_open_position()) +
            static_cast<int>(g_nbm_oil_london.has_open_position());
        // ?? Session-aware position cap ????????????????????????????????
        // Asia = max 2 (low liquidity, wide spreads, few signals worth taking)
        // Dead zone (05-07 UTC) = max 1 (preparation period, no fresh data)
        // London-NY overlap = full config cap (peak liquidity, all engines active)
        // All other sessions = config cap
        {
            int session_cap = g_cfg.max_open_positions;
            const int slot = g_macro_ctx.session_slot;
            // slot 0 never assigned (24h mode) -- no dead zone cap needed
            if (slot == 6) session_cap = std::min(session_cap, 2); // Asia 22-05 UTC
            // ?? Asia breakout quality gate ????????????????????????????????
            // Allow Asia trading but only when volatility is genuinely expanding
            // (real breakout move, not Asia chop). Two-path gate:
            //   PATH A (sustained): vol_ratio >= 2.0 -- recent 80-tick range is 2?
            //     the slow EWM baseline. Proves the move has been running long
            //     enough to fill the governor window. Works well once a move
            //     is underway but lags ~28 ticks (4-5 min) on move onset.
            //   PATH B (fast-onset): |ewm_drift| >= 1.5pts -- GoldEngineStack
            //     fast EWM (?=0.05) has separated from slow (?=0.005) by $1.50+.
            //     Fires ~7 ticks (70s) into a 1pt/tick surge, catching the move
            //     while the ratio gate is still blocked by an elevated baseline
            //     carried over from a big prior session (e.g. Friday's 211pt day
            //     inflates base_vol via ?=0.002, blocking all of Monday morning).
            //   Root cause of Monday 30 Mar 2026 zero trades: Asia session,
            //   Friday base_vol elevated at ~0.35%, Monday pre-surge vol ~0.04%.
            //   Ratio = 0.13 ? entire strong downtrend+surge blocked all session.
            //   EWM drift would have opened the gate 70s into each move.
            //   Fail-open if base_vol=0 (warmup): ratio = 3.0 ? PATH A passes.
            // ASIA-GATE REMOVED: vol_ratio/drift/RSI thresholds were blocking entries
            // even when candle structure and DOM clearly show a valid trade.
            // The candle IS the volatility signal. The DOM IS the confirmation.
            // Each engine has its own quality gates -- this outer vol gate was
            // double-blocking with a lagging proxy and is gone.
            // Asia session cap (max 2 positions) still applies above.
            // overlap (slot 3) and London/NY get full cap
            if (open_positions >= session_cap) {
                ++g_gov_pos;
                return false;
            }
        }
        // ?? Daily profit target ???????????????????????????????????????????
        // Stop new entries once daily P&L hits the target -- lock in the day.
        if (g_cfg.daily_profit_target > 0.0) {
            const double daily_pnl = g_omegaLedger.dailyPnl();
            if (daily_pnl >= g_cfg.daily_profit_target) {
                static int64_t s_last_profit_log = 0;
                if (nowSec() - s_last_profit_log > 60) {
                    s_last_profit_log = nowSec();
                    printf("[OMEGA-RISK] Daily profit target $%.0f reached (P&L=$%.2f) -- no new entries\n",
                           g_cfg.daily_profit_target, daily_pnl);
                }
                return false;
            }
        }
        // ?? Portfolio open SL risk cap ????????????????????????????????????????
        // Block new entries when total simultaneous SL exposure exceeds threshold.
        // Prevents correlated crashes: gold+silver+oil all SHORT at once means
        // a single RISK_ON reversal hits all of them simultaneously.
        // Cap = max_portfolio_sl_risk_usd (default 0 = disabled).
        // Each engine adds its SL risk on entry via portfolio_sl_risk_add().
        if (g_cfg.max_portfolio_sl_risk_usd > 0.0) {
            const double open_sl_risk = g_open_sl_risk_cents.load(std::memory_order_relaxed) / 100.0;
            if (open_sl_risk >= g_cfg.max_portfolio_sl_risk_usd) {
                static int64_t s_ptf_log = 0;
                if (nowSec() - s_ptf_log > 30) {
                    s_ptf_log = nowSec();
                    printf("[PORTFOLIO-CAP] %s blocked: open_sl_risk=$%.2f >= cap=$%.0f\n",
                           symbol.c_str(), open_sl_risk, g_cfg.max_portfolio_sl_risk_usd);
                    fflush(stdout);
                }
                return false;
            }
        }

        // ?? Session watermark drawdown ????????????????????????????????????
        // Stop if drawdown from intra-day peak exceeds threshold AND daily P&L
        // is still negative (i.e. we are actually losing money today).
        //
        // Critical rule: if daily_pnl > 0 the day is profitable -- NEVER stop
        // trading because of a drawdown from peak. A $800 day that pulls back
        // $121 is still +$679. Stopping there is wrong. Only protect against
        // real capital loss: drawdown threshold fires only when daily_pnl <= 0.
        //
        // Threshold = watermark_pct * daily_loss_limit (always fixed reference).
        // e.g. 0.27 * $450 = $121. If daily_pnl goes negative by $121 ? stop.
        if (g_cfg.session_watermark_pct > 0.0) {
            const double peak_pnl   = g_omegaLedger.peakDailyPnl();
            const double daily_pnl  = g_omegaLedger.dailyPnl();
            const double drawdown   = peak_pnl - daily_pnl;
            const double reference  = g_cfg.daily_loss_limit;
            const double threshold  = reference * g_cfg.session_watermark_pct;
            // Only block if: (a) we've given back threshold from peak AND
            //                (b) the day is net negative -- still profitable = ride on
            const bool drawdown_hit = (peak_pnl > 0.0 && drawdown >= threshold);
            const bool day_negative = (daily_pnl <= 0.0);
            if (drawdown_hit && day_negative) {
                static int64_t s_last_wm_log = 0;
                if (nowSec() - s_last_wm_log > 60) {
                    s_last_wm_log = nowSec();
                    printf("[OMEGA-RISK] Watermark: peak=$%.2f current=$%.2f drawdown=$%.2f >= $%.2f AND day negative -- no new entries\n",
                           peak_pnl, daily_pnl, drawdown, threshold);
                }
                return false;
            }
        }
        // ?? Hourly loss throttle ??????????????????????????????????????????
        // Block new entries if rolling 2h net P&L loss exceeds hourly_loss_limit.
        if (g_cfg.hourly_loss_limit > 0.0) {
            double rolling_pnl = 0.0;
            {
                std::lock_guard<std::mutex> lk(g_hourly_pnl_mtx);
                const int64_t cutoff = nowSec() - HOURLY_WINDOW_SEC;
                for (const auto& r : g_hourly_pnl_records)
                    if (r.ts_sec >= cutoff) rolling_pnl += r.net_pnl;
            }
            if (rolling_pnl < -g_cfg.hourly_loss_limit) {
                static int64_t s_last_hourly_log = 0;
                if (nowSec() - s_last_hourly_log > 60) {
                    s_last_hourly_log = nowSec();
                    printf("[OMEGA-RISK] 2h rolling loss $%.2f exceeds limit $%.0f -- throttling entries\n",
                           rolling_pnl, g_cfg.hourly_loss_limit);
                }
                return false;
            }
        }
        // ?? Stale quote watchdog ??????????????????????????????????????????
        // Block entries if no tick received in last STALE_QUOTE_SEC seconds.
        // A frozen feed with open positions is the most dangerous possible state --
        // we have no pricing, but the broker is still live and SLs can gap.
        if (!stale_watchdog_ok(symbol)) {
            static thread_local int64_t s_stale_log = 0;
            if (nowSec() - s_stale_log > 10) {
                s_stale_log = nowSec();
                std::printf("[STALE-FEED] %s no tick in >%llds -- blocking entry\n",
                            symbol.c_str(), (long long)STALE_QUOTE_SEC);
            }
            return false;
        }
        // ?? Drawdown velocity circuit breaker ????????????????????????????
        // Fires when loss rate in rolling 30-min window exceeds threshold.
        // Threshold is configured as dd_velocity.threshold_usd (set in init
        // to 0.5 * daily_loss_limit). When active, halts new entries 15 min.
        if (!shadow_mode && !g_adaptive_risk.dd_velocity.new_entries_allowed(nowSec())) return false;  // shadow: no rate-of-loss block
        // ?? Portfolio VaR gate ????????????????????????????????????????????
        // REMOVED: portfolio_VaR -- redundant with per-trade max_loss, false blocks on correlated positions
        // REMOVED: VPIN -- BlackBull L2 volume unreliable, causes false blocks
        // REMOVED: TOD gate -- needs 30 trades/bucket to activate, currently zero data = pure false blocks
        // REMOVED: corr_heat -- with 1-2 open positions max never adds value, only blocks
        // ?? News blackout gate ????????????????????????????????????????????
        if (g_news_blackout.is_blocked(symbol, nowSec())) return false;
        // ?? Relative spread Z-score gate ??????????????????????????????????
        // Block if current spread is anomalous vs rolling 200-tick median.
        // Applies in all modes -- shadow is a simulation.
        // bid/ask passed directly from on_tick (cTrader source) -- no cache lookup needed.
        if (bid > 0.0 && ask > 0.0) {
            const double spread = ask - bid;
            if (!g_edges.spread_gate.ok(symbol, spread)) {
                static thread_local int64_t s_sz_log = 0;
                if (nowSec() - s_sz_log > 30) {
                    s_sz_log = nowSec();
                    printf("[SPREAD-Z] %s spread=%.5f z=%.2f -- anomalous, blocking entry\n",
                           symbol.c_str(), spread,
                           g_edges.spread_gate.z_score(symbol, spread));
                }
                return false;
            }
        }
        // ?? Regime block gate -- applied in all modes ??????????????????????
        if (g_regime_adaptor.equity_blocked(symbol)) return false;
        return !symbol_risk_blocked(symbol);
    }
    // Legacy global portfolio mode.
    if (symbol_risk_blocked("GLOBAL")) return false;
    const int open_positions =
        static_cast<int>(g_eng_sp.pos.active) +
        static_cast<int>(g_eng_nq.pos.active) +
        static_cast<int>(g_eng_cl.pos.active) +
        static_cast<int>(g_eng_us30.pos.active) +
        static_cast<int>(g_eng_nas100.pos.active) +
        static_cast<int>(g_eng_ger30.pos.active) +
        static_cast<int>(g_eng_uk100.pos.active) +
        static_cast<int>(g_eng_estx50.pos.active) +
        static_cast<int>(g_eng_xag.pos.active) +
        static_cast<int>(g_bracket_xag.pos.active) +
        static_cast<int>(g_bracket_gold.pos.active) +
        static_cast<int>(g_bracket_sp.pos.active) +
        static_cast<int>(g_bracket_nq.pos.active) +
        static_cast<int>(g_bracket_us30.pos.active) +
        static_cast<int>(g_bracket_nas100.pos.active) +
        static_cast<int>(g_bracket_ger30.pos.active) +
        static_cast<int>(g_bracket_uk100.pos.active) +
        static_cast<int>(g_bracket_estx50.pos.active) +
        static_cast<int>(g_bracket_brent.pos.active) +
        static_cast<int>(g_bracket_eurusd.pos.active) +
        static_cast<int>(g_bracket_gbpusd.pos.active) +
        static_cast<int>(g_bracket_audusd.pos.active) +
        static_cast<int>(g_bracket_nzdusd.pos.active) +
        static_cast<int>(g_bracket_usdjpy.pos.active) +
        static_cast<int>(g_ca_esnq.has_open_position()) +
        static_cast<int>(g_ca_eia_fade.has_open_position()) +
        static_cast<int>(g_ca_brent_wti.has_open_position()) +
        static_cast<int>(g_ca_fx_cascade.has_open_position()) +
        static_cast<int>(g_ca_carry_unwind.has_open_position()) +
        static_cast<int>(g_orb_us.has_open_position()) +
        static_cast<int>(g_orb_ger30.has_open_position()) +
        static_cast<int>(g_orb_silver.has_open_position()) +
        static_cast<int>(g_orb_uk100.has_open_position()) +
        static_cast<int>(g_orb_estx50.has_open_position()) +
        static_cast<int>(g_vwap_rev_sp.has_open_position()) +
        static_cast<int>(g_vwap_rev_nq.has_open_position()) +
        static_cast<int>(g_vwap_rev_ger40.has_open_position()) +
        static_cast<int>(g_vwap_rev_eurusd.has_open_position()) +
        static_cast<int>(g_trend_pb_gold.has_open_position()) +
        static_cast<int>(g_trend_pb_ger40.has_open_position()) +
        static_cast<int>(g_trend_pb_nq.has_open_position()) +    // TrendPB USTEC
        static_cast<int>(g_trend_pb_sp.has_open_position()) +    // TrendPB US500
        static_cast<int>(g_eng_eurusd.pos.active) +
        static_cast<int>(g_eng_gbpusd.pos.active) +
        static_cast<int>(g_eng_audusd.pos.active) +
        static_cast<int>(g_eng_nzdusd.pos.active) +
        static_cast<int>(g_eng_usdjpy.pos.active) +
        static_cast<int>(g_eng_brent.pos.active) +
        static_cast<int>(g_gold_stack.has_open_position()) +
        static_cast<int>(g_le_stack.has_open_position()) +
        static_cast<int>(g_gold_flow.has_open_position()) +        // gold flow
        static_cast<int>(g_gold_flow_reload.has_open_position()) + // reload instance
        static_cast<int>(g_nbm_sp.has_open_position()) +
        static_cast<int>(g_nbm_nq.has_open_position()) +
        static_cast<int>(g_nbm_nas.has_open_position()) +
        static_cast<int>(g_nbm_us30.has_open_position()) +
        static_cast<int>(g_nbm_gold_london.has_open_position()) +
        static_cast<int>(g_nbm_oil_london.has_open_position());
    // Session-aware cap (mirrors independent_symbols path)
    int session_cap2 = g_cfg.max_open_positions;
    const int slot2 = g_macro_ctx.session_slot;
    if      (slot2 == 0) session_cap2 = std::min(session_cap2, 1);
    else if (slot2 == 6) session_cap2 = std::min(session_cap2, 2);
    const bool pos_budget_ok = open_positions < session_cap2;
    if (!pos_budget_ok) ++g_gov_pos;
    if (!pos_budget_ok) return false;
    // ?? Daily profit target / session watermark / hourly throttle ?????????
    // SHADOW MODE: skip all PnL-based gates. Testing must never be blocked.
    // LIVE MODE: enforce all limits below.
    if (!shadow_mode) {
    if (g_cfg.daily_profit_target > 0.0 &&
        g_omegaLedger.dailyPnl() >= g_cfg.daily_profit_target) return false;
    if (g_cfg.session_watermark_pct > 0.0) {
        const double peak      = g_omegaLedger.peakDailyPnl();
        const double daily_pnl = g_omegaLedger.dailyPnl();
        const double drawdown  = peak - daily_pnl;
        const double threshold = g_cfg.daily_loss_limit * g_cfg.session_watermark_pct;
        // Only block when day is actually net negative -- never kill a profitable day
        if (peak > 0.0 && drawdown >= threshold && daily_pnl <= 0.0)
            return false;
    }
    if (g_cfg.hourly_loss_limit > 0.0) {
        double rolling = 0.0;
        { std::lock_guard<std::mutex> lk(g_hourly_pnl_mtx);
          for (const auto& r : g_hourly_pnl_records)
              if (r.ts_sec >= nowSec() - HOURLY_WINDOW_SEC) rolling += r.net_pnl; }
        if (rolling < -g_cfg.hourly_loss_limit) return false;
    }
    } // end !shadow_mode PnL gates
    // ?? News blackout gate ????????????????????????????????????????????????
    if (g_news_blackout.is_blocked(symbol, nowSec())) return false;
    // ?? Regime block gate -- applied in all modes ??????????????????????????
    if (g_regime_adaptor.equity_blocked(symbol)) return false;
    // REMOVED: corr_heat -- with 1-2 open positions max never adds value, only blocks
    return true;
}

// ── ca_on_close ──────────────────────────────────────────────────────────────
static void ca_on_close(const omega::TradeRecord& tr) {
    handle_closed_trade(tr);
    send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
}

// ── bracket_on_close ─────────────────────────────────────────────────────────
static void bracket_on_close(const omega::TradeRecord& tr) {
    handle_closed_trade(tr);

    // ?? Per-symbol bracket trend bias update ?????????????????????????????
    // Applies to ALL bracket symbols. Tracks consecutive profitable exits
    // to detect a dominant trend, then suppresses counter-trend re-arms.
    // L2 imbalance extends/shortens the block dynamically (see BracketTrendState).
    {
        const int64_t now_ms_bc = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const bool profitable = (tr.exitReason == std::string("TRAIL_HIT") ||
                                 tr.exitReason == std::string("TP_HIT")    ||
                                 tr.exitReason == std::string("BE_HIT"));
        g_bracket_trend[tr.symbol].on_exit(tr.side == "LONG", profitable, now_ms_bc);

        // ?? Pyramid SL cooldown ???????????????????????????????????????????
        // If this closing trade was a pyramid entry that hit SL, record the
        // timestamp. pyramid_allowed() will block new pyramids for 2 minutes.
        if (!profitable && tr.exitReason == std::string("SL_HIT")) {
            if (g_pyramid_clordids.count(tr.symbol)) {
                g_bracket_trend[tr.symbol].last_pyramid_sl_ms = now_ms_bc;
                printf("[PYRAMID-SL-COOLDOWN] %s pyramid SL hit -- blocking new pyramids for 120s\n",
                       tr.symbol.c_str());
            }
        }
        // Clear pyramid flag on any close (win, loss, or BE)
        g_pyramid_clordids.erase(tr.symbol);
    }

    // Only send a market close order for reasons where the BROKER has no
    // pending order to handle the close automatically:
    //   BREAKOUT_FAIL -- engine detected failure before TP/SL; broker still
    //                   has the OCO live and must be cancelled + closed.
    //   FORCE_CLOSE   -- disconnect or manual close; broker may have live orders.
    // Do NOT send for TP_HIT / SL_HIT / BE_HIT -- in LIVE mode the broker's
    // OCO order fills the close. Sending another market order doubles the position.
    const bool needs_market_close =
        (tr.exitReason == "BREAKOUT_FAIL" ||
         tr.exitReason == "FORCE_CLOSE");
    if (!needs_market_close) return;
    const bool close_is_long = (tr.side == "SHORT");
    std::cout << "\033[1;35m[BRACKET-CLOSE] " << tr.symbol
              << " " << (close_is_long ? "BUY" : "SELL") << " (close)"
              << " qty=" << tr.size
              << " exit=" << std::fixed << std::setprecision(4) << tr.exitPrice
              << " reason=" << tr.exitReason
              << "\033[0m\n";
    std::cout.flush();
    send_live_order(tr.symbol, close_is_long, tr.size, tr.exitPrice);
}

// ── sup_decision ─────────────────────────────────────────────────────────────
template<typename EngT>
static omega::SupervisorDecision sup_decision(
    omega::SymbolSupervisor& sup, EngT& eng, bool base_can_enter,
    const std::string& sym, double bid, double ask)
{
    int fb = 0;
    { std::lock_guard<std::mutex> lk(g_false_break_mtx);
      auto it = g_false_break_counts.find(sym); if (it != g_false_break_counts.end()) fb = it->second; }
    // Momentum: vol expansion relative to baseline -- how much vol has grown.
    // Was: (mid - comp_low)/mid which is a price distance, not directional momentum.
    // Correct: (recent_vol - base_vol) / base_vol -- captures vol expansion direction.
    const double momentum_proxy = (eng.base_vol_pct > 0.0)
        ? ((eng.recent_vol_pct - eng.base_vol_pct) / eng.base_vol_pct * 100.0)
        : 0.0;
    // in_compression: use the engine's own compression state (phase==COMPRESSION)
    // but also pass true during BREAKOUT_WATCH -- supervisor should know the engine
    // just exited compression and is watching for a break, not re-classify as no-setup.
    const bool in_comp_or_watch = (eng.phase == omega::Phase::COMPRESSION
                                || eng.phase == omega::Phase::BREAKOUT_WATCH);
    const auto sdec_result = sup.update(
        bid, ask,
        eng.recent_vol_pct, eng.base_vol_pct,
        momentum_proxy,
        eng.comp_high, eng.comp_low,
        in_comp_or_watch,
        fb);
    // Count actual spread blocks -- only when a signal was being evaluated
    if (sdec_result.regime == omega::Regime::HIGH_RISK_NO_TRADE
        && sdec_result.reason != nullptr
        && std::string(sdec_result.reason) == "spread_too_wide"
        && base_can_enter)
        ++g_gov_spread;
    return sdec_result;
}

// ── enter_directional ────────────────────────────────────────────────────────
static bool cross_engine_dedup_ok(const std::string& sym) {
    std::lock_guard<std::mutex> lk(g_dedup_mtx);
    auto it = g_last_cross_entry.find(sym);
    if (it != g_last_cross_entry.end() &&
        (nowSec() - it->second) < CROSS_ENG_DEDUP_SEC) {
        printf("[CROSS-DEDUP] %s blocked -- another engine entered %.0fs ago\n",
               sym.c_str(), static_cast<double>(nowSec() - it->second));
        return false;
    }
    // NOTE: timestamp is NOT stamped here -- only stamped on successful execution
    // (at send_live_order call). Stamping here would block the next 30s even when
    // the trade is subsequently rejected by vwap_gate, L2 score, cost guard, etc.
    return true;
}

static void cross_engine_dedup_stamp(const std::string& sym) {
    std::lock_guard<std::mutex> lk(g_dedup_mtx);
    g_last_cross_entry[sym] = nowSec();
}

static double enter_directional(
    const char* esym, bool is_long, double entry, double sl, double tp,
    double fallback_lot, bool skip_vwap_gate,
    double bid, double ask, const std::string& sym, const std::string& regime)
{
    // Returns computed lot size on success, 0.0 on failure/blocked.
    // Callers use the return value directly -- eliminates g_last_directional_lot
    // dependency which caused cross-symbol lot corruption (the $1407 bug).
    const double sl_abs_raw = std::fabs(entry - sl);
    // ATR-normalised SL floor -- same logic as breakout dispatch
    const double sl_abs  = g_adaptive_risk.vol_scaler.atr_sl_floor(
        std::string(esym), sl_abs_raw);

    // ?? ATR SL expansion ? proportional TP adjustment ????????????????????
    // FIX: when atr_sl_floor expands the SL (sl_abs > sl_abs_raw),
    // the TP must scale by the same ratio to preserve original R:R.
    // Without this: USOIL sl_raw=0.5 ? sl_floor=2.0, tp=1.5 unchanged
    //   ? R:R = 0.75 ? RR-FLOOR blocks every trade when ATR is elevated.
    // With this:    sl expands 4?, tp expands 4? ? R:R preserved ? OK.
    double tp_rr_adjusted = tp;
    if (tp > 0.0 && sl_abs > sl_abs_raw * 1.01 && sl_abs_raw > 1e-9) {
        const double expand_ratio = sl_abs / sl_abs_raw;
        const double tp_dist_raw  = std::fabs(entry - tp);
        const double tp_dist_adj  = tp_dist_raw * expand_ratio;
        tp_rr_adjusted = is_long ? entry + tp_dist_adj : entry - tp_dist_adj;
        std::printf("[ATR-SL-EXPAND] %s sl_raw=%.5f->sl_floor=%.5f (x%.2f) tp=%.5f->%.5f\n",
                    esym, sl_abs_raw, sl_abs, expand_ratio, tp, tp_rr_adjusted);
    }

    // ?? ATR-based TP scaling ??????????????????????????????????????????????
    // On high-volatility days (e.g. gold ATR $25 vs normal $10), a fixed
    // percentage TP exits 3? too early. Scale TP proportionally with the
    // ratio of current ATR to the rolling slow-ATR baseline.
    // Bounds: 0.70? (protect low-vol entries) to 2.50? (cap on extreme days).
    // Only applies when the caller provided a real TP (tp > 0); the 2R default
    // fallback is already sized relative to sl_abs and does not need scaling.
    double tp_scaled = tp_rr_adjusted;
    if (tp > 0) {
        const double atr_fast_v = g_adaptive_risk.vol_scaler.atr_fast(std::string(esym));
        const double atr_slow_v = g_adaptive_risk.vol_scaler.atr_slow(std::string(esym));
        if (atr_fast_v > 0.0 && atr_slow_v > 0.0) {
            const double atr_ratio = std::min(2.50, atr_fast_v / atr_slow_v);
            const double atr_tp_mult = std::max(0.70, std::min(2.50, atr_ratio));
            if (atr_tp_mult > 1.05 || atr_tp_mult < 0.95) {  // only log meaningful changes
                static thread_local int64_t s_atr_tp_log = 0;
                if (nowSec() - s_atr_tp_log > 30) {
                    s_atr_tp_log = nowSec();
                    std::printf("[ATR-TP-SCALE] %s %s tp=%.5f->%.5f (atr_fast=%.4f atr_slow=%.4f ratio=%.2f mult=%.2f)\n",
                                esym, is_long ? "LONG" : "SHORT",
                                tp, is_long ? entry + (tp - entry) * atr_tp_mult
                                            : entry - (entry - tp) * atr_tp_mult,
                                atr_fast_v, atr_slow_v, atr_ratio, atr_tp_mult);
                }
            }
            tp_scaled = is_long
                ? entry + (tp - entry) * atr_tp_mult
                : entry - (entry - tp) * atr_tp_mult;
        }
    }
    const double tp_dist = (tp_scaled > 0) ? std::fabs(entry - tp_scaled) : sl_abs * 2.0;
    const double base_lot_raw = compute_size(esym, sl_abs, ask - bid, fallback_lot);
    // Vol-regime size scale -- CRUSH=1.10x, HIGH=0.75x, NORMAL=1.0x
    const double base_lot = base_lot_raw * static_cast<double>(
        g_regime_adaptor.vol_size_scale(std::string(esym)));

    // ?? Regime weight ?????????????????????????????????????????????????????
    // Map symbol ? EngineClass and apply macro regime weight.
    // FX_CASCADE and FX_CARRY return 0.0 in RISK_OFF ? hard block.
    using EC = omega::regime::EngineClass;
    EC ec = EC::CROSS_ASSET; // safe default for ORB/VWAP/TrendPB
    const std::string_view sv(esym);
    if      (sv == "US500.F" || sv == "USTEC.F" || sv == "DJ30.F" || sv == "NAS100")
        ec = EC::US_EQUITY_BREAKOUT;
    else if (sv == "GER40" || sv == "UK100" || sv == "ESTX50")
        ec = EC::EU_EQUITY_BREAKOUT;
    else if (sv == "XAGUSD")
        ec = EC::SILVER_BREAKOUT;
    else if (sv == "USOIL.F" || sv == "BRENT")
        ec = EC::OIL_BREAKOUT;
    else if (sv == "EURUSD")
        ec = EC::FX_BREAKOUT;
    else if (sv == "GBPUSD" || sv == "AUDUSD" || sv == "NZDUSD")
        ec = EC::FX_CASCADE;
    else if (sv == "USDJPY")
        ec = EC::FX_CARRY;
    else if (sv == "XAUUSD")
        ec = EC::GOLD_STACK; // shouldn't reach here -- gold has bespoke path

    const float regime_wt = g_regime_adaptor.weight(ec);
    if (regime_wt <= 0.0f) {
        // Hard block: engine class disabled under current macro regime
        // (FX_CASCADE=0.0 and FX_CARRY=0.0 in RISK_OFF per weight table)
        static thread_local int64_t s_regime_log = 0;
        if (nowSec() - s_regime_log > 60) {
            s_regime_log = nowSec();
            std::printf("[REGIME-BLOCK] %s %s blocked -- engine class weight=0 in regime=%s\n",
                        esym, is_long ? "LONG" : "SHORT",
                        g_regime_adaptor.last_regime.c_str());
        }
        return 0.0;
    }

    // ?? L2 liveness hard gate -- XAUUSD requires live DOM data ??????????
    // L2 DOM is core signal infrastructure for XAUUSD entries:
    //   - imbalance gates MacroCrash, GoldFlow, bracket arming
    //   - microprice_bias feeds edge score
    //   - wall/vacuum gates entry_score_l2
    // If L2 is dead (gold_l2_real=false), ALL of these run on stale 0.500
    // values. Every entry is effectively blind. Block until L2 recovers.
    // Non-gold symbols: use ctrader_l2_live (any DOM event = ok).
    if (sv == "XAUUSD" && !g_macro_ctx.gold_l2_real) {
        static thread_local int64_t s_l2_block_log = 0;
        if (nowSec() - s_l2_block_log > 30) {
            s_l2_block_log = nowSec();
            printf("[L2-DEAD-BLOCK] XAUUSD entry blocked -- gold_l2_real=0, waiting for DOM\n");
            fflush(stdout);
        }
        return 0.0;
    }
    if (sv != "XAUUSD" && !g_macro_ctx.ctrader_l2_live) {
        static thread_local int64_t s_l2_idx_log = 0;
        if (nowSec() - s_l2_idx_log > 30) {
            s_l2_idx_log = nowSec();
            printf("[L2-DEAD-BLOCK] %s entry blocked -- ctrader_l2_live=0, waiting for DOM\n", esym);
            fflush(stdout);
        }
        return 0.0;
    }

    // ?? Cross-engine deduplication ????????????????????????????????????
    if (!cross_engine_dedup_ok(std::string(esym))) return 0.0;

    // ?? EWM Correlation Matrix gate ????????????????????????????????????????
    // Block entry if any currently-open symbol is correlated > 0.85 with esym.
    // Uses realised EWM correlation (last ~120 ticks) not static cluster counts.
    // Runs AFTER the static corr_heat count guard as a second, precise layer.
    {  // EWM correlation gate -- returns true immediately if insufficient data
        // Collect currently-open symbol names
        std::vector<std::string> open_syms;
        open_syms.reserve(16);
        auto add_if = [&](const char* s, bool active) { if (active) open_syms.emplace_back(s); };
        add_if("US500.F",  g_eng_sp.pos.active);
        add_if("USTEC.F",  g_eng_nq.pos.active);
        add_if("DJ30.F",   g_eng_us30.pos.active);
        add_if("NAS100",   g_eng_nas100.pos.active);
        add_if("USOIL.F",  g_eng_cl.pos.active);
        add_if("BRENT",    g_eng_brent.pos.active);
        add_if("XAGUSD",   g_eng_xag.pos.active);
        add_if("EURUSD",   g_eng_eurusd.pos.active);
        add_if("GBPUSD",   g_eng_gbpusd.pos.active);
        add_if("USDJPY",   g_eng_usdjpy.pos.active);
        add_if("AUDUSD",   g_eng_audusd.pos.active);
        add_if("NZDUSD",   g_eng_nzdusd.pos.active);
        add_if("GER40",    g_eng_ger30.pos.active);
        add_if("UK100",    g_eng_uk100.pos.active);
        add_if("XAUUSD",   g_gold_flow.has_open_position()
                         || g_gold_stack.has_open_position()
                         || g_trend_pb_gold.has_open_position());
        if (!g_corr_matrix.entry_allowed(std::string(esym), open_syms))
            return 0.0;
    }  // end corr-matrix gate

    // ?? VPIN toxicity gate (XAUUSD only) ?????????????????????????????????????????
    // Block GoldFlow/GoldStack entries when order flow is toxic (VPIN >= 0.70).
    // Non-gold symbols: VPIN not yet computed for them (only gold tick feeds g_vpin).
    if (sv == "XAUUSD" && g_vpin.warmed() && g_vpin.toxic()) {
        static thread_local int64_t s_vpin_log = 0;
        if (nowSec() - s_vpin_log > 15) {
            s_vpin_log = nowSec();
            std::printf("[VPIN-GATE] XAUUSD entry blocked: vpin=%.3f >= %.2f (toxic flow)\n",
                        g_vpin.vpin(), g_vpin.toxic_threshold);
            std::fflush(stdout);
        }
        return 0.0;
    }

    // ?? VWAP chop gate ????????????????????????????????????????????????????
    // Entries within 0.05% of daily VWAP have no directional edge.
    // VWAP is the mean-reversion anchor -- breakouts from inside the VWAP zone
    // chop back constantly. Get VWAP from the matching BreakoutEngine.
    // EXEMPT: mean-reversion engines (VWAP_REV, FX_FIX, carry unwind, TrendPB)
    // that specifically target the VWAP zone -- they pass skip_vwap_gate=true.
    if (!skip_vwap_gate) {
        double vwap = 0.0;
        if      (sv == "US500.F")  vwap = g_eng_sp.vwap();
        else if (sv == "USTEC.F")  vwap = g_eng_nq.vwap();
        else if (sv == "DJ30.F")   vwap = g_eng_us30.vwap();
        else if (sv == "NAS100")   vwap = g_eng_nas100.vwap();
        else if (sv == "GER40")    vwap = g_eng_ger30.vwap();
        else if (sv == "UK100")    vwap = g_eng_uk100.vwap();
        else if (sv == "ESTX50")   vwap = g_eng_estx50.vwap();
        else if (sv == "XAGUSD")   vwap = g_eng_xag.vwap();
        else if (sv == "EURUSD")   vwap = g_eng_eurusd.vwap();
        else if (sv == "GBPUSD")   vwap = g_eng_gbpusd.vwap();
        else if (sv == "AUDUSD")   vwap = g_eng_audusd.vwap();
        else if (sv == "NZDUSD")   vwap = g_eng_nzdusd.vwap();
        else if (sv == "USDJPY")   vwap = g_eng_usdjpy.vwap();
        else if (sv == "USOIL.F") vwap = g_eng_cl.vwap();
        else if (sv == "BRENT")   vwap = g_eng_brent.vwap();
        else if (sv == "XAUUSD")  vwap = g_gold_stack.vwap();
        if (!g_edges.vwap_gate(entry, vwap)) {
            printf("[VWAP-CHOP] %s %s entry=%.4f vwap=%.4f dist=%.3f%% -- in chop zone, skipping\n",
                   esym, is_long?"LONG":"SHORT", entry, vwap,
                   vwap > 0 ? std::fabs(entry - vwap) / vwap * 100.0 : 0.0);
            return 0.0;
        }
    }

    // sized_lot declared here at outer scope -- computed inside L2 block below
    double sized_lot = base_lot * static_cast<double>(regime_wt); // default (no L2 data)

    // ?? L2 microstructure edge score ? lot multiplier ?????????????????????
    // Combines: CVD, PDH/PDL, round numbers (original 4 signals) PLUS:
    //   - Microprice bias confirmation/contradiction
    //   - Liquidity vacuum in direction (+2 fast move)
    //   - Wall between entry and TP (-3 hard block)
    //   - Order flow absorption (-2 institutional fading)
    //   - Volume profile node/thin area (+/-1)
    // Score range -7..+7. Block at <= -3. Boost at >= +3.
    {
        // Pull L2 microstructure from MacroContext for this symbol
        double microprice_bias = 0.0;
        double l2_imbalance    = 0.5;
        bool   vacuum_in_dir   = false;
        bool   wall_to_tp      = false;

        if      (sv == "XAUUSD") {
            microprice_bias = g_macro_ctx.gold_microprice_bias;
            l2_imbalance    = g_macro_ctx.gold_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.gold_vacuum_ask : g_macro_ctx.gold_vacuum_bid;
            wall_to_tp      = is_long ? g_macro_ctx.gold_wall_above : g_macro_ctx.gold_wall_below;
        } else if (sv == "US500.F" || sv == "USTEC.F" || sv == "NAS100" || sv == "DJ30.F") {
            microprice_bias = g_macro_ctx.sp_microprice_bias;
            l2_imbalance    = (sv == "US500.F") ? g_macro_ctx.sp_l2_imbalance : g_macro_ctx.nq_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.sp_vacuum_ask : g_macro_ctx.sp_vacuum_bid;
            wall_to_tp      = is_long ? g_macro_ctx.sp_wall_above : g_macro_ctx.sp_wall_below;
        } else if (sv == "XAGUSD") {
            microprice_bias = g_macro_ctx.xag_microprice_bias;
            l2_imbalance    = g_macro_ctx.xag_l2_imbalance;
            // Silver: no vacuum/wall in MacroContext yet -- use L2 imbalance as proxy
            vacuum_in_dir   = is_long ? (l2_imbalance < 0.30) : (l2_imbalance > 0.70);
        } else if (sv == "USOIL.F" || sv == "BRENT") {
            microprice_bias = g_macro_ctx.cl_microprice_bias;
            l2_imbalance    = g_macro_ctx.cl_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.cl_vacuum_ask : g_macro_ctx.cl_vacuum_bid;
        } else if (sv == "EURUSD") {
            microprice_bias = g_macro_ctx.eur_microprice_bias;
            l2_imbalance    = g_macro_ctx.eur_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.eur_vacuum_ask : g_macro_ctx.eur_vacuum_bid;
            wall_to_tp      = is_long ? g_macro_ctx.eur_wall_above  : g_macro_ctx.eur_wall_below;
        } else if (sv == "GBPUSD") {
            microprice_bias = g_macro_ctx.gbp_microprice_bias;
            l2_imbalance    = g_macro_ctx.gbp_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.gbp_vacuum_ask : g_macro_ctx.gbp_vacuum_bid;
            wall_to_tp      = is_long ? g_macro_ctx.gbp_wall_above  : g_macro_ctx.gbp_wall_below;
        } else if (sv == "AUDUSD") {
            microprice_bias = g_macro_ctx.aud_microprice_bias;
            l2_imbalance    = g_macro_ctx.aud_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.aud_vacuum_ask : g_macro_ctx.aud_vacuum_bid;
        } else if (sv == "NZDUSD") {
            microprice_bias = g_macro_ctx.nzd_microprice_bias;
            l2_imbalance    = g_macro_ctx.nzd_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.nzd_vacuum_ask : g_macro_ctx.nzd_vacuum_bid;
        } else if (sv == "USDJPY") {
            microprice_bias = g_macro_ctx.jpy_microprice_bias;
            l2_imbalance    = g_macro_ctx.jpy_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.jpy_vacuum_ask : g_macro_ctx.jpy_vacuum_bid;
        } else if (sv == "GER40") {
            microprice_bias = g_macro_ctx.ger40_microprice_bias;
            l2_imbalance    = g_macro_ctx.ger40_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.ger40_vacuum_ask : g_macro_ctx.ger40_vacuum_bid;
            wall_to_tp      = is_long ? g_macro_ctx.ger40_wall_above  : g_macro_ctx.ger40_wall_below;
        } else if (sv == "UK100") {
            l2_imbalance    = g_macro_ctx.uk100_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.uk100_vacuum_ask : g_macro_ctx.uk100_vacuum_bid;
        } else if (sv == "ESTX50") {
            l2_imbalance    = g_macro_ctx.estx50_l2_imbalance;
            vacuum_in_dir   = is_long ? g_macro_ctx.estx50_vacuum_ask : g_macro_ctx.estx50_vacuum_bid;
        }

        const double tp_for_score = tp_scaled > 0 ? tp_scaled : entry + (is_long?1:-1)*sl_abs*2.0;
        const int edge_score = g_edges.entry_score_l2(
            esym, entry, is_long, tp_for_score, nowSec(),
            microprice_bias, l2_imbalance, vacuum_in_dir, wall_to_tp);

        if (edge_score <= -3) {
            printf("[EDGE-BLOCK-L2] %s %s score=%d (micro=%.4f l2=%.2f vac=%d wall=%d absorb=%d vp=%d)\n",
                   esym, is_long?"LONG":"SHORT", edge_score,
                   microprice_bias, l2_imbalance,
                   vacuum_in_dir ? 1 : 0, wall_to_tp ? 1 : 0,
                   g_edges.absorption.is_absorbing(esym, is_long) ? 1 : 0,
                   g_edges.vol_profile.score(esym, entry));
            return 0.0;
        }
        const double edge_mult = 1.0 + std::max(-0.25, std::min(0.35, edge_score * 0.07));
        sized_lot = base_lot * static_cast<double>(regime_wt) * edge_mult;
    }

    // Adaptive risk: DD throttle + Kelly + vol regime
    double sym_loss = 0.0; int sym_consec = 0;
    {
        std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
        auto it = g_sym_risk.find(esym);
        if (it != g_sym_risk.end()) {
            sym_loss   = std::max(0.0, -it->second.daily_pnl);
            sym_consec = it->second.consec_losses;
        }
    }
    double lot = g_adaptive_risk.adjusted_lot(
        esym, sized_lot, sym_loss, g_cfg.daily_loss_limit, sym_consec);
    // ?? TOD-weighted lot scaling ??????????????????????????????????????
    {
        const double tod_mult = g_edges.tod.size_scale(
            std::string(esym), "ALL", nowSec());
        if (tod_mult < 1.0) {
            printf("[TOD-SCALE] %s lot %.4f ? %.4f (tod_mult=%.2f)\n",
                   esym, lot, lot * tod_mult, tod_mult);
            lot = std::max(0.01, std::floor(lot * tod_mult * 100.0 + 0.5) / 100.0);
        }
    }
    // ?? Max loss per trade cap ????????????????????????????????????????????
    // Hard dollar backstop: if sl_abs * lot * tick_mult > max_loss_per_trade_usd,
    // scale lot down. Fires last -- after Kelly, regime weight, DD throttle.
    double final_lot = lot;
    if (g_cfg.max_loss_per_trade_usd > 0.0 && sl_abs > 0.0) {
        const double tick_mult = tick_value_multiplier(esym);
        const double max_loss_lot = g_cfg.max_loss_per_trade_usd / (sl_abs * tick_mult);
        if (final_lot > max_loss_lot) {
            final_lot = std::max(0.01, std::floor(max_loss_lot * 100.0 + 0.5) / 100.0);
            printf("[MAX-LOSS-CAP] %s lot capped %.4f?%.4f (sl=$%.2f max=$%.0f)\n",
                   esym, lot, final_lot, sl_abs * tick_mult * lot, g_cfg.max_loss_per_trade_usd);
        }
    }
    // ?? Vol-parity sizing multiplier (OmegaCorrelationMatrix) ??????????
    // Scale lot by vol_target/ewm_vol(sym) so high-vol assets get smaller
    // positions and low-vol assets get proportionally larger ones.
    // Clamped to [0.50, 1.50]. Returns 1.0 when matrix not yet warmed.
    {
        const double vp_scale = g_corr_matrix.vol_parity_scale(std::string(esym));
        if (vp_scale < 0.99 || vp_scale > 1.01) {  // only log non-trivial changes
            static thread_local int64_t s_vp_log = 0;
            if (nowSec() - s_vp_log > 30) {
                s_vp_log = nowSec();
                std::printf("[VOL-PARITY] %s lot %.4f -> %.4f (scale=%.3f)\n",
                            esym, final_lot, final_lot * vp_scale, vp_scale);
                std::fflush(stdout);
            }
        }
        final_lot = std::max(0.01, std::floor(final_lot * vp_scale * 100.0 + 0.5) / 100.0);
    }

    // ?? Covariance-based position sizing ??????????????????????????????????????????
    // When entering a symbol that is highly covariant with an already-open position,
    // reduce lot size proportionally. Prevents doubling metals exposure when both
    // XAUUSD and XAGUSD are open, or stacking correlated index positions.
    // Uses g_corr_matrix.covariance_sizing_mult() which:
    //   - Returns 1.0 when no open positions or covariance below threshold
    //   - Scales down to [0.50, 1.0) as net covariance exposure increases
    //   - floor of 0.50 prevents sizing to zero on perfect correlation
    {
        // Build open position list with direction signs
        std::vector<std::pair<std::string,int>> cov_open;
        cov_open.reserve(16);
        auto cov_add = [&](const char* s, bool active, bool is_long_pos) {
            if (active) cov_open.emplace_back(s, is_long_pos ? 1 : -1);
        };
        cov_add("US500.F",  g_eng_sp.pos.active,     g_eng_sp.pos.is_long);
        cov_add("USTEC.F",  g_eng_nq.pos.active,     g_eng_nq.pos.is_long);
        cov_add("DJ30.F",   g_eng_us30.pos.active,   g_eng_us30.pos.is_long);
        cov_add("NAS100",   g_eng_nas100.pos.active,  g_eng_nas100.pos.is_long);
        cov_add("USOIL.F",  g_eng_cl.pos.active,     g_eng_cl.pos.is_long);
        cov_add("BRENT",    g_eng_brent.pos.active,  g_eng_brent.pos.is_long);
        cov_add("XAGUSD",   g_eng_xag.pos.active,    g_eng_xag.pos.is_long);
        cov_add("EURUSD",   g_eng_eurusd.pos.active, g_eng_eurusd.pos.is_long);
        cov_add("GBPUSD",   g_eng_gbpusd.pos.active, g_eng_gbpusd.pos.is_long);
        cov_add("USDJPY",   g_eng_usdjpy.pos.active, g_eng_usdjpy.pos.is_long);
        cov_add("AUDUSD",   g_eng_audusd.pos.active, g_eng_audusd.pos.is_long);
        cov_add("NZDUSD",   g_eng_nzdusd.pos.active, g_eng_nzdusd.pos.is_long);
        cov_add("GER40",    g_eng_ger30.pos.active,  g_eng_ger30.pos.is_long);
        cov_add("UK100",    g_eng_uk100.pos.active,  g_eng_uk100.pos.is_long);
        cov_add("XAUUSD",   g_gold_flow.has_open_position()
                          || g_gold_stack.has_open_position()
                          || g_trend_pb_gold.has_open_position(),
                            is_long);  // direction of this entry
        const double cov_mult = g_corr_matrix.covariance_sizing_mult(std::string(esym), cov_open);
        if (cov_mult < 0.99) {
            final_lot = std::max(0.01, std::floor(final_lot * cov_mult * 100.0 + 0.5) / 100.0);
        }
    }

    // ?? Hard R:R floor -- checked at dispatch, not signal generation ????
    // Signal generation uses comp_range ? 1.6 / comp_range ? 0.4 = 4:1.
    // But L2 wall penalties, SL adjustments, and ATR sizing can push this
    // below 1.5:1 before the order fires. Block any trade with R:R < 1.5.
    if (sl_abs > 1e-9 && tp_dist > 1e-9) {
        const double rr = tp_dist / sl_abs;
        if (rr < 1.5) {
            printf("[RR-FLOOR] %s %s blocked: R:R=%.2f (tp=%.4f sl=%.4f) < 1.5 floor\n",
                   esym, is_long?"LONG":"SHORT", rr, tp_dist, sl_abs);
            return 0.0;
        }
    }
    // Cost guard
    if (!ExecutionCostGuard::is_viable(esym, ask - bid, tp_dist, final_lot)) {
        g_telemetry.IncrCostBlocked();
        return 0.0;
    }
    // All gates passed -- stamp cross-engine dedup NOW (not at check time)
    cross_engine_dedup_stamp(std::string(esym));
    g_last_directional_lot = final_lot;  // expose for caller pos_.size patch
    // Log entry to trade opens CSV before firing
    write_trade_open_log(esym,
        "directional",                       // refined by caller via UpdateLastSignal
        is_long ? "LONG" : "SHORT",
        entry,
        tp_scaled > 0 ? tp_scaled : entry + (is_long?1:-1)*tp_dist,
        sl, final_lot, ask - bid,
        regime.empty() ? "?" : regime,
        "ENTRY");
    // Arm partial exit and fire
    // XAUUSD is excluded: GoldFlow arms g_partial_exit explicitly after entry
    // with the correct ATR and 2R TP2 (line ~7222), and GoldStack arms at signal
    // time (line ~6573). Arming here would double-arm with wrong TP2 = enter_directional's
    // tp_scaled (often 0), causing a phantom second partial order on the same position.
    if (std::string(esym) != "XAUUSD") {
        g_partial_exit.arm(esym, is_long, entry, tp_scaled > 0 ? tp_scaled : entry + (is_long?1:-1)*tp_dist,
                           sl, final_lot, g_adaptive_risk.vol_scaler.atr_fast(esym));
    }
    // ?? Increment portfolio open SL risk ????????????????????????????????????
    // Tracks total simultaneous max-loss across all open positions.
    // Decremented in handle_closed_trade when position closes.
    {
        const double tick_val = tick_value_multiplier(esym);
        portfolio_sl_risk_add(sl_abs, final_lot, tick_val);
    }
    // ?? Entry log -- every trade every engine every symbol ??????????????
    // Previously only GoldFlow/GoldStack printed entry lines.
    // VWAPRev TrendPB NBM ORB were silent -- impossible to audit live.
    printf("[ENTRY] %s %s @ %.5f sl=%.5f tp=%.5f lot=%.4f sl_pts=%.5f\n",
           esym, is_long ? "LONG" : "SHORT",
           entry, sl,
           tp_scaled > 0 ? tp_scaled : entry + (is_long?1:-1)*tp_dist,
           final_lot, sl_abs);
    fflush(stdout);
    send_live_order(esym, is_long, final_lot, entry);
    g_telemetry.UpdateLastEntryTs();  // watchdog: stamp last successful entry
    return final_lot;
}

// ── cross_engine_dedup_ok ───────────────────────────────────────────────────
// ── cross_engine_dedup_stamp ────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// Per-symbol tick handlers — included here (inside on_tick scope is NOT needed
// since all lambdas are now static functions). Included before on_tick so the
// handler functions are defined before on_tick calls them via dispatch.
// ─────────────────────────────────────────────────────────────────────────────
#include "tick_indices.hpp"
#include "tick_oil.hpp"
#include "tick_fx.hpp"
#include "tick_gold.hpp"

