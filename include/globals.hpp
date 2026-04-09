// globals.hpp -- extracted from main.cpp
// Section: globals (original lines 435-964)
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

// ?? Per-symbol config manager -- loaded from symbols.ini at startup ????????????
static SymbolConfigManager g_sym_cfg;

static omega::SpEngine    g_eng_sp("US500.F");
static omega::NqEngine    g_eng_nq("USTEC.F");
static omega::OilEngine   g_eng_cl("USOIL.F");
static omega::Us30Engine  g_eng_us30("DJ30.F");
static omega::Nas100Engine g_eng_nas100("NAS100");
static omega::EuIndexEngine  g_eng_ger30("GER40");
static omega::EuIndexEngine  g_eng_uk100("UK100");
static omega::EuIndexEngine  g_eng_estx50("ESTX50");
static omega::BreakoutEngine g_eng_xag("XAGUSD");
static omega::BreakoutEngine g_eng_eurusd("EURUSD");
static omega::BreakoutEngine g_eng_gbpusd("GBPUSD");
static omega::BrentEngine    g_eng_brent("BRENT");
static omega::BreakoutEngine g_eng_audusd("AUDUSD");
static omega::BreakoutEngine g_eng_nzdusd("NZDUSD");
static omega::BreakoutEngine g_eng_usdjpy("USDJPY");

// Shared macro context -- updated each tick, read by SP/NQ shouldTrade()
static omega::MacroContext g_macro_ctx;

// Multi-engine gold stack -- CompressionBreakout + ImpulseContinuation +
// SessionMomentum + VWAPSnapback + LiquiditySweepPro + LiquiditySweepPressure
// Primary gold executor -- sole handler for all XAUUSD ticks.
static omega::gold::GoldEngineStack g_gold_stack;
// Cross-asset engines
static omega::cross::EsNqDivergenceEngine  g_ca_esnq;
static omega::cross::OilEventFadeEngine    g_ca_eia_fade;
static omega::cross::BrentWtiSpreadEngine  g_ca_brent_wti;
static omega::cross::FxCascadeEngine       g_ca_fx_cascade;
static omega::cross::CarryUnwindEngine     g_ca_carry_unwind;
static omega::cross::OpeningRangeEngine    g_orb_us;     // US equity 13:30 UTC
static omega::cross::OpeningRangeEngine    g_orb_ger30;  // Xetra 08:00 UTC
static omega::cross::OpeningRangeEngine    g_orb_silver; // COMEX 13:30 UTC
static omega::cross::OpeningRangeEngine    g_orb_uk100;  // LSE 08:00 UTC, 15-min window
static omega::cross::OpeningRangeEngine    g_orb_estx50; // Euronext 09:00 UTC, 15-min window

// Engine 7: VWAP Reversion -- enter on reversal tick back toward daily VWAP
// Wired to: US500.F, USTEC.F, GER40, EURUSD
static omega::cross::VWAPReversionEngine   g_vwap_rev_sp;     // US500.F
static omega::cross::VWAPReversionEngine   g_vwap_rev_nq;     // USTEC.F
static omega::cross::VWAPReversionEngine   g_vwap_rev_ger40;  // GER40
static omega::cross::VWAPReversionEngine   g_vwap_rev_eurusd; // EURUSD

// Engine 9: Noise Band Momentum -- Zarattini/Aziz/Maroy research (Sharpe 3.0-5.9)
// Rolling ATR noise band since session open. Entry on band breakout.
// VWAP crossing is primary stop. One instance per instrument.
// Wired to: US500.F, USTEC.F, NAS100, DJ30.F
static omega::cross::NoiseBandMomentumEngine g_nbm_sp;    // US500.F -- NY 13:30-21:30 UTC
static omega::cross::NoiseBandMomentumEngine g_nbm_nq;    // USTEC.F -- NY 13:30-21:30 UTC
static omega::cross::NoiseBandMomentumEngine g_nbm_nas;   // NAS100  -- NY 13:30-21:30 UTC
static omega::cross::NoiseBandMomentumEngine g_nbm_us30;  // DJ30.F  -- NY 13:30-21:30 UTC

// NBM London session engines (07:00-13:30 UTC) -- covers the gap before NY open.
// XAUUSD and USOIL.F are the most liquid instruments in the London window.
// Session anchor = London open (07:00 UTC). Same ATR/band logic as NY engines.
// These are additional instances -- the gold stack and oil engines remain primary.
static omega::cross::NoiseBandMomentumEngine g_nbm_gold_london;  // XAUUSD  -- London 07:00-13:30 UTC
static omega::cross::NoiseBandMomentumEngine g_nbm_oil_london;   // USOIL.F -- London 07:00-13:30 UTC

// Engine 10: SilverTurtleTick -- DISABLED after real-tick backtest FAILURE.
// Backtest on 42,306,203 real XAGUSD ticks (Jan 2023-Jan 2025):
//   Sharpe=-16.23, MaxDD=$18,381, WR=31.8%, 0/24 positive months.
// Root cause: 65% timeout rate, TP=$0.30 = 49x the actual avg 45-min move.
// Instance retained for force_close safety only. No new entries ever.
static omega::cross::SilverTurtleTickEngine g_silver_turtle;  // DISABLED

