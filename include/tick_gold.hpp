#pragma once  // OM-08 (audit 2026-07-13): TU-fragment guard
// tick_gold.hpp -- per-symbol tick handlers
// Extracted from on_tick(). Same translation unit -- all static functions visible.

// Session 9: GoldCoordinator wiring -- allow-by-default skeleton. The coordinator
// tracks per-lane position counts so later sessions can enforce budget limits
// without another refactor. Engine behaviour is UNCHANGED in Session 9.
#include "gold_coordinator.hpp"
#include "PortfolioGuard.hpp"  // S48: portfolio-level kill-switch + sizing helpers
#include "GoldTrendMimicLadder.hpp"  // S-2026-07-14: XAU-H1 regime gate feed at H1 close
#include "OmegaBeCascadeBook.hpp"    // S-2026-07-16: omega::be_cascade_book() (BE-CASCADE gold cell)
#include "GoldExecSpreadBasis.hpp"   // S-2026-07-14: XAUUSD cost-gate spread basis (exec = IBKR futures book, not the BlackBull feed quote)

// -- XAUUSD -------------------------------------------------
static void on_tick_gold(
    const std::string& sym, double bid, double ask,
        bool tradeable, bool lat_ok, const std::string& regime,
        double rtt_check)
{
    // 2026-05-05 (audit-fixes-40): per-engine heartbeat pulses. One pulse per
    //   gold engine driven from this dispatcher. The XAUUSD tick handler
    //   forwards to all gold engines below, so a single tick drives every
    //   listed engine. Tsmom_<TF>_long pulses fire here too because
    //   TsmomPortfolio::on_tick is called from this handler (intrabar SL
    //   management); on_h1_bar additionally fires every H1 close.
    // S11 P3b: HybridGold pulse removed (engine culled in P3a + globals/init removed in P3b).
    // 2026-06-12: feed the shared price-based regime brain once per gold tick so
    //   every gold engine can query omega::gold_regime() for the bull/bear state
    //   (sustained-bear long-block). Self-contained (no external feed) -> cannot
    //   silently degrade to all-clear in a real bear. See RegimeState.hpp.
    omega::gold_regime().on_tick(bid, ask, omega::pg::_pg_now_ms());
    // (XauBracketCascade + gold mimic dispatch removed S-2026-07-13 code cull — families deleted.)
    // 2026-06-17: macro-hostile tightening ON TOP of the price core. Fail-safe --
    //   g_macro_gold_gate.hostile() returns false on disabled/missing/stale feed,
    //   so this can only ADD a long block when real yields + dollar rise hard,
    //   never unblock. All 8 long-only gold engines inherit it via long_blocked().
    //   Producer: tools/macro_gold_gate.py (daily). See MacroGoldGate.hpp.
    omega::gold_regime().set_macro_hostile(g_macro_gold_gate.hostile(omega::pg::_pg_now_ms()));

    g_engine_heartbeat.pulse("MidScalperGold");
    g_engine_heartbeat.pulse("MicroScalperGold");
    g_engine_heartbeat.pulse("GoldStack");
    g_engine_heartbeat.pulse("CandleFlow");
    g_engine_heartbeat.pulse("EMACross");
    g_engine_heartbeat.pulse("XauusdFvg");
    g_engine_heartbeat.pulse("GoldScalpPyramid");
    g_engine_heartbeat.pulse("RSIReversal");
    g_engine_heartbeat.pulse("XauSessNYpm");
    g_engine_heartbeat.pulse("XauSessOvernight");
    g_engine_heartbeat.pulse("RSIExtreme");
    g_engine_heartbeat.pulse("h1_swing_gold");
    g_engine_heartbeat.pulse("h4_regime_gold");
    g_engine_heartbeat.pulse("NbmGoldLondon");
    g_engine_heartbeat.pulse("Tsmom_H1_long");
    g_engine_heartbeat.pulse("Tsmom_H2_long");
    g_engine_heartbeat.pulse("Tsmom_H4_long");
    g_engine_heartbeat.pulse("Tsmom_H6_long");
    g_engine_heartbeat.pulse("Tsmom_D1_long");
    // 2026-05-26 (Stage 2): XAU trend zoo coverage. Each engine's on_tick
    // is dispatched below from this handler; pulse here makes "engine alive"
    // visible to EngineHeartbeat::check_misses. See engine_contract_check.py
    // for the gap inventory that motivated this batch.
    g_engine_heartbeat.pulse("XauTrendFollow1h");
    g_engine_heartbeat.pulse("XauTrendFollow2h");
    g_engine_heartbeat.pulse("XauTrendFollow4h");
    g_engine_heartbeat.pulse("XauTrendFollowD1");
    g_engine_heartbeat.pulse("XauTsmomFastD1");
    g_engine_heartbeat.pulse("XauTurtleD1");
    g_engine_heartbeat.pulse("XauStopRunD1");
    g_engine_heartbeat.pulse("XauPullbackContH4");
    g_engine_heartbeat.pulse("XauPullbackContD1");
    g_engine_heartbeat.pulse("XauNbmD1");
    g_engine_heartbeat.pulse("XauEmaCrossH4");
    g_engine_heartbeat.pulse("XauBBScalpD1");
    g_engine_heartbeat.pulse("XauSwingBreakD1");
    g_engine_heartbeat.pulse("XauDojiRejD1");
    g_engine_heartbeat.pulse("XauOutsideBarD1");
    g_engine_heartbeat.pulse("XauInsideBarD1");
    g_engine_heartbeat.pulse("TrendLineBreak");
    g_engine_heartbeat.pulse("Xau3BarMomH4");
    g_engine_heartbeat.pulse("XauDonchian55GatedM30");
    // 2026-05-26 (Stage 5): gold-portfolio engines dispatched from this
    // handler via on_tick + on_h1_bar (g_c1_retuned/donchian/ema_pullback/
    // tsmom_v2 at the H1-bar block below; g_pdhl_rev at PDHL block).
    g_engine_heartbeat.pulse("C1Retuned");
    g_engine_heartbeat.pulse("Donchian");
    g_engine_heartbeat.pulse("EmaPullback");
    g_engine_heartbeat.pulse("TsmomV2");
    g_engine_heartbeat.pulse("PdhlRev");

    // S48 2026-05-27: portfolio-level kill-switch refresh.
    // Checks for C:/Omega/KILL_SWITCH.lock every 30s. If file exists,
    // omega::pg::can_open_new_position() returns false until operator
    // deletes the file. Existing positions continue to manage to exit.
    // 2026-05-27 S58: use _pg_now_ms() — now_ms_g not yet declared this early
    // in on_tick_gold (declared at ~L735); was a build break on VPS.
    omega::pg::refresh_kill_switch(omega::pg::_pg_now_ms());

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
        g_bracket_gold.has_open_position()      ||
        g_h1_swing_gold.has_open_position()     ||  // H1 swing open blocks all other gold entries
        g_candle_flow.has_open_position()       ||  // CFE open blocks other gold engines
        g_ema_cross.has_open_position()         ||  // ECE open blocks other gold engines
        g_xauusd_fvg.has_open_position();

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
        // g_feed_stale_xauusd is dormant since S13 cTrader cull (2026-05-08) -- the
        // CTraderDepthClient starvation watchdog that wrote it has been removed.
        // The flag now permanently reads false, which is correct: FIX 264=0 owns
        // L2, and FIX session keepalive surfaces feed loss directly. Kept as a
        // gate input so the existing IntradaySeasonality plumbing stays intact.
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
                // S-2026-07-14 (sweep P1-3): spread basis = IBKR futures book, NOT
                // (ask - bid) from the BlackBull feed quote — the XAUUSD cost row
                // assumes the tight exchange spread is supplied; the feed's spot
                // markup was silently inflating the hurdle. See GoldExecSpreadBasis.hpp.
                {
                    const double gold_tp_dist = gsig.tp_ticks * 0.10;  // ticks ? pts (gold tick = $0.10)
                    if (!ExecutionCostGuard::is_viable("XAUUSD", omega::kGoldExecSpreadPts, gold_tp_dist, gold_lot)) {
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
        // S-2026-06-03: daily momentum-gate summary (fired vs blocked, lifetime)
        {
            static int s_momgate_day = -1;
            const int cur_day = (int)(now_ms_g / 86400000LL);
            if (s_momgate_day < 0) s_momgate_day = cur_day;
            else if (cur_day != s_momgate_day) {
                const long ps = omega::gold_wt().passes();
                const long sk = omega::gold_wt().skips();
                const long tot = ps + sk;
                printf("[GOLD-MOMGATE-DAILY] gold trend entries: fired=%ld blocked=%ld "
                       "(%.0f%% blocked by momentum gate) lifetime\n",
                       ps, sk, tot ? 100.0 * sk / tot : 0.0);
                fflush(stdout);
                s_momgate_day = cur_day;
            }
        }
        // M1
        if (s_bar1_ms == 0) { s_cur1 = {b1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar1_ms = b1; }
        else if (b1 != s_bar1_ms) {
            // S-2026-06-03: one-time warm-seed of the WaveTrend gate from the
            // disk-hydrated M1 history (get_bars, ~300 bars) so the gate is ready
            // immediately on restart instead of failing open ~55min. We already
            // have the bars — use them.
            static bool s_wt_seeded = false;
            if (!s_wt_seeded) {
                for (const auto& wb : g_bars_gold.m1.get_bars())
                    omega::gold_wt().on_m1_close(wb.high, wb.low, wb.close);
                s_wt_seeded = true;
                printf("[GOLD-MOMGATE] WaveTrend warm-seeded from %d M1 bars (warm=%d)\n",
                       g_bars_gold.m1.bar_count(), (int)omega::gold_wt().warm());
                fflush(stdout);
            }
            g_bars_gold.m1.add_bar(s_cur1); omega::gold_wt().on_m1_close(s_cur1.high, s_cur1.low, s_cur1.close); g_ema_cross.on_bar(s_cur1.close, g_bars_gold.m1.ind.atr14.load(std::memory_order_relaxed), g_bars_gold.m1.ind.rsi14.load(std::memory_order_relaxed), b1); s_cur1 = {b1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar1_ms = b1; }
        else { if(xau_mid>s_cur1.high)s_cur1.high=xau_mid; if(xau_mid<s_cur1.low)s_cur1.low=xau_mid; s_cur1.close=xau_mid; }
        // M5
        if (s_bar5_ms == 0) { s_cur5 = {b5/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar5_ms = b5; }
        else if (b5 != s_bar5_ms) { g_bars_gold.m5.add_bar(s_cur5); s_cur5 = {b5/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar5_ms = b5; }
        else { if(xau_mid>s_cur5.high)s_cur5.high=xau_mid; if(xau_mid<s_cur5.low)s_cur5.low=xau_mid; s_cur5.close=xau_mid; }
        // M15
        if (s_bar15_ms == 0) { s_cur15 = {b15/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar15_ms = b15; }
        else if (b15 != s_bar15_ms) {
            g_bars_gold.m15.add_bar(s_cur15);
            // -- XauTrendFollow M15 Donchian-40 dispatch (S-2026-06-02) --
            // Same engine type as g_xau_tf_1h, fed M15 bars. atr14_external=0.0
            // -> engine uses its own internal Wilder ATR over M15 bars (matches
            // the gold_cost_unlock_sweep.cpp validation). Donchian40 cell only.
            {
                omega::XauTfBar1h bar15{};
                bar15.bar_start_ms = s_bar15_ms;
                bar15.open  = s_cur15.open;
                bar15.high  = s_cur15.high;
                bar15.low   = s_cur15.low;
                bar15.close = s_cur15.close;
                // OCO straddle on M15 (same engine, fed M15 bars)
            }
            s_cur15 = {b15/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar15_ms = b15;
        }
        else { if(xau_mid>s_cur15.high)s_cur15.high=xau_mid; if(xau_mid<s_cur15.low)s_cur15.low=xau_mid; s_cur15.close=xau_mid; }
        // M30 -- feeds XauThreeBar30mEngine (S36-P4 2026-05-12)
        // Three-bar continuation pattern at 30m. Single-position; shadow-only by
        // default. ATR computed locally inside the engine (g_bars_gold has no
        // m30 indicator pipeline; SymBarState only carries m1/m5/m15/h1/h4).
        static OHLCBar s_cur30{};
        static int64_t s_bar30_ms = 0;
        const int64_t b30 = (now_ms_g / 1800000LL) * 1800000LL;  // 30min
        if (s_bar30_ms == 0) { s_cur30 = {b30/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar30_ms = b30; }
        else if (b30 != s_bar30_ms) {
            // -- XauThreeBar30mEngine 30m-close dispatch (S36-P4 2026-05-12) --
            {
                omega::XauThreeBar30mBar bar30m{};
                bar30m.bar_start_ms = s_bar30_ms;
                bar30m.open  = s_cur30.open;
                bar30m.high  = s_cur30.high;
                bar30m.low   = s_cur30.low;
                bar30m.close = s_cur30.close;
                g_xau_threebar_30m.on_30m_bar(
                    bar30m, bid, ask,
                    /*atr14_external=*/0.0,
                    now_ms_g, bracket_on_close);
            }
            // -- XauDonchian55GatedM30Engine 30m-close dispatch (S136 2026-05-24) --
            // -- XauStraddleM30Engine 30m-close: roll box + re-arm OCO straddle --
            // -- GoldVolBreakoutM30Engine 30m-close: entry/trail (S-2026-06-03) --
            // Long-only vol-breakout runner; trend gate set on H1 close above.
            g_gold_volbrk_m30.on_m30_bar(s_cur30.high, s_cur30.low, s_cur30.close,
                                         bid, ask, now_ms_g, bracket_on_close);
            s_cur30 = {b30/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar30_ms = b30;
        }
        else { if(xau_mid>s_cur30.high)s_cur30.high=xau_mid; if(xau_mid<s_cur30.low)s_cur30.low=xau_mid; s_cur30.close=xau_mid; }
        // H1 -- feeds H1SwingEngine + broader HTF context.
        // 14 H1 bars = 14 hours cold; warm restart immediate from saved indicators.
        if (s_bar_h1_ms == 0) { s_cur_h1 = {bh1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h1_ms = bh1; }
        else if (bh1 != s_bar_h1_ms) {
            g_bars_gold.h1.add_bar(s_cur_h1);
            // GoldVolBreakoutM30Engine: H1 EMA200+slope trend gate (S-2026-06-03).
            // Must run on every H1 close before the M30 entry path uses trend_.
            g_gold_volbrk_m30.on_h1_close(s_cur_h1.close);
            // GoldTrendMimic XAU-H1 SMA200 regime gate (S-2026-07-14): feeds the bull_only
            // mimic books (XAU_4h_DonchN20 bear-gate proviso). Seeded at boot; cheap.
            omega::gold_trend_mimic().xau_regime_h1_close(s_cur_h1.close);
            // S-2026-07-16: BE-CASCADE companion gold cell (XAUUSD; MGC folds in here — one
            // gold underlying, not double-wired). s_bar_h1_ms = the closing H1 bar's ms.
            omega::be_cascade_book().on_bar("XAUUSD", s_bar_h1_ms / 1000,
                                            s_cur_h1.high, s_cur_h1.low, s_cur_h1.close);
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
            // C1RetunedPortfolio H1 dispatch -- drives Donchian H1 + synthesised H2/H6 cells.
            // Self-contained shadow engine; runs independently of H1Swing / H4Regime / Minimal.
            {
                omega::C1Bar c1_h1{};
                c1_h1.bar_start_ms = s_bar_h1_ms;
                c1_h1.open  = s_cur_h1.open;
                c1_h1.high  = s_cur_h1.high;
                c1_h1.low   = s_cur_h1.low;
                c1_h1.close = s_cur_h1.close;
            }
            // S118 2026-05-19: XauTrendFollow1hEngine H1 dispatch.
            // Long-only 2-cell ensemble (EmaCross_20_80 + Donchian_N40).
            // Per-tick management runs via g_xau_tf_1h.on_tick() in the
            // post-bar tick loop alongside g_xau_tf_4h.on_tick().
            {
                omega::XauTfBar1h tf_h1{};
                tf_h1.bar_start_ms = s_bar_h1_ms;
                tf_h1.open  = s_cur_h1.open;
                tf_h1.high  = s_cur_h1.high;
                tf_h1.low   = s_cur_h1.low;
                tf_h1.close = s_cur_h1.close;
                g_xau_tf_1h.on_h1_bar(
                    tf_h1, bid, ask,
                    g_bars_gold.h1.ind.atr14.load(std::memory_order_relaxed),
                    now_ms_g, bracket_on_close);
            }
            // TsmomPortfolio H1 dispatch -- Tier-1 ship 2026-04-30. Drives
            // 5 long cells: H1 directly, H2/H4/H6/D1 synthesised internally
            // from H1 bars. Self-contained shadow engine; runs independently
            // of every other gold engine. Macro regime feed is best-effort
            // (RISK_OFF blocks new entries; existing positions still managed).
            // Cells are pre-warmed at init() from
            // phase1/signal_discovery/tsmom_warmup_H1.csv so the very first
            // live H1 bar can fire signals on every cell -- no cold start.
            {
                omega::TsmomBar ts_h1{};
                ts_h1.bar_start_ms = s_bar_h1_ms;
                ts_h1.open  = s_cur_h1.open;
                ts_h1.high  = s_cur_h1.high;
                ts_h1.low   = s_cur_h1.low;
                ts_h1.close = s_cur_h1.close;
            }
            // TsmomPortfolioV2 H1 dispatch -- Phase 2a CellEngine-refactor shadow.
            // Same H1 bar / bid / ask / atr / now_ms as g_tsmom above so the
            // V2 ledger is directly comparable to the V1 slice of g_omegaLedger.
            // Callback writes to logs/shadow/tsmom_v2.csv only -- never touches
            // g_omegaLedger (no double-counting in master accounting state).
            // See engine_init.hpp g_tsmom_v2 block for the rationale.
            {
                omega::cell::Bar v2_h1{};
                v2_h1.bar_start_ms = s_bar_h1_ms;
                v2_h1.open  = s_cur_h1.open;
                v2_h1.high  = s_cur_h1.high;
                v2_h1.low   = s_cur_h1.low;
                v2_h1.close = s_cur_h1.close;
            }
            // DonchianPortfolio H1 dispatch -- Tier-2 ship 2026-04-30. Drives
            // 7 cells (H2 long, H4/H6/D1 long+short). H1 atr14 is unused (cells
            // self-compute ATR per synthesised TF) but passed for signature
            // parity with other portfolios. H1 long itself is NOT in this
            // engine -- it lives in C1RetunedPortfolio.
            {
                omega::DonchianBar dn_h1{};
                dn_h1.bar_start_ms = s_bar_h1_ms;
                dn_h1.open  = s_cur_h1.open;
                dn_h1.high  = s_cur_h1.high;
                dn_h1.low   = s_cur_h1.low;
                dn_h1.close = s_cur_h1.close;
            }
            // ── XauTrendFollow2hEngine H1 dispatch (S33k 2026-05-11) ──────────
            // 4-cell 2h trend-follow ensemble. Takes H1 bars; aggregates into
            // 2h internally. Position management via on_tick further below.
            {
                omega::XauTf2hBar tf_h1{};
                tf_h1.bar_start_ms = s_bar_h1_ms;
                tf_h1.open  = s_cur_h1.open;
                tf_h1.high  = s_cur_h1.high;
                tf_h1.low   = s_cur_h1.low;
                tf_h1.close = s_cur_h1.close;
                g_xau_tf_2h.on_h1_bar(tf_h1, bid, ask, now_ms_g, bracket_on_close);
            }
            // EmaPullbackPortfolio H1 dispatch -- Tier-3 ship 2026-04-30.
            // Drives 4 long cells (H1/H2/H4/H6). H1 cell uses raw H1; H2/H4/H6
            // synthesised internally. EWMs warm up via warmup_from_csv at init.
            {
                omega::EpbBar epb_h1{};
                epb_h1.bar_start_ms = s_bar_h1_ms;
                epb_h1.open  = s_cur_h1.open;
                epb_h1.high  = s_cur_h1.high;
                epb_h1.low   = s_cur_h1.low;
                epb_h1.close = s_cur_h1.close;
            }
            // TrendRiderPortfolio H1 dispatch -- Tier-4 ship 2026-04-30.
            // 6 trend-rider cells (H2 L+S, H4 L+S, H6 L, D1 L) with 40-bar
            // Donchian breakout entry + stage trail. Single H1 dispatch
            // synthesises H2/H4/H6/D1 internally. Higher conviction = higher
            // sizing (2x baseline risk_pct + max_lot_cap).
            {
                omega::TrBar tr_h1{};
                tr_h1.bar_start_ms = s_bar_h1_ms;
                tr_h1.open  = s_cur_h1.open;
                tr_h1.high  = s_cur_h1.high;
                tr_h1.low   = s_cur_h1.low;
                tr_h1.close = s_cur_h1.close;
                g_trend_rider.set_macro_regime(g_macroDetector.regime());
                g_trend_rider.on_h1_bar(
                    tr_h1, bid, ask,
                    g_bars_gold.h1.ind.atr14.load(std::memory_order_relaxed),
                    now_ms_g, ca_on_close);
            }
            s_cur_h1 = {bh1/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h1_ms = bh1;
        } else { if(xau_mid>s_cur_h1.high)s_cur_h1.high=xau_mid; if(xau_mid<s_cur_h1.low)s_cur_h1.low=xau_mid; s_cur_h1.close=xau_mid; }
        // H4 -- HTF gate for TrendPullback + H4RegimeEngine.
        // 14 H4 bars = 56 hours cold; warm restart immediate.
        if (s_bar_h4_ms == 0) { s_cur_h4 = {bh4/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h4_ms = bh4; }
        else if (bh4 != s_bar_h4_ms) {
            g_bars_gold.h4.add_bar(s_cur_h4);
            // ── XauTrendFollow4hEngine (S33d 2026-05-11; extended to 5 cells S33e) ──
            // 5-cell trend-follow ensemble (Donchian N=20, InsideBar, ER0.20,
            // Keltner K=2, ADX_Mom adx>25). Shadow-only by default. Driven by
            // the same s_cur_h4 bar as C1Retuned above. Per-tick position
            // management runs via g_xau_tf_4h.on_tick() in the post-bar tick loop.
            {
                omega::XauTfBar tf_h4{};
                tf_h4.bar_start_ms = s_bar_h4_ms;
                tf_h4.open  = s_cur_h4.open;
                tf_h4.high  = s_cur_h4.high;
                tf_h4.low   = s_cur_h4.low;
                tf_h4.close = s_cur_h4.close;
                g_xau_tf_4h.on_h4_bar(
                    tf_h4, bid, ask,
                    g_bars_gold.h4.ind.atr14.load(std::memory_order_relaxed),
                    now_ms_g, bracket_on_close);
            }
            // ── XauTrendFollowD1Engine (S33e 2026-05-11) ──────────────────────
            // 3-cell daily trend-follow ensemble. Synthesises D1 bars from the
            // H4 stream internally; we just hand it each H4 close and the
            // engine groups by UTC date. Shadow-only by default. Per-tick
            // position management runs via g_xau_tf_d1.on_tick() below.
            {
                omega::XauTfD1Bar d1_in{};
                d1_in.bar_start_ms = s_bar_h4_ms;
                d1_in.open  = s_cur_h4.open;
                d1_in.high  = s_cur_h4.high;
                d1_in.low   = s_cur_h4.low;
                d1_in.close = s_cur_h4.close;
                g_xau_tf_d1.on_h4_bar(d1_in, bid, ask, now_ms_g, bracket_on_close);
            }
            // ── XauTsmomFastD1Engine (2026-05-20) ──────────────────────────────
            // Short-lookback D1 momentum (lb=5 sl=1.0 tp=5.0 hold=20). Uses
            // same H4-close stream to synthesise D1 bars internally.
            // ── XauTurtleD1Engine (2026-05-20) -- 40d Donchian long break
            // ── XauStopRunD1Engine (2026-05-20) -- 5d stop-run rejection rally
            // ── XauPullbackContH4Engine (2026-05-20) -- pullback to fast EMA
            // ── XauNbmD1Engine (2026-05-20) -- Noise Band Momentum break
            // ── XauEmaCrossH4Engine (2026-05-20) -- 20/100 golden cross
            // ── 2026-05-20 mega-sweep batch: PullbackContD1 / BBScalpD1 / SwingBreakD1
            // ── S136 2026-05-24: Xau3BarMomGatedH4Engine ──────────────────────
            g_xau_swing_break_d1.on_h4_bar(s_cur_h4.high, s_cur_h4.low, s_cur_h4.close,
                                            bid, ask, now_ms_g, bracket_on_close);
            // ── 2026-05-20 mega-sweep2 candle patterns: DojiRej, OutsideBar, InsideBar
            // ── 2026-05-21 GoldD1TrendState update -- regime gate for shorts
            omega::gold_d1_trend().on_h4_bar(s_cur_h4.high, s_cur_h4.low, s_cur_h4.close, now_ms_g);
            s_cur_h4 = {bh4/60000LL, xau_mid, xau_mid, xau_mid, xau_mid}; s_bar_h4_ms = bh4;
        } else { if(xau_mid>s_cur_h4.high)s_cur_h4.high=xau_mid; if(xau_mid<s_cur_h4.low)s_cur_h4.low=xau_mid; s_cur_h4.close=xau_mid; }
    }

    // ?? VPIN toxicity tracker -- updated every XAUUSD tick ????????????????
    g_vpin.on_tick(xau_mid, now_ms_g);
    // ?? Correlation matrix feed -- XAUUSD ??????????????????????????????????
    g_corr_matrix.on_price("XAUUSD", xau_mid);

    // -- L2 tick logger -- UNCONDITIONAL, every XAUUSD tick ---------------
    // Logs the FIX-fed L2 imbalance and core OHLC fields. Historical
    // depth/event-count columns are kept in the row layout (zeros) so that
    // appending to today's CSV from a prior binary run does not corrupt the
    // schema. cTrader-derived fields (raw_bid/raw_ask, depth_events_total,
    // micro_edge, watchdog_dead) are ALL zero since 2026-05-08 (S13).
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
            // 2026-05-08 (S13): cTrader-sourced columns ZEROED.
            //   cTrader Open API was runtime-disabled 2026-04-29 and is being
            //   culled in S9. The columns below previously read cTrader-only
            //   atomics (raw_bid, raw_ask, depth_events_total, micro_edge) and
            //   the cTrader feed-watchdog flag. Mirroring the SP/USTEC pattern
            //   in on_tick.hpp (which has always written zeros for these),
            //   we keep the 17-column layout for backward-compatible appends to
            //   today's CSV but populate cTrader fields with 0. l2_imb stays
            //   live -- it is sourced from g_macro_ctx.gold_l2_imbalance which
            //   is FIX-fed via fix_dispatch.hpp since S8.
            //
            //   Hydrator only requires ts_ms, mid, bid, ask -- all preserved.
            const double vol_ratio_log = (g_gold_stack.base_vol_pct() > 0.0)
                ? g_gold_stack.recent_vol_pct() / g_gold_stack.base_vol_pct() : 0.0;
            // S14 fix 2026-04-24: restore fprintf row-write. S19 Stage 1B (a199bec)
            // L2 CSV logger was unconditional by design. Result: header written
            // once per day then zero data rows, CSV stale for every tick since
            // 2026-04-17 (VERIFY_STARTUP flagged 16767s stale 2026-04-24 07:43 UTC).
            // S88 (2026-05-22): if IBKR bridge fresh, log its volume + level
            // counts instead of zeros. Schema unchanged so existing readers
            // (backtest replay, hydrator) keep working.
            const bool ibkr_fresh_log = g_ibkr_l2.xau.fresh(now_ms_g, 5000);
            const double log_bid_vol = ibkr_fresh_log
                ? g_ibkr_l2.xau.bid_vol.load(std::memory_order_relaxed) : 0.0;
            const double log_ask_vol = ibkr_fresh_log
                ? g_ibkr_l2.xau.ask_vol.load(std::memory_order_relaxed) : 0.0;
            const int log_bid_levels = ibkr_fresh_log
                ? g_ibkr_l2.xau.bid_levels.load(std::memory_order_relaxed) : 0;
            const int log_ask_levels = ibkr_fresh_log
                ? g_ibkr_l2.xau.ask_levels.load(std::memory_order_relaxed) : 0;
            fprintf(s_l2f_unc,
                "%lld,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,"
                "%d,%d,%llu,"
                "%d,%.3f,%d,%.3f,%d,%.4f,%.4f\n",
                (long long)now_ms_g, xau_mid, bid, ask,
                g_macro_ctx.gold_l2_imbalance,            // IBKR if fresh, else FIX
                log_bid_vol, log_ask_vol,                 // IBKR real sizes if fresh, else 0
                log_bid_levels, log_ask_levels, (unsigned long long)0,
                0,                                        // watchdog_dead -- cTrader watchdog, zeroed
                vol_ratio_log,
                (int)gold_sdec.regime,
                g_vpin.warmed() ? g_vpin.vpin() : 0.0,
                0,                                        // has_pos placeholder (legacy 0)
                0.0,                                      // micro_edge -- cTrader microstructure, zeroed
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
        // Suppressed when HybridBracketGold engine is disabled (see
        // g_disable_bracket_gold in globals.hpp). When disabled, on_tick is
        // never called for g_bracket_gold so its window never updates --
        // range/brk_hi/brk_lo stay 0 forever and the diag is misleading
        // ("can_enter=1 can_arm=1" while the engine receives no ticks).
        // Print one banner per session-start so log search still finds it,
        // then go quiet until the disable flag is cleared.
        if (!g_disable_bracket_gold || g_bracket_gold.has_open_position()) {
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
        } else {
            // Engine permanently disabled (g_disable_bracket_gold) and no open
            // position -> the live diag above never fires, so 0 [GOLD-BRK-DIAG]
            // lines reach the log and the frontend gold_bracket.range_alive
            // health check perma-warns ("No [GOLD-BRK-DIAG] lines in last 2000").
            // Emit a terse heartbeat (every 30s) so the check stays satisfied
            // and the disabled state stays explicit in the log.
            static int64_t s_last_brk_disabled_diag = 0;
            if (now_ms_g - s_last_brk_disabled_diag >= 30000) {
                s_last_brk_disabled_diag = now_ms_g;
                std::cout << "[GOLD-BRK-DIAG] phase=DISABLED engine_off=1"
                          << " reason=g_disable_bracket_gold"
                          << " note=permanent_disable_2026-05-29\n";
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
        // (Stale note removed S12 P3c: g_hybrid_gold/GoldHybridBracketEngine
        //  retired; nothing parallel to wire.)
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
        // Audit-disable gate (2026-04-30): -324pts in 4wk, 12.8% WR. Skip
        // the call when no position is open; existing positions still
        // manage to exit via the engine's own state machine.
        if (g_bracket_gold.has_open_position() || !g_disable_bracket_gold) {
            g_bracket_gold.on_tick(bid, ask, now_ms_g,
                bracket_can_enter_eff,
                regime.c_str(), bracket_on_close, gold_vwap_now,
                g_macro_ctx.gold_l2_imbalance,
                g_gold_stack.ewm_drift());   // Session 13: drift arg drives regime-flip exit
        }
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
            // S-2026-07-14 (sweep P1-3): exchange spread basis, not the BlackBull
            // feed quote — see GoldExecSpreadBasis.hpp. compute_size above keeps the
            // live feed spread (sizing conservatism is fine; the VIABILITY hurdle is not).
            const double bg_tp_dist = std::fabs(bgsigs.long_entry - bgsigs.long_sl) * g_bracket_gold.RR;
            const bool bg_cost_ok = ExecutionCostGuard::is_viable("XAUUSD", omega::kGoldExecSpreadPts, bg_tp_dist, bg_lot);
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
    // RSIExtremeTurnEngine indicator warmup -- UNCONDITIONAL, every tick
    // Must run so tick_rsi / tick_atr are always current regardless of position state.
    // Inject M1 bar RSI for entry signal -- bar RSI is smooth (60s) and matches chart
    if (g_bars_gold.m1.ind.m1_ready.load(std::memory_order_relaxed)) {
        const double rsi_bar_mid = (bid + ask) * 0.5;
        (void)rsi_bar_mid;
        // RSIExtremeTurnEngine bar RSI injection -- tracks sustained extreme bars
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
        // FIX 2026-04-02: seed M5 trend from M1 EMA crossover, not M5 swing pivot (15+ min lag)
        {
            const double tpb_ema9  = g_bars_gold.m1.ind.ema9 .load(std::memory_order_relaxed);
            const double tpb_ema50 = g_bars_gold.m1.ind.ema50.load(std::memory_order_relaxed);
            const int tpb_trend = (tpb_ema9 > 0.0 && tpb_ema50 > 0.0)
                ? (tpb_ema9 < tpb_ema50 ? -1 : +1) : 0;
            (void)tpb_trend;
        }
    }
    // H4 trend gate -- feeds HTF direction into TrendPullback gold entry filter.
    if (g_bars_gold.h4.ind.m1_ready.load(std::memory_order_relaxed)) {
    }
    // H1/H4 engine status -> telemetry snap (every tick, lock-free)
    {
        auto* snap = g_telemetry.snap();
        if (snap) {
            snap->h1_swing_open      = g_h1_swing_gold.has_open_position() ? 1 : 0;
            snap->h1_swing_daily_pnl = static_cast<float>(g_h1_swing_gold.daily_pnl_);
            snap->h1_swing_shadow    = g_h1_swing_gold.shadow_mode ? 1 : 0;
            snap->h1_adx = static_cast<float>(
                g_bars_gold.h1.ind.adx14.load(std::memory_order_relaxed));
            snap->h4_adx = static_cast<float>(
                g_bars_gold.h4.ind.adx14.load(std::memory_order_relaxed));
            snap->h4_trend_state = g_bars_gold.h4.ind.trend_state.load(
                std::memory_order_relaxed);
            // MinimalH4Breakout mirror fields
        }
    }
    // H1/H4 engine tick-level management -- on_tick() handles SL/TP/partial/trail.
    // on_h1_bar() / on_h4_bar() handle bar-level exits (EMA cross, ADX collapse, timeout).
    if (g_h1_swing_gold.has_open_position())
        g_h1_swing_gold.on_tick(bid, ask, now_ms_g, ca_on_close);
    // MinimalH4Breakout tick management -- runs parallel to H4Regime, independent.
    // C1RetunedPortfolio tick management -- 4 cells, all independent of the rest.
    // TsmomPortfolio tick management -- 5 long cells (H1/H2/H4/H6/D1).
    // Tier-1 shipped 2026-04-30; runs alongside C1Retuned, no shared state.
    // TsmomPortfolioV2 tick management -- Phase 2a refactor shadow.
    // Trades go to logs/shadow/tsmom_v2.csv only (NOT g_omegaLedger).
    // DonchianPortfolio tick management -- 7 cells (H2 long, H4/H6/D1 long+short).
    // Tier-2 shipped 2026-04-30. Bidirectional. No shared state with other engines.
    // XauTrendFollow4hEngine tick management -- 5 cells (Donchian, InsideBar,
    // ER0.20, Keltner, ADX_Mom). S33d shipped 2026-05-11; extended to 5 cells
    // in S33e. Single-position per cell, 5 max concurrent. Shadow-default.
    // S-2026-07-02 KILL-THE-4H-WAIT: on the first live tick after a restart, evaluate
    // the last CLOSED live H4 bar (stashed by append_fresh_h4) for entry so a valid
    // signal is taken immediately instead of idling up to 4h for the next H4 close.
    // Guarded + one-shot inside the engine (self-disarms after the first call).
    g_xau_tf_4h.try_boot_fire(bid, ask, now_ms_g, bracket_on_close);
    g_xau_tf_4h.on_tick(bid, ask, now_ms_g, bracket_on_close);
    // S-2026-06-19: TrendRider companion -- banks +N*ATR per host cell + reloads
    // while the 4h cell stays open (shadow). Validated D1+4h only.
    g_rider_4h.on_host(g_xau_tf_4h.pos, bid, ask, now_ms_g, bracket_on_close);
    g_engine_heartbeat.pulse("Rider4H");  // S-2026-06-29 ENABLED+NO_PULSE fix
    // S118 2026-05-19: H1 long-only ensemble tick management.
    g_xau_tf_1h.on_tick(bid, ask, now_ms_g, bracket_on_close);
    // S-2026-07-20: GoldBullTrendGated -- long-only bull-GATED trend ensemble
    // (DONCH 1h + EMA 30m). Self-aggregates both TFs internally from this tick;
    // regime (gold_regime, fed at top of on_tick_gold) + slow-SMA dual gate.
    g_gold_bull_trend.on_tick(bid, ask, now_ms_g, bracket_on_close);
    g_engine_heartbeat.pulse("GoldBullTrendGated");
    // S42 2026-05-31: SessionMomentum x2 -- clock-based session-window long.
    // feed_tick() self-aggregates H1 + manages the open position (time exit).
    g_xau_sess_nypm.feed_tick(bid, ask, now_ms_g, bracket_on_close);
    // XauTrendFollowD1Engine tick management -- 3 daily-timeframe cells
    // (Momentum, Keltner, ADX_Mom). S33e shipped 2026-05-11. Daily bars are
    // built internally from H4 stream. Single-position per cell, 3 max
    // concurrent. Shadow-default.
    g_xau_tf_d1.on_tick(bid, ask, now_ms_g, bracket_on_close);
    // S-2026-06-21b CalendarTom on gold (turn-of-month, shadow) -- gcf_daily 2010-26 PF1.63 both-regime.
    g_tom_xau.on_tick(bid, ask, now_ms_g, handle_closed_trade);
    // S-2026-07-08c GoldTsmomD1V2 (deep-dive #2): D1 TSMOM {42,63,84} vol-targeted, BOTH
    // directions, monthly rebalance. Internal UTC-D1 aggregation (CalendarTom pattern).
    g_gold_tsmom_d1.on_tick(bid, ask, now_ms_g, handle_closed_trade);
    g_engine_heartbeat.pulse("GoldTsmomD1V2");
    // S-2026-07-13z GoldCampaignD1Anch: structural campaign CORE (D1-anchor first-pullback,
    // symmetric L/S). Internal UTC-M1 aggregation; detector runs on closed M1 bars only.
    g_gold_campaign_d1.on_tick(bid, ask, now_ms_g, handle_closed_trade);
    g_engine_heartbeat.pulse("GoldCampaignD1Anch");
    // S-2026-06-19: TrendRider companion on the D1 host (shadow).
    g_rider_d1.on_host(g_xau_tf_d1.pos, bid, ask, now_ms_g, bracket_on_close);
    g_engine_heartbeat.pulse("RiderD1");  // S-2026-06-29 ENABLED+NO_PULSE fix
    // XauTsmomFastD1Engine tick management (SL/TP per tick).
    // XauTurtleD1Engine + XauStopRunD1Engine tick management.
    // 2026-05-20 batch: PullbackContH4 / NbmD1 / EmaCrossH4 tick mgmt
    // 2026-05-20 mega-sweep batch tick mgmt
    g_xau_swing_break_d1.on_tick(bid, ask, now_ms_g, bracket_on_close);
    // 2026-05-20 mega-sweep2 candle patterns tick mgmt
    // XauTrendFollow2hEngine tick management -- 4 2h-timeframe cells
    // (Keltner, Donchian20, Donchian50, InsideBar). S33k shipped 2026-05-11.
    // 2h bars built internally from H1 stream. Single-position per cell, 4
    // max concurrent. Shadow-default.
    g_xau_tf_2h.on_tick(bid, ask, now_ms_g, bracket_on_close);
    // XauThreeBar30mEngine tick management -- 30m three-bar continuation (S36-P4).
    // Shadow-only by default. Intra-bar SL/TP/BE/trail management every tick.
    g_xau_threebar_30m.on_tick(bid, ask, now_ms_g, bracket_on_close);
    // ── S136 2026-05-24: new engines per-tick management ────────────────────
    g_gold_orb_retrace.on_tick(bid, ask, now_ms_g);                    // 2026-06-06 ORB 50%-retrace + structural RUNNER (shadow); callback via on_trade_record
    g_gold_orb_london.on_tick(bid, ask, now_ms_g);                     // S-2026-06-20 orb-widen: LONDON-open 2nd session +BullGate (shadow)
    g_gold_panic_bounce.on_tick(bid, ask, now_ms_g);                   // S-2026-06-29 REACTIVATED (shadow): V-bounce; macro long-block entry filter (post-cull) + IBKR-cost re-judge bull PF~1.80
    g_gold_volbrk_m30.on_tick(bid, ask, now_ms_g, bracket_on_close);   // S-2026-06-03 vol-breakout SL/trail per-tick
    g_engine_heartbeat.pulse("GoldVolbrkM30");  // S-2026-06-29 ENABLED+NO_PULSE fix
    // GoldUltimateEngine tick dispatch -- standalone v12 OOS-validated trend
    // engine. Self-contained 1-min bar aggregation + 7-factor entry filter +
    // edge-hour/ATR gates. S91 shipped 2026-05-15.
    // EmaPullbackPortfolio tick management -- 4 long cells (H1/H2/H4/H6).
    // Tier-3 shipped 2026-04-30. Long-only. No shared state.
    // TrendRiderPortfolio tick management -- 6 cells (H2 L+S, H4 L+S, H6 L, D1 L).
    // Tier-4 shipped 2026-04-30. Stage trail (no TP, no time exit).
    g_trend_rider.on_tick(bid, ask, now_ms_g, ca_on_close);
    g_engine_heartbeat.pulse("TrendRider");  // S-2026-06-29 ENABLED+NO_PULSE fix
    // -- Improvement 5: CVD confirmation gate ------------------------------

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
        }
    }

    // -- Improvement 8: Pyramid add-on on second EMA50 pullback -------------
    // When TrendPullback is live, check for second pullback directly from EMA state.
    // NEVER call on_tick() again while position is open -- that runs double management.
    // Instead: read EMA50 and check the same pullback+bounce condition manually.

    // ----------------------------------------------------------------------
    // HBG fully retired S12 P3c (2026-05-07): GoldHybridBracketEngine.
    //   - Dispatch removed S10 P3a    (commit ba5f0e9)
    //   - Globals decl, init/shadow_mode, register_engine, heartbeat
    //     registration, heartbeat pulse, g_open_positions source, and all
    //     gate-reads removed S11 P3b (commit a6e3403)
    //   - Header file include/GoldHybridBracketEngine.hpp DELETED + all
    //     #include refs removed S12 P3c (this commit)
    //   Original dispatch (compression range -> dual stop entry -> first-fill
    //   wins -> cancel loser) was lines ~2097-2202 of tick_gold.hpp pre-S10.
    //   Engine family fully retired; backtest harnesses for HBG also removed
    //   in this commit (see backtest/OmegaBacktest.cpp + backtest/omega_bt.cpp
    //   diffs; backtest/hbg_duka_bt.cpp deleted entirely).
    // ----------------------------------------------------------------------

    // -- 2026-05-01 SESSION_h: GoldMidScalperEngine dispatch -----------------
    // Mid-band sister to HybridGold targeting the $20-40 capture zone (range
    // $8-20 with TP_RR=4 -> TP $18-42). Runs in shadow mode by default --
    // no FIX orders, only ledger entries via bracket_on_close.
    //
    // 2026-05-12 DECOMMISSIONED on XAU. Three independent parameter sweeps
    //   (Plan A: exit-side; Plan B: TP/SL/trail; entry-side: MIN_RANGE,
    //   MAX_RANGE, EXPANSION_MULT, vol_regime) over 24 months of Dukascopy
    //   tick data showed no profitable configuration in the explored space.
    //   Mean gross/trade slightly negative (-$0.25 best case after entry
    //   tightening); mean cost/trade $2.50-3.00. Engine cannot break even
    //   at 0.01 lot on XAU under current regime. See:
    //     outputs/PLAN_A_B_REPORT.md
    //     outputs/PLAN_A_B_ADDENDUM.md
    //   To re-enable: remove the #if 0 / #endif wrapper below.
    //   The engine source (include/GoldMidScalperEngine.hpp) is preserved
    //   in place rather than moved to disabled_engines/ so the snapshot
    //   reporter + heartbeat in engine_init.hpp continue to compile cleanly.
    //   The dispatch below is the only behavioural change.
    //
    // Tick-feed model identical to HybridGold (FIX 2026-04-07): on_tick is
    // called on every tick to keep the 300-tick structure window full, even
    // when can_enter=false. The engine internally short-circuits the new-
    // entry path when its gates fail.
    //
    // Gating: midscalper does NOT add itself to gold_any_open (other engines
    // can run alongside it freely while it is shadow-only). It IS gated by
    // gold_can_enter (session/symbol/post-impulse) like the other XAUUSD
    // engines. Once promoted to live, add g_gold_midscalper.has_open_position()
    // to gold_any_open above to enforce 1-at-a-time on the live path.
#if 0  // 2026-05-12: MidScalperGold decommissioned, see comment block above
    {
        // Position management -- unconditional when live
        if (g_gold_midscalper.has_open_position()) {
            g_gold_midscalper.on_tick(bid, ask, now_ms_g,
                                      gold_can_enter, false, false, 0,
                                      bracket_on_close,
                                      g_macro_ctx.gold_slope,
                                      g_macro_ctx.gold_vacuum_ask,
                                      g_macro_ctx.gold_vacuum_bid,
                                      g_macro_ctx.gold_wall_above,
                                      g_macro_ctx.gold_wall_below,
                                      g_macro_ctx.gold_l2_real);
        }
        // New entry path: same vol-floor logic as HybridGold so the engine
        // sits out unseeded cold starts. Re-derived locally because the
        // hybrid_vol_ok variable from the HybridGold block above is out of
        // scope after that block's closing brace.
        const double ms_vol_range    = g_gold_stack.vol_range();
        const bool   ms_vol_unseeded = (ms_vol_range == 0.0);
        const double ms_vwap         = g_gold_stack.vwap();
        const double ms_mid          = (bid + ask) * 0.5;
        const double ms_vwap_disp    = (ms_vwap > 0.0)
            ? std::fabs(ms_mid - ms_vwap) : 0.0;
        const bool ms_macro_bypass   = (ms_vwap_disp >= 6.0)
                                    || (std::fabs(g_gold_stack.ewm_drift()) >= 2.0)
                                    || crash_impulse_bypass;
        const bool ms_vol_ok = (ms_vol_range >= 0.5)
                            || (ms_vol_unseeded && ms_macro_bypass)
                            || (!ms_vol_unseeded && ms_macro_bypass);
        // S54 2026-05-04 (audit-fixes-35): block MidScalperGold when HybridGold
        //   has an open position OR is currently arming/pending/live in the
        //   same tick. The phase check is the critical bit: HybridGold runs
        //   FIRST in this dispatch; if it transitioned IDLE -> ARMED on this
        //   tick, MidScalper would otherwise see has_open_position()=false
        //   (no fill yet) and fire on the same compression structure --
        //   producing the 2026-05-04 03:45:15 double LONG @ 4609.61 that
        //   cost -$2.49 per side at BE exit. With this check, only one of
        //   the two engines can enter on any given compression structure.
        //   COOLDOWN is treated as IDLE-equivalent because the prior trade
        //   has already closed and a fresh setup is allowed.
        // S11 P3b: g_hybrid_gold culled in P3a, removed in P3b. The S54 anti-
        //   double-entry guards (hybrid_active_or_arming + has_open_position)
        //   become unreachable -- pinned to false, removed from the conjunction.
        const bool hybrid_active_or_arming = false;
        (void)hybrid_active_or_arming;  // tombstone, intentionally unused
        const bool midscalper_can_enter =
            gold_can_enter
            && ms_vol_ok
            && !g_gold_midscalper.has_open_position();
        if (!g_gold_midscalper.has_open_position()) {
            g_gold_midscalper.on_tick(bid, ask, now_ms_g,
                                      midscalper_can_enter, false, false, 0,
                                      bracket_on_close,
                                      g_macro_ctx.gold_slope,
                                      g_macro_ctx.gold_vacuum_ask,
                                      g_macro_ctx.gold_vacuum_bid,
                                      g_macro_ctx.gold_wall_above,
                                      g_macro_ctx.gold_wall_below,
                                      g_macro_ctx.gold_l2_real);
        }
        // No FIX-order block here -- shadow mode means PENDING phase auto-fills
        // when price crosses bracket boundaries (handled inside on_tick).
        // When promoted to live, write a [MID-SCALPER-GOLD] ORDERS SENT block
        // analogous to the FIX-order patterns used by other live engines
        // (HBG's pattern was the original reference but HBG was culled S10/S11).
    }
#endif  // 2026-05-12: MidScalperGold decommissioned

    // -- 2026-05-08 S19: GoldMicroScalperEngine dispatch ---------------------
    // Bidirectional micro-tick scalper for XAUUSD. Captures many small moves
    // up and down via 20-tick z-score reversion entry, fast BE-arm (0.5pt),
    // aggressive trail (0.5pt below MFE post-BE), and tick-momentum/L2-flip
    // reversal exit. Calibrated 2026-05-08 on 28 days / 6.7M tick L2 capture
    // (CRTP sweep top combo: Z=0.75 TP=1.0 SL=3.0 BE=0.5 TR=0.5 / 34K trades
    // 86% WR PF 3.05). Runs in shadow mode by default (engine_init.hpp pin).
    //
    // Tick-feed model: the engine maintains a 20-tick rolling z-score window
    // and a 5-tick reversal window. on_tick must be called every tick to
    // keep these warm even when can_enter=false; the engine internally
    // short-circuits the new-entry path when its gates fail.
    //
    // Gating: shadow-only on first deployment; does NOT add itself to
    // gold_any_open. Once promoted to live, add
    // g_gold_microscalper.has_open_position() to gold_any_open and add an
    // anti-double-entry guard against MidScalperGold+MicroScalperGold
    // double-firing on the same compression structure (mirror the S54
    // audit-fixes-35 pattern documented in the MidScalper dispatch above).
    //
    // L2 args sourced from g_macro_ctx. When gold_l2_real=false, the
    // engine's L2 confirmation gate degrades to z-only entry and the
    // L2-flip reversal-exit leg is disabled (safe fallback).
    {
        // Position management -- unconditional when live.
        // 2026-05-08 S21: callback changed bracket_on_close -> microscalper_on_close
        //   so that EVERY exit (TP_HIT / SL_HIT / REVERSAL_EXIT / MAX_HOLD_EXIT /
        //   FORCE_CLOSE / BREAKOUT_FAIL) sends a real market close to the broker.
        //   bracket_on_close only sent on FORCE_CLOSE / BREAKOUT_FAIL because it
        //   assumes the broker has an OCO active; microscalper uses straight
        //   market entries, no OCO, so every exit needs its own market order.
        if (g_gold_microscalper.has_open_position()) {
            g_gold_microscalper.on_tick(bid, ask, now_ms_g,
                                        gold_can_enter,
                                        microscalper_on_close,
                                        g_macro_ctx.gold_l2_imbalance,
                                        g_macro_ctx.gold_slope,
                                        g_macro_ctx.gold_vacuum_ask,
                                        g_macro_ctx.gold_vacuum_bid,
                                        g_macro_ctx.gold_l2_real);
        }
        // New-entry path: same vol-floor + macro-bypass logic as MidScalper
        // so the engine sits out unseeded cold starts. Re-derived locally
        // because ms_vol_range / ms_macro_bypass are out of scope after the
        // MidScalper block's closing brace above.
        const double mcs_vol_range    = g_gold_stack.vol_range();
        const bool   mcs_vol_unseeded = (mcs_vol_range == 0.0);
        const double mcs_vwap         = g_gold_stack.vwap();
        const double mcs_mid          = (bid + ask) * 0.5;
        const double mcs_vwap_disp    = (mcs_vwap > 0.0)
            ? std::fabs(mcs_mid - mcs_vwap) : 0.0;
        const bool mcs_macro_bypass   = (mcs_vwap_disp >= 6.0)
                                     || (std::fabs(g_gold_stack.ewm_drift()) >= 2.0)
                                     || crash_impulse_bypass;
        const bool mcs_vol_ok = (mcs_vol_range >= 0.5)
                             || (mcs_vol_unseeded && mcs_macro_bypass)
                             || (!mcs_vol_unseeded && mcs_macro_bypass);
        const bool microscalper_can_enter =
            gold_can_enter
            && mcs_vol_ok
            && !g_gold_microscalper.has_open_position();

        // 2026-05-08 S21 LIVE-ENTRY ORDER (authorised by user in chat):
        //   Detect the "just opened" transition by snapshotting
        //   has_open_position() BEFORE the entry-path on_tick call, then
        //   comparing to the post-call state. If pre=false and post=true,
        //   the engine just opened a fresh position on this tick. Fire a
        //   market order to the broker IFF the engine is not in shadow.
        //   This pairs with microscalper_on_close in trade_lifecycle.hpp
        //   which sends the corresponding market close on every exit
        //   reason.
        //
        //   Why this snapshot-and-diff pattern rather than a time-based
        //   freshness check (e.g., "entry_ts within last second"): the
        //   tick rate is ~200/min so multiple ticks land in the same UTC
        //   second after an entry, and a time-based gate would fire
        //   duplicate orders. A pre/post transition diff fires exactly
        //   once per entry, by construction.
        //
        //   NOTE: this implementation is intentionally minimal -- no
        //   partial-fill reconciliation (broker may fill less than 0.30;
        //   engine internal pos.size won't auto-update), no reject
        //   handling (broker may reject; engine state would diverge from
        //   broker state). For a single-engine deploy in a single trading
        //   session under operator attention, the simple form is
        //   acceptable; the proper hardening is queued follow-up.
        //
        // ── 2026-05-08 S21 NEXT-ITERATION ACCOUNTING TODO ──────────────
        // Required before this engine is left running unattended:
        //
        //   1. Track open clOrdId per microscalper position. Store the
        //      send_live_order return value in a per-engine "open_clOrdId"
        //      field. Cleared when matching ExecutionReport (8=8 fill or
        //      8=2 reject) arrives.
        //   2. ExecReport-driven pos.size update. handle_execution_report
        //      already parses fills into g_live_orders; extend it so a
        //      partial fill on a microscalper clOrdId mutates
        //      g_gold_microscalper.pos.size to the actual filled qty.
        //      Engine then closes whatever it actually has, not what it
        //      thought it had.
        //   3. Reject handling. If ExecReport reports 8=2 (rejected) or
        //      8=4 (cancelled) for an open clOrdId, immediately:
        //        a. Force pos.active=false on the engine (engine has no
        //           position because broker has none).
        //        b. Auto-pin g_gold_microscalper.shadow_mode = true so
        //           subsequent entries don't repeat the failure.
        //        c. Print [MICROSCALPER-REJECT] log line + emit a
        //           [RISK-MON] notification.
        //   4. Close-side clOrdId tracking. Same as (1) but for the close
        //      order. If the close is rejected, the engine's pos is
        //      already cleared internally but the broker still has an
        //      open position -- need to re-fire the close order with a
        //      bounded retry (3 attempts at 200ms intervals) before
        //      giving up and printing [MICROSCALPER-CLOSE-FAIL].
        //   5. Position reconciliation heartbeat. Every 60s, query the
        //      broker's PositionReport (35=AP) and compare net position
        //      vs engine pos.active * pos.size * (is_long?+1:-1). If
        //      they disagree, log [MICROSCALPER-DRIFT] and auto-shadow.
        //   6. Sanity bound on lot. If pos.size from the engine ever
        //      exceeds max_lot_gold (config), refuse the send_live_order
        //      and auto-shadow. Belt for the LIVE_LOT vs max_lot mismatch
        //      bug class that was flagged in the S20 handoff.
        //
        // Until items 1-6 ship, treat this engine as operator-attended
        // only. Walk away => stop the service. The OMEGA.ps1 stop manual
        // kill is the operator's safety net; the engine cannot be trusted
        // to recover from a partial-fill or reject by itself.
        // ──────────────────────────────────────────────────────────────
        const bool ms_pre_entry_open = g_gold_microscalper.has_open_position();
        if (!ms_pre_entry_open && !g_disable_microscalper) {   // S-2026-06-02 CULLED (L2 check failed)
            g_gold_microscalper.on_tick(bid, ask, now_ms_g,
                                        microscalper_can_enter,
                                        microscalper_on_close,
                                        g_macro_ctx.gold_l2_imbalance,
                                        g_macro_ctx.gold_slope,
                                        g_macro_ctx.gold_vacuum_ask,
                                        g_macro_ctx.gold_vacuum_bid,
                                        g_macro_ctx.gold_l2_real);
        }
        const bool ms_post_entry_open = g_gold_microscalper.has_open_position();
        if (!ms_pre_entry_open && ms_post_entry_open &&
            !g_gold_microscalper.shadow_mode) {
            // 2026-05-08 S21 HEDGING-MODE FIX: clear any stale position ID
            // before the entry order is sent. The fresh entry has no broker
            // position ID yet -- the ACK handler will populate it once the
            // ExecutionReport arrives with tag 1006 / 721. Clearing here
            // means the close-side gate will correctly REFUSE to send a
            // close if the broker hasn't ACKed yet (very tight race window).
            g_gold_microscalper.pos.broker_position_id.clear();

            const auto& mp = g_gold_microscalper.pos;
            // Use the existing FIRE log marker so the live-order line is
            // immediately adjacent to the engine's own FIRE log.
            std::cout << "\033[1;33m[MICROSCALPER-ENTRY] XAUUSD "
                      << (mp.is_long ? "BUY" : "SELL")
                      << " qty=" << mp.size
                      << " entry=" << std::fixed << std::setprecision(4) << mp.entry
                      << "\033[0m\n";
            std::cout.flush();
            // Entry-side: no position_id (this IS the entry, broker books a
            // fresh position).
            const std::string entry_clOrdId = send_live_order(
                "XAUUSD", mp.is_long, mp.size, mp.entry);
            // Store the entry clOrdId so handle_execution_report can match
            // the inbound ExecReport to THIS specific entry.
            g_gold_microscalper.pos.entry_clOrdId = entry_clOrdId;
        }
    }

    // -- 2026-05-02: XauusdFvgEngine dispatch --------------------------------
    // 15-minute-bar FVG engine on the XAUUSD tick stream. The engine
    // accumulates ticks into 15-min UTC-aligned OHLC bars internally and
    // runs FVG detection / mitigation entry on each bar close. Per design
    // doc §7.3 the engine is fed every tick (so its bar accumulator,
    // ATR(14) RMA, and tv_mean rolling-20 stay live regardless of whether a
    // position is open) and gated at entry by gold_can_enter (which already
    // includes g_xauusd_fvg.has_open_position() via gold_any_open above, so
    // the engine cannot fire while another gold-cohort engine holds a
    // position OR when FVG itself is mid-trade -- one-position-at-a-time
    // across the whole gold cohort, as designed).
    //
    // The engine fires omega::TradeRecord through its own on_close_cb wired
    // in engine_init.hpp -- no second close-callback is plumbed here. We
    // pass nullptr for the on_close arg below so on_tick falls back to
    // on_close_cb, mirroring the comment in XauusdFvgEngine::on_tick.
    // S46 2026-05-27: gated by g_disable_xauusd_fvg (default true) pending
    // real-class audit. Has-position branch keeps manage path alive so
    // any in-flight position exits cleanly via SL/timeout.
    if (g_xauusd_fvg.has_open_position() || !g_disable_xauusd_fvg) {
        g_xauusd_fvg.on_tick(bid, ask, now_ms_g,
                             gold_can_enter,
                             nullptr);
    }

    // ?? GoldScalpPyramid (2026-05-18) ????????????????????????????????????????
    // M5 scalper with pyramid + aggressive trail. Fed every tick for bar
    // accumulation + per-tick trail management. Entry gated by gold_can_enter.
    // Close callback wired in engine_init.hpp -> handle_closed_trade.
    // L2 fields from MacroContext: imbalance, slope, vacuum, wall, liveness.
    // When gold_l2_real=false, engine degrades all L2 filters to neutral.

    // ?? GoldRegimeDaily (2026-05-19 S110) ?????????????????????????????????????
    // H4 EMA-cross trend-follow. Fed every tick for H4 bar accumulation +
    // per-tick exit management (BE-lock, TP, hard SL, trend-flip on EMA9
    // vs EMA21 cross-back). L2 fields ignored by this engine (signature
    // arg slots filled with neutral values).
    // Close callback wired in engine_init.hpp -> handle_closed_trade.

    // ?? BBandScalpEngine (2026-05-18 part B) ?????????????????????????????????
    // M1 Bollinger + RSI mean-reversion scalper. Indicator inputs are pulled
    // from the persisted M1 atomics (bb_upper/mid/lower, rsi14, atr14) so the
    // engine is firable from tick 1 with no cold-prime window. Entry gated by
    // gold_can_enter (which already folds in g_bband_scalp.has_open_position()
    // via gold_any_open). Close callback wired in engine_init.hpp ->
    // handle_closed_trade.
    //
    // L2 not required: engine accepts l2_imbalance / l2_real but does NOT
    // block entry on stale L2. This lets the engine validate on price-only
    // historical tape and run unmodified in live regardless of DOM freshness.

    // ?? NBM London position management -- ALWAYS runs when position open ??
    // entry guard, so _manage_position() (SL/VWAP trail) was never reached once a
    // position was open. Fix: call on_tick unconditionally when position is open.
    // on_tick returns {} immediately after _manage_position() when pos_.active.

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
        (void)pdhl_ts_ms;
        auto pdhl_cb = [](const omega::TradeRecord& tr){ g_omegaLedger.record(tr); };
        (void)pdhl_cb;
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
        // Audit-disable gate (2026-04-30): if g_disable_candle_flow is true,
        // skip the call when no position is open. Existing open positions still
        // get on_tick (manage path) so they exit cleanly via SL/timeout.
        if (g_candle_flow.has_open_position() || !g_disable_candle_flow) {
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

    // ── BreakBounceEngine -- self-managing, independent position (shadow) ───
    // D1 bias / H1 break / M20 retest. on_trade_record -> handle_closed_trade
    // (wired in engine_init). Push the live L2 imbalance every tick so the
    // (off-by-default) L2 profit-protect has fresh book flow when enabled.
    g_engine_heartbeat.pulse("BreakBounce");

}

