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
    // Per-symbol typed overloads -- each applies instrument-specific params + macro context ptr
    apply_engine_config(g_eng_sp);   // [sp] section: tp=0.60%, sl=0.35%, vol=0.04%, regime-gated
    apply_engine_config(g_eng_nq);   // [nq] section: tp=0.70%, sl=0.40%, vol=0.05%, regime-gated
    apply_engine_config(g_eng_cl);   // [oil] section: tp=1.20%, sl=0.60%, vol=0.08%, inventory-blocked
    apply_engine_config(g_eng_us30); // typed Us30Engine: macro-gated like SP/NQ
    apply_engine_config(g_eng_nas100); // typed Nas100Engine: macro-gated, independent from USTEC.F
    apply_generic_index_config(g_eng_ger30);
    apply_generic_index_config(g_eng_uk100);
    apply_generic_index_config(g_eng_estx50);
    apply_generic_silver_config(g_eng_xag);
    // Bracket engines -- configure() with tuned production params.
    // buffer, lookback, RR, cooldown_ms, MIN_RANGE, CONFIRM_MOVE, confirm_timeout_ms, min_hold_ms
    g_bracket_gold.configure(
        0.8,    // buffer: place orders 0.8pts outside the range
        600,    // FEED-CALIBRATED lookback: 600 ticks = 185s = 3min at 195/min XAUUSD.
                //   300 ticks (90s) was borderline: gold moves 0.5-1.0pt in 90s of compression.
                //   MIN_RANGE=1.0pt -> bracket_high reset to 0 every tick (oscillates around threshold).
                //   600 ticks (3min) shows 1-4pt range in compression -> arms reliably.
                //   MAX_RANGE=12pt still prevents bracketing full trending session moves.
        4.0,    // DATA-CALIBRATED RR: 4.0x SL. Best on 2yr tick data ($38k profit).
                //   TP = 4x the structure range. Median range $2.67 ? TP ~$10.68
        90000,  // cooldown_ms: 90s
        1.5,    // DATA-CALIBRATED MIN_RANGE: $1.5. Allows small but real compressions.
                //   Brute-force showed $1.5-$12 range captures $38k vs $5 min misses most signals.
        0.05,   // CONFIRM_MOVE static fallback
        4000,   // confirm_timeout_ms
        12000,  // min_hold_ms
        0.0,    // VWAP_MIN_DIST: removed.
        30000,  // MIN_STRUCTURE_MS
        25000,  // FAILURE_WINDOW_MS
        20,     // ATR_PERIOD
        0.15,   // ATR_CONFIRM_K
        2.0,    // ATR_RANGE_K
        0.8,    // SLIPPAGE_BUFFER
        1.5     // EDGE_MULTIPLIER
    );
    // XAGUSD (~$65): daily range $3.5, typical compression $0.30, spread $0.08
    // Silver amplifies gold -- same cascade logic, same cooldown, trail rides $3-5 weekly moves.
    // Trail at 3R: comp=$0.30, trail_dist=$0.075 -- very tight, holds through volatile moves.
    g_bracket_xag.configure(
        0.04,   // buffer: spread*0.5 = $0.04 outside range
        150,    // FEED-CALIBRATED: 150 ticks = 60s at XAGUSD=150/min. Old 30=12s.
        3.0,    // RR: 3.0 matches gold. On $0.30 compression: trail arms at $0.90 in.
                //   On a $3.50 weekly move: 10R+ captured via trail.
        30000,  // cooldown_ms: 30s -- silver cascades same as gold, re-arm fast.
        0.40,   // MIN_RANGE: $0.40 minimum raw structural range.
                //   At $64, $0.40 = 0.63% -- real compression, not tick noise.
                //   Old value was $0.15 which fired on $0.18 structures that
                //   were swept in seconds. Raw range check in arm_both_sides
                //   now enforces this against the spread-padded dist.
        0.06,   // CONFIRM_MOVE
        4000,   // confirm_timeout_ms
        8000,   // min_hold_ms
        0.0,    // VWAP_MIN_DIST: removed -- silver near VWAP pre-breakout by definition.
        20000,  // MIN_STRUCTURE_MS: 20s
        12000,  // FAILURE_WINDOW_MS: 12s -- silver sweeps slightly faster than gold.
        20,     // ATR_PERIOD
        0.17,   // ATR_CONFIRM_K
        1.4,    // ATR_RANGE_K
        0.08,   // SLIPPAGE_BUFFER: $0.08 matches typical silver spread
        1.5     // EDGE_MULTIPLIER
    );
    // Wire shadow fill simulation -- price-triggered in PENDING, not immediate at arm
    g_bracket_gold.shadow_mode = (g_cfg.mode != "LIVE");
    g_bracket_xag.shadow_mode  = (g_cfg.mode != "LIVE");

    // ?? MacroCrashEngine config ??????????????????????????????????????????????????
    // Always-on macro event engine. Shadow mode = log only, no live orders.
    // Enable live once shadow logs confirm it fires correctly on real expansion events.
    g_macro_crash.shadow_mode     = true;  // SHADOW: enable live after validation
    g_macro_crash.enabled         = true;
    // ASIA-TUNED THRESHOLDS (2026-04-07):
    // London/NY macro events: $50-150pt moves, ATR=10-25pt, drift=8-20pt -> original gates correct.
    // Asia macro events (last 2 weeks): $10-30pt spikes, ATR=4-8pt, drift=3-6pt.
    // ATR_THRESHOLD=8 and DRIFT_MIN=6 BOTH miss Asia spikes:
    //   - Asia ATR is typically 3-6pt; threshold=8 means MCE never arms in Asia.
    //   - Asia drift peaks at 3-5pt on a $15 move; DRIFT_MIN=6 blocks entry on all of them.
    // Fix: lower Asia thresholds. These are tuned to the last 2 weeks of Asia activity:
    //   ATR_THRESHOLD=4.0: a $10 Asia spike at tick-rate 1/s generates ATR~4-5pt in 60s.
    //   DRIFT_MIN=3.0:     a $12 sustained Asia move builds drift to ~3-4pt.
    //   VOL_RATIO_MIN=2.0: Asia baseline vol is lower, 2x expansion is still real.
    // The session-aware gating happens at on_tick call time (MCE checks session_slot via
    // the tick_gold.hpp wrapper). We store the LOWER threshold so Asia is covered.
    // London/NY sessions produce higher ATR/drift naturally so they still clear the bar.
    // Evidence: 2026-04-07 image shows $13 drop in 6min during Asia (13:18-13:54 UTC slot=3)
    //           with RSI touching 30 -- ATR was ~5pt, drift ~-4pt. Old gates: blocked.
    g_macro_crash.ATR_THRESHOLD   = 4.0;   // lowered 8.0->4.0: covers Asia spikes ($10-15pt ATR=4-6pt)
    g_macro_crash.VOL_RATIO_MIN   = 2.0;   // lowered 2.5->2.0: Asia baseline lower, 2x still real
    g_macro_crash.DRIFT_MIN       = 3.0;   // lowered 6.0->3.0: Asia $12 moves build drift=3-4pt
    g_macro_crash.BASE_RISK_USD   = 80.0;  // scales with ATR (6x max = 0.48 lots at ATR=10)
    g_macro_crash.STEP1_TRIGGER_USD = 80.0;  // lowered 200->80: Asia moves are $10-30pt not $200+
                                              // At $10 move with 0.10 lots: open_pnl=$100 -- still clears
    g_macro_crash.STEP2_TRIGGER_USD = 160.0; // lowered 400->160: proportional to Asia move size
    g_macro_crash.on_close = [](double exit_px, bool is_long, double size, const std::string& reason) {
        if (g_macro_crash.shadow_mode) return;  // shadow: no live order
        send_live_order("XAUUSD", is_long, size, exit_px);
        printf("[MCE] Live close sent %s %.3f @ %.2f reason=%s\n",
               is_long ? "LONG" : "SHORT", size, exit_px, reason.c_str());
        fflush(stdout);
    };
    // Wire trade record callback -- fires in BOTH shadow and live.
    // This is what makes MCE trades appear in GUI, ledger, and CSV with correct costs.
    g_macro_crash.on_trade_record = [](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);
    };
    printf("[MCE] MacroCrashEngine ARMED (shadow_mode=%s) ATR>%.0f vol>%.1fx drift>%.0f\n",
           g_macro_crash.shadow_mode ? "true" : "false",
           g_macro_crash.ATR_THRESHOLD, g_macro_crash.VOL_RATIO_MIN, g_macro_crash.DRIFT_MIN);
    // RSI Reversal Engine startup config
    g_rsi_reversal.enabled       = true;
    g_rsi_reversal.shadow_mode   = true;   // SHADOW until 30 trades validate WR
    g_rsi_reversal.RSI_OVERSOLD  = 42.0;   // LONG below this -- catches 5pt moves (RSI 60->40)
    g_rsi_reversal.RSI_OVERBOUGHT= 58.0;   // SHORT above this -- catches 5pt rises
    g_rsi_reversal.RSI_EXIT_LONG = 52.0;   // exit LONG when RSI recovers here
    g_rsi_reversal.RSI_EXIT_SHORT= 48.0;   // exit SHORT when RSI recovers here
    g_rsi_reversal.SL_ATR_MULT   = 0.6;    // SL = 0.6x ATR (Asia ATR=3pt -> $1.80 SL)
    g_rsi_reversal.TRAIL_ATR_MULT= 0.40;   // trail = 0.4x ATR -- tight, don't give back move
    g_rsi_reversal.BE_ATR_MULT   = 0.40;   // BE at 0.4x ATR -- locks when cost covered
    g_rsi_reversal.COOLDOWN_S    = 60;     // 60s cooldown -- RSI can cycle again quickly
    g_rsi_reversal.MAX_HOLD_S    = 600;    // 10 min hard exit
    printf("[RSI-REV] RSIReversalEngine configured (shadow_mode=%s "
           "oversold=%.0f overbought=%.0f sl_mult=%.1fx)\n",
           g_rsi_reversal.shadow_mode ? "true" : "false",
           g_rsi_reversal.RSI_OVERSOLD,
           g_rsi_reversal.RSI_OVERBOUGHT,
           g_rsi_reversal.SL_ATR_MULT);
    fflush(stdout);

    // MicroMomentumEngine startup config
    // FIX 2026-04-07 v3: chop filters added -- RSI level gate + price move confirmation.
    // Root cause of losses: RSI_DELTA_MIN=3 fired on every RSI wiggle in ranging markets.
    // Chart showed RSI 48->68->50->62 producing entries every 20s all hitting SL.
    // Two new gates in engine prevent this:
    //   RSI_LEVEL_LONG/SHORT: RSI must be above 52 / below 48 -- dead zone around 50 = chop.
    //   PRICE_MOVE_MIN: price must have moved 1.5pt in signal direction over the window.
    //                   RSI moving without price following = Wilder smoothing noise = skip.
    // RSI_DELTA_MIN raised 3->6. COOLDOWN raised 20->45s.
    // SL_ATR_MULT raised 0.5->1.5, TP raised 4->6pt (prior 3s SL hit).
    g_micro_momentum.enabled           = false;  // DISABLED: no proven edge, churning in chop -- re-enable after backtest
    g_micro_momentum.shadow_mode       = true;
    g_micro_momentum.ENTRY_DISP_PTS    = 0.0;   // disabled -- anchor too fast
    g_micro_momentum.RSI_DELTA_MIN     = 6.0;   // raised 3->6: 3 fired on every micro-wiggle
    g_micro_momentum.RSI_DELTA_WINDOW  = 10;
    g_micro_momentum.BORDERLINE_DELTA  = 3.0;   // = RSI_DELTA_MIN: borderline zone empty
    g_micro_momentum.RSI_LEVEL_LONG    = 52.0;  // LONG only when RSI > 52
    g_micro_momentum.RSI_LEVEL_SHORT   = 48.0;  // SHORT only when RSI < 48
    g_micro_momentum.PRICE_MOVE_MIN    = 1.5;   // price must move 1.5pt in signal dir over window
    g_micro_momentum.L2_LONG_MIN       = 0.40;
    g_micro_momentum.L2_SHORT_MAX      = 0.60;
    g_micro_momentum.TP_PTS            = 6.0;
    g_micro_momentum.SL_ATR_MULT       = 1.5;
    g_micro_momentum.BE_TRIGGER_PTS    = 1.0;
    g_micro_momentum.LOCK_TRIGGER_PTS  = 2.0;
    g_micro_momentum.TRAIL_DIST_PTS    = 1.0;
    g_micro_momentum.COOLDOWN_S        = 45;    // raised 20->45s: stop churning in chop
    g_micro_momentum.MAX_HOLD_S        = 240;
    printf("[MICROMOM] MicroMomentumEngine configured "
           "(shadow_mode=%s disp=%.1fpt rsi_delta_min=%.1f tp=%.1fpt cooldown=%ds)\n",
           g_micro_momentum.shadow_mode ? "true" : "false",
           g_micro_momentum.ENTRY_DISP_PTS,
           g_micro_momentum.RSI_DELTA_MIN,
           g_micro_momentum.TP_PTS,
           g_micro_momentum.COOLDOWN_S);
    fflush(stdout);

    // [BUG-5 NOTE] MCE is shadow_mode=true by design -- it logs [MCE-SHADOW] but sends
    // no FIX orders. Entry/exit logic is fully functional via on_close callback wired above.
    // To enable live MCE trades: set g_macro_crash.shadow_mode = false (requires authorisation).
    if (g_macro_crash.shadow_mode) {
        printf("[MCE] WARNING: MacroCrashEngine is in SHADOW mode -- no live orders will fire.\n"
               "[MCE] To enable: change shadow_mode=false in omega_main.hpp after validation.\n");
    }
    fflush(stdout);
    fflush(stdout);
    // PENDING_TIMEOUT_SEC: gold/silver compress for minutes before breaking -- 60s was expiring before the move
    g_bracket_gold.PENDING_TIMEOUT_SEC = 600;  // 10 min: gold compression can last well beyond 5 min
    g_bracket_xag.PENDING_TIMEOUT_SEC  = 300;  // 5 min: silver moves faster than gold
    // MIN_BREAK_TICKS: sweep guard -- price must stay inside the bracket for N consecutive
    // ticks before orders are sent. Catches London open liquidity sweeps (07:00:34 SHORT
    // -$7.97): bracket range $7.80 was exactly one sweep wide, SHORT filled in 1 tick
    // then price snapped back $7.80 to SL in 16s. 3 ticks ~= 0.3-0.6s -- long enough to
    // distinguish a single-tick spike from genuine compression holding at the boundary.
    // Silver also benefits: London open sweep pattern identical, slightly faster ticks.
    g_bracket_gold.MIN_BREAK_TICKS = 3;
    g_bracket_xag.MIN_BREAK_TICKS  = 3;
    // ATR-based dynamic minimum range: eff_min_range = max(recent_noise * ATR_RANGE_K, MIN_RANGE)
    // Prevents brackets arming when the market noise floor exceeds the bracket width --
    // the SL then sits inside normal noise and gets swept without a real move.
    //
    // ATR_PERIOD=20 ticks: at London open ~5-15 ticks/sec = 1-4s of recent price action.
    // This captures the current noise floor, not historical vol. Updates every tick.
    //
    // Gold: London noise $8-20. MIN_RANGE=6. ATR_RANGE_K=1.5 ? when noise=$12, eff_min=18.
    //   A $10 bracket in $12 noise is invalid -- SL would be swept immediately.
    //   A $20 bracket in $12 noise is valid -- genuine compression above noise.
    // Silver: proportionally similar (~$0.10-0.40 noise). ATR_RANGE_K=1.5.
    // FX (EURUSD etc.): noise ~0.0003-0.0008. ATR_RANGE_K=1.8 (tighter price, more sensitive).
    // Gold bracket: ATR_RANGE_K=0 disables ATR floor -- use MIN_RANGE=1.0pt directly.
    // With VIX at 24 gold compresses to 1-3pt ranges which ATR_RANGE_K=1.5 rejects (needs 15pt).
    // The bracket window itself defines the range -- 1pt minimum just filters single-tick noise.
    g_bracket_gold.ATR_PERIOD  = 20;  g_bracket_gold.ATR_RANGE_K  = 0.0;
    g_bracket_gold.MIN_RANGE   = 1.0;
    g_bracket_xag.ATR_PERIOD   = 20;  g_bracket_xag.ATR_RANGE_K   = 1.5;
    g_bracket_eurusd.ATR_PERIOD = 20; g_bracket_eurusd.ATR_RANGE_K = 1.8;
    g_bracket_gbpusd.ATR_PERIOD = 20; g_bracket_gbpusd.ATR_RANGE_K = 1.8;
    g_bracket_audusd.ATR_PERIOD = 20; g_bracket_audusd.ATR_RANGE_K = 1.8;
    g_bracket_nzdusd.ATR_PERIOD = 20; g_bracket_nzdusd.ATR_RANGE_K = 1.8;
    g_bracket_usdjpy.ATR_PERIOD = 20; g_bracket_usdjpy.ATR_RANGE_K = 1.8;
    // Indices: leave ATR disabled -- noise floor more stable, fixed MIN_RANGE sufficient
    // MAX_RANGE: prevents bracketing full trending session moves instead of real compression
    // Gold at $4400: 0.4% = $17.6 max range. Tight compression is $8-16. Day range is $40-120.
    g_bracket_gold.MAX_RANGE   = 12.0;   // DATA-CALIBRATED: $12 max. Ranges >$12 are trending, not bracketing.
    // Silver at $68: 0.4% = $0.27 max range. Compression = $0.15-0.25. Day range = $1-3.
    g_bracket_xag.MAX_RANGE    = 0.30;   // ~0.44% of silver ~$68
    // Configure opening range engines
    g_orb_us.OPEN_HOUR    = 13; g_orb_us.OPEN_MIN    = 30;  // NY open 13:30 UTC
    g_orb_ger30.OPEN_HOUR = 8;  g_orb_ger30.OPEN_MIN = 0;   // Xetra open 08:00 UTC
    g_orb_silver.OPEN_HOUR= 13; g_orb_silver.OPEN_MIN= 30;  // COMEX open 13:30 UTC
    // New ORB instruments: LSE and Euronext with tighter 15-min range windows
    g_orb_uk100.OPEN_HOUR  = 8;  g_orb_uk100.OPEN_MIN  = 0;   // LSE open 08:00 UTC
    g_orb_uk100.RANGE_WINDOW_MIN = 15;  // 15-min range (LSE moves fast at open)
    g_orb_uk100.TP_PCT  = 0.12;  g_orb_uk100.SL_PCT  = 0.07;  // UK100 TP/SL calibrated to GBP volatility
    g_orb_estx50.OPEN_HOUR = 9;  g_orb_estx50.OPEN_MIN = 0;   // Euronext open 09:00 UTC
    g_orb_estx50.RANGE_WINDOW_MIN = 15; // 15-min range
    g_orb_estx50.TP_PCT = 0.10;  g_orb_estx50.SL_PCT = 0.06;  // ESTX50 TP/SL similar to GER40
    // VWAPReversionEngine params -- per-instrument tuning
    // Indices: 0.20% extension threshold, 180s cooldown (fast mean-reversion)
    g_vwap_rev_sp.EXTENSION_THRESH_PCT    = 0.20; g_vwap_rev_sp.COOLDOWN_SEC    = 180;
    g_vwap_rev_nq.EXTENSION_THRESH_PCT    = 0.20; g_vwap_rev_nq.COOLDOWN_SEC    = 180;
    g_vwap_rev_ger40.EXTENSION_THRESH_PCT = 0.20; g_vwap_rev_ger40.COOLDOWN_SEC = 180;
    // EURUSD: 0.12% extension threshold (FX moves more precisely, smaller range)
    g_vwap_rev_eurusd.EXTENSION_THRESH_PCT = 0.12; g_vwap_rev_eurusd.COOLDOWN_SEC = 120;
    // ?? NBM London session engines (07:00-13:30 UTC) ????????????????????????????
    // Covers the gap before NY open. Gold and oil are liquid from London open.
    // Uses same ATR/band logic as NY engines but anchored to London open price.
    g_nbm_gold_london.SESSION_OPEN_UTC  =  7;  g_nbm_gold_london.SESSION_OPEN_MIN  =  0;
    g_nbm_gold_london.SESSION_CLOSE_UTC = 13;  g_nbm_gold_london.SESSION_CLOSE_MIN = 30;
    g_nbm_gold_london.MAX_SPREAD_PCT    = 0.02;  // gold spread tighter than indices
    g_nbm_gold_london.WARMUP_TICKS      = 120;
    g_nbm_gold_london.COOLDOWN_SEC      = 600;

    g_nbm_oil_london.SESSION_OPEN_UTC  =  7;  g_nbm_oil_london.SESSION_OPEN_MIN  =  0;
    g_nbm_oil_london.SESSION_CLOSE_UTC = 13;  g_nbm_oil_london.SESSION_CLOSE_MIN = 30;
    g_nbm_oil_london.MAX_SPREAD_PCT    = 0.05;
    g_nbm_oil_london.WARMUP_TICKS      = 120;
    g_nbm_oil_london.COOLDOWN_SEC      = 600;

    // TrendPullbackEngine params -- per-instrument tuning
    //
    // GOLD (M15 bar EMAs -- seeded from g_bars_gold.m15):
    //   EMAs come from OHLCBarEngine on 50 M15 bars:
    //     EMA9  half-life = 3.1 bars = 47 min
    //     EMA21 half-life = 7.3 bars = 109 min
    //     EMA50 half-life = 17.3 bars = 260 min = 4.3 hours
    //
    //   PULLBACK_BAND_PCT: the critical setting.
    //   Old value (0.08%) = ±3.7pts at $4700. With M15 EMA50 representing
    //   4.3h weighted average, price is typically 10-30pts from EMA50 during
    //   an active trend. 0.08% = never fires. Must be wide enough to detect
    //   genuine pullbacks TO the M15 EMA50 level.
    //   0.50% = ±23.5pts at $4700. Fires when price is within ~24pts of
    //   the M15 EMA50 -- correct for pullback-to-slow-average strategy.
    //   Upper bound: 1.0% = ±47pts -- too loose, fires mid-trend constantly.
    //
    //   COOLDOWN_SEC: M15 trade = swing trade, minimum 1 M15 bar (15min)
    //   between signals. 900s (15min) = 1 full M15 bar -- prevents rapid
    //   re-entry on the same pullback level.
    //
    //   TRAIL_ARM/DIST: ATR from M15 bars = 4-8pts typical. 2x arm = 8-16pts
    //   before trailing. 1x dist = 4-8pt trail. Correct for swing scale.
    //   Leave at class defaults (2.0x arm, 1.0x dist, 1.0x BE).
    //
    //   BE_ATR_MULT: lock BE at 1x M15 ATR (~5pts). Unchanged -- good.
    g_trend_pb_gold.PULLBACK_BAND_PCT  = 0.50;  // M15: ±23.5pts at $4700. Old 0.08% (±3.7pts) never fired.
    g_trend_pb_gold.COOLDOWN_SEC       = 60;    // 60s cooldown -- reduced from 900s (15min was insane, missed 100pt moves)
    g_trend_pb_gold.MIN_EMA_SEP        = 5.0;   // gold: 5pt EMA9-EMA50 separation = real trend
    g_trend_pb_gold.H4_GATE_ENABLED    = true;  // gate M15 entries on H4 trend direction
    g_trend_pb_gold.ATR_SL_MULT        = 1.2;   // SL floor = 1.2x M15 ATR (adaptive, not fixed 8pt)
    // Improvement 1: vol regime sizing
    g_trend_pb_gold.VOL_SCALE_HIGH_MULT = 1.5;
    g_trend_pb_gold.VOL_SCALE_LOW_MULT  = 0.7;
    g_trend_pb_gold.VOL_SCALE_CUT       = 0.60;
    g_trend_pb_gold.VOL_SCALE_BOOST     = 1.20;
    // Improvement 2: daily loss cap -- stop gold TrendPB after $150 loss in a day
    g_trend_pb_gold.DAILY_LOSS_CAP      = 150.0;
    // Improvement 4: time-of-day weighting
    g_trend_pb_gold.TOD_WEIGHT_ENABLED  = true;
    // Improvement 5: CVD gate
    g_trend_pb_gold.CVD_GATE_ENABLED    = true;
    // Improvement 7: news SL widening
    g_trend_pb_gold.NEWS_WARN_SECS      = 900;   // 15min before event
    g_trend_pb_gold.NEWS_SL_MULT        = 1.5;
    // Widen pullback band: 0.15% -> 0.50% (±23pts at 4620)
    // Default 0.15% = ±6.9pts. On a $20 trending move price is 20pts from EMA50
    // and never enters the band -- engine silent on all clean trends.
    // 0.50% allows entry when price is trending away from EMA50 but still directional.
    g_trend_pb_gold.PULLBACK_BAND_PCT   = 0.50;
    // Improvement 8: pyramid on second pullback
    g_trend_pb_gold.PYRAMID_ENABLED     = true;
    g_trend_pb_gold.PYRAMID_SIZE_MULT   = 0.5;
    g_trend_pb_gold.PYRAMID_MAX_ADDS    = 1;
    // Trail/BE params: class defaults are correct for M15 ATR scale (4-8pts)
    // TRAIL_ARM_ATR_MULT=2.0, TRAIL_DIST_ATR_MULT=1.0, BE_ATR_MULT=1.0 -- no change needed
    // GER40: tighter band (index moves more cleanly around EMAs)
    g_trend_pb_ger40.PULLBACK_BAND_PCT = 0.05;  // 0.05% of GER40 = ~11pts at 22500
    g_trend_pb_ger40.COOLDOWN_SEC     = 120;
    g_trend_pb_ger40.MIN_EMA_SEP      = 15.0;
    // NQ/SP TrendPullback: daily loss cap + tighter controls
    // Without DAILY_LOSS_CAP, NQ TrendPullback fired 7 consecutive losing entries
    // during the Apr 2 tariff crash (NQ dropped ~1000pts). Each SL hit was $12-13,
    // but the direction block (2 consec SL hits = 10min pause) was the only guard.
    // Daily loss cap stops the engine entirely after a bad sequence.
    g_trend_pb_nq.MIN_EMA_SEP         = 25.0;
    g_trend_pb_nq.DAILY_LOSS_CAP      = 80.0;   // $80 daily cap: ~6 SL hits at $12 each
    g_trend_pb_sp.MIN_EMA_SEP         = 15.0;
    g_trend_pb_sp.DAILY_LOSS_CAP      = 80.0;   // same cap for SP
    g_trend_pb_ger40.DAILY_LOSS_CAP   = 80.0;   // GER40 too -- no cap was previously set
    // Load warm EMA state -- skips EMA_WARMUP_TICKS cold period on restart
    g_trend_pb_gold.load_state(log_root_dir()  + "/trend_pb_gold.dat");
    g_trend_pb_ger40.load_state(log_root_dir() + "/trend_pb_ger40.dat");
    g_trend_pb_nq.load_state(log_root_dir()    + "/trend_pb_nq.dat");
    g_trend_pb_sp.load_state(log_root_dir()    + "/trend_pb_sp.dat");

    // ?? Nuke stale ctrader_bar_failed.txt on every startup ??????????????????
    // Old binaries wrote M5/M15 periods (5/7) and BOM-prefixed keys into this
    // file, permanently blacklisting the tick-data fallback requests that seed
    // M15 bars on cold start. Effect: bars NEVER seed, GoldFlow blocked all day.
    // Fix: delete the file and rewrite it clean from the pre-seeded in-memory set.
    // The in-memory set is pre-seeded in ctrader setup (XAUUSD:1 and live subs).
    // Any valid entries that were on disk will be re-discovered naturally --
    // GetTrendbarsReq is already blocked in-memory. The only thing we lose is
    // session-specific crash history, which resets anyway on every restart.
    {
        const std::string failed_path = log_root_dir() + "/ctrader_bar_failed.txt";
        if (std::remove(failed_path.c_str()) == 0) {
            printf("[STARTUP] Deleted stale ctrader_bar_failed.txt -- clean slate for bar requests\n");
        }
        fflush(stdout);
    }

    // Load OHLCBarEngine indicator state -- instant warm restart, no tick data request needed.
    // If .dat files exist and are <24hr old: m1_ready=true immediately on first tick.
    // Bars update live from on_spot_event (M15 bar closes pushed by broker every 15min).
    // This eliminates the 2-minute GoldFlow bar gate delay on every restart.
    {
        const std::string base = log_root_dir();
        const bool m1_ok  = g_bars_gold.m1 .load_indicators(base + "/bars_gold_m1.dat");
        const bool m5_ok  = g_bars_gold.m5 .load_indicators(base + "/bars_gold_m5.dat");
        const bool m15_ok = g_bars_gold.m15.load_indicators(base + "/bars_gold_m15.dat");
        const bool h4_ok  = g_bars_gold.h4 .load_indicators(base + "/bars_gold_h4.dat");
        if (m15_ok) {
            // Immediately seed TrendPullback EMAs + ATR from M15 bar state
            g_trend_pb_gold.seed_bar_emas(
                g_bars_gold.m15.ind.ema9 .load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.ema21.load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.ema50.load(std::memory_order_relaxed),
                g_bars_gold.m15.ind.atr14.load(std::memory_order_relaxed));
            // M1 EMA crossover for bar trend gate -- loaded from disk, no 15-min warmup
            if (m1_ok) {
                const double st_e9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
                const double st_e50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
                const int st_trend  = (st_e9 > 0.0 && st_e50 > 0.0)
                    ? (st_e9 < st_e50 ? -1 : +1) : 0;
                g_trend_pb_gold.seed_m5_trend(st_trend);
                printf("[STARTUP] M1 bar state loaded: EMA9=%.2f EMA50=%.2f RSI=%.1f trend=%+d"
                       " -- GoldFlow/GoldStack bar gates active immediately\n",
                       st_e9, st_e50,
                       g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed),
                       st_trend);
            } else if (m5_ok) {
                // Fallback: seed trend from M5 if M1 not available
                const double st_e9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
                const double st_e50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
                const int st_trend  = (st_e9 > 0.0 && st_e50 > 0.0)
                    ? (st_e9 < st_e50 ? -1 : +1) : 0;
                g_trend_pb_gold.seed_m5_trend(st_trend);
            }
            // Immediately seed H4 HTF trend gate -- no need to wait for first tick
            if (h4_ok) {
                g_trend_pb_gold.seed_h4_trend(
                    g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed));
            }
            printf("[STARTUP] Bar state loaded: M1=%s M5=%s M15=%s H4=%s"
                   " EMA50=%.2f ATR=%.2f H4_trend=%d\n",
                   m1_ok?"ok":"cold", m5_ok?"ok":"cold", m15_ok?"ok":"cold", h4_ok?"ok":"cold",
                   g_bars_gold.m15.ind.ema50.load(std::memory_order_relaxed),
                   g_bars_gold.m15.ind.atr14.load(std::memory_order_relaxed),
                   g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed));
        } else {
            printf("[STARTUP] No bar state on disk (cold start) -- 15min M1 warmup required\n");
        }
        fflush(stdout);
    }

    // Load macro event calendar for OmegaVolTargeter high_impact_window() gate.
    // File: config\macro_events_today.txt (same dir as omega_config.ini)
    // Format: "HH:MM CURRENCY HIGH LABEL"  e.g. "13:30 USD HIGH NFP"
    // Must be updated daily (manually or via a pre-open script).
    // Graceful: if file absent, high_impact_window() returns false (no blocking).
    {
        // Mirror config resolution: try cwd first, then config\ subdir
        const std::string ev_cwd = "macro_events_today.txt";
        const std::string ev_cfg = "config\\macro_events_today.txt";
        std::ifstream test(ev_cwd);
        g_vol_targeter.load_events(test.is_open() ? ev_cwd : ev_cfg);
    }

    // TrendPullback gold: M15 bar EMAs seeded live from g_bars_gold.m15 each tick.
    g_bracket_gold.cancel_order_fn = [](const std::string& id) { send_cancel_order(id); };
    g_bracket_xag.cancel_order_fn  = [](const std::string& id) { send_cancel_order(id); };

    // ?? Configure new bracket engines ????????????????????????????????????????
    // US equity indices: arms both sides, captures whichever direction breaks out
    g_bracket_sp.symbol     = "US500.F"; g_bracket_sp.ENTRY_SIZE     = 0.01;
    g_bracket_nq.symbol     = "USTEC.F"; g_bracket_nq.ENTRY_SIZE     = 0.01;
    g_bracket_us30.symbol   = "DJ30.F";  g_bracket_us30.ENTRY_SIZE   = 0.01;
    g_bracket_nas100.symbol = "NAS100";  g_bracket_nas100.ENTRY_SIZE = 0.01;
    g_bracket_ger30.symbol  = "GER40";   g_bracket_ger30.ENTRY_SIZE  = 0.01;
    g_bracket_uk100.symbol  = "UK100";   g_bracket_uk100.ENTRY_SIZE  = 0.01;
    g_bracket_estx50.symbol = "ESTX50";  g_bracket_estx50.ENTRY_SIZE = 0.01;
    g_bracket_brent.symbol  = "BRENT"; g_bracket_brent.ENTRY_SIZE  = 0.01;
    g_bracket_eurusd.symbol = "EURUSD";  g_bracket_eurusd.ENTRY_SIZE = 0.01;
    g_bracket_gbpusd.symbol = "GBPUSD";  g_bracket_gbpusd.ENTRY_SIZE = 0.01;
    g_bracket_audusd.symbol = "AUDUSD";  g_bracket_audusd.ENTRY_SIZE = 0.01;
    g_bracket_nzdusd.symbol = "NZDUSD";  g_bracket_nzdusd.ENTRY_SIZE = 0.01;

    // ?? MAX_RANGE caps: ~0.4% of instrument price ?????????????????????????????
    // Prevents bracketing full day-range trending moves as if they were compression.
    // Evidence: ESTX50 bracket fired on 79.8pt range (1.4% of 5600) = entire London
    // open move, not compression. Rule: MAX_RANGE ? 2? MIN_RANGE ? 0.4% of price.
    g_bracket_sp.MAX_RANGE      = 25.0;   // ~0.40% of SP ~6200
    g_bracket_nq.MAX_RANGE      = 90.0;   // ~0.40% of NQ ~22500
    g_bracket_us30.MAX_RANGE    = 180.0;  // ~0.40% of DJ30 ~45000
    g_bracket_nas100.MAX_RANGE  = 90.0;   // ~0.40% of NAS100 ~22500
    g_bracket_ger30.MAX_RANGE   = 90.0;   // ~0.40% of GER40 ~22500
    g_bracket_uk100.MAX_RANGE   = 40.0;   // ~0.40% of UK100 ~10000
    g_bracket_estx50.MAX_RANGE  = 22.0;   // ~0.40% of ESTX50 ~5500
    g_bracket_brent.MAX_RANGE   = 1.20;   // ~0.40% of Brent ~$90 (oil tight)
    g_bracket_eurusd.MAX_RANGE  = 0.0008; // ~0.07% of EURUSD ~1.15 (FX tight)
    g_bracket_gbpusd.MAX_RANGE  = 0.0010; // ~0.08% of GBPUSD ~1.33
    g_bracket_audusd.MAX_RANGE  = 0.0006; // ~0.09% of AUDUSD ~0.70
    g_bracket_nzdusd.MAX_RANGE  = 0.0006; // ~0.10% of NZDUSD ~0.60
    // USDJPY/GOLD/XAGUSD: MAX_RANGE set after their configure() calls below
    g_bracket_usdjpy.symbol = "USDJPY";  g_bracket_usdjpy.ENTRY_SIZE = 0.01;

    // ?? Bracket calibration -- March 2026 actual prices ?????????????????????????
    // All params derived from real price levels and observed daily ranges.
    // configure(buf, lookback, RR, cooldown_ms, min_range, cfm, ctout, min_hold_ms,
    //           vwap_dist, struct_ms, fail_win_ms, atr_per, atr_ck, atr_rk, slip_buf, edge_mult)
    //
    // RR=2.5 for equity indices -- trail kicks in at 2.5R, stepped SL locks gains
    // cooldown=60s for indices -- slightly choppier intraday than commodities
    // fail_win=10s for indices -- sweeps resolve faster on liquid index futures
    // vwap_dist=0 everywhere -- pre-breakout price near VWAP by definition
    //
    // US500.F (~$6,600): daily range $120, typical compression $8, spread $0.50
    // Index bracket engines -- MIN_RANGE calibrated to current 2026 price levels.
    // Rule: MIN_RANGE >= 0.20% of instrument price. Below that is tick noise.
    // At wrong (old) values: UK100 $4 @ 9720 = 0.041%, ESTX50 $3 @ 5387 = 0.056% -- pure noise.
    // Cooldown raised 60s?180s: 60s cooldown re-armed into same chop 1 min after SL hit.
    // MIN_STRUCTURE_MS raised 20s?30s: 20s is too short for real index compression.
    //
    // configure(buf, lookback, RR, cooldown_ms, MIN_RANGE, cfm, ctout, min_hold_ms,
    //           vwap_dist, MIN_STRUCTURE_MS, FAILURE_WINDOW_MS, atr_period,
    //           atr_confirm_k, atr_range_k, slippage_buffer, edge_multiplier)
    //
    // US500.F (~6000): 0.20% = $12.0 min range
    g_bracket_sp.configure(    0.25, 120, 2.5, 180000, 12.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.30, 1.5);
    // USTEC.F (~21000): 0.20% = $42.0 min range
    g_bracket_nq.configure(    0.75, 180, 2.5, 180000, 42.0, 0.05, 4000, 10000, 0.0, 45000, 10000, 20, 0.15, 2.0, 2.50, 1.5);
    // DJ30.F (~43000): 0.20% = $86.0 min range
    g_bracket_us30.configure(  2.50, 120, 2.5, 180000, 86.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 3.00, 1.5);
    // NAS100 (~21000): same as USTEC
    g_bracket_nas100.configure(0.75, 165, 2.5, 180000, 42.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 1.00, 1.5);
    // GER40 (~22000): 0.20% = $44.0 min range
    g_bracket_ger30.configure( 1.00, 120, 2.5, 180000, 44.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 1.00, 1.5);
    // UK100 (~9720): 0.20% = $19.5 min range
    g_bracket_uk100.configure( 0.50, 120, 2.5, 180000, 20.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.50, 1.5);
    // ESTX50 (~5387): 0.20% = $10.8 min range
    g_bracket_estx50.configure(0.50, 120, 2.5, 180000, 11.0, 0.05, 4000, 10000, 0.0, 30000, 10000, 20, 0.15, 2.0, 0.50, 1.5);
    g_bracket_brent.configure(
        0.10,   // buffer
        30,     // lookback
        2.5,    // RR: raised 1.8?2.5. Brent has $2-5 daily ranges -- trail captures multi-hour moves.
        45000,  // cooldown_ms: reduced 120s?45s. Slightly longer than gold (oil choppier intraday).
        0.5,    // MIN_RANGE -- 0.50pts minimum (unchanged)
        0.05,   // CONFIRM_MOVE
        4000,   // confirm_timeout_ms
        10000,  // min_hold_ms
        0.0,    // VWAP_MIN_DIST: removed -- same rationale as gold/silver
        20000,  // MIN_STRUCTURE_MS
        12000,  // FAILURE_WINDOW_MS: raised 5s?12s -- oil sweeps similar duration to silver
        20,     // ATR_PERIOD
        0.15,   // ATR_CONFIRM_K
        2.0,    // ATR_RANGE_K
        0.10,   // SLIPPAGE_BUFFER
        1.5     // EDGE_MULTIPLIER
    );
    // FX brackets -- calibrated for March 2026 price levels
    // RR=2.0 for FX -- tighter pip-based moves, trail at 2R
    // cooldown=45s -- FX compresses and re-compresses faster than commodities
    // fail_win=8s -- FX sweeps resolve quickly (highly liquid, tight spread)
    // vwap_dist=0 everywhere -- pre-breakout FX near session VWAP
    //
    // MIN_RANGE raised across all FX pairs vs original calibration:
    //   Old EURUSD 0.00035 (3.5 pip) armed on 1-pip Asian tape noise ? junk bracket
    //   New 0.00060 (6 pip): genuine compression; below that is spread-level noise
    // MIN_STRUCTURE_MS raised 20s?45s: FX needs longer confirmation in thin tape
    // EDGE_MULTIPLIER raised 1.5?2.0: TP must cover 2? total round-trip cost
    //
    // EURUSD (~1.156): daily range ~100 pips, compression ~7 pips, spread ~1.4 pip
    g_bracket_eurusd.configure(0.00007, 30, 2.0, 45000, 0.00060, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00010, 2.0);
    // GBPUSD (~1.330): daily range ~120 pips, compression ~8 pips, spread ~1.8 pip
    g_bracket_gbpusd.configure(0.00009, 30, 2.0, 45000, 0.00070, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00012, 2.0);
    // AUDUSD (~0.701): daily range ~80 pips, compression ~5 pips, spread ~1.2 pip
    g_bracket_audusd.configure(0.00006, 30, 2.0, 45000, 0.00050, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00008, 2.0);
    // NZDUSD (~0.583): similar to AUD, slightly wider spread
    g_bracket_nzdusd.configure(0.00007, 30, 2.0, 45000, 0.00050, 0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.00009, 2.0);
    // USDJPY (~149.5): daily range ~120 pips, compression ~25 pips, spread ~4 pip
    // MIN_RANGE raised 0.12?0.20 (2 pips)
    g_bracket_usdjpy.configure(0.02,    30, 2.0, 45000, 0.20,    0.05, 4000, 8000, 0.0, 45000, 8000, 20, 0.15, 2.0, 0.04,    2.0);
    g_bracket_usdjpy.MAX_RANGE = 0.60;   // ~0.40% of USDJPY ~150

    // Shadow mode + cancel wiring for all new bracket engines
    const bool shadow = (g_cfg.mode != "LIVE");
    auto wire_bracket = [&](auto& beng, int pending_timeout_sec = 180) {
        beng.shadow_mode         = shadow;
        beng.PENDING_TIMEOUT_SEC = pending_timeout_sec;
        beng.cancel_order_fn     = [](const std::string& id) { send_cancel_order(id); };
    };
    // Pending timeouts calibrated per symbol:
    // Commodities: longer compression periods before breaking (5-10 min)
    // Equity indices: faster breakouts (3-5 min)
    // FX: tightest -- often break within 2-3 min or reset
    wire_bracket(g_bracket_sp,      300);  // US500: 5min -- index compression holds ~3-5min
    wire_bracket(g_bracket_nq,      300);  // USTEC: 5min
    wire_bracket(g_bracket_us30,    300);  // DJ30:  5min
    wire_bracket(g_bracket_nas100,  300);  // NAS100: 5min
    wire_bracket(g_bracket_ger30,   300);  // GER40: 5min
    wire_bracket(g_bracket_uk100,   300);  // UK100: 5min
    wire_bracket(g_bracket_estx50,  300);  // ESTX50: 5min
    wire_bracket(g_bracket_brent,   480);  // Brent: 8min -- oil compresses longer than indices
    wire_bracket(g_bracket_eurusd,  180);  // EURUSD: 3min -- FX breaks fast or resets
    wire_bracket(g_bracket_gbpusd,  180);  // GBPUSD: 3min
    wire_bracket(g_bracket_audusd,  180);  // AUDUSD: 3min
    wire_bracket(g_bracket_nzdusd,  180);  // NZDUSD: 3min
    wire_bracket(g_bracket_usdjpy,  240);  // USDJPY: 4min -- JPY can compress longer
    apply_generic_fx_config(g_eng_eurusd);
    apply_generic_gbpusd_config(g_eng_gbpusd);
    apply_generic_audusd_config(g_eng_audusd);
    apply_generic_nzdusd_config(g_eng_nzdusd);
    apply_generic_usdjpy_config(g_eng_usdjpy);
    apply_generic_brent_config(g_eng_brent);

    // ?? SymbolConfig override -- load symbols.ini, apply per-symbol params ?????
    // Loads on top of the apply_* defaults above. Every symbol independently
    // tunable from symbols.ini with no shared inheritance.
    {
        // Try multiple paths -- binary may run from different working directories
        const std::vector<std::string> sym_ini_candidates = {
            "symbols.ini",
            "C:\\Omega\\symbols.ini",
            "C:\\Omega\\config\\symbols.ini",
        };
        std::string sym_ini;
        for (const auto& candidate : sym_ini_candidates) {
            if (g_sym_cfg.load(candidate)) { sym_ini = candidate; break; }
        }
        if (!sym_ini.empty()) {
            // Helper: apply SymbolConfig to a BreakoutEngine.
            // TP_PCT = SL_PCT * tp_mult (scales TP relative to existing SL).
            // MAX_SPREAD: symbols.ini stores absolute price units. BreakoutEngine
            // uses MAX_SPREAD_PCT (% of mid). Convert: pct = abs / typical_price * 100.
            // Typical prices: FX ~1-150, indices ~4000-50000, oil ~80-100.
            // For FX the absolute spread (e.g. 0.0002) divided by price (~1.1) * 100 = 0.018% -- correct.
            // For indices (e.g. 2.5 pts / 6000) * 100 = 0.042% -- correct.
            // So we store the raw abs value and let the caller provide typical price.
            // Simpler: we know the instrument types -- pass typical_price per symbol.
            auto apply_be = [](auto& eng, const SymbolConfig& c, double typical_price) {
                if (c.sl_mult > 0.0 && c.sl_mult != 1.0) eng.SL_PCT *= c.sl_mult;
                if (c.tp_mult > 0.0)                      eng.TP_PCT  = eng.SL_PCT * c.tp_mult;
                if (c.max_spread > 0.0 && typical_price > 0.0)
                    eng.MAX_SPREAD_PCT = (c.max_spread / typical_price) * 100.0;
                if (c.min_hold_ms  > 0)   eng.MAX_HOLD_SEC  = c.min_hold_ms / 1000;
                if (c.max_hold_sec > 0)   eng.MAX_HOLD_SEC  = c.max_hold_sec;
                // Use >= 0 so MIN_EDGE_BP=0 (indices) actually disables the bp check.
                // > 0.0 left the default 6.0 in place when symbols.ini said 0.
                if (c.min_edge_bp    >= 0.0) eng.EDGE_CFG.min_edge_bp  = c.min_edge_bp;
                if (c.min_breakout_pct> 0.0) eng.MIN_BREAKOUT_PCT  = c.min_breakout_pct;
                if (c.min_range      > 0.0)  eng.MIN_COMP_RANGE    = c.min_range;
                if (c.min_confirm_ticks > 0) eng.MIN_CONFIRM_TICKS = c.min_confirm_ticks;
            };

            // BreakoutEngine symbols -- typical prices for MAX_SPREAD_PCT conversion
            apply_be(g_eng_sp,     g_sym_cfg.get("US500.F"), 6000.0);
            apply_be(g_eng_nq,     g_sym_cfg.get("USTEC.F"), 20000.0);
            apply_be(g_eng_cl,     g_sym_cfg.get("USOIL.F"), 80.0);
            apply_be(g_eng_us30,   g_sym_cfg.get("DJ30.F"),  42000.0);
            apply_be(g_eng_nas100, g_sym_cfg.get("NAS100"),  20000.0);
            apply_be(g_eng_ger30,  g_sym_cfg.get("GER40"),   22000.0);
            apply_be(g_eng_uk100,  g_sym_cfg.get("UK100"),   8500.0);
            apply_be(g_eng_estx50, g_sym_cfg.get("ESTX50"),  5300.0);
            apply_be(g_eng_xag,    g_sym_cfg.get("XAGUSD"),  30.0);
            apply_be(g_eng_eurusd, g_sym_cfg.get("EURUSD"),  1.10);
            apply_be(g_eng_gbpusd, g_sym_cfg.get("GBPUSD"),  1.27);
            apply_be(g_eng_audusd, g_sym_cfg.get("AUDUSD"),  0.65);
            apply_be(g_eng_nzdusd, g_sym_cfg.get("NZDUSD"),  0.60);
            apply_be(g_eng_usdjpy, g_sym_cfg.get("USDJPY"),  150.0);
            apply_be(g_eng_brent,  g_sym_cfg.get("BRENT"), 85.0);

            // Per-symbol WATCH_TIMEOUT_SEC -- 120s indices, 120s FX
            g_eng_ger30.WATCH_TIMEOUT_SEC  = 240;
            g_eng_uk100.WATCH_TIMEOUT_SEC  = 240;
            g_eng_estx50.WATCH_TIMEOUT_SEC = 240;
            g_eng_xag.WATCH_TIMEOUT_SEC    = 240;
            g_eng_eurusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_gbpusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_audusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_nzdusd.WATCH_TIMEOUT_SEC = 240;
            g_eng_usdjpy.WATCH_TIMEOUT_SEC = 240;
            g_eng_brent.WATCH_TIMEOUT_SEC  = 240;

            // BracketEngine symbols -- override configure() fields from symbols.ini.
            // NOTE: SLIPPAGE_BUFFER is NOT overridden here. The configure() calls above
            // already set correct per-symbol slippage in price-point units (0.50 for ESTX50,
            // 0.80 for gold, 0.00010 for EURUSD etc). The SLIPPAGE_EST_BP field in symbols.ini
            // is in basis-points which is only meaningful for FX -- converting bp*price/10000
            // for indices produces wildly inflated values (5bp*5600=2.80pts vs correct 0.50pts)
            // and blocks all index bracket trades. Do not re-derive it here.
            auto apply_bracket = [](auto& eng, const SymbolConfig& c) {
                if (c.min_range        > 0.0) eng.MIN_RANGE          = c.min_range;
                if (c.max_range        > 0.0) eng.MAX_RANGE          = c.max_range;
                if (c.min_structure_ms > 0)   eng.MIN_STRUCTURE_MS   = c.min_structure_ms;
                if (c.breakout_fail_ms > 0)   eng.FAILURE_WINDOW_MS  = c.breakout_fail_ms;
                if (c.min_hold_ms      > 0)   eng.MIN_HOLD_MS        = c.min_hold_ms;
                if (c.max_hold_sec     > 0)   eng.PENDING_TIMEOUT_SEC = c.max_hold_sec;
                if (c.tp_mult          > 0.0) eng.RR                  = c.tp_mult;
                if (c.max_spread       > 0.0) eng.MAX_SPREAD          = c.max_spread;
                // SLIPPAGE_BUFFER intentionally NOT set here -- configure() has correct values
            };
            apply_bracket(g_bracket_gold,   g_sym_cfg.get("XAUUSD"));
            apply_bracket(g_bracket_xag,    g_sym_cfg.get("XAGUSD"));
            apply_bracket(g_bracket_sp,     g_sym_cfg.get("US500.F"));
            apply_bracket(g_bracket_nq,     g_sym_cfg.get("USTEC.F"));
            apply_bracket(g_bracket_us30,   g_sym_cfg.get("DJ30.F"));
            apply_bracket(g_bracket_nas100, g_sym_cfg.get("NAS100"));
            apply_bracket(g_bracket_ger30,  g_sym_cfg.get("GER40"));
            apply_bracket(g_bracket_uk100,  g_sym_cfg.get("UK100"));
            apply_bracket(g_bracket_estx50, g_sym_cfg.get("ESTX50"));
            apply_bracket(g_bracket_brent,  g_sym_cfg.get("BRENT"));
            apply_bracket(g_bracket_eurusd, g_sym_cfg.get("EURUSD"));
            apply_bracket(g_bracket_gbpusd, g_sym_cfg.get("GBPUSD"));
            apply_bracket(g_bracket_audusd, g_sym_cfg.get("AUDUSD"));
            apply_bracket(g_bracket_nzdusd, g_sym_cfg.get("NZDUSD"));
            apply_bracket(g_bracket_usdjpy, g_sym_cfg.get("USDJPY"));
            std::cout << "[SYMCFG] All bracket engine params overridden from " << sym_ini << "\n";

            // Apply supervisor config from symbols.ini to each supervisor
            // apply_supervisor: wires SymbolConfig fields into supervisor cfg.
            // max_spread_pct is NOT in SymbolConfig -- it lives in OmegaConfig
            // per-symbol fields. Pass it explicitly per symbol so the supervisor's
            // spread gate fires at the correct threshold (was stuck at 0.10 default,
            // which at $24,000 = $2,400 threshold -- never triggered).
            auto apply_supervisor = [](omega::SymbolSupervisor& sup,
                                       const std::string& sym,
                                       const SymbolConfig& c,
                                       double max_spread_pct_override) {
                sup.symbol                          = sym;
                sup.cfg.allow_bracket               = c.allow_bracket;
                sup.cfg.allow_breakout              = c.allow_breakout;
                sup.cfg.min_regime_confidence       = c.min_regime_confidence;
                sup.cfg.min_engine_win_margin       = c.min_engine_win_margin;
                sup.cfg.min_winner_score            = c.min_winner_score;
                sup.cfg.min_bracket_score           = c.min_bracket_score;
                sup.cfg.max_false_breaks            = c.max_false_breaks;
                sup.cfg.bracket_in_quiet_comp       = c.bracket_in_quiet_comp;
                sup.cfg.breakout_in_trend           = c.breakout_in_trend;
                sup.cfg.cooldown_fail_threshold     = c.cooldown_fail_threshold;
                sup.cfg.cooldown_duration_ms        = static_cast<int64_t>(c.cooldown_duration_ms);
                sup.cfg.max_spread_pct              = max_spread_pct_override;
            };
            apply_supervisor(g_sup_sp,     "US500.F", g_sym_cfg.get("US500.F"), g_cfg.sp_max_spread_pct);
            apply_supervisor(g_sup_nq,     "USTEC.F", g_sym_cfg.get("USTEC.F"), g_cfg.nq_max_spread_pct);
            apply_supervisor(g_sup_cl,     "USOIL.F", g_sym_cfg.get("USOIL.F"), g_cfg.oil_max_spread_pct);
            apply_supervisor(g_sup_us30,   "DJ30.F",  g_sym_cfg.get("DJ30.F"),  g_cfg.us30_max_spread_pct);
            apply_supervisor(g_sup_nas100, "NAS100",  g_sym_cfg.get("NAS100"),  g_cfg.nas100_max_spread_pct);
            apply_supervisor(g_sup_ger30,  "GER40",   g_sym_cfg.get("GER40"),   g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_uk100,  "UK100",   g_sym_cfg.get("UK100"),   g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_estx50, "ESTX50",  g_sym_cfg.get("ESTX50"),  g_cfg.eu_index_max_spread_pct);
            apply_supervisor(g_sup_xag,    "XAGUSD",  g_sym_cfg.get("XAGUSD"),  g_cfg.silver_max_spread_pct);
            apply_supervisor(g_sup_eurusd, "EURUSD",  g_sym_cfg.get("EURUSD"),  g_cfg.fx_max_spread_pct);
            apply_supervisor(g_sup_gbpusd, "GBPUSD",  g_sym_cfg.get("GBPUSD"),  g_cfg.gbpusd_max_spread_pct);
            // AUDUSD/NZDUSD: use their dedicated max_spread_pct (0.030/0.035), NOT
            // fx_max_spread_pct (0.017). During Asia session AUD/NZD spreads widen to
            // ~2-3 pips (~0.025-0.030%), which exceeds 0.017% and locks the supervisor
            // in permanent HIGH_RISK_NO_TRADE. Their own spread thresholds were already
            // calibrated for this -- apply_supervisor was ignoring them.
            apply_supervisor(g_sup_audusd, "AUDUSD",  g_sym_cfg.get("AUDUSD"),  g_cfg.audusd_max_spread_pct);
            apply_supervisor(g_sup_nzdusd, "NZDUSD",  g_sym_cfg.get("NZDUSD"),  g_cfg.nzdusd_max_spread_pct);
            apply_supervisor(g_sup_usdjpy, "USDJPY",  g_sym_cfg.get("USDJPY"),  g_cfg.usdjpy_max_spread_pct);
            apply_supervisor(g_sup_brent,  "BRENT", g_sym_cfg.get("BRENT"), g_cfg.brent_max_spread_pct);
            apply_supervisor(g_sup_gold,   "XAUUSD",  g_sym_cfg.get("XAUUSD"),  g_cfg.bracket_gold_max_spread_pct);
            std::cout << "[SUPERVISOR] All supervisors configured from " << sym_ini << "\n";
            std::cout << "[SYMCFG] All engine params overridden from " << sym_ini << "\n";
        } else {
            // ?? CRITICAL: symbols.ini not found ??????????????????????????????????
            // Without symbols.ini: allow_bracket=false for all non-metals.
            // Bracket engines for indices/FX will NOT fire until symbols.ini is deployed.
            // This is intentional -- bracket config is not safe to run with compiled defaults.
            std::cout << "\n";
            std::cout << "[SYMCFG] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
            std::cout << "[SYMCFG] !!! CRITICAL: symbols.ini NOT FOUND -- BRACKET DISABLED !!!\n";
            std::cout << "[SYMCFG] !!! Searched:                                           !!!\n";
            std::cout << "[SYMCFG] !!!   symbols.ini                                       !!!\n";
            std::cout << "[SYMCFG] !!!   C:\\Omega\\symbols.ini                              !!!\n";
            std::cout << "[SYMCFG] !!!   C:\\Omega\\config\\symbols.ini                      !!!\n";
            std::cout << "[SYMCFG] !!! Copy symbols.ini to Omega.exe directory and restart !!!\n";
            std::cout << "[SYMCFG] !!! allow_bracket=false for all non-metals until fixed  !!!\n";
            std::cout << "[SYMCFG] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
            std::cout << "\n";
            // Assign symbol names directly so supervisor logs are readable even without ini
            g_sup_sp.symbol     = "US500.F"; g_sup_nq.symbol     = "USTEC.F";
            g_sup_cl.symbol     = "USOIL.F"; g_sup_us30.symbol   = "DJ30.F";
            g_sup_nas100.symbol = "NAS100";  g_sup_ger30.symbol  = "GER40";
            g_sup_uk100.symbol  = "UK100";   g_sup_estx50.symbol = "ESTX50";
            g_sup_xag.symbol    = "XAGUSD";  g_sup_gold.symbol   = "XAUUSD";
            g_sup_eurusd.symbol = "EURUSD";  g_sup_gbpusd.symbol = "GBPUSD";
            g_sup_audusd.symbol = "AUDUSD";  g_sup_nzdusd.symbol = "NZDUSD";
            g_sup_usdjpy.symbol = "USDJPY";  g_sup_brent.symbol  = "BRENT";
            // Without symbols.ini: disable bracket on non-metals (no bracket engine exists)
            for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_cl, &g_sup_us30, &g_sup_nas100,
                              &g_sup_ger30, &g_sup_uk100, &g_sup_estx50,
                              &g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                              &g_sup_nzdusd, &g_sup_usdjpy, &g_sup_brent})
                sup->cfg.allow_bracket = false;
            // Raise cooldown threshold from default 3 to 20
            for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_cl, &g_sup_us30, &g_sup_nas100,
                              &g_sup_ger30, &g_sup_uk100, &g_sup_estx50, &g_sup_xag,
                              &g_sup_gold, &g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                              &g_sup_nzdusd, &g_sup_usdjpy, &g_sup_brent})
                sup->cfg.cooldown_fail_threshold = 20;
        }
    }
    // Reset all supervisor hysteresis counters on startup.
    // Prevents stale HIGH_RISK/CHOP counts from a prior session blocking
    // trading before baseline vol warms (typically first 60-120 ticks).
    for (auto* sup : {&g_sup_sp, &g_sup_nq, &g_sup_cl, &g_sup_us30, &g_sup_nas100,
                      &g_sup_ger30, &g_sup_uk100, &g_sup_estx50, &g_sup_xag,
                      &g_sup_gold, &g_sup_eurusd, &g_sup_gbpusd, &g_sup_audusd,
                      &g_sup_nzdusd, &g_sup_usdjpy, &g_sup_brent})
        sup->reset();
    printf("[SUPERVISOR] All hysteresis counters reset on startup\n");
    fflush(stdout);
    // ?? Hot-reload config watcher ??????????????????????????????????????????????
    // Watches omega_config.ini every 2s. On change: re-runs load_config() +
    // sanitize_config() + all apply_engine_config() calls -- zero downtime,
    // no position closes, no reconnect. Change any param in the .ini and it
    // takes effect within 2 seconds without rebooting Omega.
    // NOT reloadable: FIX host/port/credentials, mode (SHADOW/LIVE), code changes.
    OmegaHotReload::start(cfg_path, [&cfg_path]() {
        load_config(cfg_path);
        sanitize_config();
        apply_engine_config(g_eng_sp);
        apply_engine_config(g_eng_nq);
        apply_engine_config(g_eng_cl);
        apply_engine_config(g_eng_us30);
        apply_engine_config(g_eng_nas100);
        apply_generic_index_config(g_eng_ger30);
        apply_generic_index_config(g_eng_uk100);
        apply_generic_index_config(g_eng_estx50);
        apply_generic_silver_config(g_eng_xag);
        apply_generic_fx_config(g_eng_eurusd);
        apply_generic_gbpusd_config(g_eng_gbpusd);
        apply_generic_audusd_config(g_eng_audusd);
        apply_generic_nzdusd_config(g_eng_nzdusd);
        apply_generic_usdjpy_config(g_eng_usdjpy);
        apply_generic_brent_config(g_eng_brent);
        printf("[HOT-RELOAD] All engine configs refreshed\n");
        fflush(stdout);
    });

    // ?? Position sizing ???????????????????????????????????????????????????????
    // ENTRY_SIZE on each engine is the FALLBACK lot used only when
    // risk_per_trade_usd == 0. When risk sizing is active, compute_size()
    // calculates the lot dynamically and ENTRY_SIZE is never used.
    // NAS100 broker minimum is 0.10 lots -- enforced as fallback and in compute_size floor.
    if (g_cfg.risk_per_trade_usd <= 0.0) {
        // Fixed lot mode -- risk sizing disabled, use hardcoded fallbacks
        g_eng_sp.ENTRY_SIZE       = 0.01;
        g_eng_nq.ENTRY_SIZE       = 0.01;
        g_eng_cl.ENTRY_SIZE       = 0.01;
        g_eng_us30.ENTRY_SIZE     = 0.01;
        g_eng_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_eng_ger30.ENTRY_SIZE    = 0.01;
        g_eng_uk100.ENTRY_SIZE    = 0.01;
        g_eng_estx50.ENTRY_SIZE   = 0.01;
        g_eng_xag.ENTRY_SIZE      = 0.01;
        g_eng_eurusd.ENTRY_SIZE   = 0.01;
        g_eng_gbpusd.ENTRY_SIZE   = 0.01;
        g_eng_brent.ENTRY_SIZE    = 0.01;
        // Bracket engine fallbacks -- same as breakout engines
        g_bracket_sp.ENTRY_SIZE       = 0.01;
        g_bracket_nq.ENTRY_SIZE       = 0.01;
        g_bracket_us30.ENTRY_SIZE     = 0.01;
        g_bracket_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_bracket_ger30.ENTRY_SIZE    = 0.01;
        g_bracket_uk100.ENTRY_SIZE    = 0.01;
        g_bracket_estx50.ENTRY_SIZE   = 0.01;
        g_bracket_brent.ENTRY_SIZE    = 0.01;
        g_bracket_eurusd.ENTRY_SIZE   = 0.01;
        g_bracket_gbpusd.ENTRY_SIZE   = 0.01;
        g_bracket_audusd.ENTRY_SIZE   = 0.01;
        g_bracket_nzdusd.ENTRY_SIZE   = 0.01;
        g_bracket_usdjpy.ENTRY_SIZE   = 0.01;
        std::cout << "[SIZING] Fixed lot mode active (risk_per_trade_usd=0)\n"
                  << "[SIZING]   All instruments: 0.01 lots | NAS100: 0.10 lots\n";
    } else {
        // Risk-based sizing active -- ENTRY_SIZE is only a safety fallback,
        // compute_size() drives actual lot size from risk_per_trade_usd.
        // Set fallbacks to sensible minimums in case compute_size() ever bails.
        g_eng_sp.ENTRY_SIZE       = 0.01;
        g_eng_nq.ENTRY_SIZE       = 0.01;
        g_eng_cl.ENTRY_SIZE       = 0.01;
        g_eng_us30.ENTRY_SIZE     = 0.01;
        g_eng_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_eng_ger30.ENTRY_SIZE    = 0.01;
        g_eng_uk100.ENTRY_SIZE    = 0.10;  // indices: $10 / ~8pt SL * $1/pt = 1.25 ? capped at max
        g_eng_estx50.ENTRY_SIZE   = 0.10;
        g_eng_xag.ENTRY_SIZE      = 0.01;
        g_eng_eurusd.ENTRY_SIZE   = 0.01;
        g_eng_gbpusd.ENTRY_SIZE   = 0.01;
        g_eng_brent.ENTRY_SIZE    = 0.01;
        // Bracket engine fallbacks
        g_bracket_sp.ENTRY_SIZE       = 0.10;
        g_bracket_nq.ENTRY_SIZE       = 0.10;
        g_bracket_us30.ENTRY_SIZE     = 0.10;
        g_bracket_nas100.ENTRY_SIZE   = 0.10;  // NAS100 broker minimum
        g_bracket_ger30.ENTRY_SIZE    = 0.10;
        g_bracket_uk100.ENTRY_SIZE    = 0.10;
        g_bracket_estx50.ENTRY_SIZE   = 0.10;
        g_bracket_brent.ENTRY_SIZE    = 0.01;
        g_bracket_eurusd.ENTRY_SIZE   = 0.01;
        g_bracket_gbpusd.ENTRY_SIZE   = 0.01;
        g_bracket_audusd.ENTRY_SIZE   = 0.01;
        g_bracket_nzdusd.ENTRY_SIZE   = 0.01;
        g_bracket_usdjpy.ENTRY_SIZE   = 0.01;
        std::cout << "[SIZING] Risk-based sizing active (risk_per_trade_usd=$"
                  << g_cfg.risk_per_trade_usd << ")\n"
                  << "[SIZING]   Lot size computed dynamically per trade from SL distance\n"
                  << "[SIZING]   Fallback (if compute_size fails): indices=0.10 | others=0.01\n";
    }

    // Wire account equity to edge model -- applies in all modes.
    // Shadow uses the same account_equity for Kelly sizing as live would.
    {
        const double acct_eq = g_cfg.account_equity;
        g_eng_sp.ACCOUNT_EQUITY     = acct_eq;
        g_eng_nq.ACCOUNT_EQUITY     = acct_eq;
        g_eng_cl.ACCOUNT_EQUITY     = acct_eq;
        g_eng_us30.ACCOUNT_EQUITY   = acct_eq;
        g_eng_nas100.ACCOUNT_EQUITY = acct_eq;
        g_eng_ger30.ACCOUNT_EQUITY  = acct_eq;
        g_eng_uk100.ACCOUNT_EQUITY  = acct_eq;
        g_eng_estx50.ACCOUNT_EQUITY = acct_eq;
        g_eng_xag.ACCOUNT_EQUITY    = acct_eq;
        g_eng_eurusd.ACCOUNT_EQUITY = acct_eq;
        g_eng_gbpusd.ACCOUNT_EQUITY = acct_eq;
        g_eng_brent.ACCOUNT_EQUITY  = acct_eq;
        std::cout << "[SIZING] account_equity=" << acct_eq
                  << (g_cfg.mode == "LIVE" ? " (LIVE)" : " (SHADOW -- same as live)") << "\n";
    }
    std::cout.flush();

    // GoldEngineStack config -- applies all [gold_stack] ini values.
    // Must be called AFTER load_config(). Defaults are safe (match prior constexpr).
    g_gold_stack.configure(g_cfg.gs_cfg);

    // LatencyEdgeStack config -- applies all [latency_edge] ini values.
    // Must be called AFTER load_config(). Defaults are safe (match prior constexpr).
    g_le_stack.configure(g_cfg.le_cfg);

    // ?? SHELVED ENGINE DISABLE -- 2026-03-31 ??????????????????????????????????
    // Engines disabled based on live performance audit. Each engine's own
    // guard (if (!enabled_) return noSignal()) prevents new entries.
    // Existing positions (if any) are still drained via has_open_position() paths.
    //
    // NBM indices: live data insufficient, not validated. Still shelved.
    g_nbm_sp.enabled     = false;
    g_nbm_nq.enabled     = false;
    g_nbm_nas.enabled    = false;
    g_nbm_us30.enabled   = false;
    // NBM gold london: RE-ENABLED 2026-04-01 -- live MT5 data confirms the logic
    // (London open ATR breakout, 51min hold, +$185). Omega NBM is identical concept.
    g_nbm_gold_london.enabled = true;
    g_nbm_oil_london.enabled  = false;
    //
    // ORB (OpeningRange): no live data. Shelved pending shadow validation.
    g_orb_us.enabled     = false;
    g_orb_ger30.enabled  = false;
    g_orb_uk100.enabled  = false;
    g_orb_estx50.enabled = false;
    g_orb_silver.enabled = false;
    //
    // Cross-asset: EIA fade, BrentWTI spread, FX cascade, carry unwind.
    // All have insufficient live data. Shelved pending shadow validation.
    g_ca_eia_fade.enabled    = false;
    g_ca_brent_wti.enabled   = false;
    g_ca_fx_cascade.enabled  = false;
    g_ca_carry_unwind.enabled = false;
    // ESNQ: already guarded by esnq_enabled=false in config. Belt-and-suspenders.
    g_ca_esnq.enabled        = false;
    // ?? END SHELVED ENGINE DISABLE ????????????????????????????????????????????

    // ?? Adaptive intelligence layer startup ???????????????????????????????????
    {
        const int64_t now_s = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        // Configure news blackout scheduler from config flags (defaults are fine, shown for clarity)
        g_news_blackout.scheduler.block_nfp   = true;
        g_news_blackout.scheduler.block_fomc  = true;
        g_news_blackout.scheduler.block_cpi   = true;
        g_news_blackout.scheduler.block_eia   = true;
        g_news_blackout.scheduler.block_cb    = true;

        // ?? Live calendar: inject exact event times from Forex Factory ????????
        // Fetches this week's HIGH-impact events over HTTPS and injects precise
        // blackout windows. Falls back gracefully to hardcoded schedule on failure.
        // Pre: 5 min before event. Post: 15 min after event.
        g_live_calendar.pre_min  = 5;
        g_live_calendar.post_min = 15;
        g_live_calendar.refresh_interval_sec = 86400; // re-fetch daily
        g_live_calendar.refresh(g_news_blackout, now_s);

        // Print final schedule (includes both hardcoded + live injected windows)
        g_news_blackout.print_schedule(now_s);

        // ?? Edge systems startup configuration ???????????????????????????????
        // TOD gate: load historical bucket data so gate is active from first trade
        {
            const std::string tod_path = log_root_dir() + "/omega_tod_buckets.csv";
            g_edges.tod.load_csv(tod_path);
            g_edges.tod.min_trades   = 30;    // need 30 trades per bucket before blocking
            g_edges.tod.min_win_rate = 0.38;  // block if WR < 38%
            g_edges.tod.min_avg_ev_usd = -2.0; // block if avg EV < -$2/trade
            g_edges.tod.enabled      = true;  // active in all modes -- shadow is a simulation
        }
        // Fill quality: load persisted fill history so adverse selection detection is
        // active from the first trade -- no minimum fill count, history survives restarts.
        g_edges.fill_quality.load_csv(log_root_dir() + "/fill_quality.csv");
        // Spread Z-score gate: starts building after first 20 ticks per symbol
        g_edges.spread_gate.window_ticks = 200;
        g_edges.spread_gate.max_z_score  = 3.0; // conservative: 3? anomaly
        g_edges.spread_gate.enabled      = true;  // active in all modes -- shadow is a simulation
        // Round number filter
        g_edges.round_numbers.proximity_frac = 0.08; // within 8% of increment = "near"
        // Previous day levels
        g_edges.prev_day.proximity_frac = 0.05; // within 5% of prior range = "near PDH/PDL"
        // FX fix engines
        g_edges.fx_fix.tp_pips        = 15.0;
        g_edges.fx_fix.sl_pips        = 8.0;
        g_edges.fx_fix.min_cvd_signal = 0.12; // need 12% normalised CVD to enter fix
        g_edges.fx_fix.enabled        = true;  // active in all modes -- shadow is a simulation
        // Fill quality
        g_edges.fill_quality.window_trades = 50;
        g_edges.fill_quality.adverse_threshold_bps = 2.0;
        g_edges.fill_quality.enabled = true;
        std::printf("[EDGES] All 7 edge systems initialised\n");

        // Adaptive risk: start with Kelly disabled for first 15 trades per symbol
        // (confidence ramps from 0?1 automatically as trades accumulate)
        g_adaptive_risk.kelly_enabled            = true;
        g_adaptive_risk.dd_throttle_enabled      = true;
        g_adaptive_risk.corr_heat_enabled        = true;
        g_adaptive_risk.vol_regime_enabled       = true;
        g_adaptive_risk.multiday_throttle_enabled = true;
        g_adaptive_risk.fill_quality_enabled     = true;
        g_adaptive_risk.fill_quality_scale       = 0.70;  // 30% size cut on adverse selection

        // Drawdown velocity circuit breaker: halt new entries for 15 min if we
        // lose more than 50% of daily_loss_limit within any 30-minute window.
        // e.g. daily_loss_limit=$200 ? fires if rolling 30-min loss > $100.
        // Set threshold_usd=0 to disable (off by default until shadow validates).
        g_adaptive_risk.dd_velocity.threshold_usd = g_cfg.daily_loss_limit * 0.5;
        g_adaptive_risk.dd_velocity.window_sec    = 1800;   // 30-minute window
        g_adaptive_risk.dd_velocity.halt_sec      = 900;    // 15-minute halt
        std::printf("[ADAPTIVE] DD-velocity threshold=$%.0f/30min halt=15min\n",
                    g_adaptive_risk.dd_velocity.threshold_usd);

        // Correlation cluster limits -- 2 is the safe default for all clusters
        g_adaptive_risk.corr_heat.max_per_cluster_us_equity = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_eu_equity = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_oil       = 1;  // never both OIL+BRENT open
        g_adaptive_risk.corr_heat.max_per_cluster_metals    = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_jpy_risk  = 2;
        g_adaptive_risk.corr_heat.max_per_cluster_eur_gbp   = 1;  // EURUSD or GBPUSD, not both

        // Load Kelly performance history from previous sessions
        {
            const std::string kelly_dir = log_root_dir() + "/kelly";
            // ensure_parent_dir creates the directory if needed
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(kelly_dir, ec);
            g_adaptive_risk.load_perf(kelly_dir);
        }

        // Load GoldFlowEngine ATR state -- eliminates 100-tick blind zone on restart
        {
            const std::string atr_path        = log_root_dir() + "/gold_flow_atr.dat";
            const std::string atr_backup_path = log_root_dir() + "/gold_flow_atr_backup.dat";

            g_gold_flow.load_atr_state(atr_path);  // try primary first

            // If primary failed (corrupt, missing, stale), fall back to backup (~60s older).
            // Backup is written every 60s so it survives a hard kill that corrupts the primary.
            if (g_gold_flow.current_atr() <= 0.0) {
                printf("[GFE] Primary ATR load failed -- trying backup %s\n",
                       atr_backup_path.c_str());
                g_gold_flow.load_atr_state(atr_backup_path);
                if (g_gold_flow.current_atr() > 0.0) {
                    printf("[GFE] ATR restored from backup: atr=%.4f\n",
                           g_gold_flow.current_atr());
                } else {
                    printf("[GFE] ATR backup also failed -- cold seed will be used\n");
                }
            }

            // Pass VIX at startup -- if VIX feed not yet live this gives 10pt default
            // which is far safer than the old 3pt hardcoded seed
            g_gold_flow.seed(0.0, g_macro_ctx.vix);  // VIX-scaled fallback if no ATR file
            printf("[GFE] Startup ATR: m_atr=%.4f (%s)\n",
                   g_gold_flow.current_atr(),
                   g_gold_flow.current_atr() > 0.0 ? "warmed" : "cold-seeded");

            // ?? Velocity trail shadow mode ????????????????????????????????????
            // Run velocity trail in observation mode by default: SL/time-stop logic
            // is fully active but [VEL-TRAIL-SHADOW] logs are emitted for every
            // regime classification so we can verify on real ticks before live.
            // To go live: set velocity_shadow_mode = false on both instances.
            g_gold_flow.velocity_shadow_mode        = true;
            g_gold_flow_reload.velocity_shadow_mode = true;
            printf("[GFE] Velocity trail: SHADOW mode (observation only -- verify regime logs before enabling live)\n");
            fflush(stdout);

            // ?? Hard stop broker-side order callback ??????????????????????
            // Wires GoldFlowEngine's on_hard_stop to send_hard_stop_order so
            // the broker holds a resting LIMIT order even if Omega crashes.
            // Fires once per position when adverse >= 20pts (tombstone-guarded).
            g_gold_flow.on_hard_stop = [](const std::string& sym, bool is_long,
                                          double qty, double sl_px, int64_t ts) {
                send_hard_stop_order(sym, is_long, qty, sl_px, ts);
            };
            g_gold_flow_reload.on_hard_stop = [](const std::string& sym, bool is_long,
                                                  double qty, double sl_px, int64_t ts) {
                send_hard_stop_order(sym, is_long, qty, sl_px, ts);
            };
            printf("[GFE] Hard stop: ARMED (broker LIMIT order fires at 20pts adverse)\n");
            fflush(stdout);
            // [BUG-4 FIX] Log goldflow_enabled status at startup so operators know
            // immediately whether GoldFlow entries are active or silently disabled.
            printf("[GFE-CONFIG] goldflow_enabled=%s -- GoldFlow entries are %s\n",
                   g_cfg.goldflow_enabled ? "true" : "false",
                   g_cfg.goldflow_enabled ? "ACTIVE (L2 watchdog gated)" : "DISABLED (no entries will fire)");
            fflush(stdout);

            // ?? Velocity add-on callback ??????????????????????????????????????????
            // Wires GoldFlowEngine's on_addon to fire g_gold_flow_addon when the base
            // position confirms a velocity expansion move (trail_stage>=2 + vol>2.5).
            // Add-on instance uses same session/risk gates as reload.
            // Shadow mode disabled -- gates verified in incident testing + code audit.
            g_gold_flow.addon_shadow_mode = false;  // LIVE: [GFE-ADDON] fires real entries
            g_gold_flow.on_addon = [](bool is_long, double bid, double ask,
                                      double atr, double base_trail_sl,
                                      int64_t base_entry_ts) {
                // Gate: addon instance must be flat (no double-stacking)
                if (g_gold_flow_addon.has_open_position()) {
                    printf("[GFE-ADDON] BLOCKED -- addon instance already has open position\n");
                    fflush(stdout);
                    return;
                }
                // Gate: daily loss proximity -- if within 20% of daily limit, skip
                {
                    const double daily_pnl = g_omegaLedger.dailyPnl();
                    if (daily_pnl < -g_cfg.daily_loss_limit * 0.80) {
                        printf("[GFE-ADDON] BLOCKED -- daily_pnl=$%.2f within 20%% of limit=$%.2f\n",
                               daily_pnl, g_cfg.daily_loss_limit);
                        fflush(stdout);
                        return;
                    }
                }
                // Seed addon instance ATR and force-entry
                const double addon_size = std::max(GFE_MIN_LOT,
                    std::min(g_gold_flow.pos.full_size * 0.5, GFE_MAX_LOT));
                g_gold_flow_addon.risk_dollars        = addon_size * std::fabs(
                    (is_long ? ask : bid) - base_trail_sl) * 100.0;
                g_gold_flow_addon.addon_shadow_mode   = false; // addon instance never recurses
                g_gold_flow_addon.velocity_shadow_mode = g_gold_flow.velocity_shadow_mode;
                g_gold_flow_addon.on_hard_stop = [](const std::string& sym, bool il,
                                                     double qty, double sl_px, int64_t ts) {
                    send_hard_stop_order(sym, il, qty, sl_px, ts);
                };
                // force_entry bypasses all persistence gates -- addon confirmation
                // was already done by the base position's 10+ gates.
                // Clamp session_slot: -1 (supervisor not warmed) -> 1 (London fallback).
                const int addon_slot = (g_macro_ctx.session_slot >= 0)
                    ? g_macro_ctx.session_slot : 1;
                const bool entered = g_gold_flow_addon.force_entry(
                    is_long, bid, ask, atr, 
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()),
                    addon_slot);
                if (entered) {
                    // Override the addon SL to be exactly the base trail SL
                    // force_entry sets SL based on ATR -- we want it at base trail
                    if (is_long  && base_trail_sl < g_gold_flow_addon.pos.entry)
                        g_gold_flow_addon.pos.sl = base_trail_sl;
                    if (!is_long && base_trail_sl > g_gold_flow_addon.pos.entry)
                        g_gold_flow_addon.pos.sl = base_trail_sl;
                    printf("[GFE-ADDON] ENTERED %s @ %.2f sl=%.2f(base_trail) size=%.4f "
                           "base_entry_ts=%lld\n",
                           is_long ? "LONG" : "SHORT",
                           is_long ? ask : bid,
                           g_gold_flow_addon.pos.sl,
                           g_gold_flow_addon.pos.size,
                           (long long)base_entry_ts);
                    fflush(stdout);
                    // Send live order
                    send_live_order("XAUUSD", is_long, g_gold_flow_addon.pos.size,
                                    is_long ? ask : bid);
                } else {
                    printf("[GFE-ADDON] force_entry failed (already has position?)\n");
                    fflush(stdout);
                }
            };
            printf("[GFE] Velocity add-on: LIVE mode -- [GFE-ADDON] fires on confirmed expansion moves\n");
            fflush(stdout);
        }

        // ?? Bleed-flip callback ??????????????????????????????????????????????????
        // When bleed guard fires (stalled at BE for 5min), flip direction on reload.
        // Original trade momentum is dead -- the opposing force is winning. Trade it.
        g_gold_flow.on_bleed_flip = [](bool flip_is_long, double bid, double ask,
                                        double atr, double original_entry) {
            // Gate 1: reload must be flat
            if (g_gold_flow_reload.has_open_position()) {
                printf("[GFE-BLEED-FLIP] BLOCKED -- reload already has position\n");
                fflush(stdout);
                return;
            }
            // Gate 2: daily loss proximity
            if (g_omegaLedger.dailyPnl() < -g_cfg.daily_loss_limit * 0.80) {
                printf("[GFE-BLEED-FLIP] BLOCKED -- near daily loss limit\n");
                fflush(stdout);
                return;
            }
            // Gate 3: spread must be normal (don't flip into wide spread)
            // Uses ask-bid directly -- if > 1.0pt skip
            if ((ask - bid) > 1.0) {
                printf("[GFE-BLEED-FLIP] BLOCKED -- spread=%.2f too wide\n", ask - bid);
                fflush(stdout);
                return;
            }
            // Size: half the normal risk -- flip is a mean-reversion scalp, not a trend ride
            g_gold_flow_reload.risk_dollars = g_cfg.risk_per_trade_usd * 0.5;
            g_gold_flow_reload.addon_shadow_mode   = false;
            g_gold_flow_reload.velocity_shadow_mode = false;
            g_gold_flow_reload.on_hard_stop = [](const std::string& sym, bool il,
                                                  double qty, double sl_px, int64_t ts) {
                send_hard_stop_order(sym, il, qty, sl_px, ts);
            };
            const int flip_slot = (g_macro_ctx.session_slot >= 0)
                ? g_macro_ctx.session_slot : 1;
            const bool entered = g_gold_flow_reload.force_entry(
                flip_is_long, bid, ask, atr,
                static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()),
                flip_slot);
            if (entered) {
                // Override SL: tight at 0.5*ATR -- flip must work fast or not at all
                const double flip_sl = flip_is_long
                    ? (g_gold_flow_reload.pos.entry - atr * 0.50)
                    : (g_gold_flow_reload.pos.entry + atr * 0.50);
                g_gold_flow_reload.pos.sl = flip_sl;
                // Override TP via tight trail: set MFE target at 1.5*ATR
                // (mean reversion -- price should return to original_entry area)
                printf("[GFE-BLEED-FLIP] ENTERED %s @ %.2f sl=%.2f "
                       "target=%.2f(orig_entry) atr=%.2f size=%.4f\n",
                       flip_is_long ? "LONG" : "SHORT",
                       flip_is_long ? ask : bid,
                       flip_sl, original_entry, atr,
                       g_gold_flow_reload.pos.size);
                fflush(stdout);
                send_live_order("XAUUSD", flip_is_long,
                                g_gold_flow_reload.pos.size,
                                flip_is_long ? ask : bid);
            } else {
                printf("[GFE-BLEED-FLIP] force_entry failed\n");
                fflush(stdout);
            }
        };

        // Load GoldStack vol baseline + governor EWM -- skips 400-tick regime warmup
        {
            const std::string gs_path = log_root_dir() + "/gold_stack_state.dat";
            g_gold_stack.load_atr_state(gs_path);
        }

        // Multi-day drawdown throttle: load history, log startup state
        {
            const std::string md_path = log_root_dir() + "/day_results.csv";
            g_adaptive_risk.multiday.load(md_path);
            const int streak      = g_adaptive_risk.multiday.consecutive_losing_days();
            const double md_scale = g_adaptive_risk.multiday.size_scale();
            g_telemetry.UpdateMultiDayThrottle(streak, md_scale,
                                               g_adaptive_risk.multiday.is_active() ? 1 : 0);
            if (g_adaptive_risk.multiday.is_active())
                std::cout << "[MULTIDAY-THROTTLE] *** ACTIVE *** consec_loss=" << streak
                          << " -> sizes halved this session\n";
        }

        // Wire fill quality adverse selection check into adjusted_lot().
        // Lambda avoids circular include between OmegaAdaptiveRisk and OmegaEdges.
        g_adaptive_risk.fill_quality_check_fn = [](const std::string& sym) -> bool {
            return g_edges.fill_quality.adverse_selection_detected(sym);
        };

        // Weekend gap size scale: halve lots during Fri 21:00 - Sun 22:00 UTC.
        g_adaptive_risk.weekend_gap_scale_fn = []() -> double {
            return weekend_gap_size_scale();
        };

        // VPIN: enabled, registers size-scale callback into adjusted_lot.
        // block_threshold=0.80 (entry blocked), high_threshold=0.60 (0.5? size).
        g_edges.vpin.enabled         = true;
        g_edges.vpin.bucket_size     = 50;
        g_edges.vpin.window_buckets  = 10;
        g_edges.vpin.high_threshold  = 0.60;
        g_edges.vpin.block_threshold = 0.80;
        g_adaptive_risk.vpin_scale_fn = [](const std::string& sym) -> double {
            return g_edges.vpin.size_scale(sym);
        };

        // Walk-forward OOS validation scale (RenTec #6).
        // Updated every 20 trades via handle_closed_trade -> g_walk_forward.update().
        // Returns 1.0 (pass), 0.75 (degraded), 0.50 (failing).
        // No penalty during warmup (< 40 trades).
        g_adaptive_risk.wfo_scale_fn = [](const std::string& sym) -> double {
            return g_walk_forward.scale(sym);
        };

        // Regime adaptor is enabled; in SHADOW mode it is informational only
        g_regime_adaptor.enabled = true;

        // Portfolio VaR: correlation-adjusted exposure gate.
        // Limit = 1.5 ? daily_loss_limit. At $200 daily limit ? blocks when
        // correlated exposure implies >$300 of potential simultaneous loss.
        g_portfolio_var.init_betas();
        g_portfolio_var.var_limit_usd = g_cfg.daily_loss_limit * 1.5;
        // Correlation matrix -- load warm state from previous session
        g_corr_matrix.load_state(log_root_dir() + "/corr_matrix.dat");
        // VPIN -- reset at session start (stale tick-classification carries no meaning)
        g_vpin.reset();
        g_vpin.toxic_threshold = 0.70;  // block entries above 70% toxic flow
        std::printf("[PORTFOLIO-VAR] limit=$%.0f (1.5? daily_loss_limit)\n",
                    g_portfolio_var.var_limit_usd);

        std::cout << "[ADAPTIVE] Kelly=" << (g_adaptive_risk.kelly_enabled ? "ON" : "OFF")
                  << "  DDthrottle="    << (g_adaptive_risk.dd_throttle_enabled ? "ON" : "OFF")
                  << "  CorrHeat="      << (g_adaptive_risk.corr_heat_enabled ? "ON" : "OFF")
                  << "  VolRegime="     << (g_adaptive_risk.vol_regime_enabled ? "ON" : "OFF")
                  << "  MultiDayDD="    << (g_adaptive_risk.multiday_throttle_enabled ? "ON" : "OFF")
                  << "  FillQuality="   << (g_adaptive_risk.fill_quality_enabled ? "ON" : "OFF")
                  << "  NewsBlackout="  << (g_news_blackout.enabled ? "ON" : "OFF")
                  << "  PartialExit="   << (g_partial_exit.enabled ? "ON" : "OFF")
                  << "  RegimeAdaptor=" << (g_regime_adaptor.enabled ? "ON" : "OFF") << "\n";

        std::cout << "[ADAPTIVE] Corr cluster limits:"
                  << " US_EQ=" << g_adaptive_risk.corr_heat.max_per_cluster_us_equity
                  << " EU_EQ=" << g_adaptive_risk.corr_heat.max_per_cluster_eu_equity
                  << " OIL="   << g_adaptive_risk.corr_heat.max_per_cluster_oil
                  << " METALS=" << g_adaptive_risk.corr_heat.max_per_cluster_metals
                  << " JPY="   << g_adaptive_risk.corr_heat.max_per_cluster_jpy_risk
                  << " EUR_GBP=" << g_adaptive_risk.corr_heat.max_per_cluster_eur_gbp << "\n";
    }

    // ?? Startup parameter validation -- logged on every start ?????????????????
    // This block documents the exact live values every engine will use.
    // Any mismatch between config intent and actual values is visible immediately.
    std::cout << "\n[OMEGA-PARAMS] ???????????????????????????????????????????\n"
              << "[OMEGA-PARAMS] ENGINE PARAMETER AUDIT (live values after all config overrides)\n"
              << "[OMEGA-PARAMS] ???????????????????????????????????????????\n"
              << "[OMEGA-PARAMS] US500.F  TP=" << g_eng_sp.TP_PCT  << "% SL=" << g_eng_sp.SL_PCT
              << "% vol=" << g_eng_sp.VOL_THRESH_PCT << "% mom=" << g_eng_sp.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_sp.MIN_BREAKOUT_PCT << "% gap=" << g_eng_sp.MIN_GAP_SEC
              << "s hold=" << g_eng_sp.MAX_HOLD_SEC << "s spread=" << g_eng_sp.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] USTEC.F  TP=" << g_eng_nq.TP_PCT  << "% SL=" << g_eng_nq.SL_PCT
              << "% vol=" << g_eng_nq.VOL_THRESH_PCT << "% mom=" << g_eng_nq.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_nq.MIN_BREAKOUT_PCT << "% gap=" << g_eng_nq.MIN_GAP_SEC
              << "s hold=" << g_eng_nq.MAX_HOLD_SEC << "s spread=" << g_eng_nq.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] USOIL.F  TP=" << g_eng_cl.TP_PCT  << "% SL=" << g_eng_cl.SL_PCT
              << "% vol=" << g_eng_cl.VOL_THRESH_PCT << "% mom=" << g_eng_cl.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_cl.MIN_BREAKOUT_PCT << "% gap=" << g_eng_cl.MIN_GAP_SEC
              << "s hold=" << g_eng_cl.MAX_HOLD_SEC << "s spread=" << g_eng_cl.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] DJ30.F   TP=" << g_eng_us30.TP_PCT << "% SL=" << g_eng_us30.SL_PCT
              << "% vol=" << g_eng_us30.VOL_THRESH_PCT << "% mom=" << g_eng_us30.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_us30.MIN_BREAKOUT_PCT << "% gap=" << g_eng_us30.MIN_GAP_SEC
              << "s hold=" << g_eng_us30.MAX_HOLD_SEC << "s spread=" << g_eng_us30.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] NAS100   TP=" << g_eng_nas100.TP_PCT << "% SL=" << g_eng_nas100.SL_PCT
              << "% vol=" << g_eng_nas100.VOL_THRESH_PCT << "% mom=" << g_eng_nas100.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_nas100.MIN_BREAKOUT_PCT << "% gap=" << g_eng_nas100.MIN_GAP_SEC
              << "s hold=" << g_eng_nas100.MAX_HOLD_SEC << "s spread=" << g_eng_nas100.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] XAGUSD   TP=" << g_eng_xag.TP_PCT  << "% SL=" << g_eng_xag.SL_PCT
              << "% vol=" << g_eng_xag.VOL_THRESH_PCT << "% mom=" << g_eng_xag.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_xag.MIN_BREAKOUT_PCT << "% gap=" << g_eng_xag.MIN_GAP_SEC
              << "s hold=" << g_eng_xag.MAX_HOLD_SEC << "s spread=" << g_eng_xag.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] AUDUSD   TP=" << g_eng_audusd.TP_PCT << "% SL=" << g_eng_audusd.SL_PCT
              << "% vol=" << g_eng_audusd.VOL_THRESH_PCT << "% mom=" << g_eng_audusd.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_audusd.MIN_GAP_SEC << "s spread=" << g_eng_audusd.MAX_SPREAD_PCT << "% [ASIA]\n"
              << "[OMEGA-PARAMS] NZDUSD   TP=" << g_eng_nzdusd.TP_PCT << "% SL=" << g_eng_nzdusd.SL_PCT
              << "% vol=" << g_eng_nzdusd.VOL_THRESH_PCT << "% mom=" << g_eng_nzdusd.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_nzdusd.MIN_GAP_SEC << "s spread=" << g_eng_nzdusd.MAX_SPREAD_PCT << "% [ASIA]\n"
              << "[OMEGA-PARAMS] USDJPY   TP=" << g_eng_usdjpy.TP_PCT << "% SL=" << g_eng_usdjpy.SL_PCT
              << "% vol=" << g_eng_usdjpy.VOL_THRESH_PCT << "% mom=" << g_eng_usdjpy.MOMENTUM_THRESH_PCT
              << "% gap=" << g_eng_usdjpy.MIN_GAP_SEC << "s spread=" << g_eng_usdjpy.MAX_SPREAD_PCT << "% [ASIA/TOKYO-FIX]\n"
              << "[OMEGA-PARAMS] GBPUSD   TP=" << g_eng_gbpusd.TP_PCT << "% SL=" << g_eng_gbpusd.SL_PCT
              << "% vol=" << g_eng_gbpusd.VOL_THRESH_PCT << "% mom=" << g_eng_gbpusd.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_gbpusd.MIN_BREAKOUT_PCT << "% gap=" << g_eng_gbpusd.MIN_GAP_SEC
              << "s spread=" << g_eng_gbpusd.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] EURUSD   TP=" << g_eng_eurusd.TP_PCT << "% SL=" << g_eng_eurusd.SL_PCT
              << "% vol=" << g_eng_eurusd.VOL_THRESH_PCT << "% mom=" << g_eng_eurusd.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_eurusd.MIN_BREAKOUT_PCT << "% gap=" << g_eng_eurusd.MIN_GAP_SEC
              << "s spread=" << g_eng_eurusd.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] GER40    TP=" << g_eng_ger30.TP_PCT  << "% SL=" << g_eng_ger30.SL_PCT
              << "% vol=" << g_eng_ger30.VOL_THRESH_PCT << "% mom=" << g_eng_ger30.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_ger30.MIN_BREAKOUT_PCT << "% gap=" << g_eng_ger30.MIN_GAP_SEC
              << "s hold=" << g_eng_ger30.MAX_HOLD_SEC << "s spread=" << g_eng_ger30.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] UK100    TP=" << g_eng_uk100.TP_PCT  << "% SL=" << g_eng_uk100.SL_PCT
              << "% vol=" << g_eng_uk100.VOL_THRESH_PCT << "% mom=" << g_eng_uk100.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_uk100.MIN_BREAKOUT_PCT << "% gap=" << g_eng_uk100.MIN_GAP_SEC
              << "s hold=" << g_eng_uk100.MAX_HOLD_SEC << "s spread=" << g_eng_uk100.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] ESTX50   TP=" << g_eng_estx50.TP_PCT << "% SL=" << g_eng_estx50.SL_PCT
              << "% vol=" << g_eng_estx50.VOL_THRESH_PCT << "% mom=" << g_eng_estx50.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_estx50.MIN_BREAKOUT_PCT << "% minrange=" << g_eng_estx50.MIN_COMP_RANGE
              << " gap=" << g_eng_estx50.MIN_GAP_SEC
              << "s hold=" << g_eng_estx50.MAX_HOLD_SEC << "s spread=" << g_eng_estx50.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] UKBRENT  TP=" << g_eng_brent.TP_PCT  << "% SL=" << g_eng_brent.SL_PCT
              << "% vol=" << g_eng_brent.VOL_THRESH_PCT << "% mom=" << g_eng_brent.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_brent.MIN_BREAKOUT_PCT << "% gap=" << g_eng_brent.MIN_GAP_SEC
              << "s hold=" << g_eng_brent.MAX_HOLD_SEC << "s spread=" << g_eng_brent.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] XAUUSD   GoldEngineStack active | gap=" << g_cfg.gs_cfg.min_entry_gap_sec
              << "s hold=" << g_cfg.gs_cfg.max_hold_sec << "s vwap_min=" << g_cfg.gs_cfg.min_vwap_dislocation
              << " spread_max=" << g_cfg.gs_cfg.max_entry_spread << "\n"
              << "[OMEGA-PARAMS] GoldStack MIN_ENTRY_GAP=30s MAX_HOLD=600s REGIME_FLIP_MIN=60s\n"
              << "[OMEGA-PARAMS] LeadLag=ACTIVE(3-window-confirm) SpreadDisloc=managed-only EventComp=managed-only\n"
              << "[OMEGA-PARAMS] ???????????????????????????????????????????\n\n";
    std::cout.flush();

    if (g_cfg.mode == "SHADOW") {
        if (g_cfg.shadow_ustec_pilot_only) {
            g_eng_nq.ENTRY_SIZE = g_cfg.ustec_pilot_size;
            g_eng_nq.MIN_GAP_SEC = g_cfg.ustec_pilot_min_gap_sec;
            g_eng_nq.MAX_TRADES_PER_MIN = std::min(g_eng_nq.MAX_TRADES_PER_MIN, 2);
            std::cout << "[OMEGA-PILOT] USTEC shadow pilot enabled | size=" << g_eng_nq.ENTRY_SIZE
                      << " min_gap=" << g_eng_nq.MIN_GAP_SEC
                      << " max_trades_per_min=" << g_eng_nq.MAX_TRADES_PER_MIN << "\n";
        } else {
            std::cout << "[OMEGA-PILOT] Multi-symbol shadow enabled | all configured engines may trade\n";
        }
        std::cout << "[OMEGA-MODE] SHADOW -- exact live simulation (orders paper only, all risk gates active)\n";
    }

    build_id_map();

    // Open log file and tee stdout into it
    // Log file: stdout/stderr teed to file via RollingTeeBuffer
    // g_tee_buf wires std::cout -> file. Console output preserved.
    {
        const std::string log_dir = "C:\\Omega\\logs";
        CreateDirectoryA(log_dir.c_str(), nullptr);
        g_orig_cout = std::cout.rdbuf();
        g_tee_buf   = new RollingTeeBuffer(g_orig_cout, log_dir);
        if (!g_tee_buf->is_open()) {
            // Log failed -- print to stderr (not tee'd) and continue without log
            // Do NOT return 1 -- process must keep running
            fprintf(stderr, "[OMEGA-LOG-WARN] Cannot open log under %s -- continuing without log\n",
                    log_dir.c_str());
            delete g_tee_buf;
            g_tee_buf = nullptr;
        } else {
            std::cout.rdbuf(g_tee_buf);
            std::cerr.rdbuf(g_tee_buf);
            std::cout << "[OMEGA] Log: " << g_tee_buf->current_path() << "\n";
            // RUNNING COMMIT here so it goes into latest.log via the tee buffer.
            // Previously this was printed via std::printf before tee opened -- went to
            // NSSM stdout only, never to latest.log. Now it goes to both.
            std::cout << "[OMEGA] RUNNING COMMIT: " << OMEGA_VERSION << " built=" << OMEGA_BUILT << "\n";
            std::cout.flush();
        }
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
            "latency_ms,regime,exit_reason";

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
        g_ctrader_depth.atomic_l2_write_fn = [](const std::string& sym, double imb, double mp, bool hd) noexcept {
            AtomicL2* al = get_atomic_l2(sym);
            if (!al) return;
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            al->imbalance.store(imb, std::memory_order_relaxed);
            al->microprice_bias.store(mp, std::memory_order_relaxed);
            al->has_data.store(hd, std::memory_order_relaxed);
            al->last_update_ms.store(now_ms, std::memory_order_release);  // release: visible after imbalance/has_data
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
        g_ctrader_depth.symbol_whitelist.insert("NGAS");
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
        g_ctrader_depth.name_alias["NGAS"]     = "NGAS.F";
        g_ctrader_depth.name_alias["NATGAS"]   = "NGAS.F";
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
        // Write STARTING immediately so deploy script knows process launched
        { std::ofstream f(status_path); f << "STARTING\n"; }

        const auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&]{ return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count(); };

        auto write_status = [&](const std::string& s) {
            std::ofstream f(status_path);
            f << s << "\n";
            std::cout << "[STARTUP-CHECK] " << s << "\n";
            std::cout.flush();
        };

        // --- Check 1: cTrader depth connects within 60s ---
        // cTrader waits 30s before first connect attempt, then 5-15s to auth
        while (elapsed() < 65) {
            if (g_ctrader_depth.depth_active.load()) break;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        if (!g_ctrader_depth.depth_active.load()) {
            write_status("FAIL: cTrader depth not connected after 65s -- check token/network");
            return;
        }
        std::cout << "[STARTUP-CHECK] cTrader depth connected (" << elapsed() << "s)\n";
        std::cout.flush();

        // --- Check 2: L2 depth events flowing for XAUUSD ---
        // Check that cTrader is actually sending depth events (any side, not necessarily both).
        // has_data() requires BOTH bid AND ask sides simultaneously -- too strict at startup
        // Check FIX book (g_l2_books) which has real bid+ask from the FIX W/X feed.
        // cTrader atomic (g_l2_gold.imbalance) is ask-only from BlackBull depth.
        const auto t1 = std::chrono::steady_clock::now();
        bool l2_ok = false;
        while (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t1).count() < 30) {
            const double imb = g_l2_gold.imbalance.load(std::memory_order_relaxed);
            const bool   hd  = g_l2_gold.has_data.load(std::memory_order_relaxed);
            if (hd || imb < 0.499 || imb > 0.501) { l2_ok = true; break; }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!l2_ok) {
            const bool events_flowing = g_ctrader_depth.depth_events_total.load() > 10;
            if (events_flowing) {
                write_status("OK: cTrader live, L2 events flowing (book still filling)");
                return;
            }
            write_status("FAIL: Gold L2 data not flowing after 30s -- no depth events received");
            return;
        }
        std::cout << "[STARTUP-CHECK] Gold L2 live (" << elapsed() << "s)\n";
        std::cout.flush();

        // --- All checks passed ---
        const double imb = g_l2_gold.imbalance.load(std::memory_order_relaxed);
        write_status("OK: cTrader live, L2 live (imb=" + std::to_string(imb).substr(0,5) + ")");
    }).detach();

    // =========================================================================
    // L2 WATCHDOG THREAD
    // =========================================================================
    // cTrader L2 feed (ctid=43014358) is the BASIS of all GoldFlow engine
    // functionality. Without L2 imbalance the engine degrades to drift-only
    // mode which has no proven edge (backtest: 63% WR, negative P&L).
    //
    // This watchdog:
    //   1. Monitors L2 liveness every 30s (imbalance != 0.500 within 5s)
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

            // L2 is alive if: imbalance != 0.500 within last 5s
            const bool l2_alive = g_l2_gold.fresh(now_ms, 5000)
                && (std::fabs(g_l2_gold.imbalance.load(std::memory_order_relaxed) - 0.5) > 0.001);

            if (l2_alive) {
                // L2 is flowing
                if (!was_alive) {
                    // Just recovered
                    const double imb = g_l2_gold.imbalance.load(std::memory_order_relaxed);
                    printf("[L2-WATCHDOG] ALIVE -- L2 depth flowing from ctid=43014358 imb=%.3f\n", imb);
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

                    printf("[L2-WATCHDOG] *** DEAD *** L2 depth from ctid=43014358 not flowing for %llds\n"
                           "[L2-WATCHDOG] GoldFlow engine GATED -- drift-only mode has no proven edge\n"
                           "[L2-WATCHDOG] ACTION REQUIRED: check cTrader connection / ctid=43014358\n",
                           (long long)(dead_ms / 1000));
                    fflush(stdout);

                    if (!alert_written) {
                        // Write alert file -- readable by monitoring script / deploy tools
                        std::ofstream f(alert_path);
                        f << "L2_DEAD\n"
                          << "ctid=43014358 not delivering depth events\n"
                          << "dead_seconds=" << (dead_ms / 1000) << "\n"
                          << "action=check cTrader Open API connection and account depth permissions\n"
                          << "immutable=ctid_trader_account_id=43014358 is the ONLY account with L2\n";
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
            // L2 is live when: depth_active=true AND imbalance != 0.500
            const bool depth_up  = g_ctrader_depth.depth_active.load();
            const double imb     = g_l2_gold.imbalance.load(std::memory_order_relaxed);
            const bool l2_flowing = depth_up && (std::fabs(imb - 0.5) > 0.001
                                    || g_ctrader_depth.depth_events_total.load() > 2);
            if (l2_flowing) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - ct_wait_start).count();
                printf("[STARTUP] cTrader L2 live after %llds (imb=%.3f) -- starting FIX\n",
                       (long long)elapsed, imb);
                fflush(stdout);
                ct_ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!ct_ready) {
            printf("[STARTUP] cTrader L2 not live after 45s -- starting FIX in degraded mode\n");
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
    g_adaptive_risk.save_perf(log_root_dir() + "/kelly");
    g_gold_flow.save_atr_state(log_root_dir() + "/gold_flow_atr.dat");
    g_gold_stack.save_atr_state(log_root_dir() + "/gold_stack_state.dat");
    g_trend_pb_gold.save_state(log_root_dir()  + "/trend_pb_gold.dat");

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
    g_trend_pb_ger40.save_state(log_root_dir() + "/trend_pb_ger40.dat");
    g_trend_pb_nq.save_state(log_root_dir()    + "/trend_pb_nq.dat");
    g_trend_pb_sp.save_state(log_root_dir()    + "/trend_pb_sp.dat");
    g_adaptive_risk.print_summary();
    if (g_tee_buf)   { g_tee_buf->flush_and_close(); std::cout.rdbuf(g_orig_cout); delete g_tee_buf; g_tee_buf = nullptr; }
    WSACleanup();
    ReleaseMutex(g_singleton_mutex);
    CloseHandle(g_singleton_mutex);
    g_shutdown_done.store(true);  // unblock console_ctrl_handler -- process may now exit
    return 0;
}