// Engine 8: Trend Pullback -- EMA9/21/50 trend + pullback to EMA50 + bounce confirmation
// Wired to: XAUUSD (gated -- no other gold position), GER40, USTEC.F, US500.F
// TrendPullback handles slow grind trades that VWAPReversion times out on.
// Enters on EMA50 pullback, trails ATR behind MFE, no timeout.
static omega::cross::TrendPullbackEngine   g_trend_pb_gold;   // XAUUSD
static omega::cross::TrendPullbackEngine   g_trend_pb_ger40;  // GER40
static omega::cross::TrendPullbackEngine   g_trend_pb_nq;     // USTEC.F
static omega::cross::TrendPullbackEngine   g_trend_pb_sp;     // US500.F

// =============================================================================
// IndexFlowEngine -- L2 flow + EWM drift engines for US equity indices.
// Architecture mirrors GoldFlowEngine: L2 persistence + EWM drift + ATR-prop SL
// + staircase trail. Per-symbol calibrated (see IndexFlowEngine.hpp).
//
// SHADOW mode: IndexMacroCrashEngine instances are shadow-only by default.
// NEVER set shadow_mode=false without explicit authorization.
//
// L2 data: fed from existing AtomicL2 instances (g_l2_sp, g_l2_nq, g_l2_nas,
// g_l2_us30) already updated by cTrader depth thread in omega_main.hpp.
// Pass l2_imb via: g_l2_sp.imbalance.load(std::memory_order_relaxed)
// =============================================================================
static omega::idx::IndexFlowEngine       g_iflow_sp("US500.F");
static omega::idx::IndexFlowEngine       g_iflow_nq("USTEC.F");
static omega::idx::IndexFlowEngine       g_iflow_nas("NAS100");
static omega::idx::IndexFlowEngine       g_iflow_us30("DJ30.F");
static omega::idx::IndexMacroCrashEngine g_imacro_sp("US500.F"); // shadow_mode=true always
static omega::idx::IndexMacroCrashEngine g_imacro_nq("USTEC.F"); // shadow_mode=true always

// VWAPAtrTrail -- ATR-proportional BE lock + trail upgrade for existing
// VWAPReversionEngine instances. Holds upgrade state only (no new entries).
// Applied each tick after g_vwap_rev_sp/nq.on_tick() in tick_indices.hpp.
static omega::idx::VWAPAtrTrail g_vwap_atr_trail_sp;   // US500.F
static omega::idx::VWAPAtrTrail g_vwap_atr_trail_nq;   // USTEC.F
static omega::idx::VWAPAtrTrail g_vwap_atr_trail_nas;  // NAS100 (no VWAPRev, unused)
static omega::idx::VWAPAtrTrail g_vwap_atr_trail_us30; // DJ30.F (no VWAPRev, unused)

// Co-location latency edge stack -- GoldSpreadDislocation + GoldEventCompression.
// GoldSilverLeadLag DELETED 2026-03-31. Both remaining engines run MANAGE-ONLY
// (new entries disabled -- RTT ~68ms, edge requires <1ms).
static omega::latency::LatencyEdgeStack g_le_stack;

// ?? Cross-engine deduplication -- file-scope so dispatch lambda can see it ??
// Per-symbol timestamp of the last entry across ALL engine types.
// Prevents simultaneous entries from GoldStack + Breakout + Bracket + TrendPB.
static constexpr int64_t CROSS_ENG_DEDUP_SEC = 30;
static std::mutex         g_dedup_mtx;
static std::unordered_map<std::string, int64_t> g_last_cross_entry;

// Bracket engines
#include "BracketEngine.hpp"
#include "GoldFlowEngine.hpp"
#include "MacroCrashEngine.hpp"
static omega::GoldBracketEngine   g_bracket_gold;

// ?? Hybrid bracket engines -- fire both sides simultaneously, cancel loser ??
// GoldHybridBracketEngine: compression range -> long stop + short stop -> cancel loser on fill
// IndexHybridBracketEngine: same for SP/NQ/DJ30/NAS100 with per-symbol calibration
// These are in SHADOW mode by default -- validated against live data before enabling.
#include "GoldHybridBracketEngine.hpp"
#include "IndexHybridBracketEngine.hpp"
static omega::GoldHybridBracketEngine         g_hybrid_gold;
static omega::idx::IndexHybridBracketEngine   g_hybrid_sp(omega::idx::make_sp_config());
static omega::idx::IndexHybridBracketEngine   g_hybrid_nq(omega::idx::make_nq_config());
static omega::idx::IndexHybridBracketEngine   g_hybrid_us30(omega::idx::make_us30_config());
static omega::idx::IndexHybridBracketEngine   g_hybrid_nas100(omega::idx::make_nas100_config());
static GoldFlowEngine             g_gold_flow;
static omega::MacroCrashEngine    g_macro_crash;

// RSI Reversal Engine -- direct RSI extreme entries, bypasses regime system
// LONG when RSI < 35, SHORT when RSI > 65, ATR-based SL + trail
// shadow_mode=true until 30 live shadow trades validate Asia WR
#include "RSIReversalEngine.hpp"
static omega::RSIReversalEngine   g_rsi_reversal;

