#pragma once
// engine_init.hpp -- Engine configuration, wiring, and startup.
// All engine config, callback wiring, state loading, and startup checks
// that were previously embedded in main() have been extracted here.
// Called once from main() as: init_engines(cfg_path);
//
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static void init_engines(const std::string& cfg_path)
{
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
    // RSIReversalEngine -- tuned for high-frequency XAUUSD mean-reversion scalping.
    // Based on documented backtest results (TradingView, QuantifiedStrategies):
    //   RSI 30/70 extremes on tick data = genuine exhaustion, not chop noise.
    //   EMA200 trend filter in tick_gold gates direction (LONG only above, SHORT only below).
    //   Exit at RSI 50 = full mean reversion captured.
    //   2:1 R:R: SL = 0.5x ATR, TP via RSI_EXIT at 50.
    //   Cooldown 15s = high frequency, gold RSI cycles every 30-60s at extremes.
    //   MAX_HOLD 90s = scalp timeframe, don't overstay.
    // Win rate target: 55-70%, profit factor >1.4 (per backtested research).
    g_rsi_reversal.enabled        = true;   // ENABLED -- RSI turn strategy
    g_rsi_reversal.shadow_mode    = true;   // SHADOW first -- verify signals before live
    g_rsi_reversal.RSI_OVERSOLD   = 42.0;  // turn from any low -- not just extreme OS
    g_rsi_reversal.RSI_OVERBOUGHT = 58.0;  // turn from any high -- not just extreme OB
    g_rsi_reversal.RSI_EXIT_LONG  = 55.0;  // exit when RSI recovers to 55
    g_rsi_reversal.RSI_EXIT_SHORT = 45.0;  // exit when RSI fades to 45
    g_rsi_reversal.SL_ATR_MULT    = 0.6;   // SL = 0.6x ATR
    g_rsi_reversal.TRAIL_ATR_MULT = 0.40;  // trail at 0.4x ATR behind MFE
    g_rsi_reversal.BE_ATR_MULT    = 0.40;  // BE at 0.4x ATR profit
    g_rsi_reversal.COOLDOWN_S     = 30;    // 30s between entries
    g_rsi_reversal.COOLDOWN_S_VACUUM = 15; // 15s with vacuum confirm
    g_rsi_reversal.MAX_HOLD_S     = 300;   // 5min max -- RSI turns can take time
    g_rsi_reversal.MIN_HOLD_S     = 5;     // 5s minimum hold
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
    // MicroMomentum tuned to emulate observed winning pattern:
    // Winners are SLOPE_EXIT trades: 8-17s hold, +$36-63 = 3-4pt moves at 0.01 lots.
    // Losers are SL hits where prior SL_ATR_MULT=1.5 * ATR=8pt = 12pt SL -- far too wide.
    // New design: tight SL to match the move size, lean on SLOPE_EXIT as primary exit.
    //   SL_ATR_MULT=0.3 -> 0.3*8=2.4pt SL -- cuts losers before they compound
    //   TP_PTS=3.0       -> matches observed winner move size
    //   BE_TRIGGER=0.5   -> BE faster so winners never flip to losers
    //   TRAIL_DIST=0.5   -> tight trail to lock small wins
    //   COOLDOWN=15s     -> fast scalp engine, more opportunities
    //   MAX_HOLD=45s     -> force exit if move stalls, don't overstay
    g_micro_momentum.enabled           = false;  // DISABLED -- no edge confirmed in backtest
    g_micro_momentum.shadow_mode       = true;
    g_micro_momentum.ENTRY_DISP_PTS    = 0.0;
    g_micro_momentum.RSI_DELTA_MIN     = 6.0;
    g_micro_momentum.RSI_DELTA_WINDOW  = 10;
    g_micro_momentum.BORDERLINE_DELTA  = 3.0;
    g_micro_momentum.RSI_LEVEL_LONG    = 52.0;
    g_micro_momentum.RSI_LEVEL_SHORT   = 48.0;
    g_micro_momentum.PRICE_MOVE_MIN    = 1.5;
    g_micro_momentum.L2_LONG_MIN       = 0.40;
    g_micro_momentum.L2_SHORT_MAX      = 0.60;
    g_micro_momentum.TP_PTS            = 3.0;   // matches observed 3-4pt winner moves
    g_micro_momentum.SL_ATR_MULT       = 0.3;   // 0.3*ATR=2.4pt -- tight, cuts losers fast
    g_micro_momentum.BE_TRIGGER_PTS    = 0.5;   // BE at +0.5pt -- protect early
    g_micro_momentum.LOCK_TRIGGER_PTS  = 1.5;   // lock +0.5pt at +1.5pt
    g_micro_momentum.LOCK_SL_PTS       = 0.5;   // locked SL 0.5pt from entry
    g_micro_momentum.TRAIL_DIST_PTS    = 0.5;   // tight 0.5pt trail
    g_micro_momentum.COOLDOWN_S        = 15;    // fast scalp -- 15s between trades
    g_micro_momentum.MAX_HOLD_S        = 45;    // force exit at 45s if stalled
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
    g_bracket_gold.MIN_BREAK_TICKS = 5;  // raised 3->5: extra sweep protection
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
    g_bracket_gold.MIN_RANGE   = 2.5;  // raised 1.0->2.5: filter small noisy compressions
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
    // Indices: raised 0.20->0.40% -- USTEC at $24000: 0.20%=$48 fires on noise.
    // 0.40%=$96 requires a genuine VWAP dislocation not a 2-tick wiggle.
    // MAX_EXTENSION raised 0.80->1.20%: prevents blocking real dislocations.
    // MAX_HOLD raised 900->600s: exit stalled trades faster (was holding 15min losers).
    g_vwap_rev_sp.enabled = false; g_vwap_rev_sp.EXTENSION_THRESH_PCT    = 0.35; g_vwap_rev_sp.COOLDOWN_SEC    = 300;
    g_vwap_rev_sp.MAX_EXTENSION_PCT       = 1.20;
    g_vwap_rev_sp.MAX_HOLD_SEC            = 600;
    g_vwap_rev_nq.enabled = false; g_vwap_rev_nq.EXTENSION_THRESH_PCT    = 0.40; g_vwap_rev_nq.COOLDOWN_SEC    = 300;
    g_vwap_rev_nq.MAX_EXTENSION_PCT       = 1.20;
    g_vwap_rev_nq.MAX_HOLD_SEC            = 600;
    g_vwap_rev_ger40.enabled = false; g_vwap_rev_ger40.EXTENSION_THRESH_PCT = 0.30; g_vwap_rev_ger40.COOLDOWN_SEC = 300;
    g_vwap_rev_ger40.MAX_EXTENSION_PCT    = 1.00;
    g_vwap_rev_ger40.MAX_HOLD_SEC         = 600;
    // EURUSD: 0.12% extension threshold (FX moves more precisely, smaller range)
    g_vwap_rev_eurusd.enabled = false; g_vwap_rev_eurusd.EXTENSION_THRESH_PCT = 0.12; g_vwap_rev_eurusd.COOLDOWN_SEC = 120;
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

    // DISABLED: TrendPullback gold fires counter-trend repeatedly in Asia with no valid
    // H4 gate (h4_trend_state_=0 until 14 H4 bars seen = 56hrs on fresh start).
    // All session trades: TIME_STOP losses shorting an uptrend. No edge confirmed.
    g_trend_pb_gold.enabled = false;

    // HTF swing engines v2 -- per-instrument params, partial TP, weekend close gate.
    // shadow_mode=true always. To go live: validate shadow signals then set false.
    {
        g_h1_swing_gold.p           = omega::make_h1_gold_params();
        g_h1_swing_gold.symbol      = "XAUUSD";
        g_h1_swing_gold.shadow_mode = true;
        g_h1_swing_gold.enabled     = true;
        g_h4_regime_gold.p           = omega::make_h4_gold_params();
        g_h4_regime_gold.symbol      = "XAUUSD";
        g_h4_regime_gold.shadow_mode = true;
        g_h4_regime_gold.enabled     = true;
        printf("[INIT] H1SwingEngine  XAUUSD: shadow=true adx_min=%.0f sl=%.1fx"
               " tp1=%.1fx trail_arm=%.1fx trail_dist=%.1fx daily_cap=$%.0f\n",
               g_h1_swing_gold.p.adx_min,    g_h1_swing_gold.p.sl_mult,
               g_h1_swing_gold.p.tp1_mult,   g_h1_swing_gold.p.tp2_trail_arm_mult,
               g_h1_swing_gold.p.tp2_trail_dist_mult, g_h1_swing_gold.p.daily_cap);
        printf("[INIT] H4RegimeEngine XAUUSD: shadow=true channel=%d bars adx=%.0f"
               " sl=%.1fx tp=%.1fx trail_arm=%.1fx trail_dist=%.1fx daily_cap=$%.0f\n",
               g_h4_regime_gold.p.channel_bars, g_h4_regime_gold.p.adx_min,
               g_h4_regime_gold.p.sl_struct_mult, g_h4_regime_gold.p.tp_mult,
               g_h4_regime_gold.p.trail_arm_mult, g_h4_regime_gold.p.trail_dist_mult,
               g_h4_regime_gold.p.daily_cap);
        fflush(stdout);
    }

    // DISABLED: Index TrendPullback never explicitly disabled -- no live validation.
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
    g_trend_pb_nq.enabled             = false;   // DISABLED: not live-validated
    g_trend_pb_sp.MIN_EMA_SEP         = 15.0;
    g_trend_pb_sp.DAILY_LOSS_CAP      = 80.0;   // same cap for SP
    g_trend_pb_sp.enabled             = false;   // DISABLED: not live-validated
    g_trend_pb_ger40.DAILY_LOSS_CAP   = 80.0;   // GER40 too -- no cap was previously set
    g_trend_pb_ger40.enabled          = false;   // DISABLED: not live-validated
    // Load warm EMA state -- skips EMA_WARMUP_TICKS cold period on restart
    g_trend_pb_gold.load_state(state_root_dir()  + "/trend_pb_gold.dat");
    g_trend_pb_ger40.load_state(state_root_dir() + "/trend_pb_ger40.dat");
    g_trend_pb_nq.load_state(state_root_dir()    + "/trend_pb_nq.dat");
    g_trend_pb_sp.load_state(state_root_dir()    + "/trend_pb_sp.dat");

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
        // H1 bars: gold + indices swing context (warm restart for IndexSwingEngine)
        g_bars_gold.h1.load_indicators(base + "/bars_gold_h1.dat");
        g_bars_sp.h1  .load_indicators(base + "/bars_sp_h1.dat");
        g_bars_nq.h1  .load_indicators(base + "/bars_nq_h1.dat");
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

    // ?? IndexSwingEngine configure -- shadow mode confirmed ??????????????????
    // sl_pts / min_ema_sep / pnl_scale set at construction in omega_types.hpp.
    // shadow_mode=true: engine tracks paper P&L only, no FIX orders sent.
    // NEVER set shadow_mode=false without explicit authorization + live validation.
    g_iswing_sp.shadow_mode = true;
    g_iswing_nq.shadow_mode = true;

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
            const std::string kelly_dir = state_root_dir() + "/kelly";
            // ensure_parent_dir creates the directory if needed
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(kelly_dir, ec);
            g_adaptive_risk.load_perf(kelly_dir);
        }

        // Load GoldFlowEngine ATR state -- eliminates 100-tick blind zone on restart
        {
            const std::string atr_path        = state_root_dir() + "/gold_flow_atr.dat";
            const std::string atr_backup_path = state_root_dir() + "/gold_flow_atr_backup.dat";

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
            const std::string gs_path = state_root_dir() + "/gold_stack_state.dat";
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
        g_corr_matrix.load_state(state_root_dir() + "/corr_matrix.dat");
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
            // ENGINE CONFIG SUMMARY -- must be emitted AFTER tee opens so it goes into the log file.
            // RSI-REV and GFE-CONFIG are configured early (before tee) and printed to NSSM stdout only.
            // Repeating them here guarantees VERIFY_STARTUP and QUICK_RESTART engine checks can find them.
            std::cout << "[GFE-CONFIG] goldflow_enabled=" << (g_cfg.goldflow_enabled ? "true" : "false")
                      << " -- GoldFlow entries are "
                      << (g_cfg.goldflow_enabled ? "ACTIVE" : "DISABLED") << "\n";
            std::cout << "[RSI-REV] RSIReversalEngine configured (shadow_mode="
                      << (g_rsi_reversal.shadow_mode ? "true" : "false")
                      << " oversold=" << (int)g_rsi_reversal.RSI_OVERSOLD
                      << " overbought=" << (int)g_rsi_reversal.RSI_OVERBOUGHT
                      << " sl_mult=" << g_rsi_reversal.SL_ATR_MULT << "x)\n";
            std::cout.flush();
        }
    }
}


