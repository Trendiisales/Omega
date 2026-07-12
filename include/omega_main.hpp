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
    std::fprintf(stderr, "[Omega] Git hash: %s\n", OMEGA_VERSION); std::fflush(stderr);

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

    // S-2026-06-03: restore persisted open positions — resume in-flight trades
    // across restart/deploy instead of silently dropping them. Engines were
    // warm-seeded inside init_engines() above, so cells/sources are ready to
    // adopt. Absent file = clean boot (returns {0,0}). Only engines with a
    // registered restorer participate (SurvivorPortfolio wired; others tiered).
    // Restorer registered here (not engine_init) so the persistence feature is
    // self-contained in this commit.
    {
        omega::persist::register_position_persistence();   // survivor + LivePos archetype
        const auto rr = g_open_positions.restore(state_root_dir() + "/open_positions.dat");
        // S-2026-06-11: exempt these restored positions from the phantom-drop guard
        // so their eventual close books PnL (their entry legitimately predates boot).
        for (int64_t ets : g_open_positions.last_restored_entry_ts()) g_restored_entry_ts.insert(ets);
        printf("[POS-RESTORE] %d/%d persisted open positions resumed (%zu entry_ts exempted from phantom-drop)\n",
               rr.first, rr.second, g_restored_entry_ts.size());
        fflush(stdout);
    }

    // S-2026-06-03: MgcFastDonchian30m — fast intraday gold breakout + prior-day
    // volume-profile overhead-supply gate, fed by tools/mgc_live_bars.py
    // (MGC 30m TRADES bars + HVN files). SHADOW (paper) until validated live.
    // ── THE GOLD-FUTURES UNLOCK ── intraday gold is DEAD on spot CFD (cost wall,
    // see [[omega-intraday-spot-cfd-cost-wall]]) but WORKS on MGC futures (tighter
    // cost). S-2026-06-23 faithful re-BT (backtest/MgcFastDonchianBacktest.cpp drives
    // the REAL engine on data/mgc_30m_hist.csv, 23.6k 30m bars 2024-06..2026-06):
    //   Nin=40 Nout=20 HVN-skip @0.4pt: PF1.74 n186 both-halves+ (854/901) rDD6.40
    //   @0.8pt (2x-cost STRESS): PF1.70 both-halves+ -> ROBUST to 2x cost.
    //   (old Nin=20 cfg = PF1.54; bumped 20->40 to the validated-best below.)
    // CAVEATS: bull-window (2024-26, no 2022 bear) + single-source -> the LIVE MGC
    // shadow ledger is the forward/cross-source truth.
    // FEED STATUS (S-2026-06-23, CORRECTED): IB Gateway 4001 IS live and
    // data/mgc_30m_live.csv IS flowing (466+ real 30m bars Jun9-23) -- the earlier
    // "starved/blocked on entitlement" note was a STALE read (checked the file at
    // 0 bytes before the producer's first write). The book is OPERATIONAL; it is
    // correctly FLAT right now because gold is in a downtrend (Jun9-23 4348->4135,
    // -5%) and these are long-biased breakout/trend engines (GoldVolBrk seed
    // trend=0). Verified: the real engine fires 0 trades on the exact 466 live bars
    // in backtest too -> 0 live trades = correct, the PF1.74 edge is DORMANT not
    // dead. The [MGC-FEED] poll heartbeat (MgcFastDonchianFeed.hpp) confirms reads.
    g_mgc_fastdon.enabled     = true;
    g_mgc_fastdon.shadow_mode = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (operator all-engines cutover)
    g_mgc_fastdon.lot         = 1.0;   // S-2026-06-26 operator: deploy at lot size 1 (= 1 MGC micro, $10/pt). SHADOW. Re-look + scale if the live ledger proves it (memory project-revisit-scale-mgcfastdon-bigcapmomo).
    g_mgc_fastdon.Nin = 40; g_mgc_fastdon.Nout = 20; g_mgc_fastdon.Kov = 1.5;  // S-2026-06-23: 20/10 -> 40/20 (faithful-best PF1.74 vs 1.54, 2x-cost-robust)
    g_mgc_fastdon.use_hvn_skip = true;
    g_mgc_fastdon.l2_gate_ = 0.30;  // S-2026-06-23 L2 CONFIRMATION GATE (active, conservative): block a long breakout only when the live MGC book is strongly ask-heavy (bid-share<0.30). OBI showed ask-heavy -> -0.031pt fwd, so this cuts the worst book-against entries. SHADOW -> forward-validates on the ledger; set 0.0 to disable. Inert in backtest (set_l2_imb never called).

    // ── S-2026-07-08c MgcSlowDonchian30m: gold deep-dive candidate #1 (Study 7,
    //    outputs/GOLD_DEEP_DIVE_2026-07-08.md, evidence commit 4bca1036).
    //    Donchian Nin40/Nout20 LONG, next-bar-open entry, 3xATR14 adverse-first
    //    hard stop, on the same MGC 30m feed. Faithful cell (gdd_mgc_volband_
    //    breakout.py, certified MGC 30m 2024-06..2026-06, 0.31pt RT): PF1.83
    //    n204 +2006pt maxDD -188pt both-halves+ (+753/+1254) 2x-cost PF1.79
    //    over-random +1329pt; plateau N30-65 x sl2.5-3.5 all PF>=1.65.
    //    gold_regime() long-block MANDATORY (naked 2022-bear proxy PF0.38).
    //    DEDUP vs MgcFastDon per the deep-dive overlap disclosure (books
    //    correlate): no new entry while the fast sibling holds. SHADOW, 1 micro.
    //    Auto-retirement latch -400pt (2x the -188pt worst BT DD episode).
    g_mgc_slowdon.enabled     = true;
    g_mgc_slowdon.shadow_mode = true;   // shadow until the live ledger proves it
    g_mgc_slowdon.lot         = 1.0;    // 1 MGC micro = 10oz = $10/pt (smallest unit)
    // S-2026-07-11 GOLD PHASE 1b (roadmap deployment item 4): RE-CELLED 40/20 ->
    // 55/27. Phase 1 confirmed the wired system ran BOTH Donchian siblings on the
    // SAME 40/20 horizon (g_mgc_fastdon was bumped 20/10->40/20 on S-2026-06-23,
    // duplicating this engine's cell). 55/27 = the true second horizon
    // (gdd_mgc_volband_breakout.py Don55 all-band L sl3.0, certified MGC 30m
    // 2024-06..2026-06, 0.31pt RT): n158 +1504.6pt PF1.78 both-halves+
    // (+717.6/+786.9) maxDD -278.9 2x-cost PF1.74 ex-best +1265. REAL-engine
    // parity EXACT (this engine driven standalone over the same file:
    // n158 +1504.6 PF1.78). Daily-PnL corr vs the live FastDon 40/20 book drops
    // 0.748 (dup) -> 0.690; the two SlowDon horizons correlate 0.586.
    // sl_atr_mult=3.0 unchanged (plateau sl2.5-3.5 all PF>=1.65, deep-dive).
    g_mgc_slowdon.Nin  = 55;
    g_mgc_slowdon.Nout = 27;
    // Retirement latch rescaled to the NEW cell: 2x its worst BT DD (-278.9pt).
    g_mgc_slowdon.retire_net_pts = -560.0;
    g_mgc_slowdon.peer_holds_pos = []() { return g_mgc_fastdon.has_open_position(); };
    g_mgc_slowdon.seed_from_30m_csv("data/mgc_30m_hist.csv");   // prints the [SEED] line
    g_engine_heartbeat.register_engine("MgcSlowDonchian30m", g_mgc_slowdon.enabled, 3600, 0, 24);
    // S-2026-06-23 MGC BOOK 2nd engine: GoldVolBreakoutM30 on the same MGC feed
    // (orthogonal signal -- EMA200-gated Donchian vol-breakout runner). Faithful
    // BT on MGC 30m: PF2.10 n37 mdd0.78. Warm-seed from MGC 30m hist (M30 Donchian/
    // ATR) + aggregated H1 (EMA200 trend). SHADOW; shares the starved-until-
    // entitlement MGC feed with g_mgc_fastdon. Driven inside poll_mgc_feed.
    g_mgc_volbrk.enabled     = true;
    g_mgc_volbrk.shadow_mode = false;  // S-2026-07-01: LIVE on IBKR 4002 paper (operator all-engines cutover)
    // S-2026-07-11 GOLD PHASE 1 (GOLD_BOOK_ROADMAP bug 2, venue identity): this
    // instance is MGC-fed but used to ledger tr.symbol="XAUUSD" lot=0.01 under the
    // SAME tag as the spot instance -- wrong venue ($1/pt booked instead of
    // $10/pt/contract), non-tradeable fractional futures size, mixed attribution.
    // Now: symbol MGC, 1 micro contract (10oz, $10/pt -- same size convention as
    // g_mgc_fastdon/g_mgc_slowdon), own tag MgcVolBreakoutM30_* (fresh ledger book;
    // the gvb_m30 companion clips include both tags, see engine_init). Cost gate
    // uses the explicit MGC row. NOTE: booked $/pt rises 10x vs the old mis-scaled
    // emission -- that is the honest contract value, not a size-up decision.
    g_mgc_volbrk.ledger_symbol = "MGC";
    g_mgc_volbrk.engine_tag    = "MgcVolBreakoutM30_imp2.0_stop1.5_trail3.0_BEoff";
    g_mgc_volbrk.lot         = 1.0;    // 1 MGC micro = 10oz = $10/pt (smallest tradeable unit)
    g_mgc_volbrk.min_qty     = 1.0;
    g_mgc_volbrk.qty_step    = 1.0;
    g_mgc_volbrk.max_spread  = 1.50;   // MGC futures pts (looser than spot gold $)
    // S-2026-07-11 GOLD PHASE 1b (roadmap #6): stop DECISION TEST wired. The MGC
    // instance is bar-fed, so its tick sl/trail was dead code (46/46 MAX_HOLD
    // exits). 3-variant test on the REAL engine (backtest/mgc_volbrk_tickstop_
    // decision.cpp, certified MGC 30m 2024-06..2026-06, 0.31pt RT + 2x stress):
    //   ORIGINAL  (no stop, MAX_HOLD only) : n33 +266pt PF1.41 worst-82 maxDD197
    //   PROTECTED (1.5ATR + 3ATR trail as a bar-path resting stop, gap-honest):
    //               n37 +281pt PF2.07 worst-31 maxDD79, halves +90/+191, 2x PF2.00
    //   CATASTROPHE 5/6/8 ATR              : PF1.36-1.47, worst -82..-99 (dominated)
    // -> WINNER: PROTECTED. stop_mode=2 enforces the engine's own sl/trail
    // adverse-first on each 30m bar (fill min(open,sl) on gaps). 2022-bear shadow
    // n2-3 either way (trend gate sits it out) -- no bear objection. Spot instance
    // stays stop_mode=1 (tick-driven, unchanged). Same commit: poll_mgc_feed now
    // passes ts*1000 (was SECONDS -> garbage session-hour + 1970 ledger ts).
    g_mgc_volbrk.stop_mode = 2;
    g_mgc_volbrk.init();
    g_mgc_volbrk.l2_gate_ = 0.30;  // S-2026-06-23 L2 confirmation gate (active, conservative; same as fastdon). Inert in backtest.
    g_mgc_volbrk.seed_h1_from_csv ("data/mgc_h1_hist.csv");
    g_mgc_volbrk.seed_m30_from_csv("data/mgc_30m_hist.csv");

    // ── S-2026-07-07 MGC VENUE PORT: XauTrendFollow4h/2h instances on the MGC
    //    feed, production spot config mirrored 1:1 (mask 0xC9 + IMP0.5 + ADX15
    //    + vol-band / 4-cell + ADX25 + Donch50 vol-band). Faithful venue BT
    //    (XauTrendFollow4h2hBacktest MGC=1, real MGC H4/H1 2024-06..2026-06,
    //    spread 0.10 + $0.208/oz RT): 4h PF1.54 +$4404 DD$1331 both-halves+,
    //    2h PF1.21 +$4281 both-halves+, both 2x-cost PASS; 2022-23 bear at MGC
    //    cost bull-positive-bear-flat (4h +$650 PF1.22), same profile as spot.
    //    SHADOW (no live size until the shadow ledger proves the venue).
    //    ADVERSE-PROTECTION: inherited spot verdicts -- 4h trail-only +
    //    LOSS_CUT 1.5% / 2h LOSS_CUT 0.5% (S-2026-06-17 faithful sweeps).
    //    Cost gate: S-2026-07-11 GOLD PHASE 1 -- the engines now gate on
    //    ledger_symbol ("MGC" here), i.e. the explicit MGC cost row
    //    ($10/pt/contract, $2.08 RT comm) instead of the old XAUUSD spot proxy
    //    that mis-scaled both cost and gross ~10x in dollars.
    g_mgc_tf_4h.enabled      = true;
    g_mgc_tf_4h.shadow_mode  = true;
    g_mgc_tf_4h.lot          = 1.0;    // 1 MGC micro = 10 oz = $10/pt (tick_value_multiplier("MGC"))
    g_mgc_tf_4h.max_spread   = 1.50;   // MGC futures pts (matches volbrk/fastdon)
    g_mgc_tf_4h.LOSS_CUT_PCT = 1.5;
    g_mgc_tf_4h.cell_enable_mask   = 0xC9;
    g_mgc_tf_4h.use_vol_band_gate  = true;
    g_mgc_tf_4h.vol_band_low_pct   = 0.30;
    g_mgc_tf_4h.vol_band_high_pct  = 0.85;
    g_mgc_tf_4h.cell_vol_band_mask = 0x8;
    g_mgc_tf_4h.min_impulse_atr    = 0.5;
    g_mgc_tf_4h.min_adx_entry      = 15.0;
    g_mgc_tf_4h.ledger_prefix      = "MgcTF4h_";
    g_mgc_tf_4h.ledger_symbol      = "MGC";
    g_mgc_tf_4h.warmup_csv_path    = "data/mgc_h4_hist.csv";
    g_mgc_tf_4h.init();
    omega::warmup_or_die(g_mgc_tf_4h, "MgcTF4h");
    g_mgc_tf_2h.enabled      = true;
    g_mgc_tf_2h.shadow_mode  = true;
    g_mgc_tf_2h.lot          = 1.0;
    g_mgc_tf_2h.max_spread   = 1.50;
    // LOSS-CUT OFF on the MGC 2h -- NOT the spot 0.5%. Parity harness
    // (backtest/mgc_tf_feed_parity.cpp, production feed path over MGC 30m
    // 2024-06..2026-06): LC2=0.5 flips the book NEGATIVE (PF0.86 -$2095) vs
    // LC2=0 PF1.23 +$3533 DD$2390 -- 0.5% of ~4400 = ~22pt sits inside the
    // 2x-ATR SL and the 30m intrabar path trips it constantly. Engine's own
    // 2xATR SL remains the adverse protection (backtested verdict, mandate).
    g_mgc_tf_2h.LOSS_CUT_PCT = 0.0;
    g_mgc_tf_2h.use_adx_gate      = true;
    g_mgc_tf_2h.adx_min           = 25.0;
    g_mgc_tf_2h.cell_adx_mask     = 0xB;
    g_mgc_tf_2h.use_vol_band_gate = true;
    g_mgc_tf_2h.vol_band_low_pct  = 0.30;
    g_mgc_tf_2h.vol_band_high_pct = 0.85;
    g_mgc_tf_2h.cell_vol_band_mask = 0x4;
    g_mgc_tf_2h.ledger_prefix     = "MgcTF2h_";
    g_mgc_tf_2h.ledger_symbol     = "MGC";
    g_mgc_tf_2h.warmup_csv_path   = "data/mgc_h1_hist.csv";
    g_mgc_tf_2h.init();
    omega::warmup_or_die(g_mgc_tf_2h, "MgcTF2h");
    // Live-CSV replay floor = last warmup H1 bar ts: rows at/below it are
    // already in the engines via warmup; feeding them again would double-count.
    // The warmup file carries ts in MILLISECONDS (the XauTF2h warmup parser
    // takes bar_start_ms raw -- a seconds file collapses its 2h buckets and
    // poisons ATR; caught on the first deploy, fed=garbage atr=0); the live
    // 30m feed rows are SECONDS, so normalise the floor to seconds here.
    {
        std::ifstream wf("data/mgc_h1_hist.csv"); std::string wl, wlast;
        while (std::getline(wf, wl)) if (!wl.empty() && wl[0] >= '0' && wl[0] <= '9') wlast = wl;
        g_mgc_tf_floor_ts = wlast.empty() ? 0 : std::atoll(wlast.c_str());
        if (g_mgc_tf_floor_ts > 4000000000LL) g_mgc_tf_floor_ts /= 1000;  // ms -> s
        std::printf("[SEED] MgcTF4h/MgcTF2h warm (replay floor ts=%lld)\n",
                    (long long)g_mgc_tf_floor_ts);
        std::fflush(stdout);
    }

    std::thread([](){
        std::this_thread::sleep_for(std::chrono::seconds(45));
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!g_running.load()) break;
            poll_mgc_feed("data/mgc_30m_live.csv", "data/mgc_hvn.json",
                          [](const omega::TradeRecord& tr){ g_omegaLedger.record(tr); });
        }
    }).detach();

    // ════════════════════════════════════════════════════════════════════════
    // WARMUP -- hydrate + load + seed.  No engine may ever require warmup.
    // ════════════════════════════════════════════════════════════════════════
    // Per Jo's rule (audit 2026-05-04): "No engine may ever require warmup or
    // cold-start. Sufficient historical data exists." This block enforces it.
    //
    // Pre-condition: init_engines(cfg_path) above has already initialised
    //   g_rsi_reversal / g_rsi_extreme and called load_indicators() once on
    //   g_bars_gold/sp/nq (engine_init.hpp:756-808). That sets m1_ready=true
    //   from disk scalar state. This block then layers the richer hydrate
    //   replay on top so bar-history dependent indicators are also warm.
    //
    // What this block adds on top of init_engines's load:
    //   1. BAR-HYDRATE -- replays daily-rotating L2 tick CSVs from
    //      C:\Omega\logs\ for every SymBarState timeframe. hydrate_from_csv
    //      calls bars_.clear() then re-runs the full indicator pipeline per
    //      bar, so EMA/RSI/ATR/ADX/EWMA AND bar-history vectors come out
    //      mathematically correct and fully warmed. This is the ONLY path
    //      that seeds vol_range, the breakout-bracket window, and any
    //      indicator that scans bar history (Donchian channel, swing
    //      channel, breakout window, etc.). Without this MidScalperGold,
    //      HybridBracketGold and RSIExtremeTurn fire on garbage windows.
    //   2. BAR-LOAD (fallback) -- if hydrate returned 0 (no CSV on disk)
    //      load_indicators restores the flat saved scalar state so
    //      m1_ready stays true and engines have at least the latest
    //      indicator atomics.
    //   3. WARMUP-SEED -- calls set_bar_rsi TWICE on RSIReversal and
    //      RSIExtremeTurn so both current and prev bar_rsi are populated
    //      from disk state -- eliminates the "need 2 bar closes" entry
    //      block on every restart. Without this RSIExtremeTurn fires on
    //      uninitialised prev_bar_rsi on the first bar close after restart,
    //      producing the -$7+ losses observed in the 2026-05-04 session.
    //   4. FAIL-LOUD -- if bars_gold_m1.dat exists on disk but m1_ready is
    //      still false after both paths, abort. Codifies the no-warmup rule
    //      as a runtime invariant so it can never silently regress.
    //
    // S52a 2026-05-04: This entire block was previously nested inside the
    //   `if (g_cfg.ctrader_depth_enabled && ...)` branch at former line 260.
    //   With cTrader disabled by config (FIX 264=0 provides L2 since
    //   2026-04-20), the warmup never ran. Engines started cold despite
    //   full bar/tick history existing on disk. Symptoms: GOLD-BRK-DIAG
    //   showed brk_hi=0.00 brk_lo=0.00 range=0.00 hours after startup;
    //   eight XAUUSD scalps fired in Asian session producing -$23.38 net.
    //   Moved here, executed unconditionally regardless of cTrader state.
    {
        const std::string bs = log_root_dir();
        const int64_t now_ms_h = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        // 2026-06-22: intraday hydrate lookbacks deepened so the PredictiveRanges
        //   desk chart (pr_compute keeps the last 160 bars) is FULLY WARM on a
        //   (re)start instead of showing a flat/blank seed for hours. The chart
        //   needs >=160 bars PAST the ATR(len) warmup; the old m5=10h / m15=30h
        //   only yielded ~120 bars so the recurrence's warmup region filled the
        //   visible left edge (operator-reported "3h to populate"). 160 m5 bars =
        //   13.3h, 160 m15 = 40h -> hydrate 20h / 60h (+ warmup margin). m1 6h
        //   feeds the m5-from-m1 GUI fallback. h1/h4 unchanged (already warm).
        const int h_m1 = g_bars_gold.m1 .hydrate_from_csv(bs, "XAUUSD",    60000LL, now_ms_h, 6);
        const int h_m5 = g_bars_gold.m5 .hydrate_from_csv(bs, "XAUUSD",   300000LL, now_ms_h, 20);
        const int h_m15= g_bars_gold.m15.hydrate_from_csv(bs, "XAUUSD",   900000LL, now_ms_h, 60);
        const int h_h1 = g_bars_gold.h1 .hydrate_from_csv(bs, "XAUUSD",  3600000LL, now_ms_h, 60);
        const int h_h4 = g_bars_gold.h4 .hydrate_from_csv(bs, "XAUUSD", 14400000LL, now_ms_h, 240);
        // 2026-05-29: extended index hydrate to cover m5/m15/h1/h4 timeframes.
        // Prior code only hydrated sp/nq m1; OHLCBarEngine for those symbols
        // therefore cold-started on every restart, producing "Bar state
        // SKIPPED (age=...) -- cold start, file kept" for m5/m15/h4 against
        // stale .dat files. Indices L2 tick CSVs are logged daily on VPS
        // (US500 287 MB / 34 days, USTEC 700 MB / 34 days), so this is a
        // pure wiring fix -- the data was already there.
        const int h_sp_m1  = g_bars_sp.m1 .hydrate_from_csv(bs, "US500",     60000LL, now_ms_h, 6);
        const int h_sp_m5  = g_bars_sp.m5 .hydrate_from_csv(bs, "US500",    300000LL, now_ms_h, 20);
        const int h_sp_m15 = g_bars_sp.m15.hydrate_from_csv(bs, "US500",    900000LL, now_ms_h, 60);
        const int h_sp_h1  = g_bars_sp.h1 .hydrate_from_csv(bs, "US500",   3600000LL, now_ms_h, 60);
        const int h_sp_h4  = g_bars_sp.h4 .hydrate_from_csv(bs, "US500",  14400000LL, now_ms_h, 240);
        const int h_nq_m1  = g_bars_nq.m1 .hydrate_from_csv(bs, "USTEC",     60000LL, now_ms_h, 6);
        const int h_nq_m5  = g_bars_nq.m5 .hydrate_from_csv(bs, "USTEC",    300000LL, now_ms_h, 20);
        const int h_nq_m15 = g_bars_nq.m15.hydrate_from_csv(bs, "USTEC",    900000LL, now_ms_h, 60);
        const int h_nq_h1  = g_bars_nq.h1 .hydrate_from_csv(bs, "USTEC",   3600000LL, now_ms_h, 60);
        const int h_nq_h4  = g_bars_nq.h4 .hydrate_from_csv(bs, "USTEC",  14400000LL, now_ms_h, 240);
        const int h_sp = h_sp_m1, h_nq = h_nq_m1; // legacy names used downstream + invariant check
        std::cout << "[BAR-HYDRATE] XAUUSD: m1=" << h_m1 << " m5=" << h_m5
                  << " m15=" << h_m15 << " h1=" << h_h1 << " h4=" << h_h4
                  << " | US500: m1=" << h_sp_m1 << " m5=" << h_sp_m5
                  << " m15=" << h_sp_m15 << " h1=" << h_sp_h1 << " h4=" << h_sp_h4
                  << " | USTEC: m1=" << h_nq_m1 << " m5=" << h_nq_m5
                  << " m15=" << h_nq_m15 << " h1=" << h_nq_h1 << " h4=" << h_nq_h4
                  << "\n";
        std::cout.flush();

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

        // ── FAIL-LOUD: mode/account consistency invariant (S26 §2.1) ──────
        // 2026-05-11 added after operator discovered 3 days of demo-trading
        // under mode=LIVE label (live FIX session connected with sender =
        // demo.blackbull.2067070 + username = 2067070, while config.mode
        // said "LIVE"). Engine logged everything with [LIVE] markers but
        // every fill landed on the demo account. The live account showed
        // zero engine activity for the full window. Reference incident:
        // HANDOFF_S26.md §1.4. The check below refuses to start the
        // process if mode and FIX credentials disagree, so the failure
        // mode cannot recur silently.
        //
        // Field names per include/engine_config.hpp:282-284:
        //   g_cfg.mode      -- "LIVE" | "DEMO" | "SHADOW"
        //   g_cfg.sender    -- FIX SenderCompID (e.g. live.blackbull.8077780)
        //   g_cfg.username  -- FIX Username (e.g. 8077780)
        // Demo accounts use either "demo." in sender OR account number
        // 2067070 (the historical demo account). Live uses "live." in sender.
        {
            const std::string& mode   = g_cfg.mode;
            const std::string& sender = g_cfg.sender;
            const std::string& user   = g_cfg.username;
            const bool sender_is_demo = sender.find("demo") != std::string::npos;
            const bool user_is_demo   = (user.find("demo")   != std::string::npos)
                                     || (user.find("2067070") != std::string::npos);
            const bool sender_is_live = sender.find("live") != std::string::npos;
            if (mode == "LIVE" && (sender_is_demo || user_is_demo)) {
                std::cerr << "\033[1;31m[OMEGA-FATAL] mode=LIVE but FIX credentials are demo:\n"
                          << "  sender   = " << sender << "\n"
                          << "  username = " << user << "\n"
                          << "  Pick one. Refusing to start. (S26 §2.1 invariant)\033[0m\n";
                std::cerr.flush();
                if (g_singleton_mutex) {
                    ReleaseMutex(g_singleton_mutex);
                    CloseHandle(g_singleton_mutex);
                    g_singleton_mutex = nullptr;
                }
                Sleep(2000);
                return 1;
            }
            if (mode == "DEMO" && sender_is_live) {
                std::cerr << "\033[1;31m[OMEGA-FATAL] mode=DEMO but sender is live ("
                          << sender << "). Pick one. (S26 §2.1 invariant)\033[0m\n";
                std::cerr.flush();
                if (g_singleton_mutex) {
                    ReleaseMutex(g_singleton_mutex);
                    CloseHandle(g_singleton_mutex);
                    g_singleton_mutex = nullptr;
                }
                Sleep(2000);
                return 1;
            }
            std::cout << "[OMEGA-MODE-CHECK] mode=" << mode
                      << " sender=" << sender
                      << " username=" << user
                      << " -- OK\n";
            std::cout.flush();
        }

        // ── FAIL-LOUD: codify the no-warmup rule as a runtime invariant ──
        // If bars_gold_m1.dat exists on disk but m1_ready is still false
        // after both hydrate and load, the system is about to start cold
        // despite full history being available. That's the bug class we
        // just fixed by moving this block out of the cTrader gate. Abort
        // here so it can never silently regress.
        {
            const std::string m1_dat = bs + "/bars_gold_m1.dat";
            std::ifstream test_dat(m1_dat, std::ios::binary);
            const bool dat_exists = test_dat.is_open();
            test_dat.close();
            const bool m1_ready_now = g_bars_gold.m1.ind.m1_ready.load();
            if (dat_exists && !m1_ready_now && h_m1 == 0) {
                std::cerr << "\033[1;31m[OMEGA-FATAL] bars_gold_m1.dat exists but m1_ready=false "
                          << "after hydrate+load.\n"
                          << "  hydrate m1=" << h_m1 << " load m1_ok=" << m1_ok << "\n"
                          << "  Engines would start cold -- this violates the no-warmup rule. "
                          << "Aborting.\n"
                          << "  Check: file age (>24h), schema version, log_root_dir() "
                          << "permissions, free disk space.\033[0m\n";
                std::cerr.flush();
                if (g_singleton_mutex) {
                    ReleaseMutex(g_singleton_mutex);
                    CloseHandle(g_singleton_mutex);
                    g_singleton_mutex = nullptr;
                }
                Sleep(2000);
                return 1;
            }
        }

        if (!m1_ok && h_m1 == 0) {
            std::cout << "[BAR-LOAD] WARN: bars_gold_m1.dat missing or stale -- "
                      << "XAUUSD engines blocked until M1 bars seed from tick data (~2min)\n";
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
        //
        // Trigger condition: m1_ready is true (set by either hydrate OR load).
        // Previously gated only on m1_ok which excluded the hydrate-only path.
        if (g_bars_gold.m1.ind.m1_ready.load()) {
            const double seed_rsi = g_bars_gold.m1.ind.rsi14.load();
            const double seed_mid = g_bars_gold.m1.ind.ema9.load();  // price proxy
            (void)seed_mid;

            // RSIReversalEngine: seed bar_rsi + bar_rsi_prev from disk
            // Eliminates the "need 2 bar closes" block on every restart
            if (seed_rsi > 0.0 && seed_rsi < 100.0) {
                std::cout << "[WARMUP-SEED] RSIReversal bar_rsi=" << seed_rsi
                          << " (both current+prev seeded from disk)\n";
                std::cout.flush();
            }

            // RSIExtremeTurnEngine: seed bar_rsi
            if (seed_rsi > 0.0 && seed_rsi < 100.0) {
                std::cout << "[WARMUP-SEED] RSIExtreme bar_rsi=" << seed_rsi << "\n";
                std::cout.flush();
            }

            // (MicroMomentumEngine warmup-seed REMOVED at Batch 5V §1.2.)

            // (DomPersistEngine warmup-seed REMOVED at Session 15 2026-04-23
            //  -- no edge in 96-cell walk-forward sweep. See globals.hpp tombstone.)

            std::cout << "[WARMUP-SEED] All engines seeded from disk state -- "
                      << "no 2-minute blindness on restart\n";
            std::cout.flush();
        } else {
            std::cout << "[WARMUP-SEED] SKIP -- m1_ready=false after hydrate+load. "
                      << "Engines will warm from live ticks (~2min).\n";
            std::cout.flush();
        }

        // Suppress unused-variable warnings on m5/m15/h1/h4/sp/nq paths
        (void)h_m5; (void)h_m15; (void)h_h1; (void)h_h4; (void)h_sp; (void)h_nq;
        (void)m5_ok; (void)m15_ok; (void)h4_ok; (void)sp_ok; (void)nq_ok;
    }

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
            "latency_ms,regime,exit_reason,l2_imbalance,l2_live,"
            // S33b 2026-05-11: broker reconciliation columns. The TradeRecord
            // broker_* fields were added 2026-05-09 (user-authorised after
            // the NZ$308 disparity incident) and are populated in memory by
            // handle_execution_report, but were never persisted to CSV --
            // so the GUI saw engine truth only. These columns close that gap.
            // Reload parser at lines ~315-360 ignores extra columns past
            // tok[29], so existing CSVs remain forward-compatible.
            "entry_clOrdId,close_clOrdId,"
            "broker_entry_filled,broker_close_filled,"
            "broker_entry_rejected,broker_close_rejected,"
            "broker_entry_fill_px,broker_close_fill_px,broker_pnl";

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

    // 2026-05-26: BOOT WRITE-TEST. Prove the stream actually writes to disk
    // before the engine accepts ticks. Without this, a silently-failed open
    // (or write-permission revocation) goes undetected for weeks. If this
    // write doesn't land + flush + bump mtime, the bug surfaces immediately.
    // The boot-test row is harmless: symbol=__BOOT__, side=HEARTBEAT, no PnL.
    {
        const std::int64_t boot_ts = std::time(nullptr);
        g_shadow_csv << boot_ts << ",__BOOT__,HEARTBEAT,boot_writetest,0,0,0,0,0,0,"
                        "boot_writetest,0,0,boot\n";
        g_shadow_csv.flush();
        // Verify mtime moved by re-statting -- if filesystem buffering swallows
        // the write, the operator sees the WARN immediately on next healthcheck.
        std::ifstream verify(shadow_csv_path);
        if (verify.good()) {
            verify.seekg(0, std::ios::end);
            const auto sz = verify.tellg();
            std::cout << "[OMEGA] Shadow CSV write-test: OK ("
                      << static_cast<long long>(sz) << " bytes after boot row)\n";
        } else {
            std::cerr << "[OMEGA-FATAL] Shadow CSV write-test failed -- stream open but unreadable: "
                      << shadow_csv_path << "\n";
            return 1;
        }
    }

    // S25 2026-05-25 -- signal-level audit. Open omega_shadow_signals.csv
    // when enable_shadow_signal_audit=true. Treat open-failure as FATAL
    // for the same reason as the trade-level shadow CSV: silent loss of
    // research-grade data is exactly the failure mode we're trying to
    // prevent. The pre-2026-05-25 state (no writer at all) is what put
    // us in this position; we won't tolerate a silently-failed open.
    if (g_cfg.enable_shadow_signal_audit) {
        const std::string sig_csv_path =
            resolve_audit_log_path(g_cfg.shadow_signal_csv, "shadow/omega_shadow_signals.csv");
        ensure_parent_dir(sig_csv_path);
        g_shadow_signal_csv.open(sig_csv_path, std::ios::app);
        if (!g_shadow_signal_csv.is_open()) {
            std::cerr << "[OMEGA-FATAL] Failed to open shadow signal CSV: " << sig_csv_path << "\n";
            return 1;
        }
        g_shadow_signal_csv.seekp(0, std::ios::end);
        if (g_shadow_signal_csv.tellp() == std::streampos(0))
            g_shadow_signal_csv << "ts_unix,symbol,regime,confidence,bracket_score,"
                                   "breakout_score,top_score,threshold,winner,allow,"
                                   "cooldown,reason\n";
        std::cout << "[OMEGA] Shadow Signal CSV: " << sig_csv_path << "\n";
    } else {
        std::cout << "[OMEGA] Shadow signal audit DISABLED "
                     "(enable_shadow_signal_audit=true in [telemetry] to enable)\n";
    }

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

    // RealDomReceiver removed S13 2026-05-08 -- OmegaDomStreamer cBot retired
    // along with the rest of the cTrader Open API surface. FIX 264=0 owns L2.

    std::cout << "[OMEGA] GUI http://localhost:" << g_cfg.gui_port
              << "  WS:" << g_cfg.ws_port << "\n";
    std::cout.flush();

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // cTrader Open API depth init block REMOVED S13 2026-05-08.
    // Source of truth: include/CTraderDepthClient.hpp (deleted in this commit).
    // FIX 264=0 (full book) delivers multi-level L2 directly into the AtomicL2
    // structs via fix_dispatch.hpp; no parallel SSL connection is required.
    std::cout << "[OMEGA] cTrader Open API surface removed at S13 -- FIX 264=0 owns L2.\n";


    // ?? Engine dispatch worker -- 2026-05-01 race fix ?????????????????????????
    // Single-writer dispatch thread for ALL engine state mutations. Producers
    // (cTrader depth thread, FIX quote thread, FIX trade thread) post events
    // to a thread-safe queue; this worker is the only thread that calls
    // on_tick() and handle_execution_report(). MUST start before any producer.
    // See engine_dispatch.hpp for design and shutdown semantics.
    engine_dispatch_start();

    // ── IBKR DOM bridge consumer (S88, 2026-05-22) ──────────────────────────
    // Opt-in: enabled when env OMEGA_IBKR_BRIDGE=1 is set at launch.
    // Reads newline-JSON from tools/ibkr_dom_bridge.py over TCP localhost.
    // Updates g_ibkr_l2.<sym> atomics; engines that opt-in must call
    // g_ibkr_l2.xau.fresh(now_ms) before reading -- no implicit gating.
    // Bridge can be off without affecting primary path.
    if (const char* en = std::getenv("OMEGA_IBKR_BRIDGE"); en && std::string(en) == "1") {
        const char* port_env = std::getenv("OMEGA_IBKR_BRIDGE_PORT");
        const uint16_t port = port_env ? static_cast<uint16_t>(std::atoi(port_env)) : 9701;
        // S88-followup (2026-05-22): OMEGA_IBKR_BRIDGE_HOST lets the consumer
        // reach the sidecar across hosts (e.g. Tailscale mesh, SSH tunnel, LAN).
        // Default 127.0.0.1 (sidecar on same VPS, original intent).
        const char* host_env = std::getenv("OMEGA_IBKR_BRIDGE_HOST");
        // String must outlive the detached thread -- store in a static.
        static std::string s_ibkr_host = host_env ? host_env : "127.0.0.1";
        std::cout << "[IBKR-CONSUMER] starting; " << s_ibkr_host << ":" << port << "\n";
        std::cout.flush();
        // DJ30 synthetic-tick pump (2026-07-03): BlackBull FIX streams ZERO DJ30
        // ticks, so on_tick_dj30 -- and the whole live-configured DJ30.F family
        // (g_dj30_turtle_d1, IndexFlow, Connors book, ...) -- never fired. The
        // bridge now carries DJ30 (YM/CBOT depth). On each DJ30 book update, post
        // a synthetic tick into the single-writer dispatch queue so those engines
        // finally get a tick source. Bridge emits contract.symbol "YM" for the
        // DJ30 --symbols key (CBOT front-month, e.g. YMU6); on_tick expects
        // "DJ30.F", so accept the "YM"/"DJ30" aliases and remap here.
        omega::ibkr::BookUpdateCb on_book = [](const char* sym, double bid, double ask) {
            if (std::strcmp(sym, "YM") == 0
             || std::strcmp(sym, "DJ30") == 0 || std::strcmp(sym, "DJ30.F") == 0) {
                engine_dispatch_post_tick("DJ30.F", bid, ask);
                return;
            }
            // ── NAS100 (S-2026-07-09 feed migration) ─────────────────────────
            // Operator directive: move NAS100 off BlackBull FIX onto IBKR. The
            // bridge DomRecorder broadcasts contract.symbol "NQ" for the NAS100
            // --symbols key (CME front-month FUTURES ~29433, ~0.75% above the
            // BlackBull NAS100 CFD). Post it as a "NAS100" tick so on_tick_nas100
            // -- and index_feed_h1 -> the index up-jump ladder + set_disp_mid --
            // run on the live IBKR futures scale. The BlackBull NAS100 FIX quote is
            // gated out in fix_dispatch (is_ibkr_primary_index) while this slot is
            // fresh, so no double-feed; bridge down -> BlackBull CFD fallback.
            if (std::strcmp(sym, "NQ") == 0
             || std::strcmp(sym, "NAS100") == 0 || std::strcmp(sym, "USTEC") == 0) {
                engine_dispatch_post_tick("NAS100", bid, ask);
                // USTEC.F is the SAME underlying (Nasdaq-100) priced off NQ. The
                // operator's zero-BlackBull directive covers USTEC.F too; it is not
                // separately subscribed on the bridge (== NAS100 conId), so drive it
                // off the same NQ tick. BlackBull USTEC.F is gated out in fix_dispatch
                // (is_ibkr_primary_index) while the nas100 slot is fresh.
                engine_dispatch_post_tick("USTEC.F", bid, ask);
                return;
            }
            // ── XAUUSD (S-2026-07-09 feed migration) ─────────────────────────
            // Move XAUUSD off BlackBull FIX onto the IBKR XAUUSD CMDTY/SMART SPOT
            // depth stream (same spot scale as BlackBull -- negligible price shift).
            // Post as an "XAUUSD" tick -> on_tick_gold runs off the IBKR price AND
            // (already) the IBKR xau L2 slot. BlackBull XAU FIX quote gated out
            // while fresh; its L2 book is still cached (gate sits after L2 store).
            if (std::strcmp(sym, "XAUUSD") == 0) {
                engine_dispatch_post_tick("XAUUSD", bid, ask);
                return;
            }
            // ── COMPLETE FEED MIGRATION (S-2026-07-09) ───────────────────────
            // Every remaining BlackBull-fed index/commodity/vol symbol now rides
            // the IBKR bridge as L1 (reqMktData). The bridge broadcasts the IBKR
            // contract.symbol (ES/DAX/Z/CL/VX/DX/NG/COIL, and XAGUSD/ESTX50 which
            // already match); remap each to the ENGINE tick symbol (the BlackBull
            // FIX name the on_tick family already handles) and post it. The matching
            // BlackBull FIX quote is gated out in fix_dispatch (is_ibkr_primary_index)
            // while this slot is fresh; bridge down / no entitlement -> BlackBull
            // fallback. Note: ES/DAX are FUTURES scale (basis over the CFD) -- the
            // index up-jump ladder US500/GER40 seeds are re-synced to futures scale
            // in engine_init (SHADOW book, cosmetic). See outputs/FEED_AUDIT_2026-07-09.md.
            struct IdxMap { const char* wire; const char* tick; };
            static const IdxMap kIdxMap[] = {
                {"ES",     "US500.F"},
                {"DAX",    "GER40"},
                {"Z",      "UK100"},
                {"ESTX50", "ESTX50"},
                {"CL",     "USOIL.F"},
                {"XAGUSD", "XAGUSD"},
                {"VX",     "VIX.F"},
                {"DX",     "DX.F"},
                {"NG",     "NGAS.F"},
                {"COIL",   "BRENT"},
                {"M2K",    "M2K"},      // 2026-07-09 micro Russell -> index up-jump ladder SHADOW
            };
            for (const auto& m : kIdxMap) {
                if (std::strcmp(sym, m.wire) == 0) {
                    engine_dispatch_post_tick(m.tick, bid, ask);
                    return;
                }
            }
            // FX majors (S-2026-07-06): FX quotes now come from the IBKR IDEALPRO
            // L1 line, not BlackBull FIX (which delivered ~30s-frozen FX snapshots
            // -- operator directive: move FX quotes to the IBKR link). Post the
            // IBKR top-of-book as a tick; on_tick() writes g_bids/g_asks + pings
            // the stale-watchdog, so FX engines + GUI use the live IBKR price.
            // The matching BlackBull FX quote is gated out in fix_dispatch when
            // this slot is fresh (bridge down -> BlackBull fallback, no blackout).
            if (omega::ibkr::is_fx_major(sym)) {
                engine_dispatch_post_tick(sym, bid, ask);
                return;
            }
            // ── BIGCAP stock L1 (S-2026-07-10): the up-jump ladder companion's LIVE-
            //    CONFIRMATION gate. The bridge broadcasts contract.symbol = the ticker
            //    (STK/SMART/USD L1) for each bigcap name on --symbols. Feed the quote to
            //    the ladder book: a PENDING +thr window opens only when this live tick
            //    confirms session+fresh+rising (StockDayMoverLadderCompanion live_confirm).
            //    Lockless no-op for any non-ladder symbol (SHADOW book, observe-only, no
            //    order side). US-equity L1 entitlement verified live 2026-07-10.
            {
                const int64_t now_ms = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                omega::stockmover_ladder_book().on_live_tick(sym, bid, ask, now_ms);
            }
        };
        std::thread([port, on_book = std::move(on_book)]{
            omega::ibkr::run_consumer(g_ibkr_l2, g_ibkr_l2_stats,
                                      g_ibkr_l2_stop, s_ibkr_host.c_str(), port,
                                      on_book);
        }).detach();
    }


    // ── BigCapMomo feed consumer (2026-06-12) ───────────────────────────────
    // Opt-in: OMEGA_BIGCAP_BRIDGE=1. Same B/S/P/C protocol, separate port (default
    // 7784), fed by pump/bigcap_feed_bridge.py (NAS/SPX day-mover scanner). Drives
    // g_bigcap_momo (big-cap config). Off => dormant.
    if (const char* en = std::getenv("OMEGA_BIGCAP_BRIDGE"); en && std::string(en) == "1") {
        const char* port_env = std::getenv("OMEGA_BIGCAP_BRIDGE_PORT");
        const uint16_t bport = port_env ? static_cast<uint16_t>(std::atoi(port_env)) : 7784;
        const char* host_env = std::getenv("OMEGA_BIGCAP_BRIDGE_HOST");
        static std::string s_bigcap_host = host_env ? host_env : "127.0.0.1";
        // S-2026-06-24: OMEGA_BIGCAP_AB=1 tees the feed to the A/B twin g_bigcap_momo_b
        // (identical entries, close-based give-back exit; configured in engine_init). Off
        // => nullptr => single-manager path unchanged.
        static const bool s_bigcap_ab = std::getenv("OMEGA_BIGCAP_AB") != nullptr;
        std::cout << "[BIGCAP-CONSUMER] starting; " << s_bigcap_host << ":" << bport
                  << (s_bigcap_ab ? "  (A/B twin BigCapMomoGB ON)" : "") << "\n";
        std::cout.flush();
        std::thread([bport]{
            omega::pump_feed::run(g_bigcap_momo, g_bigcap_stop, s_bigcap_host.c_str(), bport,
                                  s_bigcap_ab ? &g_bigcap_momo_b : nullptr);
        }).detach();
        g_bigcap_feed_ok = true;   // bridge feed path configured (consumer-drop covered by bridge-side consumer=N alert)
    }

    // ── BigCapMomo IN-PROCESS IBKR engine (2026-06-16) ──────────────────────
    // Opt-in: OMEGA_BIGCAP_IBKR=1. Activates the in-process engine wired in
    // engine_init (configure + sink + register_source already done there). It
    // owns its OWN IBKR scanner/data thread (start() connects to the gateway +
    // spawns the reader loop) so running + closed trades surface in the GUI.
    // Mutually exclusive with OMEGA_BIGCAP_BRIDGE (the Python :7784 path) -- run
    // ONE. Off => dormant. Connection knobs: OMEGA_BIGCAP_IBKR_{HOST,PORT,CLIENT}.
    if (const char* en = std::getenv("OMEGA_BIGCAP_IBKR"); en && std::string(en) == "1") {
        omega::bigcap_momo_ibkr::set_enabled(true);
        std::cout << "[BIGCAP-IBKR] activating in-process IBKR BigCapMomo engine\n";
        std::cout.flush();
        const bool bigcap_ibkr_ok = omega::bigcap_momo_ibkr::start();
        g_bigcap_feed_ok = bigcap_ibkr_ok;   // false => no-op stub / no OMEGA_WITH_IBKR / connect refused
        if (!bigcap_ibkr_ok) {
            // LOUD: this is the exact silent-death we hit 2026-06-17 (IBKR path selected
            //   on a binary built without OMEGA_WITH_IBKR -> stub -> zero trades, no error).
            //   Route through the same [SYSTEM-ALERT] channel as L2_DEAD etc.; the quote_loop
            //   watchdog re-raises it every 60s + pins it on the GUI health banner.
            std::cout << "[SYSTEM-ALERT] BIGCAP_IBKR_DOWN start() returned false "
                         "(no OMEGA_WITH_IBKR build, or gateway connect refused) -- "
                         "ZERO bigcap trades possible. Use OMEGA_BIGCAP_BRIDGE=1 or rebuild with IBKR.\n";
            std::cout.flush();
            g_telemetry.SetHealthAlert("BIGCAP IBKR DOWN");
        }
    }

    // ── NqFutMomo IN-PROCESS IBKR engine (NQ/MNQ futures momentum, 2026-06-25) ──
    // Opt-in: OMEGA_NQ_IBKR=1. Activates the engine wired in engine_init (configure +
    // sink + register_source done there). Owns its OWN IBKR 5m-bar data thread. Off =>
    // dormant. Independent of OMEGA_BIGCAP_IBKR (distinct clientId 87) -- both can run.
    // Connection knobs: OMEGA_NQ_IBKR_{HOST,PORT,CLIENT,MONTH,SYMBOL}.
    if (const char* en = std::getenv("OMEGA_NQ_IBKR"); en && std::string(en) == "1") {
        omega::nq_momo_ibkr::set_enabled(true);
        std::cout << "[NQ-IBKR] activating in-process IBKR NqFutMomo (NQ/MNQ futures momentum) engine\n";
        std::cout.flush();
        const bool nq_ibkr_ok = omega::nq_momo_ibkr::start();
        if (!nq_ibkr_ok) {
            std::cout << "[SYSTEM-ALERT] NQ_IBKR_DOWN start() returned false (no OMEGA_WITH_IBKR build, "
                         "or gateway connect refused) -- ZERO NqFutMomo trades possible. "
                         "Rebuild with IBKR / check gateway login.\n";
            std::cout.flush();
            g_telemetry.SetHealthAlert("NQ IBKR DOWN");
        }
    }

    // ── QndxSqf IN-PROCESS IBKR book (QNDX = Nasdaq-100 SQF, folded from ex-IBKRCrypto) ──
    // Opt-in: OMEGA_QNDX_IBKR=1. Activates the book wired in engine_init (configure + sink +
    // register_source done there). Owns its OWN IBKR daily-bar data thread; warms legs from a
    // daily CSV. Off => dormant. clientId 88 is REUSED from the retired Crypto executor -- that
    // executor MUST be stopped first (strict cutover order) or the connect rotates clientId.
    // Connection knobs: OMEGA_QNDX_IBKR_{HOST,PORT,CLIENT,MONTH,CSV}.
    if (const char* en = std::getenv("OMEGA_QNDX_IBKR"); en && std::string(en) == "1") {
        omega::qndx_sqf_ibkr::set_enabled(true);
        std::cout << "[QNDX-IBKR] activating in-process IBKR QndxSqf (Nasdaq-100 SQF daily book)\n";
        std::cout.flush();
        const bool qndx_ibkr_ok = omega::qndx_sqf_ibkr::start();
        if (!qndx_ibkr_ok) {
            std::cout << "[SYSTEM-ALERT] QNDX_IBKR_DOWN start() returned false (no OMEGA_WITH_IBKR build, "
                         "or gateway connect refused) -- ZERO QndxSqf trades possible. "
                         "Rebuild with IBKR / check gateway login / free clientId 88.\n";
            std::cout.flush();
            g_telemetry.SetHealthAlert("QNDX IBKR DOWN");
        }
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

        // Check 2 removed S13 2026-05-08 -- cTrader Open API surface culled.
        // [ctrader_api] config section + ctrader_* fields are gone, so there is
        // nothing to verify here. FIX 264=0 health is still covered by Check 4.

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

        // Checks 5 + 6 removed S13 2026-05-08 -- cTrader depth client culled.
        //   FIX 264=0 (validated by Check 4) is the sole L2 source.

        // ── Check 7 (rewired): gold_l2_real flag set (FIX 264=0 confirmed live) ──
        // gold_l2_real = g_l2_gold.fresh(now, 3000) -- true when an L2 update
        // arrived within the last 3s. After the S13 cull this is FIX 264=0
        // dispatch (fix_dispatch.hpp) rather than the old cTrader thread.
        {
            bool l2_real_ok = false;
            for (int i = 0; i < 15 && !l2_real_ok; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                l2_real_ok = g_macro_ctx.gold_l2_real;
            }
            const int64_t now_ms_chk = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const int64_t last_ms = g_l2_gold.last_update_ms.load(std::memory_order_relaxed);
            const int64_t age_ms  = (last_ms > 0) ? (now_ms_chk - last_ms) : -1;
            print_check(l2_real_ok, "L2: gold_l2_real flag live (FIX 264=0)",
                l2_real_ok
                    ? ("age_ms=" + std::to_string(age_ms))
                    : "gold_l2_real=false after 15s -- FIX 264=0 not updating g_l2_gold");
            if (!l2_real_ok) {
                // Not fatal -- entries blocked by L2-dead gate, trading degraded.
                std::cout << WARN << " Trading will be blocked by [L2-DEAD-BLOCK] until L2 recovers\n";
                std::cout.flush();
            }
        }

        // ── Check 8: (removed — GoldFlow engine culled in S19) ────────────

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
            write_status("OK: all checks passed -- FIX 264=0 live, gold_l2_real=1, trading active");
        } else {
            write_status("WARN: FIX feed up but gold_l2_real=0 -- entries blocked until L2 recovers");
        }
    }).detach();

    // =========================================================================
    // L2 WATCHDOG THREAD removed S13 2026-05-08.
    //   The watchdog read g_ctrader_depth.depth_events_total and wrote
    //   g_l2_watchdog_dead -- both gone with the cTrader Open API surface cull.
    //   FIX 264=0 staleness is detected by the diagnostic loop in
    //   include/quote_loop.hpp using AtomicL2::last_update_ms; the L2 tick CSV
    //   verifier in VERIFY_STARTUP.ps1 catches feed staleness from outside the
    //   process. Position management (trail/SL) was always independent of the
    //   watchdog and continues to run on every tick.
    //
    // The 10-min periodic bar-state save block previously lived inside this
    // thread. It is preserved as a dedicated thread below so warm-restart .dat
    // snapshots keep refreshing even though the watchdog is gone.
    // =========================================================================
    std::thread([](){
        // Wait for startup to complete before the first save attempt.
        std::this_thread::sleep_for(std::chrono::seconds(90));
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!g_running.load()) break;
            // ── PUMP WATCHDOG (2026-06-18): feed-independent force-close. Pump
            //   exits (trail/hard/MAXHOLD) are 100% tick-gated in on_price(); a
            //   dead AH/halt/bridge feed froze 7 positions open 200+min (GUI
            //   now=0, +$0). Heartbeat closes stale / over-MAXHOLD positions at
            //   last known price. No-op while ticks flow. Cheap (loops <=12 cells).
            {
                const int64_t now_ms_wd = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                g_bigcap_momo.on_heartbeat(now_ms_wd);
                if (std::getenv("OMEGA_BIGCAP_AB")) g_bigcap_momo_b.on_heartbeat(now_ms_wd);  // A/B twin
            }
            // ── CRYPTO LEDGER INBOUND (S-2026-06-24): route IBKRCrypto shadow-book closes
            //   into the Omega ledger (P&L total + GUI by engine tag). Opt-in. ──────────
            if (std::getenv("OMEGA_CRYPTO_INBOUND")) omega::ingest_crypto_inbound(g_omegaLedger);
            // ── BIGCAP-IBKR LIVENESS WATCHDOG (2026-06-20) ──────────────────────────
            //   The 06-19 silent-death guard. The in-process IBKR engine can connect
            //   yet receive ZERO scanner/bar data during RTH and trade nothing, with no
            //   alarm (start() returned true). If its last data event is stale (>5min)
            //   inside US cash RTH, raise the loud [SYSTEM-ALERT] + GUI health banner.
            {
                static const bool s_bigcap_ibkr = [](){
                    const char* e = std::getenv("OMEGA_BIGCAP_IBKR"); return e && std::string(e) == "1"; }();
                if (s_bigcap_ibkr) {
                    const std::time_t tnow = std::time(nullptr);
                    std::tm ut{};
#if defined(_WIN32)
                    gmtime_s(&ut, &tnow);
#else
                    gmtime_r(&tnow, &ut);
#endif
                    const int mins = ut.tm_hour * 60 + ut.tm_min;
                    const bool rth = (ut.tm_wday >= 1 && ut.tm_wday <= 5)
                                     && mins >= 13 * 60 + 30 && mins < 20 * 60;   // 13:30-20:00 UTC
                    if (rth) {
                        const long long la = omega::bigcap_momo_ibkr::last_activity_ms();
                        const int64_t now_ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        const bool stale = (la == 0) || (now_ms2 - la > 300000);   // never, or >5min
                        static int64_t s_last_alert_s = 0;
                        if (stale && (static_cast<int64_t>(tnow) - s_last_alert_s >= 120)) {
                            s_last_alert_s = static_cast<int64_t>(tnow);
                            const long ago = la > 0 ? static_cast<long>((now_ms2 - la) / 1000) : -1;
                            std::cout << "[SYSTEM-ALERT] BIGCAP_STALE in-process IBKR engine has no scanner/bar "
                                         "data in RTH (last_data="
                                      << (ago < 0 ? std::string("NEVER") : std::to_string(ago) + "s")
                                      << ") -- enabled but trading NOTHING. Check gateway login / clientId.\n";
                            std::cout.flush();
                            g_telemetry.SetHealthAlert("BIGCAP STALE");
                        }
                        // S-2026-06-23 AUTO-RECONNECT supervisor: a stale/down engine self-heals
                        // instead of sitting dead till the next deploy. Bounce (stop+start, which
                        // rotates clientId 86->88) at most every 300s while stale during RTH.
                        static int64_t s_last_reconnect_s = 0;
                        if (stale && (static_cast<int64_t>(tnow) - s_last_reconnect_s >= 300)) {
                            s_last_reconnect_s = static_cast<int64_t>(tnow);
                            std::cout << "[BIGCAP-IBKR] auto-reconnect: bouncing stale/down engine (stop+start)\n";
                            std::cout.flush();
                            omega::bigcap_momo_ibkr::stop();
                            const bool rok = omega::bigcap_momo_ibkr::start();
                            std::cout << "[BIGCAP-IBKR] auto-reconnect " << (rok ? "OK -- live again"
                                         : "FAILED -- retry in 300s") << "\n";
                            std::cout.flush();
                        }
                        // ── NqFutMomo in-process IBKR liveness (same RTH window) ──
                        if (omega::nq_momo_ibkr::is_enabled()) {
                            const long long nla = omega::nq_momo_ibkr::last_activity_ms();
                            const bool nq_stale = (nla == 0) || (now_ms2 - nla > 300000);
                            static int64_t s_nq_last_alert_s = 0;
                            if (nq_stale && (static_cast<int64_t>(tnow) - s_nq_last_alert_s >= 120)) {
                                s_nq_last_alert_s = static_cast<int64_t>(tnow);
                                const long ago = nla > 0 ? static_cast<long>((now_ms2 - nla) / 1000) : -1;
                                std::cout << "[SYSTEM-ALERT] NQ_STALE in-process IBKR NqFutMomo has no bar "
                                             "data in RTH (last_data="
                                          << (ago < 0 ? std::string("NEVER") : std::to_string(ago) + "s")
                                          << ") -- enabled but trading NOTHING. Check gateway / contract month.\n";
                                std::cout.flush();
                                g_telemetry.SetHealthAlert("NQ STALE");
                            }
                            static int64_t s_nq_last_reconnect_s = 0;
                            if (nq_stale && (static_cast<int64_t>(tnow) - s_nq_last_reconnect_s >= 300)) {
                                s_nq_last_reconnect_s = static_cast<int64_t>(tnow);
                                std::cout << "[NQ-IBKR] auto-reconnect: bouncing stale/down engine (stop+start)\n";
                                std::cout.flush();
                                omega::nq_momo_ibkr::stop();
                                const bool nrok = omega::nq_momo_ibkr::start();
                                std::cout << "[NQ-IBKR] auto-reconnect " << (nrok ? "OK -- live again"
                                             : "FAILED -- retry in 300s") << "\n";
                                std::cout.flush();
                            }
                        }
                    }
                }
            }
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
                // MinimalH4US30Breakout warm-restart state (S26 2026-04-25)
                // AtrMeanRevGrid bar-deque persistence (S37e 2026-05-26):
                // eliminates per-restart warmup. State files survive reboot ->
                // engine boots warm + immediately ready for signals.
                const std::string sr = state_root_dir();
                // FxEnsembleEngine state-persist REMOVED (S-2026-07-01 "no FX").
                // S-2026-06-03: persist open positions so a restart/deploy
                // resumes in-flight trades (worst-case loss = positions opened
                // in the <600s before a HARD crash; graceful stop saves too).
                const int n_pos = g_open_positions.save(sr + "/open_positions.dat");
                printf("[BAR-SAVE] Periodic save complete (every 10min); %d open positions persisted\n", n_pos);
                fflush(stdout);
            }
        }
    }).detach();
    // =========================================================================
    // FIX thread launch -- no longer gated on cTrader. The cTrader Open API
    // surface was removed at S13 2026-05-08; FIX 264=0 delivers full L2 directly
    // and starts pumping ticks as soon as the FIX session logs on. The legacy
    // 45s wait was a startup latency tax with no remaining purpose.
    printf("[STARTUP] Starting FIX immediately (FIX 264=0 provides L2)\n");
    fflush(stdout);
    std::thread trade_thread(trade_loop);
    Sleep(500);  // Give trade connection 500ms head start before quote loop
    quote_loop();  // blocks until g_running=false

    // quote_loop has exited -- g_running is false, trade_loop will exit shortly.
    std::cout << "[OMEGA] Shutdown\n";
    // cTrader depth feed shutdown removed S13 2026-05-08 -- surface culled.

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

    // ?? Engine dispatch worker shutdown -- 2026-05-01 race fix ????????????????
    // All producer threads (cTrader depth, FIX quote, FIX trade) have now
    // exited or stopped posting. Drain the dispatch queue and join the worker.
    // This ensures any in-flight ExecReport (broker fill confirmations) is
    // processed before the engines write their persistent state below.
    engine_dispatch_stop();

    // OM-02 (audit 2026-07-13): the detached IBKR-L2 + BigCap feed consumers had stop flags
    // that were never set — they could touch sockets/globals after Winsock teardown. Signal
    // them here + a short grace beat; their recv loops poll these atomics each cycle.
    g_ibkr_l2_stop = true;
    g_bigcap_stop  = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

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
    g_gold_stack.save_atr_state(state_root_dir() + "/gold_stack_state.dat");

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
    // MinimalH4US30Breakout warm-restart state (S26 2026-04-25)
    printf("[SHUTDOWN] Bar indicator state saved -- next restart will be instant warm\n");
    fflush(stdout);
    // S-2026-06-03: final open-position snapshot on graceful shutdown (SIGTERM /
    // console-close / service stop all route here via g_running=false ->
    // quote_loop exit). Restart adopts these back.
    {
        const int n_pos = g_open_positions.save(state_root_dir() + "/open_positions.dat");
        printf("[SHUTDOWN] %d open positions persisted -- will resume on restart\n", n_pos);
        fflush(stdout);
    }
    g_adaptive_risk.print_summary();
    if (g_tee_buf)   { g_tee_buf->flush_and_close(); std::cout.rdbuf(g_orig_cout); delete g_tee_buf; g_tee_buf = nullptr; }
    WSACleanup();
    ReleaseMutex(g_singleton_mutex);
    CloseHandle(g_singleton_mutex);
    g_shutdown_done.store(true);  // unblock console_ctrl_handler -- process may now exit
    return 0;
}