// MicroMomentumEngine -- fast 4-8pt momentum capture
// RSI slope + price displacement, both directions, all sessions
// shadow_mode=true until validated
#include "MicroMomentumEngine.hpp"
static omega::MicroMomentumEngine g_micro_momentum;  // always-on macro event engine
// Reload instance: independent GoldFlowEngine for continuation entries.
// Fires after g_gold_flow banks PARTIAL_1R and confirms price still moving.
// Managed exactly like g_gold_flow but never arms its own reload (avoids cascade).
// Shares all session/risk gates. Counted toward gold open position cap.
static GoldFlowEngine             g_gold_flow_reload;
// Add-on instance: independent GoldFlowEngine for velocity add-on entries.
// Fires once per base position when base hits trail_stage>=2 + expansion + vol_ratio>2.5.
// Entry at current price, SL = base trail SL (already in profit territory).
// Sized at 50% of base full_size. Has its own staircase, trail, ratchet.
// Never arms its own addon or reload (prevents cascade).
static GoldFlowEngine             g_gold_flow_addon;

// ?? Trend-day multi-engine state ?????????????????????????????????????????????
// Tracks GoldFlow exit details so CompBreakout/Bracket can re-enter on trend days
// and GoldStack can fast-reverse after a reversal signal.
static std::atomic<int64_t>  g_gold_flow_exit_ts{0};      // epoch sec of last GoldFlow exit
static std::atomic<int>      g_gold_flow_exit_dir{0};     // +1=was long, -1=was short
static std::atomic<int>      g_gold_flow_exit_reason{0};  // 0=other 1=SL_HIT 2=trail/BE
// Price at last GoldFlow exit * 100 stored as integer (atomic double workaround)
// Used to detect post-close reversal magnitude for drift reset.
static std::atomic<int64_t>  g_gold_flow_exit_price_x100{0};

// Last GoldFlow gate block reason -- updated every time gf_tick_ok=false
// Used by health watchdog to show specific reason on GUI instead of "NO TRADES Xmin"
static std::atomic<const char*> g_last_gf_block_reason{nullptr};

// Engine pause tracking -- maps engine key to pause_until epoch sec
// Used by health watchdog to detect consecutive loss pauses
static std::unordered_map<std::string, int64_t> g_engine_pause;
// When GoldFlow SL_HIT and drift reverses: allow GoldStack fast counter-entry
// by bypassing the 120s SL cooldown. Window = 60s from flow exit.
static std::atomic<int64_t>  g_gold_reversal_window_until{0};
// P4 velocity re-entry bypass: set to 1 when velocity_active + no open > 5min + conf>1.0.
// Allows GoldFlow to bypass the exclusivity gate on confirmed velocity expansion days.
// Only read inside the GoldFlow on_tick block -- not used by other engines.
static std::atomic<int>       g_gf_vel_reentry_bypass{0};
static std::atomic<int64_t>  g_gold_post_impulse_until{0};  // block new entries 3min after IMPULSE ends
static std::atomic<int>      g_gold_impulse_ticks{0};          // consecutive IMPULSE ticks -- must be >=3 before GoldFlow enters on IMPULSE
static std::atomic<int64_t>  g_gold_trail_block_until{0};    // same-dir re-entry blocked 30s after trail/BE (GoldStack)
static std::atomic<int>       g_gold_trail_block_dir{0};      // direction that was blocked (+1=long, -1=short)
// crash_impulse_bypass consecutive-SL cooldown
// After GF_CRASH_BYPASS_CONSEC_SL_MAX consecutive SL_HITs while crash bypass is active,
// bypass is blocked for GF_CRASH_BYPASS_COOLDOWN_SEC (15 min) to prevent runaway losses
// during fake/exhausted crash impulses.
static constexpr int     GF_CRASH_BYPASS_CONSEC_SL_MAX  = 3;
static constexpr int64_t GF_CRASH_BYPASS_COOLDOWN_SEC   = 900;  // 15 min
static std::atomic<int>      g_gf_crash_consec_sl{0};           // consecutive SL_HIT counter while bypass active
static std::atomic<int64_t>  g_gf_crash_bypass_block_until{0};  // epoch sec: bypass blocked until this time
// Directional SL cooldown: after 2 SL_HITs in same direction within 5 min, block that direction 3 min.
// Prevents 5 consecutive longs into a crash (April 2 2026 pattern).
static constexpr int     GF_DIR_SL_MAX          = 2;    // SL_HITs before direction blocked
static constexpr int64_t GF_DIR_SL_WINDOW_SEC   = 300;  // 5 min window
static constexpr int64_t GF_DIR_SL_COOLDOWN_SEC = 180;  // 3 min block
static std::atomic<int>      g_gf_dir_sl_long_count{0};   // consecutive long SL_HITs
static std::atomic<int>      g_gf_dir_sl_short_count{0};  // consecutive short SL_HITs
static std::atomic<int64_t>  g_gf_dir_sl_long_first{0};   // timestamp of first long SL in window
static std::atomic<int64_t>  g_gf_dir_sl_short_first{0};  // timestamp of first short SL in window
static std::atomic<int64_t>  g_gf_long_blocked_until{0};  // block long entries until this time
static std::atomic<int64_t>  g_gf_short_blocked_until{0}; // block short entries until this time
// ENGINE-CULLED atomic: set when GoldFlow hits 4 consecutive SL_HITs.
// Mirrors engine_culled in g_shadow_quality but readable cross-thread (quote thread checks it).
// g_shadow_quality is only safe to read in the trade thread -- this atomic is the bridge.
// Reset: g_shadow_quality.clear() at midnight resets the shadow map; this atomic mirrors it.
static std::atomic<bool>     g_gf_engine_culled{false};   // true = GoldFlow disabled until midnight
// L2 watchdog: set true when cTrader depth (ctid=43014358) not flowing > 120s.
// GoldFlow entry gated on this -- drift-only mode has no proven edge.
// Cleared automatically when L2 recovers. Written by L2 watchdog thread in omega_main.hpp.
// IMMUTABLE: ctid=43014358 is the only account delivering L2 depth.
static std::atomic<bool>     g_l2_watchdog_dead{false};   // true = L2 dead, GoldFlow entries blocked

