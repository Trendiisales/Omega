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

    // Startup marker read by QUICK_RESTART.ps1 verify step -- match regex: \[Omega\] Git hash: [0-9a-f]{7}
    std::cout << "[Omega] Git hash: " << OMEGA_VERSION << "\n"; std::cout.flush();

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

    {
        const std::string trade_dir = log_root_dir() + "/trades";
        const std::string gold_dir  = log_root_dir() + "/gold";
        const std::string shadow_trade_dir = log_root_dir() + "/shadow/trades";
        const std::string header =
            "trade_id,trade_ref,entry_ts_unix,entry_ts_utc,entry_utc_weekday,"
            "exit_ts_unix,exit_ts_utc,exit_utc_weekday,symbol,engine,side,"
            "entry_px,exit_px,tp,sl,size,gross_pnl,net_pnl,"
            "slippage_entry,slippage_exit,commission,"
            "slip_entry_pct,slip_exit_pct,comm_per_side,"
            "mfe,mae,hold_sec,spread_at_entry,"
            "latency_ms,regime,exit_reason,l2_imbalance,l2_live";

        const std::string trade_csv_path = trade_dir + "/omega_trade_closes.csv";
        ensure_parent_dir(trade_csv_path);
        g_trade_close_csv.open(trade_csv_path, std::ios::app);
        if (!g_trade_close_csv.is_open()) {
            std::cerr << "[OMEGA-FATAL] Failed to open full trade CSV: " << trade_csv_path << "\n";
            return 1;
        }
        // Write header only if file is empty -- never truncate, all trades must persist
        g_trade_close_csv.seekp(0, std::ios::end);
        if (g_trade_close_csv.tellp() == std::streampos(0))
            g_trade_close_csv << header << '\n';
        std::cout << "[OMEGA] Full Trade CSV: " << trade_csv_path << "\n";

        g_daily_trade_close_log = std::make_unique<RollingCsvLogger>(
            trade_dir, "omega_trade_closes", header);
        g_daily_gold_trade_close_log = std::make_unique<RollingCsvLogger>(
            gold_dir, "omega_gold_trade_closes", header);
        g_daily_shadow_trade_log = std::make_unique<RollingCsvLogger>(
            shadow_trade_dir, "omega_shadow_trades", header);

        // Trade opens CSV -- entry-time audit log, persists across restarts
        const std::string opens_header =
            "entry_ts_unix,entry_ts_utc,entry_utc_weekday,"
            "symbol,engine,side,"
            "entry_px,tp,sl,size,"
            "spread_at_entry,regime,reason";
        const std::string opens_csv_path = trade_dir + "/omega_trade_opens.csv";
        ensure_parent_dir(opens_csv_path);
        g_trade_open_csv.open(opens_csv_path, std::ios::app);
        if (!g_trade_open_csv.is_open()) {
            std::cerr << "[OMEGA-FATAL] Failed to open trade opens CSV: " << opens_csv_path << "\n";
            return 1;
        }
        g_trade_open_csv.seekp(0, std::ios::end);
        if (g_trade_open_csv.tellp() == std::streampos(0))
            g_trade_open_csv << opens_header << '\n';
        std::cout << "[OMEGA] Trade Opens CSV: " << opens_csv_path << "\n";
        g_daily_trade_open_log = std::make_unique<RollingCsvLogger>(
            trade_dir, "omega_trade_opens", opens_header);

        std::cout << "[OMEGA] Daily Trade Logs: " << trade_dir
                  << "/omega_trade_closes_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
        std::cout << "[OMEGA] Daily Gold Logs: " << gold_dir
                  << "/omega_gold_trade_closes_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
        std::cout << "[OMEGA] Daily Shadow Trade Logs: " << shadow_trade_dir
                  << "/omega_shadow_trades_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
    }

    const std::string shadow_csv_path =
        resolve_audit_log_path(g_cfg.shadow_csv, "shadow/omega_shadow.csv");
    ensure_parent_dir(shadow_csv_path);
    g_shadow_csv.open(shadow_csv_path, std::ios::app);
    if (!g_shadow_csv.is_open()) {
        std::cerr << "[OMEGA-FATAL] Failed to open shadow trade CSV: " << shadow_csv_path << "\n";
        return 1;
    }
    // Write header only if file is empty -- never truncate, data must survive restarts
    g_shadow_csv.seekp(0, std::ios::end);
    if (g_shadow_csv.tellp() == std::streampos(0))
        g_shadow_csv << "ts_unix,symbol,side,engine,entry_px,exit_px,pnl,mfe,mae,"
                        "hold_sec,exit_reason,spread_at_entry,latency_ms,regime\n";
    std::cout << "[OMEGA] Shadow CSV: " << shadow_csv_path << "\n";

    // ?? Startup ledger reload -- restore today's closed trades from CSV ???????
    // g_omegaLedger is in-memory only and resets on restart. On restart mid-session
    // the GUI shows daily_pnl=$0 and total_trades=0 even though trades happened.
    // Fix: read today's daily rotating trade CSV on startup and replay into ledger
    // so daily P&L, win/loss counts, and engine attribution are correct immediately.
    // Reads: logs/trades/omega_trade_closes_YYYY-MM-DD.csv (full detail format).
    // Only replays if today's file exists -- no-op on first startup of the day.
    {
        const auto t_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti_now{}; gmtime_s(&ti_now, &t_now);
        char today_date[16];
        snprintf(today_date, sizeof(today_date), "%04d-%02d-%02d",
                 ti_now.tm_year+1900, ti_now.tm_mon+1, ti_now.tm_mday);
        const std::string reload_path =
            log_root_dir() + "/trades/omega_trade_closes_" + today_date + ".csv";

        std::ifstream reload_f(reload_path);
        if (!g_cfg.reload_trades_on_startup) {
            std::cout << "[OMEGA] Startup reload: DISABLED (reload_trades_on_startup=false) -- clean PnL slate\n";
        } else if (reload_f.is_open()) {
            int reloaded = 0;
            std::string line;
            std::getline(reload_f, line); // skip header
            while (std::getline(reload_f, line)) {
                if (line.empty()) continue;
                // Parse CSV: trade_id(0), trade_ref(1), entry_ts_unix(2), entry_ts_utc(3),
                // entry_wd(4), exit_ts_unix(5), exit_ts_utc(6), exit_wd(7),
                // symbol(8), engine(9), side(10),
                // entry_px(11), exit_px(12), tp(13), sl(14), size(15),
                // gross_pnl(16), net_pnl(17), slip_e(18), slip_x(19), comm(20),
                // slip_epct(21), slip_xpct(22), comm_side(23),
                // mfe(24), mae(25), hold_sec(26), spread(27), lat(28),
                // regime(29), exit_reason(30)
                std::vector<std::string> tok;
                tok.reserve(32);
                std::string cell;
                bool in_q = false;
                for (char c : line) {
                    if (c == '"') { in_q = !in_q; continue; }
                    if (c == ',' && !in_q) { tok.push_back(cell); cell.clear(); continue; }
                    cell += c;
                }
                tok.push_back(cell);
                if (tok.size() < 20) continue;

                omega::TradeRecord tr;
                tr.id         = std::stoi(tok[0].empty() ? "0" : tok[0]);
                tr.entryTs    = std::stoll(tok[2].empty() ? "0" : tok[2]);
                tr.exitTs     = std::stoll(tok[5].empty() ? "0" : tok[5]);
                tr.symbol     = tok[8];
                tr.engine     = tok[9];
                tr.side       = tok[10];
                tr.entryPrice = std::stod(tok[11].empty() ? "0" : tok[11]);
                tr.exitPrice  = std::stod(tok[12].empty() ? "0" : tok[12]);
                tr.tp         = std::stod(tok[13].empty() ? "0" : tok[13]);
                tr.sl         = std::stod(tok[14].empty() ? "0" : tok[14]);
                tr.size       = std::stod(tok[15].empty() ? "0" : tok[15]);
                tr.pnl        = std::stod(tok[16].empty() ? "0" : tok[16]);
                tr.net_pnl    = std::stod(tok[17].empty() ? "0" : tok[17]);
                tr.slippage_entry = std::stod(tok[18].empty() ? "0" : tok[18]);
                tr.slippage_exit  = std::stod(tok[19].empty() ? "0" : tok[19]);
                tr.commission     = tok.size() > 20 ? std::stod(tok[20].empty() ? "0" : tok[20]) : 0.0;
                tr.mfe            = tok.size() > 24 ? std::stod(tok[24].empty() ? "0" : tok[24]) : 0.0;
                tr.mae            = tok.size() > 25 ? std::stod(tok[25].empty() ? "0" : tok[25]) : 0.0;
                tr.spreadAtEntry  = tok.size() > 27 ? std::stod(tok[27].empty() ? "0" : tok[27]) : 0.0;
                tr.regime         = tok.size() > 29 ? tok[29] : "";
                tr.exitReason     = tok.size() > 30 ? tok[30] : "";
                tr.l2_imbalance   = tok.size() > 31 ? std::stod(tok[31].empty() ? "0.5" : tok[31]) : 0.5;
                tr.l2_live        = tok.size() > 32 ? (tok[32] == "1") : false;

                if (tr.symbol.empty() || tr.entryTs == 0) continue;

                g_omegaLedger.record(tr);
                // Also restore engine P&L attribution in telemetry
                g_telemetry.AccumEnginePnl(tr.engine.c_str(), tr.net_pnl);
                ++reloaded;
            }
            reload_f.close();
            if (reloaded > 0) {
                std::cout << "[OMEGA] Startup reload: " << reloaded
                          << " trades from " << reload_path
                          << " ? daily_pnl=$" << std::fixed << std::setprecision(2)
                          << g_omegaLedger.dailyPnl() << "\n";
            } else {
                std::cout << "[OMEGA] Startup reload: " << reload_path
                          << " found but empty (first run of day)\n";
            }
        } else {
            std::cout << "[OMEGA] Startup reload: no today CSV yet ("
                      << reload_path << ") -- clean start\n";
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[OMEGA] WSAStartup failed\n"; return 1;
    }
    SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();

    if (!g_telemetry.Init()) std::cerr << "[OMEGA] Telemetry init failed\n";
    g_telemetry.SetMode(g_cfg.mode.c_str());
    g_telemetry.UpdateBuildVersion(OMEGA_VERSION, OMEGA_BUILT);
    if (g_telemetry.snap()) {
        g_telemetry.snap()->uptime_sec = 0;
        g_telemetry.snap()->start_time = g_start_time;
    }

    omega::OmegaTelemetryServer gui_server;
    std::cout.flush();  // flush before spawning GUI threads to avoid interleaved output
    gui_server.start(g_cfg.gui_port, g_cfg.ws_port, g_telemetry.snap());
    Sleep(200);  // let GUI threads print their startup lines before we continue

    // ── Real DOM receiver -- connects to OmegaDomStreamer cBot on port 8765 ────
    // Starts background thread that reads real XAUUSD DOM sizes from cTrader.
    // Reconnects automatically if cBot restarts. No-op if cBot not running.
    g_real_dom_receiver.start();
    printf("[STARTUP] RealDomReceiver started -- connecting to cBot on localhost:8765\n");
    fflush(stdout);

    std::cout << "[OMEGA] GUI http://localhost:" << g_cfg.gui_port
              << "  WS:" << g_cfg.ws_port << "\n";
    std::cout.flush();

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // ?? cTrader Open API depth feed (parallel to FIX -- read-only L2 data) ????
    // Provides real multi-level order book (ProtoOADepthEvent) to replace
    // FIX 264=1 single-level estimates. Runs in its own thread independently.
    if (g_cfg.ctrader_depth_enabled && !g_cfg.ctrader_access_token.empty() && g_cfg.ctrader_ctid_account_id > 0) {  // disabled: broker hasn't enabled Open API
        g_ctrader_depth.client_id           = "20304_NqeKlH3FEECOWqeP1JvoT2czQV9xkUHE7UXxfPU2dRuDXrZsIM";
        g_ctrader_depth.client_secret       = "jeYwDPzelIYSoDppuhSZoRpaRi1q572FcBJ44dXNviuSEKxdB9";
        g_ctrader_depth.access_token        = g_cfg.ctrader_access_token;
        g_ctrader_depth.refresh_token       = g_cfg.ctrader_refresh_token;
        g_ctrader_depth.ctid_account_id     = g_cfg.ctrader_ctid_account_id;
        g_ctrader_depth.l2_mtx              = &g_l2_mtx;
        g_ctrader_depth.l2_books            = &g_l2_books;
        // Do NOT load bar_failed from disk on startup.
        // The pre-seeded set below (XAUUSD:1 + live subs) is the correct blocked list.
        // Loading from disk adds stale entries (XAUUSD:0, XAUUSD:5, XAUUSD:7) that
        // permanently block the GetTickDataReq fallback -- causing vol_range=0.00 all day.
        // Evidence: April 2 2026 -- every restart loaded stale failures, bars never seeded,
        // GoldFlow ran blind all session.
        g_ctrader_depth.bar_failed_path_    = log_root_dir() + "/ctrader_bar_failed.txt";
        // load_bar_failed intentionally NOT called -- always start clean.
        // ?? PRIMARY PRICE SOURCE: cTrader depth ? on_tick ????????????????????
        // cTrader Open API streams every tick from the matching engine directly.
        // FIX quote feed can lag 0.5-2pts in fast markets due to gateway batching.
        // Proven: screenshot shows FIX=4643.54 while cTrader depth=4642.54 (1pt off).
        // Solution: cTrader depth drives on_tick() as primary price source.
        // FIX W/X handler calls on_tick() ONLY when cTrader depth is stale (>500ms).
        g_ctrader_depth.on_tick_fn = [](const std::string& sym, double bid, double ask) noexcept {
            // Track last cTrader tick time per symbol for FIX fallback staleness check
            set_ctrader_tick_ms(sym, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            on_tick(sym, bid, ask);
        };
        // Stamp per-symbol tick time on EVERY depth event, not just when both sides present.
        // This prevents gold_size_dead from firing during incremental book fill and
        // triggering connection restarts that clear the book before L2 can stabilise.
        g_ctrader_depth.on_live_tick_ms_fn = [](const std::string& sym) noexcept {
            set_ctrader_tick_ms(sym, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
        // Register atomic write callback -- cTrader thread writes derived scalars
        // (imbalance, microprice_bias, has_data) lock-free after each depth event.
        // FIX tick reads these atomics directly with no mutex contention at all.
        g_ctrader_depth.atomic_l2_write_fn = [](const std::string& sym, double imb, double mp, bool hd, int rbid, int rask, double me) noexcept {
            AtomicL2* al = get_atomic_l2(sym);
            if (!al) return;
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            al->imbalance.store(imb, std::memory_order_relaxed);
            al->microprice_bias.store(mp, std::memory_order_relaxed);
            al->has_data.store(hd, std::memory_order_relaxed);
            al->raw_bid.store(rbid, std::memory_order_relaxed);
            al->raw_ask.store(rask, std::memory_order_relaxed);
            al->micro_edge.store(me, std::memory_order_relaxed);
            al->last_update_ms.store(now_ms, std::memory_order_release);
        };
        // Subscribe depth only for actively traded symbols -- not passive cross-pairs.
        // cTrader drops connection when too many depth streams are requested at once.
        for (int i = 0; i < OMEGA_NSYMS; ++i)
            g_ctrader_depth.symbol_whitelist.insert(OMEGA_SYMS[i].name);
        for (const auto& e : g_ext_syms)
            if (e.name[0] != 0) g_ctrader_depth.symbol_whitelist.insert(e.name);
        // Alternate broker names for gold/silver -- broker may not use .F suffix
        g_ctrader_depth.symbol_whitelist.insert("XAUUSD");
        g_ctrader_depth.symbol_whitelist.insert("SILVER");
        g_ctrader_depth.symbol_whitelist.insert("XAGUSD");
        g_ctrader_depth.symbol_whitelist.insert("VIX");
        g_ctrader_depth.dump_all_symbols = false;  // audit complete -- USOIL.F id=2632 confirmed
        // Alias map: broker name ? internal name used by getImb/getBook
        // XAUUSD is already the canonical name -- no alias needed

        // ?? cTrader broker name ? internal name aliases ?????????????????????
        // BlackBull may use different names in cTrader Open API vs FIX feed.
        // All variants observed or expected mapped here.
        // Rule: internal name = FIX name (OMEGA_SYMS/g_ext_syms) -- aliases
        //       translate broker cTrader names to our internal names.
        // US indices
        g_ctrader_depth.name_alias["US500"]    = "US500.F";
        g_ctrader_depth.name_alias["SP500"]    = "US500.F";
        g_ctrader_depth.name_alias["SPX500"]   = "US500.F";
        g_ctrader_depth.name_alias["USTEC"]    = "USTEC.F";
        // NAS100 is NOT aliased to USTEC.F -- it is a separate cash instrument.
        // The NBM engine runs on "NAS100" and needs its own on_tick() calls.
        // USTEC.F (futures) and NAS100 (cash) have different prices.
        g_ctrader_depth.name_alias["NASDAQ"]   = "USTEC.F";
        g_ctrader_depth.name_alias["TECH100"]  = "USTEC.F";
        g_ctrader_depth.name_alias["US30"]     = "DJ30.F";
        g_ctrader_depth.name_alias["DJ30"]     = "DJ30.F";
        g_ctrader_depth.name_alias["DOW30"]    = "DJ30.F";
        g_ctrader_depth.name_alias["DOWJONES"] = "DJ30.F";
        // Metals
        g_ctrader_depth.name_alias["SILVER"]   = "XAGUSD";
        g_ctrader_depth.name_alias["XAGUSD"]   = "XAGUSD";
        // Oil -- USOIL.F id=2632 shows ~$102; may be Brent priced instrument
        // Aliases cover all known BlackBull oil names until dump_all_symbols confirms
        g_ctrader_depth.name_alias["USOIL"]    = "USOIL.F";
        g_ctrader_depth.name_alias["WTI"]      = "USOIL.F";
        g_ctrader_depth.name_alias["CRUDE"]    = "USOIL.F";
        g_ctrader_depth.name_alias["OIL"]      = "USOIL.F";
        g_ctrader_depth.name_alias["UKBRENT"]  = "BRENT";
        g_ctrader_depth.name_alias["BRENT.F"]  = "BRENT";
        // EU indices
        g_ctrader_depth.name_alias["GER30"]    = "GER40";   // old broker name
        g_ctrader_depth.name_alias["DAX"]      = "GER40";
        g_ctrader_depth.name_alias["DAX40"]    = "GER40";
        g_ctrader_depth.name_alias["FTSE100"]  = "UK100";
        g_ctrader_depth.name_alias["FTSE"]     = "UK100";
        g_ctrader_depth.name_alias["UK100"]    = "UK100";
        g_ctrader_depth.name_alias["STOXX50"]  = "ESTX50";
        g_ctrader_depth.name_alias["SX5E"]     = "ESTX50";
        g_ctrader_depth.name_alias["EUSTX50"]  = "ESTX50";
        // FX
        g_ctrader_depth.name_alias["EUR/USD"]  = "EURUSD";
        g_ctrader_depth.name_alias["GBP/USD"]  = "GBPUSD";
        g_ctrader_depth.name_alias["AUD/USD"]  = "AUDUSD";
        g_ctrader_depth.name_alias["NZD/USD"]  = "NZDUSD";
        g_ctrader_depth.name_alias["USD/JPY"]  = "USDJPY";
        // Other
        g_ctrader_depth.name_alias["VIX"]      = "VIX.F";
        g_ctrader_depth.name_alias["VOLX"]     = "VIX.F";

        // ?? OHLC bar subscriptions -- XAUUSD M1+M5 only ???????????????????
        // XAUUSD spot id=41 (hardcoded, same as depth subscription).
        // On startup: requests 200 M1 + 100 M5 historical bars, then subscribes
        // live bar closes. Indicators (RSI, ATR, EMA, BB, swing, trend) are
        // written to g_bars_gold atomically and read by GoldFlow/GoldStack.
        //
        // REMOVED: US500.F and USTEC.F bar subscriptions.
        // ROOT CAUSE OF SESSION DESTRUCTION: BlackBull broker returns INVALID_REQUEST
        // for trendbar requests on US500.F and USTEC.F (cash/futures index instruments).
        // This causes read_one() to return rc=-1 (SSL connection drop) on EVERY reconnect,
        // immediately after the depth feed becomes stable. Effect:
        //   1. Reconnect cycle fires every 5s indefinitely
        //   2. XAUUSD M1 bars never seed (interrupted before 52 bars load)
        //   3. m1_ready=false -> Gates 3+4 inactive -> naked GoldFlow entries
        //   4. At 02:39 this caused a full process shutdown, missing the 8-min uptrend
        //
        // Evidence from logs: every single reconnect shows exactly:
        //   [CTRADER-BARS] USTEC.F history req period=1 count=200
        //   [CTRADER] Error:  -- INVALID_REQUEST
        //   [CTRADER] Connection error
        //
        // GER40 was removed earlier for the same reason (id=1899, same INVALID_REQUEST).
        // US500.F and USTEC.F now use tick-based vol estimation (same as GER40 fallback).
        // g_bars_sp and g_bars_nq remain allocated -- indicators just won't be seeded.
        // Engines that read g_bars_sp/g_bars_nq already handle m1_ready=false gracefully.
        g_ctrader_depth.bar_subscriptions["XAUUSD"]  = {41,   &g_bars_gold};
        // Index bar subscriptions removed -- broker sends INVALID_REQUEST for all
        // GetTrendbarsReq (pt=2137) calls on index symbols, dropping the TCP connection.
        // g_ctrader_depth.bar_subscriptions["US500.F"] = {2642, &g_bars_sp};
        // g_ctrader_depth.bar_subscriptions["USTEC.F"] = {2643, &g_bars_nq};
        // g_ctrader_depth.bar_subscriptions["GER40"]   = {1899, &g_bars_ger};

        // CRITICAL: Pre-seed bar_failed_reqs to permanently block GetTrendbarsReq (pt=2137).
        // BlackBull cTrader rejects pt=2137 with INVALID_REQUEST for ALL symbols/periods,
        // then sends a TCP RST which drops the live price feed connection.
        // This is the root cause of 2000+ reconnects per day.
        //
        // Fix: mark ALL period=1 requests as "failed" before start() so they are
        // skipped entirely. The dispatch loop routes them to GetTickDataReq (pt=2145)
        // instead, which BlackBull serves correctly.
        //
        // The bar_subscriptions loop uses period sentinels:
        //   period=1   -> was GetTrendbarsReq (pt=2137) -> NOW routed via tick fallback
        //   period=105 -> GetTickDataReq for M5
        //   period=107 -> GetTickDataReq for M15
        // With period=1 now also using tick fallback, pt=2137 is NEVER sent.
        g_ctrader_depth.bar_failed_reqs.insert("XAUUSD:1");        // block pt=2137 for XAUUSD M1
        // Pre-block live trendbar subs -- BlackBull sends TCP RST on pt=2135
        g_ctrader_depth.bar_failed_reqs.insert("XAUUSD:live:1");
        g_ctrader_depth.bar_failed_reqs.insert("XAUUSD:live:5");
        g_ctrader_depth.bar_failed_reqs.insert("XAUUSD:live:7");
        g_ctrader_depth.save_bar_failed(g_ctrader_depth.bar_failed_path_);
        std::cout << "[CTRADER] Pre-blocked trendbar reqs + live subs -- no crash loop\n";

        // ── BAR STATE LOAD -- MUST run before ctrader start() ────────────────
        // Loads saved indicator state (EMA, ATR, RSI, BB) from prior session.
        // Sets m1_ready=true immediately so GoldFlow/GoldStack/RSIReversal/
        // CandleFlow/RSIExtremeTurn are all unblocked from tick 1.
        // (MicroMomentum removed at Batch 5V §1.2.)
        // Without this every restart is a cold start -- bars never seed because
        // BlackBull blocks all trendbar API requests (bar_failed_reqs above).
        // save_indicators() is called on clean shutdown (signal handler below).
        // Rejection criteria: age > 24h, e9<=0, e50<=0 -- any bad file is deleted.
        {
            const std::string bs = log_root_dir();
            const bool m1_ok  = g_bars_gold.m1 .load_indicators(bs + "/bars_gold_m1.dat");
            const bool m5_ok  = g_bars_gold.m5 .load_indicators(bs + "/bars_gold_m5.dat");
            const bool m15_ok = g_bars_gold.m15.load_indicators(bs + "/bars_gold_m15.dat");
            const bool h4_ok  = g_bars_gold.h4 .load_indicators(bs + "/bars_gold_h4.dat");
            const bool sp_ok  = g_bars_sp.m1   .load_indicators(bs + "/bars_sp_m1.dat");
            const bool nq_ok  = g_bars_nq.m1   .load_indicators(bs + "/bars_nq_m1.dat");
            std::cout << "[BAR-LOAD] XAUUSD m1=" << m1_ok << " m5=" << m5_ok
                      << " m15=" << m15_ok << " h4=" << h4_ok
                      << " sp=" << sp_ok << " nq=" << nq_ok
                      << " -- m1_ready=" << g_bars_gold.m1.ind.m1_ready.load()
                      << "\n";
            std::cout.flush();
            if (!m1_ok) {
                std::cout << "[BAR-LOAD] WARN: bars_gold_m1.dat missing or stale -- "
                          << "GoldFlow blocked until M1 bars seed from tick data (~2min)\n";
                std::cout.flush();
            }

            // ── WARMUP SEED: inject disk-loaded bar state into engines that
            // require 2+ bar closes before firing. Without this, every restart
            // causes 2-120min blindness depending on engine and tick rate.
            //
            // Pattern: call set_bar_rsi / seed_bar_atr TWICE with the same
            // disk-loaded value. First call sets current, second call moves
            // current to prev -- engine sees "two bar closes" immediately.
            // Safe: engines only act on direction CHANGE from prev to current,
            // so seeding both to the same value means no spurious signals.
            if (m1_ok) {
                const double seed_rsi = g_bars_gold.m1.ind.rsi14.load();
                const double seed_atr = g_bars_gold.m1.ind.atr14.load();
                const double seed_mid = g_bars_gold.m1.ind.ema9.load();  // price proxy

                // RSIReversalEngine: seed bar_rsi + bar_rsi_prev from disk
                // Eliminates the "need 2 bar closes" block on every restart
                if (seed_rsi > 0.0 && seed_rsi < 100.0) {
                    g_rsi_reversal.set_bar_rsi(seed_rsi, seed_mid);
                    g_rsi_reversal.set_bar_rsi(seed_rsi, seed_mid);  // second call sets prev
                    std::cout << "[WARMUP-SEED] RSIReversal bar_rsi=" << seed_rsi
                              << " (both current+prev seeded from disk)\n";
                    std::cout.flush();
                }

                // RSIExtremeTurnEngine: seed bar_rsi
                if (seed_rsi > 0.0 && seed_rsi < 100.0) {
                    g_rsi_extreme.set_bar_rsi(seed_rsi);
                    std::cout << "[WARMUP-SEED] RSIExtreme bar_rsi=" << seed_rsi << "\n";
                    std::cout.flush();
                }

                // (MicroMomentumEngine warmup-seed REMOVED at Batch 5V §1.2.)

                // DomPersistEngine: seed ATR + warmup counters from disk
                if (seed_atr > 0.5 && seed_atr < 100.0) {
                    g_dom_persist.seed_bar_atr(seed_atr);
                    g_dom_persist.seed_warmup();  // skip 50+100 tick warmup requirement
                    std::cout << "[WARMUP-SEED] DomPersist bar_atr=" << seed_atr
                              << " warmup counters seeded\n";
                    std::cout.flush();
                }

                std::cout << "[WARMUP-SEED] All engines seeded from disk state -- "
                          << "no 2-minute blindness on restart\n";
                std::cout.flush();
            } else {
                std::cout << "[WARMUP-SEED] SKIP -- bars not loaded from disk, "
                          << "engines will warm from live ticks (~2min)\n";
                std::cout.flush();
            }
        }

        g_ctrader_depth.start();
        std::cout << "[CTRADER] Depth feed starting (ctid=" << g_cfg.ctrader_ctid_account_id << ")\n";

        // ?? Symbol subscription cross-check ??????????????????????????????????
        // Runs 5s after start() -- by then SymbolsListRes should have arrived
        // and all bar/depth subscriptions resolved.
        // Logs WARNING for any symbol that will fall back to FIX prices.
        std::thread([&]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::cout << "[CTRADER-AUDIT] Symbol subscription check:\n";
            // Check primary symbols
            for (int i = 0; i < OMEGA_NSYMS; ++i) {
                const std::string& name = OMEGA_SYMS[i].name;
                const bool has_ct = g_ctrader_depth.has_depth_subscription(name);
                std::cout << "[CTRADER-AUDIT]   " << name
                          << (has_ct ? " -> cTrader OK" : " -> *** FIX FALLBACK ONLY ***") << "\n";
            }
            // Check ext symbols
            for (const auto& e : g_ext_syms) {
                if (e.name[0] == 0) continue;
                const bool has_ct = g_ctrader_depth.has_depth_subscription(e.name);
                std::cout << "[CTRADER-AUDIT]   " << e.name
                          << (has_ct ? " -> cTrader OK" : " -> *** FIX FALLBACK ONLY ***") << "\n";
            }
            std::cout.flush();
        }).detach();
    } else {
        std::cout << "[CTRADER] Depth feed disabled -- add [ctrader_api] to omega_config.ini\n";
    }


    std::cout << "[OMEGA] FIX loop starting -- " << g_cfg.mode << " mode\n";

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

            // ── Periodic bar state save every 10 min ─────────────────────────
            // Ensures restart is always warm even if clean shutdown signal is missed.
            // save_indicators() skips flat/holiday/cold state automatically.
            {
                static int64_t s_last_bar_save_s = 0;
                const int64_t now_s_save = static_cast<int64_t>(std::time(nullptr));
                if (now_s_save - s_last_bar_save_s >= 600) {
                    s_last_bar_save_s = now_s_save;
                    const std::string bs = log_root_dir();
                    g_bars_gold.m1 .save_indicators(bs + "/bars_gold_m1.dat");
                    g_bars_gold.m5 .save_indicators(bs + "/bars_gold_m5.dat");
                    g_bars_gold.m15.save_indicators(bs + "/bars_gold_m15.dat");
                    g_bars_gold.h1 .save_indicators(bs + "/bars_gold_h1.dat");
                    g_bars_gold.h4 .save_indicators(bs + "/bars_gold_h4.dat");
                    g_bars_sp.m1   .save_indicators(bs + "/bars_sp_m1.dat");
                    g_bars_nq.m1   .save_indicators(bs + "/bars_nq_m1.dat");
                    printf("[BAR-SAVE] Periodic save complete (every 10min)\n");
                    fflush(stdout);
                }
            }

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
    g_bars_gold.h1 .save_indicators(base_save + "/bars_gold_h1.dat");
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




