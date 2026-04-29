// tick_gold.hpp -- per-symbol tick handlers
// Extracted from on_tick(). Same translation unit -- all static functions visible.

// Session 9: GoldCoordinator wiring -- allow-by-default skeleton. The coordinator
// tracks per-lane position counts so later sessions can enforce budget limits
// without another refactor. Engine behaviour is UNCHANGED in Session 9.
#include "gold_coordinator.hpp"

// -- XAUUSD -------------------------------------------------
static void on_tick_gold(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime,
        double rtt_check)
{
    // ?? Gold master exclusion gate ????????????????????????????????????????
    // Default: ANY open gold position blocks new entries (1-at-a-time invariant).
    // TREND DAY exception: when |ewm_drift| > 5.0 AND vol_ratio > 1.5,
    //   ? GoldStack counter-entry allowed within 60s (bypasses SL cooldown).
    // on a winner, allow GoldStack to add a position in the same direction.
    // ATR-proportional gate variables -- computed once at function entry,
    // used by dead-zone lambda, crash_impulse_bypass, asia_crash_bypass,
    // counter-trend gate, GoldStack MR block, and lot scaling below.
    // Must be defined before any lambda that captures them by reference.
    // gf_atr_gate: ATR-based gate used by rsi_atr_scale, drift_bypass_thresh, gs_atr_scale,
    // g_gold_stack which tracks vol_range as an ATR proxy.
    const double gf_atr_gate       = std::max(2.0, g_gold_stack.vol_range());
    const double rsi_atr_scale     = std::min(3.0, std::max(0.5, gf_atr_gate / 5.0));
    const double rsi_crash_lo      = 50.0 - 6.0 * rsi_atr_scale;
    const double rsi_crash_hi      = 50.0 + 6.0 * rsi_atr_scale;
    const double drift_bypass_thresh = std::max(1.0, 0.3 * gf_atr_gate);

    const double gf_mid = (bid + ask) * 0.5;
    // GoldStack winning: allow bracket pyramid when GoldStack has a live profitable position
    const bool gs_open = g_gold_stack.has_open_position();
    const bool gs_winning = gs_open && g_gold_stack.has_profitable_trail();
    const bool gold_any_open =
        (gs_open && !gs_winning)                ||  // GoldStack blocks unless profitable trail
        // g_le_stack.has_open_position() REMOVED at S13 Finding B 2026-04-24 — engine culled.
        g_bracket_gold.has_open_position()      ||
        g_trend_pb_gold.has_open_position()     ||
        g_nbm_gold_london.has_open_position()   ||  // London NBM also blocks other gold engines
        g_h1_swing_gold.has_open_position() ||      // H1 swing open blocks all other gold entries
        g_h4_regime_gold.has_open_position()    ||  // H4 regime open blocks all other gold entries
        g_candle_flow.has_open_position()       ||  // AUDIT 2026-04-29: CFE open blocks other gold engines (re-enabled)
        // (g_pullback_cont/prem has_open_position gates removed S49 X5 — engine culled)
        g_ema_cross.has_open_position()              ;  // ECE open blocks other gold engines

    // ?? Trend day detection ???????????????????????????????????????????????
    const double gold_ewm_drift_now = g_gold_stack.ewm_drift();
    const double gold_recent_vol_now = g_gold_stack.recent_vol_pct();
    const double gold_base_vol_now   = g_gold_stack.base_vol_pct();
    const double gold_vol_ratio_now  = (gold_base_vol_now > 0.0)
        ? gold_recent_vol_now / gold_base_vol_now : 0.0;
    // Strong trend: drift > $3 sustained + any vol expansion above baseline.
    // Lowered from drift>=5.0 + ratio>=1.5:
    //   $5 drift requires a very established trend -- misses the first legs.
    //   ratio>=1.5 fails on Sunday open when baseline_vol is not yet warmed.
    //   $3 drift is still unambiguous directional pressure (not chop).
    //   ratio>=1.0 = any vol expansion above baseline -- correct for gap opens.
    const bool gold_trend_day = (std::fabs(gold_ewm_drift_now) >= 3.0)
                                && (gold_vol_ratio_now >= 1.0 || gold_vol_ratio_now == 0.0);
    // Trend direction: +1 long trend, -1 short trend
    // gold_trend_dir: +1 long trend, -1 short trend (used for future per-engine bias filter)
    (void)(gold_trend_day ? (gold_ewm_drift_now > 0 ? 1 : -1) : 0);

    // ?? Gold regime helpers ??????????????????????????????????????????????
    const char* gold_stack_regime   = g_gold_stack.regime_name();
    {
        static bool    s_was_impulse     = false;
        // post-impulse block is permanently off; GoldStack + supervisor
        // handle regime transitions directly.
        (void)s_was_impulse;
    }
    const bool gold_post_impulse_block = false;

    // ?? Reversal window check ?????????????????????????????????????????????
    const int64_t now_s_gate = static_cast<int64_t>(std::time(nullptr));
    (void)now_s_gate;
    const bool drift_reversed = false;
    (void)drift_reversed;

    const bool gold_session_ok  = true;  // 24h -- no time-based blocks
    const bool gold_trail_blocked = false;
    // GoldStack direction-aware trail block -- only block SAME direction as last close
    // Opposite direction entries always allowed (reversal trades)
    // g_gold_trail_block_dir: +1=last close was LONG (block new LONGs), -1=SHORT (block new SHORTs)
    // gs_trail_blocked passed to stack_enter_effective below -- GoldStack checks signal direction internally
    const bool gs_trail_blocked = gold_trail_blocked; // direction check done inside GoldStack signal gate
    // Session transition noise blocks -- spread spikes at open/close
    // NY close (22:00-22:10 UTC) and Sydney open (00:00-00:15 UTC)
    const bool in_ny_close_noise = [&]() -> bool {
        struct tm ti_ny{}; const auto t_ny = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        gmtime_s(&ti_ny, &t_ny);
        const int mins_utc = ti_ny.tm_hour * 60 + ti_ny.tm_min;
        return (mins_utc >= 1320 && mins_utc < 1330)  // 22:00-22:10 UTC NY close
            || (mins_utc >= 0    && mins_utc < 5);    // 00:00-00:05 UTC Sydney open (was 15min -- too long)
    }();
    // ATR-proportional gate thresholds computed at function top (see above).
    const double rsi_for_gate   = g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed);
    const double drift_for_gate = g_gold_stack.ewm_drift();
    const bool crash_impulse_bypass = (rsi_for_gate > 0.0)
        && ((rsi_for_gate < rsi_crash_lo)
         || (rsi_for_gate > rsi_crash_hi)
         || (rsi_for_gate < rsi_crash_lo && drift_for_gate < -drift_bypass_thresh)
         || (rsi_for_gate > rsi_crash_hi && drift_for_gate >  drift_bypass_thresh));
    // L2 liveness gate -- gold DOM must be live before any entry is allowed.
    // gold_l2_real is set in on_tick.hpp when g_l2_gold.fresh(now, 3000) is true.
    // If L2 is dead: log once per 30s, block ALL gold entries until DOM recovers.
    // This prevents blind entries on 0.500 synthetic imbalance values.
    // gold_l2_gate REMOVED from gold_can_enter:
    // gold_l2_real (depth event freshness) was blocking ALL gold engines -- bracket,
    // stack, MacroCrash, RSIReversal, HybridBracket, TrendPullback --
    // whenever a depth event was >N ms old. Those engines do not need live DOM.
    const bool gold_can_enter = gold_session_ok && symbol_gate("XAUUSD", gold_any_open, "", tradeable, lat_ok, regime, bid, ask)
                             && (!gold_post_impulse_block || crash_impulse_bypass);
    const bool gold_can_enter_trend_reentry = false;
    (void)gold_can_enter_trend_reentry;

    // Run supervisor -- gold has its own GoldStack (not a BreakoutEngine),
    // so we use a dedicated gold BreakoutEngine-based vol state if available.
    // For gold we track phase/vol from the bracket engine's internal data.
    // (Historical note: earlier versions used the silver engine as a vol/phase
    //  proxy; silver trading removed in Scope B migration, Sessions 10/11.)
    int fb_gold = 0;
    { std::lock_guard<std::mutex> lk(g_false_break_mtx);
      auto it = g_false_break_counts.find("XAUUSD"); if (it != g_false_break_counts.end()) fb_gold = it->second; }
    // ?? Gold supervisor -- REAL measured volatility (no synthetic injection) ??
    //
    // REMOVED: synthetic vol mapping (IMPULSE?0.25%, COMPRESSION?0.04%, etc.)
    // WHY REMOVED: that mapping bypassed the supervisor's vol_ratio check entirely.
    //   IMPULSE ? hardcoded recent=0.25, base=0.10 ? vol_ratio=2.5 ? allow_breakout=true
    //   regardless of whether real volatility was expanding. This caused the bad
    //   SESSION_MOM_SHORT trade: IMPULSE label alone permitted entry during a
    //   liquidity sweep with no real vol expansion.
    //
    // REAL DATA SOURCES (all computed from live tick prices inside GoldEngineStack):
    //   recent_vol_pct(): governor 80-tick MinMax range / mid * 100
    //                     Updates every tick. Zero until 80-tick warmup.
    //   base_vol_pct():   512-tick rolling range + EWM ?=0.002 baseline.
    //                     Doesn't chase trends -- stays elevated during/after moves.
    //                     Zero until 40-tick warmup.
    //   in_compression:   governor range < CE ($4.00) confirmed over CONFIRM_TICKS=5.
    //   vol_ratio:        recent / base -- computed inside SymbolSupervisor.update()
    //                     > 0.85 ? QUIET_COMPRESSION
    //                     > 1.0  ? expansion candidate
    //                     > 1.3  ? EXPANSION_BREAKOUT or TREND_CONTINUATION
    //
    // CONFIDENCE ? POSITION SIZING:
    //   gold_sdec.confidence is now passed to compute_size() as a multiplier.
    //   confidence 0.45-0.55 ? 0.7? size. 0.80+ ? 1.2? size.
    //   Weak signals automatically get smaller positions.
    //
    // COUNTER-TREND GUARD FOR SESSIONMOMENTUM:
    //   SessionMomentum SHORT blocked when price > VWAP (price above mean = no short).
    //   SessionMomentum LONG blocked when price < VWAP.
    //   IMPULSE regime alone no longer permits counter-trend entries.
    const double gold_gov_hi        = g_gold_stack.governor_hi();
    const double gold_gov_lo        = g_gold_stack.governor_lo();
    const double gold_mid_now       = (bid + ask) * 0.5;
    g_macro_ctx.gold_mid_price      = gold_mid_now;  // needed for wall detection
    const double gold_vwap_now      = g_gold_stack.vwap();
    const double gold_momentum      = (gold_vwap_now > 0.0 && gold_mid_now > 0.0)
        ? ((gold_mid_now - gold_vwap_now) / gold_mid_now * 100.0)
        : 0.0;
    // Positive = price above VWAP, negative = price below VWAP, in raw gold points.
    const double gold_vwap_pts = (gold_vwap_now > 0.0) ? (gold_mid_now - gold_vwap_now) : 0.0;
    (void)gold_vwap_pts;
    const bool gold_is_compressing  = (strcmp(gold_stack_regime,"COMPRESSION")==0);

    // -- PDH/PDL daily range tracker -------------------------------------------
    // Research (2026-04-15): 111M tick / 2yr backtest: inside daily range
    // EV=+1.732pts at 15min. Outside = negative EV. Gold is mean-reverting intraday.
    {
        const int64_t pdhl_day  = (int64_t)(nowSec() / 86400LL);
        static int64_t s_pdhl_day  = -1;
        static double  s_pdhl_hi   = 0.0;
        static double  s_pdhl_lo   = 1e9;
        static double  s_pdhl_prev_hi = 0.0;
        static double  s_pdhl_prev_lo = 0.0;
        if (pdhl_day != s_pdhl_day) {
            if (s_pdhl_day >= 0) {
                s_pdhl_prev_hi = s_pdhl_hi;
                s_pdhl_prev_lo = (s_pdhl_lo < 1e8) ? s_pdhl_lo : 0.0;
            }
            s_pdhl_hi  = gold_mid_now;
            s_pdhl_lo  = gold_mid_now;
            s_pdhl_day = pdhl_day;
        } else {
            if (gold_mid_now > s_pdhl_hi) s_pdhl_hi = gold_mid_now;
            if (gold_mid_now < s_pdhl_lo) s_pdhl_lo = gold_mid_now;
        }
        g_macro_ctx.pdh = s_pdhl_prev_hi;
        g_macro_ctx.pdl = s_pdhl_prev_lo;
    }
    const bool gold_inside_daily_range =
        (g_macro_ctx.pdh > 0.0 && g_macro_ctx.pdl > 0.0)
        ? (gold_mid_now <= g_macro_ctx.pdh + 2.0 && gold_mid_now >= g_macro_ctx.pdl - 2.0)
        : true;
    (void)gold_inside_daily_range;

    // REAL vol measurements from GoldEngineStack (tick-by-tick, no hardcoding)
    const double gold_recent_vol    = g_gold_stack.recent_vol_pct();  // 80-tick range/mid
    const double gold_base_vol      = g_gold_stack.base_vol_pct();    // 512-tick EWM baseline

    const auto gold_sdec = g_sup_gold.update(
        bid, ask,
        gold_recent_vol, gold_base_vol,   // REAL measured vol -- no synthetic injection
        gold_momentum,
        gold_gov_hi, gold_gov_lo,
        gold_is_compressing,
        fb_gold);

    // ?? P4: Velocity re-entry exclusivity exception ??????????????????????
    // GoldStack + supervisor + MacroCrash provide direction re-entry paths now.
    // Block deliberately empty — do not re-introduce without new gating logic.

    // Diagnostic: log vol_ratio every 30s so we can verify real data is flowing
    {
        static int64_t s_last_vol_log = 0;
        const int64_t now_ms_vl = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_ms_vl - s_last_vol_log >= 30000) {
            s_last_vol_log = now_ms_vl;
            const double vr = (gold_base_vol > 0.0) ? gold_recent_vol / gold_base_vol : 0.0;
            {
                char _msg[512];
                snprintf(_msg, sizeof(_msg), "[GOLD-VOL] regime=%s recent_vol=%.4f%% base_vol=%.4f%% ratio=%.3f"                    " sdec_regime=%s conf=%.3f allow_bo=%d\n",                    gold_stack_regime, gold_recent_vol, gold_base_vol, vr,                    omega::regime_name(gold_sdec.regime), gold_sdec.confidence,                    (int)gold_sdec.allow_breakout);
                std::cout << _msg;
                std::cout.flush();
            }
        }
    }

    // GoldEngineStack: always tick for VWAP/vol warmup -- supervisor gating only blocks
    // entry execution, not feature accumulation. Without this, VWAP stays 0.00 forever
    // because the stack only runs when allow_breakout=1, but allow_breakout depends on
    // regime confidence which needs VWAP to be populated -- a deadlock.
    // Fix: always call on_tick, pass can_enter=false when supervisor blocks to prevent
    // actual signal execution while still building features.
    {
        // ?? Vol ratio confirmation ????????????????????????????????????????
        // Require genuine vol expansion before allowing GoldStack breakout entries.
        // Cold start (base_vol=0) is permitted -- the engine's VolatilityFilter
        // already blocks signals during its own 50-tick warmup.
        const double gold_vol_ratio = (gold_base_vol > 0.0)
            ? gold_recent_vol / gold_base_vol : 0.0;
        // Threshold lowered 1.10 ? 1.02: require only a minimal vol expansion
        // (2% above baseline). The EA comparison fires on any momentum candle;
        // 1.10 was blocking valid London/NY setups where vol barely expands.
        // The supervisor's own vol_ratio check and confidence gate still apply.
        const bool vol_expanding = (gold_base_vol <= 0.0) || (gold_vol_ratio >= 1.02);
        (void)vol_expanding;

        // ?? Confidence threshold ??????????????????????????????????????????
        const bool conf_ok = (gold_sdec.confidence >= 0.45);
        (void)conf_ok;

        // ?? Bar indicator context for GoldStack ???????????????????????????
        // FIX 2026-04-02: replaced M5 swing trend_state with M1 EMA9/EMA50 crossover.
        // M5 swing trend_state requires SWING_P=3 bars each side to confirm a pivot --
        // minimum 15+ min lag. During a live crash trend_state stays +1 from the prior
        // rally and blocks all shorts via the counter-trend gate.
        // EMA9/EMA50 on M1 reflects momentum within 1-3 bars. When EMA9 < EMA50 on
        // M1, the chart shows a clear downtrend -- visible to any human, invisible to
        // the engine until now. This is what was blocking every entry in today's crash.
        const bool bar_ready      = g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const bool bar_ema_live   = g_bars_gold.m1.ind.m1_ema_live.load(std::memory_order_relaxed);
        const double bar_rsi_gs   = bar_ready     ? g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed) : 50.0;
        const double bar_ema9_gs  = bar_ema_live  ? g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed) : 0.0;
        const double bar_ema50_gs = bar_ema_live  ? g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed) : 0.0;
        // EMA9 < EMA50 = momentum downtrend (-1). EMA9 > EMA50 = uptrend (+1). 0 = no signal.
        // Only read when m1_ema_live -- stale disk EMA can show wrong direction from prior session.
        const int    bar_trend_gs = (bar_ema9_gs > 0.0 && bar_ema50_gs > 0.0)
            ? (bar_ema9_gs < bar_ema50_gs ? -1 : +1)
            : 0;

        // ?? MR/Range engine path -- bypasses allow_breakout requirement ????????
        // Problem: supervisor requires EXPANSION_BREAKOUT or TREND_CONTINUATION for
        // allow_breakout=true. In QUIET_COMPRESSION and CHOP_REVERSAL (consolidation),
        // allow_breakout=false -> stack_can_enter=false -> GoldStack receives
        // can_enter=false -> ZERO signals regardless of what engines see internally.
        // This silences MeanReversion, DynamicRange, IntradaySeasonality, VWAPSnapback,
        // NR3Breakout, AsianRange, TwoBarReversal -- all engines DESIGNED for compression.
        // These engines are not breakout engines and do not need EXPANSION_BREAKOUT.
        //
        // Fix: allow GoldStack entry when:
        //   a) Supervisor regime is QUIET_COMPRESSION or CHOP_REVERSAL (range-bound tape)
        //   b) No open position in any gold engine (same exclusivity as breakout path)
        //   c) Session is tradeable (same session gate as breakout path)
        //   d) Supervisor confidence >= 0.30 (lower bar -- ranging tape has lower conf naturally)
        //   e) NOT in NY close noise window (same as other paths)
        //
        // The GoldStack's own RegimeGovernor + per-engine session gates + VolatilityFilter
        // + entry_quality_ok() score gate still apply internally -- this only lifts the
        // outer supervisor breakout requirement that was silencing all MR engines.
        const bool in_ranging_regime =
            (gold_sdec.regime == omega::Regime::QUIET_COMPRESSION) ||
            (gold_sdec.regime == omega::Regime::CHOP_REVERSAL);
        (void)in_ranging_regime;
        // closed via trail/BE on a strong trend. Stack handles the EWM drift
        // direction gate internally so only trend-aligned signals fire.
        // Also allow reversal counter-entry when drift_reversed is confirmed.
        // Trail block: 30s after same-direction close, check if this signal
        // would re-enter the same direction. Allow if direction differs (reversal).
        const bool gs_trail_dir_match = gs_trail_blocked;
        // g_feed_stale_xauusd is set by CTraderDepthClient starvation watchdog.
        // IntradaySeasonality fired twice into a dead feed (21:00-21:40 UTC 2026-04-13)
        // because GoldStack had no feed-liveness check. Now all GoldStack entries
        const bool gs_feed_ok = !g_feed_stale_xauusd.load(std::memory_order_relaxed);
        // GoldStack now does its own regime filtering internally.
        const bool stack_enter_effective = gs_feed_ok
            && gold_can_enter
            && !gs_trail_dir_match
            && !in_ny_close_noise;
        // Pass DX.F mid for DXYDivergenceEngine -- g_macroDetector.updateDXY()
    // is called every DX.F tick so this is fresh or 0.0 if feed not yet seen.
    const double dx_mid_now = g_macroDetector.dxyMid();
    // Inject M1 EMA9/EMA50 into VWAPStretchReversion before on_tick.
    // GoldSnapshot has no EMA fields so this must be wired here where M1 indicators are live.
    g_gold_stack.set_vwap_stretch_ema(bar_ema9_gs, bar_ema50_gs);
    const auto gsig = g_gold_stack.on_tick(bid, ask, rtt_check, handle_closed_trade,
                                            stack_enter_effective, dx_mid_now);
        if (gsig.valid) {
            // ?? Bar indicator confirmation for GoldStack ??????????????????
            bool gs_bar_blocked = false;
            if (bar_ready) {
                const bool gs_is_mr = (std::strcmp(gsig.engine, "MeanReversion")       == 0) ||
                                      (std::strcmp(gsig.engine, "VWAPStretchReversion") == 0) ||
                                      (std::strcmp(gsig.engine, "SessionMomentum")      == 0);
                if (gs_is_mr) {
                    // ATR-proportional MR bar block.
                    // Normal day (ATR=5pt): block LONG above RSI 65, SHORT below RSI 35.
                    // Crash day (ATR=10pt): block LONG above RSI 72, SHORT below RSI 28.
                    // This means on a quiet normal day mean-reversion entries aren't
                    // blocked just because RSI is 66 -- that's valid MR territory.
                    // On a crash day the threshold tightens so only extreme readings pass.
                    // Formula: gs_ob = 55 + 10 * clamp(atr/5, 1.0, 2.0)
                    //          gs_os = 45 - 10 * clamp(atr/5, 1.0, 2.0)
                    //   ATR=5pt:  gs_ob=65  gs_os=35
                    //   ATR=10pt: gs_ob=75  gs_os=25
                    const double gs_atr_scale = std::min(2.0, std::max(1.0, gf_atr_gate / 5.0));
                    const double gs_ob = 55.0 + 10.0 * gs_atr_scale;
                    const double gs_os = 45.0 - 10.0 * gs_atr_scale;
                    if (gsig.is_long  && (bar_rsi_gs > gs_ob || bar_trend_gs == -1)) gs_bar_blocked = true;
                    if (!gsig.is_long && (bar_rsi_gs < gs_os && bar_trend_gs == +1)) gs_bar_blocked = true;
                } else {
                    if (gsig.is_long  && bar_rsi_gs > 78.0) gs_bar_blocked = true;
                    if (!gsig.is_long && bar_rsi_gs < 22.0) gs_bar_blocked = true;
                }
                if (gs_bar_blocked) {
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[GS-BAR-BLOCK] XAUUSD %s %s blocked RSI=%.1f trend=%+d\n",                            gsig.is_long ? "LONG" : "SHORT", gsig.engine,                            bar_rsi_gs, bar_trend_gs);
                        std::cout << _msg;
                        std::cout.flush();
                    }
                }
            }
            if (!gs_bar_blocked) {
            // ?? VWAP counter-trend guard ??????????????????????????????????
            // Only block MEAN-REVERSION engines trading against VWAP trend.
            // NR3Breakout, TrendPullback etc.) SHOULD enter above VWAP on longs
            // and below VWAP on shorts -- that IS the trend.
            // Only block if this is a mean-reversion signal fading into the trend.
            const bool is_mean_rev_engine =
                (std::strcmp(gsig.engine, "MeanReversion")       == 0) ||
                (std::strcmp(gsig.engine, "VWAPStretchReversion") == 0) ||
                (std::strcmp(gsig.engine, "SessionMomentum")      == 0);
            const bool vwap_valid    = (gold_vwap_now > 0.0) && is_mean_rev_engine;
            const bool direction_ok  = !vwap_valid
                || (gsig.is_long  && gold_mid_now <= gold_vwap_now * 1.001)
                || (!gsig.is_long && gold_mid_now >= gold_vwap_now * 0.999);

            // ?? Covariance trend-day gate for mean-reversion engines ??????????
            // If XAUUSD and XAGUSD are moving together (|corr| > 0.70), this is
            // a macro trend day -- block fade engines to avoid selling into a move.
            // trend_day_corr() uses the live EWM correlation matrix which is fed
            // XAGUSD ticks every tick (on_tick.hpp line 265, before is_active_sym gate).
            const bool trend_day_corr_block =
                is_mean_rev_engine && g_corr_matrix.trend_day_corr("XAUUSD");

            // ?? CVD divergence block for GoldStack ????????????????????????????????
            // Block LONG when CVD bearish divergence: price made new high but CVD
            // did NOT -- distribution signal, institutional selling into strength.
            // Block SHORT when CVD bullish divergence: price made new low but CVD
            // did NOT -- absorption signal, institutional buying into weakness.
            // Applies to ALL GoldStack engines -- CVD divergence means execution
            // flow opposes signal direction regardless of strategy type.
            const bool cvd_gs_blocked =
                ( gsig.is_long && g_macro_ctx.gold_cvd_bear_div) ||
                (!gsig.is_long && g_macro_ctx.gold_cvd_bull_div);

            // ?? PDH/PDL structural block for GoldStack ??????????????????????
            // Block LONG when price is approaching PDH from below (within $1.50)
            // but has not confirmed a breakout ($3+ above PDH).
            // Block SHORT when price is approaching PDL from above (within $1.50)
            // but has not confirmed a breakdown ($3+ below PDL).
            // Breakout confirmed at $3+ past level -- allow entry.
            const omega::edges::DayLevels gs_pdl = g_edges.prev_day.previous("XAUUSD");
            bool gs_pdh_block = false;
            bool gs_pdl_block = false;
            if (gs_pdl.valid()) {
                if (gsig.is_long) {
                    const bool below_pdh = gold_mid_now <  gs_pdl.high + 3.00;
                    const bool near_pdh  = gold_mid_now >= gs_pdl.high - 1.50;
                    gs_pdh_block = near_pdh && below_pdh;
                } else {
                    const bool above_pdl = gold_mid_now >  gs_pdl.low - 3.00;
                    const bool near_pdl  = gold_mid_now <= gs_pdl.low + 1.50;
                    gs_pdl_block = near_pdl && above_pdl;
                }
            }
            const bool pdlevel_gs_blocked = gs_pdh_block || gs_pdl_block;

            if (!direction_ok || trend_day_corr_block || cvd_gs_blocked || pdlevel_gs_blocked) {
                if (cvd_gs_blocked)
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[GS-CVD-BLOCK] XAUUSD %s %s cvd_bear=%d cvd_bull=%d\n",                            gsig.is_long ? "LONG" : "SHORT", gsig.engine,                            (int)g_macro_ctx.gold_cvd_bear_div,                            (int)g_macro_ctx.gold_cvd_bull_div);
                        std::cout << _msg;
                        std::cout.flush();
                    }
                if (pdlevel_gs_blocked)
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[GS-PDL-BLOCK] XAUUSD %s %s mid=%.2f pdh=%.2f pdl=%.2f\n",                            gsig.is_long ? "LONG" : "SHORT", gsig.engine,                            gold_mid_now, gs_pdl.high, gs_pdl.low);
                        std::cout << _msg;
                        std::cout.flush();
                    }
                if (!cvd_gs_blocked && !pdlevel_gs_blocked)
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[GOLD-STACK-BLOCKED] %s counter-trend mid=%.2f vwap=%.2f"                            " vol_ratio=%.3f eng=%s trend_day_corr=%d\n",                            gsig.is_long ? "LONG" : "SHORT",                            gold_mid_now, gold_vwap_now, gold_vol_ratio, gsig.engine,                            (int)trend_day_corr_block);
                        std::cout << _msg;
                        std::cout.flush();
                    }
            } else {
                g_telemetry.UpdateLastSignal("XAUUSD",
                    gsig.is_long ? "LONG" : "SHORT", gsig.entry, gsig.reason,
                    omega::regime_name(gold_sdec.regime), regime.c_str(), "BREAKOUT",
                    gsig.entry + (gsig.is_long ? 1.0 : -1.0) * gsig.tp_ticks * 0.10,
                    gsig.entry - (gsig.is_long ? 1.0 : -1.0) * gsig.sl_ticks * 0.10);
                const double gold_sl_abs_raw = gsig.sl_ticks * 0.10;
                // ATR-normalised SL floor for gold -- same logic as other engines
                const double gold_sl_abs = g_adaptive_risk.vol_scaler.atr_sl_floor(
                    "XAUUSD", gold_sl_abs_raw);

                // ?? Confidence-scaled sizing ??????????????????????????????
                // Scale by supervisor confidence: conf=0.45?0.65?, conf=0.80?1.08?
                // Formula: clamp(0.40 + conf*0.85, 0.65, 1.25)
                double conf_mult = std::max(0.65, std::min(1.25,
                                            0.40 + gold_sdec.confidence * 0.85));
                // ?? IntradaySeasonality t-stat lot scaling ???????????????
                // gsig.confidence encodes t-stat/10 for seasonality signals.
                // t=24.2 (bucket 43) -> confidence=2.42 -> mult=1.50x (cap)
                // t=5.1  (bucket  9) -> confidence=0.51 -> mult=1.00x (floor)
                // Other engines: confidence is in 0..1.5 range, formula unchanged.
                if (std::strcmp(gsig.engine, "IntradaySeasonality") == 0
                    && gsig.confidence > 1.5) {
                    // t-stat encoded as confidence/10 -- scale 0.51..2.42 -> 1.0..1.5x
                    const double tstat = gsig.confidence;  // already t/10
                    const double seas_mult = std::max(1.0, std::min(1.5,
                        1.0 + (tstat - 0.51) / (2.42 - 0.51) * 0.5));
                    conf_mult = std::max(conf_mult, seas_mult);
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[SEAS-SIZE] bucket t=%.1f -> lot_mult=%.2fx\n",                            tstat * 10.0, seas_mult);
                        std::cout << _msg;
                        std::cout.flush();
                    }
                }
                // ?? RSI conviction mult: SHORT in momentum regime with RSI<20 ??
                // is_momentum_regime() = ADX>=25 AND ATR expanding.
                // In that regime RSI<20 = continuation short not reversal.
                // Scale conviction [1.00..1.20] by ADX strength (25->40 linear).
                // Only applies to SHORT entries; returns 1.0 for LONG or non-momentum.
                conf_mult *= g_vol_targeter.rsi_conviction_mult(
                    g_bars_gold.m1.ind, !gsig.is_long);

                const double base_lot  = compute_size("XAUUSD", gold_sl_abs, ask - bid,
                                                      gsig.size > 0.0 ? gsig.size : 0.02);
                // ?? Regime weight: boost gold in RISK_OFF, reduce in RISK_ON ??
                const float regime_wt  = g_regime_adaptor.weight(
                    omega::regime::EngineClass::GOLD_STACK);
                // ?? Vol-regime multiplier: scale lot by inverse volatility ??
                // gold_vol_ratio_now = recent_vol / base_vol (30-bar / 300-bar).
                // High vol = overheated tape = reduce size to protect capital.
                // RiskMetrics EWMA vol targeting: VOL_TARGET(20%) / ewma_vol_20
                // Replaces coarse bucket ladder -- continuous, mathematically principled.
                // Pre-computed and clamped [0.25, 1.50] by OHLCBarEngine._update_ewma_vol().
                // Falls back to 1.0 when EWMA not yet initialised (ewma_vol_20 == 0).
                const double vol_regime_mult = g_bars_gold.m1.ind.vol_target_mult.load(
                    std::memory_order_relaxed);
                // ?? Adaptive risk: DD throttle + Kelly on gold ?????????????
                double gold_daily_loss = 0.0; int gold_consec = 0;
                {
                    std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
                    auto it = g_sym_risk.find("XAUUSD");
                    if (it != g_sym_risk.end()) {
                        gold_daily_loss = std::max(0.0, -it->second.daily_pnl);
                        gold_consec     = it->second.consec_losses;
                    }
                }
                const double gold_adaptive = g_adaptive_risk.adjusted_lot(
                    "XAUUSD", base_lot * conf_mult * static_cast<double>(regime_wt) * vol_regime_mult,
                    gold_daily_loss, g_cfg.daily_loss_limit, gold_consec);
                // Re-clamp to max_lot_gold: adjusted_lot applies Kelly which can
                // multiply past the compute_size cap. Cap must be the final word.
                double gold_lot = std::max(0.01,
                    std::min(gold_adaptive, g_cfg.max_lot_gold));
                // ?? Cross-asset size filters for GoldStack ????????????????
                {
                    const bool gs_is_long = gsig.is_long;
                    // FIX 2026-04-22: HTF regime HARD-BLOCK before any sizing logic.
                    // Counter-regime trades have negative expectancy on clean trend
                    // days (today: 16 LONGs in 101pt BEARISH gold = -$49.01).
                    // bias() requires daily+intraday agreement (2/2 rule) so chop
                    // days return NEUTRAL and never trigger this block.
                    if (g_htf_filter.bias_opposes("XAUUSD", gs_is_long)) {
                        static thread_local int64_t s_gs_blk_log = 0;
                        const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        if (now_s - s_gs_blk_log > 30) {
                            s_gs_blk_log = now_s;
                            char _msg[512];
                            snprintf(_msg, sizeof(_msg),
                                "[HTF-BLOCK] XAUUSD STACK %s skipped -- HTF bias=%s opposes\n",
                                gs_is_long?"LONG":"SHORT",
                                g_htf_filter.bias_name("XAUUSD"));
                            std::cout << _msg; std::cout.flush();
                        }
                        gold_lot = 0.0;  // sentinel: downstream guard skips trade
                    } else {
                        double gs_xa_mult = 1.0;
                        // DXY momentum
                        const double dxy_ret = g_macroDetector.dxyReturn();
                        const double dxy_thr = g_macroDetector.DXY_RISK_OFF_PCT / 100.0;
                        if (gs_is_long  && dxy_ret >  dxy_thr) gs_xa_mult *= 0.70;
                        if (!gs_is_long && dxy_ret < -dxy_thr) gs_xa_mult *= 0.70;
                        // HTF bias soft scale (now no-op since size_scale returns 1.0)
                        gs_xa_mult *= g_htf_filter.size_scale("XAUUSD", gs_is_long);
                        // Macro: don't short gold in RISK_OFF
                        if (g_macro_ctx.regime == "RISK_OFF" && !gs_is_long) gs_xa_mult *= 0.60;
                        gs_xa_mult = std::max(0.30, std::min(1.20, gs_xa_mult));
                        if (gs_xa_mult != 1.0)
                            {
                                char _msg[512];
                                snprintf(_msg, sizeof(_msg), "[XA-FILTER] XAUUSD STACK %s mult=%.2f DXY=%.4f HTF=%s macro=%s\n",                                gs_is_long?"LONG":"SHORT", gs_xa_mult, dxy_ret,                                g_htf_filter.bias_name("XAUUSD"), g_macro_ctx.regime.c_str());
                                std::cout << _msg;
                                std::cout.flush();
                            }
                        gold_lot = std::max(0.01, std::min(gold_lot * gs_xa_mult, g_cfg.max_lot_gold));
                    }
                }

                // Max loss per trade dollar cap (gold stack bypasses enter_directional)
                if (g_cfg.max_loss_per_trade_usd > 0.0 && gold_sl_abs > 0.0) {
                    const double max_loss_lot = g_cfg.max_loss_per_trade_usd / (gold_sl_abs * 100.0);
                    if (gold_lot > max_loss_lot) {
                        const double capped = std::max(0.01, std::floor(max_loss_lot * 100.0 + 0.5) / 100.0);
                        {
                            char _msg[512];
                            snprintf(_msg, sizeof(_msg), "[MAX-LOSS-CAP] XAUUSD lot capped %.4f?%.4f (sl=$%.2f max=$%.0f)\n",                                gold_lot, capped, gold_sl_abs * 100.0 * gold_lot, g_cfg.max_loss_per_trade_usd);
                            std::cout << _msg;
                            std::cout.flush();
                        }
                        gold_lot = capped;
                    }
                }
                // FIX 2026-04-22: HTF block sentinel check -- gold_lot=0 from upstream
                if (gold_lot < 0.005) {
                    g_telemetry.IncrCostBlocked();  // count as a blocked entry for telemetry
                } else {
                g_gold_stack.patch_position_size(gold_lot);
                std::cout << "\033[1;" << (gsig.is_long ? "32" : "31") << "m"
                          << "[GOLD-STACK-ENTRY] " << (gsig.is_long ? "LONG" : "SHORT")
                          << " entry=" << gsig.entry
                          << " tp="    << gsig.tp_ticks << "ticks"
                          << " sl="    << gsig.sl_ticks << "ticks / "
                          << std::fixed << std::setprecision(2) << gold_sl_abs << "pts"
                          << " size="  << gold_lot
                          << " conf="  << gsig.confidence
                          << " conf_mult=" << conf_mult
                          << " regime_wt=" << regime_wt
                          << " vol_ratio=" << gold_vol_ratio
                          << " eng="   << gsig.engine
                          << " sup_regime=" << omega::regime_name(gold_sdec.regime)
                          << " winner=" << gold_sdec.winner
                          << "\033[0m\n";
                // Cost guard: ensure gold TP covers spread + commission + slippage
                {
                    const double gold_tp_dist = gsig.tp_ticks * 0.10;  // ticks ? pts (gold tick = $0.10)
                    if (!ExecutionCostGuard::is_viable("XAUUSD", ask - bid, gold_tp_dist, gold_lot)) {
                        g_telemetry.IncrCostBlocked();
                    } else {
                        // ── Cross-engine dedup (S20 2026-04-25) ────────────────────────
                        // Mirror of the generic on_tick.hpp gate at line ~1653. Blocks
                        // another XAUUSD engine from entering within CROSS_ENG_DEDUP_SEC
                        // of a prior XAUUSD entry. Prevents pairs like
                        // DXYDivergence@15:45:11 + XAUUSD_BRACKET@15:45:17 running
                        // concurrent same-side trades. Also registers the new entry so
                        // BracketGold's dedup check will see it.
                        {
                            std::lock_guard<std::mutex> _lk(g_dedup_mtx);
                            auto _it = g_last_cross_entry.find("XAUUSD");
                            if (_it != g_last_cross_entry.end() &&
                                (nowSec() - _it->second) < CROSS_ENG_DEDUP_SEC) {
                                char _msg[256];
                                snprintf(_msg, sizeof(_msg),
                                    "[CROSS-DEDUP] XAUUSD %s blocked -- another engine entered %lds ago (dedup=%lds)\n",
                                    gsig.engine,
                                    static_cast<long>(nowSec() - _it->second),
                                    static_cast<long>(CROSS_ENG_DEDUP_SEC));
                                std::cout << _msg;
                                std::cout.flush();
                                g_telemetry.IncrCostBlocked();
                                return;
                            }
                            g_last_cross_entry["XAUUSD"] = nowSec();
                        }
                        // Log entry
                        write_trade_open_log("XAUUSD", gsig.engine,
                            gsig.is_long ? "LONG" : "SHORT",
                            gsig.entry,
                            gsig.entry + (gsig.is_long ? 1.0 : -1.0) * gold_tp_dist,
                            gsig.entry - (gsig.is_long ? 1.0 : -1.0) * gold_sl_abs,
                            gold_lot, ask - bid, gold_stack_regime, gsig.reason);
                        // Arm partial exit for gold stack entries
                        g_partial_exit.arm("XAUUSD", gsig.is_long, gsig.entry,
                                           gsig.entry + (gsig.is_long ? 1.0 : -1.0) * gold_tp_dist,
                                           gsig.entry - (gsig.is_long ? 1.0 : -1.0) * gold_sl_abs,
                                           gold_lot,
                                           g_adaptive_risk.vol_scaler.atr_fast("XAUUSD"));
                        portfolio_sl_risk_add(gold_sl_abs, gold_lot, 100.0);  // gold: 100 USD/pt/lot
                        send_live_order("XAUUSD", gsig.is_long, gold_lot, gsig.entry);
                        g_telemetry.UpdateLastEntryTs();  // watchdog: GoldStack entry counts as activity
                    }
                }
                }  // FIX 2026-04-22: close HTF-block else
            }
            } // end !gs_bar_blocked
        }
    }

    // BracketEngine: NOT gated by supervisor allow_bracket.
    // The supervisor was designed for breakout engines (vol_ratio?regime?allow).
    // For gold bracket, that chain fails during cascades:
    //   MEAN_REVERSION ? vol_ratio=1.0 ? HIGH_RISK_NO_TRADE ? allow_bracket=False
    //   TREND/IMPULSE  ? bracket_score was 0.05 ? blocked
    // The BracketEngine has its OWN structural compression detector (30-tick lookback,
    // MIN_RANGE=$2.80). That is the correct gate for the bracket.
    // We only apply: gold_can_enter (risk/session/latency), freq_ok, asia_trend_ok,
    // no-double-position, and spread gate from supervisor.
    // Only block bracket if spread is too wide (supervisor's spread gate is still valid)
    const bool gold_spread_ok = !(gold_sdec.regime == omega::Regime::HIGH_RISK_NO_TRADE
        && gold_sdec.reason != nullptr && std::string(gold_sdec.reason) == "spread_too_wide");
    // now_ms_g declared here so both the bracket block and flow engine block can use it
    const int64_t now_ms_g = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // ?? Bar metric updates -- every XAUUSD tick ????????????????????????????
    g_bars_gold.m1.update_tick_metrics(ask - bid, now_ms_g);

    g_bars_gold.m1.update_volume_delta(g_macro_ctx.gold_l2_imbalance);

    // ?? FIX-tick bar builder -- accumulates ticks into M1/M5/M15/H4 OHLC bars ??
    // xau_mid hoisted out of block so VPIN + corr-matrix can reference it below
    const double xau_mid = (bid + ask) * 0.5;
    {
        static OHLCBar s_cur1{}, s_cur5{}, s_cur15{}, s_cur_h1{}, s_cur_h4{};
        static int64_t s_bar1_ms = 0, s_bar5_ms = 0, s_bar15_ms = 0, s_bar_h1_ms = 0, s_bar_h4_ms = 0;
        const int64_t b1  = (now_ms_g /   60000LL) *   60000LL;
        const int64_t b5  = (now_ms_g /  300000LL) *  300000LL;
        const int64_t b15 = (now_ms_g /  900000LL) *  900000LL;
        const int64_t bh1 = (now_ms_g /  3600000LL) * 3600000LL;   // 1h = 3600s
        const int64_t bh4 = (now_ms_g / 14400000LL) * 14400000LL;  // 4h = 14400s
        // M1
        if (s_bar1_ms == 0) { s_cur1 = {b1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar1_ms = b1; }
        else if (b1 != s_bar1_ms) { g_bars_gold.m1.add_bar(s_cur1); g_ema_cross.on_bar(s_cur1.close, g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed), g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed), b1); s_cur1 = {b1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar1_ms = b1; }
        else { if(xau_mid>s_cur1.high)s_cur1.high=xau_mid; if(xau_mid<s_cur1.low)s_cur1.low=xau_mid; s_cur1.close=xau_mid; }
        // M5
        if (s_bar5_ms == 0) { s_cur5 = {b5/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar5_ms = b5; }
        else if (b5 != s_bar5_ms) { g_bars_gold.m5.add_bar(s_cur5); s_cur5 = {b5/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar5_ms = b5; }
        else { if(xau_mid>s_cur5.high)s_cur5.high=xau_mid; if(xau_mid<s_cur5.low)s_cur5.low=xau_mid; s_cur5.close=xau_mid; }
        // M15
        if (s_bar15_ms == 0) { s_cur15 = {b15/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar15_ms = b15; }
        else if (b15 != s_bar15_ms) { g_bars_gold.m15.add_bar(s_cur15); s_cur15 = {b15/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar15_ms = b15; }
        else { if(xau_mid>s_cur15.high)s_cur15.high=xau_mid; if(xau_mid<s_cur15.low)s_cur15.low=xau_mid; s_cur15.close=xau_mid; }
        // H1 -- feeds H1SwingEngine + broader HTF context.
        // 14 H1 bars = 14 hours cold; warm restart immediate from saved indicators.
        if (s_bar_h1_ms == 0) { s_cur_h1 = {bh1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h1_ms = bh1; }
        else if (bh1 != s_bar_h1_ms) {
            g_bars_gold.h1.add_bar(s_cur_h1);
            // H1 bar close dispatch: management always runs; entry only when slot is clear
            if (g_h1_swing_gold.has_open_position()) {
                g_h1_swing_gold.on_h1_bar(
                    xau_mid, bid, ask,
                    g_bars_gold.h1.ind.ema9  .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.ema21 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.ema50 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.atr14 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.rsi14 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.adx14 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.adx_rising.load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.trend_state.load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.adx14 .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.atr14 .load(std::memory_order_relaxed),  // crash-day gate
                    g_macro_ctx.session_slot, now_ms_g, ca_on_close);
            } else if (!gold_any_open && !g_h4_regime_gold.has_open_position()
                       && g_bars_gold.h1.ind.m1_ready.load(std::memory_order_relaxed)) {
                const auto h1sig = g_h1_swing_gold.on_h1_bar(
                    xau_mid, bid, ask,
                    g_bars_gold.h1.ind.ema9  .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.ema21 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.ema50 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.atr14 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.rsi14 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.adx14 .load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.adx_rising.load(std::memory_order_relaxed),
                    g_bars_gold.h1.ind.trend_state.load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.adx14 .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.atr14 .load(std::memory_order_relaxed),  // crash-day gate
                    g_macro_ctx.session_slot, now_ms_g, ca_on_close);
                if (h1sig.valid) {
                    const double h1_lot = enter_directional("XAUUSD", h1sig.is_long,
                        h1sig.entry, h1sig.sl, h1sig.tp, 0.01, true, bid, ask, sym, regime, "H1SwingGold");
                    if (!h1_lot) { g_h1_swing_gold.cancel(); }
                    else {
                        g_h1_swing_gold.patch_size(h1_lot);
                        g_telemetry.UpdateLastSignal("XAUUSD",
                            h1sig.is_long ? "LONG" : "SHORT", h1sig.entry, h1sig.reason,
                            "H1_SWING", regime.c_str(), "H1_SWING", h1sig.tp, h1sig.sl);
                    }
                }
            }
            s_cur_h1 = {bh1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h1_ms = bh1;
        } else { if(xau_mid>s_cur_h1.high)s_cur_h1.high=xau_mid; if(xau_mid<s_cur_h1.low)s_cur_h1.low=xau_mid; s_cur_h1.close=xau_mid; }
        // H4 -- HTF gate for TrendPullback + H4RegimeEngine.
        // 14 H4 bars = 56 hours cold; warm restart immediate.
        if (s_bar_h4_ms == 0) { s_cur_h4 = {bh4/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h4_ms = bh4; }
        else if (bh4 != s_bar_h4_ms) {
            g_bars_gold.h4.add_bar(s_cur_h4);
            // H4 bar close dispatch
            if (g_h4_regime_gold.has_open_position()) {
                g_h4_regime_gold.on_h4_bar(
                    s_cur_h4.high, s_cur_h4.low, s_cur_h4.close,
                    xau_mid, bid, ask,
                    g_bars_gold.h4.ind.ema9  .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.ema50 .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.atr14 .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.rsi14 .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.adx14 .load(std::memory_order_relaxed),
                    g_bars_gold.m15.ind.atr_expanding.load(std::memory_order_relaxed),
                    now_ms_g, ca_on_close);
            } else if (!gold_any_open && !g_h1_swing_gold.has_open_position()
                       && g_bars_gold.h4.ind.m1_ready.load(std::memory_order_relaxed)) {
                const auto h4sig = g_h4_regime_gold.on_h4_bar(
                    s_cur_h4.high, s_cur_h4.low, s_cur_h4.close,
                    xau_mid, bid, ask,
                    g_bars_gold.h4.ind.ema9  .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.ema50 .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.atr14 .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.rsi14 .load(std::memory_order_relaxed),
                    g_bars_gold.h4.ind.adx14 .load(std::memory_order_relaxed),
                    g_bars_gold.m15.ind.atr_expanding.load(std::memory_order_relaxed),
                    now_ms_g, ca_on_close);
                if (h4sig.valid) {
                    const double h4_lot = enter_directional("XAUUSD", h4sig.is_long,
                        h4sig.entry, h4sig.sl, h4sig.tp, 0.01, true, bid, ask, sym, regime, "H4RegimeGold");
                    if (!h4_lot) { g_h4_regime_gold.cancel(); }
                    else {
                        g_h4_regime_gold.patch_size(h4_lot);
                        g_telemetry.UpdateLastSignal("XAUUSD",
                            h4sig.is_long ? "LONG" : "SHORT", h4sig.entry, h4sig.reason,
                            "H4_REGIME", regime.c_str(), "H4_REGIME", h4sig.tp, h4sig.sl);
                    }
                }
            }

            // MinimalH4Breakout dispatch -- runs PARALLEL to H4RegimeEngine.
            // Independent of gold_any_open / H1 swing blocks (shadow only).
            // Entry is local-only in shadow mode: no enter_directional call, no
            // broker orders. The engine manages TP/SL internally via on_tick()
            // below and reports closed trades through ca_on_close -> handle_closed_trade
            // which feeds telemetry. When the engine is promoted to LIVE, add
            // an enter_directional call here mirroring the H4RegimeEngine pattern.
            {
                const auto m4sig = g_minimal_h4_gold.on_h4_bar(
                    s_cur_h4.high, s_cur_h4.low, s_cur_h4.close,
                    bid, ask,
                    g_bars_gold.h4.ind.atr14.load(std::memory_order_relaxed),
                    now_ms_g, ca_on_close);
                if (m4sig.valid) {
                    g_telemetry.UpdateLastSignal("XAUUSD",
                        m4sig.is_long ? "LONG" : "SHORT", m4sig.entry, m4sig.reason,
                        "MINIMAL_H4", regime.c_str(), "MINIMAL_H4", m4sig.tp, m4sig.sl);
                }
            }
            s_cur_h4 = {bh4/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h4_ms = bh4;
        } else { if(xau_mid>s_cur_h4.high)s_cur_h4.high=xau_mid; if(xau_mid<s_cur_h4.low)s_cur_h4.low=xau_mid; s_cur_h4.close=xau_mid; }
    }

    // ?? VPIN toxicity tracker -- updated every XAUUSD tick ????????????????
    g_vpin.on_tick(xau_mid, now_ms_g);
    // ?? Correlation matrix feed -- XAUUSD ??????????????????????????????????
    g_corr_matrix.on_price("XAUUSD", xau_mid);

    // -- L2 tick logger -- UNCONDITIONAL, every XAUUSD tick ---------------
    // the logger also stopped, meaning we had ZERO L2 data saved on days
    // when L2 was supposedly "dead". This is the fix.
    //
    // Logs ALL L2 data regardless of engine state: depth levels, bid/ask vol,
    // event count from CTraderDepthClient, and watchdog dead flag.
    // Daily rotating CSV: C:\Omega\logs\l2_ticks_XAUUSD_YYYY-MM-DD.csv
    // 2026-04-22: renamed from l2_ticks_YYYY-MM-DD.csv (no symbol) to match
    // the new SP/NQ logger naming. Also added 'mid' as the 2nd column so
    // hydrate_from_csv() doesn't have to re-compute (bid+ask)/2 on every row
    // during CSV replay at startup.
    {
        static FILE*   s_l2f_unc = nullptr;
        static int     s_l2_day_unc = -1;
        const time_t   t_l2_unc  = (time_t)(now_ms_g / 1000);
        struct tm      tm_l2_unc{};
        gmtime_s(&tm_l2_unc, &t_l2_unc);
        if (tm_l2_unc.tm_yday != s_l2_day_unc) {
            if (s_l2f_unc) { fclose(s_l2f_unc); s_l2f_unc = nullptr; }
            char l2path_unc[256];
            snprintf(l2path_unc, sizeof(l2path_unc),
                "C:\\Omega\\logs\\l2_ticks_XAUUSD_%04d-%02d-%02d.csv",
                tm_l2_unc.tm_year+1900, tm_l2_unc.tm_mon+1, tm_l2_unc.tm_mday);
            bool is_new_unc = (GetFileAttributesA(l2path_unc) == INVALID_FILE_ATTRIBUTES);
            s_l2f_unc = fopen(l2path_unc, "a");
            if (s_l2f_unc) {
                if (is_new_unc)
                    fprintf(s_l2f_unc,
                        "ts_ms,mid,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,"
                        "depth_bid_levels,depth_ask_levels,depth_events_total,"
                        "watchdog_dead,vol_ratio,regime,vpin,has_pos,micro_edge,ewm_drift\n");
                // Confirm file opened successfully in latest.log
                std::cout << "[L2-CSV-OPEN] " << l2path_unc
                          << (is_new_unc ? " (new file, header written)" : " (appending)") << "\n";
                std::cout.flush();
            } else {
                // File failed to open -- this is always visible in latest.log
                std::cout << "[L2-CSV-OPEN-FAIL] Cannot open " << l2path_unc
                          << " -- L2 tick data will NOT be saved this session!\n";
                std::cout.flush();
            }
            s_l2_day_unc = tm_l2_unc.tm_yday;
        }
        if (s_l2f_unc) {
            // Read raw bid/ask counts directly from AtomicL2 -- these are the full
            // cTrader DOM counts BEFORE the 5-level cap in to_l2book().
            // This is real data. depth_bid_levels = raw_bid from cTrader DOM.
            const int    l2_bid_lvls  = g_l2_gold.raw_bid.load(std::memory_order_relaxed);
            const int    l2_ask_lvls  = g_l2_gold.raw_ask.load(std::memory_order_relaxed);
            // Volume: cTrader sends sz_raw for each quote. Sum from raw counts * 1.0
            // (since sz_raw=0 is substituted as 1.0 per level in to_l2book).
            // Actual volume signal is not available from cTrader for XAUUSD.
            // Log counts as proxy -- real directional signal is l2_imb (raw_imbalance).
            const double l2_bvol_unc  = static_cast<double>(l2_bid_lvls);
            const double l2_avol_unc  = static_cast<double>(l2_ask_lvls);
            const double vol_ratio_log = (g_gold_stack.base_vol_pct() > 0.0)
                ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 0.0;
            // S14 fix 2026-04-24: restore fprintf row-write. S19 Stage 1B (a199bec)
            // L2 CSV logger was unconditional by design. Result: header written
            // once per day then zero data rows, CSV stale for every tick since
            // 2026-04-17 (VERIFY_STARTUP flagged 16767s stale 2026-04-24 07:43 UTC).
            fprintf(s_l2f_unc,
                "%lld,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,"
                "%d,%d,%llu,"
                "%d,%.3f,%d,%.3f,%d,%.4f,%.4f\n",
                (long long)now_ms_g, xau_mid, bid, ask,
                g_macro_ctx.gold_l2_imbalance,
                l2_bvol_unc, l2_avol_unc,
                l2_bid_lvls, l2_ask_lvls,
                (unsigned long long)g_ctrader_depth.depth_events_total.load(std::memory_order_relaxed),
                (int)g_l2_watchdog_dead.load(std::memory_order_relaxed),
                vol_ratio_log,
                (int)gold_sdec.regime,
                g_vpin.warmed() ? g_vpin.vpin() : 0.0,
                0,
                g_l2_gold.micro_edge.load(std::memory_order_relaxed),
                static_cast<double>(g_gold_stack.ewm_drift()));
            fflush(s_l2f_unc);

            // L2 CSV health heartbeat every 60s -- confirms file is open and writing.
            // If [L2-CSV-HEALTH] stops appearing in latest.log, the CSV has stopped.
            static int64_t  s_l2_health_ts = 0;
            static uint64_t s_l2_row_count = 0;
            ++s_l2_row_count;
            if (now_ms_g - s_l2_health_ts >= 60000) {
                s_l2_health_ts = now_ms_g;
                std::cout << "[L2-CSV-HEALTH] file=OPEN rows=" << s_l2_row_count
                          << " date=" << (tm_l2_unc.tm_year+1900) << "-"
                          << (tm_l2_unc.tm_mon+1) << "-" << tm_l2_unc.tm_mday << "\n";
                std::cout.flush();
            }
        } else {
            // File failed to open or was never created -- alert every 30s
            static int64_t s_l2_fail_ts = 0;
            if (now_ms_g - s_l2_fail_ts >= 30000) {
                s_l2_fail_ts = now_ms_g;
                std::cout << "[L2-CSV-FAIL] L2 tick file NOT open -- data LOST. "
                          << "Check C:\\Omega\\logs\\ write permissions\n";
                std::cout.flush();
            }
        }
    }
    // -- end L2 tick logger (unconditional) -------------------------------

    if (gold_spread_ok) {
        if (now_ms_g - g_bracket_gold_minute_start >= 60000) {
            g_bracket_gold_minute_start       = now_ms_g;
            g_bracket_gold_trades_this_minute = 0;
        }
        const bool gold_freq_ok    = (g_bracket_gold_trades_this_minute < 3);
        const bool bracket_open    = g_bracket_gold.has_open_position();

        const bool in_dead_zone  = false;  // dead zone removed -- gold trades 24h
        const bool in_asia_slot  = (g_macro_ctx.session_slot == 6);
        // Asia trend gate: require meaningful price drift in Asia slot.
        // OLD: used is_drift_trending(l2_imbalance) -- passed L2 imbalance into
        //   a function that checks book skew. With neutral book (imb=0.502) it
        //   returned false even during a $7 EWM drift. Big Asia moves were missed.
        // NEW: use EWM drift directly. If |drift| >= 1.5 in Asia, a real trend
        //   (SIGNAL_STALE check) but the outer bracket/flow gate uses drift.
        const double gold_ewm_drift_abs = std::fabs(g_gold_stack.ewm_drift());
        // Asia trend gate: require sustained directional drift to enter in Asia session.
        // Threshold raised 1.5->2.5: drift oscillates around 1.5 on quiet Asia ticks,
        // causing constant flapping that prevents the bracket from ever arming.
        // At 2.5 the gate is stable -- only real directional pressure clears it.
        //
        // CRASH BYPASS: macro crash/rally -- bypass Asia drift gate entirely.
        // OLD: required drift < -4.0. During a slow 125pt grind, EWM drift
        // only reaches -1.7 even though price fell $125. The EWM smooths it.
        // FIX: use RSI alone as the primary gate -- RSI<32 = genuine crash,
        // no drift threshold needed. Drift just confirms the direction.
        // Also bypass if price has moved > $15 from VWAP (macro displacement).
        const double gf_vwap_now   = g_gold_stack.vwap();
        const double gf_mid_now    = (bid + ask) * 0.5;
        const double vwap_disp     = (gf_vwap_now > 0.0)
            ? std::fabs(gf_mid_now - gf_vwap_now) : 0.0;
        // ?? RSI reversal momentum tracking for asia_crash_bypass ??????????????
        // Track whether RSI has been recovering (rising from below 35 toward above 40,
        // or falling from above 65 toward below 60). This is the reversal candle visible
        // on the chart that previously had no code path to enter.
        //
        // Simple implementation: store last RSI value, detect direction.
        // Rise from <35: RSI was oversold and is now recovering = LONG reversal setup.
        // Fall from >65: RSI was overbought and is now recovering = SHORT reversal setup.
        // Gate: prior RSI must have been in extreme zone (< 38 or > 62) at some point
        // within last 60s to prevent false triggers on mid-range RSI oscillations.
        static double  s_rsi_prev          = 50.0;   // RSI on prior tick
        static double  s_rsi_extreme_seen  = 50.0;   // most extreme RSI seen in last 60s
        static int64_t s_rsi_extreme_ts    = 0;       // when extreme was last seen
        {
            const int64_t now_rsi_t = static_cast<int64_t>(std::time(nullptr));
            // Update extreme tracker (60s window)
            if ((rsi_for_gate > 0.0 && rsi_for_gate < 38.0) || rsi_for_gate > 62.0) {
                s_rsi_extreme_seen = rsi_for_gate;
                s_rsi_extreme_ts   = now_rsi_t;
            } else if (now_rsi_t - s_rsi_extreme_ts > 60) {
                s_rsi_extreme_seen = 50.0;  // expired
            }
        }
        // RSI reversal: was extreme recently AND now recovering in opposite direction
        const bool rsi_reversal_long_bypass  = (s_rsi_extreme_seen < 38.0)  // was oversold
                                            && (rsi_for_gate > s_rsi_prev)  // rising
                                            && (rsi_for_gate > 30.0);        // not still at rock bottom
        const bool rsi_reversal_short_bypass = (s_rsi_extreme_seen > 62.0)  // was overbought
                                            && (rsi_for_gate < s_rsi_prev)  // falling
                                            && (rsi_for_gate < 70.0);        // not still at peak
        s_rsi_prev = rsi_for_gate;

        const bool asia_crash_bypass =
            // ATR-proportional -- reuses rsi_crash_lo/hi computed from gf_atr_gate above.
            // Normal day (ATR=5): lo=44 hi=56. Crash day (ATR=10): lo=38 hi=62.
            // VWAP displacement also scales: 1.5 * atr (3.75pt at ATR=5, 7.5pt at ATR=10 -- was hardcoded 6pt)
            (rsi_for_gate > 0.0 && rsi_for_gate < rsi_crash_lo)
            || (rsi_for_gate > rsi_crash_hi)
            || (drift_for_gate < -drift_bypass_thresh && vwap_disp > 1.5 * gf_atr_gate)
            || (drift_for_gate >  drift_bypass_thresh && vwap_disp > 1.5 * gf_atr_gate)
            // [2026-04-07] RSI REVERSAL: was extreme, now recovering -- the bounce candle
            || rsi_reversal_long_bypass
            || rsi_reversal_short_bypass;
        // Schmitt trigger on asia_trend_ok -- hysteresis prevents flapping.
        // ARM  threshold: 0.5pt drift -- real directional pressure (chop < 0.4 consistently)
        // DISARM threshold: 0.4pt -- tight enough to not linger on retracement
        static thread_local bool s_asia_trend_armed = false;
        if      (gold_ewm_drift_abs >= 0.5) s_asia_trend_armed = true;
        else if (gold_ewm_drift_abs <  0.4) s_asia_trend_armed = false;

        // FIX 2026-04-07: add tick-based emergency bypasses for asia_trend_ok.
        // asia_crash_bypass uses rsi_crash_lo/hi (ATR-proportional) which requires
        // warmed ATR. Add two independent bypasses that work from live tick data:
        //   a) VWAP displacement >= 6pt -- price has left fair value, trend is real
        //   b) EWM drift magnitude >= 2.0 -- strong sustained directional pressure
        // These fire even when bars are stale/frozen and rsi_crash_lo/hi is at default.
        // vwap_disp already computed above from gf_vwap_now/gf_mid_now -- reuse directly.
        const bool asia_tick_bypass = (vwap_disp >= 6.0)
                                   || (gold_ewm_drift_abs >= 2.0);

        const bool asia_trend_ok = !in_asia_slot
            || asia_crash_bypass
            || asia_tick_bypass
            || s_asia_trend_armed;

        // London open noise guard: 07:00-07:15 UTC -- first 15min of London open
        // has violent liquidity sweeps as Asian orders get repriced. The gold stack
        // already blocks this window (confirmed fix in GoldEngineStack.hpp line 494).
        // The bracket must also block new arming: evidence = SHORT 07:00:34 SL_HIT $7.97,
        // entire $7.80 bracket range was one London open sweep.
        // Existing ARMED/PENDING/LIVE positions are NOT cancelled -- only new arming blocked.
        const bool in_london_open_noise = false;  // REMOVED: spread/regime/SL gates are sufficient protection

        // ?? Trend bias: handled generically via g_bracket_trend["XAUUSD"] ?
        // Counter-trend suppression, L2 extension/shortening, and pyramiding
        // are all applied inside dispatch_bracket via BracketTrendState.
        // Gold uses the same system as all other bracket symbols.
        // Dead zone (05:00-07:00 UTC): not hard-blocked -- spread/regime gates suffice.
        BracketTrendState& gold_trend = g_bracket_trend["XAUUSD"];
        gold_trend.update_l2(g_macro_ctx.gold_l2_imbalance, now_ms_g);  // real cTrader DOM imbalance
        const bool gold_trend_blocked = gold_trend.counter_trend_blocked(now_ms_g);

        // BracketTrendState normally requires 2 consecutive bracket wins to set bias.
        // so pyramid_allowed() returns false even when gold is trending hard.
        //
        // active, not just BE), inject the flow direction as trend bias directly.
        // This lets pyramid_allowed() gate on L2 confirmation alone.
        // Only inject if bias is currently 0 -- never override an earned bracket bias.
        // Withdraw injection when flow closes (handled naturally -- bias resets on timeout).
        // Withdraw bias injection when flow closes or reverses

        const bool gold_pyramid_ok    = gold_trend.pyramid_allowed(g_macro_ctx.gold_l2_imbalance, now_ms_g);  // real cTrader DOM imbalance
        // Block pyramid in IMPULSE regime: price is thrusting hard -- adding on
        // during a thrust means chasing the move at peak momentum with tight SLs.
        // ?? Impulse regime gate ???????????????????????????????????????????????
        // breaks down in a thrust -- the bracket range is meaningless mid-thrust.
        // EXCEPTION: during a confirmed crash (RSI<35, drift<-4) or rally (RSI>65,
        // drift>4), the IMPULSE regime IS the trade. Block the bracket only if
        // we are NOT in a crash/rally. TrendBracket (one-sided) fires instead in
        // IMPULSE -- but that requires trend_bias != 0. During Asia cold-start,
        // trend_bias may not be set, so TrendBracket also misses. This crash bypass
        // ensures at least one path fires on confirmed directional IMPULSE moves.
        const bool gold_impulse_regime = (std::strcmp(gold_stack_regime, "IMPULSE") == 0)
                                       && !asia_crash_bypass; // allow bracket in crash IMPULSE

        // ?? Exhaustion check: don't pyramid at local extremes ?????????????
        // If price has already moved >4pts from the bracket entry in the pyramid
        // direction AND the last tick reversed (price retracing), the move may
        // be exhausted. This is exactly what happened: SHORT entry 4621, pyramid
        // at 4614 (-7pts) = price already extended, then V-reversed to 4620.
        // Also: require bracket position to be profitable before pyramiding --
        // no sense adding to a position that hasn't proven itself yet.
        const bool bracket_profitable = bracket_open &&
            (g_bracket_gold.pos.is_long
                ? (gf_mid > g_bracket_gold.pos.entry + 2.0)   // LONG needs $2+ in profit
                : (gf_mid < g_bracket_gold.pos.entry - 2.0)); // SHORT needs $2+ in profit
        const bool gold_is_pyramiding = gold_pyramid_ok && bracket_profitable &&
                                        !gold_impulse_regime &&
                                        ((gold_trend.bias == 1  && g_bracket_gold.pos.is_long) ||
                                         (gold_trend.bias == -1 && !g_bracket_gold.pos.is_long));

        // ?? Trend-direction cooldown bypass ???????????????????????????????
        // On a trending day, the bracket closes via TRAIL_HIT and enters the 90s
        // cooldown. The next compression may appear within 60s. The cooldown is
        // correct for the counter-trend leg (prevent re-arming into the same bad
        // direction), but the TREND-DIRECTION leg should be able to re-arm as soon
        // as new structure qualifies -- the trend is the thesis, not a risk.
        // Bypass: when trend bias is active AND the bracket is in COOLDOWN AND the
        // structure gates (freq, spread, session) all pass, allow re-arm.
        // Safety: london_open_noise and IMPULSE regime blocks still apply.
        const bool in_cooldown_phase = (g_bracket_gold.phase == omega::BracketPhase::COOLDOWN);
        const bool trend_dir_bypasses_cooldown =
            (gold_trend.bias != 0) && in_cooldown_phase &&
            !gold_impulse_regime &&
            !in_london_open_noise;
        if (trend_dir_bypasses_cooldown) {
            static int64_t s_bypass_log = 0;
            if (now_ms_g - s_bypass_log > 10000) {
                s_bypass_log = now_ms_g;
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[BRACKET-COOLDOWN-BYPASS] XAUUSD bias=%d -- trend leg bypasses 90s cooldown\n",                        gold_trend.bias);
                    std::cout << _msg;
                    std::cout.flush();
                }
            }
        }

        // the bracket is normally blocked from arming. This kills pyramiding on the
        // strongest trending moves -- exactly when we want to be adding size.
        //
        // position when flow is profitable (trail_stage >= 1 = past breakeven).
        // The bracket add-on uses a reduced size (50% of PYRAMID_SIZE_MULT = 37.5%)
        //
        // Safety gates still apply: no IMPULSE regime, no counter-trend, spread gate,
        // L2 confirmation required, position cap enforced.
        // Bracket-override block (gold_bracket_already_armed / BRK_OVERRIDE_DIST /
        // bypass logic — removed with engine. Bracket now arms on base gates only.
        const bool can_arm_bracket = gold_can_enter
                                  && gold_freq_ok
                                  && gold_spread_ok
                                  && !bracket_open
                                  && !gold_impulse_regime
                                  && !in_london_open_noise;

        // ?? Trend-direction bracket (IMPULSE regime) ??????????????????????????
        // removed, this block is retired. Symmetric bracket + MacroCrash handle
        // impulse / trend-direction entries now.

        // ?? Gold bracket gate diagnostic -- prints every 10s ???????????????
        {
            static int64_t s_last_brk_diag = 0;
            if (now_ms_g - s_last_brk_diag >= 10000) {
                s_last_brk_diag = now_ms_g;
                const char* phase_str =
                    g_bracket_gold.phase == omega::BracketPhase::IDLE     ? "IDLE"
                  : g_bracket_gold.phase == omega::BracketPhase::ARMED    ? "ARMED"
                  : g_bracket_gold.phase == omega::BracketPhase::PENDING  ? "PENDING"
                  : g_bracket_gold.phase == omega::BracketPhase::CONFIRM  ? "CONFIRM"
                  : g_bracket_gold.phase == omega::BracketPhase::LIVE     ? "LIVE"
                  :                                                          "COOLDOWN";
                std::cout << "[GOLD-BRK-DIAG]"
                          << " phase="         << phase_str
                          << " can_enter="     << gold_can_enter
                          << " spread_ok="     << gold_spread_ok
                          << " freq_ok="       << gold_freq_ok
                          << " bracket_open="  << bracket_open
                          << " stack_open="    << g_gold_stack.has_open_position()
                          << " in_dead_zone="  << in_dead_zone
                          << " london_noise="  << in_london_open_noise
                          << " in_asia="       << in_asia_slot
                          << " asia_ok="       << asia_trend_ok
                          << " drift="         << std::fixed << std::setprecision(2) << g_gold_stack.ewm_drift()
                          << " l2_imb="        << std::setprecision(3) << g_macro_ctx.gold_l2_imbalance
                          << " can_arm="       << can_arm_bracket
                          << " trend_bias="    << gold_trend.bias
                          << " trend_blocked=" << gold_trend_blocked
                          << " pyramid_ok="    << gold_pyramid_ok
                          << " impulse_block=" << gold_impulse_regime
                          << " cd_bypass="     << trend_dir_bypasses_cooldown
                          << " pyr_sl_age_s="  << (gold_trend.last_pyramid_sl_ms == 0
                                                    ? std::string("never")
                                                    : std::to_string((now_ms_g - gold_trend.last_pyramid_sl_ms) / 1000) + "s")
                          << " brk_hi="        << std::setprecision(2) << g_bracket_gold.bracket_high
                          << " brk_lo="        << g_bracket_gold.bracket_low
                          << " range="         << (g_bracket_gold.bracket_high - g_bracket_gold.bracket_low)
                          << " session_slot="  << g_macro_ctx.session_slot
                          << "\n";
                std::cout.flush();
            }
        }
        // Phase-aware gate:
        //   IDLE    ? can_arm_bracket: all gates apply to start arming
        //   ARMED   ? true: timer must run uninterrupted, no supervisor re-gating
        //   PENDING ? true: orders at broker, only timeout or spread gate cancels
        //   LIVE    ? gold_can_enter: allow force-close on session end
        const bool gold_bracket_armed   = (g_bracket_gold.phase == omega::BracketPhase::ARMED);
        const bool gold_bracket_pending = (g_bracket_gold.phase == omega::BracketPhase::PENDING);
        const bool can_manage      = (gold_bracket_armed || gold_bracket_pending) ? true : gold_can_enter;

        // g_bracket_gold is GoldBracketEngine (BracketEngineBase subclass).
        // Signature: on_tick(bid, ask, now_ms, can_enter, regime, on_close, vwap, l2_imb)
        // NOTE: g_hybrid_gold (GoldHybridBracketEngine) is separate and already wired
        // correctly at lines ~2929/2945 with the bool flow_live signature.
        //
        // Session 9: GoldCoordinator BRACKET_LANE gate. Skeleton is allow-by-default
        // (can_enter() always returns true). Present behaviour: unchanged. When the
        // coordinator starts denying in Session 10+, an IDLE-phase engine will be
        // kept IDLE by forcing can_enter=false below.
        const bool gold_coord_allow = omega::g_gold_coordinator.can_enter(
            omega::GoldLane::BRACKET_LANE, "XAUUSD", g_bracket_gold.ENTRY_SIZE);
        const bool bracket_can_enter_base =
            (bracket_open || gold_bracket_armed) ? can_manage : can_arm_bracket;
        const bool bracket_can_enter_eff =
            bracket_can_enter_base && gold_coord_allow;
        if (!gold_coord_allow && bracket_can_enter_base) {
            // Diagnostic: coordinator denied something the base gate would have allowed.
            // In Session 9 skeleton this branch is never taken (can_enter() always true).
            printf("[COORD] XAUUSD BRACKET_LANE can_enter=false (base=true) lots=%.4f\n",
                   g_bracket_gold.ENTRY_SIZE);
        }
        g_bracket_gold.on_tick(bid, ask, now_ms_g,
            bracket_can_enter_eff,
            regime.c_str(), bracket_on_close, gold_vwap_now,
            g_macro_ctx.gold_l2_imbalance,
            g_gold_stack.ewm_drift());   // Session 13: drift arg drives regime-flip exit
        g_telemetry.UpdateBracketState("XAUUSD",
            static_cast<int>(g_bracket_gold.phase),
            g_bracket_gold.bracket_high,
            g_bracket_gold.bracket_low);
        const auto bgsigs = g_bracket_gold.get_signals();
        if (bgsigs.valid) {
            // Pyramid: use tighter SL than normal bracket -- cap pyramid risk at 50% of base risk
            // Normal bracket SL can be $5-7 wide; pyramid add-ons use half that
            // Pyramid now triggers on gold_is_pyramiding only (bracket self-pyramid path).
            const double raw_sl_dist = std::fabs(bgsigs.long_entry - bgsigs.long_sl);
            const double pyr_sl_dist = gold_is_pyramiding
                ? std::min(raw_sl_dist, 3.0)  // cap pyramid SL at $3 distance
                : raw_sl_dist;
            const double base_bg_lot = compute_size("XAUUSD",
                pyr_sl_dist, ask - bid,
                g_bracket_gold.ENTRY_SIZE);
            const double bg_lot = gold_is_pyramiding
                ? std::min(base_bg_lot * PYRAMID_SIZE_MULT, 0.20)  // cap pyramid lot at 0.20
                : base_bg_lot;
            // Cost guard: bracket TP dist = SL dist * RR
            const double bg_tp_dist = std::fabs(bgsigs.long_entry - bgsigs.long_sl) * g_bracket_gold.RR;
            const bool bg_cost_ok = ExecutionCostGuard::is_viable("XAUUSD", ask - bid, bg_tp_dist, bg_lot);
            if (!bg_cost_ok) {
                g_telemetry.IncrCostBlocked();
            } else {
                // ── Cross-engine dedup (S20 2026-04-25) ────────────────────────
                // Same mechanism as GoldEngineStack gate above. Blocks BracketGold
                // arm when another XAUUSD engine entered within dedup window.
                // Registers its own entry timestamp so subsequent engines see it.
                // Flag used to skip arm-and-send while letting the rest of the
                // tick function (HybridBracketGold, other engines) continue.
                bool bg_dedup_blocked = false;
                {
                    std::lock_guard<std::mutex> _lk(g_dedup_mtx);
                    auto _it = g_last_cross_entry.find("XAUUSD");
                    if (_it != g_last_cross_entry.end() &&
                        (nowSec() - _it->second) < CROSS_ENG_DEDUP_SEC) {
                        char _msg[256];
                        snprintf(_msg, sizeof(_msg),
                            "[CROSS-DEDUP] XAUUSD BracketGold blocked -- another engine entered %lds ago (dedup=%lds)\n",
                            static_cast<long>(nowSec() - _it->second),
                            static_cast<long>(CROSS_ENG_DEDUP_SEC));
                        std::cout << _msg;
                        std::cout.flush();
                        g_telemetry.IncrCostBlocked();
                        bg_dedup_blocked = true;
                    } else {
                        g_last_cross_entry["XAUUSD"] = nowSec();
                    }
                }
                if (!bg_dedup_blocked) {
                char bg_reason[80];
                snprintf(bg_reason, sizeof(bg_reason), "HI:%.2f LO:%.2f bias:%d l2:%.2f",
                         bgsigs.long_entry, bgsigs.short_entry,
                         gold_trend.bias, g_macro_ctx.gold_l2_imbalance);
                g_telemetry.UpdateLastSignal("XAUUSD", "BRACKET", bgsigs.long_entry, bg_reason,
                    omega::regime_name(gold_sdec.regime), regime.c_str(), "BRACKET",
                    bgsigs.long_tp, bgsigs.long_sl);
                std::cout << "\033[1;33m[BRACKET] XAUUSD"
                          << " sup_regime=" << omega::regime_name(gold_sdec.regime)
                          << " bracket_score=" << gold_sdec.bracket_score
                          << " winner=" << gold_sdec.winner
                          << " bias=" << gold_trend.bias
                          << " l2=" << std::fixed << std::setprecision(2) << g_macro_ctx.gold_l2_imbalance
                          << (gold_is_pyramiding ? " PYRAMID" : "")
                          << "\033[0m\n";
                std::string long_id, short_id;
                if (gold_trend.bias != 0) {
                    const bool long_is_trend = (gold_trend.bias == 1);
                    const double long_lot  = long_is_trend  ? bg_lot : (bg_lot * 0.5);
                    const double short_lot = !long_is_trend ? bg_lot : (bg_lot * 0.5);
                    write_trade_open_log("XAUUSD", "BracketGold", "LONG",
                        bgsigs.long_entry,
                        bgsigs.long_entry + (bgsigs.long_entry - bgsigs.short_entry) * 1.5,
                        bgsigs.short_entry, long_lot, ask - bid,
                        gold_stack_regime, gold_is_pyramiding ? "BRACKET_PYRAMID" : "BRACKET_ARM");
                    write_trade_open_log("XAUUSD", "BracketGold", "SHORT",
                        bgsigs.short_entry,
                        bgsigs.short_entry - (bgsigs.long_entry - bgsigs.short_entry) * 1.5,
                        bgsigs.long_entry, short_lot, ask - bid,
                        gold_stack_regime, gold_is_pyramiding ? "BRACKET_PYRAMID" : "BRACKET_ARM");
                    long_id  = send_live_order("XAUUSD", true,  long_lot,  bgsigs.long_entry);
                    short_id = send_live_order("XAUUSD", false, short_lot, bgsigs.short_entry);
                    g_telemetry.UpdateLastEntryTs();  // watchdog: bracket (biased) entry
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[BRACKET-L2] XAUUSD bias=%d trend_lot=%.4f counter_lot=%.4f l2=%.3f\n",                            gold_trend.bias, bg_lot, bg_lot * 0.5,                            g_macro_ctx.gold_l2_imbalance);
                        std::cout << _msg;
                        std::cout.flush();
                    }
                } else {
                    write_trade_open_log("XAUUSD", "BracketGold", "LONG",
                        bgsigs.long_entry,
                        bgsigs.long_entry + (bgsigs.long_entry - bgsigs.short_entry) * 1.5,
                        bgsigs.short_entry, bg_lot, ask - bid, gold_stack_regime, "BRACKET_ARM");
                    write_trade_open_log("XAUUSD", "BracketGold", "SHORT",
                        bgsigs.short_entry,
                        bgsigs.short_entry - (bgsigs.long_entry - bgsigs.short_entry) * 1.5,
                        bgsigs.long_entry, bg_lot, ask - bid, gold_stack_regime, "BRACKET_ARM");
                    long_id  = send_live_order("XAUUSD", true,  bg_lot, bgsigs.long_entry);
                    short_id = send_live_order("XAUUSD", false, bg_lot, bgsigs.short_entry);
                    g_telemetry.UpdateLastEntryTs();  // watchdog: bracket (neutral) entry
                }
                g_bracket_gold.pending_long_clOrdId  = long_id;
                g_bracket_gold.pending_short_clOrdId = short_id;
                // Tag pyramid arms so bracket_on_close can enforce SL cooldown
                if (gold_is_pyramiding) {
                    g_pyramid_clordids.insert(std::string("XAUUSD"));
                }
                ++g_bracket_gold_trades_this_minute;
                // Session 9: GoldCoordinator BRACKET_LANE entry tracking.
                // Fires at arm-time (orders submitted to broker), not at actual fill.
                // OCO cancels the losing side, so max single-fill exposure is bg_lot.
                // Counter is diagnostic in Session 9 skeleton (no gating enforcement).
                omega::g_gold_coordinator.mark_entered(
                    omega::GoldLane::BRACKET_LANE, "XAUUSD", "BracketGold", bg_lot);
                printf("[COORD] XAUUSD BRACKET_LANE mark_entered lots=%.4f pos_count=%d\n",
                       bg_lot,
                       omega::g_gold_coordinator.position_count(omega::GoldLane::BRACKET_LANE));
                }  // end if (!bg_dedup_blocked)
            }
        }
    }

    // CRITICAL: manage_position() must run on every XAUUSD tick to check SL/trail.
    // Avg winner $15 vs avg loser $74, payoff 0.20:1. 2yr MFE scan showed
    // microstructure signal only -- not the 1-3pt structural moves needed to
    // Mirrors the main flow manage block exactly. The reload instance is an
    // Mirrors the reload manage block. The addon instance is independent --
    // its own SL, trail, ratchet. When base closes, addon keeps running
    // until its own trail/SL fires (already deep in profit by definition).
    // Does NOT arm its own addon or reload.

    // ?? RSIReversal indicator warmup -- UNCONDITIONAL, every tick ????????????
    // Must run before MacroCrashEngine so tick_rsi() is current when MCE reads it.
    // RSIReversalEngine.on_tick() is gated (rsi_rev_can_enter), so when the gate
    // is closed (e.g. hybrid_gold has position) indicators go stale and MacroCrash
    // reads a stale RSI value. update_indicators() is ungated -- always live.
    g_rsi_reversal.update_indicators(bid, ask);
    // RSIExtremeTurnEngine indicator warmup -- UNCONDITIONAL, every tick
    // Must run so tick_rsi / tick_atr are always current regardless of position state.
    g_rsi_extreme.update_indicators(bid, ask);
    // Inject M1 bar RSI for entry signal -- bar RSI is smooth (60s) and matches chart
    if (g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
        const double rsi_bar_mid = (bid + ask) * 0.5;
        g_rsi_reversal.set_bar_rsi(
            g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed),
            rsi_bar_mid);
        // RSIExtremeTurnEngine bar RSI injection -- tracks sustained extreme bars
        g_rsi_extreme.set_bar_rsi(
            g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed));
        g_rsi_reversal.set_bar_context(
            g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed),
            g_bars_gold.m1.ind.atr_expanding.load(std::memory_order_relaxed),
            g_bars_gold.m1.ind.bb_squeeze.load(std::memory_order_relaxed),
            g_bars_gold.m1.ind.adx_trending.load(std::memory_order_relaxed));
    }

    // ?? MacroCrashEngine -- always-on macro event capture ????????????????
    // Fires on: ATR + vol_ratio + drift thresholds (session-aware: lower in Asia).
    // DOM primer: book_slope / vacuum / microprice_bias lower drift threshold 40%
    //   when the DOM is confirming direction before EWM drift catches up.
    // RSI confirmation: RSI extreme aligning with drift substitutes for expansion_regime
    //   in Asia (supervisor lags by CONFIRM_TICKS; RSI is live price-based).
    // Both directions: is_long = (ewm_drift > 0) -- LONG on spikes up, SHORT on crashes.
    {
        const bool expansion_regime = (gold_sdec.regime == omega::Regime::EXPANSION_BREAKOUT
                                    || gold_sdec.regime == omega::Regime::TREND_CONTINUATION);
        // (live candle-based) with vol_range fallback and a 2.0pt absolute floor.
        const double mce_m1_atr   = g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed);
        const double mce_vol_rng  = g_gold_stack.vol_range();
        const double mce_atr      = std::max({2.0, mce_m1_atr, mce_vol_rng * 0.5});
        const double mce_vol_ratio = (g_gold_stack.recent_vol_pct() > 0.0
                                   && g_gold_stack.base_vol_pct() > 0.0)
                                     ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct()
                                     : 1.0;
        // DOM signals -- live from g_macro_ctx (updated every cold-path tick from L2 book)
        const double mce_book_slope     = g_macro_ctx.gold_slope;
        const bool   mce_vacuum_ask     = g_macro_ctx.gold_vacuum_ask;
        const bool   mce_vacuum_bid     = g_macro_ctx.gold_vacuum_bid;
        const double mce_microprice     = g_macro_ctx.gold_microprice_bias;

        // RSI: tick RSI primary (always live, updated every tick, never 0 after warmup).
        // Bar RSI (g_bars_gold.m1.ind.rsi14) initialises to 0.0 and only updates every 60s.
        // When bar RSI = 0.0, the rsi14 > 0.0 guard in MacroCrashEngine kills the RSI bypass
        // so eff_expansion stays false during the entire crash. Use tick RSI from
        // RSIReversalEngine which computes from the same mid price every tick.
        const double mce_bar_rsi  = g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed);
        const double mce_tick_rsi = g_rsi_reversal.tick_rsi();
        const double mce_rsi      = (mce_tick_rsi > 0.0) ? mce_tick_rsi : mce_bar_rsi;

        // VOL_RATIO macro bypass: EWM baseline stays elevated after prior large sessions.
        // After a big prior day base_vol_pct is high, keeping mce_vol_ratio < 2.5 even
        // during a genuine crash. When |drift| >= 8pt the move is unambiguously real --
        // pass 99.0 to clear the vol_ratio gate unconditionally.
        const double mce_ewm_drift     = g_gold_stack.ewm_drift();
        const double mce_vol_ratio_eff = (std::fabs(mce_ewm_drift) >= 6.0)
                                         ? 99.0   // drift >= 6pt bypasses vol gate
                                         : mce_vol_ratio;

        // Block MacroCrash entry when HybridBracketGold has an opposing position.
        // MacroCrash SHORT into a bracket LONG = engines fighting each other.
        // If bracket is active and MacroCrash has no position, check direction conflict.
        //
        // OPEN-LOG FIX 2026-04-21: capture "was open before on_tick" so we can
        // detect a fresh MCE entry transition after on_tick returns and emit
        // write_trade_open_log -- parity with the other engines already
        // TrendBracket). MCE was silently opening positions with no row in
        // the open-trades CSV.
        const bool mce_was_open_before_tick = g_macro_crash.has_open_position();
        const bool mce_bracket_conflict =
            !g_macro_crash.has_open_position() &&
            g_hybrid_gold.has_open_position() &&
            (g_hybrid_gold.pos.is_long ? (mce_ewm_drift < 0) : (mce_ewm_drift > 0));
        // FIX 2026-04-22: HTF hard-block for MacroCrash entry.
        // MCE enters LONG on spikes up, SHORT on crashes (direction = sign of ewm_drift).
        // bias() returns BULLISH/BEARISH only when daily+intraday agree (2/2 rule);
        // NEUTRAL chop days bypass this block.
        // Only block NEW entries -- management of existing MCE position is
        // handled below in the else branch via mce_was_open_before_tick.
        const bool mce_htf_long_intent = (mce_ewm_drift > 0.0);
        const bool mce_htf_blocked = !g_macro_crash.has_open_position()
            && g_htf_filter.bias_opposes("XAUUSD", mce_htf_long_intent);
        if (mce_htf_blocked) {
            static thread_local int64_t s_mce_htf_log = 0;
            if (now_ms_g - s_mce_htf_log > 30000) {
                s_mce_htf_log = now_ms_g;
                char _msg[512];
                snprintf(_msg, sizeof(_msg),
                    "[HTF-BLOCK] XAUUSD MCE %s skipped -- HTF bias=%s opposes\n",
                    mce_htf_long_intent ? "LONG" : "SHORT",
                    g_htf_filter.bias_name("XAUUSD"));
                std::cout << _msg; std::cout.flush();
            }
        }
        // Dispatch order:
        //   entry-allowed          -> dispatch (entry or manage)
        //   HTF blocks entry       -> manage open position only; log handled above
        //   bracket-conflict blocks-> manage open position only; log [MCE-BLOCK]
        if (!mce_bracket_conflict && !mce_htf_blocked) {
            g_macro_crash.on_tick(bid, ask,
                mce_atr, mce_vol_ratio_eff,
                mce_ewm_drift,
                expansion_regime,
                now_ms_g,
                mce_book_slope,
                mce_vacuum_ask,
                mce_vacuum_bid,
                mce_microprice,
                mce_rsi,
                g_macro_ctx.session_slot);
        } else if (mce_bracket_conflict) {
            static int64_t s_mce_conflict_log = 0;
            if (now_ms_g - s_mce_conflict_log > 10000) {
                s_mce_conflict_log = now_ms_g;
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[MCE-BLOCK] MacroCrash entry blocked -- opposing HybridBracket %s active\n",                        g_hybrid_gold.pos.is_long ? "LONG" : "SHORT");
                    std::cout << _msg;
                    std::cout.flush();
                }
            }
            // Still call on_tick to manage any existing MacroCrash position
            if (g_macro_crash.has_open_position()) {
                g_macro_crash.on_tick(bid, ask,
                    mce_atr, mce_vol_ratio_eff,
                    mce_ewm_drift,
                    expansion_regime,
                    now_ms_g,
                    mce_book_slope,
                    mce_vacuum_ask,
                    mce_vacuum_bid,
                    mce_microprice,
                    mce_rsi,
                    g_macro_ctx.session_slot);
            }
        }
        // Note: mce_htf_blocked branch needs no else-if body -- [HTF-BLOCK] was
        // already logged above, and the block itself requires !has_open_position(),
        // so there is no open position to manage here.

        // OPEN-LOG FIX 2026-04-21: detect MCE fresh-entry transition and log.
        // Fires exactly once per MCE entry (the tick where pos.active flipped
        // false->true). Same shape as the 10 other engines' open-log calls.
        // MCE's tp field = bracket_tp (the floor limit price); 0.0 when bracket
        // is not armed. reason = "MACRO_EXPANSION" (the gate that MCE uses).
        if (!mce_was_open_before_tick && g_macro_crash.has_open_position()) {
            write_trade_open_log("XAUUSD", "MacroCrash",
                g_macro_crash.pos.is_long ? "LONG" : "SHORT",
                g_macro_crash.pos.entry,
                g_macro_crash.pos.bracket_tp,  // 0.0 if bracket not armed
                g_macro_crash.pos.sl,
                g_macro_crash.pos.full_size,
                ask - bid, regime, "MACRO_EXPANSION");
        }
    }

    // ?? RSIReversalEngine -- tick-level RSI entries, no bar dependency ????????
    // Computes its own RSI(14) from mid price on every tick.
    // No bars_ready gate, no bar RSI, fires as soon as tick RSI reaches extreme.
    // Entry: RSI<42=LONG, RSI>58=SHORT (catches 5pt moves, not just 20pt+ crashes)
    {
        // Position management -- always runs when open (no gate)
        if (g_rsi_reversal.has_open_position()) {
            g_rsi_reversal.on_tick(bid, ask,
                g_macro_ctx.session_slot, now_ms_g,
                g_macro_ctx.gold_l2_imbalance,
                g_macro_ctx.gold_wall_above,
                g_macro_ctx.gold_wall_below,
                g_macro_ctx.gold_vacuum_ask,
                g_macro_ctx.gold_vacuum_bid,
                g_macro_ctx.gold_l2_real,
                bracket_on_close,
                g_gold_stack.ewm_drift());  // drift: counter-trend block inside engine
        }

        // Entry gate: no other XAUUSD position open + tradeable + not dead zone
        // Blocked only when those engines are losing/flat (risk management).
        const bool rsi_rev_can_enter =
            !g_rsi_reversal.has_open_position()
            && tradeable
            && !in_ny_close_noise
            && !g_bracket_gold.has_open_position()
            && !(g_gold_stack.has_open_position() && !gs_winning)
            && !g_trend_pb_gold.has_open_position()
            && !g_hybrid_gold.has_open_position()
            && !g_nbm_gold_london.has_open_position();

        if (rsi_rev_can_enter) {

            g_rsi_reversal.on_tick(bid, ask,
                g_macro_ctx.session_slot, now_ms_g,
                g_macro_ctx.gold_l2_imbalance,
                g_macro_ctx.gold_wall_above,
                g_macro_ctx.gold_wall_below,
                g_macro_ctx.gold_vacuum_ask,
                g_macro_ctx.gold_vacuum_bid,
                g_macro_ctx.gold_l2_real,
                bracket_on_close,
                g_gold_stack.ewm_drift());  // drift: counter-trend block inside engine

            if (g_rsi_reversal.has_open_position()) {
                // FIX 2026-04-22 P1a: HTF post-dispatch hard-block for RSIReversal.
                // RSIReversal is a mean-reverter: direction is determined inside
                // on_tick() based on RSI extremes, so pre-dispatch drift gating
                // does not work (engine intentionally enters opposite to drift).
                // Post-dispatch pattern: if HTF bias opposes the just-opened
                // position, force-close immediately before any broker order
                // (send_live_order) is issued. No broker reversal needed because
                // the entry order has not yet been sent from this block.
                // Log marker: [HTF-BLOCK-POST] -- distinct from pre-dispatch [HTF-BLOCK].
                if (g_htf_filter.bias_opposes("XAUUSD", g_rsi_reversal.pos.is_long)) {
                    static thread_local int64_t s_rrv_htf_post_log = 0;
                    if (now_ms_g - s_rrv_htf_post_log > 30000) {
                        s_rrv_htf_post_log = now_ms_g;
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg),
                            "[HTF-BLOCK-POST] XAUUSD RSIReversal %s forced-close -- HTF bias=%s opposes\n",
                            g_rsi_reversal.pos.is_long ? "LONG" : "SHORT",
                            g_htf_filter.bias_name("XAUUSD"));
                        std::cout << _msg; std::cout.flush();
                    }
                    g_rsi_reversal.force_close(bid, ask, now_ms_g, handle_closed_trade);
                } else {
                    const double rsi_sl_dist = std::fabs(g_rsi_reversal.pos.entry - g_rsi_reversal.pos.sl);
                    const double rsi_lot     = (rsi_sl_dist > 0.0)
                        ? std::max(0.01, std::min(g_cfg.max_lot_gold,
                            g_cfg.risk_per_trade_usd / (rsi_sl_dist * 100.0)))
                        : 0.05;  // fixed fallback 0.05 lots
                    g_rsi_reversal.patch_size(rsi_lot);

                    // Log and telemetry always fire (shadow or live) -- GUI shows signal
                    // OPEN-LOG FIX 2026-04-22: RSIReversal (arg9 regime slot)
                    write_trade_open_log("XAUUSD", "RSIReversal",
                        g_rsi_reversal.pos.is_long ? "LONG" : "SHORT",
                        g_rsi_reversal.pos.entry, 0.0, g_rsi_reversal.pos.sl,
                        g_rsi_reversal.pos.size, ask - bid, regime, "RSI_EXTREME");
                    g_telemetry.UpdateLastSignal("XAUUSD",
                        g_rsi_reversal.pos.is_long ? "LONG" : "SHORT",
                        g_rsi_reversal.pos.entry, "RSI_EXTREME",
                        "RSI_REVERSAL", regime.c_str(), "RSI_REVERSAL",
                        0.0, g_rsi_reversal.pos.sl);
                    if (!g_rsi_reversal.shadow_mode) {
                        send_live_order("XAUUSD",
                            g_rsi_reversal.pos.is_long,
                            g_rsi_reversal.pos.size,
                            g_rsi_reversal.pos.entry);
                        g_telemetry.UpdateLastEntryTs();
                    }
                }
            }
        }
    }

    // ?? RSIExtremeTurnEngine -- RSI extreme + sustained turn (no DOM) ???????????
    // Entry: bar RSI fell below 20 for 3+ bars (LONG) or above 70 (SHORT), then turns.
    // Exit:  bar RSI recovers to 55 (LONG) or 45 (SHORT). SL=0.5xATR. No DOM.
    // Runs standalone: not blocked by other gold engines except its own open position.
    // Exclusion: blocked when any gold position is open (one-at-a-time).
    {
        // Position management -- always runs when open (no gate)
        if (g_rsi_extreme.has_open_position()) {
            g_rsi_extreme.on_tick(bid, ask,
                g_macro_ctx.session_slot, now_ms_g,
                [&](const omega::TradeRecord& tr) {
                    handle_closed_trade(tr);
                    if (!g_rsi_extreme.shadow_mode) {
                        send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
                        g_telemetry.UpdateLastEntryTs();
                    }
                });
        }

        // Entry gate: no other gold position open + bars ready + tradeable + not NY close noise
        // Bar RSI required (set_bar_rsi injected above when m1_ready=true).
        // DOM deliberately NOT gated here -- backtest proved DOM adds noise not signal.
        const bool rsi_ext_bars_ready = g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed);
        const bool rsi_ext_can_enter =
            !g_rsi_extreme.has_open_position()
            && rsi_ext_bars_ready
            && tradeable
            && !in_ny_close_noise
            && !g_bracket_gold.has_open_position()
            && !g_gold_stack.has_open_position()
            && !g_trend_pb_gold.has_open_position()
            && !g_rsi_reversal.has_open_position()
            && !g_hybrid_gold.has_open_position()
            && !g_nbm_gold_london.has_open_position();

        if (rsi_ext_can_enter) {
            g_rsi_extreme.on_tick(bid, ask,
                g_macro_ctx.session_slot, now_ms_g,
                [&](const omega::TradeRecord& tr) {
                    handle_closed_trade(tr);
                    if (!g_rsi_extreme.shadow_mode) {
                        send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
                        g_telemetry.UpdateLastEntryTs();
                    }
                });

            if (g_rsi_extreme.has_open_position()) {
                // FIX 2026-04-22 P1a: HTF post-dispatch hard-block for RSIExtremeTurn.
                // RSIExtremeTurn is a mean-reverter: direction determined inside
                // on_tick() from sustained bar-RSI extremes. Pre-dispatch drift
                // gating cannot be used (engine enters counter to prevailing
                // drift by design). Post-dispatch pattern identical to RSIReversal:
                // if HTF bias opposes the just-opened position, force-close
                // before any broker order is sent.
                if (g_htf_filter.bias_opposes("XAUUSD", g_rsi_extreme.pos.is_long)) {
                    static thread_local int64_t s_rext_htf_post_log = 0;
                    if (now_ms_g - s_rext_htf_post_log > 30000) {
                        s_rext_htf_post_log = now_ms_g;
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg),
                            "[HTF-BLOCK-POST] XAUUSD RSIExtremeTurn %s forced-close -- HTF bias=%s opposes\n",
                            g_rsi_extreme.pos.is_long ? "LONG" : "SHORT",
                            g_htf_filter.bias_name("XAUUSD"));
                        std::cout << _msg; std::cout.flush();
                    }
                    g_rsi_extreme.force_close(bid, ask, now_ms_g, handle_closed_trade);
                } else {
                    // Size using standard risk engine
                    const double rsi_ext_sl_dist = std::fabs(
                        g_rsi_extreme.pos.entry - g_rsi_extreme.pos.sl);
                    const double rsi_ext_lot = (rsi_ext_sl_dist > 0.0)
                        ? std::max(0.01, std::min(g_cfg.max_lot_gold,
                            g_cfg.risk_per_trade_usd / (rsi_ext_sl_dist * 100.0)))
                        : 0.02;  // fixed fallback
                    g_rsi_extreme.patch_size(rsi_ext_lot);

                    // Log and telemetry
                    // OPEN-LOG FIX 2026-04-22: RSIExtremeTurn (arg9 regime slot)
                    write_trade_open_log("XAUUSD", "RSIExtremeTurn",
                        g_rsi_extreme.pos.is_long ? "LONG" : "SHORT",
                        g_rsi_extreme.pos.entry, 0.0, g_rsi_extreme.pos.sl,
                        g_rsi_extreme.pos.size, ask - bid,
                        regime, "RSI_EXTREME_TURN");
                    g_telemetry.UpdateLastSignal("XAUUSD",
                        g_rsi_extreme.pos.is_long ? "LONG" : "SHORT",
                        g_rsi_extreme.pos.entry, "RSI_EXTREME_TURN",
                        "RSI_EXTREME", regime.c_str(), "RSI_EXTREME",
                        0.0, g_rsi_extreme.pos.sl);
                    if (!g_rsi_extreme.shadow_mode) {
                        send_live_order("XAUUSD",
                            g_rsi_extreme.pos.is_long,
                            g_rsi_extreme.pos.size,
                            g_rsi_extreme.pos.entry);
                        g_telemetry.UpdateLastEntryTs();
                    }
                }
            }
        }
    }

    //  Real-tick backtest: 4320 trades / 2yr, -$3.8k. Momentum = negative EV.

    // When stair step 1 banks the first 33% and arms a reload, try_reload()
    // is called every tick until it fires, cancels, or times out (5s).
    //
    // The reload enters a NEW fresh full-size position in the same direction

    // (LatencyEdge comment removed S13 Finding B 2026-04-24 — engine culled)
    // Full exclusion: checks ALL other gold engines to prevent stacking.
    // ?? TrendPullbackEngine position management -- ALWAYS runs when position open ??
    // CRITICAL: on_tick() handles SL/trail internally when pos_.active, but it was
    // only called inside the entry guard (!has_open_position()), so once a position
    // opened the manage path was NEVER reached ? SL never checked ? unmanaged trades.
    // Seed M15 bar EMAs into TrendPullback gold every tick -- mirrors SP/NQ pattern.
    // Seed M15 bar EMAs every tick from live bar accumulator.
    // Uses m15 (not m1): gold TrendPullback is a swing engine --
    // EMA9/21/50 half-lives of 47min/109min/260min are correct for the timeframe.
    // m1_ready=true after 14 M15 bars (3.5hr cold) or immediately on warm restart.
    if (g_bars_gold.m15.ind.m1_ready.load(std::memory_order_relaxed)) {
        g_trend_pb_gold.seed_bar_emas(
            g_bars_gold.m15.ind.ema9.load(std::memory_order_relaxed),
            g_bars_gold.m15.ind.ema21.load(std::memory_order_relaxed),
            g_bars_gold.m15.ind.ema50.load(std::memory_order_relaxed),
            g_bars_gold.m15.ind.atr14.load(std::memory_order_relaxed));
        // FIX 2026-04-02: seed M5 trend from M1 EMA crossover, not M5 swing pivot (15+ min lag)
        {
            const double tpb_ema9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
            const double tpb_ema50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
            const int tpb_trend = (tpb_ema9 > 0.0 && tpb_ema50 > 0.0)
                ? (tpb_ema9 < tpb_ema50 ? -1 : +1) : 0;
            g_trend_pb_gold.seed_m5_trend(tpb_trend);
        }
    }
    // H4 trend gate -- feeds HTF direction into TrendPullback gold entry filter.
    if (g_bars_gold.h4.ind.m1_ready.load(std::memory_order_relaxed)) {
        g_trend_pb_gold.seed_h4_trend(
            g_bars_gold.h4.ind.trend_state.load(std::memory_order_relaxed));
    }
    // H1/H4 engine status -> telemetry snap (every tick, lock-free)
    {
        auto* snap = g_telemetry.snap();
        if (snap) {
            snap->h1_swing_open      = g_h1_swing_gold.has_open_position() ? 1 : 0;
            snap->h4_regime_open     = g_h4_regime_gold.has_open_position() ? 1 : 0;
            snap->h1_swing_daily_pnl = static_cast<float>(g_h1_swing_gold.daily_pnl_);
            snap->h4_regime_daily_pnl= static_cast<float>(g_h4_regime_gold.daily_pnl_);
            snap->h1_swing_shadow    = g_h1_swing_gold.shadow_mode ? 1 : 0;
            snap->h4_regime_shadow   = g_h4_regime_gold.shadow_mode ? 1 : 0;
            snap->h1_adx = static_cast<float>(
                g_bars_gold.h1.ind.adx14.load(std::memory_order_relaxed));
            snap->h4_adx = static_cast<float>(
                g_bars_gold.h4.ind.adx14.load(std::memory_order_relaxed));
            snap->h4_trend_state = g_bars_gold.h4.ind.trend_state.load(
                std::memory_order_relaxed);
            // MinimalH4Breakout mirror fields
            snap->minimal_h4_open       = g_minimal_h4_gold.has_open_position() ? 1 : 0;
            snap->minimal_h4_daily_pnl  = static_cast<float>(g_minimal_h4_gold.daily_pnl_);
            snap->minimal_h4_shadow     = g_minimal_h4_gold.shadow_mode ? 1 : 0;
        }
    }
    // H1/H4 engine tick-level management -- on_tick() handles SL/TP/partial/trail.
    // on_h1_bar() / on_h4_bar() handle bar-level exits (EMA cross, ADX collapse, timeout).
    if (g_h1_swing_gold.has_open_position())
        g_h1_swing_gold.on_tick(bid, ask, now_ms_g, ca_on_close);
    if (g_h4_regime_gold.has_open_position()) {
        g_h4_regime_gold.on_tick(bid, ask, now_ms_g, ca_on_close);
        g_h4_regime_gold.check_weekend_close(bid, ask, now_ms_g, ca_on_close);
    }
    // MinimalH4Breakout tick management -- runs parallel to H4Regime, independent.
    if (g_minimal_h4_gold.has_open_position()) {
        g_minimal_h4_gold.on_tick(bid, ask, now_ms_g, ca_on_close);
        g_minimal_h4_gold.check_weekend_close(bid, ask, now_ms_g, ca_on_close);
    }
    // -- Improvement 5: CVD confirmation gate ------------------------------
    g_trend_pb_gold.seed_cvd(g_macro_ctx.gold_cvd_dir);

    // -- Improvement 1: Volatility regime scaling --------------------------
    // Feed rolling 20-bar ATR average so engine can detect vol regime.
    // Use M1 ATR as proxy -- OHLCBarEngine computes true range ATR14.
    // avg_atr20 approximated as EWM of atr14 with alpha=2/21.
    {
        static double s_atr_avg = 0.0;
        const double cur_atr = g_bars_gold.m15.ind.atr14.load(std::memory_order_relaxed);
        if (cur_atr > 0.0) {
            s_atr_avg = (s_atr_avg <= 0.0) ? cur_atr
                       : s_atr_avg + (2.0/21.0) * (cur_atr - s_atr_avg);
            g_trend_pb_gold.seed_vol_atr_avg(s_atr_avg);
        }
    }
    // -- Improvement 7: News proximity -------------------------------------
    g_trend_pb_gold.seed_news_secs(
        g_news_blackout.secs_until_next(static_cast<int64_t>(std::time(nullptr))));
    // TrendPullback gold position management -- always runs when position open
    if (g_trend_pb_gold.has_open_position()) {
        g_trend_pb_gold.on_tick("XAUUSD", bid, ask, ca_on_close);
    }

    // Trend Pullback: EMA9/21/50 -- only when no other XAUUSD position is open.
    // TrendPullback: M15 swing trades (1-3hr hold, 20-50pt targets).
    // different position sizes, independent SL/TP levels, no conflict.
    // Still blocked by bracket (was also LatencyEdge — culled S13 Finding B 2026-04-24)
    // that would directly conflict with a swing position at the same level).
    // GoldStack (tick-pattern engine) also blocked -- shares exact same entry zone.
    // TrendPullback gold -- re-enabled with tick-EMA-correct TP (ATR-based, not EMA9)
    // and widened pullback band (0.15% not 0.05%).
    // Does NOT require bar data -- runs on tick EMAs with proper time-equivalent alphas.
    // TrendPullback gold: 24h entry gate -- trend is a trend regardless of session.
    // Uses symbol_gate (risk/max_positions, tradeable, lat_ok, regime, bid, ask) but NOT session slot gate.
    // Only hard blocks: dead-zone spread spike window (05:00-06:30 UTC) and NY close noise.
    // TrendPullback session gate: London + NY only (slots 1,2,3,4,5).
    // Asia (slot 6) and dead-zone (slot 0): spreads wide, price ranges, no trend.
    // All trades in Asia were TIME_STOPs with gross=$0 -- no edge in that session.
    const bool tpb_gold_session_ok = !in_ny_close_noise
        && (g_macro_ctx.session_slot >= 1 && g_macro_ctx.session_slot <= 5);
    const bool tpb_gold_can_enter = tpb_gold_session_ok
                                 && symbol_gate("XAUUSD", gold_any_open, "", tradeable, lat_ok, regime, bid, ask)
                                 && !gold_post_impulse_block;

    // ?? CRASH CONTINUATION OVERRIDE ????????????????????????????????????????
    // When gold is crashing hard (>30pt in the last ~15min window), the
    // TrendPullback cooldown is bypassed if no position is open.
    // This catches second and third legs of crash moves that the 60s cooldown
    // (previously 900s) would still block.
    // Condition: gold has moved >30pt from recent high (60-tick window),
    //            RSI < 30 (oversold confirms direction), no open position,
    //            and we are not in the 10-min direction block.
    // Implementation: temporarily reset cooldown if conditions met.
    {
        const double rsi_now  = g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed);
        // Use gold stack EWM drift as crash proxy: drift < -5 = strong downtrend
        const double drift_now = g_gold_stack.ewm_drift();
        // FIX 2026-04-02: raised RSI thresholds 35->38, 65->62 -- consistent with full chain.
        const bool crash_mode = (drift_now < -5.0 && rsi_now < 38.0 && rsi_now > 0.0)
                             || (drift_now >  5.0 && rsi_now > 62.0);
        if (crash_mode
            && tpb_gold_can_enter
            && !g_trend_pb_gold.has_open_position()
            && !g_bracket_gold.has_open_position()
            && !g_gold_stack.has_open_position()) {
            // Force cooldown to expire so on_tick() can generate a signal
            g_trend_pb_gold.force_cooldown_expire();
            static int64_t s_crash_log = 0;
            if (nowSec() - s_crash_log > 30) {
                s_crash_log = nowSec();
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[CRASH-OVERRIDE] drift=%.2f RSI=%.1f -- TrendPB cooldown bypassed\n",                        drift_now, rsi_now);
                    std::cout << _msg;
                    std::cout.flush();
                }
            }
        }
    }

    if (tpb_gold_can_enter
        && !g_bracket_gold.has_open_position()
        && !g_gold_stack.has_open_position()
        // && !g_le_stack.has_open_position()  -- REMOVED S13 Finding B 2026-04-24
        && !g_trend_pb_gold.has_open_position()) {
        const auto tpb = g_trend_pb_gold.on_tick("XAUUSD", bid, ask, ca_on_close);
        if (tpb.valid) {
            const double gold_drift = g_gold_stack.ewm_drift();
            const bool drift_ok = (tpb.is_long  && gold_drift >= -1.0) ||
                                  (!tpb.is_long && gold_drift <=  1.0);
            if (!drift_ok) {
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[TRENDPB-GOLD] %s suppressed -- EWM drift=%.2f opposes direction\n",                        tpb.is_long ? "LONG" : "SHORT", gold_drift);
                    std::cout << _msg;
                    std::cout.flush();
                }
                g_trend_pb_gold.cancel();
            } else {
                const double tpb_lot = enter_directional("XAUUSD", tpb.is_long, tpb.entry, tpb.sl, tpb.tp, 0.01, true, bid, ask, sym, regime, "TrendPullbackGold");
                if (!tpb_lot) {
                    g_trend_pb_gold.cancel();
                } else {
                    // Patch pos_.size with actual risk-computed lot so shadow PnL is correct
                    // Without this: pos_.size=0.01 but PnL computed against internal size -> 100x inflation
                    g_trend_pb_gold.patch_size(tpb_lot);
                    g_telemetry.UpdateLastSignal("XAUUSD",
                        tpb.is_long ? "LONG" : "SHORT", tpb.entry, tpb.reason,
                        "TREND_PB", regime.c_str(), "TREND_PB",
                        tpb.tp, tpb.sl);
                }
            }
        }
    }

    // -- Improvement 8: Pyramid add-on on second EMA50 pullback -------------
    // When TrendPullback is live, check for second pullback directly from EMA state.
    // NEVER call on_tick() again while position is open -- that runs double management.
    // Instead: read EMA50 and check the same pullback+bounce condition manually.
    if (g_trend_pb_gold.PYRAMID_ENABLED
        && g_trend_pb_gold.has_open_position()
        && g_trend_pb_gold.pyramid_adds_ < g_trend_pb_gold.PYRAMID_MAX_ADDS
        && g_trend_pb_gold.daily_pnl() > -g_trend_pb_gold.DAILY_LOSS_CAP * 0.5
        && tpb_gold_can_enter) {
        const double pyr_mid    = (bid + ask) * 0.5;
        const double pyr_ema50  = g_trend_pb_gold.ema50();
        const double pyr_band   = pyr_mid * g_trend_pb_gold.PULLBACK_BAND_PCT / 100.0;
        const bool   pyr_long   = g_trend_pb_gold.open_is_long();
        const bool   at_ema50   = std::fabs(pyr_mid - pyr_ema50) < pyr_band;
        // Only add when: at EMA50, price bouncing in trade direction, already profitable (BE locked)
        const double open_entry = g_trend_pb_gold.open_entry();
        const bool   in_profit  = pyr_long ? (bid > open_entry) : (ask < open_entry);
        const bool   bouncing   = pyr_long ? (pyr_mid > pyr_ema50) : (pyr_mid < pyr_ema50);
        if (at_ema50 && bouncing && in_profit) {
            const double pyr_atr     = g_trend_pb_gold.current_atr();
            const double pyr_sl_dist = std::max(pyr_atr * g_trend_pb_gold.ATR_SL_MULT, 3.0);
            const double pyr_sl      = pyr_long ? (pyr_mid - pyr_sl_dist) : (pyr_mid + pyr_sl_dist);
            const double pyr_tp_dist = std::max(pyr_atr * 2.5, pyr_sl_dist * 2.0);
            const double pyr_tp      = pyr_long ? (pyr_mid + pyr_tp_dist) : (pyr_mid - pyr_tp_dist);
            // Fix: use open_size() (the actual patched lot sent to broker) as the base
            // for pyramid sizing. Hardcoded 0.01 caused add-ons to be sized independently
            // of the base trade -- if base was 0.08 lots, pyramid was calculated as if
            // base was 0.01, producing an inflated or mismatched add-on lot.
            const double base_open_lot = std::max(0.01, g_trend_pb_gold.open_size());
            const double add_lot     = std::max(0.005,
                compute_size("XAUUSD", pyr_sl_dist, ask - bid, base_open_lot)
                * g_trend_pb_gold.PYRAMID_SIZE_MULT);
            if (enter_directional("XAUUSD", pyr_long, pyr_mid, pyr_sl, pyr_tp, add_lot, true, bid, ask, sym, regime, "TrendPullbackGoldPyramid")) {
                ++g_trend_pb_gold.pyramid_adds_;
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[TRENDPB-GOLD] PYRAMID ADD #%d lot=%.4f sl=%.3f tp=%.3f\n",                        g_trend_pb_gold.pyramid_adds_, add_lot, pyr_sl, pyr_tp);
                    std::cout << _msg;
                    std::cout.flush();
                }
            }
        }
    }

    // ?? GoldHybridBracketEngine -- compression range -> both sides -> cancel loser ??
    // SHADOW mode by default. Fires when:
    //   1. Compression range detected (MIN_RANGE=1.5pt, MAX_RANGE=12pt over 30 ticks)
    // Both a long stop above hi AND a short stop below lo are sent simultaneously.
    // First fill becomes the position; the other order is cancelled via cancel_fn.
    {
        // Position management -- unconditional when live
        if (g_hybrid_gold.has_open_position()) {
            g_hybrid_gold.on_tick(bid, ask, now_ms_g,
                                  gold_can_enter, false, false, 0,
                                  bracket_on_close,
                                  g_macro_ctx.gold_slope,
                                  g_macro_ctx.gold_vacuum_ask,
                                  g_macro_ctx.gold_vacuum_bid,
                                  g_macro_ctx.gold_wall_above,
                                  g_macro_ctx.gold_wall_below,
                                  g_macro_ctx.gold_l2_real);
        }
        // New entry -- only when no other gold position open AND market is genuinely compressing.
        // vol_range gate: hybrid bracket needs real compression, not noise oscillation.
        // Require vol_range >= 2.0pt: confirms the market HAS been compressing recently.
        // FIX 2026-04-07: add bypass when vol_range is stale (bars frozen/cold-start).
        // vol_range=0 previously blocked ALL hybrid entries on startup -- even during
        // genuine 30pt moves. Now: bypass when macro displacement > $6 from VWAP OR
        // drift > 2.0 (unambiguous directional move regardless of bar state).
        // This is the "allow trades to happen" fallback -- if price is clearly moving
        // and compression range forms, let the hybrid bracket arm.
        const double hybrid_vol_range  = g_gold_stack.vol_range();
        const bool   hybrid_vol_unseeded = (hybrid_vol_range == 0.0);
        const double hybrid_vwap        = g_gold_stack.vwap();
        const double hybrid_mid         = (bid + ask) * 0.5;
        const double hybrid_vwap_disp   = (hybrid_vwap > 0.0)
            ? std::fabs(hybrid_mid - hybrid_vwap) : 0.0;
        // Macro bypass: strong move visible even without bar history
        const bool hybrid_macro_bypass  = (hybrid_vwap_disp >= 6.0)
                                       || (std::fabs(g_gold_stack.ewm_drift()) >= 2.0)
                                       || crash_impulse_bypass;
        // vol_ok: normal path OR unseeded-but-macro-move
        const bool hybrid_vol_ok = (hybrid_vol_range >= 0.5)
                                || (hybrid_vol_unseeded && hybrid_macro_bypass)
                                || (!hybrid_vol_unseeded && hybrid_macro_bypass);
        const bool hybrid_can_enter =
            gold_can_enter
            && hybrid_vol_ok
            && !g_bracket_gold.has_open_position()
            // && !g_le_stack.has_open_position()  -- REMOVED S13 Finding B 2026-04-24
            && !g_trend_pb_gold.has_open_position()
            && !g_nbm_gold_london.has_open_position();

        // FIX 2026-04-07: always call on_tick to feed the structure window unconditionally.
        // Previously on_tick was only called when hybrid_can_enter=true -- so when
        // ASIA-GATE blocked entries for the first 35min, m_window never received ticks,
        // m_ticks_received never reached MIN_ENTRY_TICKS=150, and range stayed 0.00
        // permanently. The engine was never able to arm even when gates opened.
        // Fix: call on_tick every tick. When can_enter=false the engine feeds the window
        // but does not transition IDLE->ARMED. When can_enter becomes true the window
        // is already full and range computes immediately on the next tick.
        // Position management path (has_open_position) handled unconditionally above.
        if (!g_hybrid_gold.has_open_position()) {
            g_hybrid_gold.on_tick(bid, ask, now_ms_g,
                                  hybrid_can_enter, false, false, 0,
                                  bracket_on_close,
                                  g_macro_ctx.gold_slope,
                                  g_macro_ctx.gold_vacuum_ask,
                                  g_macro_ctx.gold_vacuum_bid,
                                  g_macro_ctx.gold_wall_above,
                                  g_macro_ctx.gold_wall_below,
                                  g_macro_ctx.gold_l2_real);
        }

        // When hybrid transitions to PENDING, send both stop orders.
        // Use pending_lot computed by the engine (correct SL-based sizing).
        // Do NOT recompute lot here -- engine already applied risk/sl_dist formula.
        if (g_hybrid_gold.phase == omega::GoldHybridBracketEngine::Phase::PENDING
            && g_hybrid_gold.pending_long_clOrdId.empty()
            && g_hybrid_gold.pending_short_clOrdId.empty()) {
            const double h_hi  = g_hybrid_gold.bracket_high;
            const double h_lo  = g_hybrid_gold.bracket_low;
            const double h_lot = g_hybrid_gold.pending_lot;
            if (h_hi > 0.0 && h_lo > 0.0 && h_lot >= 0.01 && g_cfg.mode == "LIVE") {
                // Wire cancel_fn once (idempotent -- already set on re-entry but safe to set again)
                g_hybrid_gold.cancel_fn = [](const std::string& id) { send_cancel_order(id); };
                const std::string h_long_id  = send_live_order("XAUUSD", true,  h_lot, h_hi);
                const std::string h_short_id = send_live_order("XAUUSD", false, h_lot, h_lo);
                g_hybrid_gold.pending_long_clOrdId  = h_long_id;
                g_hybrid_gold.pending_short_clOrdId = h_short_id;
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[HYBRID-GOLD] ORDERS SENT long_id=%s short_id=%s "                        "hi=%.2f lo=%.2f range=%.2f lot=%.3f\n",                        h_long_id.c_str(), h_short_id.c_str(),                        h_hi, h_lo, g_hybrid_gold.range, h_lot);
                    std::cout << _msg;
                    std::cout.flush();
                }
            }
        }
    }

    // ?? NBM London position management -- ALWAYS runs when position open ??
    // entry guard, so _manage_position() (SL/VWAP trail) was never reached once a
    // position was open. Fix: call on_tick unconditionally when position is open.
    // on_tick returns {} immediately after _manage_position() when pos_.active.
    if (g_nbm_gold_london.has_open_position()) {
        g_nbm_gold_london.on_tick(sym, bid, ask, ca_on_close);
    }

    // ?? NBM London session (07:00-13:30 UTC) on XAUUSD ???????????????????
    // Runs independently of the gold stack/flow/bracket -- pure momentum
    // engine using London open as session anchor. Gated: no other gold pos.
    // Walk-forward sweep (96 cells, 14 days of L2 data, T=116 on production
    // params i=0.05 p=5 London+NY) showed no edge at any threshold x
    // persist_ticks x session_filter combination. Production cell WR=22%,
    // net -$47 over 14 days, MaxDD=$65. Higher thresholds (>=0.08) starved
    // the signal to T<=2 per 14 days -- untradeable. Original block was
    // ~120 lines covering seed_bar_atr, position management, chop gate, EWM
    // drift gate, HTF hard-block, entry dispatch, and open-log emission.
    // Evidence: backtest/dpe_sweep/summary.csv, leaderboard_oos.csv.
    // was sub-noise for current cTrader L2 depth regime).

    // 11-day / 3.4M tick full-L2 sweep: T=4469 WR=20.8% PnL=-$12,770.36
    // 576-config grid sweep found ZERO profitable configurations.
    // Best tested: T=222 WR=36.0% PnL=-$336.39 (still a loser).
    // and globals.hpp tombstone for full evidence.

    // -- PDH/PDL Reversion Engine --------------------------------------------
    // Mean reversion inside yesterday's daily range.
    // Research: 2yr/111M tick backtest -- only statistically valid intraday edge.
    {
        const int64_t pdhl_ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto pdhl_cb = [](const omega::TradeRecord& tr){ g_omegaLedger.record(tr); };
        g_pdhl_rev.on_tick(
            bid, ask, pdhl_ts_ms,
            g_macro_ctx.pdh,
            g_macro_ctx.pdl,
            gf_atr_gate,
            g_macro_ctx.gold_l2_imbalance,
            g_l2_gold.raw_bid.load(std::memory_order_relaxed),
            g_l2_gold.raw_ask.load(std::memory_order_relaxed),
            g_macro_ctx.gold_l2_real,
            static_cast<double>(g_gold_stack.ewm_drift()),
            g_macro_ctx.session_slot,
            pdhl_cb
        );
    }

    // 11-day / 3.4M tick full-L2 sweep: T=285 WR=24.6% PnL=-$1171.82 MaxDD=$1679.
    // 288-config grid sweep found NO config with WR >= 40%. Best-tuned config
    // globals.hpp tombstone for full evidence.
    //  Engine itself REMOVED -- 6-day sweep / 1.5M ticks showed no edge.
    // 41-cell walk-forward REENTER_COMP sweep showed no profitable
    // configuration on 9 days of XAUUSD tick data (train 04-13..17 /
    // test 04-20..23). See globals.hpp tombstone for full evidence.

    // -- CandleFlowEngine -- RESTORED 2026-04-29 with audit-tightened gates --
    // Re-enabled in shadow_mode after S19 cull. The cull was driven by a
    // 576-config sweep on the same flawed structure. The 2026-04-29 audit
    // sliced 445 actual CFE trades and found PARTIAL_TP + TRAIL_SL exits
    // returned +$2,076 across 142 trades at 96% WR -- the winning logic is
    // intact. Bleed concentrated in noise-hour overtrading. New gates
    // (CFE_TOD_BLOCKED, HMM gating live, tighter persist+thresh) restrict
    // entries to hours where edge actually exists. Validate with N>=30
    // shadow trades AND multi-month backtest before considering LIVE.
    // -- CandleFlowEngine -- candle structure + cost coverage + DOM entry/exit --
    // Manage always (position already open)
    if (g_candle_flow.has_open_position()) {
        // Build current DOM snapshot from L2Book
        // cfe_prev_book_mgmt is static so it persists between ticks --
        // required for imbalance_level() delta to work correctly in build_dom().
        static L2Book cfe_prev_book_mgmt;
        L2Book cfe_book;
        {
            std::lock_guard<std::mutex> lk(g_l2_mtx);
            auto it = g_l2_books.find("XAUUSD");
            if (it != g_l2_books.end()) cfe_book = it->second;
        }
        L2Book& cfe_prev_book = cfe_prev_book_mgmt;
        const auto cfe_dom = omega::CandleFlowEngine::build_dom(
            cfe_book, cfe_prev_book, (bid + ask) * 0.5);
        cfe_prev_book_mgmt = cfe_book;  // advance prev for next tick
        // Management call: bar snap not needed (CFE only uses bar for new entries,
        // manage() path ignores bar entirely). Dummy bar with valid=false is correct.
        // ewm_drift MUST be live -- sustained-drift tracker and DFE persist state
        // run unconditionally every tick including management ticks. Passing 0.0
        // corrupted m_drift_sustained_dir and m_dfe_persist_ticks on every managed tick.
        // Fixed: pass g_gold_stack.ewm_drift() to both management and entry calls.
        const omega::CandleFlowEngine::BarSnap cfe_bar_dummy{};  // valid=false: correct for manage path
        // ATR floor: never let CFE use an ATR below 2.0pt from GoldFlow.
        // If current_atr() returns a transient near-zero value (e.g. after
        // GoldFlow force-close and re-warm), the fallback of 5.0 only catches
        // zero. A value like 0.86pt passes the > 0.0 check and produces
        // sl=0.60pt, size=0.50 lots (MAX_LOT cap) -- catastrophic on wide spread.
        // Floor of 2.0pt: sl=1.40pt, max size=0.214 lots at $30 risk.
        // 3-way ATR: max(GoldFlow EWM ATR, M1 bar ATR, 2.0pt floor).
        // GoldFlow EWM lags session transitions (Asia->London = 30-60min lag).
        // M1 bar ATR14 responds to actual candle ranges within 1-2 bars.
        // At London open: GoldFlow may read 1pt (overnight), M1 reads 4-6pt.
        // 2.0pt absolute floor: sl=1.4pt, max size=0.214 lots at $30 risk.
        const double cfe_m1_atr  = g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed);
        // (GoldFlow-related code removed S19 Stage 1B — engine culled)
        // cfe_gf_atr removed with GoldFlow. CFE now uses max(2.0pt floor, M1 bar ATR14).
        const double cfe_atr     = std::max(2.0, cfe_m1_atr);
        g_candle_flow.on_tick(bid, ask, cfe_bar_dummy, cfe_dom,
            now_ms_g, cfe_atr,
            [&](const omega::TradeRecord& tr) {
                handle_closed_trade(tr);
                if (!g_candle_flow.shadow_mode)
                    send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
            },
            g_gold_stack.ewm_drift(),   // FIX: was 0.0 -- corrupted sustained-drift tracker
            g_bars_gold.m1.ind.tick_rate.load(std::memory_order_relaxed)); // HMM tick_rate
    }

    // Entry: only when no other gold position open and gold_can_enter passes
    // STARTUP LOCKOUT: block CFE entries for 90s after process start.
    // Root cause of three consecutive FC losses (03:50, 02:58, 01:40):
    // DFE fires within seconds of restart because:
    //   1. ewm_drift cold-starts at 0, first ticks spike it to 1.5+ instantly
    //   2. RSI warms in ~30 ticks but reflects only the startup price movement
    // Both signals are artificial artifacts of cold-start, not real market state.
    // 90s gives ewm_drift (alpha=0.05 fast EWM) ~18 ticks to settle and
    // RSI 30+ ticks on real price action before any DFE entry is allowed.
    // The 90s block also prevents entries during the VERIFY_STARTUP window.
    static int64_t s_cfe_startup_ms = 0;
    if (s_cfe_startup_ms == 0) s_cfe_startup_ms = now_ms_g;
    const bool cfe_startup_locked = (now_ms_g - s_cfe_startup_ms) < 90000LL;
    if (cfe_startup_locked) {
        static int64_t s_cfe_lock_log = 0;
        if (now_ms_g - s_cfe_lock_log > 15000) {
            s_cfe_lock_log = now_ms_g;
            const int secs_remaining = static_cast<int>(
                (90000LL - (now_ms_g - s_cfe_startup_ms)) / 1000LL);
            {
                char _msg[512];
                snprintf(_msg, sizeof(_msg), "[CFE-STARTUP-LOCK] entries blocked %ds remaining (cold-start warmup)\n",                    secs_remaining);
                std::cout << _msg;
                std::cout.flush();
            }
        }
    }
    // CFE TOD dead zone: block entries 00:00-01:30 UTC.
    // All 5 consecutive losses on 2026-04-14 were in the 00:15-00:31 UTC window.
    // Lowest-liquidity window on gold: Sydney thin tape, no NY/London flow.
    const bool cfe_tod_dead_zone = [&]() -> bool {
        const int64_t cfe_tod_utc_sec  = now_ms_g / 1000LL;
        const int     cfe_tod_utc_hour = static_cast<int>((cfe_tod_utc_sec % 86400LL) / 3600LL);
        const int     cfe_tod_utc_min  = static_cast<int>((cfe_tod_utc_sec % 3600LL) / 60LL);
        const int     cfe_tod_mins     = cfe_tod_utc_hour * 60 + cfe_tod_utc_min;
        return (cfe_tod_mins >= 0 && cfe_tod_mins < 90);  // 00:00-01:30 UTC
    }();
    if (cfe_tod_dead_zone) {
        static int64_t s_cfe_tod_log = 0;
        if (now_ms_g - s_cfe_tod_log > 60000) {
            s_cfe_tod_log = now_ms_g;
            {
                char _msg[512];
                snprintf(_msg, sizeof(_msg), "[CFE-TOD-DEAD] entries blocked: 00:00-01:30 UTC dead zone\n");
                std::cout << _msg;
                std::cout.flush();
            }
        }
    }

    // -- EMACrossEngine -- sweep-confirmed 2026-04-16 -----------------------
    // 6-day / 1.5M tick sweep: fast=9 slow=15 rsi_lo=40 rsi_hi=50 sl=1.5 rr=1.0
    // 99 trades, 46.5% WR, $402.30 / 6 days = $67/day. Shadow mode.
    if (g_ema_cross.has_open_position()) {
        g_ema_cross.on_tick(bid, ask, now_ms_g,
            [&](const omega::TradeRecord& tr) { handle_closed_trade(tr); });
    }
    // FIX 2026-04-22: HTF hard-block for EMACross entry.
    // EMACross is trend-following (9/15 crossover); valid crossovers align
    // with short-term drift direction. ewm_drift sign is a reliable proxy.
    const bool ece_htf_long_intent = (g_gold_stack.ewm_drift() > 0.0);
    const bool ece_htf_blocked = g_htf_filter.bias_opposes("XAUUSD", ece_htf_long_intent);
    if (ece_htf_blocked) {
        static thread_local int64_t s_ece_htf_log = 0;
        if (now_ms_g - s_ece_htf_log > 30000) {
            s_ece_htf_log = now_ms_g;
            char _msg[512];
            snprintf(_msg, sizeof(_msg),
                "[HTF-BLOCK] XAUUSD ECE %s skipped -- HTF bias=%s opposes\n",
                ece_htf_long_intent ? "LONG" : "SHORT",
                g_htf_filter.bias_name("XAUUSD"));
            std::cout << _msg; std::cout.flush();
        }
    }

}