// ?? FEED-STALE gate ??????????????????????????????????????????????????????????
// Set true when cTrader depth has subscribed for XAUUSD but delivered ZERO depth
// events for >= FEED_STALE_THRESHOLD_S seconds after subscription.
// Root cause of April 5/6 frozen-session: broker ACKs the depth sub but sends no
// events, leaving FIX fallback active with a stale cached price from the prior
// session. This produces 452 identical ticks (4677.03/4677.25) with vol_range=0.00
// for the entire session while the real market moves $40.
//
// BEHAVIOUR WHEN SET:
//   - All XAUUSD new entries blocked (GoldFlow + GoldStack + BracketEngine)
//   - Position management (SL/trail) continues unaffected
//   - [FEED-STALE] logged once per 60s so the frozen state is unmissable
//   - CTraderDepthClient escalates: re-subscribe -> full reconnect
//   - Cleared when XAUUSD depth events resume flowing
//
// Written by CTraderDepthClient recv_loop (per-symbol starvation watchdog).
// Read by tick_gold.hpp entry gates.
std::atomic<bool>            g_feed_stale_xauusd{false};  // true = cTrader depth subscribed but no XAUUSD events

// ?? Indices FORCE_CLOSE circuit breaker ??????????????????????????????????????
// Problem: on April 2 2026, repeated disconnect/reconnect cycles on US indices
// allowed engines to re-enter immediately after each reconnect FORCE_CLOSE,
// compounding losses -$340. Root cause: FORCE_CLOSE fires on disconnect, then
// the reconnect warmup (30s) expires, and the engine re-enters into the same
// losing conditions with no memory of the prior close.
//
// Fix: stamp a 30-minute indices cooldown whenever a FORCE_CLOSE fires on any
// US index symbol. New entries on ALL US index engines are blocked for 30 minutes.
// The cooldown is per-symbol-group (US indices share one timer) so a NQ disconnect
// doesn't block a simultaneously running SP position.
//
// US indices covered: US500.F, USTEC.F, DJ30.F, NAS100
// EU indices (GER40, UK100, ESTX50) are separate -- different session, independent issue.
//
// Cooldown: 30 min = 1800s. Long enough to cover a full disconnect/reconnect cycle
// and enough time for the regime causing the reconnect loop to resolve.
static constexpr int64_t INDICES_DISCONNECT_COOLDOWN_SEC = 1800; // 30 min
static std::atomic<int64_t> g_indices_disconnect_until{0}; // epoch sec: block US index new entries until
static omega::SilverBracketEngine g_bracket_xag;
// US equity index bracket engines -- arms both sides on compression,
// captures the move regardless of direction. Eliminates wrong-direction losses.
static omega::BracketEngine g_bracket_sp;
static omega::BracketEngine g_bracket_nq;
static omega::BracketEngine g_bracket_us30;
static omega::BracketEngine g_bracket_nas100;
// EU index bracket engines
static omega::BracketEngine g_bracket_ger30;
static omega::BracketEngine g_bracket_uk100;
static omega::BracketEngine g_bracket_estx50;
// Oil/Brent bracket
static omega::BracketEngine g_bracket_brent;
// g_bracket_cl removed -- USOIL.F bracket was never dispatched in on_tick (dead code).
// Oil uses g_eng_cl (breakout) + g_ca_eia_fade + g_ca_brent_wti only.
// FX bracket engines
static omega::BracketEngine g_bracket_eurusd;
static omega::BracketEngine g_bracket_gbpusd;
static omega::BracketEngine g_bracket_audusd;
static omega::BracketEngine g_bracket_nzdusd;
static omega::BracketEngine g_bracket_usdjpy;

// Bracket trade frequency tracking
static int      g_bracket_gold_trades_this_minute = 0;
static int      g_bracket_xag_trades_this_minute  = 0;
static int64_t  g_bracket_gold_minute_start       = 0;
static int64_t  g_bracket_xag_minute_start        = 0;
// Rate-limit vars for new bracket engines (shared int/int64 pairs)
static int      g_bracket_idx_trades_this_minute  = 0;
static int64_t  g_bracket_idx_minute_start        = 0;
static int      g_bracket_fx_trades_this_minute   = 0;
static int64_t  g_bracket_fx_minute_start         = 0;

// ?? Per-symbol bracket trend bias system ?????????????????????????????????????
// Applies to ALL bracket symbols, not just gold.
//
// Problem: during a strong trend (e.g. gold -$66 in 20min), the bracket
// re-arms on every compression. The trend-direction leg wins; the counter-
// trend leg fires on dead-cat bounces ? BREAKOUT_FAIL / SL_HIT losses.
//
// Solution: track consecutive profitable exits per symbol. When N same-
// direction wins occur within TREND_WINDOW_MS, set a trend bias:
//   - Counter-trend re-arm is BLOCKED for COUNTER_BLOCK_MS
//   - L2 imbalance EXTENDS the block (confirming trend) or SHORTENS it (opposing)
//   - When bias is active and L2 strongly confirms, PYRAMIDING is allowed:
//     a second bracket arm in the trend direction with reduced size
//
// L2 integration:
//   l2 > L2_STRONG_THRESHOLD (0.70): bid-heavy ? long pressure ? confirms LONG bias,
//                                     opposes SHORT bias ? shorten SHORT bias block
//   l2 < L2_WEAK_THRESHOLD  (0.30): ask-heavy ? short pressure ? confirms SHORT bias,
//                                    opposes LONG bias ? shorten LONG bias block
//   0.30-0.70: neutral ? no adjustment
//
// Pyramiding: when trend bias is active AND L2 strongly confirms direction AND
// the trend-direction position is already open ? allow a second arm at reduced
// size (PYRAMID_SIZE_MULT) to compound the move.

