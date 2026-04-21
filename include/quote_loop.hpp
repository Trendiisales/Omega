#pragma once
// quote_loop.hpp -- extracted from main.cpp
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static void quote_loop() {
    int backoff_ms = 1000;
    const int max_backoff = 30000;

    while (g_running.load()) {
        std::cout << "[OMEGA] Connecting " << g_cfg.host << ":" << g_cfg.port << "\n";
        g_telemetry.UpdateFixStatus("CONNECTING", "CONNECTING", 0, 0);

        int sock = -1;
        SSL* ssl = connect_ssl(g_cfg.host, g_cfg.port, sock);
        if (!ssl) {
            std::cerr << "[OMEGA] Connect failed -- retry " << backoff_ms << "ms\n";
            // Interruptible backoff -- exits immediately on shutdown signal
            for (int i = 0; i < backoff_ms / 10 && g_running.load(); ++i) Sleep(10);
            backoff_ms = std::min(backoff_ms * 2, max_backoff);
            continue;
        }

        backoff_ms = 1000;
        extract_messages(nullptr, 0, true);  // reset recv buffer on reconnect
        g_quote_seq      = 1;
        g_rtt_pending_ts = 0;
        g_quote_ready.store(false);
        g_md_subscribed.store(false);

        const std::string logon = fix_build_logon(g_quote_seq++, "QUOTE");
        SSL_write(ssl, logon.c_str(), static_cast<int>(logon.size()));
        std::cout << "[OMEGA] Logon sent\n";

        auto last_ping      = std::chrono::steady_clock::now();
        auto last_heartbeat = std::chrono::steady_clock::now();  // FIX 35=0 heartbeat
        auto last_diag      = std::chrono::steady_clock::now();
        auto logon_sent_at  = std::chrono::steady_clock::now();


        while (g_running.load()) {
            const auto now = std::chrono::steady_clock::now();

            if (!g_quote_ready.load() &&
                std::chrono::duration_cast<std::chrono::seconds>(now - logon_sent_at).count() >= 10) {
                std::cerr << "[OMEGA] Logon timeout (10s) -- reconnecting\n";
                break;
            }

            // Proactive 13min reconnect REMOVED.
            // Was added to handle "32 drops at 15min intervals" -- those drops
            // were caused by d7a0a16's L2 auto-restart, not a broker timeout.
            // With L2 restart removed, FIX session stays up indefinitely.

            // ?? FIX Heartbeat (35=0) every 30s ???????????????????????????????
            // CRITICAL: broker requires a proper Heartbeat MsgType=0 from us
            // every HeartBtInt seconds (set to 30 in logon tag 108=30).
            // Without this, broker terminates the session after ~30s of silence.
            // TestRequest (35=1) does NOT substitute for a proactive Heartbeat.
            // This was the root cause of 17 disconnects per trading day.
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_heartbeat).count() >= g_cfg.heartbeat) {
                last_heartbeat = now;
                const std::string hb = build_heartbeat(g_quote_seq++, "QUOTE");
                if (SSL_write(ssl, hb.c_str(), static_cast<int>(hb.size())) <= 0) break;
            }

            // ?? Stale symbol auto-resubscribe ?????????????????????????????????
            // Broker occasionally drops individual symbol subscriptions silently
            // (observed: XAUUSD stops streaming while all others continue).
            // Every 60s check all primary symbols -- if any has gone silent for
            // > 45s, force a full re-subscribe to recover the dropped feed.
            {
                static auto last_stale_check = now;
                if (std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_stale_check).count() >= 60 && g_quote_ready.load()) {
                    last_stale_check = now;
                    bool any_stale = false;
                    const int64_t now_ms_sc = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    // Check primary symbols only (the ones we always subscribe)
                    static const char* primary_syms[] = {
                        "XAUUSD","US500.F","USTEC.F","DJ30.F","NAS100","USOIL.F"
                    };
                    for (const char* psym : primary_syms) {
                        std::lock_guard<std::mutex> lk(g_last_tick_mtx);
                        auto it = g_last_tick_ts.find(psym);
                        // Check 1: silence -- no tick for 45s
                        if (it == g_last_tick_ts.end() ||
                            (now_ms_sc - it->second) > 45000) {
                            printf("[STALE-RESUB] %s silent >45s -- forcing full re-subscribe\n", psym);
                            fflush(stdout);
                            any_stale = true;
                            break;
                        }
                        // Check 2: frozen price -- N consecutive identical bids
                        // Catches broker sending repeated last-price instead of live feed.
                        // Broker repeated last-price with updated timestamps -- price never moved.
                        // At FROZEN_TICK_MAX=20 ticks (~3.3 min at Asia tick rate), re-subscribe.
                        auto fc = g_frozen_count.find(psym);
                        if (fc != g_frozen_count.end() && fc->second >= FROZEN_TICK_MAX) {
                            printf("[STALE-RESUB] %s frozen price x%d ticks (bid=%.5f) -- forcing re-subscribe\n",
                                   psym, fc->second, g_last_tick_bid.count(psym) ? g_last_tick_bid.at(psym) : 0.0);
                            fflush(stdout);
                            g_frozen_count[psym] = 0;  // reset so we don't re-trigger immediately
                            any_stale = true;
                            break;
                        }
                    }
                    if (any_stale) {
                        const std::string unsub_s = fix_build_md_unsub_all(g_quote_seq++);
                        if (!unsub_s.empty()) SSL_write(ssl, unsub_s.c_str(), static_cast<int>(unsub_s.size()));
                        Sleep(150);
                        const std::string resub_s = fix_build_md_subscribe_all(g_quote_seq++);
                        if (!resub_s.empty()) {
                            SSL_write(ssl, resub_s.c_str(), static_cast<int>(resub_s.size()));
                            g_md_subscribed.store(true);
                            printf("[STALE-RESUB] Full re-subscribe sent\n");
                            fflush(stdout);
                        }
                    }
                }
            }

            // RTT ping every 5s
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count() >= 5) {
                last_ping = now;
                if (g_rtt_pending_ts == 0) {
                    g_rtt_pending_id = "omega-" + std::to_string(nowSec());
                    g_rtt_pending_ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    const std::string tr = build_test_request(g_quote_seq++, "QUOTE", g_rtt_pending_id);
                    SSL_write(ssl, tr.c_str(), static_cast<int>(tr.size()));
                }
            }

            // Diagnostic every 60s -- visibility into engine phase + vol state
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_diag).count() >= 60) {
                last_diag = now;
                if (g_tee_buf) g_tee_buf->force_rotate_check();  // ensure daily log rolls at UTC midnight even if stdout is quiet

                // Push log + state to git every 5 minutes so remote reads are never stale.
                // Fire-and-forget via cmd /c start -- does not block the quote loop.
                {
                    static auto s_last_push = std::chrono::steady_clock::now() - std::chrono::minutes(10);
                    if (std::chrono::duration_cast<std::chrono::minutes>(now - s_last_push).count() >= 5) {
                        s_last_push = now;
                        const std::string push_cmd =
                            "cmd /c start /min powershell -WindowStyle Hidden -ExecutionPolicy Bypass "
                            "-File C:\\Omega\\push_log.ps1 -RepoRoot C:\\Omega";
                        std::system(push_cmd.c_str());
                    }
                }

                // Save ATR state every 60s so restarts always have a fresh, valid value
                g_gold_flow.save_atr_state(log_root_dir() + "/gold_flow_atr.dat");
                g_gold_stack.save_atr_state(log_root_dir() + "/gold_stack_state.dat");
                // Belt-and-suspenders: write backup ATR every 60s
                // If primary gold_flow_atr.dat gets corrupted on a hard crash (process killed
                // mid-write), gold_flow_atr_backup.dat has the value from ~60s earlier.
                // push_log.ps1 tracks both files so Claude can verify ATR health any time.
                g_gold_flow.save_atr_state(log_root_dir() + "/gold_flow_atr_backup.dat");
                g_trend_pb_gold.save_state(log_root_dir()  + "/trend_pb_gold.dat");
                g_trend_pb_ger40.save_state(log_root_dir() + "/trend_pb_ger40.dat");
                g_trend_pb_nq.save_state(log_root_dir()    + "/trend_pb_nq.dat");
                g_trend_pb_sp.save_state(log_root_dir()    + "/trend_pb_sp.dat");
                // Save bar indicator state every 60s -- ensures warm restart on crash/kill.
                // Previously saved only at daily rollover and clean shutdown.
                // If process is killed (OOM, watchdog, manual stop), .dat files were stale
                // or missing -> cold start -> M15 tick request times out -> never seeded.
                // With 60s saves and 12h age limit, bars are always valid on restart.
                if (g_bars_gold.m5 .ind.m1_ready.load(std::memory_order_relaxed))
                    g_bars_gold.m1 .save_indicators(log_root_dir() + "/bars_gold_m1.dat");
    g_bars_gold.m5 .save_indicators(log_root_dir() + "/bars_gold_m5.dat");
                if (g_bars_gold.m15.ind.m1_ready.load(std::memory_order_relaxed))
                    g_bars_gold.m15.save_indicators(log_root_dir() + "/bars_gold_m15.dat");
                if (g_bars_gold.h4 .ind.m1_ready.load(std::memory_order_relaxed))
                    g_bars_gold.h4 .save_indicators(log_root_dir() + "/bars_gold_h4.dat");
                 // Save Kelly perf, TOD buckets and fill quality every 60s.
                 // Previously only saved on clean shutdown -- a hard kill (OOM, NSSM
                 // watchdog, power loss) discarded all intra-session trades and forced
                 // a cold fixed-size restart next session.  With 60s saves any restart
                 // picks up trades from at most 60s ago and sizes correctly immediately.
                 g_adaptive_risk.save_perf(log_root_dir() + "/kelly");
                 g_edges.tod.save_csv(log_root_dir() + "/omega_tod_buckets.csv");
                 g_edges.fill_quality.save_csv(log_root_dir() + "/fill_quality.csv");
                 // Save correlation matrix every 60s -- previously shutdown-only.
                 // Accumulates rolling price correlations across all symbols.
                 // Hard kill without this loses the session's correlation history.
                 g_corr_matrix.save_state(log_root_dir() + "/corr_matrix.dat");
                std::cout << "[OMEGA-DIAG] PnL=" << g_omegaLedger.dailyPnl()
                          << " T=" << g_omegaLedger.total()
                          << " WR=" << g_omegaLedger.winRate() << "%"
                          << " RTTp95=" << g_rtt_p95 << "ms"
                          << " cap=" << g_cfg.max_latency_ms << "ms"
                          << " lat_ok=" << (g_governor.checkLatency((g_rtt_p95 > 0.0 ? g_rtt_p95 : g_rtt_last), g_cfg.max_latency_ms) ? 1 : 0)
                          << " session=" << (session_tradeable() ? "ACTIVE" : "CLOSED") << "\n"
                          << "[OMEGA-DIAG] SP phase=" << static_cast<int>(g_eng_sp.phase)
                          << " recent=" << g_eng_sp.recent_vol_pct << "% base=" << g_eng_sp.base_vol_pct << "%"
                          << " ratio=" << (g_eng_sp.base_vol_pct>0 ? g_eng_sp.recent_vol_pct/g_eng_sp.base_vol_pct : 0) << "\n"
                          << "[OMEGA-DIAG] NQ phase=" << static_cast<int>(g_eng_nq.phase)
                          << " recent=" << g_eng_nq.recent_vol_pct << "% base=" << g_eng_nq.base_vol_pct << "%"
                          << " ratio=" << (g_eng_nq.base_vol_pct>0 ? g_eng_nq.recent_vol_pct/g_eng_nq.base_vol_pct : 0) << "\n"
                          << "[OMEGA-DIAG] CL phase=" << static_cast<int>(g_eng_cl.phase)
                          << " recent=" << g_eng_cl.recent_vol_pct << "% base=" << g_eng_cl.base_vol_pct << "%"
                          << " ratio=" << (g_eng_cl.base_vol_pct>0 ? g_eng_cl.recent_vol_pct/g_eng_cl.base_vol_pct : 0) << "\n";
                // Gold multi-engine stack stats
                g_gold_stack.print_stats();
                std::cout << "[GOLD-DIAG] regime=" << g_gold_stack.regime_name()
                          << " vwap=" << std::fixed << std::setprecision(2) << g_gold_stack.vwap()
                          << " vol_range=" << std::fixed << std::setprecision(2) << g_gold_stack.vol_range() << "\n";
                // ── OPEN POSITION STATE -- every 5s, ALL engines ─────────────────
                // Logs entry, SL, MFE, trail stage for every engine with an open position.
                // Critical for monitoring trail movement and diagnosing missed exits.
                {
                    bool any_open = false;
                    // GoldFlow base
                    if (g_gold_flow.has_open_position()) {
                        const auto& p = g_gold_flow.pos;
                        printf("[OPEN-POS] GoldFlow %s entry=%.2f sl=%.2f mfe=%.2f atr=%.2f be=%d stage=%d\n",
                               p.is_long?"LONG":"SHORT", p.entry, p.sl, p.mfe,
                               p.atr_at_entry, (int)p.be_locked, (int)p.trail_stage);
                        any_open = true;
                    }
                    // GoldFlow reload
                    if (g_gold_flow_reload.has_open_position()) {
                        const auto& p = g_gold_flow_reload.pos;
                        printf("[OPEN-POS] GoldFlow-Reload %s entry=%.2f sl=%.2f mfe=%.2f stage=%d\n",
                               p.is_long?"LONG":"SHORT", p.entry, p.sl, p.mfe, (int)p.trail_stage);
                        any_open = true;
                    }
                    // GoldFlow addon
                    if (g_gold_flow_addon.has_open_position()) {
                        const auto& p = g_gold_flow_addon.pos;
                        printf("[OPEN-POS] GoldFlow-Addon %s entry=%.2f sl=%.2f mfe=%.2f stage=%d\n",
                               p.is_long?"LONG":"SHORT", p.entry, p.sl, p.mfe, (int)p.trail_stage);
                        any_open = true;
                    }
                    // HybridBracketGold
                    if (g_hybrid_gold.has_open_position()) {
                        const auto& p = g_hybrid_gold.pos;
                        printf("[OPEN-POS] HybridBracketGold %s entry=%.2f sl=%.2f mfe=%.2f\n",
                               p.is_long?"LONG":"SHORT", p.entry, p.sl, p.mfe);
                        any_open = true;
                    }
                    // RSIReversal
                    if (g_rsi_reversal.has_open_position()) {
                        const auto& p = g_rsi_reversal.pos;
                        printf("[OPEN-POS] RSIReversal %s entry=%.2f sl=%.2f mfe=%.2f be=%d\n",
                               p.is_long?"LONG":"SHORT", p.entry, p.sl, p.mfe, (int)p.be_locked);
                        any_open = true;
                    }
                    // (MicroMomentum [OPEN-POS] block REMOVED at Batch 5V §1.2.)
                    // MacroCrash
                    if (g_macro_crash.has_open_position()) {
                        const auto& p = g_macro_crash.pos;
                        printf("[OPEN-POS] MacroCrash %s entry=%.2f sl=%.2f mfe=%.2f be=%d banked=%.2f\n",
                               p.is_long?"LONG":"SHORT", p.entry, p.sl, p.mfe,
                               (int)p.be_locked, p.banked_usd);
                        any_open = true;
                    }
                    // GoldStack
                    if (g_gold_stack.has_open_position()) {
                        printf("[OPEN-POS] GoldStack ACTIVE (see GOLD-BRK-DIAG for details)\n");
                        any_open = true;
                    }
                    if (!any_open) {
                        printf("[OPEN-POS] no open positions\n");
                    }
                    fflush(stdout);
                }
                std::cout.unsetf(std::ios::fixed);
                std::cout << std::setprecision(6);
                // Latency edge engines stats
                g_le_stack.print_stats();
                print_perf_stats();

                // ================================================================
                // SYSTEM HEALTH WATCHDOG -- fires every 30s
                // Every failure mode has: detection, log, GUI alert, fallback action.
                // No silent failures. Priority order: highest impact first.
                // ================================================================
                {
                    const bool    sess_active = session_tradeable();
                    const int64_t now_s       = nowSec();

                    static int64_t s_last_depth_events  = 0;
                    static int64_t s_last_depth_check_s = 0;
                    static int64_t s_depth_dead_since   = 0;
                    static int64_t s_no_trade_since     = 0;
                    static int64_t s_last_trade_count   = 0;
                    static int64_t s_startup_s          = 0;
                    static int64_t s_l2_reconnect_ts[3] = {0,0,0}; // ring buffer of reconnect times
                    static int     s_l2_reconnect_idx   = 0;
                    static bool    s_l2_reconnect_blocked = false;
                    static int64_t s_bracket_stall_since = 0;
                    static int64_t s_vix_dead_since     = 0;
                    static int     s_fix_reconnect_n    = 0;
                    static int64_t s_last_fix_connected = 0;

                    if (s_startup_s == 0) s_startup_s = now_s;
                    const int64_t uptime = now_s - s_startup_s;

                    const int64_t depth_now   = (int64_t)g_ctrader_depth.depth_events_total.load();
                    const bool    l2_live      = g_macro_ctx.ctrader_l2_live;
                    const bool    gold_seeded  = g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed);
                    const int64_t trade_count  = g_omegaLedger.total();

                    // Determine dominant alert to show (highest priority wins)
                    std::string alert_msg;
                    bool any_critical = false;

                    // ---- [1] L2 depth feed dead + AUTO-RECONNECT ----------------
                    // Detection: depth_events_total frozen while l2_live=true
                    // Fallback:  after 60s dead, force cTrader reconnect (stop/start)
                    // Alert:     GUI "L2 FEED DEAD Xs"
                    if (l2_live && depth_now == s_last_depth_events && s_last_depth_check_s > 0) {
                        if (s_depth_dead_since == 0) s_depth_dead_since = now_s;
                        const int64_t dead_secs = now_s - s_depth_dead_since;
                        if (dead_secs >= 30) {
                            printf("[SYSTEM-ALERT] L2_DEAD depth_events frozen %llds -- cTrader feed dropped\n",
                                   (long long)dead_secs);
                            fflush(stdout);
                            alert_msg = "L2 FEED DEAD " + std::to_string(dead_secs) + "s";
                            any_critical = true;
                        }
                        if (dead_secs >= 60 && !s_l2_reconnect_blocked) {
                            // Circuit breaker: track reconnect times in ring buffer
                            s_l2_reconnect_ts[s_l2_reconnect_idx % 3] = now_s;
                            ++s_l2_reconnect_idx;
                            // If 3 reconnects within 5 minutes: block further reconnects
                            const int64_t oldest = s_l2_reconnect_ts[(s_l2_reconnect_idx) % 3];
                            const bool loop_detected = (s_l2_reconnect_idx >= 3)
                                && (now_s - oldest < 300);
                            if (loop_detected) {
                                s_l2_reconnect_blocked = true;
                                printf("[SYSTEM-ALERT] L2_RECONNECT_LOOP 3 reconnects in %llds"
                                       " -- reconnect blocked, running on FIX prices only\n",
                                       (long long)(now_s - oldest));
                                fflush(stdout);
                                g_telemetry.SetHealthAlert("RECONNECT LOOP -- L2 DISABLED");
                            } else {
                                // L2 dead >60s -- LOG ONLY, do NOT restart cTrader.
                                // Auto-restart was introduced in d7a0a16 but is the ROOT CAUSE
                                // of the constant cTrader reconnect loop:
                                //   L2 quiet (normal in Asia) -> restart -> tick data req sent
                                //   -> BlackBull drops TCP -> L2 dead again -> restart -> loop
                                // Before d7a0a16 cTrader never restarted mid-session and
                                // overnight sessions worked fine. Reverting to that behaviour.
                                // The circuit breaker above still catches genuine stuck feeds.
                                if (now_s - s_depth_dead_since >= 60) {
                                    static int64_t s_l2_log = 0;
                                    if (now_s - s_l2_log >= 60) {
                                        s_l2_log = now_s;
                                        printf("[SYSTEM-ALERT] L2_DEAD >%llds -- running on FIX prices (no auto-restart)\n",
                                               (long long)(now_s - s_depth_dead_since));
                                        fflush(stdout);
                                    }
                                }
                                alert_msg = "L2 DEAD -- FIX PRICES ONLY";
                            }
                        }
                    } else {
                        if (s_depth_dead_since > 0) {
                            printf("[SYSTEM-ALERT] L2_RESTORED after %llds\n",
                                   (long long)(now_s - s_depth_dead_since));
                            fflush(stdout);
                            s_l2_reconnect_blocked = false;  // clear block on successful restore
                        }
                        s_depth_dead_since = 0;
                    }
                    s_last_depth_events  = depth_now;
                    s_last_depth_check_s = now_s;

                    // ---- [2] Gold bars unseeded + ATR SNAP FALLBACK -------------
                    // Detection: m1_ready=false after 150s uptime
                    // Fallback:  snap ATR from GoldStack vol_range (real market data)
                    //            instead of 5.00 floor — "degraded" not "blind"
                    // Alert:     GUI "GOLD BARS Xs"
                    if (!gold_seeded && uptime > 150) {
                        printf("[SYSTEM-ALERT] GOLD_BARS_UNSEEDED %llds -- GoldFlow degraded (no RSI/EMA gates)\n",
                               (long long)uptime);
                        fflush(stdout);
                        if (alert_msg.empty())
                            alert_msg = "GOLD BARS " + std::to_string(uptime) + "s";
                        any_critical = true;

                        // ATR snap fallback: derive ATR from live vol_range instead of 5.00 floor
                        const double snap_vol = g_gold_stack.vol_range();
                        if (snap_vol > 1.5 && g_gold_flow.current_atr() <= 5.0) {
                            // vol_range = high-low of last 50 ticks ~ 1.5x ATR empirically
                            const double snapped_atr = snap_vol / 1.5;
                            g_gold_flow.set_atr_override(snapped_atr);
                            printf("[SYSTEM-ALERT] BARS_ATR_SNAP vol_range=%.2f -> atr_override=%.2f\n",
                                   snap_vol, snapped_atr);
                            fflush(stdout);
                        }
                    }

                    // ---- [3] Bar state corrupt on disk --------------------------
                    // Detection: load_indicators logged rejection (checked at startup)
                    // Fallback:  save_indicators now rejects flat state (3ad3dcd)
                    // Alert:     GUI "BAR STATE CORRUPT" set at load time (see startup block)
                    // (No runtime action needed — handled at startup)

                    // ---- [4] cTrader reconnect loop -----------------------------
                    // Handled inside [1] circuit breaker above.
                    // Additional: expose reconnect count on GUI
                    {
                        const bool ct_now_active = g_ctrader_depth.depth_active.load();
                        if (ct_now_active && s_last_fix_connected == 0)
                            s_last_fix_connected = now_s;
                        if (!ct_now_active && s_last_fix_connected > 0) {
                            ++s_fix_reconnect_n;
                            s_last_fix_connected = 0;
                            printf("[SYSTEM-ALERT] CTRADER_RECONNECT #%d\n", s_fix_reconnect_n);
                            fflush(stdout);
                            if (alert_msg.empty())
                                alert_msg = "FIX RECONNECT #" + std::to_string(s_fix_reconnect_n);
                        }
                    }

                    // ---- [5] Bracket window stall --------------------------------
                    // Detection: can_arm=1 but range=0.00 for >5 minutes
                    // Fallback:  efb68a8 fixed window starvation — this is a residual check
                    // Alert:     GUI "BRACKET STALLED Xmin"
                    {
                        const bool can_arm_gold = true;  // 24h -- no slot block
                        const double brk_range  = g_bracket_gold.current_range();
                        if (can_arm_gold && brk_range < 0.01 && sess_active) {
                            if (s_bracket_stall_since == 0) s_bracket_stall_since = now_s;
                            const int64_t stall_mins = (now_s - s_bracket_stall_since) / 60;
                            if (stall_mins >= 5) {
                                printf("[SYSTEM-ALERT] BRACKET_STALLED can_arm=1 range=0.00 for %lldmin\n",
                                       (long long)stall_mins);
                                fflush(stdout);
                                if (alert_msg.empty())
                                    alert_msg = "BRACKET STALLED " + std::to_string(stall_mins) + "min";
                            }
                        } else {
                            s_bracket_stall_since = 0;
                        }
                    }

                    // ---- [6] GoldFlow block reason on GUI -----------------------
                    // Detection: no [GOLD-FLOW] ENTRY for 45+ min during session
                    // Fallback:  none — gates are working as designed
                    // Alert:     GUI shows last gf_block_reason not just "NO TRADES"
                    if (sess_active) {
                        if (trade_count == s_last_trade_count) {
                            if (s_no_trade_since == 0) s_no_trade_since = now_s;
                            const int64_t idle_mins = (now_s - s_no_trade_since) / 60;
                            if (idle_mins >= 45) {
                                printf("[SYSTEM-ALERT] NO_TRADES session active %lldmin -- check gates\n",
                                       (long long)idle_mins);
                                fflush(stdout);
                                // Show last GF block reason on GUI rather than generic message
                                const std::string last_block = g_last_gf_block_reason.load()
                                    ? std::string(g_last_gf_block_reason.load()) : "UNKNOWN";
                                if (alert_msg.empty())
                                    alert_msg = "NO TRADES " + std::to_string(idle_mins)
                                                + "min [" + last_block + "]";
                            }
                        } else {
                            s_no_trade_since   = 0;
                            s_last_trade_count = trade_count;
                        }
                    }

                    // ---- [7] VIX feed dead --------------------------------------
                    // Detection: vix_is_stale() and session active
                    // Fallback:  ATR uses last known VIX or floor — already handled
                    // Alert:     GUI "VIX DEAD Xmin"
                    if (sess_active && g_macroDetector.vix_is_stale()) {
                        if (s_vix_dead_since == 0) s_vix_dead_since = now_s;
                        const int64_t vix_dead_mins = (now_s - s_vix_dead_since) / 60;
                        if (vix_dead_mins >= 10) {
                            printf("[SYSTEM-ALERT] VIX_DEAD no VIX.F tick for %lldmin -- ATR using last known\n",
                                   (long long)vix_dead_mins);
                            fflush(stdout);
                            if (alert_msg.empty())
                                alert_msg = "VIX DEAD " + std::to_string(vix_dead_mins) + "min";
                        }
                    } else {
                        s_vix_dead_since = 0;
                    }

                    // ---- [8] Consecutive loss pause -----------------------------
                    // Detection: pause_until > now on any engine state
                    // Fallback:  built-in — entries blocked until pause expires
                    // Alert:     GUI "CONSEC LOSS PAUSE"
                    {
                        bool any_paused = false;
                        // Check gold engine pause state via ledger
                        for (const auto& kv : g_engine_pause) {
                            if (kv.second > now_s) { any_paused = true; break; }
                        }
                        if (any_paused && alert_msg.empty())
                            alert_msg = "CONSEC LOSS PAUSE";
                    }

                    // ---- Write final alert to GUI (highest priority wins) --------
                    if (!alert_msg.empty()) {
                        g_telemetry.SetHealthAlert(alert_msg);
                    } else if (!any_critical) {
                        // Only clear if truly healthy: l2 live, bars seeded, events flowing
                        if (l2_live && gold_seeded && depth_now > 0)
                            g_telemetry.ClearHealthAlert();
                    }
                }
                // ================================================================
            }

            // ?? Depth fallback poll: if 264=5 was rejected mid-session ?????????
            // The type-Y handler does immediate re-sub via ssl, but as a belt-and-
            // suspenders check: if g_md_subscribed dropped false AND g_md_depth_fallback
            // is set (meaning Y was already handled), re-sub again in case the immediate
            // re-sub inside dispatch_fix was lost due to timing.
            if (g_quote_ready.load() && g_md_depth_fallback.load() &&
                !g_md_subscribed.load()) {
                const std::string fb_resub = fix_build_md_subscribe_all(g_quote_seq++);
                if (!fb_resub.empty()) {
                    SSL_write(ssl, fb_resub.c_str(), static_cast<int>(fb_resub.size()));
                    g_md_subscribed.store(true);
                    std::cout << "[OMEGA-DEPTH] Belt-and-suspenders re-sub at 264=1 complete.\n";
                    std::cout.flush();
                }
            }

            if (g_quote_ready.load() && g_cfg.enable_extended_symbols &&
                g_ext_md_refresh_needed.exchange(false)) {
                // SecurityList just populated ext symbol IDs (GER40/UK100/ESTX50 etc).
                // We MUST re-subscribe regardless of g_md_subscribed: the initial LOGON
                // subscription fired before SecurityList arrived so all ext IDs were 0
                // and were filtered out. Unsub first to avoid ALREADY_SUBSCRIBED reject,
                // then send a fresh subscription with the now-known IDs.
                const std::string unsub_r = fix_build_md_unsub_all(g_quote_seq++);
                if (!unsub_r.empty()) {
                    SSL_write(ssl, unsub_r.c_str(), static_cast<int>(unsub_r.size()));
                }
                Sleep(100);  // brief gap -- let broker process unsub before resub
                const std::string resub = fix_build_md_subscribe_all(g_quote_seq++);
                if (!resub.empty()) {
                    SSL_write(ssl, resub.c_str(), static_cast<int>(resub.size()));
                    g_md_subscribed.store(true);
                    std::cout << "[OMEGA] Re-subscribed ALL symbols with learned ext IDs"
                                 " (GER40/UK100/ESTX50/XAGUSD/EURUSD/BRENT/GBPUSD/AUDUSD/NZDUSD/USDJPY now included)\n";
                }
            }

            char buf[8192];
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)) - 1);
            if (n <= 0) {
                const int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    Sleep(1); continue;
                }
                // SO_RCVTIMEO timeout fires as SSL_ERROR_SYSCALL + WSAETIMEDOUT --
                // not a real disconnect, just no data in 200ms window
                if (err == SSL_ERROR_SYSCALL && WSAGetLastError() == WSAETIMEDOUT) {
                    continue;
                }
                // SSL_ERROR_ZERO_RETURN = clean TCP close by peer (BlackBull sent FIN).
                // This is the ~15min forced session termination. Log distinctly so it's
                // visible in startup_report analysis. proactive reconnect at 13min should
                // prevent this from ever appearing -- if it does, broker killed us early.
                if (err == SSL_ERROR_ZERO_RETURN) {
                    std::cout << "[OMEGA] Clean TCP close from broker (SSL_ZERO_RETURN)"
                              << " -- broker session limit hit, reconnecting\n";
                    fflush(stdout);
                } else {
                    std::cerr << "[OMEGA] SSL error " << err << " -- reconnecting\n";
                }
                break;
            }
            for (const auto& m : extract_messages(buf, n)) dispatch_fix(m, ssl);
            // If server sent us a Logout (ghost session), break immediately
            if (g_quote_logout_received.load()) {
                g_quote_logout_received.store(false);
                std::cout << "[OMEGA] Breaking read loop -- server logout received\n";
                break;
            }
        }

        // SO_SNDTIMEO (500ms, set in connect_ssl) caps SSL_write duration.
        // DO NOT set FIONBIO -- it causes SSL_write to return WANT_WRITE
        // immediately, silently dropping the unsub and logout messages.
        // ?? STEP 1: Snapshot all prices BEFORE unsubscribing ?????????????????
        // This is critical -- prices are cleared when market data is unsubscribed.
        // All position force-closes MUST use these snapshots, not live book.
        std::unordered_map<std::string,double> px_snap_bid, px_snap_ask;
        {
            std::lock_guard<std::mutex> lk(g_book_mtx);
            px_snap_bid = g_bids;
            px_snap_ask = g_asks;
        }
        std::cout << "[OMEGA] Price snapshot taken: " << px_snap_bid.size() << " symbols\n";

        // ?? STEP 2: Close ALL open positions on reconnect ?????????????????????
        // SHADOW mode: positions are local-only -- the broker has no open orders.
        //   Skip force-close so profitable trades survive reconnects/rebuilds.
        //   Engine state (trail_stage, mfe, sl) is preserved across reconnects.
        // LIVE mode: must close -- broker holds real positions and we may have
        //   lost order state on the server side. Close and let engines re-enter.
        // Force-close positions from a prior UTC day before proceeding.
        // Root cause of $844 recurring bug: TrendPullback opens in session N,
        // Omega redeploys at ~01:02 UTC (new day), SHADOW mode preserves the
        // position, it closes at 01:03 writing $844 into the new day's CSV.
        // Fix: on any reconnect, detect and close positions whose entry_ts is
        // from a previous UTC day -- they are stale and must not bleed into today.
        {
            const int64_t now_s_rc = static_cast<int64_t>(std::time(nullptr));
            struct tm ti_rc{}; gmtime_s(&ti_rc, &now_s_rc);
            const int today_yday_rc = ti_rc.tm_yday;
            const int today_year_rc = ti_rc.tm_year;
            auto is_stale = [&](int64_t entry_ts) -> bool {
                if (entry_ts <= 0) return false;
                struct tm te{}; gmtime_s(&te, &entry_ts);
                return (te.tm_yday != today_yday_rc || te.tm_year != today_year_rc);
            };
            auto stale_cb = [](const omega::TradeRecord& tr) {
                omega::TradeRecord t = tr;
                const double mult = tick_value_multiplier(t.symbol);
                t.pnl *= mult; t.mfe *= mult; t.mae *= mult;
                double cps = 0.0;
                { const std::string& s = t.symbol;
                  if (s=="XAUUSD"||s=="XAGUSD"||s=="EURUSD"||s=="GBPUSD"||
                      s=="AUDUSD"||s=="NZDUSD"||s=="USDJPY") cps = 3.0; }
                omega::apply_realistic_costs(t, cps, mult);
                // Prior-day positions must NOT enter today's ledger or P&L total.
                // They opened and closed in a prior session -- their PnL already
                // exists in yesterday's CSV. Recording them here double-counts into
                // today's daily_pnl (the $844 bug). Write CSV audit only.
                write_trade_close_logs(t);
                printf("[STALE-CLOSE] %s %s entry=%lld pnl=$%.2f -- prior-day position purged (audit only, not counted in today P&L)\n",
                       t.symbol.c_str(), t.engine.c_str(), (long long)t.entryTs, t.net_pnl);
                fflush(stdout);
            };
            // Get snapshot price for XAUUSD
            double xb_rc=0, xa_rc=0;
            { auto bi=px_snap_bid.find("XAUUSD"); if(bi!=px_snap_bid.end()) xb_rc=bi->second;
              auto ai=px_snap_ask.find("XAUUSD"); if(ai!=px_snap_ask.end()) xa_rc=ai->second; }
            if (xb_rc <= 0 || xa_rc <= 0) {
                std::lock_guard<std::mutex> lk(g_book_mtx);
                auto bi=g_bids.find("XAUUSD"); if(bi!=g_bids.end()) xb_rc=bi->second;
                auto ai=g_asks.find("XAUUSD"); if(ai!=g_asks.end()) xa_rc=ai->second;
            }
            const int64_t fc_ms_rc = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            // Helper: get price from snapshot map, fallback to entry price
            auto stale_px = [&](const char* sym, double& b, double& a) {
                const auto bi = px_snap_bid.find(sym); b = (bi != px_snap_bid.end()) ? bi->second : 0.0;
                const auto ai = px_snap_ask.find(sym); a = (ai != px_snap_ask.end()) ? ai->second : 0.0;
            };
            // BreakoutEngine stale purge
            auto stale_beng = [&](auto& eng, const char* sym) {
                if (!eng.pos.active || !is_stale(eng.pos.entry_ts)) return;
                double b=0,a=0; stale_px(sym,b,a);
                if (b<=0) { b=eng.pos.entry*0.9999; a=eng.pos.entry*1.0001; }
                eng.forceClose(b, a, "STALE_PRIOR_DAY", g_rtt_last, "", stale_cb);
                printf("[STALE-CLOSE] Purged prior-day Breakout %s\n", sym); fflush(stdout);
            };
            // BracketEngine stale purge
            auto stale_bracket = [&](auto& eng, const char* sym) {
                if (!eng.has_open_position() || !is_stale(eng.pos.entry_ts)) return;
                double b=0,a=0; stale_px(sym,b,a);
                if (b<=0) { b=1.0; a=1.0; }
                eng.forceClose(b, a, "STALE_PRIOR_DAY", g_rtt_last, "", stale_cb);
                printf("[STALE-CLOSE] Purged prior-day Bracket %s\n", sym); fflush(stdout);
            };
            // CrossAsset / NBM / ORB / VWAP / TrendPB stale purge (force_close(b,a,cb))
            // NOTE: pos_ is private on all CrossAsset engines so we cannot check entry_ts.
            // CrossAsset / NBM / ORB / VWAP stale purge.
            // All CA engine classes now expose open_entry_ts() so we can perform a real
            // UTC-day staleness check -- same logic as BreakoutEngine / BracketEngine.
            // Prior behaviour (always-close on reconnect) was killing profitable same-day
            // positions on normal mid-session reconnects (NOT the $844 scenario).
            auto stale_ca = [&](auto& eng, const char* sym) {
                if (!eng.has_open_position()) return;
                if (!is_stale(eng.open_entry_ts())) return;  // same-day position -- preserve it
                double b=0,a=0; stale_px(sym,b,a);
                if (b<=0) return;
                eng.force_close(b, a, stale_cb);
                printf("[STALE-CLOSE] Purged prior-day CA/NBM/ORB/VWAP %s\n", sym); fflush(stdout);
            };

            // -- Gold engines (original) --
            if (xb_rc > 0 && xa_rc > 0) {
                if (g_trend_pb_gold.has_open_position() && is_stale(g_trend_pb_gold.open_entry_ts()))
                    { g_trend_pb_gold.force_close(xb_rc, xa_rc, stale_cb);
                      std::cout << "[STALE-CLOSE] Purged prior-day TrendPullback-Gold\n"; }
                if (g_gold_flow.has_open_position() && is_stale(g_gold_flow.pos.entry_ts))
                    { g_gold_flow.force_close(xb_rc, xa_rc, fc_ms_rc, stale_cb);
                      std::cout << "[STALE-CLOSE] Purged prior-day GoldFlow\n"; }
                if (g_gold_flow_reload.has_open_position() && is_stale(g_gold_flow_reload.pos.entry_ts))
                    { g_gold_flow_reload.force_close(xb_rc, xa_rc, fc_ms_rc, stale_cb);
                    g_gold_flow_addon.force_close(xb_rc, xa_rc, fc_ms_rc, stale_cb);
                      std::cout << "[STALE-CLOSE] Purged prior-day GoldFlow-Reload\n"; }
                if (g_gold_stack.has_open_position() && is_stale(g_gold_stack.live_entry_ts()))
                    { g_gold_stack.force_close(xb_rc, xa_rc, g_rtt_last, stale_cb);
                      std::cout << "[STALE-CLOSE] Purged prior-day GoldStack\n"; }
                // LEStack: has_open_position() is true if either sub-engine is live.
                // live_entry_ts() not exposed -- LEStack max hold is ~2min so prior-day
                // carry is only possible after a very long disconnect. Close only if stale
                // by checking the internal SpreadDisloc engine's pos timestamp via force_close
                // guard: we always close LEStack on reconnect (same as before) -- its
                // positions are so short-lived that any surviving position IS stale.
                if (g_le_stack.has_open_position())
                    { g_le_stack.force_close_all(xb_rc, xa_rc, xb_rc, xa_rc, g_rtt_last, stale_cb);
                      std::cout << "[STALE-CLOSE] Purged LEStack on reconnect (max hold ~2min, always stale)\n"; }
            }
            // -- Breakout engines (all symbols) --
            stale_beng(g_eng_sp,     "US500.F");
            stale_beng(g_eng_nq,     "USTEC.F");
            stale_beng(g_eng_us30,   "DJ30.F");
            stale_beng(g_eng_nas100, "NAS100");
            stale_beng(g_eng_ger30,  "GER40");
            stale_beng(g_eng_uk100,  "UK100");
            stale_beng(g_eng_estx50, "ESTX50");
            stale_beng(g_eng_cl,     "USOIL.F");
            stale_beng(g_eng_brent,  "BRENT");
            stale_beng(g_eng_xag,    "XAGUSD");
            stale_beng(g_eng_eurusd, "EURUSD");
            stale_beng(g_eng_gbpusd, "GBPUSD");
            stale_beng(g_eng_audusd, "AUDUSD");
            stale_beng(g_eng_nzdusd, "NZDUSD");
            stale_beng(g_eng_usdjpy, "USDJPY");
            // -- Bracket engines --
            stale_bracket(g_bracket_gold,   "XAUUSD");
            stale_bracket(g_bracket_xag,    "XAGUSD");
            stale_bracket(g_bracket_sp,     "US500.F");
            stale_bracket(g_bracket_nq,     "USTEC.F");
            stale_bracket(g_bracket_us30,   "DJ30.F");
            stale_bracket(g_bracket_nas100, "NAS100");
            stale_bracket(g_bracket_ger30,  "GER40");
            stale_bracket(g_bracket_uk100,  "UK100");
            stale_bracket(g_bracket_estx50, "ESTX50");
            stale_bracket(g_bracket_eurusd, "EURUSD");
            stale_bracket(g_bracket_gbpusd, "GBPUSD");
            stale_bracket(g_bracket_audusd, "AUDUSD");
            stale_bracket(g_bracket_nzdusd, "NZDUSD");
            stale_bracket(g_bracket_usdjpy, "USDJPY");
            stale_bracket(g_bracket_brent,  "BRENT");
            // -- NBM / ORB / VWAP / TrendPB / CrossAsset --
            stale_ca(g_nbm_sp,          "US500.F");
            stale_ca(g_nbm_nq,          "USTEC.F");
            stale_ca(g_nbm_nas,         "NAS100");
            stale_ca(g_nbm_us30,        "DJ30.F");
            stale_ca(g_nbm_gold_london, "XAUUSD");
            stale_ca(g_nbm_oil_london,  "USOIL.F");
            stale_ca(g_orb_us,          "US500.F");
            stale_ca(g_orb_ger30,       "GER40");
            stale_ca(g_orb_uk100,       "UK100");
            stale_ca(g_orb_estx50,      "ESTX50");
            stale_ca(g_orb_silver,      "XAGUSD");
            stale_ca(g_vwap_rev_sp,     "US500.F");
            stale_ca(g_vwap_rev_nq,     "USTEC.F");
            stale_ca(g_vwap_rev_ger40,  "GER40");
            stale_ca(g_vwap_rev_eurusd, "EURUSD");
            // TrendPullback engines: all expose open_entry_ts() -- use explicit is_stale()
            // guard so we handle them exactly like g_trend_pb_gold (not always-close).
            // stale_ca() now also checks is_stale() but keeping these explicit is clearer.
            if (g_trend_pb_sp.has_open_position() && is_stale(g_trend_pb_sp.open_entry_ts())) {
                double b=0,a=0; stale_px("US500.F",b,a);
                if (b>0) { g_trend_pb_sp.force_close(b,a,stale_cb);
                           printf("[STALE-CLOSE] Purged prior-day TrendPullback-SP\n"); fflush(stdout); } }
            if (g_trend_pb_nq.has_open_position() && is_stale(g_trend_pb_nq.open_entry_ts())) {
                double b=0,a=0; stale_px("USTEC.F",b,a);
                if (b>0) { g_trend_pb_nq.force_close(b,a,stale_cb);
                           printf("[STALE-CLOSE] Purged prior-day TrendPullback-NQ\n"); fflush(stdout); } }
            if (g_trend_pb_ger40.has_open_position() && is_stale(g_trend_pb_ger40.open_entry_ts())) {
                double b=0,a=0; stale_px("GER40",b,a);
                if (b>0) { g_trend_pb_ger40.force_close(b,a,stale_cb);
                           printf("[STALE-CLOSE] Purged prior-day TrendPullback-GER40\n"); fflush(stdout); } }
            stale_ca(g_ca_esnq,         "US500.F");
            stale_ca(g_ca_eia_fade,     "USOIL.F");
            stale_ca(g_ca_brent_wti,    "USOIL.F");
            stale_ca(g_ca_carry_unwind, "USDJPY");
            // g_ca_fx_cascade: legs (GBP/AUD/NZD) are private -- entry_ts inaccessible.
            // FX cascade positions are short-lived scalps (max hold ~5min) so prior-day
            // carry is impossible in practice. Omitted intentionally.
        }

        const bool do_reconnect_close = (g_cfg.mode == "LIVE");
        if (do_reconnect_close) {
            std::cout << "[OMEGA] LIVE mode -- closing all positions before reconnect\n";
        } else {
            std::cout << "[OMEGA] SHADOW mode -- preserving positions across reconnect\n";
        }
        if (do_reconnect_close)
        { // SHADOW: this entire block is skipped
        // Helper: get price from snapshot, fallback to live book, fallback to entry
        auto snap_px = [&](const char* sym, double& b, double& a) {
            b = 0.0; a = 0.0;
            const auto bi = px_snap_bid.find(sym); if (bi != px_snap_bid.end()) b = bi->second;
            const auto ai = px_snap_ask.find(sym); if (ai != px_snap_ask.end()) a = ai->second;
            if (b <= 0.0 || a <= 0.0) {
                std::lock_guard<std::mutex> lk(g_book_mtx);
                const auto bi2 = g_bids.find(sym); if (bi2 != g_bids.end()) b = bi2->second;
                const auto ai2 = g_asks.find(sym); if (ai2 != g_asks.end()) a = ai2->second;
            }
        };
        auto shutdown_cb = [](const omega::TradeRecord& tr) {
            // Shadow-aware guard (2026-04-21): when an engine flags itself as shadow
            // (tr.shadow == true) we must NOT call handle_closed_trade (which would
            // pollute the live ledger, daily_pnl, risk state, crowding/WFO/TOD gates,
            // and auto-disable tracker) and we must NOT call send_live_order (which
            // would send a real broker order to close a phantom position that never
            // existed at the broker).
            //
            // Note: this whole do_reconnect_close block is already gated on
            // g_cfg.mode == "LIVE" above, so reaching shutdown_cb means global mode
            // is LIVE. The per-engine shadow flag is the only path that can slip
            // through the outer gate.
            //
            // handle_closed_trade itself has its own shadow short-circuit as the
            // central guard (belt), but we also check here (braces) specifically to
            // stop send_live_order from firing on shadow records.
            if (tr.shadow) {
                printf("[SHUTDOWN-CB-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                       tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                fflush(stdout);
                return;
            }
            handle_closed_trade(tr);
            send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
        };

        // Close every engine type
        auto fc_snap = [&](auto& eng, const char* sym) {
            if (!eng.pos.active) return;
            double bid = 0.0, ask = 0.0; snap_px(sym, bid, ask);
            if (bid <= 0.0) { bid = eng.pos.entry * 0.9999; ask = eng.pos.entry * 1.0001; }
            eng.forceClose(bid, ask, "SHUTDOWN", g_rtt_last,
                g_macroDetector.regime().c_str(), shutdown_cb);
            std::cout << "[OMEGA-SHUTDOWN] Closed " << sym << " position\n";
        };
        auto fc_bracket_snap = [&](auto& beng, const char* sym) {
            if (!beng.has_open_position()) return;
            double b = 0.0, a = 0.0; snap_px(sym, b, a);
            if (b <= 0.0) { b = 1.0; a = 1.0; }
            beng.forceClose(b, a, "SHUTDOWN", g_rtt_last, "", shutdown_cb);
            std::cout << "[OMEGA-SHUTDOWN] Closed bracket " << sym << "\n";
        };

        // Breakout engines
        fc_snap(g_eng_sp,     "US500.F"); fc_snap(g_eng_nq,    "USTEC.F");
        fc_snap(g_eng_cl,     "USOIL.F"); fc_snap(g_eng_us30,  "DJ30.F");
        fc_snap(g_eng_nas100, "NAS100");  fc_snap(g_eng_ger30, "GER40");
        fc_snap(g_eng_uk100,  "UK100");   fc_snap(g_eng_estx50,"ESTX50");
        fc_snap(g_eng_xag,    "XAGUSD"); fc_snap(g_eng_eurusd, "EURUSD");
        fc_snap(g_eng_gbpusd, "GBPUSD"); fc_snap(g_eng_audusd, "AUDUSD");
        fc_snap(g_eng_nzdusd, "NZDUSD"); fc_snap(g_eng_usdjpy, "USDJPY");
        fc_snap(g_eng_brent,  "BRENT");

        // Gold/Silver bracket engines
        { double b=0,a=0; snap_px("XAUUSD",b,a);
          if(b<=0){b=1;a=1;}
          g_bracket_gold.forceClose(b,a,"SHUTDOWN",g_rtt_last,"",shutdown_cb); }
        { double b=0,a=0; snap_px("XAGUSD",b,a);
          if(b<=0){b=1;a=1;}
          g_bracket_xag.forceClose(b,a,"SHUTDOWN",g_rtt_last,"",shutdown_cb); }

        // Index/FX/Oil bracket engines
        fc_bracket_snap(g_bracket_sp,     "US500.F");
        fc_bracket_snap(g_bracket_nq,     "USTEC.F");
        fc_bracket_snap(g_bracket_us30,   "DJ30.F");
        fc_bracket_snap(g_bracket_nas100, "NAS100");
        fc_bracket_snap(g_bracket_ger30,  "GER40");
        fc_bracket_snap(g_bracket_uk100,  "UK100");
        fc_bracket_snap(g_bracket_estx50, "ESTX50");
        fc_bracket_snap(g_bracket_brent,  "BRENT");
        fc_bracket_snap(g_bracket_eurusd, "EURUSD");
        fc_bracket_snap(g_bracket_gbpusd, "GBPUSD");
        fc_bracket_snap(g_bracket_audusd, "AUDUSD");
        fc_bracket_snap(g_bracket_nzdusd, "NZDUSD");
        fc_bracket_snap(g_bracket_usdjpy, "USDJPY");

        // Gold stack, flow, latency
        { double b=0,a=0; snap_px("XAUUSD",b,a);
          if(b<=0){b=1;a=1;}
          const int64_t now_ms_sd = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
          g_gold_stack.force_close(b,a,g_rtt_last,shutdown_cb);
          g_gold_flow.force_close(b,a,now_ms_sd,shutdown_cb);
          g_gold_flow_reload.force_close(b,a,now_ms_sd,shutdown_cb);
          g_gold_flow_addon.force_close(b,a,now_ms_sd,shutdown_cb); }
        { double b=0,a=0; snap_px("XAUUSD",b,a);
          if(b<=0){b=1;a=1;}
          double s_bid=0,s_ask=0; snap_px("XAGUSD",s_bid,s_ask);
          if(s_bid<=0){s_bid=b*0.0185;s_ask=a*0.0185;}
          g_le_stack.force_close_all(b,a,s_bid,s_ask,g_rtt_last,
              [&](const omega::TradeRecord& tr){shutdown_cb(tr);}); }

        // Cross-asset engines (VWAP, TrendPB, ORB, Carry, etc.)
        { double b=0,a=0;
          snap_px("US500.F",b,a); if(b>0&&a>0){g_ca_esnq.force_close(b,a,shutdown_cb);g_orb_us.force_close(b,a,shutdown_cb);g_vwap_rev_sp.force_close(b,a,shutdown_cb);g_nbm_sp.force_close(b,a,shutdown_cb);g_trend_pb_sp.force_close(b,a,shutdown_cb);}
          snap_px("USTEC.F",b,a); if(b>0&&a>0){g_vwap_rev_nq.force_close(b,a,shutdown_cb);g_nbm_nq.force_close(b,a,shutdown_cb);g_trend_pb_nq.force_close(b,a,shutdown_cb);}
          snap_px("NAS100",b,a);  if(b>0&&a>0){g_nbm_nas.force_close(b,a,shutdown_cb);}
          snap_px("DJ30.F",b,a);  if(b>0&&a>0){g_nbm_us30.force_close(b,a,shutdown_cb);}
          snap_px("EURUSD",b,a);  if(b>0&&a>0){g_vwap_rev_eurusd.force_close(b,a,shutdown_cb);}
          snap_px("GER40",b,a);   if(b>0&&a>0){g_orb_ger30.force_close(b,a,shutdown_cb);g_vwap_rev_ger40.force_close(b,a,shutdown_cb);g_trend_pb_ger40.force_close(b,a,shutdown_cb);}
          snap_px("XAUUSD",b,a);  if(b>0&&a>0){g_trend_pb_gold.force_close(b,a,shutdown_cb);g_nbm_gold_london.force_close(b,a,shutdown_cb);}
          snap_px("USOIL.F",b,a); if(b>0&&a>0){g_nbm_oil_london.force_close(b,a,shutdown_cb);}  // London NBM oil
          snap_px("XAGUSD",b,a);  if(b>0&&a>0){g_orb_silver.force_close(b,a,shutdown_cb);}
          snap_px("UK100",b,a);   if(b>0&&a>0){g_orb_uk100.force_close(b,a,shutdown_cb);}
          snap_px("ESTX50",b,a);  if(b>0&&a>0){g_orb_estx50.force_close(b,a,shutdown_cb);}
          snap_px("USOIL.F",b,a); if(b>0&&a>0){g_ca_eia_fade.force_close(b,a,shutdown_cb);g_ca_brent_wti.force_close(b,a,shutdown_cb);}
          snap_px("USDJPY",b,a);  if(b>0&&a>0){g_ca_carry_unwind.force_close(b,a,shutdown_cb);}
          { double ab=0,aa=0,nb=0,na=0;
            snap_px("GBPUSD",b,a); snap_px("AUDUSD",ab,aa); snap_px("NZDUSD",nb,na);
            if(b>0&&a>0){g_ca_fx_cascade.force_close(b,a,shutdown_cb);}
            if(ab>0&&aa>0){g_ca_fx_cascade.force_close_audusd(ab,aa,shutdown_cb);}
            if(nb>0&&na>0){g_ca_fx_cascade.force_close_nzdusd(nb,na,shutdown_cb);} }
        }
        std::cout << "[OMEGA-SHUTDOWN] All positions closed\n";
        } // end do_reconnect_close (LIVE mode only)

        // ?? STEP 3: Unsubscribe and logout ??????????????????????????????????????
        // SHADOW mode: do NOT close positions -- they survive the reconnect.
        //   Engine state (entry, sl, trail, mfe) is all in-process memory and
        //   persists across the TCP disconnect/reconnect cycle. We only unsub
        //   and logout the FIX session, not the engine positions.
        // LIVE mode: close all positions before unsubscribing -- broker holds
        //   real orders and we cannot guarantee state recovery after reconnect.
        if (do_reconnect_close) {
            std::unordered_map<std::string,double> shut_bid, shut_ask;
            { std::lock_guard<std::mutex> lk(g_book_mtx); shut_bid = g_bids; shut_ask = g_asks; }
            std::cout << "[OMEGA-SHUTDOWN] Snapped " << shut_bid.size() << " prices, closing all positions\n";

            auto get_px = [&](const char* s, double& b, double& a) {
                const auto bi = shut_bid.find(s); b = (bi != shut_bid.end()) ? bi->second : 0.0;
                const auto ai = shut_ask.find(s); a = (ai != shut_ask.end()) ? ai->second : 0.0;
                // If snapshot empty, try live book
                if (b <= 0.0 || a <= 0.0) {
                    std::lock_guard<std::mutex> lk(g_book_mtx);
                    const auto bi2 = g_bids.find(s); if (bi2 != g_bids.end()) b = bi2->second;
                    const auto ai2 = g_asks.find(s); if (ai2 != g_asks.end()) a = ai2->second;
                }
                // Absolute last resort: synthesize from position entry (prevents silent skip)
                if (b <= 0.0) { b = 1.0; a = 1.01; }
            };
            auto scb = [](const omega::TradeRecord& tr) {
                // Shadow-aware guard (2026-04-21): mirror of shutdown_cb. Same
                // rationale -- per-engine shadow_mode=true while global mode is LIVE
                // can slip a shadow TradeRecord into this path. Both handle_closed_trade
                // and send_live_order must be blocked for shadow records.
                if (tr.shadow) {
                    printf("[SCB-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                           tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                    fflush(stdout);
                    return;
                }
                handle_closed_trade(tr);
                send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
            };

            // Breakout engines
            auto sfc = [&](auto& e, const char* s) {
                if (!e.pos.active) return;
                double b,a; get_px(s,b,a);
                e.forceClose(b,a,"SHUTDOWN",g_rtt_last,g_macroDetector.regime().c_str(),scb);
                printf("[OMEGA-SHUTDOWN] Closed %s\n",s);
            };
            sfc(g_eng_sp,"US500.F"); sfc(g_eng_nq,"USTEC.F"); sfc(g_eng_cl,"USOIL.F");
            sfc(g_eng_us30,"DJ30.F"); sfc(g_eng_nas100,"NAS100");
            sfc(g_eng_ger30,"GER40"); sfc(g_eng_uk100,"UK100"); sfc(g_eng_estx50,"ESTX50");
            sfc(g_eng_xag,"XAGUSD"); sfc(g_eng_eurusd,"EURUSD"); sfc(g_eng_gbpusd,"GBPUSD");
            sfc(g_eng_audusd,"AUDUSD"); sfc(g_eng_nzdusd,"NZDUSD"); sfc(g_eng_usdjpy,"USDJPY");
            sfc(g_eng_brent,"BRENT");

            // Bracket engines
            auto sbk = [&](auto& e, const char* s) {
                if (!e.has_open_position()) return;
                double b,a; get_px(s,b,a);
                e.forceClose(b,a,"SHUTDOWN",g_rtt_last,"",scb);
                printf("[OMEGA-SHUTDOWN] Closed bracket %s\n",s);
            };
            { double b,a; get_px("XAUUSD",b,a); g_bracket_gold.forceClose(b,a,"SHUTDOWN",g_rtt_last,"",scb); }
            { double b,a; get_px("XAGUSD",b,a); g_bracket_xag.forceClose(b,a,"SHUTDOWN",g_rtt_last,"",scb); }
            sbk(g_bracket_sp,"US500.F"); sbk(g_bracket_nq,"USTEC.F");
            sbk(g_bracket_us30,"DJ30.F"); sbk(g_bracket_nas100,"NAS100");
            sbk(g_bracket_ger30,"GER40"); sbk(g_bracket_uk100,"UK100");
            sbk(g_bracket_estx50,"ESTX50"); sbk(g_bracket_brent,"BRENT");
            sbk(g_bracket_eurusd,"EURUSD"); sbk(g_bracket_gbpusd,"GBPUSD");
            sbk(g_bracket_audusd,"AUDUSD"); sbk(g_bracket_nzdusd,"NZDUSD");
            sbk(g_bracket_usdjpy,"USDJPY");

            // Gold stack + flow + latency
            { double b,a; get_px("XAUUSD",b,a);
              const int64_t now_ms_scb = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
              g_gold_stack.force_close(b,a,g_rtt_last,scb);
              g_gold_flow.force_close(b,a,now_ms_scb,scb);
              g_gold_flow_reload.force_close(b,a,now_ms_scb,scb);
              g_gold_flow_addon.force_close(b,a,now_ms_scb,scb); }
            { double b,a,sb,sa; get_px("XAUUSD",b,a); get_px("XAGUSD",sb,sa);
              g_le_stack.force_close_all(b,a,sb,sa,g_rtt_last,[&](const omega::TradeRecord& tr){scb(tr);}); }

            // Cross-asset: VWAP, TrendPB, ORB, Carry, FxCascade
            { double b,a;
              get_px("US500.F",b,a);  g_ca_esnq.force_close(b,a,scb); g_orb_us.force_close(b,a,scb); g_vwap_rev_sp.force_close(b,a,scb); g_nbm_sp.force_close(b,a,scb);
              get_px("USTEC.F",b,a);  g_vwap_rev_nq.force_close(b,a,scb); g_nbm_nq.force_close(b,a,scb);
              get_px("NAS100",b,a);   g_nbm_nas.force_close(b,a,scb);
              get_px("DJ30.F",b,a);   g_nbm_us30.force_close(b,a,scb);
              get_px("EURUSD",b,a);   g_vwap_rev_eurusd.force_close(b,a,scb);
              get_px("GER40",b,a);    g_orb_ger30.force_close(b,a,scb); g_vwap_rev_ger40.force_close(b,a,scb); g_trend_pb_ger40.force_close(b,a,scb);
              get_px("XAUUSD",b,a);   g_trend_pb_gold.force_close(b,a,scb); g_nbm_gold_london.force_close(b,a,scb);
              get_px("USOIL.F",b,a);  g_nbm_oil_london.force_close(b,a,scb);  // London NBM oil
              get_px("XAGUSD",b,a);   g_orb_silver.force_close(b,a,scb);
              get_px("UK100",b,a);    g_orb_uk100.force_close(b,a,scb);
              get_px("ESTX50",b,a);   g_orb_estx50.force_close(b,a,scb);
              get_px("USOIL.F",b,a);  g_ca_eia_fade.force_close(b,a,scb); g_ca_brent_wti.force_close(b,a,scb);
              get_px("USDJPY",b,a);   g_ca_carry_unwind.force_close(b,a,scb);
              double gb,ga,ab,aa,nb,na;
              get_px("GBPUSD",gb,ga); get_px("AUDUSD",ab,aa); get_px("NZDUSD",nb,na);
              g_ca_fx_cascade.force_close(gb,ga,scb);
              g_ca_fx_cascade.force_close_audusd(ab,aa,scb);
              g_ca_fx_cascade.force_close_nzdusd(nb,na,scb);
            }
            std::cout << "[OMEGA-SHUTDOWN] All positions closed before disconnect\n";
        } // end LIVE mode position close
        else {
            std::cout << "[OMEGA-SHADOW] Positions preserved across reconnect -- skipping force-close\n";
        }
        // ?????????????????????????????????????????????????????????????????????
        if (g_quote_ready.load()) {
            const std::string unsub_all = fix_build_md_unsub_all(g_quote_seq++);
            SSL_write(ssl, unsub_all.c_str(), static_cast<int>(unsub_all.size()));
            std::cout << "[OMEGA] Unsubscribed market data before disconnect\n";
        }
        {
            const std::string lo = fix_build_logout(g_quote_seq++, "QUOTE");
            SSL_write(ssl, lo.c_str(), static_cast<int>(lo.size()));
            std::cout << "[OMEGA] Logout sent\n";
        }

        g_quote_ready.store(false);
        g_md_subscribed.store(false);
        g_connected_since.store(0);
        // Old force-close block removed -- positions already closed above in STEP 2
        auto fc = [](auto& eng, const char* sym) {
            if (!eng.pos.active) return;
            double bid = 0.0, ask = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find(sym); if (bi != g_bids.end()) bid = bi->second;
              const auto ai = g_asks.find(sym); if (ai != g_asks.end()) ask = ai->second; }
            if (bid > 0.0 && ask > 0.0)
                eng.forceClose(bid, ask, "DISCONNECT", g_rtt_last,
                    g_macroDetector.regime().c_str(),
                    [](const omega::TradeRecord& tr) {
                        // Shadow-aware guard (2026-04-21): see shutdown_cb rationale.
                        if (tr.shadow) {
                            printf("[FC-DISCONNECT-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                                   tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                            fflush(stdout);
                            return;
                        }
                        handle_closed_trade(tr);
                        // Send market close -- broker doesn't know we're disconnecting
                        send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
                    });
        };
        // Old force-close calls removed -- all positions closed in STEP 2 above
        (void)fc;  // suppress unused warning
        // Force-close bracket engines and cross-asset -- LIVE mode only
        // SHADOW: positions are local state only, no broker-side orders to close.
        if (do_reconnect_close) {
        // Force-close bracket engines -- look up current prices from book
        {
            double bxag_bid = 0.0, bxag_ask = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find("XAGUSD"); if (bi != g_bids.end()) bxag_bid = bi->second;
              const auto ai = g_asks.find("XAGUSD"); if (ai != g_asks.end()) bxag_ask = ai->second; }
            if (bxag_bid > 0.0 && bxag_ask > 0.0)
                g_bracket_xag.forceClose(bxag_bid, bxag_ask, "FORCE_CLOSE", g_rtt_last, "",
                    [](const omega::TradeRecord& tr) {
                        // Shadow-aware guard (2026-04-21): see shutdown_cb rationale.
                        if (tr.shadow) {
                            printf("[BRACKET-XAG-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                                   tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                            fflush(stdout);
                            return;
                        }
                        handle_closed_trade(tr);
                        send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
                    });
        }
        {
            double bgld_bid = 0.0, bgld_ask = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find("XAUUSD"); if (bi != g_bids.end()) bgld_bid = bi->second;
              const auto ai = g_asks.find("XAUUSD"); if (ai != g_asks.end()) bgld_ask = ai->second; }
            if (bgld_bid > 0.0 && bgld_ask > 0.0)
                g_bracket_gold.forceClose(bgld_bid, bgld_ask, "FORCE_CLOSE", g_rtt_last, "",
                    [](const omega::TradeRecord& tr) {
                        // Shadow-aware guard (2026-04-21): see shutdown_cb rationale.
                        if (tr.shadow) {
                            printf("[BRACKET-GOLD-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                                   tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                            fflush(stdout);
                            return;
                        }
                        handle_closed_trade(tr);
                        send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
                    });
        }
        fc(g_eng_audusd, "AUDUSD"); fc(g_eng_nzdusd, "NZDUSD"); fc(g_eng_usdjpy, "USDJPY");
        fc(g_eng_brent, "BRENT");
        // Force-close all new bracket engines
        auto fc_bracket = [](auto& beng, const char* sym_fc) {
            if (!beng.has_open_position()) return;
            double b = 0.0, a = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find(sym_fc); if (bi != g_bids.end()) b = bi->second;
              const auto ai = g_asks.find(sym_fc); if (ai != g_asks.end()) a = ai->second; }
            if (b > 0.0 && a > 0.0)
                beng.forceClose(b, a, "FORCE_CLOSE", g_rtt_last, "",
                    [](const omega::TradeRecord& tr) {
                        // Shadow-aware guard (2026-04-21): see shutdown_cb rationale.
                        if (tr.shadow) {
                            printf("[FC-BRACKET-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                                   tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                            fflush(stdout);
                            return;
                        }
                        handle_closed_trade(tr);
                        // Send market close -- broker doesn't know we disconnected.
                        // Matches gold/xag bracket behaviour on disconnect.
                        send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
                    });
        };
        fc_bracket(g_bracket_sp,      "US500.F");
        fc_bracket(g_bracket_nq,      "USTEC.F");
        fc_bracket(g_bracket_us30,    "DJ30.F");
        fc_bracket(g_bracket_nas100,  "NAS100");
        fc_bracket(g_bracket_ger30,   "GER40");
        fc_bracket(g_bracket_uk100,   "UK100");
        fc_bracket(g_bracket_estx50,  "ESTX50");
        fc_bracket(g_bracket_brent,   "BRENT");
        fc_bracket(g_bracket_eurusd,  "EURUSD");
        fc_bracket(g_bracket_gbpusd,  "GBPUSD");
        fc_bracket(g_bracket_audusd,  "AUDUSD");
        fc_bracket(g_bracket_nzdusd,  "NZDUSD");
        fc_bracket(g_bracket_usdjpy,  "USDJPY");
        // Cross-asset engine force-close
        {
            std::function<void(const omega::TradeRecord&)> ca_cb =
                [](const omega::TradeRecord& tr) {
                    // Shadow-aware guard (2026-04-21): see shutdown_cb/scb rationale.
                    if (tr.shadow) {
                        printf("[CA-CB-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                               tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                        fflush(stdout);
                        return;
                    }
                    handle_closed_trade(tr);
                    send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
                };
            auto ca_get_px = [](const char* s, double& b, double& a) {
                b = 0.0; a = 0.0;
                std::lock_guard<std::mutex> lk(g_book_mtx);
                const auto bi = g_bids.find(s); if (bi != g_bids.end()) b = bi->second;
                const auto ai = g_asks.find(s); if (ai != g_asks.end()) a = ai->second;
            };
            double ca_b = 0.0, ca_a = 0.0;
            ca_get_px("US500.F", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_ca_esnq.force_close(ca_b, ca_a, ca_cb); g_orb_us.force_close(ca_b, ca_a, ca_cb); }
            ca_get_px("USOIL.F", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_ca_eia_fade.force_close(ca_b, ca_a, ca_cb); g_ca_brent_wti.force_close(ca_b, ca_a, ca_cb); }
            ca_get_px("GBPUSD", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_ca_fx_cascade.force_close(ca_b, ca_a, ca_cb); }
            // FxCascade AUD/NZD legs -- force-close with each pair's own price
            { double aud_b = 0.0, aud_a = 0.0; ca_get_px("AUDUSD", aud_b, aud_a);
              if (aud_b > 0.0 && aud_a > 0.0) g_ca_fx_cascade.force_close_audusd(aud_b, aud_a, ca_cb); }
            { double nzd_b = 0.0, nzd_a = 0.0; ca_get_px("NZDUSD", nzd_b, nzd_a);
              if (nzd_b > 0.0 && nzd_a > 0.0) g_ca_fx_cascade.force_close_nzdusd(nzd_b, nzd_a, ca_cb); }
            ca_get_px("USDJPY", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_ca_carry_unwind.force_close(ca_b, ca_a, ca_cb); }
            ca_get_px("GER40", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) {
                g_orb_ger30.force_close(ca_b, ca_a, ca_cb);
                g_vwap_rev_ger40.force_close(ca_b, ca_a, ca_cb);
                g_trend_pb_ger40.force_close(ca_b, ca_a, ca_cb);
            }
            ca_get_px("XAGUSD", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_orb_silver.force_close(ca_b, ca_a, ca_cb); }
            ca_get_px("UK100", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_orb_uk100.force_close(ca_b, ca_a, ca_cb); }
            ca_get_px("ESTX50", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_orb_estx50.force_close(ca_b, ca_a, ca_cb); }
            ca_get_px("US500.F", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_vwap_rev_sp.force_close(ca_b, ca_a, ca_cb); }
            ca_get_px("USTEC.F", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_vwap_rev_nq.force_close(ca_b, ca_a, ca_cb); }
            ca_get_px("EURUSD", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_vwap_rev_eurusd.force_close(ca_b, ca_a, ca_cb); }
            // FxCascade: force_close() closes all three legs (GBPUSD/AUDUSD/NZDUSD).
            // Each leg uses the GBPUSD price as approximation -- acceptable for emergency
            // disconnect close since the pairs are highly correlated and positions are 0.01 lot.
            // The individual on_tick_audusd/nzdusd calls above already handle manage() on each tick.
            // XAUUSD TrendPullback
            ca_get_px("XAUUSD", ca_b, ca_a);
            if (ca_b > 0.0 && ca_a > 0.0) { g_trend_pb_gold.force_close(ca_b, ca_a, ca_cb); }
        }
        // Force-close GoldEngineStack
        {
            double g_bid = 0.0, g_ask = 0.0, s_bid = 0.0, s_ask = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find("XAUUSD"); if (bi != g_bids.end()) g_bid = bi->second;
              const auto ai = g_asks.find("XAUUSD"); if (ai != g_asks.end()) g_ask = ai->second;
              const auto sbi = g_bids.find("XAGUSD"); if (sbi != g_bids.end()) s_bid = sbi->second;
              const auto sai = g_asks.find("XAGUSD"); if (sai != g_asks.end()) s_ask = sai->second; }
            if (g_bid > 0.0 && g_ask > 0.0) {
                omega::gold::GoldEngineStack::CloseCallback gold_fc_cb =
                    [](const omega::TradeRecord& tr) {
                        // Shadow-aware guard (2026-04-21): see shutdown_cb rationale.
                        if (tr.shadow) {
                            printf("[GOLD-FC-CB-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                                   tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                            fflush(stdout);
                            return;
                        }
                        handle_closed_trade(tr);
                        send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
                    };
                g_gold_stack.force_close(g_bid, g_ask, g_rtt_last, gold_fc_cb);
                // Force-close GoldFlowEngine
                {
                    const int64_t fc_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    g_gold_flow.force_close(g_bid, g_ask, fc_now_ms, gold_fc_cb);
                }
                // Force-close latency edge engines
                omega::latency::LatencyEdgeStack::CloseCb le_cb =
                    [](const omega::TradeRecord& tr) {
                        // Shadow-aware guard (2026-04-21): see shutdown_cb rationale.
                        if (tr.shadow) {
                            printf("[LE-CB-SHADOW] %s %s size=%.4f exit=%.5f -- SHADOW trade, skipping handle_closed_trade and send_live_order\n",
                                   tr.symbol.c_str(), tr.side.c_str(), tr.size, tr.exitPrice);
                            fflush(stdout);
                            return;
                        }
                        handle_closed_trade(tr);
                        send_live_order(tr.symbol, tr.side == "SHORT", tr.size, tr.exitPrice);
                    };
                g_le_stack.force_close_all(g_bid, g_ask,
                    s_bid > 0.0 ? s_bid : g_bid * 0.0185,
                    s_ask > 0.0 ? s_ask : g_ask * 0.0185,
                    g_rtt_last, le_cb);
            }
        }

        } // end do_reconnect_close (second block -- LIVE mode only)

        Sleep(150);  // let kernel flush logout/unsub to wire
        // Close socket FIRST -- SSL_free finds dead socket, returns immediately.
        // quiet_shutdown is set on the SSL_CTX so no close-notify handshake occurs.
        closesocket(static_cast<SOCKET>(sock));
        SSL_free(ssl);
        g_telemetry.UpdateFixStatus("DISCONNECTED", "DISCONNECTED", 0, 0);

        // Reconnect after any disconnect.
        // Extra 2s when server sent Logout (ghost session).
        if (g_quote_logout_received.exchange(false) && g_running.load()) {
            std::cout << "[OMEGA] Ghost session -- waiting 2s for server to clear old session\n";
            for (int i = 0; i < 200 && g_running.load(); ++i) Sleep(10);
        }
        for (int i = 0; i < backoff_ms / 10 && g_running.load(); ++i) Sleep(10);
        backoff_ms = std::min(backoff_ms * 2, max_backoff);
    }
}

// ?????????????????????????????????????????????????????????????????????????????
// main
// ?????????????????????????????????????????????????????????????????????????????
