#pragma once
// =============================================================================
// BracketTrendState.hpp -- Per-symbol bracket rejection/continuation bias state
//
// Extracted from globals.hpp at Session 6 Priority 1 (engine 1/8 deploy).
// Reason for extraction: the read-only accessor bracket_trend_bias() needs to
// be callable from GoldEngineStack.hpp (used by both main Omega and
// OmegaBacktest), but globals.hpp is SINGLE-TRANSLATION-UNIT (main-only),
// which caused OmegaBacktest to fail linking with an unresolved reference:
//     LNK2019: unresolved external symbol "int __cdecl bracket_trend_bias(char const *)"
//     referenced in VWAPStretchReversionEngine::process
//
// Architecture:
//   - All types + `static` map + inline accessor live HERE.
//   - Because g_bracket_trend is declared `static`, each translation unit that
//     includes this header gets its own private copy. In the main Omega TU the
//     map is populated by trade_lifecycle.hpp via BracketTrendState::on_exit.
//     In the OmegaBacktest TU the map stays empty (no live-trade-exit plumbing
//     is wired into the backtest), so bracket_trend_bias() always returns 0
//     and all consumer engines run unguarded -- which is the correct
//     historical-simulation behaviour (the backtest does not model live
//     bracket trend state).
//
// This extraction changes NO runtime behaviour in main Omega. The types, the
// constants, the accessor body, and the g_bracket_trend map are all
// byte-identical to what was in globals.hpp; the file they live in has just
// changed. All existing consumers (on_tick.hpp, tick_gold.hpp,
// trade_lifecycle.hpp) already get access to these symbols because they are
// only included from main.cpp, which now pulls this header via globals.hpp.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <string>
#include <deque>
#include <unordered_map>
#include <algorithm>

// ?? Bracket-trend bias parameters ????????????????????????????????????????????
// Applies to ALL bracket symbols, not just gold. See the narrative comment
// block in globals.hpp (immediately above the #include of this file) for the
// design rationale.
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

// Bracket-trend exit outcome classification (three-state, not two).
// BE_HIT is NEUTRAL -- neither a real win nor a loss. Previous two-state
// encoding lumped BE into was_profitable=true, which caused the loss
// scanner in BracketTrendState::on_exit to reset on every BE exit and
// prevented rejection bias from ever activating on BE-peppered days
// (e.g. Apr 21 2026: 13:08/13:39/17:05/19:42 all BE_HIT, [BRACKET-TREND]
// never logged, trend_bias stayed 0 all session, four downstream
// features in tick_gold.hpp dormant).
static constexpr int BRACKET_EXIT_WIN     =  1;  // TRAIL_HIT, TP_HIT
static constexpr int BRACKET_EXIT_NEUTRAL =  0;  // BE_HIT
static constexpr int BRACKET_EXIT_LOSS    = -1;  // SL_HIT, BREAKOUT_FAIL, FORCE_CLOSE