static constexpr int     BRACKET_TREND_LOOKBACK    = 2;      // consecutive same-dir wins to activate bias
static constexpr int64_t BRACKET_TREND_WINDOW_MS   = 300000; // 5min: exits must cluster to count
static constexpr int64_t BRACKET_COUNTER_BLOCK_MS  = 180000; // 3min base block on counter-trend arm
static constexpr int64_t BRACKET_L2_EXTEND_MS      = 60000;  // L2 confirmation extends block by 1min
static constexpr int64_t BRACKET_L2_SHORTEN_MS     = 60000;  // L2 opposition shortens block by 1min
static constexpr double  L2_STRONG_THRESHOLD       = 0.70;   // bid-heavy: long pressure
static constexpr double  L2_WEAK_THRESHOLD         = 0.30;   // ask-heavy: short pressure
static constexpr double  PYRAMID_SIZE_MULT         = 0.75;   // EA-matched: 75% size add-on for aggressive compounding
static constexpr double  L2_PYRAMID_THRESHOLD      = 0.68;   // pyramid fires on solid (not extreme) L2 confirmation
static constexpr int64_t PYRAMID_SL_COOLDOWN_MS    = 120000; // 2min cooldown after any pyramid SL hit

struct BracketExitRecord {
    bool    is_long;
    bool    was_profitable; // TRAIL_HIT, TP_HIT, BE_HIT = true; SL_HIT, BREAKOUT_FAIL = false
    int64_t ts_ms;
};

struct BracketTrendState {
    std::deque<BracketExitRecord> exits;
    int     bias          = 0;    // 0=none, 1=long_bias (block short arm), -1=short_bias (block long arm)
    int64_t bias_set_ms   = 0;    // when bias was last activated
    int64_t block_until_ms = 0;   // dynamic: extended/shortened by L2
    int64_t last_l2_adj   = 0;    // per-instance L2 throttle (was static -- shared across all symbols)
    int64_t last_pyramid_sl_ms = 0; // timestamp of most recent SL hit on a pyramid trade

    // Called on each bracket close for this symbol
    void on_exit(bool is_long, bool profitable, int64_t now_ms) {
        exits.push_back({is_long, profitable, now_ms});
        // Prune old exits outside the trend window
        while (!exits.empty() && (now_ms - exits.front().ts_ms) > BRACKET_TREND_WINDOW_MS)
            exits.pop_front();

        // ?? Win bias: consecutive profitable same-direction exits ?????????????
        // 2 consecutive wins in same dir ? set trend bias (block counter-direction)
        int consec_long = 0, consec_short = 0;
        for (auto it = exits.rbegin(); it != exits.rend(); ++it) {
            if (!it->was_profitable) break;
            if (it->is_long)  { if (consec_short > 0) break; ++consec_long;  }
            else              { if (consec_long  > 0) break; ++consec_short; }
        }

        // ?? Rejection bias: consecutive losses in same direction ??????????????
        // If the bracket keeps entering LONG and keeps losing, the market is
        // rejecting that direction. Block LONGs, favour SHORTs.
        // Triggered by SL_HIT or BREAKOUT_FAIL (not BE -- breakeven = neutral).
        // This fixes: gold 13:03-13:41 (4 LONG SLs/BFs) and EUR 15:07-15:25 (3 LONG SLs)
        int loss_long = 0, loss_short = 0;
        for (auto it = exits.rbegin(); it != exits.rend(); ++it) {
            if (it->was_profitable) break;  // stop at first win/BE
            if (it->is_long)  { if (loss_short > 0) break; ++loss_long;  }
            else              { if (loss_long  > 0) break; ++loss_short; }
        }
        // ?? 30-min window loss counter -- independent of consecutive streak ????
        // Counts losses per direction within 30 minutes regardless of opposite-dir
        // trades in between. Fixes: 11:27 LONG -$10, 11:37 SHORT (resets consec),
        // 11:49 LONG -$18 -- the two LONG losses 22min apart still trip the block.
        const int64_t WINDOW_30MIN_MS = 1800000LL;
        int window_loss_long = 0, window_loss_short = 0;
        for (auto it = exits.rbegin(); it != exits.rend(); ++it) {
            if ((now_ms - it->ts_ms) > WINDOW_30MIN_MS) break;
            if (!it->was_profitable) {
                if (it->is_long)  ++window_loss_long;
                else              ++window_loss_short;
            }
        }
        const int64_t REJECTION_BLOCK_MS = 1800000; // 30 min window block (was 5min -- too short)
        static constexpr int REJECTION_LOOKBACK = 2; // 2 same-dir losses

        const int prev_bias = bias;
        if (consec_short >= BRACKET_TREND_LOOKBACK && bias != -1) {
            bias           = -1; // short trend ? block LONG arm
            bias_set_ms    = now_ms;
            block_until_ms = now_ms + BRACKET_COUNTER_BLOCK_MS;
        } else if (consec_long >= BRACKET_TREND_LOOKBACK && bias != 1) {
            bias           = 1;  // long trend ? block SHORT arm
            bias_set_ms    = now_ms;
            block_until_ms = now_ms + BRACKET_COUNTER_BLOCK_MS;
        } else if ((loss_long >= REJECTION_LOOKBACK || window_loss_long >= REJECTION_LOOKBACK) && bias != -1) {
            // Market rejected repeated LONG entries -- block LONGs for 30min
            // Triggers on: 2 consecutive LONG losses OR 2 LONG losses within 30min window
            bias           = -1;
            bias_set_ms    = now_ms;
            block_until_ms = now_ms + REJECTION_BLOCK_MS;
        } else if ((loss_short >= REJECTION_LOOKBACK || window_loss_short >= REJECTION_LOOKBACK) && bias != 1) {
            // Market rejected repeated SHORT entries -- block SHORTs for 30min
            bias           = 1;
            bias_set_ms    = now_ms;
            block_until_ms = now_ms + REJECTION_BLOCK_MS;
        }
        if (bias != prev_bias) {
            printf("[BRACKET-TREND] symbol bias=%d consec_l=%d consec_s=%d loss_l=%d loss_s=%d reason=%s\n",
                   bias, consec_long, consec_short, loss_long, loss_short, profitable ? "win" : "loss");
        }
    }

