#pragma once
// omega_main.hpp -- extracted from main.cpp
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

int main(int argc, char* argv[])
{
    g_singleton_mutex = CreateMutexA(NULL, TRUE, "Global\\Omega_Breakout_System");
    if (!g_singleton_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cout << "[OMEGA] ALREADY RUNNING -- another Omega instance holds the mutex. Exiting.\n";
        std::cerr << "[OMEGA] ALREADY RUNNING -- another Omega instance holds the mutex. Exiting.\n";
        std::cout.flush(); std::cerr.flush();
        if (g_singleton_mutex) { CloseHandle(g_singleton_mutex); g_singleton_mutex = nullptr; }
        Sleep(2000);  // keep window open long enough to read
        return 1;
    }

    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    std::cout << "\033[1;36m"
              << "=======================================================\n"
              << "  OMEGA  |  Commodities & Indices  |  Breakout System  \n"
              << "=======================================================\n"
              << "  Build:   " << OMEGA_VERSION << "  (" << OMEGA_BUILT << ")\n"
              << "  Commit:  " << OMEGA_COMMIT  << "\n"
              << "=======================================================\n"
              << "\033[0m";
    // Print to stderr for service log
    std::fprintf(stderr, "[OMEGA] version=%s built=%s\n", OMEGA_VERSION, OMEGA_BUILT);

    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);  // handles X button, CTRL_CLOSE, shutdown

    // Resolve config path: explicit arg > cwd\omega_config.ini > config\omega_config.ini
    std::string cfg_path = "omega_config.ini";
    if (argc > 1) {
        cfg_path = argv[1];
    } else {
        std::ifstream test_cwd("omega_config.ini");
        if (!test_cwd.is_open()) cfg_path = "config\\omega_config.ini";
    }
    load_config(cfg_path);
    sanitize_config();
    apply_shadow_research_profile();
    // ── Engine configuration, wiring, and startup ──────────────────────────
    // All engine config, callback wiring, state loading, and startup checks
    // are in engine_init.hpp. main() is responsible for process lifecycle only.
    init_engines(cfg_path);

    std::cout << "[OMEGA] FIX loop starting -- " << g_cfg.mode << " mode\n";

    // Push log to git on every startup so remote reads are never stale after restart.
    // Fire-and-forget -- does not block startup.
    std::system("cmd /c start /min powershell -WindowStyle Hidden"
                " -ExecutionPolicy Bypass"
                " -File C:\\Omega\\push_log.ps1 -RepoRoot C:\\Omega");

    // =========================================================================
    // STARTUP VERIFICATION THREAD
    // Checks all critical systems within 120s of launch.
    // Writes C:\Omega\logs\startup_status.txt:
    //   "OK"   = all systems go
    //   "FAIL: <reason>" = something is broken
    // DEPLOY_OMEGA.ps1 reads this file and aborts + alerts if FAIL.
    // =========================================================================
    std::thread([](){
        const std::string status_path = log_root_dir() + "/startup_status.txt";
        { std::ofstream f(status_path); f << "STARTING\n"; }

        const auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&]{ return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count(); };

        // ANSI: bold green tick, bold red cross
        static const char* TICK  = "\033[1;32m [OK]\033[0m";
        static const char* CROSS = "\033[1;31m[FAIL]\033[0m";
        static const char* WARN  = "\033[1;33m[WARN]\033[0m";

        auto write_status = [&](const std::string& s) {
            std::ofstream f(status_path);
            f << s << "\n";
            std::cout << "[STARTUP-CHECK] " << s << "\n";
            std::cout.flush();
        };

        auto print_check = [&](bool ok, const std::string& label, const std::string& detail = "") {
            std::cout << (ok ? TICK : CROSS) << " " << label;
            if (!detail.empty()) std::cout << " -- " << detail;
            std::cout << "\n";
            std::cout.flush();
        };

        std::cout << "\n\033[1;36m========================================\033[0m\n";
        std::cout << "\033[1;36m  OMEGA STARTUP VERIFICATION\033[0m\n";
        std::cout << "\033[1;36m========================================\033[0m\n";
        std::cout.flush();

        // ── Check 1: Correct git hash ─────────────────────────────────────
        // Verifies the binary matches the expected commit -- not a stale build.
        // We print the hash; operator must visually confirm it matches the push.
        {
            const std::string hash = OMEGA_VERSION;
            print_check(hash != "unknown" && !hash.empty(),
                "Git hash: " + hash,
                hash == "unknown" ? "build system did not inject hash" : "verify against GitHub HEAD");
        }

        // ── Check 2: Config loaded with cTrader credentials ───────────────
        {
            const bool cfg_ok = g_cfg.ctrader_depth_enabled
                             && !g_cfg.ctrader_access_token.empty()
                             && g_cfg.ctrader_ctid_account_id == 43014358;
            print_check(cfg_ok, "Config: ctrader_api section",
                cfg_ok ? ("ctid=" + std::to_string(g_cfg.ctrader_ctid_account_id))
                       : "enabled=" + std::to_string(g_cfg.ctrader_depth_enabled)
                         + " token_len=" + std::to_string(g_cfg.ctrader_access_token.size())
                         + " ctid=" + std::to_string(g_cfg.ctrader_ctid_account_id));
            if (!cfg_ok) {
                write_status("FAIL: ctrader_api config missing or wrong ctid");
                return;
            }
        }

        // ── Check 3: Mode is SHADOW ───────────────────────────────────────
        {
            const bool shadow_ok = (g_cfg.mode == "SHADOW");
            print_check(shadow_ok, "Mode: " + g_cfg.mode,
                shadow_ok ? "shadow trades only -- no real orders" : "WARNING: LIVE mode active");
            // Not a fatal check -- just informational
        }

        // ── Check 4: FIX connected (ticks flowing) ────────────────────────
        // Wait up to 45s for first tick to arrive via FIX
        {
            bool fix_ok = false;
            for (int i = 0; i < 45 && !fix_ok; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                fix_ok = (g_macro_ctx.gold_mid_price > 0.0);
            }
            print_check(fix_ok, "FIX feed: XAUUSD ticks",
                fix_ok ? ("mid=" + [&]{ std::ostringstream o; o << std::fixed << std::setprecision(2) << g_macro_ctx.gold_mid_price; return o.str(); }())
                       : "no XAUUSD tick received after 45s");
            if (!fix_ok) {
                write_status("FAIL: FIX feed not delivering XAUUSD ticks after 45s");
                return;
            }
        }

        // ── Check 5: cTrader depth client connected ───────────────────────
        // depth_active goes true once the SSL handshake + auth completes
        {
            bool ct_ok = false;
            for (int i = 0; i < 60 && !ct_ok; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                ct_ok = g_ctrader_depth.depth_active.load();
            }
            print_check(ct_ok, "cTrader: depth client connected (ctid=43014358)",
                ct_ok ? ("connected at " + std::to_string(elapsed()) + "s")
                      : "not connected after 60s -- check token expiry / network");
            if (!ct_ok) {
                write_status("FAIL: cTrader depth not connected after 60s");
                return;
            }
        }

        // ── Check 6: XAUUSD depth events flowing ─────────────────────────
        // depth_events_total must be increasing -- the only reliable liveness signal.
        // L2 watchdog uses depth_events_total (not imbalance value) as liveness check.
        {
            const uint64_t ev0 = g_ctrader_depth.depth_events_total.load();
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const uint64_t ev1 = g_ctrader_depth.depth_events_total.load();
            const bool events_ok = (ev1 > ev0);
            const uint64_t xau_evts = ev1 - ev0;
            print_check(events_ok, "cTrader: XAUUSD depth events flowing",
                events_ok ? (std::to_string(xau_evts) + " events in 10s")
                          : "depth_events_total not increasing -- feed stalled");
            if (!events_ok) {
                write_status("FAIL: cTrader depth events not increasing -- feed stalled");
                return;
            }
        }

        // ── Check 7: gold_l2_real flag set (DOM confirmed live) ───────────
        // gold_l2_real = g_l2_gold.fresh(now, 3000) -- true when depth event
        // arrived within last 3s. Confirms XAUUSD specifically is receiving events.
        {
            bool l2_real_ok = false;
            for (int i = 0; i < 15 && !l2_real_ok; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                l2_real_ok = g_macro_ctx.gold_l2_real;
            }
            const uint64_t total = g_ctrader_depth.depth_events_total.load();
            const int rbid = g_l2_gold.raw_bid.load(std::memory_order_relaxed);
            const int rask = g_l2_gold.raw_ask.load(std::memory_order_relaxed);
            print_check(l2_real_ok, "cTrader: gold_l2_real flag live",
                l2_real_ok
                    ? ("events_total=" + std::to_string(total)
                       + " raw_bid=" + std::to_string(rbid)
                       + " raw_ask=" + std::to_string(rask))
                    : "gold_l2_real=false after 15s -- XAUUSD depth events not updating g_l2_gold");
            if (!l2_real_ok) {
                // Not fatal -- entries blocked by L2-dead gate, trading degraded
                std::cout << WARN << " Trading will be blocked by [L2-DEAD-BLOCK] until DOM recovers\n";
                std::cout.flush();
            }
        }

        // ── Check 8: ATR seeded (GoldFlow can size positions) ─────────────
        {
            const bool atr_ok = (g_gold_flow.current_atr() > 0.5);
            print_check(atr_ok, "GoldFlow ATR seeded",
                atr_ok ? ("atr=" + [&]{ std::ostringstream o; o << std::fixed << std::setprecision(2) << g_gold_flow.current_atr(); return o.str(); }() + "pt")
                       : "atr=0 -- bars not yet loaded, GoldFlow entries will be delayed");
        }

        // ── Check 9: Daily loss limit not already hit ─────────────────────
        {
            const double dpnl = g_omegaLedger.dailyPnl();
            const bool loss_ok = (dpnl > -(g_cfg.daily_loss_limit * 0.90));
            print_check(loss_ok, "Risk: daily loss limit",
                "daily_pnl=$" + [&]{ std::ostringstream o; o << std::fixed << std::setprecision(2) << dpnl; return o.str(); }()
                + " limit=$" + [&]{ std::ostringstream o; o << std::fixed << std::setprecision(0) << g_cfg.daily_loss_limit; return o.str(); }());
        }

        std::cout << "\033[1;36m========================================\033[0m\n\n";
        std::cout.flush();

        // ── Final status ──────────────────────────────────────────────────
        const bool l2_final = g_macro_ctx.gold_l2_real;
        if (l2_final) {
            write_status("OK: all checks passed -- cTrader live, gold_l2_real=1, trading active");
        } else {
            write_status("WARN: cTrader connected but gold_l2_real=0 -- entries blocked until DOM recovers");
        }
    }).detach();

    // =========================================================================
    // L2 WATCHDOG THREAD
    // =========================================================================
    // cTrader L2 feed (ctid=43014358) is the BASIS of all GoldFlow engine
    // functionality. Without L2 imbalance the engine degrades to drift-only
    // mode which has no proven edge (backtest: 63% WR, negative P&L).
    //
    // This watchdog:
    //   1. Monitors L2 liveness every 30s (depth_events_total increasing)
    //   2. Sets g_l2_watchdog_dead atomic if L2 has been dead > 120s
    //   3. Writes C:\Omega\logs\L2_ALERT.txt immediately on failure
    //   4. Logs [L2-WATCHDOG] DEAD/ALIVE to main log every 30s
    //   5. On recovery: logs restoration and clears alert file
    //
    // GoldFlowEngine checks g_l2_watchdog_dead via goldflow_enabled gate
    // in tick_gold.hpp -- entries blocked when L2 is confirmed dead.
    // Position management (trail/SL) continues regardless.
    //
    // IMMUTABLE: ctid=43014358 is the ONLY account that delivers L2 depth.
    // DO NOT change ctid_trader_account_id in omega_config.ini.
    // =========================================================================
    std::thread([](){
        const std::string alert_path = log_root_dir() + "/L2_ALERT.txt";
        int64_t dead_since_ms    = 0;
        bool    alert_written    = false;
        bool    was_alive        = false;
        static constexpr int64_t DEAD_GRACE_MS   = 120000; // 2 min before declaring dead
        static constexpr int64_t CHECK_INTERVAL  = 30000;  // check every 30s

        // Wait for startup to complete before monitoring
        std::this_thread::sleep_for(std::chrono::seconds(90));

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL));
            if (!g_running.load()) break;

            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            // L2 liveness = depth_events_total increasing each 30s interval.
            // cTrader ProtoOADepthEvent sends size_raw=0 for XAUUSD DOM quotes.
            // imbalance_level() (level count) is used instead of imbalance() (volume),
            // so 0.500 is valid neutral -- NOT an indicator of dead feed.
            // The ONLY reliable liveness signal is events_total increasing.
            static uint64_t s_last_event_count = 0;
            const uint64_t cur_event_count = g_ctrader_depth.depth_events_total.load(std::memory_order_relaxed);
            const bool events_flowing = (cur_event_count > s_last_event_count);
            s_last_event_count = cur_event_count;

            // Liveness = events_flowing ONLY.
            // imbalance_level() produces 0.500 for genuinely neutral DOM (equal bid/ask
            // levels from cTrader). Do NOT treat 0.500 as "dead" -- it is valid neutral.
            // The ONLY reliable signal that cTrader is connected is depth_events_total
            // increasing each 30s interval.
            const double cur_imb = g_l2_gold.imbalance.load(std::memory_order_relaxed);
            const bool l2_alive = events_flowing;

            // Diagnostic every 30s -- always visible, confirms imbalance is moving
            printf("[L2-WATCHDOG] events_total=%llu events_flowing=%d imb=%.4f alive=%d watchdog_dead=%d\n",
                   (unsigned long long)cur_event_count,
                   (int)events_flowing,
                   cur_imb, (int)l2_alive,
                   (int)g_l2_watchdog_dead.load(std::memory_order_relaxed));
            fflush(stdout);

            if (l2_alive) {
                // L2 is flowing
                if (!was_alive) {
                    // Just recovered
                    printf("[L2-WATCHDOG] ALIVE -- L2 depth flowing from ctid=43014358 events=%llu imb=%.3f\n",
                           (unsigned long long)cur_event_count, cur_imb);
                    fflush(stdout);
                    // Clear alert file
                    { std::ofstream f(alert_path); f << "OK\n"; }
                    alert_written = false;
                }
                was_alive    = true;
                dead_since_ms = 0;
                g_l2_watchdog_dead.store(false, std::memory_order_relaxed);

            } else {
                // L2 not flowing
                if (dead_since_ms == 0) dead_since_ms = now_ms;
                const int64_t dead_ms = now_ms - dead_since_ms;

                if (dead_ms >= DEAD_GRACE_MS) {
                    // Confirmed dead -- gate engines
                    g_l2_watchdog_dead.store(true, std::memory_order_relaxed);

                    printf("[L2-WATCHDOG] *** DEAD *** cTrader depth from ctid=43014358 dead for %llds\n"
                           "[L2-WATCHDOG] events_total=%llu imb=%.4f\n"
                           "[L2-WATCHDOG] DIAGNOSIS: events_total not increasing => cTrader TCP disconnected\n"
                           "[L2-WATCHDOG] DIAGNOSIS: imb value irrelevant when events=0 (no data flowing)\n"
                           "[L2-WATCHDOG] GoldFlow GATED. ACTION REQUIRED: restart Omega or check ctid=43014358\n",
                           (long long)(dead_ms / 1000),
                           (unsigned long long)cur_event_count,
                           cur_imb);
                    fflush(stdout);

                    if (!alert_written) {
                        std::ofstream f(alert_path);
                        f << "L2_DEAD\n"
                          << "dead_seconds=" << (dead_ms / 1000) << "\n"
                          << "events_total=" << cur_event_count << "\n"
                          << "imbalance=" << cur_imb << "\n"
                          << "diagnosis=no_depth_events_from_ctid_43014358_TCP_dead_or_auth_failed\n"
                          << "ctid=43014358 is the ONLY account with L2\n"
                          << "action=restart_Omega_or_check_cTrader_Open_API_connection\n";
                        alert_written = true;
                        printf("[L2-WATCHDOG] Alert written: %s\n", alert_path.c_str());
                        fflush(stdout);
                    }
                    was_alive = false;

                } else {
                    // Within grace period -- just log
                    printf("[L2-WATCHDOG] L2 not flowing for %llds (grace=%llds) -- watching\n",
                           (long long)(dead_ms / 1000), (long long)(DEAD_GRACE_MS / 1000));
                    fflush(stdout);
                }
            }
        }
    }).detach();
    // =========================================================================
    // Gate FIX thread launch on cTrader being live.
    // cTrader L2 data is essential for supervisor regime classification and
    // vol baseline warmup. Starting FIX before L2 flows means the supervisor
    // operates with l2_imb=0.500 (stale default) and tips to HIGH_RISK_NO_TRADE,
    // permanently blocking entries until the cooldown chain clears.
    // Wait up to 45s for cTrader to connect and L2 to start flowing.
    // If cTrader fails to connect in 45s, start FIX anyway (degraded mode).
    {
        const auto ct_wait_start = std::chrono::steady_clock::now();
        bool ct_ready = false;
        printf("[STARTUP] Waiting for cTrader L2 before starting FIX...\n");
        fflush(stdout);
        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - ct_wait_start).count() < 45) {
            // cTrader is ONLY L2 source. Live = depth_active AND >10 events received.
            // Do NOT check imbalance != 0.5 -- book fills incrementally from 0.5.
            const bool depth_up2  = g_ctrader_depth.depth_active.load();
            const uint64_t events = g_ctrader_depth.depth_events_total.load();
            if (depth_up2 && events > 10) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - ct_wait_start).count();
                printf("[STARTUP] cTrader L2 live after %llds (events=%llu) -- starting FIX\n",
                       (long long)elapsed, (unsigned long long)events);
                fflush(stdout);
                ct_ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!ct_ready) {
            printf("[STARTUP] cTrader L2 not live after 45s -- starting FIX anyway\n");
            fflush(stdout);
        }
    }
    std::thread trade_thread(trade_loop);
    Sleep(500);  // Give trade connection 500ms head start before quote loop
    quote_loop();  // blocks until g_running=false

    // quote_loop has exited -- g_running is false, trade_loop will exit shortly.
    std::cout << "[OMEGA] Shutdown\n";
    // Stop cTrader depth feed before joining other threads
    g_ctrader_depth.stop();

    // Wait up to 5s for any pending close orders to be ACKed by broker before
    // tearing down the trade connection. Only matters in LIVE mode.
    if (g_cfg.mode == "LIVE") {
        const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < close_deadline) {
            std::lock_guard<std::mutex> lk(g_live_orders_mtx);
            bool any_pending = false;
            for (const auto& kv : g_live_orders)
                if (!kv.second.acked && !kv.second.rejected) { any_pending = true; break; }
            if (!any_pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::cout << "[OMEGA] Close orders settled\n";
    }

    // Wait up to 3s for trade_loop to finish its own logout+SSL_free sequence.
    {
        auto start = std::chrono::steady_clock::now();
        while (!g_trade_thread_done.load() &&
               std::chrono::steady_clock::now() - start < std::chrono::milliseconds(3000)) {
            Sleep(10);
        }
    }
    // If trade thread did not finish in 3s (stuck in SSL/reconnect), detach it
    // and force-exit the process. Calling join() on a stuck SSL thread hangs
    // indefinitely -- the process never exits and Ctrl+C appears to do nothing.
    // Evidence: [OMEGA] Shutdown printed but process hangs after [ADAPTIVE-RISK] lines.
    if (!g_trade_thread_done.load()) {
        std::cout << "[OMEGA] Trade thread still running after 3s -- detaching and forcing exit\n";
        std::cout.flush();
        if (trade_thread.joinable()) trade_thread.detach();
        // Flush/close logs before hard exit
        if (g_tee_buf) { g_tee_buf->flush_and_close(); std::cout.rdbuf(g_orig_cout); delete g_tee_buf; g_tee_buf = nullptr; }
        WSACleanup();
        ReleaseMutex(g_singleton_mutex);
        CloseHandle(g_singleton_mutex);
        g_shutdown_done.store(true);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }
    if (trade_thread.joinable()) trade_thread.join();
    gui_server.stop();
    if (g_daily_trade_close_log) g_daily_trade_close_log->close();
    if (g_daily_gold_trade_close_log) g_daily_gold_trade_close_log->close();
    if (g_daily_shadow_trade_log) g_daily_shadow_trade_log->close();
    if (g_daily_trade_open_log) g_daily_trade_open_log->close();
    g_trade_close_csv.close();
    g_trade_open_csv.close();
    g_shadow_csv.close();
    // Stop hot-reload watcher before saving state -- prevents reload during shutdown
    OmegaHotReload::stop();
    // ?? Edge systems shutdown -- persist TOD + Kelly + fill quality data ????????
    g_edges.tod.save_csv(log_root_dir() + "/omega_tod_buckets.csv");
    g_edges.tod.print_worst(15);
    g_edges.fill_quality.save_csv(log_root_dir() + "/fill_quality.csv");
    g_edges.fill_quality.print_summary();
    // Save Kelly performance on shutdown (not just rollover) so intra-session
    // trades survive process restart without re-warming for 15+ trades.
    g_adaptive_risk.save_perf(state_root_dir() + "/kelly");
    g_gold_flow.save_atr_state(state_root_dir() + "/gold_flow_atr.dat");
    g_gold_stack.save_atr_state(state_root_dir() + "/gold_stack_state.dat");
    g_trend_pb_gold.save_state(state_root_dir()  + "/trend_pb_gold.dat");

    // Save bar indicator state -- instant warm restart, no 15-min cold start
    // load_indicators() at startup reads these files and sets m1_ready=true immediately
    // save_indicators() skips flat/holiday state automatically (built-in sanity check)
    const std::string base_save = log_root_dir();
    g_bars_gold.m1 .save_indicators(base_save + "/bars_gold_m1.dat");
    g_bars_gold.m5 .save_indicators(base_save + "/bars_gold_m5.dat");
    g_bars_gold.m15.save_indicators(base_save + "/bars_gold_m15.dat");
    g_bars_gold.h4 .save_indicators(base_save + "/bars_gold_h4.dat");
    g_bars_sp.m1   .save_indicators(base_save + "/bars_sp_m1.dat");
    g_bars_nq.m1   .save_indicators(base_save + "/bars_nq_m1.dat");
    printf("[SHUTDOWN] Bar indicator state saved -- next restart will be instant warm\n");
    fflush(stdout);
    g_trend_pb_ger40.save_state(state_root_dir() + "/trend_pb_ger40.dat");
    g_trend_pb_nq.save_state(state_root_dir()    + "/trend_pb_nq.dat");
    g_trend_pb_sp.save_state(state_root_dir()    + "/trend_pb_sp.dat");
    g_adaptive_risk.print_summary();
    if (g_tee_buf)   { g_tee_buf->flush_and_close(); std::cout.rdbuf(g_orig_cout); delete g_tee_buf; g_tee_buf = nullptr; }
    WSACleanup();
    ReleaseMutex(g_singleton_mutex);
    CloseHandle(g_singleton_mutex);
    g_shutdown_done.store(true);  // unblock console_ctrl_handler -- process may now exit
    return 0;
}


