// tick_gold.hpp — per-symbol tick handlers
// Extracted from on_tick(). Same translation unit — all static functions visible.

// ── XAUUSD ─────────────────────────────────────────────────
static void on_tick_gold(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime,
        double rtt_check)
{
    // ?? Gold master exclusion gate ????????????????????????????????????????
    // Default: ANY open gold position blocks new entries (1-at-a-time invariant).
    // TREND DAY exception: when |ewm_drift| > 5.0 AND vol_ratio > 1.5,
    //   allow CompBreakout to re-enter within 90s of GoldFlow exiting (trail/BE).
    //   CompBreakout is the safest re-entry -- tight $3 SL, EWM drift gated.
    //   Bracket can also arm (not fire) in parallel with GoldFlow.
    // REVERSAL exception: GoldFlow SL_HIT + drift now in opposite direction
    //   ? GoldStack counter-entry allowed within 60s (bypasses SL cooldown).
    // GoldFlow open: only block other engines if GoldFlow is NOT in profit or
    // is in early stage (stage=0, no trail yet). If GoldFlow is trailing (stage>=1)
    // on a winner, allow GoldStack to add a position in the same direction.
    // ATR-proportional gate variables -- computed once at function entry,
    // used by dead-zone lambda, crash_impulse_bypass, asia_crash_bypass,
    // counter-trend gate, GoldStack MR block, and lot scaling below.
    // Must be defined before any lambda that captures them by reference.
    const double gf_atr_gate       = std::max(2.0, g_gold_flow.current_atr());
    const double rsi_atr_scale     = std::min(3.0, std::max(0.5, gf_atr_gate / 5.0));
    const double rsi_crash_lo      = 50.0 - 6.0 * rsi_atr_scale;
    const double rsi_crash_hi      = 50.0 + 6.0 * rsi_atr_scale;
    const double drift_bypass_thresh = std::max(1.0, 0.3 * gf_atr_gate);

    const bool gf_open = g_gold_flow.has_open_position();
    const double gf_mid = (bid + ask) * 0.5;
    const bool gf_winning = gf_open
        && g_gold_flow.pos.trail_stage >= 2  // must be at stage 2 trail -- well past BE
        && (g_gold_flow.pos.is_long
            ? (gf_mid > g_gold_flow.pos.entry + 5.0)   // $5+ in profit before stack joins
            : (gf_mid < g_gold_flow.pos.entry - 5.0));
    // GoldStack winning: allow bracket pyramid when GoldStack has a live profitable position
    // ($10+ move = trail armed, same direction). Mirrors gf_winning logic for GoldFlow.
    const bool gs_open = g_gold_stack.has_open_position();
    const bool gs_winning = gs_open && g_gold_stack.has_profitable_trail();
    const bool gold_any_open =
        (gs_open && !gs_winning)                ||  // GoldStack blocks unless profitable trail
        g_le_stack.has_open_position()          ||
        g_bracket_gold.has_open_position()      ||
        (gf_open && !gf_winning)                ||  // GoldFlow blocks unless it's a confirmed winner
        g_trend_pb_gold.has_open_position()     ||
        g_nbm_gold_london.has_open_position();  // London NBM also blocks other gold engines

    // Write GoldFlow state to telemetry for GUI pyramid indicator
    {
        auto* snap = g_telemetry.snap();
        if (snap) {
            snap->gf_trail_stage    = gf_open ? g_gold_flow.pos.trail_stage : 0;
            snap->gf_stack_unlocked = gf_winning ? 1 : 0;
            snap->gf_atr_at_entry   = gf_open ? g_gold_flow.pos.atr_at_entry : 0.0;
            if (gf_open) {
                const double gf_move = g_gold_flow.pos.is_long
                    ? (gf_mid - g_gold_flow.pos.entry)
                    : (g_gold_flow.pos.entry - gf_mid);
                snap->gf_profit_usd = gf_move * g_gold_flow.pos.size * 100.0;
            } else {
                snap->gf_profit_usd = 0.0;
            }
        }
    }

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
        static int64_t s_impulse_since   = 0;   // when IMPULSE started
        static int     s_impulse_ticks   = 0;   // consecutive IMPULSE ticks
        const bool is_impulse_now = (std::strcmp(gold_stack_regime, "IMPULSE") == 0);
        const int64_t now_pi = static_cast<int64_t>(std::time(nullptr));
        if (is_impulse_now) {
            if (!s_was_impulse) { s_impulse_since = now_pi; s_impulse_ticks = 0; }
            ++s_impulse_ticks;
        } else {
            s_impulse_ticks = 0;
        }
        if (s_was_impulse && !is_impulse_now) {
            // Only block if IMPULSE lasted >= 8 ticks -- raised from 3.
            // At ~1 tick/sec, 3 ticks = 3s which fires on every micro-impulse flicker.
            // 8 ticks = ~8s = a genuine sustained impulse move.
            // Log showed 422 impulse_block=1 events today blocking all bracket entries.
            // Cooldown reduced 45s -> 20s: shorter re-arm window after real impulses.
            if (s_impulse_ticks >= 8) {
                g_gold_post_impulse_until.store(now_pi + 20);
                printf("[POST-IMPULSE] Regime left IMPULSE after %d ticks -- blocking 20s\n",
                       s_impulse_ticks);
                fflush(stdout);
            } else {
                printf("[POST-IMPULSE] Regime flicker (%d ticks) -- no block\n",
                       s_impulse_ticks);
                fflush(stdout);
            }
        }
        s_was_impulse = is_impulse_now;
        // Expose impulse stability for GoldFlow gate below
        // IMPULSE is only "confirmed" after 3+ consecutive ticks (~3s).
        // One-tick IMPULSE ghosts (regime flips back next tick) caused false
        // entries -- e.g. LONG into a downtrend when a micro-uptick triggered
        // IMPULSE for a single tick before supervisor corrected to COMPRESSION.
        (void)s_impulse_since;
        // Store tick count in a global so GoldFlow gate can check it
        g_gold_impulse_ticks.store(s_impulse_ticks);
    }
    const bool gold_post_impulse_block = (g_gold_post_impulse_until.load() >
        static_cast<int64_t>(std::time(nullptr)));

    // ?? Reversal window check ?????????????????????????????????????????????
    // Active when GoldFlow was SL_HIT and drift has now reversed direction.
    const int64_t now_s_gate = static_cast<int64_t>(std::time(nullptr));
    const bool in_reversal_window = (g_gold_reversal_window_until.load() > now_s_gate);
    const int  flow_exit_dir      = g_gold_flow_exit_dir.load();
    // Block reversal window if gold is in IMPULSE regime -- a SL in IMPULSE often
    // means the move is still going, not reversing. Counter-entries here cause big losses
    // (11:26 SHORT -$98 fired into 4539->4577 LONG impulse after 11:22 BE_HIT).
    const bool impulse_blocks_reversal = (std::strcmp(gold_stack_regime, "IMPULSE") == 0);
    // Reversal confirmed: drift now points OPPOSITE to the failed flow direction
    const bool drift_reversed = in_reversal_window && !impulse_blocks_reversal && (
        (flow_exit_dir ==  1 && gold_ewm_drift_now < -2.0)  // was long, now bearish drift
     || (flow_exit_dir == -1 && gold_ewm_drift_now >  2.0)  // was short, now bullish drift
    );
    if (drift_reversed) {
        // Tell GoldStack to bypass its SL cooldown this tick
        g_gold_stack.clear_sl_cooldown();
        static int64_t s_rev_log = 0;
        if (now_s_gate - s_rev_log > 10) {
            s_rev_log = now_s_gate;
            printf("[GOLD-REVERSAL] Drift reversed -- GoldStack cooldown cleared for counter-entry\n");
            fflush(stdout);
        }
    }

    // ?? Trend-day re-entry: allow CompBreakout after GoldFlow trail exit ??
    // If GoldFlow closed via trail/BE (not SL), price may still be trending.
    // CompBreakout can re-enter within 90s IF trend direction matches.
    const int64_t flow_exit_ts    = g_gold_flow_exit_ts.load();
    const int     flow_exit_reason = g_gold_flow_exit_reason.load();  // 2=trail/BE
    const bool trend_reentry_ok = gold_trend_day
        && !g_gold_flow.has_open_position()   // flow must be closed
        && !g_bracket_gold.has_open_position()
        && !g_le_stack.has_open_position()
        && !g_trend_pb_gold.has_open_position()
        && (flow_exit_reason == 2)            // trail/BE exit, not SL
        && (now_s_gate - flow_exit_ts <= 90); // within 90s of flow closing

    // Gold trades 24h. session_start_utc=0 session_end_utc=0 = 24h mode.
    // No dead zone. No session block. Spread + ATR quality gates handle thin tape.
    const int gold_session_slot = g_macro_ctx.session_slot;
    const bool gold_session_ok  = true;  // 24h -- no time-based blocks
    // On trend day re-entry: allow GoldStack even with gold_stack open position
    // (CompBreakout won't fire if stack is already open -- handled in stack gate below)
    // Same-direction trail block: 30s after a trail/BE exit, block re-entry in same dir.
    // Direction-aware: only blocks the direction that just closed, not the opposite.
    // This allows reversal entries but prevents immediate same-dir chasing.
    const bool gold_trail_blocked = (g_gold_trail_block_until.load() > now_s_gate);
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
    const bool gold_can_enter = gold_session_ok && symbol_gate("XAUUSD", gold_any_open, "", tradeable, lat_ok, regime, bid, ask)
                             && (!gold_post_impulse_block || crash_impulse_bypass);
    // Trend re-entry path bypasses gold_any_open for CompBreakout specifically
    const bool gold_can_enter_trend_reentry = gold_trend_day && trend_reentry_ok
        && gold_session_ok && symbol_gate("XAUUSD", false, "", tradeable, lat_ok, regime, bid, ask);

    // Run supervisor -- uses g_eng_xag as vol/phase proxy since gold has
    // its own GoldStack (not a BreakoutEngine). We use a dedicated gold
    // BreakoutEngine-based vol state if available, otherwise use silver as proxy.
    // For gold we track phase/vol from the bracket engine's internal data.
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
    // Dollar-native VWAP deviation -- passed to GoldFlow trend bias filter.
    // Positive = price above VWAP, negative = price below VWAP, in raw gold points.
    const double gold_vwap_pts = (gold_vwap_now > 0.0) ? (gold_mid_now - gold_vwap_now) : 0.0;
    const bool gold_is_compressing  = (strcmp(gold_stack_regime,"COMPRESSION")==0);

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
    // gold_sdec now available. Must be after supervisor update.
    // PROBLEM (2026-04-02): after 04:24 all positions closed. gold_can_enter=false
    // via gold_any_open exclusivity for 109 MINUTES while price fell 87 more pts.
    // FIX: when velocity_active + no actual open >5min + conf>1.0, bypass exclusivity.
    {
        const double gf_vel_ratio_p4   = (g_gold_stack.recent_vol_pct() > 0.0
                                       && g_gold_stack.base_vol_pct() > 0.0)
            ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 0.0;
        const bool gf_expansion_p4     = (gold_sdec.regime == omega::Regime::EXPANSION_BREAKOUT
                                       || gold_sdec.regime == omega::Regime::TREND_CONTINUATION);
        const bool velocity_active_gate = gf_expansion_p4 && (gf_vel_ratio_p4 > 2.5);

        const bool actually_no_open    = !g_gold_flow.has_open_position()
                                      && !g_gold_flow_reload.has_open_position()
                                      && !g_gold_stack.has_open_position()
                                      && !g_bracket_gold.has_open_position()
                                      && !g_le_stack.has_open_position()
                                      && !g_trend_pb_gold.has_open_position();

        const int64_t last_gf_close    = g_gold_flow_exit_ts.load();
        const int64_t now_p4           = static_cast<int64_t>(std::time(nullptr));
        const bool    no_pos_5min      = actually_no_open
                                      && (last_gf_close > 0)
                                      && ((now_p4 - last_gf_close) > 300);

        const bool vel_reentry_bypass  = velocity_active_gate
                                      && no_pos_5min
                                      && (gold_sdec.confidence > 1.0)
                                      && gold_session_ok
                                      && (!gold_post_impulse_block || crash_impulse_bypass);

        if (vel_reentry_bypass && !gold_can_enter) {
            static int64_t s_vel_reentry_log = 0;
            if (now_p4 - s_vel_reentry_log >= 30) {
                s_vel_reentry_log = now_p4;
                printf("[GF-VEL-REENTRY] Exclusivity bypass vol_ratio=%.2f conf=%.2f "
                       "no_pos_for=%lldsec regime=%s -- GoldFlow re-entry ALLOWED\n",
                       gf_vel_ratio_p4, gold_sdec.confidence,
                       (long long)(now_p4 - last_gf_close),
                       omega::regime_name(gold_sdec.regime));
                fflush(stdout);
            }
        }
        g_gf_vel_reentry_bypass.store(vel_reentry_bypass ? 1 : 0);
    }

    // Diagnostic: log vol_ratio every 30s so we can verify real data is flowing
    {
        static int64_t s_last_vol_log = 0;
        const int64_t now_ms_vl = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (now_ms_vl - s_last_vol_log >= 30000) {
            s_last_vol_log = now_ms_vl;
            const double vr = (gold_base_vol > 0.0) ? gold_recent_vol / gold_base_vol : 0.0;
            printf("[GOLD-VOL] regime=%s recent_vol=%.4f%% base_vol=%.4f%% ratio=%.3f"
                   " sdec_regime=%s conf=%.3f allow_bo=%d\n",
                   gold_stack_regime, gold_recent_vol, gold_base_vol, vr,
                   omega::regime_name(gold_sdec.regime), gold_sdec.confidence,
                   (int)gold_sdec.allow_breakout);
            fflush(stdout);
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

        // ?? Confidence threshold ??????????????????????????????????????????
        const bool conf_ok = (gold_sdec.confidence >= 0.45);

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

        const bool stack_can_enter = gold_sdec.allow_breakout
                                     && !g_bracket_gold.has_open_position()
                                     && !g_gold_flow.has_open_position()
                                     && !g_trend_pb_gold.has_open_position()
                                     && vol_expanding
                                     && conf_ok;

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
        const bool stack_can_enter_mr =
            in_ranging_regime
            && !g_bracket_gold.has_open_position()
            && !g_gold_flow.has_open_position()
            && !g_trend_pb_gold.has_open_position()
            && (gold_sdec.confidence >= 0.30);

        // Trend-day re-entry: allow CompBreakout specifically when GoldFlow
        // closed via trail/BE on a strong trend. Stack handles the EWM drift
        // direction gate internally so only trend-aligned signals fire.
        // Also allow reversal counter-entry when drift_reversed is confirmed.
        // Trail block: 30s after same-direction close, check if this signal
        // would re-enter the same direction. Allow if direction differs (reversal).
        const bool gs_trail_dir_match = gs_trail_blocked; // directional check done in GFE
        const bool stack_enter_effective = ((stack_can_enter && gold_can_enter && !gs_trail_dir_match && !in_ny_close_noise)
            || (gold_can_enter_trend_reentry && vol_expanding && !gs_trail_dir_match && !in_ny_close_noise)
            || (drift_reversed && gold_can_enter && !in_ny_close_noise)          // reversals also blocked at NY close
            || (stack_can_enter_mr && gold_can_enter && !in_ny_close_noise));    // MR/range engines in compression
        // Pass DX.F mid for DXYDivergenceEngine -- g_macroDetector.updateDXY()
    // is called every DX.F tick so this is fresh or 0.0 if feed not yet seen.
    const double dx_mid_now = g_macroDetector.dxyMid();
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
                    printf("[GS-BAR-BLOCK] XAUUSD %s %s blocked RSI=%.1f trend=%+d\n",
                           gsig.is_long ? "LONG" : "SHORT", gsig.engine,
                           bar_rsi_gs, bar_trend_gs);
                    fflush(stdout);
                }
            }
            if (!gs_bar_blocked) {
            // ?? VWAP counter-trend guard ??????????????????????????????????
            // Only block MEAN-REVERSION engines trading against VWAP trend.
            // Trend-following engines (CompressionBreakout, ImpulseContinuation,
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

            if (!direction_ok) {
                printf("[GOLD-STACK-BLOCKED] %s counter-trend mid=%.2f vwap=%.2f"
                       " vol_ratio=%.3f eng=%s\n",
                       gsig.is_long ? "LONG" : "SHORT",
                       gold_mid_now, gold_vwap_now, gold_vol_ratio, gsig.engine);
                fflush(stdout);
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
                    printf("[SEAS-SIZE] bucket t=%.1f -> lot_mult=%.2fx\n",
                           tstat * 10.0, seas_mult);
                    fflush(stdout);
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
                    double gs_xa_mult = 1.0;
                    // DXY momentum
                    const double dxy_ret = g_macroDetector.dxyReturn();
                    const double dxy_thr = g_macroDetector.DXY_RISK_OFF_PCT / 100.0;
                    if (gs_is_long  && dxy_ret >  dxy_thr) gs_xa_mult *= 0.70;
                    if (!gs_is_long && dxy_ret < -dxy_thr) gs_xa_mult *= 0.70;
                    // HTF bias
                    gs_xa_mult *= g_htf_filter.size_scale("XAUUSD", gs_is_long);
                    // Macro: don't short gold in RISK_OFF
                    if (g_macro_ctx.regime == "RISK_OFF" && !gs_is_long) gs_xa_mult *= 0.60;
                    gs_xa_mult = std::max(0.30, std::min(1.20, gs_xa_mult));
                    if (gs_xa_mult != 1.0)
                        printf("[XA-FILTER] XAUUSD STACK %s mult=%.2f DXY=%.4f HTF=%s macro=%s\n",
                               gs_is_long?"LONG":"SHORT", gs_xa_mult, dxy_ret,
                               g_htf_filter.bias_name("XAUUSD"), g_macro_ctx.regime.c_str());
                    gold_lot = std::max(0.01, std::min(gold_lot * gs_xa_mult, g_cfg.max_lot_gold));
                }
                // Max loss per trade dollar cap (gold stack bypasses enter_directional)
                if (g_cfg.max_loss_per_trade_usd > 0.0 && gold_sl_abs > 0.0) {
                    const double max_loss_lot = g_cfg.max_loss_per_trade_usd / (gold_sl_abs * 100.0);
                    if (gold_lot > max_loss_lot) {
                        const double capped = std::max(0.01, std::floor(max_loss_lot * 100.0 + 0.5) / 100.0);
                        printf("[MAX-LOSS-CAP] XAUUSD lot capped %.4f?%.4f (sl=$%.2f max=$%.0f)\n",
                               gold_lot, capped, gold_sl_abs * 100.0 * gold_lot, g_cfg.max_loss_per_trade_usd);
                        gold_lot = capped;
                    }
                }
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
        static OHLCBar s_cur1{}, s_cur5{}, s_cur15{}, s_cur_h4{};
        static int64_t s_bar1_ms = 0, s_bar5_ms = 0, s_bar15_ms = 0, s_bar_h4_ms = 0;
        const int64_t b1  = (now_ms_g /   60000LL) *   60000LL;
        const int64_t b5  = (now_ms_g /  300000LL) *  300000LL;
        const int64_t b15 = (now_ms_g /  900000LL) *  900000LL;
        const int64_t bh4 = (now_ms_g / 14400000LL) * 14400000LL;  // 4h = 14400s
        // M1
        if (s_bar1_ms == 0) { s_cur1 = {b1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar1_ms = b1; }
        else if (b1 != s_bar1_ms) { g_bars_gold.m1.add_bar(s_cur1); s_cur1 = {b1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar1_ms = b1; }
        else { if(xau_mid>s_cur1.high)s_cur1.high=xau_mid; if(xau_mid<s_cur1.low)s_cur1.low=xau_mid; s_cur1.close=xau_mid; }
        // M5
        if (s_bar5_ms == 0) { s_cur5 = {b5/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar5_ms = b5; }
        else if (b5 != s_bar5_ms) { g_bars_gold.m5.add_bar(s_cur5); s_cur5 = {b5/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar5_ms = b5; }
        else { if(xau_mid>s_cur5.high)s_cur5.high=xau_mid; if(xau_mid<s_cur5.low)s_cur5.low=xau_mid; s_cur5.close=xau_mid; }
        // M15
        if (s_bar15_ms == 0) { s_cur15 = {b15/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar15_ms = b15; }
        else if (b15 != s_bar15_ms) { g_bars_gold.m15.add_bar(s_cur15); s_cur15 = {b15/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar15_ms = b15; }
        else { if(xau_mid>s_cur15.high)s_cur15.high=xau_mid; if(xau_mid<s_cur15.low)s_cur15.low=xau_mid; s_cur15.close=xau_mid; }
        // H4 -- HTF regime gate for TrendPullback gold
        // 14 H4 bars = 56 hours to warm cold. trend_state drives seed_h4_trend().
        if (s_bar_h4_ms == 0) { s_cur_h4 = {bh4/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h4_ms = bh4; }
        else if (bh4 != s_bar_h4_ms) { g_bars_gold.h4.add_bar(s_cur_h4); s_cur_h4 = {bh4/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h4_ms = bh4; }
        else { if(xau_mid>s_cur_h4.high)s_cur_h4.high=xau_mid; if(xau_mid<s_cur_h4.low)s_cur_h4.low=xau_mid; s_cur_h4.close=xau_mid; }
    }

    // ?? VPIN toxicity tracker -- updated every XAUUSD tick ????????????????
    // Feeds g_vpin for GoldFlow pre-entry gate. Unit-volume Lee-Ready classification.
    g_vpin.on_tick(xau_mid, now_ms_g);
    // ?? Correlation matrix feed -- XAUUSD ??????????????????????????????????
    g_corr_matrix.on_price("XAUUSD", xau_mid);

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
        //   is underway regardless of book balance. L2 still gates inside GoldFlow
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
        gold_trend.update_l2(g_macro_ctx.gold_l2_imbalance, now_ms_g);
        const bool gold_trend_blocked = gold_trend.counter_trend_blocked(now_ms_g);

        // ?? GoldFlow bias injection ???????????????????????????????????????
        // BracketTrendState normally requires 2 consecutive bracket wins to set bias.
        // On a fresh session with only GoldFlow trades, bias stays 0 permanently
        // so pyramid_allowed() returns false even when gold is trending hard.
        //
        // FIX: when GoldFlow has a profitable position (trail_stage >= 2 = real trail
        // active, not just BE), inject the flow direction as trend bias directly.
        // This lets pyramid_allowed() gate on L2 confirmation alone.
        // Only inject if bias is currently 0 -- never override an earned bracket bias.
        // Withdraw injection when flow closes (handled naturally -- bias resets on timeout).
        if (g_gold_flow.pos.active && g_gold_flow.pos.trail_stage >= 2
            && gold_trend.bias == 0) {
            const int flow_dir = g_gold_flow.pos.is_long ? 1 : -1;
            gold_trend.bias          = flow_dir;
            gold_trend.bias_set_ms   = now_ms_g;
            gold_trend.block_until_ms = now_ms_g + 600000; // 10min -- long enough to pyramid
            static int64_t s_bias_inject_log = 0;
            if (now_ms_g - s_bias_inject_log > 30000) {
                s_bias_inject_log = now_ms_g;
                printf("[FLOW-BIAS-INJECT] XAUUSD GoldFlow trail_stage=%d %s ? bias=%d (L2=%.3f)\n",
                       g_gold_flow.pos.trail_stage,
                       g_gold_flow.pos.is_long ? "LONG" : "SHORT",
                       flow_dir, g_macro_ctx.gold_l2_imbalance);
            }
        }
        // Withdraw bias injection when flow closes or reverses
        if (!g_gold_flow.pos.active && gold_trend.bias != 0
            && gold_trend.bias_set_ms > 0) {
            // Only clear if bias was injected by flow (no bracket exits recorded recently)
            if (g_bracket_trend["XAUUSD"].exits.empty()) {
                gold_trend.bias = 0;
            }
        }

        const bool gold_pyramid_ok    = gold_trend.pyramid_allowed(g_macro_ctx.gold_l2_imbalance, now_ms_g);
        // Block pyramid in IMPULSE regime: price is thrusting hard -- adding on
        // during a thrust means chasing the move at peak momentum with tight SLs.
        // ?? Impulse regime gate ???????????????????????????????????????????????
        // CompressionBreakout is blocked in IMPULSE because compression geometry
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
                printf("[BRACKET-COOLDOWN-BYPASS] XAUUSD bias=%d -- trend leg bypasses 90s cooldown\n",
                       gold_trend.bias);
            }
        }

        // ?? GoldFlow trend-pyramid bypass ????????????????????????????????
        // When GoldFlow has an active profitable position (trail_stage >= 1 = BE locked),
        // the bracket is normally blocked from arming. This kills pyramiding on the
        // strongest trending moves -- exactly when we want to be adding size.
        //
        // FIX: allow bracket to arm in the SAME direction as the active GoldFlow
        // position when flow is profitable (trail_stage >= 1 = past breakeven).
        // The bracket add-on uses a reduced size (50% of PYRAMID_SIZE_MULT = 37.5%)
        // since GoldFlow is already carrying the primary position.
        //
        // Safety gates still apply: no IMPULSE regime, no counter-trend, spread gate,
        // L2 confirmation required, position cap enforced.
        const bool flow_active_profitable = g_gold_flow.pos.active
                                         && g_gold_flow.pos.trail_stage >= 1;
        const bool flow_dir_matches_bracket = flow_active_profitable
            && ((g_gold_flow.pos.is_long  && !bracket_open)   // flow LONG, bracket would arm LONG
             || (!g_gold_flow.pos.is_long && !bracket_open)); // flow SHORT, bracket would arm SHORT
        // Allow bracket arm alongside GoldFlow IF:
        //   flow is profitable (BE locked), direction matches, L2 confirms, not IMPULSE
        const bool flow_pyramid_bypass = (flow_dir_matches_bracket || gs_winning)
                                      && gold_pyramid_ok        // L2 must confirm direction
                                      && !gold_impulse_regime;  // never pyramid into IMPULSE thrust

        // asia_trend_ok removed from bracket gate:
        //   The BracketEngine (CompressionBreakout) has its own Asia quality gates:
        //   $6 compression range, $2.50 trigger, spread filter, ATR floor.
        //   asia_trend_ok was blocking ALL bracket entries in Asia when L2 was
        //   neutral (0.501) -- which is correct behaviour for GoldFlowEngine (L2-driven)
        //   but wrong for CompressionBreakout (structure-driven).
        //   asia_trend_ok still gates GoldFlow entries (via gold_can_enter chain).
        // London open noise bypass: if a strong directional move is already
        // underway (|ewm_drift| >= 3.0), allow bracket arming despite the noise
        // window. A genuine breakout produces drift >> 3.0 quickly. The sweep
        // that caused the original loss (entire $7.80 range in one bar) would
        // NOT produce sustained drift >= 3.0 -- it was a spike, not a trend.
        const bool london_drift_override = in_london_open_noise
            && (std::fabs(g_gold_stack.ewm_drift()) >= 3.0);
        const bool can_arm_bracket_base = gold_can_enter && gold_freq_ok
                                  && (!bracket_open || gold_is_pyramiding)
                                  && (!in_cooldown_phase || trend_dir_bypasses_cooldown)
                                  && (!g_gold_stack.has_open_position() || flow_pyramid_bypass)  // allow alongside winning GoldStack
                                  && (!g_gold_flow.has_open_position()  || flow_pyramid_bypass)  // allow alongside winning GoldFlow
                                  && !g_trend_pb_gold.has_open_position()
                                  && !g_le_stack.has_open_position()
                                  && (!in_london_open_noise || london_drift_override)
                                  && !gold_impulse_regime;  // block CompressionBreakout in IMPULSE -- wrong engine for thrusting moves
        // NOTE: gold_trend_blocked (counter_trend_blocked) is intentionally NOT here.
        // It blocks the counter-trend ARM direction via arm_allowed(), not can_arm_bracket itself.
        // Having it here blocked ALL bracket arming when FLOW-BIAS-INJECT set bias,
        // which is exactly when we WANT pyramiding to be possible.

        // ?? Supervisor chop_detected price-break override ????????????????
        // Problem: Supervisor fires chop_detected BEFORE the breakout candle,
        // setting allow_bracket=0. By the time price confirms direction by
        // breaking brk_hi or brk_lo, can_arm_bracket is already 0 and stays 0.
        // Fix: if bracket is ARMED with a valid range AND price has traded
        // THROUGH the bracket level by >= BRK_OVERRIDE_DIST points, override
        // the supervisor verdict. The price break IS the confirmation signal.
        // Gates still required: gold_can_enter (risk/session), freq_ok, no open pos.
        // Does NOT override: spread block, london noise, impulse regime block.
        // Applies to both LONG (ask > brk_hi) and SHORT (bid < brk_lo) breaks.
        static constexpr double BRK_OVERRIDE_DIST = 2.0;  // pts through level to confirm
        const bool gold_bracket_already_armed = (g_bracket_gold.phase == omega::BracketPhase::ARMED);
        const double brk_hi_now = g_bracket_gold.bracket_high;
        const double brk_lo_now = g_bracket_gold.bracket_low;
        const bool price_breaks_hi = gold_bracket_already_armed
                                  && (brk_hi_now > 0.0)
                                  && (ask >= brk_hi_now + BRK_OVERRIDE_DIST);
        const bool price_breaks_lo = gold_bracket_already_armed
                                  && (brk_lo_now > 0.0)
                                  && (bid <= brk_lo_now - BRK_OVERRIDE_DIST);
        // Override requires all hard gates but bypasses supervisor allow_bracket.
        // Re-uses can_arm_bracket_base which already checks gold_can_enter,
        // freq_ok, no open positions, and session gates.
        const bool chop_price_break_override = (price_breaks_hi || price_breaks_lo)
                                  && gold_freq_ok
                                  && !bracket_open
                                  && !gold_is_pyramiding
                                  && !in_cooldown_phase
                                  && !g_gold_stack.has_open_position()
                                  && !g_gold_flow.has_open_position()
                                  && !g_trend_pb_gold.has_open_position()
                                  && !g_le_stack.has_open_position()
                                  && (!in_london_open_noise || london_drift_override)
                                  && !gold_impulse_regime;
        if (chop_price_break_override) {
            static int64_t s_chop_ovr_log = 0;
            const int64_t now_ovr = static_cast<int64_t>(std::time(nullptr));
            if (now_ovr - s_chop_ovr_log >= 5) {
                s_chop_ovr_log = now_ovr;
                printf("[BRK-CHOP-OVERRIDE] XAUUSD supervisor chop_detected bypassed -- "
                       "price broke %s by %.1fpt (brk_hi=%.2f brk_lo=%.2f bid=%.2f ask=%.2f)\n",
                       price_breaks_hi ? "brk_hi" : "brk_lo",
                       price_breaks_hi ? (ask - brk_hi_now) : (brk_lo_now - bid),
                       brk_hi_now, brk_lo_now, bid, ask);
                fflush(stdout);
            }
        }
        const bool can_arm_bracket = can_arm_bracket_base || chop_price_break_override;

        // ?? Trend-direction bracket (IMPULSE regime) ??????????????????????????
        // When gold_impulse_regime=true the symmetric bracket is blocked because
        // compression geometry breaks down in a thrust.
        // This path fires ONE-SIDED: only the trend-direction stop order is sent.
        // SL is ATR-based (not range-based) so it reflects actual trend volatility.
        // Controlled by TREND_BRACKET_ENABLED and TREND_BRACKET_SL_MULT in symbols.ini.
        const auto& xau_sym_cfg = g_sym_cfg.get("XAUUSD");
        const bool can_arm_trend_bracket =
            xau_sym_cfg.trend_bracket_enabled
            && gold_impulse_regime           // only in IMPULSE/thrusting moves
            && gold_trend.bias != 0          // established trend direction required
            && gold_can_enter                // all standard risk/session gates
            && gold_freq_ok
            && !bracket_open                 // no existing bracket position
            && !g_gold_flow.has_open_position()
            && !g_gold_stack.has_open_position()
            && !in_cooldown_phase
            && !in_london_open_noise;

        if (can_arm_trend_bracket) {
            // Get current ATR from GoldStack for SL sizing
            // Use GoldFlow ATR -- it has current_atr() tracking real tick volatility.
            // GoldStack uses volatility * 2.5 proxy; GoldFlow has the proper EWM ATR.
            // Fallback to vol_range() * 0.5 if GoldFlow ATR not yet warmed.
            const double gf_atr        = g_gold_flow.current_atr();
            const double trend_atr      = (gf_atr > 2.0) ? gf_atr
                                        : (g_gold_stack.vol_range() > 0.0
                                           ? g_gold_stack.vol_range() * 0.5
                                           : 15.0);  // absolute fallback $15 on VIX27 day
            const double sl_mult        = xau_sym_cfg.trend_bracket_sl_mult;
            const double trend_sl_dist  = std::max(trend_atr * sl_mult, 5.0); // floor $5
            const bool   is_long_trend  = (gold_trend.bias == 1);

            // Entry: stop order at bracket edge in trend direction
            const double trend_entry    = is_long_trend ? g_bracket_gold.bracket_high
                                                        : g_bracket_gold.bracket_low;
            if (trend_entry > 0.0) {
                // SL: ATR-based behind entry, not the opposite bracket level
                const double trend_sl   = is_long_trend ? (trend_entry - trend_sl_dist)
                                                        : (trend_entry + trend_sl_dist);
                // TP: range ? RR in trend direction (same as normal bracket)
                const double brk_range  = g_bracket_gold.bracket_high - g_bracket_gold.bracket_low;
                const double trend_tp   = is_long_trend
                    ? (trend_entry + brk_range * g_bracket_gold.RR)
                    : (trend_entry - brk_range * g_bracket_gold.RR);

                // Sizing: risk-based on ATR SL
                const double tb_lot     = std::min(
                    compute_size("XAUUSD", trend_sl_dist, ask - bid, g_bracket_gold.ENTRY_SIZE),
                    0.30);  // hard cap at 0.30 lots for trend bracket

                // Cost viability check
                const double tb_tp_dist = std::fabs(trend_tp - trend_entry);
                const bool tb_cost_ok   = ExecutionCostGuard::is_viable(
                    "XAUUSD", ask - bid, tb_tp_dist, tb_lot);

                if (tb_cost_ok && tb_lot >= 0.01) {
                    printf("[TREND-BRACKET] XAUUSD %s entry=%.2f sl=%.2f(dist=%.2f) tp=%.2f"
                           " atr=%.2f mult=%.1f lot=%.4f bias=%d drift=%.2f\n",
                           is_long_trend ? "LONG" : "SHORT",
                           trend_entry, trend_sl, trend_sl_dist, trend_tp,
                           trend_atr, sl_mult, tb_lot, gold_trend.bias,
                           gold_ewm_drift_now);
                    fflush(stdout);

                    // Send ONLY trend-direction stop order -- no counter leg
                    write_trade_open_log("XAUUSD", "TrendBracket",
                        is_long_trend ? "LONG" : "SHORT",
                        trend_entry, trend_tp, trend_sl, tb_lot,
                        ask - bid, gold_stack_regime, "TREND_BRACKET_ARM");

                    const std::string tb_id = send_live_order(
                        "XAUUSD", is_long_trend, tb_lot, trend_entry);

                    // Register with bracket engine as one-sided pending
                    // pending_both carries the filled side only
                    omega::BracketBothSignals tb_sig;
                    tb_sig.valid        = true;
                    tb_sig.size         = tb_lot;
                    if (is_long_trend) {
                        tb_sig.long_entry  = trend_entry;
                        tb_sig.long_sl     = trend_sl;
                        tb_sig.long_tp     = trend_tp;
                        tb_sig.short_entry = 0.0;  // no counter leg
                        tb_sig.short_sl    = 0.0;
                        tb_sig.short_tp    = 0.0;
                        g_bracket_gold.pending_long_clOrdId  = tb_id;
                        g_bracket_gold.pending_short_clOrdId = "";  // nothing to cancel
                    } else {
                        tb_sig.short_entry = trend_entry;
                        tb_sig.short_sl    = trend_sl;
                        tb_sig.short_tp    = trend_tp;
                        tb_sig.long_entry  = 0.0;  // no counter leg
                        tb_sig.long_sl     = 0.0;
                        tb_sig.long_tp     = 0.0;
                        g_bracket_gold.pending_short_clOrdId = tb_id;
                        g_bracket_gold.pending_long_clOrdId  = "";  // nothing to cancel
                    }
                    g_bracket_gold.pending_both = tb_sig;
                    g_bracket_gold.phase = omega::BracketPhase::PENDING;
                    g_bracket_gold.ENTRY_SIZE = tb_lot;
                    g_telemetry.UpdateLastSignal("XAUUSD", "TREND_BRACKET",
                        trend_entry, "TREND_DIRECTION_ONLY",
                        gold_stack_regime, regime.c_str(), "TREND_BRACKET",
                        trend_tp, trend_sl);
                    ++g_bracket_gold_trades_this_minute;
                } else {
                    printf("[TREND-BRACKET] XAUUSD BLOCKED cost_ok=%d lot=%.4f tp_dist=%.2f\n",
                           (int)tb_cost_ok, tb_lot, tb_tp_dist);
                }
            }
        }

        // ?? Gold bracket gate diagnostic -- prints every 10s ???????????????
        {
            static int64_t s_last_brk_diag = 0;
            if (now_ms_g - s_last_brk_diag >= 10000) {
                s_last_brk_diag = now_ms_g;
                const char* phase_str =
                    g_bracket_gold.phase == omega::BracketPhase::IDLE     ? "IDLE"
                  : g_bracket_gold.phase == omega::BracketPhase::ARMED    ? "ARMED"
                  : g_bracket_gold.phase == omega::BracketPhase::PENDING  ? "PENDING"
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
        g_bracket_gold.on_tick(bid, ask, now_ms_g,
            (bracket_open || gold_bracket_armed) ? can_manage : can_arm_bracket,
            regime.c_str(), bracket_on_close, gold_vwap_now,
            g_macro_ctx.gold_l2_imbalance);
        g_telemetry.UpdateBracketState("XAUUSD",
            static_cast<int>(g_bracket_gold.phase),
            g_bracket_gold.bracket_high,
            g_bracket_gold.bracket_low);
        const auto bgsigs = g_bracket_gold.get_signals();
        if (bgsigs.valid) {
            // Pyramid: use tighter SL than normal bracket -- cap pyramid risk at 50% of base risk
            // Normal bracket SL can be $5-7 wide; pyramid add-ons use half that
            const double raw_sl_dist = std::fabs(bgsigs.long_entry - bgsigs.long_sl);
            const double pyr_sl_dist = (gold_is_pyramiding || flow_pyramid_bypass)
                ? std::min(raw_sl_dist, 3.0)  // cap pyramid SL at $3 distance
                : raw_sl_dist;
            const double base_bg_lot = compute_size("XAUUSD",
                pyr_sl_dist, ask - bid,
                g_bracket_gold.ENTRY_SIZE);
            const double bg_lot = (gold_is_pyramiding || flow_pyramid_bypass)
                ? std::min(base_bg_lot * PYRAMID_SIZE_MULT, 0.20)  // cap pyramid lot at 0.20
                : base_bg_lot;
            // Cost guard: bracket TP dist = SL dist * RR
            const double bg_tp_dist = std::fabs(bgsigs.long_entry - bgsigs.long_sl) * g_bracket_gold.RR;
            const bool bg_cost_ok = ExecutionCostGuard::is_viable("XAUUSD", ask - bid, bg_tp_dist, bg_lot);
            if (!bg_cost_ok) {
                g_telemetry.IncrCostBlocked();
            } else {
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
                    printf("[BRACKET-L2] XAUUSD bias=%d trend_lot=%.4f counter_lot=%.4f l2=%.3f\n",
                           gold_trend.bias, bg_lot, bg_lot * 0.5,
                           g_macro_ctx.gold_l2_imbalance);
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
            }
        }
    }

    // ?? GoldFlowEngine position management -- ALWAYS runs when position open ??
    // ?? Shared GoldFlow close handler -- must be declared before both callbacks ??
    // Called by flow_mgmt_cb (manage block) and flow_on_close (entry block).
    // Handles all post-close state updates common to both paths:
    //   hard-stop tombstone clear, exit_ts/dir/reason atomics,
    //   reversal window, dir SL cooldown, trail block.
    // Does NOT cover: partial orders (handled inline per-callback),
    //   crash-bypass consec counter (entry block only).
    auto gf_close_shared = [&](const omega::TradeRecord& tr) {
        clear_hard_stop_tombstone("XAUUSD", tr.entryTs);
        const int64_t now_s    = static_cast<int64_t>(std::time(nullptr));
        const int     exit_dir = (tr.side == "LONG") ? 1 : -1;
        g_gold_flow_exit_ts.store(now_s);
        g_gold_flow_exit_dir.store(exit_dir);
        g_gold_flow_exit_price_x100.store(
            static_cast<int64_t>(tr.exitPrice * 100.0));
        const bool is_sl = (tr.exitReason == "SL_HIT");
        g_gold_flow_exit_reason.store(is_sl ? 1 : 2);
        if (is_sl) {
            g_gold_reversal_window_until.store(now_s + 60);
            printf("[GOLD-REVERSAL] GoldFlow SL_HIT %s -- reversal window open 60s\n",
                   tr.side.c_str());
            fflush(stdout);
            const bool is_long_sl = (tr.side == "LONG");
            auto& sl_count = is_long_sl ? g_gf_dir_sl_long_count  : g_gf_dir_sl_short_count;
            auto& sl_first = is_long_sl ? g_gf_dir_sl_long_first  : g_gf_dir_sl_short_first;
            auto& blocked  = is_long_sl ? g_gf_long_blocked_until : g_gf_short_blocked_until;
            const int64_t first_ts = sl_first.load();
            if (first_ts == 0 || (now_s - first_ts) > GF_DIR_SL_WINDOW_SEC) {
                sl_count.store(1);
                sl_first.store(now_s);
            } else {
                const int count = sl_count.fetch_add(1) + 1;
                if (count >= GF_DIR_SL_MAX) {
                    blocked.store(now_s + GF_DIR_SL_COOLDOWN_SEC);
                    sl_count.store(0);
                    sl_first.store(0);
                    printf("[GFE-FADE-BLOCK] %s direction blocked %llds after %d consecutive SL_HITs\n",
                           is_long_sl ? "LONG" : "SHORT",
                           (long long)GF_DIR_SL_COOLDOWN_SEC, count);
                    fflush(stdout);
                }
            }
        }
        const bool is_trail = (tr.exitReason == "TRAIL_HIT" || tr.exitReason == "BE_HIT");
        if (is_trail) {
            g_gold_trail_block_until.store(now_s + 60);
            g_gold_trail_block_dir.store((tr.side == "LONG") ? 1 : -1);
            printf("[GOLD-TRAIL-BLOCK] GoldFlow %s %s -- same-dir re-entry blocked 60s\n",
                   tr.exitReason.c_str(), tr.side.c_str());
            fflush(stdout);
        }
    };

    // CRITICAL: manage_position() must run on every XAUUSD tick to check SL/trail.
    // GoldFlow DISABLED 2026-04-05: backtest proved no structural edge.
    // Avg winner $15 vs avg loser $74, payoff 0.20:1. 2yr MFE scan showed
    // microstructure signal only -- not the 1-3pt structural moves needed to
    // beat 0.35pt cost. Replace: OverlapMomentumEngine + OverlapFadeEngine.
    if (g_cfg.goldflow_enabled && g_gold_flow.has_open_position()) {
        // Inject trend bias (wall detection for trail tightening)
        const bool sup_trend_mgmt = (gold_sdec.regime == omega::Regime::TREND_CONTINUATION);
        const bool gf_wall_mgmt   = g_gold_flow.pos.is_long
                                    ? g_macro_ctx.gold_wall_above
                                    : g_macro_ctx.gold_wall_below;
        // expansion_mode: true when supervisor confirms EXPANSION_BREAKOUT or TREND_CONTINUATION.
        // Activates velocity trail (wide trail, delayed arm) so crash/surge moves are ridden.
        const bool gf_expansion_mgmt = (gold_sdec.regime == omega::Regime::EXPANSION_BREAKOUT
                                     || gold_sdec.regime == omega::Regime::TREND_CONTINUATION);
        const double gf_vol_ratio_mgmt = g_gold_stack.recent_vol_pct() > 0.0 && g_gold_stack.base_vol_pct() > 0.0
            ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 1.0;
        g_gold_flow.set_trend_bias(gold_momentum, gold_sdec.confidence,
                                   sup_trend_mgmt, gf_wall_mgmt, gold_vwap_pts,
                                   gf_expansion_mgmt, gf_vol_ratio_mgmt);
        // Inject bar context (RSI/trend/BB) for SL hold decisions.
        // When RSI confirms trade direction and price isn't at the extreme band,
        // manage_position() suppresses the SL for up to 30s to avoid noise exits.
        if (g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
            const double ctx_e9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
            const double ctx_e50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
            const int ctx_trend  = (ctx_e9 > 0.0 && ctx_e50 > 0.0)
                ? (ctx_e9 < ctx_e50 ? -1 : +1) : 0;
            g_gold_flow.set_bar_context(
                g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed),
                ctx_trend,
                g_bars_gold.m1.ind.bb_pct.load(std::memory_order_relaxed));
            g_gold_flow_reload.set_bar_context(
                g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed),
                ctx_trend,
                g_bars_gold.m1.ind.bb_pct.load(std::memory_order_relaxed));
        }
        // Manage callback: always-running block (position already open).
        // Does NOT include crash-bypass consec counter (that's for entry SL_HITs).
        auto flow_mgmt_cb = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == std::string("PARTIAL_1R")) {
                if (g_cfg.mode == "LIVE") {
                    send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
                }
                handle_closed_trade(tr);
                return;
            }
            handle_closed_trade(tr);
            send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
            gf_close_shared(tr);
        };
        g_gold_flow.on_tick(bid, ask,
            g_macro_ctx.gold_l2_imbalance,
            g_gold_stack.ewm_drift(),
            now_ms_g, flow_mgmt_cb,
            g_macro_ctx.session_slot);
    }

    // ?? GoldFlow reload manage -- ALWAYS runs when reload position open ?????
    // Mirrors the main flow manage block exactly. The reload instance is an
    // independent GoldFlowEngine -- it has its own SL, trail, ratchet, staircase.
    // It does NOT arm further reloads (only main flow arms reloads).
    if (g_gold_flow_reload.has_open_position()) {
        g_gold_flow_reload.set_trend_bias(gold_momentum, gold_sdec.confidence,
            (gold_sdec.regime == omega::Regime::TREND_CONTINUATION),
            g_gold_flow_reload.pos.is_long
                ? g_macro_ctx.gold_wall_above
                : g_macro_ctx.gold_wall_below,
            gold_vwap_pts,
            (gold_sdec.regime == omega::Regime::EXPANSION_BREAKOUT
             || gold_sdec.regime == omega::Regime::TREND_CONTINUATION),
            (g_gold_stack.recent_vol_pct() > 0.0 && g_gold_stack.base_vol_pct() > 0.0)
                ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 1.0);
        auto reload_mgmt_cb = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == std::string("PARTIAL_1R")) {
                if (g_cfg.mode == "LIVE") {
                    send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
                }
                handle_closed_trade(tr);
                return;
            }
            handle_closed_trade(tr);
            send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
            // Clear hard stop tombstone for this reload position
            clear_hard_stop_tombstone("XAUUSD", tr.entryTs);
            // Reload exits do not set reversal windows -- main flow owns those.
            printf("[GF-RELOAD] POSITION CLOSED reason=%s pnl_raw=%.4f\n",
                   tr.exitReason.c_str(), tr.pnl);
            fflush(stdout);
        };
        g_gold_flow_reload.on_tick(bid, ask,
            g_macro_ctx.gold_l2_imbalance,
            g_gold_stack.ewm_drift(),
            now_ms_g, reload_mgmt_cb,
            g_macro_ctx.session_slot);
    }

    // ?? GoldFlow add-on manage -- ALWAYS runs when addon position open ??????
    // Mirrors the reload manage block. The addon instance is independent --
    // its own SL, trail, ratchet. When base closes, addon keeps running
    // until its own trail/SL fires (already deep in profit by definition).
    // Does NOT arm its own addon or reload.
    if (g_gold_flow_addon.has_open_position()) {
        g_gold_flow_addon.set_trend_bias(gold_momentum, gold_sdec.confidence,
            (gold_sdec.regime == omega::Regime::TREND_CONTINUATION),
            g_gold_flow_addon.pos.is_long
                ? g_macro_ctx.gold_wall_above
                : g_macro_ctx.gold_wall_below,
            gold_vwap_pts,
            (gold_sdec.regime == omega::Regime::EXPANSION_BREAKOUT
             || gold_sdec.regime == omega::Regime::TREND_CONTINUATION),
            (g_gold_stack.recent_vol_pct() > 0.0 && g_gold_stack.base_vol_pct() > 0.0)
                ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 1.0);
        auto addon_mgmt_cb = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == std::string("PARTIAL_1R")) {
                if (g_cfg.mode == "LIVE")
                    send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
                handle_closed_trade(tr);
                return;
            }
            handle_closed_trade(tr);
            send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
            clear_hard_stop_tombstone("XAUUSD", tr.entryTs);
            printf("[GF-ADDON] POSITION CLOSED reason=%s pnl_raw=%.4f\n",
                   tr.exitReason.c_str(), tr.pnl);
            fflush(stdout);
        };
        g_gold_flow_addon.on_tick(bid, ask,
            g_macro_ctx.gold_l2_imbalance,
            g_gold_stack.ewm_drift(),
            now_ms_g, addon_mgmt_cb,
            g_macro_ctx.session_slot);
    }

    // ?? RSIReversal indicator warmup -- UNCONDITIONAL, every tick ????????????
    // Must run before MacroCrashEngine so tick_rsi() is current when MCE reads it.
    // RSIReversalEngine.on_tick() is gated (rsi_rev_can_enter), so when the gate
    // is closed (e.g. hybrid_gold has position) indicators go stale and MacroCrash
    // reads a stale RSI value. update_indicators() is ungated -- always live.
    g_rsi_reversal.update_indicators(bid, ask);

    // ?? MacroCrashEngine -- always-on macro event capture ????????????????
    // Fires on: ATR + vol_ratio + drift thresholds (session-aware: lower in Asia).
    // DOM primer: book_slope / vacuum / microprice_bias lower drift threshold 40%
    //   when the DOM is confirming direction before EWM drift catches up.
    // RSI confirmation: RSI extreme aligning with drift substitutes for expansion_regime
    //   in Asia (supervisor lags by CONFIRM_TICKS; RSI is live price-based).
    // Both directions: is_long = (ewm_drift > 0) -- LONG on spikes up, SHORT on crashes.
    // Independent of GoldFlow -- runs 24/7, no session restriction.
    {
        const bool expansion_regime = (gold_sdec.regime == omega::Regime::EXPANSION_BREAKOUT
                                    || gold_sdec.regime == omega::Regime::TREND_CONTINUATION);
        const double mce_atr       = g_gold_flow.current_atr() > 0.0
                                     ? g_gold_flow.current_atr() : 5.0;
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
    // ?? RSIReversalEngine -- tick-level RSI entries, no bar dependency ????????
    // Computes its own RSI(14) from mid price on every tick.
    // No bars_ready gate, no bar RSI, fires as soon as tick RSI reaches extreme.
    // Entry: RSI<42=LONG, RSI>58=SHORT (catches 5pt moves, not just 20pt+ crashes)
    {
        // Position management -- always runs when open (no gate)
        if (g_rsi_reversal.has_open_position()) {
            g_rsi_reversal.on_tick(bid, ask,
                g_macro_ctx.session_slot, now_ms_g, bracket_on_close);
        }

        // Entry gate: no other XAUUSD position open + tradeable + not dead zone
        // RSIReversal: NOT blocked when GoldFlow/GoldStack are in a winning trail.
        // A profitable GoldFlow trend trade + a small RSI scalp are uncorrelated.
        // Blocked only when those engines are losing/flat (risk management).
        const bool rsi_rev_can_enter =
            !g_rsi_reversal.has_open_position()
            && tradeable
            && !in_ny_close_noise
            && !g_bracket_gold.has_open_position()
            && !(g_gold_stack.has_open_position() && !gs_winning)
            && !(gf_open && !gf_winning)
            && !g_trend_pb_gold.has_open_position()
            && !g_hybrid_gold.has_open_position()
            && !g_nbm_gold_london.has_open_position();

        if (rsi_rev_can_enter) {

            g_rsi_reversal.on_tick(bid, ask,
                g_macro_ctx.session_slot, now_ms_g, bracket_on_close);

            if (g_rsi_reversal.has_open_position()) {
                // Size using standard risk engine -- same as GoldFlow sizing
                const double rsi_sl_dist = std::fabs(g_rsi_reversal.pos.entry - g_rsi_reversal.pos.sl);
                const double rsi_lot     = (rsi_sl_dist > 0.0)
                    ? std::max(0.01, std::min(g_cfg.max_lot_gold,
                        g_cfg.risk_per_trade_usd / (rsi_sl_dist * 100.0)))
                    : 0.01;
                g_rsi_reversal.patch_size(rsi_lot);

                // Log and telemetry always fire (shadow or live) -- GUI shows signal
                write_trade_open_log("XAUUSD", "RSIReversal",
                    g_rsi_reversal.pos.is_long ? "LONG" : "SHORT",
                    g_rsi_reversal.pos.entry, 0.0, g_rsi_reversal.pos.sl,
                    g_rsi_reversal.pos.size, ask - bid, "RSI_REVERSAL", "RSI_EXTREME");
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

    // ?? MicroMomentumEngine -- fast 4-8pt momentum capture ???????????????????
    // RSI slope + price displacement, both directions, all sessions.
    // No bar dependency, no regime gate. Catches moves AS THEY HAPPEN.
    {
        // Position management always runs
        if (g_micro_momentum.has_open_position()) {
            g_micro_momentum.on_tick(bid, ask,
                g_macro_ctx.session_slot, now_ms_g, bracket_on_close);
        }

        // Entry gate: no other gold position + tradeable + not dead zone
        // MicroMomentum: same rule -- NOT blocked by winning GoldFlow/GoldStack.
        // Small scalp alongside profitable trend = fine. Blocked when flat/losing.
        const bool mm_can_enter =
            !g_micro_momentum.has_open_position()
            && tradeable
            && !in_ny_close_noise
            && !g_bracket_gold.has_open_position()
            && !(g_gold_stack.has_open_position() && !gs_winning)
            && !(gf_open && !gf_winning)
            && !g_trend_pb_gold.has_open_position()
            && !g_hybrid_gold.has_open_position()
            && !g_rsi_reversal.has_open_position()
            && !g_nbm_gold_london.has_open_position();

        if (mm_can_enter) {
            g_micro_momentum.on_tick(bid, ask,
                g_macro_ctx.session_slot, now_ms_g, bracket_on_close);

            if (g_micro_momentum.has_open_position()) {
                // Size: risk_per_trade_usd / (sl_dist * 100)
                const double mm_sl_dist = std::fabs(
                    g_micro_momentum.pos.entry - g_micro_momentum.pos.sl);
                const double mm_lot = (mm_sl_dist > 0.0)
                    ? std::max(0.01, std::min(g_cfg.max_lot_gold,
                        g_cfg.risk_per_trade_usd / (mm_sl_dist * 100.0)))
                    : 0.01;
                g_micro_momentum.patch_size(mm_lot);

                write_trade_open_log("XAUUSD", "MicroMomentum",
                    g_micro_momentum.pos.is_long ? "LONG" : "SHORT",
                    g_micro_momentum.pos.entry,
                    g_micro_momentum.pos.tp,
                    g_micro_momentum.pos.sl,
                    g_micro_momentum.pos.size, ask - bid,
                    "MOMENTUM", "MICRO_MOM");

                g_telemetry.UpdateLastSignal("XAUUSD",
                    g_micro_momentum.pos.is_long ? "LONG" : "SHORT",
                    g_micro_momentum.pos.entry, "MICRO_MOM",
                    "MicroMomentum", regime.c_str(), "MicroMomentum",
                    g_micro_momentum.pos.tp, g_micro_momentum.pos.sl);

                if (!g_micro_momentum.shadow_mode) {
                    send_live_order("XAUUSD",
                        g_micro_momentum.pos.is_long,
                        g_micro_momentum.pos.size,
                        g_micro_momentum.pos.entry);
                    g_telemetry.UpdateLastEntryTs();
                }
            }
        }
    }

    // When stair step 1 banks the first 33% and arms a reload, try_reload()
    // is called every tick until it fires, cancels, or times out (5s).
    //
    // The reload enters a NEW fresh full-size position in the same direction
    // ?? GoldFlow reload entry -- fires g_gold_flow_reload after PARTIAL_1R ??
    // When main flow banks step 1 and arms a reload, try_reload() checks
    // confirmation conditions each tick. When it returns true (signal fired),
    // we seed and enter on the independent g_gold_flow_reload instance.
    //
    // Gate is intentionally LIGHTER than gold_can_enter:
    //   - Session must be valid (not dead zone)
    //   - No reload already open (only one reload at a time)
    //   - No NY close noise
    //   - Not in the same direction as a fresh SL_HIT block
    //   - try_reload() internal gates: timeout, retrace, drift, confirmation ticks
    //
    // We do NOT require gold_can_enter (which checks symbol_gate ? max_positions).
    // A reload is a continuation of an already-validated trade, not a new signal.
    // If max_positions is the only thing blocking, the reload should still fire.
    if (g_gold_flow.reload_pending()
        && !g_gold_flow_reload.has_open_position()
        && true   // 24h -- no session slot block
        && !in_ny_close_noise) {

        const double gf_mid_r = (bid + ask) * 0.5;
        const bool signal = g_gold_flow.try_reload(
            bid, ask, gf_mid_r, ask - bid,
            g_gold_stack.ewm_drift(),
            now_ms_g);

        if (signal) {
            // try_reload() confirmed -- fire directly on reload instance
            const bool reload_long = g_gold_flow.reload_is_long();
            const double reload_atr = g_gold_flow.reload_atr();

            g_gold_flow_reload.risk_dollars = g_gold_flow.risk_dollars;

            // Seed bar ATR if available for accurate SL sizing
            double atr_for_reload = reload_atr;
            if (g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
                const double bar_atr_r = g_bars_gold.m1.ind.atr14
                                            .load(std::memory_order_relaxed);
                if (bar_atr_r > 1.0 && bar_atr_r < 50.0)
                    atr_for_reload = 0.7 * reload_atr + 0.3 * bar_atr_r;
            }

            // Clamp session_slot: -1 means supervisor not yet warmed.
            // Use 1 (London -- least restrictive non-Asia) as the safe fallback.
            // This prevents session=-1 in the reload ENTRY log and ensures the
            // Asia SL floor (slot==6) is only applied when confirmed in Asia.
            const int reload_slot = (g_macro_ctx.session_slot >= 0)
                ? g_macro_ctx.session_slot : 1;
            const bool entered = g_gold_flow_reload.force_entry(
                reload_long, bid, ask, atr_for_reload, now_ms_g,
                reload_slot);

            if (entered) {
                if (g_cfg.mode == "LIVE") {
                    send_live_order("XAUUSD",
                        g_gold_flow_reload.pos.is_long,
                        g_gold_flow_reload.pos.size,
                        g_gold_flow_reload.pos.entry);
                }
                g_telemetry.UpdateLastEntryTs();  // watchdog: GoldFlow reload entry
                printf("[GF-RELOAD] ENTRY FIRED %s lot=%.3f entry=%.2f sl=%.2f atr=%.2f\n",
                       g_gold_flow_reload.pos.is_long ? "LONG" : "SHORT",
                       g_gold_flow_reload.pos.size,
                       g_gold_flow_reload.pos.entry,
                       g_gold_flow_reload.pos.sl,
                       atr_for_reload);
                fflush(stdout);
            } else {
                printf("[GF-RELOAD] ENTRY FAILED -- force_entry returned false\n");
                fflush(stdout);
            }
        }
    }

    // GoldFlowEngine: L2 order-flow directional engine.
    // Fires only when no other gold position is open.
    // Entry: L2 imbalance persistence + EWM drift + momentum all confirm.
    // SL: ATR(20) * 1.0 -- volatility-sized, not bracket-range-sized.
    // Trail: progressive ATR-based stages that tighten as profit grows.
    // ?? Outer gate diagnostic -- fires when gold_can_enter or sibling gates block ?
    {
        static int64_t s_outer_gate_log = 0;
        const bool any_gold_open = g_bracket_gold.has_open_position()
                                || g_gold_stack.has_open_position()
                                || g_gold_flow.has_open_position()
                                || g_gold_flow_reload.has_open_position()
                                || g_le_stack.has_open_position()
                                || g_trend_pb_gold.has_open_position();
        if (!gold_can_enter && !any_gold_open && nowSec() - s_outer_gate_log >= 30) {
            s_outer_gate_log = nowSec();
            printf("[GF-OUTER-BLOCK] gold_can_enter=0 session_ok=%d trail_block=%d "
                   "post_impulse=%d symbol_gate=%d any_open=%d\n",
                   (g_macro_ctx.session_slot != 0) ? 1 : 0,
                   gold_trail_blocked ? 1 : 0,
                   gold_post_impulse_block ? 1 : 0,
                   (!gold_trail_blocked && !gold_post_impulse_block) ? 1 : 0,
                   any_gold_open ? 1 : 0);
            fflush(stdout);
        }
    }
    if ((gold_can_enter || g_gf_vel_reentry_bypass.load())
        && !g_bracket_gold.has_open_position()
        && !g_gold_stack.has_open_position()
        && !g_gold_flow.has_open_position()
        && !g_le_stack.has_open_position()
        && !g_trend_pb_gold.has_open_position()) {
        // IMPULSE stability gate: GoldFlow must not enter on a single-tick IMPULSE.
        // Root cause: supervisor calls regime every tick. A micro-uptick triggers
        // IMPULSE for 1-2 ticks before correcting back -- GoldFlow enters before
        // correction, producing a LONG in a downtrend with atr=5.00 floor.
        // Fix: require IMPULSE to have held for >= 3 consecutive ticks before
        // GoldFlow is allowed to enter on an IMPULSE-regime signal.
        // Non-IMPULSE regimes (COMPRESSION, TREND etc) are unaffected.
        // Impulse stability: require >=3 consecutive IMPULSE ticks before GoldFlow enters.
        const bool gf_impulse_stable = (std::strcmp(gold_stack_regime, "IMPULSE") != 0)
                                    || (g_gold_impulse_ticks.load() >= 3);
        if (!gf_impulse_stable) {
            static int64_t s_imp_log = 0;
            const int64_t now_ig = static_cast<int64_t>(std::time(nullptr));
            if (now_ig - s_imp_log >= 10) {
                s_imp_log = now_ig;
                printf("[GF-IMPULSE-GHOST] Blocked -- IMPULSE only %d ticks, need 3\n",
                       g_gold_impulse_ticks.load());
                fflush(stdout);
            }
        } else {
        g_gold_flow.risk_dollars = (g_cfg.risk_per_trade_usd > 0.0)
                                   ? g_cfg.risk_per_trade_usd : GFE_RISK_DOLLARS;

        // ?? ATR-PROPORTIONAL LOT SCALING ??????????????????????????????????
        // Scales risk_dollars continuously with market volatility so lot size
        // adapts to both normal days AND crash/spike events automatically.
        //
        // DESIGN:
        //   base_risk  = risk_per_trade_usd (config, e.g. $80)
        //   atr_scale  = clamp(atr / ATR_NORMAL, 0.5, 4.0)
        //     where ATR_NORMAL = 5.0pt (typical London session ATR)
        //
        //   Normal day  ATR=3pt:  scale=0.60 -> risk=$48,  ~0.08 lots
        //   Normal day  ATR=5pt:  scale=1.00 -> risk=$80,  ~0.16 lots  (baseline)
        //   Active day  ATR=8pt:  scale=1.60 -> risk=$128, ~0.10 lots (wider SL eats the gain)
        //   Spike day   ATR=15pt: scale=3.00 -> risk=$240, ~0.16 lots (caps at 3x)
        //   Crash day   ATR=25pt: scale=4.00 -> risk=$320, ~0.13 lots (caps at 4x, SL=25pt)
        //
        // KEY INSIGHT: because lot = risk / (ATR * $100), a 3x ATR increase
        // at 3x risk keeps lot size roughly CONSTANT. The extra risk buys a
        // WIDER SL that isn't stopped out by the first $3 retracement during
        // a genuine $50+ crash move. This is what we want -- stay in the trade.
        //
        // TRAIL ADAPTATION (injected into GoldFlow via velocity_shadow_mode flag
        // repurposed as atr_scale hint -- see manage_position):
        //   ATR <= 5pt  (normal):  step1=$20, trail=0.5xATR  -- normal scalp exits
        //   ATR 5-10pt  (active):  step1=$35, trail=0.75xATR -- hold a bit longer
        //   ATR > 10pt  (spike):   step1=$60, trail=1.5xATR  -- let it run, don't exit early
        //   ATR > 20pt  (crash):   step1=$120, trail=2.5xATR -- full velocity trail
        //
        // Daily loss limit in config ($450) is the hard backstop regardless.
        // max_lot_gold cap (0.50) is the hard ceiling regardless.
        {
            static constexpr double GFE_ATR_NORMAL   = 5.0;   // baseline ATR for normal London day
            static constexpr double GFE_ATR_SCALE_MIN = 0.5;  // floor: never below 50% risk (quiet Asia)
            static constexpr double GFE_ATR_SCALE_MAX = 6.0;  // raised 4x->6x: at ATR=10pt crash
                                                               // risk=$80*6=480, lot=480/(10*100)=0.48
                                                               // matches $11k simulation target.
                                                               // max_lot_gold=0.50 is the hard ceiling.

            const double atr_scale = std::min(GFE_ATR_SCALE_MAX,
                                     std::max(GFE_ATR_SCALE_MIN,
                                              gf_atr_gate / GFE_ATR_NORMAL));
            const double scaled_risk = g_gold_flow.risk_dollars * atr_scale;

            // Vol ratio for logging context
            const double gf_vol_ratio_scale = (g_gold_stack.recent_vol_pct() > 0.0
                                            && g_gold_stack.base_vol_pct() > 0.0)
                ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 1.0;

            // Apply scaling -- always live, no shadow flag needed.
            // The atr_scale is derived purely from measured ATR, not regime classification,
            // so it cannot be gamed or trigger on regime mis-classification.
            g_gold_flow.risk_dollars = scaled_risk;

            // Adapt step1 trigger and trail multiplier inside GoldFlowEngine
            // by overriding the velocity_shadow_mode flag as a hint channel.
            // When atr > 2x normal: arm wide trail (suppress early exits).
            // When atr <= 2x normal: normal trail behaviour.
            // This reuses the existing velocity trail infrastructure -- no engine changes needed.
            const bool wide_trail_active = (gf_atr_gate > GFE_ATR_NORMAL * 2.0);
            g_gold_flow.velocity_shadow_mode = false;  // always live
            // Inject expansion mode based on ATR alone -- not just vol_ratio regime
            // This means wide trail arms on ANY genuine volatility expansion, not
            // only when supervisor has classified EXPANSION_BREAKOUT.
            const bool gf_expansion_entry = (gold_sdec.regime == omega::Regime::EXPANSION_BREAKOUT
                                          || gold_sdec.regime == omega::Regime::TREND_CONTINUATION)
                                          || wide_trail_active;  // ATR expansion overrides regime lag
            const double gf_vol_ratio_entry = (g_gold_stack.recent_vol_pct() > 0.0
                                            && g_gold_stack.base_vol_pct() > 0.0)
                ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 1.0;

            static int64_t s_size_log = 0;
            if (nowSec() - s_size_log >= 30) {
                s_size_log = nowSec();
                const double gf_sl_pts = gf_atr_gate * GFE_ATR_SL_MULT;
                const double lot_est   = std::min(0.50, scaled_risk / (gf_sl_pts * 100.0));
                printf("[GFE-ATR-SIZE] atr=%.2f scale=%.2fx risk_base=$%.0f risk_scaled=$%.0f "
                       "lot_est=%.3f sl_pts=%.2f vol_ratio=%.2f wide_trail=%d\n",
                       gf_atr_gate, atr_scale, g_cfg.risk_per_trade_usd, scaled_risk,
                       lot_est, gf_sl_pts, gf_vol_ratio_scale, (int)wide_trail_active);
                fflush(stdout);
            }
            // Store expansion flag for use later in this tick (set_trend_bias call)
            // We need a local so the later set_trend_bias uses the ATR-adjusted value
            // Store in a lambda-captured local that shadows gf_expansion_entry below
            (void)gf_expansion_entry;  // used in set_trend_bias block below
            (void)gf_vol_ratio_entry;
        }

        // ?? Pre-tick gate block -- FOUR checks before on_tick is called ???
        // GoldFlowEngine enters internally (pos.active=true inside enter()),
        // so main.cpp cannot intercept between signal and entry. All blocking
        // must happen here, before the tick reaches the engine.
        // Gate 0a: NY close noise (21:00-22:00 UTC)
        // Gate 0b: London open noise (07:00-07:15 UTC) -- same reason as bracket.
        //   Evidence: SHORT 07:00:34 SL_HIT -$7.97 -- entire $7.80 spread/sweep absorbed.
        //   GoldEngineStack already blocks its own engines in this window (line ~549).
        //   GoldFlow did NOT have this guard. This adds parity.
        //   Exception: if |ewm_drift| >= 3.0 a genuine gap-open is underway -- allow it.
        const bool in_london_open_noise_gf = false;  // REMOVED: spread/regime/SL gates are sufficient protection
        // Gate 0c: same-direction trail block (60s after trail/BE exit).
        // gold_trail_blocked is computed in the outer gate (gold_can_enter) but
        // gold_can_enter does NOT re-check direction -- it only sets can_enter=true
        // if no position is open. GoldFlow has its own entry path here and was
        // NOT checking trail_block before calling on_tick. This caused the re-entry
        // at 22:38:46 (16s after TRAIL_HIT at 22:38:30) at the exact peak -$168.
        // Fix: block gf_tick_ok when trail_block active in the SAME direction.
        // Opposite-direction entries are still allowed (reversal trades).
        const bool gf_trail_same_dir = gold_trail_blocked && [&]() -> bool {
            // g_gold_trail_block_dir: +1 = last close was LONG (block new LONGs)
            //                         -1 = last close was SHORT (block new SHORTs)
            const int  block_dir   = g_gold_trail_block_dir.load();
            const double gf_drift  = g_gold_stack.ewm_drift();
            const double gf_l2     = g_macro_ctx.gold_l2_imbalance;
            // Direction probe: use L2 if live, drift otherwise.
            // BUG FIX: when L2 is neutral (0.40-0.60) AND drift is weak (<1.0),
            // the old probe returned likely_long=false, which meant block_dir=1
            // (was LONG) never matched -- 22:38:46 LONG re-entered 51s after
            // 22:37:55 TRAIL exit because L2=0.500 and drift<1.0 failed the probe.
            // Fix: neutral L2 + weak drift = direction unknown = block BOTH.
            // Only allow a reversal entry when direction is unambiguous:
            //   L2 strongly skewed (>0.75 or <0.25), OR drift >= 2.0.
            const bool l2_strong_long  = (gf_l2 > GFE_LONG_THRESHOLD);          // >0.75
            const bool l2_strong_short = (gf_l2 < (1.0 - GFE_LONG_THRESHOLD));  // <0.25
            const bool drift_clear     = std::fabs(gf_drift) >= 2.0;
            const bool neutral_book    = !l2_strong_long && !l2_strong_short && !drift_clear;
            // If book is neutral: block regardless of direction (can't confirm reversal)
            if (neutral_book) return true;
            const bool likely_long = l2_strong_long
                                  || (!l2_strong_short && gf_drift > 1.0);
            return (block_dir ==  1 && likely_long)   // was long, about to enter long
                || (block_dir == -1 && !likely_long); // was short, about to enter short
        }();
        bool gf_tick_ok = !in_ny_close_noise && !in_london_open_noise_gf && !gf_trail_same_dir;
        const char* gf_block_reason = in_ny_close_noise        ? "NY_CLOSE_NOISE"
                                    : in_london_open_noise_gf  ? "LONDON_OPEN_NOISE"
                                    : gf_trail_same_dir        ? "TRAIL_BLOCK_SAME_DIR"
                                    : nullptr;

        // ?? Gate 0: ENGINE-CULLED check ??????????????????????????????????????????
        // engine_culled is set in handle_closed_trade after 4 consecutive SL_HITs.
        // Previously the flag was SET but never CHECKED -- entries continued after
        // the [ENGINE-CULLED] log message. Evidence: Apr 2 12:00 LONG fired after
        // cull at 11:59, taking a 5th consecutive SL loss.
        // Fix: g_gf_engine_culled atomic is set alongside engine_culled in
        // handle_closed_trade and checked here before any entry is allowed.
        // Resets to false on session rollover (midnight UTC reset in diag thread).
        if (gf_tick_ok && g_gf_engine_culled.load(std::memory_order_relaxed)) {
            static int64_t s_cull_log = 0;
            if (nowSec() - s_cull_log >= 60) {
                s_cull_log = nowSec();
                printf("[GF-GATE-BLOCK] reason=ENGINE_CULLED -- GoldFlow disabled after 4 consecutive SL_HITs\n");
                fflush(stdout);
            }
            gf_tick_ok = false;
            gf_block_reason = "ENGINE_CULLED";
        }

        // ?? Gate 0d: Bar warmup guard ????????????????????????????????????????
        // Gates 3 and 4 (RSI, M5 trend, ATR slope, VWAP coherence, spread anomaly,
        // RSI divergence, BB squeeze) all require m1_ready=true (>=52 M1 bars seeded).
        // On a cold start or post-shutdown restart, m1_ready=false for the first
        // ~2 minutes while bar history loads. During this window Gates 3+4 are
        // completely inactive -- GoldFlow has NO RSI, NO trend, NO ATR/VWAP filter.
        //
        // Evidence: 02:31:12 LONG @ 4689.01 fired 70s after restart. Process started
        // at 02:30:01, USTEC bar req got INVALID_REQUEST at 02:30:04 dropping the
        // connection. XAUUSD bars never seeded. m1_ready=false. All bar gates skipped.
        // asia_trend_ok (drift=1.59 >= 1.5) and SUPERVISOR (TREND_CONTINUATION
        // conf=1.58) passed -- entry fired naked into a downtrend. SL hit -$83.76.
        //
        // Fix: hard-block GoldFlow entries until m1_ready=true.
        // The engine will NOT enter during warmup regardless of signal strength.
        // Once seeded, all gates operate normally -- no change to live behaviour.
        if (gf_tick_ok && !g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
            // Gate 0d: block until M1 bars seeded.
            // Exception: if bars have been unavailable for > 5 min (broker doesn't
            // support trendbar protocol e.g. BlackBull UNSUPPORTED_MESSAGE), allow
            // entries without bar gates rather than blocking ALL GoldFlow permanently.
            static int64_t s_warmup_log     = 0;
            static int64_t s_bars_first_miss = 0;  // first time we saw bars not ready
            const int64_t now_wup = static_cast<int64_t>(std::time(nullptr));
            if (s_bars_first_miss == 0) s_bars_first_miss = now_wup;
            const int64_t bars_missing_secs = now_wup - s_bars_first_miss;
            const bool bars_permanently_unavailable = (bars_missing_secs > 300); // 5 min -- allows reconnect recovery before hard-blocking
            if (bars_permanently_unavailable) {
                // Broker bars permanently unavailable (INVALID_REQUEST loop).
                // BLOCK ALL ENTRIES. A blind engine with no ATR/RSI/trend filter
                // must not trade. Evidence: April 2 2026 -- bars failed all day,
                // engine entered 5 consecutive longs into a $100 crash.
                // Previous behaviour (allow with capped lot) was wrong -- lot cap
                // was not effective and entries fired at full size anyway.
                gf_tick_ok = false;
                gf_block_reason = "BARS_PERMANENTLY_UNAVAILABLE";
                if (now_wup - s_warmup_log >= 60) {
                    s_warmup_log = now_wup;
                    printf("[GF-GATE-0D] BARS_PERMANENTLY_UNAVAILABLE -- "
                           "GoldFlow BLOCKED. No ATR/RSI/trend without seeded bars. "
                           "bars_missing=%llds. Fix: restart Omega to retry bar seeding.\n",
                           (long long)bars_missing_secs);
                    fflush(stdout);
                }
                // gf_tick_ok = false -- no entry allowed without seeded bars
            } else {
                if (now_wup - s_warmup_log >= 30) {
                    s_warmup_log = now_wup;
                    printf("[GF-GATE-BLOCK] reason=BARS_NOT_READY -- GoldFlow blocked until "
                           "M1 bars seeded (need %d bars, Gates 3+4 inactive without them). "
                           "Will allow entries in %llds if bars never seed.\n",
                           52, (long long)(120 - bars_missing_secs));
                    fflush(stdout);
                }
                gf_tick_ok = false;
                gf_block_reason = "BARS_NOT_READY";
            }
        }

        // ?? Gate 0e: Compression regime block ???????????????????????????
        // During COMPRESSION with low vol_range, no directional follow-through.
        // Evidence: 01:57 -$97, 01:58 -$86, 02:31 -$84, 03:01 -$85 all fired
        // in COMPRESSION regime. Profitable runs happened during IMPULSE/TREND.
        if (gf_tick_ok) {
            const bool in_compression = (std::strcmp(gold_stack_regime, "COMPRESSION") == 0
                                      || std::strcmp(gold_stack_regime, "QUIET_COMPRESSION") == 0);
            // Hot-reloadable via omega_config.ini [gold_flow] section.
            // Asia session uses tighter floor -- thinner tape, smaller valid coils.
            const bool   gf_in_asia_slot = (g_macro_ctx.session_slot == 6);
            const double GF_COMPRESSION_VOL_FLOOR = gf_in_asia_slot
                ? g_cfg.gf_compression_vol_floor_asia
                : g_cfg.gf_compression_vol_floor;
            const double vol_range_now = g_gold_stack.vol_range();
            // vol_range=0.00 exactly means bars not yet seeded (cold start / M15 unseeded).
            // Do NOT block on unseeded vol -- we have no data, not zero volatility.
            // Also bypass during confirmed crash (RSI<35, drift<-4): the crash IS the
            // volatility signal. Blocking GoldFlow because vol_range is stale is wrong.
            const bool vol_unseeded  = (vol_range_now == 0.0);
            // Gate crash bypass: blocked for 15 min after 3 consecutive SL_HITs
            // while bypass was active (g_gf_crash_bypass_block_until set in flow_on_close).
            const bool gf_crash_bypass = crash_impulse_bypass
                && (static_cast<int64_t>(std::time(nullptr)) > g_gf_crash_bypass_block_until.load());
            // Also bypass vol floor when vwap displacement > $15 -- macro move,
            // bars not being seeded should not block an obvious directional trade.
            // Note: vwap_disp is defined in the bracket scope above -- recompute locally.
            const double gf_vwap_local = g_gold_stack.vwap();
            const double gf_mid_local  = (bid + ask) * 0.5;
            const double gf_vwap_disp  = (gf_vwap_local > 0.0)
                ? std::fabs(gf_mid_local - gf_vwap_local) : 0.0;
            // Track session-open price for displacement when VWAP is frozen (cold start).
            // VWAP is unreliable when vol_unseeded -- it reflects stale bar data.
            // Use price change from session open as a reliable displacement proxy.
            // Lowered VWAP threshold 15->8: $8 from VWAP is already a macro move on gold.
            static double s_gf_session_open_price = 0.0;
            if (s_gf_session_open_price <= 0.0) s_gf_session_open_price = gf_mid_local;
            const double gf_open_disp = std::fabs(gf_mid_local - s_gf_session_open_price);
            // Reset session open at midnight UTC
            {
                static int64_t s_gf_open_day = 0;
                const int64_t today = static_cast<int64_t>(std::time(nullptr)) / 86400;
                if (today != s_gf_open_day) {
                    s_gf_session_open_price = gf_mid_local;
                    s_gf_open_day = today;
                }
            }
            // Bypass vol floor when:
            //   a) VWAP displacement > $8 (was $15 -- too conservative for $100+ moves)
            //   b) Session-open displacement > $10 (catches frozen-VWAP cold-start scenarios)
            //   c) vol_unseeded AND open displacement > $6 (bars not seeded, use open as anchor)
            const bool macro_displacement_bypass =
                (gf_vwap_disp > 8.0)
                || (gf_open_disp > 10.0)
                || (vol_unseeded && gf_open_disp > 6.0);
            if (in_compression && !vol_unseeded && !gf_crash_bypass && !macro_displacement_bypass
                && vol_range_now >= 0.0 && vol_range_now < GF_COMPRESSION_VOL_FLOOR) {
                static int64_t s_comp_log = 0;
                if (static_cast<int64_t>(std::time(nullptr)) - s_comp_log >= 30) {
                    s_comp_log = static_cast<int64_t>(std::time(nullptr));
                    printf("[GF-GATE-BLOCK] reason=COMPRESSION_NO_VOL regime=%s "
                           "vol_range=%.2f < %.1fpts -- no directional follow-through\n",
                           gold_stack_regime, vol_range_now, GF_COMPRESSION_VOL_FLOOR);
                    fflush(stdout);
                }
                gf_tick_ok = false;
                gf_block_reason = "COMPRESSION_NO_VOL";
            }
        }

        // ?? Gate 0f: Room-to-target (R:R geometry check) ????????????????
        // Verify price has at least GF_MIN_VWAP_ROOM_R * ATR of room to VWAP
        // when VWAP is a headwind (entering against VWAP pull).
        // Root cause of 0.48x payoff ratio: entries when price is already near
        // VWAP -- move stalls, trail never fires, SL hit on reversal.
        // Exception: entering WITH VWAP momentum (moving toward it) = not a headwind.
        if (gf_tick_ok) {
            const double gf_atr_rt   = g_gold_flow.current_atr();
            const double gf_vwap_rt  = g_gold_stack.vwap();
            if (gf_atr_rt > 0.0 && gf_vwap_rt > 0.0) {
                const double gf_mid_rt   = (bid + ask) * 0.5;
                const double vwap_dist   = std::fabs(gf_mid_rt - gf_vwap_rt);
                const double gf_l2_rt    = g_macro_ctx.gold_l2_imbalance;
                const double gf_drft_rt  = g_gold_stack.ewm_drift();
                const bool   gf_long_rt  = (gf_l2_rt > GFE_LONG_THRESHOLD)
                                        || (gf_l2_rt >= 0.40 && gf_l2_rt <= 0.60 && gf_drft_rt > 1.0);
                // VWAP headwind: entering away from VWAP = VWAP will fight us
                const bool vwap_headwind = (gf_long_rt  && gf_mid_rt > gf_vwap_rt)
                                        || (!gf_long_rt && gf_mid_rt < gf_vwap_rt);
                #ifndef GF_MIN_VWAP_ROOM_R_OVERRIDE
                static constexpr double GF_MIN_VWAP_ROOM_R = 0.55; // lowered 0.75->0.55: with ATR now
                                                                     // correctly reflecting real vol (8-18pts
                                                                     // VIX25+ days), 0.75xATR = 6-13.5pts needed
                                                                     // from VWAP -- was blocking entries 9pts away.
                                                                     // 0.55xATR = 4.4-9.9pts needed. Still ensures
                                                                     // meaningful room to VWAP before entry, without
                                                                     // requiring 3/4 of a full ATR swing just to qualify.
                #else
                static constexpr double GF_MIN_VWAP_ROOM_R = GF_MIN_VWAP_ROOM_R_OVERRIDE;
                #endif
                if (vwap_headwind && vwap_dist < gf_atr_rt * GF_MIN_VWAP_ROOM_R) {
                    static int64_t s_room_log = 0;
                    if (static_cast<int64_t>(std::time(nullptr)) - s_room_log >= 30) {
                        s_room_log = static_cast<int64_t>(std::time(nullptr));
                        printf("[GF-GATE-BLOCK] reason=NO_ROOM_TO_TARGET %s "
                               "vwap=%.2f mid=%.2f dist=%.2fpts atr=%.2f need=%.2fpts (%.1fR of %.1fR)\n",
                               gf_long_rt ? "LONG" : "SHORT",
                               gf_vwap_rt, gf_mid_rt, vwap_dist, gf_atr_rt,
                               gf_atr_rt * GF_MIN_VWAP_ROOM_R,
                               vwap_dist / gf_atr_rt, GF_MIN_VWAP_ROOM_R);
                        fflush(stdout);
                    }
                    gf_tick_ok = false;
                    gf_block_reason = "NO_ROOM_TO_TARGET";
                }
            }
        }

        // ?? Gate 1: Cost viability ?????????????????????????????????????????
        // Estimates match engine sizing: sl=ATR*1.0, tp=sl*2 (2R), lot=risk/sl/100
        // ATR=0 (warmup) -> gf_tick_ok stays true; engine returns before entering.
        {
            const double gf_atr = g_gold_flow.current_atr();
            if (gf_atr > 0.0) {
                const double gf_sl_pts  = gf_atr * GFE_ATR_SL_MULT;
                const double gf_tp_dist = gf_sl_pts * 2.0;
                const double gf_lot_est = std::max(GFE_MIN_LOT,
                    std::min(0.50, g_gold_flow.risk_dollars / (gf_sl_pts * 100.0)));
                if (!ExecutionCostGuard::is_viable("XAUUSD", ask - bid, gf_tp_dist, gf_lot_est)) {
                    g_telemetry.IncrCostBlocked();
                    gf_tick_ok = false;
                    gf_block_reason = "COST_GATE";
                }
            }
        }

        // REMOVED: Gate 2 L2 microstructure edge score -- BlackBull L2 data is
        // synthetic (cTrader depth feed unreliable at this broker), scoring consistently
        // returns 0 or negative on valid setups causing false blocks.

        // ?? Gate 3: Bar indicators -- RSI + trend state from cTrader bars ??
        // Uses real M1 OHLC bars from cTrader trendbar API (not tick approximation).
        // Only active when bars are seeded (m1_ready=true, needs ?52 bars = ~52min).
        //
        // RSI gate: blocks entries into overbought (RSI>72 blocks LONG) or
        // oversold (RSI<28 blocks SHORT) conditions. Today's chart: RSI hit 80
        // before the reversal -- this gate would have prevented all those longs.
        //
        // Trend state gate (M5): if M5 shows clear downtrend (LH/LL) don't enter
        // LONG -- trade WITH structure not against it. Raises bar for counter-trend.
        //
        // ATR from bars: if bar ATR is available, use it to update GoldFlow's ATR.
        // Real H-L range > tick-based estimation, gives more accurate SL sizing.
        if (gf_tick_ok && g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
            const double bar_rsi    = g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed);
            const double bar_atr    = g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed);
            const double bar_bb_pct = g_bars_gold.m1.ind.bb_pct.load(std::memory_order_relaxed);
            // FIX 2026-04-02: replaced M5 swing trend_state with M1 EMA9/EMA50 crossover.
            // M5 swing lag = 15+ min. EMA9/EMA50 on M1 = 1-3 bar lag.
            // This is the crossover visible on the chart that we were never reading.
            // Only use EMA crossover when live bars have updated it (not stale disk state).
            const bool   gf_ema_live  = g_bars_gold.m1.ind.m1_ema_live.load(std::memory_order_relaxed);
            const double bar_ema9_gf  = gf_ema_live ? g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed) : 0.0;
            const double bar_ema50_gf = gf_ema_live ? g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed) : 0.0;
            const int    bar_trend  = (bar_ema9_gf > 0.0 && bar_ema50_gf > 0.0)
                ? (bar_ema9_gf < bar_ema50_gf ? -1 : +1)
                : 0;
            const bool   gf_long   = (g_macro_ctx.gold_l2_imbalance > GFE_LONG_THRESHOLD);

            // RSI overbought/oversold gate
            // RSI>80: extreme overbought -- LONG entries blocked
            // RSI<20: extreme oversold   -- SHORT entries blocked
            // Loosened 72/28->80/20: 72/28 was blocking valid momentum entries mid-trend
            static constexpr double RSI_OB = 80.0;  // overbought threshold
            static constexpr double RSI_OS = 20.0;  // oversold threshold
            if (gf_long && bar_rsi > RSI_OB) {
                printf("[GF-BAR-BLOCK] XAUUSD LONG blocked RSI=%.1f > %.0f (overbought)"
                       " bb_pct=%.2f trend=%d\n",
                       bar_rsi, RSI_OB, bar_bb_pct, bar_trend);
                fflush(stdout);
                gf_tick_ok = false;
                gf_block_reason = "RSI_OVERBOUGHT";
            } else if (!gf_long && bar_rsi < RSI_OS) {
                // In a momentum regime (ADX>=25 + ATR expanding), RSI<20 on a SHORT
                // is continuation not reversal -- price is in a strong downtrend.
                // Allow the entry; log but do not block.
                const bool momentum_regime = g_vol_targeter.is_momentum_regime(
                    g_bars_gold.m1.ind);
                if (momentum_regime) {
                    printf("[GF-BAR-ALLOW] XAUUSD SHORT allowed RSI=%.1f < %.0f"
                           " -- momentum regime (ADX=%.1f ATR expanding)"
                           " bb_pct=%.2f trend=%d\n",
                           bar_rsi, RSI_OS,
                           g_bars_gold.m1.ind.adx14.load(std::memory_order_relaxed),
                           bar_bb_pct, bar_trend);
                    fflush(stdout);
                } else {
                    printf("[GF-BAR-BLOCK] XAUUSD SHORT blocked RSI=%.1f < %.0f (oversold)"
                           " bb_pct=%.2f trend=%d\n",
                           bar_rsi, RSI_OS, bar_bb_pct, bar_trend);
                    fflush(stdout);
                    gf_tick_ok = false;
                    gf_block_reason = "RSI_OVERSOLD";
                }
            }

            // Trend alignment gate (M5 swing structure)
            // If M5 shows clear downtrend and signal is LONG: need RSI < 40 to enter
            // (deeply oversold pullback in downtrend only). Same for SHORT in uptrend.
            // This stops GoldFlow fading moves INTO the trend.
            if (gf_tick_ok && bar_trend != 0) {
                const bool counter_trend = (gf_long && bar_trend == -1) ||
                                           (!gf_long && bar_trend == +1);
                if (counter_trend) {
                    // ATR-proportional counter-trend RSI gate.
                    // On a normal day (ATR=5pt) the threshold is 60/40 -- valid
                    // mean-reversion pullbacks in a mild trend are allowed through.
                    // On a crash day (ATR=10pt+) the threshold tightens to 70/30 --
                    // only extreme exhaustion justifies counter-trend entry.
                    // Formula: ob = 50 + 10 * clamp(atr/5, 1.0, 2.5)
                    //          os = 50 - 10 * clamp(atr/5, 1.0, 2.5)
                    //   ATR=5pt:  ob=60  os=40  (normal day -- allow pullbacks)
                    //   ATR=10pt: ob=70  os=30  (volatile -- only extreme RSI)
                    //   ATR=20pt: ob=75  os=25  (crash -- very tight)
                    const double ct_atr_scale = std::min(2.5, std::max(1.0, gf_atr_gate / 5.0));
                    const double ct_ob = 50.0 + 10.0 * ct_atr_scale;
                    const double ct_os = 50.0 - 10.0 * ct_atr_scale;
                    const bool rsi_extreme = gf_long ? (bar_rsi > ct_ob) : (bar_rsi < ct_os);
                    if (!rsi_extreme) {
                        printf("[GF-BAR-BLOCK] XAUUSD %s blocked -- counter-trend (M5=%+d)"
                               " RSI=%.1f not extreme enough\n",
                               gf_long ? "LONG" : "SHORT", bar_trend, bar_rsi);
                        fflush(stdout);
                        gf_tick_ok = false;
                        gf_block_reason = "COUNTER_TREND";
                    }
                }
            }

            // Feed bar ATR to GoldFlow engine for accurate SL sizing
            // Bar ATR (true range) is more accurate than tick-based ATR estimation.
            // Only update when bar ATR is non-zero and reasonable (1-50pts for gold).
            if (bar_atr > 1.0 && bar_atr < 50.0) {
                g_gold_flow.seed_bar_atr(bar_atr);
            }
        }

        // ?? Composite Signal Scorer (RenTec #2 + #3) ??????????????????
        // Scores 13 conditions (16 points max). Entry allowed if score >= 5.
        // Includes cross-asset: macro regime, DXY momentum, SPX direction.
        // Hard gates (spread, bars not ready, cost, high impact) remain below.
        if (gf_tick_ok && g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
            const bool gf_long_sc = (g_macro_ctx.gold_l2_imbalance > GFE_LONG_THRESHOLD);

            // SPX rolling return: g_bars_sp.m1 EMA9 vs EMA50 direction as proxy
            // when insufficient bars, fall back to 0 (neutral -- no penalty)
            const double spx_return = [&]() -> double {
                if (!g_bars_sp.m1.ind.m1_ready.load(std::memory_order_relaxed)) return 0.0;
                const double sp_e9  = g_bars_sp.m1.ind.ema9 .load(std::memory_order_relaxed);
                const double sp_e50 = g_bars_sp.m1.ind.ema50.load(std::memory_order_relaxed);
                if (sp_e50 <= 0.0) return 0.0;
                return (sp_e9 - sp_e50) / sp_e50;  // fractional: >0 = SPX rising, <0 = falling
            }();

            const ScoreResult sr = g_signal_scorer.score_and_store(
                g_bars_gold.m1.ind,
                gf_long_sc,
                g_macro_ctx.gold_l2_imbalance,
                g_macro_ctx.gold_microprice_bias,
                g_macro_ctx.regime,
                g_macroDetector.dxyReturn(),
                spx_return);

            g_signal_scorer.log_score(sr, gf_long_sc);

            // Crowding penalty (RenTec #4) -- subtract after scoring
            // Reads last 10 closed trades for XAUUSD; if 80%+ same direction,
            // deduct 2pts. Strong setups (>=7) survive. Borderline setups blocked.
            const int crowding_penalty = g_crowding_guard.score_penalty("XAUUSD", gf_long_sc);
            const int adjusted_score   = sr.total - crowding_penalty;
            if (crowding_penalty > 0) {
                static int64_t s_crowd_log_ts = 0;
                if (nowSec() - s_crowd_log_ts >= 30) {
                    s_crowd_log_ts = nowSec();
                    printf("[CROWDING] XAUUSD %s penalty=%d score %d->%d\n",
                           gf_long_sc ? "LONG" : "SHORT",
                           crowding_penalty, sr.total, adjusted_score);
                    fflush(stdout);
                }
            }

            // Adaptive parameter gate (RenTec #7): dynamic threshold based on
            // rolling win-rate + expectancy. STRONG edge -> 4, NORMAL -> 5,
            // SOFT_WARN -> 6, FAILING -> 7. Hysteresis: ±1 per 10 trades.
            const int min_score = g_param_gate.effective_min_score("XAUUSD");

            if (adjusted_score < min_score) {
                static int64_t s_score_log_ts = 0;
                if (nowSec() - s_score_log_ts >= 15) {
                    s_score_log_ts = nowSec();
                    printf("[GF-SCORE-BLOCK] XAUUSD %s blocked score=%d crowd_adj=%d/%d < %d%s\n",
                           gf_long_sc ? "LONG" : "SHORT",
                           sr.total, adjusted_score, sr.max_points,
                           min_score,
                           crowding_penalty > 0 ? " (crowding)" : "");
                    fflush(stdout);
                }
                gf_tick_ok = false;
                gf_block_reason = crowding_penalty > 0 ? "SCORE_CROWDING" : "SCORE_BELOW_MIN";
            }
        }

        // ?? Gate 4: Hard signal quality gates (spread anomaly -- remains hard)?
        // Tick storm, ATR/VWAP coherence, BBW squeeze counter now scored above.
        // Spread anomaly stays hard: cost literally doubles at 1.5x baseline.
        if (gf_tick_ok && g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
            const bool   gf_long_g4  = (g_macro_ctx.gold_l2_imbalance > GFE_LONG_THRESHOLD);

            // ?? Gate 4a: Spread anomaly ???????????????????????????????????
            // Rolling 200-tick average spread acts as session baseline.
            // If current spread > 1.5x baseline: news hit / thin liquidity.
            // Cost doubles at this point -- no edge to justify entry.
            // Evidence: spread spikes to 5-8pts before FOMC announcements
            // while ATR is valid; old GFE_MAX_SPREAD was a flat cap, not adaptive.
            {
                const double spread_ratio = g_bars_gold.m1.ind.spread_ratio
                                                .load(std::memory_order_relaxed);
                const double spread_avg   = g_bars_gold.m1.ind.spread_avg
                                                .load(std::memory_order_relaxed);
                if (spread_avg > 0.5 && spread_ratio > OHLCBarEngine::SPREAD_WIDE_RATIO) {
                    static int64_t s_spread_gate_log = 0;
                    if (nowSec() - s_spread_gate_log >= 30) {
                        s_spread_gate_log = nowSec();
                        printf("[GF-BAR-BLOCK] XAUUSD %s blocked SPREAD_ANOMALY"
                               " spread=%.2f avg=%.2f ratio=%.2f\n",
                               gf_long_g4 ? "LONG" : "SHORT",
                               ask - bid, spread_avg, spread_ratio);
                        fflush(stdout);
                    }
                    gf_tick_ok = false;
                    gf_block_reason = "SPREAD_ANOMALY";
                }
            }

            // REMOVED: Gate 4b RSI divergence -- imprecise timing, more false blocks than real saves

            // ?? Gate 4c: Tick storm -- suppress entries during momentum rush ?
            // When tick_rate >= 8/s, price is moving fast and aggressively.
            // Block UNLESS: continuation mode OR EWM drift >3.0 (strong breakout move).
            // drift>3.0 = real directional momentum, not noise -- allow entry on breakouts.
            if (gf_tick_ok) {
                const bool   tick_storm     = g_bars_gold.m1.ind.tick_storm
                                                  .load(std::memory_order_relaxed);
                const double tick_rate      = g_bars_gold.m1.ind.tick_rate
                                                  .load(std::memory_order_relaxed);
                const bool   cont_mode      = g_gold_flow.is_in_continuation_mode();
                const double ewm_drift_abs  = std::fabs(g_gold_stack.ewm_drift());
                const bool   strong_breakout = (ewm_drift_abs >= 3.0); // real move, not chasing
                if (tick_storm && !cont_mode && !strong_breakout) {
                    static int64_t s_storm_log = 0;
                    if (nowSec() - s_storm_log >= 15) {
                        s_storm_log = nowSec();
                        printf("[GF-BAR-BLOCK] XAUUSD %s blocked TICK_STORM"
                               " rate=%.1f/s drift=%.2f -- not continuation or breakout\n",
                               gf_long_g4 ? "LONG" : "SHORT", tick_rate, ewm_drift_abs);
                        fflush(stdout);
                    }
                    gf_tick_ok = false;
                    gf_block_reason = "TICK_STORM";
                }
            }

            // ?? Gate 4d: ATR slope + VWAP direction coherence (HARD GATE) ??
            // Promoted from log-only: atr_contracting + vwap opposing signal = block.
            //   atr_expanding + vwap aligned  ? high confidence, pass
            //   atr_contracting + vwap opposing ? structural headwind, BLOCK
            //   all other combos               ? pass with log
            {
                const bool   atr_exp      = g_bars_gold.m1.ind.atr_expanding
                                                .load(std::memory_order_relaxed);
                const bool   atr_contract = g_bars_gold.m1.ind.atr_contracting
                                                .load(std::memory_order_relaxed);
                const double atr_slope    = g_bars_gold.m1.ind.atr_slope
                                                .load(std::memory_order_relaxed);
                const int    vwap_dir     = g_bars_gold.m1.ind.vwap_direction
                                                .load(std::memory_order_relaxed);
                const double vol_delta    = g_bars_gold.m1.ind.vol_delta_ratio
                                                .load(std::memory_order_relaxed);
                // Hard block: contracting vol + VWAP opposing signal direction
                const bool vwap_opposing  = (gf_long_g4  && vwap_dir == -1)
                                         || (!gf_long_g4 && vwap_dir == +1);
                // RSI extreme bypass: when RSI is at an extreme (oversold LONG or
                // overbought SHORT), the ATR contraction is the consolidation AFTER
                // the move, not during it. Gate4D should not block recovery entries
                // at RSI bottoms/tops -- these are exactly the MR/GoldFlow setups
                // that need to fire. Use same rsi_crash_lo/hi thresholds.
                const double gate4d_rsi = g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed);
                const bool gate4d_rsi_extreme = (gate4d_rsi > 0.0)
                    && ((gf_long_g4  && gate4d_rsi < rsi_crash_lo)   // oversold LONG
                     || (!gf_long_g4 && gate4d_rsi > rsi_crash_hi)); // overbought SHORT
                if (gf_tick_ok && atr_contract && vwap_opposing && !gate4d_rsi_extreme) {
                    printf("[GF-BAR-BLOCK] XAUUSD %s blocked GATE4D_ATR_CONTRACT_VWAP_OPPOSE"
                           " atr_slope=%.3f atr_con=1 vwap_dir=%+d rsi=%.1f\n",
                           gf_long_g4 ? "LONG" : "SHORT", atr_slope, vwap_dir, gate4d_rsi);
                    fflush(stdout);
                    gf_tick_ok = false;
                    gf_block_reason = "GATE4D_ATR_CONTRACT_VWAP_OPPOSE";
                }
                // Diagnostic log throttled 60s -- shows regime even when not blocking
                static int64_t s_coherence_log = 0;
                if (nowSec() - s_coherence_log >= 60) {
                    s_coherence_log = nowSec();
                    printf("[GF-BAR-INFO] XAUUSD %s atr_slope=%.3f atr_exp=%d atr_con=%d"
                           " vwap_dir=%+d vol_delta=%.3f tick_rate=%.1f gate4d=%s\n",
                           gf_long_g4 ? "LONG" : "SHORT",
                           atr_slope, atr_exp ? 1 : 0, atr_contract ? 1 : 0,
                           vwap_dir, vol_delta,
                           g_bars_gold.m1.ind.tick_rate.load(std::memory_order_relaxed),
                           (atr_contract && vwap_opposing) ? "BLOCK" : "pass");
                    fflush(stdout);
                }
            }

            // ?? Gate 4e: BBW squeeze ? log + arm breakout watch ???????????
            // bb_squeeze = price coiling at N-bar band-width minimum.
            // This is the direct fix for the GoldStack regime lag issue noted
            // in the session summary: "16:35 surge missed because GoldStack was
            // in MR regime when breakout started."
            // When squeeze fires: log it prominently. Future: wire into GoldStack
            // regime detector to override MR ? BREAKOUT_WATCH.
            {
                const bool bb_sq      = g_bars_gold.m1.ind.bb_squeeze
                                            .load(std::memory_order_relaxed);
                const int  sq_bars    = g_bars_gold.m1.ind.bb_squeeze_bars
                                            .load(std::memory_order_relaxed);
                const double bb_w     = g_bars_gold.m1.ind.bb_width
                                            .load(std::memory_order_relaxed);
                const double bb_w_min = g_bars_gold.m1.ind.bb_width_min
                                            .load(std::memory_order_relaxed);
                if (bb_sq && sq_bars == 1) {
                    // Log the moment squeeze is first detected
                    printf("[GF-BBW-SQUEEZE] XAUUSD SQUEEZE DETECTED"
                           " bb_width=%.5f bb_width_min=%.5f sq_bars=%d"
                           " -- breakout setup forming\n",
                           bb_w, bb_w_min, sq_bars);
                    fflush(stdout);
                }
                // Squeeze active: GoldFlow counter-trend entries get extra blocking.
                // When bands are coiling, price is about to break one way hard --
                // counter-trend entries into a squeeze almost always lose.
                if (gf_tick_ok && bb_sq && sq_bars >= 3) {
                    // FIX 2026-04-02: use M1 EMA crossover not M5 swing for BBW squeeze counter check
                    const bool   bsq_ema_live = g_bars_gold.m1.ind.m1_ema_live.load(std::memory_order_relaxed);
                    const double bsq_ema9  = bsq_ema_live ? g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed) : 0.0;
                    const double bsq_ema50 = bsq_ema_live ? g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed) : 0.0;
                    const int    bar_trend_sq = (bsq_ema9 > 0.0 && bsq_ema50 > 0.0)
                        ? (bsq_ema9 < bsq_ema50 ? -1 : +1) : 0;
                    const bool   counter_sq    = (gf_long_g4  && bar_trend_sq == -1)
                                              || (!gf_long_g4 && bar_trend_sq == +1);
                    if (counter_sq) {
                        printf("[GF-BAR-BLOCK] XAUUSD %s blocked BBW_SQUEEZE_COUNTER"
                               " sq_bars=%d M5_trend=%+d -- coiling against trend\n",
                               gf_long_g4 ? "LONG" : "SHORT", sq_bars, bar_trend_sq);
                        fflush(stdout);
                        gf_tick_ok = false;
                        gf_block_reason = "BBW_SQUEEZE_COUNTER";
                    }
                }
            }
        }

        // ?? VolTargeter: high-impact macro event window gate ??????????????
        // Blocks new GoldFlow entries within +-30min of any loaded HIGH-impact event.
        // Spread blows out around NFP/FOMC/CPI -- fills are unreliable.
        // Complements OmegaNewsBlackout (hardcoded windows) with precise event times.
        if (gf_tick_ok && g_vol_targeter.high_impact_window()) {
            printf("[GF-BAR-BLOCK] XAUUSD entry blocked -- high-impact macro event window\n");
            fflush(stdout);
            gf_tick_ok = false;
            gf_block_reason = "HIGH_IMPACT_WINDOW";
        }

        // ?? Gate: Directional SL cooldown (bottom-fade gate) ??????????????
        // After GF_DIR_SL_MAX consecutive SL_HITs in same direction within
        // GF_DIR_SL_WINDOW_SEC, block that direction for GF_DIR_SL_COOLDOWN_SEC.
        // Evidence: 2026-04-02 11:49-12:01 -- 5 consecutive LONG SL_HITs while
        // gold was still crashing. Daily loss gate limited size to 0.01 but
        // this gate stops entries entirely rather than letting them through small.
        // Direction is inferred the same way as trail block above: L2 + drift.
        if (gf_tick_ok) {
            const int64_t now_fade = static_cast<int64_t>(std::time(nullptr));
            const int64_t long_blocked  = g_gf_long_blocked_until.load();
            const int64_t short_blocked = g_gf_short_blocked_until.load();
            const bool any_fade_block = (now_fade < long_blocked) || (now_fade < short_blocked);
            if (any_fade_block) {
                const double gf_drift_fb = g_gold_stack.ewm_drift();
                const double gf_l2_fb    = g_macro_ctx.gold_l2_imbalance;
                // FIX 2026-04-04: drift threshold lowered from 1.0 to 0.0.
                // Root cause: on a crashing day (Apr 2) EWM drift is negative.
                // With BlackBull synthetic L2 (~0.5 neutral), the old probe
                // required drift > 1.0 to classify as LONG. Negative drift
                // made likely_long=false even though the engine was about to
                // enter LONG, so (long_blocked AND false) = NOT blocked.
                // Result: 5 consecutive LONG SL_HITs while gold was crashing.
                // Fix: use drift > 0.0 -- any positive drift (or L2 > 0.75)
                // classifies as likely LONG. Negative drift = likely SHORT.
                // This correctly identifies re-entry direction regardless of magnitude.
                const bool likely_long_fb = (gf_l2_fb > GFE_LONG_THRESHOLD)
                                         || (gf_l2_fb >= 0.40 && gf_l2_fb <= 0.60 && gf_drift_fb > 0.0);
                const bool fade_blocked = (now_fade < long_blocked  &&  likely_long_fb)
                                       || (now_fade < short_blocked && !likely_long_fb);
                if (fade_blocked) {
                    const int64_t remaining = likely_long_fb
                        ? (long_blocked  - now_fade)
                        : (short_blocked - now_fade);
                    static int64_t s_fade_log = 0;
                    if (now_fade - s_fade_log >= 15) {
                        s_fade_log = now_fade;
                        printf("[GFE-FADE-BLOCK] %s entry blocked -- %llds remaining after consecutive SL_HITs\n",
                               likely_long_fb ? "LONG" : "SHORT", (long long)remaining);
                        fflush(stdout);
                    }
                    gf_tick_ok = false;
                    gf_block_reason = "DIR_SL_COOLDOWN";
                }
            }
        }

        // ?? VolTargeter: full diagnostic log on every valid entry attempt ??
        // Prints ADX, EWMA vol, vol_target_mult, momentum regime, high-impact flag.
        if (gf_tick_ok) {
            g_vol_targeter.log_state(g_bars_gold.m1.ind);
        }

        // ?? Diagnostic: which gate is holding, throttled 30s ??????????????
        // Fires whenever gf_tick_ok=false so logs show exactly what is blocking.
        // Replaces the opaque "NO TRADES FIRING" GUI banner with a specific reason.
        if (!gf_tick_ok) {
            static int64_t s_gf_gate_log_ts = 0;
            if (nowSec() - s_gf_gate_log_ts >= 30) {
                s_gf_gate_log_ts = nowSec();
                const bool gf_dir = (g_macro_ctx.gold_l2_imbalance > GFE_LONG_THRESHOLD);
                const double spread_ratio = g_bars_gold.m1.ind.spread_ratio
                                                .load(std::memory_order_relaxed);
                const double tick_rate    = g_bars_gold.m1.ind.tick_rate
                                                .load(std::memory_order_relaxed);
                const bool   bb_sq        = g_bars_gold.m1.ind.bb_squeeze
                                                .load(std::memory_order_relaxed);
                const bool   bull_div     = g_bars_gold.m1.ind.rsi_bull_div
                                                .load(std::memory_order_relaxed);
                const bool   bear_div     = g_bars_gold.m1.ind.rsi_bear_div
                                                .load(std::memory_order_relaxed);
                printf("[GF-GATE-BLOCK] reason=%s atr=%.2f spread=%.3f spread_ratio=%.2f "
                       "l2=%.3f micro=%.4f "
                       "wall_below=%d wall_above=%d absorb=%d vac_ask=%d vac_bid=%d "
                       "trail_block=%d post_impulse=%d ny_noise=%d gold_can_enter=%d "
                       "tick_rate=%.1f bb_sq=%d bull_div=%d bear_div=%d\n",
                       gf_block_reason ? gf_block_reason : "UNKNOWN",
                       g_gold_flow.current_atr(), ask - bid, spread_ratio,
                       g_macro_ctx.gold_l2_imbalance,
                       g_macro_ctx.gold_microprice_bias,
                       g_macro_ctx.gold_wall_below ? 1 : 0,
                       g_macro_ctx.gold_wall_above ? 1 : 0,
                       g_edges.absorption.is_absorbing("XAUUSD", gf_dir) ? 1 : 0,
                       g_macro_ctx.gold_vacuum_ask ? 1 : 0,
                       g_macro_ctx.gold_vacuum_bid ? 1 : 0,
                       gold_trail_blocked ? 1 : 0,
                       gold_post_impulse_block ? 1 : 0,
                       in_ny_close_noise ? 1 : 0,
                       gold_can_enter ? 1 : 0,
                       tick_rate, bb_sq ? 1 : 0,
                       bull_div ? 1 : 0, bear_div ? 1 : 0);
                fflush(stdout);
                // Store for health watchdog GUI display
                if (gf_block_reason) g_last_gf_block_reason.store(gf_block_reason, std::memory_order_relaxed);
            }
        }

        // Entry callback: used for on_tick when no position open (new entries).
        // Includes crash-bypass consec counter in addition to shared close logic.
        auto flow_on_close = [&](const omega::TradeRecord& tr) {
            if (tr.exitReason == std::string("PARTIAL_1R")) {
                if (g_cfg.mode == "LIVE") {
                    send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
                }
                handle_closed_trade(tr);
                return;
            }
            handle_closed_trade(tr);
            send_live_order("XAUUSD", tr.side == "SHORT", tr.size, tr.exitPrice);
            // Shared close state: reversal window, dir SL cooldown, trail block
            gf_close_shared(tr);
            // crash_impulse_bypass consecutive-SL cooldown (entry block only):
            // After GF_CRASH_BYPASS_CONSEC_SL_MAX consecutive SL_HITs,
            // block crash bypass gate for GF_CRASH_BYPASS_COOLDOWN_SEC.
            const bool is_sl = (tr.exitReason == "SL_HIT");
            if (is_sl) {
                const int64_t now_s  = static_cast<int64_t>(std::time(nullptr));
                const int new_consec = g_gf_crash_consec_sl.fetch_add(1) + 1;
                if (new_consec >= GF_CRASH_BYPASS_CONSEC_SL_MAX) {
                    const int64_t block_until = now_s + GF_CRASH_BYPASS_COOLDOWN_SEC;
                    g_gf_crash_bypass_block_until.store(block_until);
                    g_gf_crash_consec_sl.store(0);
                    printf("[GF-CRASH-BYPASS-COOLDOWN] %d consecutive SL_HITs -- "
                           "crash_impulse_bypass BLOCKED for %llds (until epoch %lld)\n",
                           GF_CRASH_BYPASS_CONSEC_SL_MAX,
                           static_cast<long long>(GF_CRASH_BYPASS_COOLDOWN_SEC),
                           static_cast<long long>(block_until));
                    fflush(stdout);
                }
            } else {
                g_gf_crash_consec_sl.store(0);
            }
        };
        // Entry only: on_tick for new entries when no position is open.
        // Position management (SL/trail) is handled in the unconditional
        // manage block above this entry guard.
        // L2 LIVENESS GATE -- cTrader ctid=43014358 is the basis of all GoldFlow edge.
        // When L2 watchdog confirms feed dead > 120s, block new entries entirely.
        // Drift-only mode has no proven edge (backtest: 63% WR, negative P&L).
        // Position management (trail/SL) runs unconditionally regardless of this gate.
        // Gate clears automatically when L2 recovers (watchdog resets the atomic).
        if (g_l2_watchdog_dead.load(std::memory_order_relaxed)) {
            static int64_t s_l2_dead_log = 0;
            if (now_ms_g - s_l2_dead_log > 60000) {
                s_l2_dead_log = now_ms_g;
                printf("[GFE-GATE] L2 DEAD -- GoldFlow entries BLOCKED (ctid=43014358 not flowing)\n"
                       "[GFE-GATE] Check C:\\Omega\\logs\\L2_ALERT.txt for details\n");
                fflush(stdout);
            }
            // Skip entry block entirely -- fall through to manage open positions only
            goto gfe_entry_skip;
        }

        // ?? FEED-STALE gate ??????????????????????????????????????????????????????????
        // Blocks ALL new XAUUSD entries when cTrader depth is subscribed but delivering
        // zero events -- the exact failure mode that produced 452 frozen ticks (4677.03/
        // 4677.25) for an entire session while the real market moved $40.
        //
        // g_feed_stale_xauusd is set by CTraderDepthClient per-symbol starvation watchdog
        // (fires every 60s, detects zero XAUUSD events after 90s grace period).
        // It is cleared only when cTrader depth events resume flowing for XAUUSD.
        //
        // Position management (trail/SL) is unaffected -- this gate is entry-only.
        // The starvation watchdog also escalates: level-1 re-subscribe, level-2 reconnect.
        // This gate is the trading-layer safety net in case escalation takes time.
        if (g_feed_stale_xauusd.load(std::memory_order_relaxed)) {
            static int64_t s_feed_stale_log = 0;
            if (now_ms_g - s_feed_stale_log > 60000) {
                s_feed_stale_log = now_ms_g;
                printf("[FEED-STALE] XAUUSD depth starved -- all new entries BLOCKED. "                       "CTraderDepthClient is escalating (resub -> reconnect).\n");
                fflush(stdout);
            }
            goto gfe_entry_skip;
        }
        if (g_cfg.goldflow_enabled && gf_tick_ok) {
            // ?? ASIA REVERSAL DRIFT SNAP -- no prior position required ???????????
            // Root cause (2026-04-07): the post-close drift reset below only fires when
            // GFE was previously open (gf_close_ts > 0). When the initial drop happened
            // without GFE being in the trade, gf_close_ts=0 forever. The drift from the
            // move it missed stays negative for 150+ ticks -- GFE cannot enter the reversal.
            //
            // Fix: when RSI reverses sharply from extreme (<35 rising, or >65 falling),
            // snap drift and clear persistence immediately regardless of prior position state.
            //
            // Gate design (conservative to avoid false snaps on sideways tape):
            //   1. RSI must cross the reversal zone (< 35 or > 65 on this tick)
            //   2. RSI must be MOVING in the reversal direction (not just at extreme)
            //      -- achieved by requiring drift OPPOSES the RSI extreme signal
            //   3. Asia slot (slot=6) or strong VWAP displacement (>= 8pt)
            //      -- outside Asia use the existing post-close snap only
            //   4. GFE has no open position (not managing an existing trade)
            //   5. One-shot per RSI event (40s cooldown -- RSI can stay extreme many ticks)
            //   6. Not in GFE cooldown (would fire then immediately block entry)
            {
                static int64_t s_asia_rsi_snap_ts = 0;
                const int64_t now_snap = static_cast<int64_t>(std::time(nullptr));
                const double snap_rsi    = g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed);
                const double snap_drift  = g_gold_stack.ewm_drift();
                const bool   in_asia_snap = (g_macro_ctx.session_slot == 6);
                const double snap_vwap    = g_gold_stack.vwap();
                const double snap_mid     = (bid + ask) * 0.5;
                const double snap_vdisp   = (snap_vwap > 0.0) ? std::fabs(snap_mid - snap_vwap) : 0.0;
                // RSI reversal conditions: RSI extreme + drift opposing (stale from prior move)
                const bool rsi_reversal_long  = (snap_rsi > 0.0 && snap_rsi < 35.0)
                                             && (snap_drift < -1.0);  // stale bearish drift blocking LONG
                const bool rsi_reversal_short = (snap_rsi > 0.0 && snap_rsi > 65.0)
                                             && (snap_drift >  1.0);  // stale bullish drift blocking SHORT
                const bool session_ok_snap    = in_asia_snap || (snap_vdisp >= 8.0);
                const bool gfe_flat           = !g_gold_flow.has_open_position();
                const bool snap_cooldown_ok   = (now_snap - s_asia_rsi_snap_ts) >= 40;
                if ((rsi_reversal_long || rsi_reversal_short)
                    && session_ok_snap
                    && gfe_flat
                    && snap_cooldown_ok
                    && !g_gold_flow.is_in_cooldown()) {
                    s_asia_rsi_snap_ts = now_snap;
                    const double snap_reversal_est = std::fabs(snap_drift) * 2.0;  // use drift magnitude as reversal proxy
                    g_gold_stack.reset_drift_on_reversal(snap_reversal_est);
                    g_gold_flow.reset_drift_persistence();
                    printf("[ASIA-RSI-SNAP] RSI=%.1f drift=%.2f -> snap fired (no prior GFE position needed) "
                           "reversal_est=%.1f slot=%d vdisp=%.1f\n",
                           snap_rsi, snap_drift, snap_reversal_est,
                           g_macro_ctx.session_slot, snap_vdisp);
                    fflush(stdout);
                }
            }

            // ?? Post-close reversal drift reset ???????????????????????????
            // Problem: ewm_slow (?=0.005) has a 200-tick half-life. After a
            // 60pt DROP it reaches drift?-40. When price then SURGES 80pts,
            // drift stays negative for 150+ ticks (~25 min). GFE cannot enter
            // LONG because drift < threshold -- entire surge missed.
            //
            // Fix: when GoldFlow has closed AND price has moved >= 2?ATR in the
            // OPPOSITE direction since the close, snap ewm_slow toward ewm_fast.
            // Snap magnitude is proportional to reversal size (5pt=20%, 60pt=95%).
            // This removes stale directional memory so new ticks immediately
            // establish the reversed drift. Fresh entry fires within ~3-5 ticks.
            //
            // Safety gates:
            //   1. Only fires when GFE has NO open position (pos.active=false)
            //   2. Minimum 2?ATR reversal required (2?2.5=5pt floor) -- noise immune
            //   3. 120s window: only resets within 2min of the close
            //   4. One-shot per close: g_drift_reset_done flag prevents repeat resets
            //   5. Does NOT fire if GFE is in cooldown (wrong direction would enter)
            {
                static bool s_drift_reset_done = false;
                static int64_t s_last_close_ts = 0;
                const int64_t  gf_close_ts  = g_gold_flow_exit_ts.load();
                const int      gf_close_dir = g_gold_flow_exit_dir.load();
                const int64_t  exit_px_x100 = g_gold_flow_exit_price_x100.load();
                const double   exit_px      = exit_px_x100 > 0
                                              ? static_cast<double>(exit_px_x100) / 100.0
                                              : 0.0;
                // Reset the done flag when a new close is recorded
                if (gf_close_ts != s_last_close_ts) {
                    s_drift_reset_done = false;
                    s_last_close_ts    = gf_close_ts;
                }
                if (!s_drift_reset_done
                    && !g_gold_flow.has_open_position()
                    && gf_close_ts > 0
                    && exit_px > 0.0
                    && (now_ms_g / 1000 - gf_close_ts) <= 120)  // within 2 min of close
                {
                    const double gf_atr       = std::max(2.5, g_gold_flow.current_atr());
                    const double min_reversal  = gf_atr * 2.0;  // 5pt floor at default ATR
                    const double reversal_dist = (gf_close_dir == -1)  // was SHORT
                        ? (bid - exit_px)   // reversal = price moved UP since close
                        : (exit_px - bid);  // was LONG, reversal = price moved DOWN
                    if (reversal_dist >= min_reversal) {
                        // Don't snap during cooldown -- GFE won't enter anyway,
                        // and the snap would be wasted. Wait until cooldown clears.
                        if (!g_gold_flow.is_in_cooldown()) {
                            g_gold_stack.reset_drift_on_reversal(reversal_dist);
                            // Also clear GFE's direction persistence windows so
                            // fresh reversal ticks build the new direction signal
                            // immediately rather than fighting 20 old opposing ticks.
                            g_gold_flow.reset_drift_persistence();
                            s_drift_reset_done = true;  // one-shot per close
                            printf("[DRIFT-RESET] GFE close_dir=%+d exit=%.2f now=%.2f reversal=%.1fpt (min=%.1f) -- drift+persistence snapped\n",
                                   gf_close_dir, exit_px, bid, reversal_dist, min_reversal);
                            fflush(stdout);
                        }
                    }
                }
            }

            // ?? Inject macro trend bias before each tick ??????????????????
            // gold_momentum = (mid - VWAP) / mid * 100 -- computed above from GoldStack
            // gold_sdec.confidence and regime from SymbolSupervisor -- computed above
            // These block GFE from entering counter-trend on strong directional days.
            const bool sup_trend = (gold_sdec.regime == omega::Regime::TREND_CONTINUATION);
            // wall_ahead: significant L2 wall within 2?ATR ahead of current price.
            // Used by GFE to tighten Stage 2 trail before the wall absorbs momentum.
            const bool gf_wall_ahead = g_gold_flow.has_open_position()
                && (g_gold_flow.pos.is_long  ? g_macro_ctx.gold_wall_above
                                             : g_macro_ctx.gold_wall_below);
            // ?? VPIN pre-entry gate -- do NOT call on_tick when flow is toxic ????
            // When VPIN >= 0.70, informed traders dominate; entering here risks
            // adverse selection. on_tick is skipped entirely so no new position
            // is opened. Management (trail/exit) of existing positions is handled
            // in the has_open_position() block below -- always runs regardless.
            bool gf_vpin_ok = true;
            if (!g_gold_flow.has_open_position() && g_vpin.warmed() && g_vpin.toxic()) {
                static int64_t s_gf_vpin_log = 0;
                if (nowSec() - s_gf_vpin_log > 15) {
                    s_gf_vpin_log = nowSec();
                    std::printf("[VPIN-GF] GoldFlow entry blocked: vpin=%.3f toxic\n",
                                g_vpin.vpin());
                    std::fflush(stdout);
                }
                gf_vpin_ok = false;
            }
            if (gf_vpin_ok) {
            // ATR-based expansion flag: wide trail activates when ATR > 2x normal (10pt+)
            // OR when supervisor confirms EXPANSION_BREAKOUT/TREND_CONTINUATION.
            // This catches spikes where ATR expands before the regime label catches up.
            const bool gf_expansion_entry = (gold_sdec.regime == omega::Regime::EXPANSION_BREAKOUT
                                          || gold_sdec.regime == omega::Regime::TREND_CONTINUATION)
                                          || (gf_atr_gate > 10.0);  // ATR > 2x normal = expansion
            const double gf_vol_ratio_entry = (g_gold_stack.recent_vol_pct() > 0.0
                                            && g_gold_stack.base_vol_pct() > 0.0)
                ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 1.0;
            // For velocity trail: vol_ratio gate inside GFE requires > 2.5.
            // When ATR > 10pt but vol_ratio < 2.5 (regime label lagging), inject
            // a synthetic ratio of 2.6 so the velocity trail arms correctly.
            const double gf_vol_ratio_injected = (gf_atr_gate > 10.0 && gf_vol_ratio_entry < 2.5)
                ? 2.6 : gf_vol_ratio_entry;
            g_gold_flow.set_trend_bias(gold_momentum, gold_sdec.confidence,
                                       sup_trend, gf_wall_ahead, gold_vwap_pts,
                                       gf_expansion_entry, gf_vol_ratio_injected);

            // ── L2 tick logger ────────────────────────────────────────────
            // Writes per-tick L2 data so we can backtest with real imbalance.
            // Daily rotating CSV: C:\Omega\logs\l2_ticks_YYYY-MM-DD.csv
            {
                static FILE*   s_l2f     = nullptr;
                static int     s_l2_day  = -1;
                const time_t   t_l2      = (time_t)(now_ms_g / 1000);
                struct tm      tm_l2{};
                gmtime_s(&tm_l2, &t_l2);
                if (tm_l2.tm_yday != s_l2_day) {
                    if (s_l2f) { fclose(s_l2f); s_l2f = nullptr; }
                    char l2path[256];
                    snprintf(l2path, sizeof(l2path),
                        "C:\\Omega\\logs\\l2_ticks_%04d-%02d-%02d.csv",
                        tm_l2.tm_year+1900, tm_l2.tm_mon+1, tm_l2.tm_mday);
                    bool is_new = (GetFileAttributesA(l2path) == INVALID_FILE_ATTRIBUTES);
                    s_l2f = fopen(l2path, "a");
                    if (s_l2f && is_new)
                        fprintf(s_l2f,
                            "ts_ms,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,"
                            "vol_ratio,regime,vpin,has_pos\n");
                    s_l2_day = tm_l2.tm_yday;
                }
                if (s_l2f) {
                    // Get bid/ask volumes from L2 book
                    double l2_bvol = 0.0, l2_avol = 0.0;
                    {
                        std::lock_guard<std::mutex> lk(g_l2_mtx);
                        // Depth client writes book under "XAUUSD" key (internal_name)
                        // NOT "GOLD.F" -- that was causing bid/ask vol to always be 0
                        auto it = g_l2_books.find("XAUUSD");
                        if (it == g_l2_books.end())
                            it = g_l2_books.find("GOLD.F");  // fallback for old alias
                        if (it != g_l2_books.end()) {
                            for (int _i = 0; _i < it->second.bid_count; ++_i) l2_bvol += it->second.bids[_i].size;
                            for (int _i = 0; _i < it->second.ask_count; ++_i) l2_avol += it->second.asks[_i].size;
                        }
                    }
                    fprintf(s_l2f,
                        "%lld,%.3f,%.3f,%.4f,%.0f,%.0f,%.3f,%d,%.3f,%d\n",
                        (long long)now_ms_g, bid, ask,
                        g_macro_ctx.gold_l2_imbalance,
                        l2_bvol, l2_avol,
                        gf_vol_ratio_entry,
                        (int)gold_sdec.regime,
                        g_vpin.warmed() ? g_vpin.vpin() : 0.0,
                        (int)g_gold_flow.has_open_position());
                    fflush(s_l2f);
                }
            }
            // ── end L2 tick logger ────────────────────────────────────────

            g_gold_flow.on_tick(bid, ask,
                g_macro_ctx.gold_l2_imbalance,
                g_gold_stack.ewm_drift(),
                now_ms_g, flow_on_close,
                g_macro_ctx.session_slot);
            }  // gf_vpin_ok
        }
        gfe_entry_skip:;  // L2 watchdog gate jumps here when L2 is dead
        if (g_gold_flow.has_open_position()) {
            // ?? Post-entry: apply regime weight + adaptive risk sizing ?????
            // GoldFlowEngine computes lot = risk_dollars / (sl_pts ? 100) internally.
            // It cannot apply regime weight (GOLD_FLOW: 0.80 RISK_ON, 1.50 RISK_OFF),
            // Kelly scaling, DD throttle, or vol regime scaler -- those require the
            // AdaptiveRiskManager which lives in main.cpp.
            // Pattern: same as TrendPB (patch_size after entry, before partial exit arm).
            // The engine's internal lot is the BASE -- we scale it here, then overwrite.
            {
                const double gf_regime_wt = static_cast<double>(
                    g_regime_adaptor.weight(omega::regime::EngineClass::GOLD_FLOW));
                double gf_daily_loss = 0.0; int gf_consec = 0;
                {
                    std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
                    auto it = g_sym_risk.find("XAUUSD");
                    if (it != g_sym_risk.end()) {
                        gf_daily_loss = std::max(0.0, -it->second.daily_pnl);
                        gf_consec     = it->second.consec_losses;
                    }
                }
                // Base lot = what the engine computed (risk_dollars / sl / 100)
                const double gf_base = g_gold_flow.pos.size;
                // Apply regime weight, then adaptive risk (Kelly + DD throttle + vol)
                const double gf_adjusted = g_adaptive_risk.adjusted_lot(
                    "XAUUSD",
                    gf_base * gf_regime_wt,
                    gf_daily_loss, g_cfg.daily_loss_limit, gf_consec);
                // Hard clamp: max_lot_gold is the safety ceiling
                const double gf_final = std::max(GFE_MIN_LOT,
                    std::min(gf_adjusted, g_cfg.max_lot_gold));
                // ?? Cross-asset size filters ??? [RENTECH GAP #1, #3] ?????
                // Apply DXY momentum, HTF bias, and macro regime soft filters.
                // These reduce size (never block) -- gold can be safe haven in RISK_OFF.
                double gf_cross_mult = 1.0;
                {
                    const bool gf_is_long = g_gold_flow.pos.is_long;

                    // 1. DXY MOMENTUM (cross-asset filter)
                    // Rising DXY = USD strength = headwind for gold LONG.
                    // Falling DXY = USD weakness = tailwind for gold LONG.
                    // dxyReturn() = fractional return over last 60 DX.F ticks.
                    const double dxy_ret = g_macroDetector.dxyReturn();
                    const double dxy_risk_off_thr = g_macroDetector.DXY_RISK_OFF_PCT / 100.0;
                    if (gf_is_long && dxy_ret > dxy_risk_off_thr) {
                        // DXY rising fast -- gold LONG opposes dollar strength
                        gf_cross_mult *= 0.70;
                        printf("[XA-FILTER] XAUUSD LONG vs rising DXY (ret=%.4f) -- size x0.70\n", dxy_ret);
                    } else if (!gf_is_long && dxy_ret > dxy_risk_off_thr) {
                        // DXY rising + gold SHORT -- momentum aligned, slight boost
                        gf_cross_mult *= 1.10;
                    } else if (gf_is_long && dxy_ret < -dxy_risk_off_thr) {
                        // DXY falling + gold LONG -- momentum aligned, slight boost
                        gf_cross_mult *= 1.10;
                    }

                    // 2. HTF BIAS FILTER (Jane Street 2-TF agreement rule)
                    // HTFBiasFilter tracks daily + intraday price direction.
                    // Opposing bias = halve size. Aligned = full size. Neutral = 0.75x.
                    const double htf_mult = g_htf_filter.size_scale("XAUUSD", gf_is_long);
                    if (htf_mult < 1.0) {
                        printf("[XA-FILTER] XAUUSD %s HTF bias=%s -- size x%.2f\n",
                               gf_is_long ? "LONG" : "SHORT",
                               g_htf_filter.bias_name("XAUUSD"), htf_mult);
                    }
                    gf_cross_mult *= htf_mult;

                    // 3. MACRO REGIME (VIX + DXY combined)
                    // RISK_OFF: gold is safe haven -- don't reduce LONGs, reduce SHORTs
                    // RISK_ON:  gold loses safe haven bid -- reduce LONGs slightly
                    const std::string& macro = g_macro_ctx.regime;
                    if (macro == "RISK_OFF" && !gf_is_long) {
                        // Shorting gold in a flight-to-safety = low edge
                        gf_cross_mult *= 0.60;
                        printf("[XA-FILTER] XAUUSD SHORT in RISK_OFF -- size x0.60\n");
                    } else if (macro == "RISK_ON" && gf_is_long) {
                        // Gold LONG when risk appetite is high = mild headwind
                        gf_cross_mult *= 0.85;
                    }

                    gf_cross_mult = std::max(0.30, std::min(1.20, gf_cross_mult)); // clamp [0.30, 1.20]
                }
                const double gf_final_xa = std::max(GFE_MIN_LOT,
                    std::min(gf_final * gf_cross_mult, g_cfg.max_lot_gold));

                // Max loss per trade dollar cap (same backstop as gold stack path)
                const double gf_sl_abs = std::fabs(g_gold_flow.pos.entry - g_gold_flow.pos.sl);
                double gf_lot = gf_final_xa;
                if (g_cfg.max_loss_per_trade_usd > 0.0 && gf_sl_abs > 0.0) {
                    const double max_loss_lot = g_cfg.max_loss_per_trade_usd / (gf_sl_abs * 100.0);
                    if (gf_lot > max_loss_lot) {
                        gf_lot = std::max(GFE_MIN_LOT,
                            std::floor(max_loss_lot * 100.0 + 0.5) / 100.0);
                        printf("[MAX-LOSS-CAP] XAUUSD FLOW lot capped %.4f?%.4f (sl=$%.2f max=$%.0f)\n",
                               gf_final, gf_lot, gf_sl_abs * 100.0 * gf_final, g_cfg.max_loss_per_trade_usd);
                    }
                }
                if (gf_lot != g_gold_flow.pos.size) {
                    printf("[GF-SIZE] XAUUSD FLOW size %.4f?%.4f (regime_wt=%.2f kelly/dd/vol applied)\n",
                           g_gold_flow.pos.size, gf_lot, gf_regime_wt);
                    g_gold_flow.pos.size = gf_lot;  // patch directly -- pos is public
                }
            }

            // Flow engine entered -- portfolio risk tracking + telemetry
            {
                const double gf_sl_abs = std::fabs(g_gold_flow.pos.entry - g_gold_flow.pos.sl);
                portfolio_sl_risk_add(gf_sl_abs, g_gold_flow.pos.size, 100.0);
            }
            g_telemetry.UpdateLastSignal("XAUUSD",
                g_gold_flow.pos.is_long ? "LONG" : "SHORT",
                g_gold_flow.pos.entry, "L2_FLOW",
                "FLOW", regime.c_str(), "GOLD_FLOW",
                0.0, g_gold_flow.pos.sl);
            g_telemetry.UpdateLastEntryTs();  // watchdog: GoldFlow entry counts as activity
            // Log entry
            write_trade_open_log("XAUUSD", "GoldFlow",
                g_gold_flow.pos.is_long ? "LONG" : "SHORT",
                g_gold_flow.pos.entry, 0.0, g_gold_flow.pos.sl,
                g_gold_flow.pos.size, ask - bid, regime, "L2_FLOW");
            // NOTE: g_partial_exit is NOT armed here for GFE.
            // GoldFlowEngine manages its own partial exit internally via
            // manage_position() ? PARTIAL_1R callback (GFE_PARTIAL_EXIT_R, 50% at 1R).
            // The g_partial_exit tick check is skipped when gfe_owns_partial=true
            // to prevent duplicate broker orders. GoldStack arms g_partial_exit
            // directly at signal time for its own positions.
        }
        }  // end gf_impulse_stable else
    }

    // LatencyEdge: not supervisor-gated (intermarket/latency signal)
    // Full exclusion: checks ALL other gold engines to prevent stacking.
    // Previously only checked bracket + stack -- GoldFlow and TrendPB were missing.
    if (!g_bracket_gold.has_open_position()
        && !g_gold_stack.has_open_position()
        && !g_gold_flow.has_open_position()      // ADDED: prevent stack with Flow
        && !g_trend_pb_gold.has_open_position()) { // ADDED: prevent stack with TrendPB
        const auto le_sig = g_le_stack.on_tick_gold(bid, ask, rtt_check, ca_on_close, gold_can_enter);
        if (le_sig.valid) {
            g_telemetry.UpdateLastSignal("XAUUSD",
                le_sig.is_long ? "LONG" : "SHORT", le_sig.entry, le_sig.reason,
                "LEAD_LAG", regime.c_str(), "LE",
                le_sig.tp, le_sig.sl);
            printf("[LE-SIZE] XAUUSD eng=%s sl_abs=%.2f spread=%.2f (enter_directional)\n",
                   le_sig.engine, std::fabs(le_sig.entry - le_sig.sl), ask - bid);
            enter_directional("XAUUSD", le_sig.is_long, le_sig.entry, le_sig.sl, le_sig.tp, le_sig.size, false, bid, ask, sym, regime);
        }
    }
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
    // ── Improvement 5: CVD confirmation gate ──────────────────────────────
    g_trend_pb_gold.seed_cvd(g_macro_ctx.gold_cvd_dir);

    // ── Improvement 1: Volatility regime scaling ──────────────────────────
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
    // ── Improvement 7: News proximity ─────────────────────────────────────
    g_trend_pb_gold.seed_news_secs(
        g_news_blackout.secs_until_next(static_cast<int64_t>(std::time(nullptr))));
    // TrendPullback gold position management -- always runs when position open
    if (g_trend_pb_gold.has_open_position()) {
        g_trend_pb_gold.on_tick("XAUUSD", bid, ask, ca_on_close);
    }

    // Trend Pullback: EMA9/21/50 -- only when no other XAUUSD position is open.
    // TrendPullback: M15 swing trades (1-3hr hold, 20-50pt targets).
    // Can run CONCURRENTLY with GoldFlow (10s scalp) -- different timeframes,
    // different position sizes, independent SL/TP levels, no conflict.
    // Still blocked by bracket and LatencyEdge (those are structural/speed trades
    // that would directly conflict with a swing position at the same level).
    // GoldStack (tick-pattern engine) also blocked -- shares exact same entry zone.
    // TrendPullback gold -- re-enabled with tick-EMA-correct TP (ATR-based, not EMA9)
    // and widened pullback band (0.15% not 0.05%).
    // Does NOT require bar data -- runs on tick EMAs with proper time-equivalent alphas.
    // TrendPullback gold: 24h entry gate -- trend is a trend regardless of session.
    // Uses symbol_gate (risk/max_positions, tradeable, lat_ok, regime, bid, ask) but NOT session slot gate.
    // Only hard blocks: dead-zone spread spike window (05:00-06:30 UTC) and NY close noise.
    const bool tpb_gold_session_ok = !in_ny_close_noise && (gold_session_slot != 0 || [&](){
        // Allow slot 0 (05:00-07:00) ONLY after 06:30 when spreads have normalised
        struct tm ti_s{}; auto t_s = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        gmtime_s(&ti_s, &t_s);
        return (ti_s.tm_hour * 60 + ti_s.tm_min) >= 390; // 06:30 UTC
    }());
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
                printf("[CRASH-OVERRIDE] drift=%.2f RSI=%.1f -- TrendPB cooldown bypassed\n",
                       drift_now, rsi_now);
                fflush(stdout);
            }
        }
    }

    if (tpb_gold_can_enter
        && !g_bracket_gold.has_open_position()
        && !g_gold_stack.has_open_position()
        && !g_le_stack.has_open_position()
        && !g_trend_pb_gold.has_open_position()) {
        const auto tpb = g_trend_pb_gold.on_tick("XAUUSD", bid, ask, ca_on_close);
        if (tpb.valid) {
            const double gold_drift = g_gold_stack.ewm_drift();
            const bool drift_ok = (tpb.is_long  && gold_drift >= -1.0) ||
                                  (!tpb.is_long && gold_drift <=  1.0);
            if (!drift_ok) {
                printf("[TRENDPB-GOLD] %s suppressed -- EWM drift=%.2f opposes direction\n",
                       tpb.is_long ? "LONG" : "SHORT", gold_drift);
                fflush(stdout);
                g_trend_pb_gold.cancel();
            } else {
                const double tpb_lot = enter_directional("XAUUSD", tpb.is_long, tpb.entry, tpb.sl, tpb.tp, 0.01, true, bid, ask, sym, regime);
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

    // ── Improvement 8: Pyramid add-on on second EMA50 pullback ─────────────
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
            if (enter_directional("XAUUSD", pyr_long, pyr_mid, pyr_sl, pyr_tp, add_lot, true, bid, ask, sym, regime)) {
                ++g_trend_pb_gold.pyramid_adds_;
                printf("[TRENDPB-GOLD] PYRAMID ADD #%d lot=%.4f sl=%.3f tp=%.3f\n",
                       g_trend_pb_gold.pyramid_adds_, add_lot, pyr_sl, pyr_tp);
                fflush(stdout);
            }
        }
    }

    // ?? GoldHybridBracketEngine -- compression range -> both sides -> cancel loser ??
    // SHADOW mode by default. Fires when:
    //   1. Compression range detected (MIN_RANGE=1.5pt, MAX_RANGE=12pt over 30 ticks)
    //   2. GoldFlow NOT in unprotected live position
    //   3. Flow pyramid bypass: fires alongside GoldFlow when be_locked + trail_stage >= 1
    // Both a long stop above hi AND a short stop below lo are sent simultaneously.
    // First fill becomes the position; the other order is cancelled via cancel_fn.
    {
        // Position management -- unconditional when live
        if (g_hybrid_gold.has_open_position()) {
            const bool flow_live    = g_gold_flow.has_open_position();
            const bool flow_be     = g_gold_flow.pos.be_locked;
            const int  flow_stage  = g_gold_flow.pos.trail_stage;
            g_hybrid_gold.on_tick(bid, ask, now_ms_g,
                                  gold_can_enter, flow_live, flow_be, flow_stage,
                                  bracket_on_close);
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
            && !g_le_stack.has_open_position()
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
            const bool flow_live   = g_gold_flow.has_open_position();
            const bool flow_be     = g_gold_flow.pos.be_locked;
            const int  flow_stage  = g_gold_flow.pos.trail_stage;
            g_hybrid_gold.on_tick(bid, ask, now_ms_g,
                                  hybrid_can_enter, flow_live, flow_be, flow_stage,
                                  bracket_on_close);
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
            if (h_hi > 0.0 && h_lo > 0.0 && h_lot >= 0.01) {
                // Wire cancel_fn once (idempotent -- already set on re-entry but safe to set again)
                g_hybrid_gold.cancel_fn = [](const std::string& id) { send_cancel_order(id); };
                const std::string h_long_id  = send_live_order("XAUUSD", true,  h_lot, h_hi);
                const std::string h_short_id = send_live_order("XAUUSD", false, h_lot, h_lo);
                g_hybrid_gold.pending_long_clOrdId  = h_long_id;
                g_hybrid_gold.pending_short_clOrdId = h_short_id;
                printf("[HYBRID-GOLD] ORDERS SENT long_id=%s short_id=%s "
                       "hi=%.2f lo=%.2f range=%.2f lot=%.3f\n",
                       h_long_id.c_str(), h_short_id.c_str(),
                       h_hi, h_lo, g_hybrid_gold.range, h_lot);
                fflush(stdout);
            }
        }
    }

    // ?? NBM London position management -- ALWAYS runs when position open ??
    // CRITICAL: same bug as TrendPB and GFE -- on_tick() was only called inside the
    // entry guard, so _manage_position() (SL/VWAP trail) was never reached once a
    // position was open. Fix: call on_tick unconditionally when position is open.
    // on_tick returns {} immediately after _manage_position() when pos_.active.
    if (g_nbm_gold_london.has_open_position()) {
        g_nbm_gold_london.on_tick(sym, bid, ask, ca_on_close);
    }

    // ?? NBM London session (07:00-13:30 UTC) on XAUUSD ???????????????????
    // Runs independently of the gold stack/flow/bracket -- pure momentum
    // engine using London open as session anchor. Gated: no other gold pos.
    if (gold_can_enter
        && !g_nbm_gold_london.has_open_position()
        && !g_gold_stack.has_open_position()
        && !g_gold_flow.has_open_position()
        && !g_bracket_gold.has_open_position()
        && !g_trend_pb_gold.has_open_position()
        && !g_le_stack.has_open_position()) {
        const auto nbm_lon = g_nbm_gold_london.on_tick(sym, bid, ask, ca_on_close);
        if (nbm_lon.valid) {
            g_telemetry.UpdateLastSignal("XAUUSD",
                nbm_lon.is_long ? "LONG" : "SHORT", nbm_lon.entry,
                nbm_lon.reason, "NBM_LONDON", regime.c_str(), "NBM_LONDON",
                nbm_lon.tp, nbm_lon.sl);
            if (!enter_directional("XAUUSD", nbm_lon.is_long, nbm_lon.entry,
                                   nbm_lon.sl, nbm_lon.tp, 0.01, false, bid, ask, sym, regime))
                g_nbm_gold_london.cancel();
            else g_nbm_gold_london.patch_size(g_last_directional_lot);
        }
    }
}