    // Called every tick to apply L2 adjustment to block duration
    // Returns updated block_until_ms (for logging)
    void update_l2(double l2_imb, int64_t now_ms) {
        if (bias == 0 || now_ms >= block_until_ms) { bias = 0; return; }
        // L2 confirms trend ? extend block (market pressure supports the trend continuing)
        // L2 opposes trend  ? shorten block (market pressure suggests reversal coming)
        if (now_ms - last_l2_adj < 5000) return; // throttle to once per 5s
        last_l2_adj = now_ms;
        if (bias == -1 && l2_imb < L2_WEAK_THRESHOLD) {
            // short bias, ask-heavy L2 confirms: extend
            block_until_ms = std::min(block_until_ms + BRACKET_L2_EXTEND_MS,
                                      bias_set_ms + BRACKET_COUNTER_BLOCK_MS * 3);
        } else if (bias == 1 && l2_imb > L2_STRONG_THRESHOLD) {
            // long bias, bid-heavy L2 confirms: extend
            block_until_ms = std::min(block_until_ms + BRACKET_L2_EXTEND_MS,
                                      bias_set_ms + BRACKET_COUNTER_BLOCK_MS * 3);
        } else if (bias == -1 && l2_imb > L2_STRONG_THRESHOLD) {
            // short bias but bid-heavy L2 opposes: shorten
            block_until_ms = std::max(block_until_ms - BRACKET_L2_SHORTEN_MS, now_ms + 5000L);
        } else if (bias == 1 && l2_imb < L2_WEAK_THRESHOLD) {
            // long bias but ask-heavy L2 opposes: shorten
            block_until_ms = std::max(block_until_ms - BRACKET_L2_SHORTEN_MS, now_ms + 5000L);
        }
    }

    // Is counter-trend arming currently blocked?
    bool counter_trend_blocked(int64_t now_ms) const {
        return bias != 0 && now_ms < block_until_ms;
    }

    // Is pyramiding allowed? Requires active bias + strong L2 confirmation + no recent pyramid SL
    bool pyramid_allowed(double l2_imb, int64_t now_ms) const {
        if (bias == 0 || now_ms >= block_until_ms) return false;
        // Block if a pyramid was SL-hit recently -- prevents chasing into reversals
        if (now_ms - last_pyramid_sl_ms < PYRAMID_SL_COOLDOWN_MS) return false;
        if (bias == -1 && l2_imb < (1.0 - L2_PYRAMID_THRESHOLD)) return true;  // short trend + ask-heavy
        if (bias ==  1 && l2_imb > L2_PYRAMID_THRESHOLD)          return true;  // long trend + bid-heavy
        return false;
    }

    // Is a given arm direction allowed? (false = blocked by trend bias)
    // Used to skip sending the counter-trend order when pyramiding
    bool arm_allowed(bool is_long, double l2_imb, int64_t now_ms) const {
        if (!counter_trend_blocked(now_ms)) return true;
        if (bias == -1 && is_long)  return false; // short trend blocks LONG arm
        if (bias ==  1 && !is_long) return false; // long trend blocks SHORT arm
        return true;
    }
};

static std::unordered_map<std::string, BracketTrendState> g_bracket_trend;

// Tracks which symbols currently have an active pyramid (add-on) bracket arm.
// Set when a pyramid bracket fires; cleared on any close for that symbol.
// On SL_HIT while symbol is in this set ? records last_pyramid_sl_ms.
static std::unordered_set<std::string> g_pyramid_clordids;