struct BracketExitRecord {
    bool    is_long;
    int     outcome; // BRACKET_EXIT_WIN / NEUTRAL / LOSS -- see constants above
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
    //
    // outcome encoding (see BRACKET_EXIT_WIN/NEUTRAL/LOSS above):
    //   +1 real win (TP/TRAIL)  -- breaks loss streak, extends win streak
    //    0 neutral (BE)         -- breaks win streak, SKIPPED by loss scanners
    //                              (does NOT reset loss count; does NOT count as loss)
    //   -1 loss (SL/BF/FORCE)   -- breaks win streak, extends loss streak
    void on_exit(bool is_long, int outcome, int64_t now_ms) {
        exits.push_back({is_long, outcome, now_ms});
        // Prune old exits outside the trend window
        while (!exits.empty() && (now_ms - exits.front().ts_ms) > BRACKET_TREND_WINDOW_MS)
            exits.pop_front();

        // ?? Win bias: consecutive real-win same-direction exits ?????????????????
        // 2 consecutive WINS in same dir ? set trend bias (block counter-direction).
        // BE is NEUTRAL and breaks the win streak (symmetric with loss scanner
        // treating BE as non-loss). A TP ? BE ? TP sequence is NOT a 3-win streak.
        int consec_long = 0, consec_short = 0;
        for (auto it = exits.rbegin(); it != exits.rend(); ++it) {
            if (it->outcome != BRACKET_EXIT_WIN) break;
            if (it->is_long)  { if (consec_short > 0) break; ++consec_long;  }
            else              { if (consec_long  > 0) break; ++consec_short; }
        }

        // ?? Rejection bias: consecutive losses in same direction ??????????????
        // If the bracket keeps entering LONG and keeps losing, the market is
        // rejecting that direction. Block LONGs, favour SHORTs.
        // Triggered by SL_HIT / BREAKOUT_FAIL / FORCE_CLOSE. BE is NEUTRAL --
        // the scanner SKIPS BE exits (does not count them, does not break on them).
        // A win terminates the streak. Fixes Apr 21 2026 BE-peppered sequence:
        // 13:08 SHORT BE, 13:39 LONG BE, 17:05 SHORT BE, 19:42 SHORT BE --
        // previously each BE reset the scanner; now they are ignored so real
        // losses before and after can still accumulate into rejection bias.
        // Also fixes: gold 13:03-13:41 (4 LONG SLs/BFs) and EUR 15:07-15:25 (3 LONG SLs)
        int loss_long = 0, loss_short = 0;
        for (auto it = exits.rbegin(); it != exits.rend(); ++it) {
            if (it->outcome == BRACKET_EXIT_WIN) break;       // real win ends streak
            if (it->outcome == BRACKET_EXIT_NEUTRAL) continue; // BE is neutral -- skip
            // outcome == LOSS
            if (it->is_long)  { if (loss_short > 0) break; ++loss_long;  }
            else              { if (loss_long  > 0) break; ++loss_short; }
        }
        // ?? 30-min window loss counter -- independent of consecutive streak ????
        // Counts losses per direction within 30 minutes regardless of opposite-dir
        // trades in between. Fixes: 11:27 LONG -$10, 11:37 SHORT (resets consec),
        // 11:49 LONG -$18 -- the two LONG losses 22min apart still trip the block.
        // BE exits are ignored (neutral -- do not inflate loss count).
        const int64_t WINDOW_30MIN_MS = 1800000LL;
        int window_loss_long = 0, window_loss_short = 0;
        for (auto it = exits.rbegin(); it != exits.rend(); ++it) {
            if ((now_ms - it->ts_ms) > WINDOW_30MIN_MS) break;
            if (it->outcome == BRACKET_EXIT_LOSS) {
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
            const char* reason_str = (outcome == BRACKET_EXIT_WIN)     ? "win"
                                   : (outcome == BRACKET_EXIT_NEUTRAL) ? "be"
                                   :                                      "loss";
            printf("[BRACKET-TREND] symbol bias=%d consec_l=%d consec_s=%d loss_l=%d loss_s=%d reason=%s\n",
                   bias, consec_long, consec_short, loss_long, loss_short, reason_str);
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

// Per-symbol bracket-trend state map.
// `static` at namespace scope => each translation unit that includes this
// header gets its own private instance. In main Omega this is populated by
// trade_lifecycle.hpp's on_exit call path (also main-TU-only). In
// OmegaBacktest no caller populates it, so it stays empty and the accessor
// below always returns 0 -- the correct historical-simulation behaviour.
static std::unordered_map<std::string, BracketTrendState> g_bracket_trend;

// ?? Read-only bias accessor for non-bracket entry engines ????????????????????
// Returns the currently-active bracket-trend bias for `sym`:
//   +1  long_bias active (market rejecting SHORT entries)  ? engines should block SHORT
//   -1  short_bias active (market rejecting LONG entries)  ? engines should block LONG
//    0  no active bias (no symbol, or block_until_ms elapsed)
//
// Non-mutating: does NOT call update_l2(), does NOT alter state. Exclusively
// readable state is bias + block_until_ms, already set by the bracket exit path
// in trade_lifecycle.hpp via BracketTrendState::on_exit().
//
// Consumers (per Session 5 handoff NEXT SESSION priority):
//   VWAPStretchReversion -- mean-reversion: block LONG on bias=-1, SHORT on bias=+1
//   TurtleTick           -- breakout-cont : block LONG on bias=-1, SHORT on bias=+1
//   DomPersist           -- trend-cont    : block LONG on bias=-1, SHORT on bias=+1
//   EMACross             -- trend-follow  : block LONG on bias=-1, SHORT on bias=+1
//   SessionMomentum / MacroCrash / CompBreakout: TBD (read source first)
//
// All consumers use the SAME rule: block entries IN the rejected direction.
// Mean-reversion engines naturally fade INTO the rejected direction, so they
// require identical gating. Different semantic framing, same gate.
inline int bracket_trend_bias(const char* sym) noexcept {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto it = g_bracket_trend.find(sym);
    if (it == g_bracket_trend.end()) return 0;
    if (now_ms >= it->second.block_until_ms) return 0;  // stale/expired
    return it->second.bias;
}