// ?? Per-symbol supervisors -- one per traded symbol ????????????????????????????
// Each supervisor classifies regime and grants engine permissions each tick.
// Config loaded from symbols.ini via apply_supervisor() at startup.
static omega::SymbolSupervisor g_sup_sp,      g_sup_nq,     g_sup_cl;
static omega::SymbolSupervisor g_sup_us30,    g_sup_nas100;
static omega::SymbolSupervisor g_sup_ger30,   g_sup_uk100,  g_sup_estx50;
static omega::SymbolSupervisor g_sup_xag,     g_sup_eurusd, g_sup_gbpusd;
static omega::SymbolSupervisor g_sup_audusd,  g_sup_nzdusd, g_sup_usdjpy;
static omega::SymbolSupervisor g_sup_brent,   g_sup_gold;
// false-break counters per symbol (reset on cooldown / regime change)
static std::mutex g_false_break_mtx;
static std::unordered_map<std::string, int> g_false_break_counts;

// Book
static std::mutex                              g_book_mtx;
static std::unordered_map<std::string,double>  g_bids;

// ?? Passive L2 observer symbols ???????????????????????????????????????????????
// ?? Passive L2 observer symbols (whitelist-controlled) ????????????????????????
// Subscribe to these cross-pairs for L2 imbalance data ONLY -- never traded.
// WHY WHITELIST: SecurityList has 1481 broker symbols. Subscribing ALL would
// produce a massive FIX message and flood on_tick with unusable pairs.
// We selectively subscribe to ~10 high-signal cross-pairs that confirm our
// traded symbol directions without adding noise.
//
// Filtered out automatically:
//   - .P suffix  = premium/STP feed variants (duplicate data, different ID)
//   - .I suffix  = index variants
//   - .F suffix  = futures variants (we already handle BRENT, USOIL.F directly)
//   - Exotic/illiquid: EURHUF, EURNOK, GBPZAR, SEKJPY, MXNJPY etc.
//
// Cross-pair values used by engines:
//   EURJPY ? CarryUnwind risk-off confirmation (better than VIX alone)
//   GBPJPY ? GBPUSD FxCascade direction amplifier
//   EURGBP ? FxCascade pair selection (which pair leads EUR vs GBP)
//   AUDCAD ? USOIL/XAGUSD commodity flow proxy
//   AUDJPY ? Risk-off confirmation (classic carry barometer)
//   NZDJPY ? NZDUSD FxCascade confirmation
//   EURCHF ? Safe-haven flow (confirms risk-off alongside JPY)
//   CHFJPY ? Pure safe-haven intensity
static const std::unordered_set<std::string> PASSIVE_WHITELIST = {
    "EURJPY", "GBPJPY", "EURGBP", "AUDCAD",
    "AUDJPY", "NZDJPY", "EURCHF", "CHFJPY",
};
// O(1) lookup: name -> true for passive symbols (populated from SecurityList)
static std::mutex                              g_passive_syms_mtx;
static std::unordered_set<std::string>         g_passive_sym_names;  // name set for fast dispatch check
static std::unordered_map<int, std::string>    g_passive_syms;       // id -> name (for subscription)

// Cross-pair imbalance cache -- updated under g_l2_mtx, read each tick
struct CrossPairL2 {
    double eurjpy = 0.5;  // >0.65=EUR bid (risk-on); <0.35=JPY bid (risk-off)
    double gbpjpy = 0.5;  // GBPUSD cascade amplifier
    double eurgbp = 0.5;  // >0.5=EUR leads; <0.5=GBP leads (cascade selection)
    double audcad = 0.5;  // commodity flow for oil/silver direction
    double audjpy = 0.5;  // risk-off barometer (carry unwind confirmation)
    double nzdjpy = 0.5;  // NZDUSD cascade confirmation
    double eurchf = 0.5;  // safe-haven flow (complements JPY signal)
    double chfjpy = 0.5;  // pure safe-haven intensity
};
static CrossPairL2 g_cross_l2;
static std::unordered_map<std::string,double>  g_asks;

// L2Level and L2Book are defined in OmegaFIX.hpp (moved 2026-03-24 for 264=5 upgrade)
std::mutex                                g_l2_mtx;
std::unordered_map<std::string, L2Book>   g_l2_books;

// ?? Per-symbol atomic L2 derived scalars ??????????????????????????????????
// Written by cTrader depth thread after each depth event -- zero lock on hot path.
// FIX tick loop reads these directly without holding g_l2_mtx.
// Full L2Book (walls, vacuums, book_slope) still uses g_l2_mtx -- cold path only.
//
// WHY NOT atomic<L2Book>: L2Book=168 bytes. atomic<T> over 16 bytes is NOT
// lock-free on x86-64 -- MSVC falls back to a hidden internal mutex, which is
// strictly worse. atomic<double>=8 bytes, aligned = genuine lock-free MOV.
struct AtomicL2 {
    std::atomic<double>   imbalance{0.5};       // raw_bid/(raw_bid+raw_ask) from CTDepthBook
    std::atomic<double>   microprice_bias{0.0}; // microprice - mid, signed
    std::atomic<bool>     has_data{false};      // true when book has non-zero sizes
    std::atomic<int64_t>  last_update_ms{0};    // epoch-ms of last cTrader depth event
    std::atomic<int>      raw_bid{0};           // raw bid quote count from CTDepthBook (all levels)
    std::atomic<int>      raw_ask{0};           // raw ask quote count from CTDepthBook (all levels)
    // Microstructure edge score: delta-based signal computed from consecutive L2Book
    // snapshots by GoldMicrostructureAnalyzer in CTraderDepthClient on every DOM event.
    // Range 0..1. >0.65 = bullish DOM pressure, <0.35 = bearish DOM pressure, ~0.5 = neutral.
    // Replaces raw_imbalance() as input to GoldFlowEngine::update_persistence() so the
    // L2 persistence path fires on real order-flow signals instead of perpetual 0.5.
    std::atomic<double>   micro_edge{0.5};      // GoldMicrostructureAnalyzer output (DOM delta score)

    // fresh(): true only when a real book update arrived within max_age_ms.
    // Prevents engines acting on stale/default-initialised imbalance (0.5).
    // Lock-free, called on hot path.
    bool fresh(int64_t now_ms, int64_t max_age_ms = 5000) const noexcept {
        const int64_t t = last_update_ms.load(std::memory_order_relaxed);
        return (t > 0) && ((now_ms - t) <= max_age_ms);
    }
};
static AtomicL2 g_l2_gold;    // XAUUSD
static AtomicL2 g_l2_sp;      // US500.F
static AtomicL2 g_l2_nq;      // USTEC.F
static AtomicL2 g_l2_cl;      // USOIL.F
static AtomicL2 g_l2_xag;     // XAGUSD
static AtomicL2 g_l2_eur;     // EURUSD
static AtomicL2 g_l2_gbp;     // GBPUSD
static AtomicL2 g_l2_aud;     // AUDUSD
static AtomicL2 g_l2_nzd;     // NZDUSD
static AtomicL2 g_l2_jpy;     // USDJPY
static AtomicL2 g_l2_ger40;   // GER40
static AtomicL2 g_l2_uk100;   // UK100
static AtomicL2 g_l2_estx50;  // ESTX50
static AtomicL2 g_l2_brent;   // BRENT
static AtomicL2 g_l2_nas;     // NAS100
static AtomicL2 g_l2_us30;    // DJ30.F

// Map symbol name to AtomicL2* -- used by cTrader write path and FIX tick read path
static AtomicL2* get_atomic_l2(const std::string& sym) noexcept {
    if (sym=="XAUUSD") return &g_l2_gold;
    if (sym=="US500.F")  return &g_l2_sp;
    if (sym=="USTEC.F")  return &g_l2_nq;
    if (sym=="USOIL.F")  return &g_l2_cl;
    if (sym=="XAGUSD")   return &g_l2_xag;
    if (sym=="EURUSD")   return &g_l2_eur;
    if (sym=="GBPUSD")   return &g_l2_gbp;
    if (sym=="AUDUSD")   return &g_l2_aud;
    if (sym=="NZDUSD")   return &g_l2_nzd;
    if (sym=="USDJPY")   return &g_l2_jpy;
    if (sym=="GER40")    return &g_l2_ger40;
    if (sym=="UK100")    return &g_l2_uk100;
    if (sym=="ESTX50")   return &g_l2_estx50;
    if (sym=="BRENT")    return &g_l2_brent;
    if (sym=="NAS100")   return &g_l2_nas;
    if (sym=="DJ30.F")   return &g_l2_us30;
    return nullptr;
}


// ?? cTrader depth tick staleness tracker ?????????????????????????????????????
// Stores the last time (ms) a cTrader depth event arrived per symbol.
// FIX W/X suppresses on_tick when cTrader depth is fresh (<500ms) --
// prevents the 1pt lag from FIX gateway batching in fast markets.
static std::atomic<int64_t> g_ct_ms_xauusd{0}, g_ct_ms_sp{0},  g_ct_ms_nq{0},
                             g_ct_ms_cl{0},    g_ct_ms_xag{0},  g_ct_ms_eur{0},
                             g_ct_ms_gbp{0},   g_ct_ms_aud{0},  g_ct_ms_nzd{0},
                             g_ct_ms_jpy{0},   g_ct_ms_ger40{0},g_ct_ms_uk100{0},
                             g_ct_ms_brent{0}, g_ct_ms_nas{0},  g_ct_ms_us30{0};

static std::atomic<int64_t>* get_ctrader_tick_ms_ptr(const std::string& sym) noexcept {
    if (sym=="XAUUSD")  return &g_ct_ms_xauusd;
    if (sym=="US500.F") return &g_ct_ms_sp;
    if (sym=="USTEC.F") return &g_ct_ms_nq;
    if (sym=="USOIL.F") return &g_ct_ms_cl;
    if (sym=="XAGUSD")  return &g_ct_ms_xag;
    if (sym=="EURUSD")  return &g_ct_ms_eur;
    if (sym=="GBPUSD")  return &g_ct_ms_gbp;
    if (sym=="AUDUSD")  return &g_ct_ms_aud;
    if (sym=="NZDUSD")  return &g_ct_ms_nzd;
    if (sym=="USDJPY")  return &g_ct_ms_jpy;
    if (sym=="GER40")   return &g_ct_ms_ger40;
    if (sym=="UK100")   return &g_ct_ms_uk100;
    if (sym=="BRENT")   return &g_ct_ms_brent;
    if (sym=="NAS100")  return &g_ct_ms_nas;
    if (sym=="DJ30.F")  return &g_ct_ms_us30;
    return nullptr;
}

// ?? Persistent state directory -- separate from logs/ ??????????????????????
// All .dat files (bars, ATR, kelly, trend state) live here.
// Never touched by git reset or log operations.
inline std::string state_root_dir() {
#ifdef _WIN32
    const char* path = "C:\\Omega\\state";
    ::CreateDirectoryA(path, nullptr);
    ::CreateDirectoryA("C:\\Omega\\state\\kelly", nullptr);
    return path;
#else
    return "state";
#endif
}
