#pragma once
// =============================================================================
// IndexHybridBracketEngine.hpp
//
// HYBRID BRACKET ARCHITECTURE for equity indices (SP, NQ, DJ30, NAS100)
//
// Same design as GoldHybridBracketEngine but parameterised for indices.
// Arms BOTH a long stop above range high AND a short stop below range low.
// First fill becomes the position. Other order immediately cancelled.
//
// REGIME GATE:
//   Blocks new arming when IndexFlowEngine is_trending() for that symbol.
//   PENDING orders already placed are NOT cancelled -- safe to leave resting.
//   Pyramid bypass: can arm when flow be_locked + trail_stage >= 1.
//
// PER-SYMBOL CALIBRATION (2026 price levels, validated against Apr 2 incident):
//   US500.F:  $50/pt,  MIN_RANGE=8pt,  MAX_RANGE=40pt,  SL=0.5*range+0.3pt
//   USTEC.F:  $20/pt,  MIN_RANGE=28pt, MAX_RANGE=140pt, SL=0.5*range+1.0pt
//   DJ30.F:   $5/pt,   MIN_RANGE=60pt, MAX_RANGE=300pt, SL=0.5*range+3.0pt
//   NAS100:   $1/pt,   MIN_RANGE=28pt, MAX_RANGE=140pt, SL=0.5*range+1.0pt
//
// SL FORMULA: entry +/- (range * 0.5 + sl_buffer)
//   This gives RR~4 with TP = sl_dist * 2.0 beyond entry.
//   Much better than SL=full_range which gives RR~1.
//
// On April 2 SP500 context:
//   SP dropped ~250pt in one session. Pre-crash range was ~25-35pt.
//   Hybrid bracket SHORT filling below 25pt range in a 30pt compression
//   -> rides 250pt crash at $50/pt x 0.01 lots = $125 per pt = potential $31,250
//   At risk-controlled 0.02 lots with $30 risk: ~$250 realistic capture
//
// S23 2026-04-25 PORT of gold fix-set to IndexHybrid:
//   Ported from S19 a076e314 (BracketEngine.hpp:372): BE trigger 40% -> 60%
//     of tp_dist. 28-day gold audit showed 41% of trades exited BE_HIT at
//     avg MFE 7.44pt (40% of tp_dist) then reverted. Widening to 60% lets
//     more trades progress to TRAIL_HIT. Now configurable via
//     IndexHybridConfig::be_trigger_frac (default 0.60).
//   Ported from S20 70bc25b6 (GoldHybridBracketEngine.hpp): trail-arm guards.
//     Position must have MFE >= min_trail_arm_pts AND held >= min_trail_arm_secs
//     before BE lock or any subsequent trail move. Prevents sub-pt MFE from
//     arming the trail on tick 2 and getting hit by bid-ask noise. Per-symbol
//     tunable -- indices need different thresholds than gold's $1.5/15s.
//
// S52 2026-05-01 PER-SYMBOL NY-NOISE WINDOW:
//   The NY-open noise gate (originally a hard-coded 13:15-13:45 UTC window
//   inside on_tick()) is now per-symbol via cfg_.ny_noise_{start,end}_min.
//   SP/NQ/DJ keep the 13:15-13:45 default. NAS100 widens to 13:30-14:30 to
//   suppress a chronic 14:00-14:30 chop band documented by:
//     - Stage 1A audit (2026-04-28): 11 of 13 NAS100 IFLow/HBI direction
//       flips fell in the 14:00-15:00 UTC window.
//     - Trade-quality audit (2026-05-01): 6 NAS100 HBI SL hits between
//       13:45-14:46 UTC totalling ~-$160 net in a single afternoon, with
//       price walking 27075-27218 (a 143pt range that defeated the existing
//       30pt SAME_LEVEL_BLOCK_PTS guard).
//   Allowing 13:15-13:30 on NAS100 (which was previously blocked) preserves
//   legitimate NY-open momentum entries; blocking 13:45-14:30 (which was
//   previously allowed) is the actual remediation.
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include "OmegaTradeLedger.hpp"

namespace omega {
namespace idx {

struct IndexHybridConfig {
    const char* symbol        = "???";
    double usd_per_pt         = 50.0;
    double min_range          = 8.0;
    double max_range          = 40.0;
    double max_spread         = 0.5;
    double risk_dollars       = 25.0;
    double risk_pyramid       = 8.0;
    double sl_frac            = 0.5;    // SL = range * sl_frac + sl_buffer
    double sl_buffer          = 0.3;    // pts beyond midpoint
    double tp_rr              = 2.0;    // TP = sl_dist * tp_rr
    double trail_frac         = 0.25;
    double lot_step           = 0.01;
    double lot_min            = 0.01;
    double lot_max            = 1.0;
    int    cooldown_s         = 60;   // reduced 120->60: rearm faster after wins
    int    dir_sl_cooldown_s  = 60;
    int    pending_timeout_s  = 300;
    int    min_hold_s         = 15;
    int    structure_lookback = 180;   // raised 30->180: 30 ticks=3s at index speed=pure noise.
                                       // 180 ticks=18s of real structure. NAS100 14:35 loss:
                                       // 30-tick 'compression' captured 63pt adverse NY open move.
    int    min_break_ticks    = 3;
    int    min_entry_ticks    = 150;
    // S23 2026-04-25: BE trigger fraction of tp_dist.
    //   Ported from gold S19 a076e314 (widened 0.40 -> 0.60).
    //   Before a winner can trigger TRAIL_HIT vs BE_HIT it must move at least
    //   be_trigger_frac * tp_dist. 0.40 was too tight -- 41% of gold trades
    //   scratched at BE after reaching 40% of TP then reversing. 0.60 gives
    //   more room for the move to mature before BE lock engages.
    //   Default 0.60 applies to all index symbols unless overridden.
    double be_trigger_frac    = 0.60;
    // S23 2026-04-25: trail-arm guards.
    //   Ported from gold S20 70bc25b6 (GoldHybridBracketEngine). The BE lock
    //   and any subsequent trail moves are gated by BOTH:
    //     - MFE >= min_trail_arm_pts
    //     - (now - entry) >= min_trail_arm_secs
    //   Prevents sub-pt MFE on tick 2 from arming the trail and being hit
    //   by bid-ask noise immediately. Set 0 to disable either guard.
    //   Per-symbol tuning in make_*_config() factories below.
    double min_trail_arm_pts  = 0.0;  // default disabled -- each factory overrides
    int    min_trail_arm_secs = 0;    // default disabled -- each factory overrides
    // S52 2026-05-01: per-symbol NY-open noise window (UTC minutes-of-day).
    //   Originally hard-coded 13:15-13:45 in on_tick(). Made per-symbol so
    //   NAS100 can use a wider 13:30-14:30 window without affecting SP/NQ/DJ.
    //   Per Stage 1A.1 Spec (i.C) + trade-quality audit 2026-05-01.
    //   Window is half-open [start, end): `mins >= start && mins < end`.
    //   To disable the gate entirely on a symbol, set start == end.
    int    ny_noise_start_min = 13 * 60 + 15;  // 13:15 UTC default
    int    ny_noise_end_min   = 13 * 60 + 45;  // 13:45 UTC default
    // S53 2026-05-01 (SESSION_h trade-quality): post-win same-level block.
    //   Mirrors the existing post-SL SAME_LEVEL_BLOCK at line 391-398 but
    //   stamps on TRAIL_HIT and TP_HIT (not BE_HIT). Default 10 min.
    //   Today's NAS100 tape showed 4 same-direction post-win re-arms within
    //   ±5pt of a recent winner exit; all 4 ended in loss/BE for ~-$78.
    //   The post-SL block did not catch them (those exits weren't SL_HIT).
    //   The 30pt block radius (SAME_LEVEL_BLOCK_PTS) is shared with the
    //   loss-side guard. Set to 0 to disable per-symbol.
    int    same_level_post_win_block_s = 600;  // 10 min
};

inline IndexHybridConfig make_sp_config() {
    IndexHybridConfig c;
    c.symbol       = "US500.F";
    c.usd_per_pt   = 50.0;
    c.min_range    = 8.0;    // ~0.13% of SP ~6200
    c.max_range    = 40.0;   // ~0.65% -- larger catches pre-crash compressions
    c.max_spread   = 0.5;
    c.risk_dollars = 25.0;
    c.risk_pyramid = 8.0;
    c.sl_frac      = 0.5;
    c.sl_buffer    = 0.3;    // pts
    c.tp_rr        = 2.0;
    c.lot_step     = 0.01;
    c.lot_min      = 0.01;
    c.lot_max      = 0.50;
    // S23 trail-arm guards -- SP tick size $0.25, typical 1sec tick count 4-8.
    //   3pt MFE = real move above spread noise. 15s hold = past micro-noise window.
    c.min_trail_arm_pts  = 3.0;
    c.min_trail_arm_secs = 15;
    return c;
}

inline IndexHybridConfig make_nq_config() {
    IndexHybridConfig c;
    c.symbol       = "USTEC.F";
    c.usd_per_pt   = 20.0;
    c.min_range    = 28.0;   // ~0.12% of NQ ~23000
    c.max_range    = 140.0;  // ~0.60%
    c.max_spread   = 2.0;
    c.risk_dollars = 25.0;
    c.risk_pyramid = 8.0;
    c.sl_frac      = 0.5;
    c.sl_buffer    = 1.0;
    c.tp_rr        = 2.0;
    c.lot_step     = 0.01;
    c.lot_min      = 0.01;
    c.lot_max      = 0.50;
    // S23 trail-arm guards -- NQ tick size $0.25 but ticks move $1-5 rapidly.
    //   8pt MFE = ~3x SP equivalent in real move terms. Same 15s hold.
    c.min_trail_arm_pts  = 8.0;
    c.min_trail_arm_secs = 15;
    return c;
}

inline IndexHybridConfig make_us30_config() {
    IndexHybridConfig c;
    c.symbol       = "DJ30.F";
    c.usd_per_pt   = 5.0;
    c.min_range    = 60.0;   // ~0.13% of DJ30 ~45000
    c.max_range    = 300.0;  // ~0.67%
    c.max_spread   = 5.0;
    c.risk_dollars = 25.0;
    c.risk_pyramid = 8.0;
    c.sl_frac      = 0.5;
    c.sl_buffer    = 3.0;
    c.tp_rr        = 2.0;
    c.lot_step     = 0.01;
    c.lot_min      = 0.01;
    c.lot_max      = 1.0;
    // S23 trail-arm guards -- DJ30 tick size $1.0, biggest point moves of the indices.
    //   15pt MFE = spread_cost_noise + small genuine move. Same 15s hold.
    c.min_trail_arm_pts  = 15.0;
    c.min_trail_arm_secs = 15;
    return c;
}

inline IndexHybridConfig make_nas100_config() {
    IndexHybridConfig c;
    c.symbol       = "NAS100";
    c.usd_per_pt   = 1.0;
    c.min_range    = 28.0;   // reverted 20->28: 20pt too tight, captures noise (19:50 false breakout -$25)
    c.max_range    = 140.0;
    c.max_spread   = 2.0;
    c.risk_dollars = 25.0;
    c.risk_pyramid = 8.0;
    c.sl_frac      = 0.5;
    c.sl_buffer    = 1.0;
    c.tp_rr        = 2.0;
    c.lot_step     = 0.10;
    c.lot_min      = 0.10;
    c.lot_max      = 5.0;
    // S23 NAS100-specific tuning per Jo 2026-04-25: "NAS needs different settings".
    //   NAS100 cash at $1/pt has 20x less dollar-value-per-pt than SP but same
    //   pt-scale volatility. Lot minimum is 10x larger (0.10 vs 0.01) which
    //   amplifies small adverse moves in dollar terms.
    //   - be_trigger_frac 0.60 -> 0.70: require stronger proof of winner before
    //     BE lock given NAS100's historical false-breakout rate (0% WR pre-S23
    //     structure_lookback widening; 19:50 false breakout -$25 reference).
    //   - min_trail_arm_pts 12 (vs NQ's 8): NAS cash bid-ask noise is wider
    //     than NQ futures. Requires ~50% more MFE to prove a move is genuine.
    //   - min_trail_arm_secs 20 (vs 15 elsewhere): NAS cash sees spike-revert
    //     patterns over 10-15s. 20s hold bypasses that window.
    c.be_trigger_frac    = 0.70;
    c.min_trail_arm_pts  = 12.0;
    c.min_trail_arm_secs = 20;
    // S52 2026-05-01: extend NY-noise window for NAS100 only.
    //   Default 13:15-13:45 -> 13:30-14:30. Suppresses the chronic 14:00-14:30
    //   chop band documented in Stage 1A audit (11/13 historical IFLow/HBI
    //   flips) and reproduced live on 2026-05-01 (6 SL hits between 13:45 and
    //   14:46 totalling ~-$160 net -- price walked 27075-27218, a 143pt range
    //   that defeated the existing 30pt SAME_LEVEL_BLOCK_PTS guard).
    //   Allowing 13:15-13:30 (previously blocked) preserves legitimate
    //   NY-open momentum entries on NAS100; blocking 13:45-14:30 (previously
    //   allowed) is the actual remediation. Other index symbols unaffected.
    c.ny_noise_start_min = 13 * 60 + 30;  // 13:30 UTC
    c.ny_noise_end_min   = 14 * 60 + 30;  // 14:30 UTC
    return c;
}

class IndexHybridBracketEngine {
public:
    explicit IndexHybridBracketEngine(IndexHybridConfig cfg) : cfg_(cfg) {}

    bool shadow_mode = true;

    enum class Phase : uint8_t { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase  phase        = Phase::IDLE;
    double bracket_high = 0.0;
    double bracket_low  = 0.0;
    double range        = 0.0;
    double pending_lot  = 0.0;

    struct OpenPos {
        bool    active          = false;
        bool    is_long         = false;
        bool    be_locked       = false;
        double  entry           = 0.0;
        double  tp              = 0.0;
        double  sl              = 0.0;
        double  size            = 0.0;
        double  mfe             = 0.0;
        // S51 1A.1.a 2026-04-28: max adverse excursion (price points, <= 0).
        //   Pre-S51 every HBI trade had tr.mae=0 because the writer was
        //   hardcoded to tr.mae = 0.0. Mirrors HBG S43 a076e314 pattern.
        //   Tracked by manage() and written at close as tr.mae = pos.mae * pos.size.
        double  mae             = 0.0;
        // S51 1A.1.a 2026-04-28: full bid-ask spread at fill time (price units).
        //   Pre-S51 tr.spreadAtEntry was hardcoded to 0.0 -> apply_realistic_costs()
        //   computed slippage_entry = slippage_exit = 0 for every HBI trade,
        //   silently treating live and shadow trades as zero-spread fills.
        //   Captured in confirm_fill() at the moment of fill.
        double  spread_at_entry = 0.0;
        int64_t entry_ts        = 0;
    } pos;

    std::string pending_long_clOrdId;
    std::string pending_short_clOrdId;

    bool has_open_position() const noexcept {
        return phase == Phase::PENDING || phase == Phase::LIVE;
    }

    using CloseCallback  = std::function<void(const omega::TradeRecord&)>;
    using CancelCallback = std::function<void(const std::string&)>;
    CancelCallback cancel_fn;

    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 bool flow_live,
                 bool flow_be_locked,
                 int  flow_trail_stage,
                 CloseCallback on_close) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= cfg_.cooldown_s) phase = Phase::IDLE;
            else return;
        }

        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, now_s, on_close);
            return;
        }

        if (phase == Phase::PENDING) {
            if (!can_enter) {
                // [BUG-5 FIX] Grace period before cancelling PENDING orders.
                // Brief spread spikes or momentary gate flips must not cancel valid
                // resting stop orders at the broker (same fix as GoldHybridBracketEngine).
                if (m_pending_blocked_since == 0) m_pending_blocked_since = now_s;
                if ((now_s - m_pending_blocked_since) >= 15) {
                    std::cout << "[HYBRID-" << cfg_.symbol << "] PENDING CANCEL blocked="
                              << (long long)(now_s - m_pending_blocked_since) << "s -- cancelling orders\n";
                    std::cout.flush();
                    cancel_both(); reset_to_idle();
                }
                return;
            } else { m_pending_blocked_since = 0; }
            if ((now_s - m_armed_ts) > cfg_.pending_timeout_s) {
                std::cout << "[HYBRID-" << cfg_.symbol << "] PENDING TIMEOUT\n";
                std::cout.flush();
                cancel_both(); reset_to_idle(); return;
            }
            if (shadow_mode) {
                // S51 1A.1.a: pass real spread to populate pos.spread_at_entry.
                // 2026-05-03: pass now_ms so entry_ts tracks the tick-stream
                // timestamp, not wall-clock. Required for backtest replay
                // where tick-time != wall-clock; otherwise held_s goes
                // negative and trail-arm guards never satisfy.
                if (ask >= bracket_high) { confirm_fill(true,  bracket_high, pending_lot, spread, now_ms); return; }
                if (bid <= bracket_low)  { confirm_fill(false, bracket_low,  pending_lot, spread, now_ms); return; }
            }
            return;
        }

        m_window.push_back(mid);
        ++m_ticks_received;
        if ((int)m_window.size() > cfg_.structure_lookback * 2) m_window.pop_front();

        // Heartbeat: every 300 ticks log state to tee'd log file
        if (m_ticks_received > 0 && m_ticks_received % 300 == 0) {
            std::cout << "[HYBRID-" << cfg_.symbol << "-DIAG] ticks=" << m_ticks_received
                      << " phase=" << static_cast<int>(phase)
                      << " window=" << (int)m_window.size() << "/" << cfg_.structure_lookback
                      << " range=" << std::fixed << std::setprecision(2) << range
                      << " can_enter=" << (int)can_enter << "\n";
            std::cout.flush();
        }
        if (m_ticks_received < cfg_.min_entry_ticks) return;
        if ((int)m_window.size() < cfg_.structure_lookback) return;
        if (!can_enter) {
            if (phase == Phase::ARMED) return;
            return;
        }
        if (spread > cfg_.max_spread) return;

        // NY open noise gate -- per-symbol via cfg_.ny_noise_{start,end}_min.
        //   SP/NQ/DJ default to 13:15-13:45 UTC. NAS100 widens to 13:30-14:30
        //   per S52 2026-05-01 trade-quality fix; see make_nas100_config().
        // Root cause history: 30-tick bracket at NY open captured the full
        //   volatile opening range as 'compression'. Examples:
        //     2026-04-25 14:35 NAS100 LONG: 63pt adverse move = -$19.04 net
        //     2026-04-25 13:37 NAS100 SHORT: 43pt adverse move = -$21.68 net
        //     2026-05-01 13:45-14:46 NAS100: 6 SL hits in chop = -~$160 net
        // ARMED/PENDING/LIVE phases pass through -- only new arming blocked.
        // Symbols can disable the gate by setting start == end in their factory.
        {
            const int64_t t_s = now_ms / 1000;
            struct tm ti{}; const time_t ts = static_cast<time_t>(t_s);
#ifdef _WIN32
            gmtime_s(&ti, &ts);
#else
            gmtime_r(&ts, &ti);
#endif
            const int mins = ti.tm_hour * 60 + ti.tm_min;
            const bool ny_open_noise = (cfg_.ny_noise_start_min < cfg_.ny_noise_end_min) &&
                                       (mins >= cfg_.ny_noise_start_min) &&
                                       (mins <  cfg_.ny_noise_end_min);
            if (ny_open_noise && phase == Phase::IDLE) return;
        }

        const bool flow_pyramid_ok = flow_live && flow_be_locked && flow_trail_stage >= 1;
        if (flow_live && !flow_pyramid_ok && phase == Phase::IDLE) return;

        double w_hi = *std::max_element(m_window.begin(), m_window.end());
        double w_lo = *std::min_element(m_window.begin(), m_window.end());
        range = w_hi - w_lo;

        if (phase == Phase::IDLE) {
            // Same-level re-arm block: after SL hit, don't re-arm within
            // 30pts of SL price for 30 minutes.
            // Prevents hammering same rejected level (12:41/13:00/13:05 NAS100
            // all at 24874 = 3 consecutive SL hits at identical bracket).
            static constexpr double  SAME_LEVEL_BLOCK_PTS = 30.0;
            static constexpr int64_t SAME_LEVEL_BLOCK_S   = 900;  // reduced 1800->900: 15min same-level block
            if (m_sl_price > 0.0 && (now_s < m_sl_cooldown_ts + SAME_LEVEL_BLOCK_S)) {
                if (std::fabs(w_hi - m_sl_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_sl_price) < SAME_LEVEL_BLOCK_PTS) {
                    return;
                }
            }
            // S53 2026-05-01 (SESSION_h trade-quality): post-win same-level block.
            //   Same 30pt radius as the loss-side block above. Window length
            //   from cfg_.same_level_post_win_block_s (default 600s = 10min).
            //   Stamped on TRAIL_HIT only (BE_HIT and TP_HIT not emitted by
            //   this engine for non-trail-ratchet exits). Continuation: once
            //   price has moved >30pt away from the prior win exit, re-arm
            //   is allowed -- the engine NATURALLY captures trend continuation
            //   on whatever fresh structure forms in the new price area.
            //   Today's NAS100 root cause: 4 same-direction post-win re-arms
            //   within ±5pt of recent winner exit, all 4 ended in loss/BE.
            if (m_win_exit_price > 0.0 && now_s < m_win_exit_block_ts) {
                if (std::fabs(w_hi - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS) {
                    return;
                }
            }
            if (range >= cfg_.min_range && range <= cfg_.max_range) {
                phase        = Phase::ARMED;
                bracket_high = w_hi;
                bracket_low  = w_lo;
                m_armed_ts   = now_s;
                m_inside_ticks = 0;
                std::cout << "[HYBRID-" << cfg_.symbol << "] ARMED hi="
                          << std::fixed << std::setprecision(2) << bracket_high
                          << " lo=" << bracket_low << " range=" << range << "\n";
                std::cout.flush();
            }
            return;
        }

        if (phase == Phase::ARMED) {
            if (mid < bracket_high && mid > bracket_low) {
                ++m_inside_ticks;
                bracket_high = std::min(bracket_high, w_hi);
                bracket_low  = std::max(bracket_low,  w_lo);
                range        = bracket_high - bracket_low;
            } else {
                m_inside_ticks = 0;
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
                return;
            }

            if (m_inside_ticks < cfg_.min_break_ticks) return;
            if (range < cfg_.min_range || range > cfg_.max_range) {
                phase = Phase::IDLE; return;
            }

            const double risk    = flow_pyramid_ok ? cfg_.risk_pyramid : cfg_.risk_dollars;
            const double sl_dist = range * cfg_.sl_frac + cfg_.sl_buffer;
            const double lot_raw = risk / (sl_dist * cfg_.usd_per_pt);
            const double lot     = std::max(cfg_.lot_min,
                                   std::min(cfg_.lot_max,
                                   std::floor(lot_raw / cfg_.lot_step) * cfg_.lot_step));

            const double tp_dist = sl_dist * cfg_.tp_rr;
            const double min_tp  = spread * 3.0 + (cfg_.usd_per_pt >= 20.0 ? 0.50 : 0.10);
            if (tp_dist < min_tp) {
                std::cout << "[HYBRID-" << cfg_.symbol << "] COST_FAIL range="
                          << std::fixed << std::setprecision(2) << range
                          << " sl_dist=" << sl_dist << "\n";
                std::cout.flush();
                phase = Phase::IDLE;
                return;
            }

            pending_lot  = lot;
            phase        = Phase::PENDING;
            m_armed_ts   = now_s;

            std::cout << "[HYBRID-" << cfg_.symbol << "] FIRE hi="
                      << std::fixed << std::setprecision(2) << bracket_high
                      << " lo=" << bracket_low << " range=" << range
                      << " sl_dist=" << sl_dist
                      << " lot=" << std::setprecision(3) << lot
                      << " " << (flow_pyramid_ok ? "[PYRAMID]" : "") << "\n";
            std::cout.flush();
            fflush(stdout);
        }
    }

    // S51 1A.1.a 2026-04-28: spread_at_fill default-arg added so existing external
    //   callers (e.g. FIX execution-report handler) compile unchanged. Internal
    //   shadow-mode call sites in on_tick() now pass (ask - bid) so the value
    //   reaches the closing TradeRecord via pos.spread_at_entry.
    void confirm_fill(bool is_long, double fill_px, double fill_lot,
                      double spread_at_fill = 0.0,
                      int64_t now_ms_at_fill = 0) noexcept {
        if (phase != Phase::PENDING) return;
        cancel_losing_side(is_long);

        const double sl_dist = range * cfg_.sl_frac + cfg_.sl_buffer;
        const double tp_dist = sl_dist * cfg_.tp_rr;

        pos.active          = true;
        pos.is_long         = is_long;
        pos.entry           = fill_px;
        pos.sl              = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp              = is_long ? (fill_px + tp_dist)  : (fill_px - tp_dist);
        pos.size            = fill_lot;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;   // S51: reset adverse-excursion tracker
        pos.spread_at_entry = spread_at_fill;  // S51: stash for tr.spreadAtEntry at close
        pos.be_locked       = false;
        // 2026-05-03: prefer now_ms_at_fill (tick-stream timestamp) over
        // std::time(nullptr) (wall-clock). Required for backtest replay
        // where tick-time != wall-clock. Production passes now_ms from the
        // tick handler; legacy callers that don't pass it (default 0) fall
        // back to wall-clock.
        pos.entry_ts        = (now_ms_at_fill > 0)
            ? (now_ms_at_fill / 1000)
            : static_cast<int64_t>(std::time(nullptr));
        phase               = Phase::LIVE;

        std::cout << "[HYBRID-" << cfg_.symbol << "] FILL "
                  << (is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << fill_px
                  << " sl=" << pos.sl << " tp=" << pos.tp
                  << " lot=" << std::setprecision(3) << fill_lot << "\n";
        std::cout.flush();
        fflush(stdout);
    }

private:
    IndexHybridConfig  cfg_;
    std::deque<double> m_window;
    int     m_ticks_received = 0;
    int     m_inside_ticks   = 0;
    int64_t m_armed_ts       = 0;
    int64_t m_cooldown_start        = 0;
    int64_t m_pending_blocked_since = 0;  // [BUG-5 FIX] grace timer for PENDING cancel
    int     m_trade_id       = 0;
    int     m_sl_cooldown_dir = 0;
    int64_t m_sl_cooldown_ts  = 0;
    double  m_sl_price        = 0.0;  // entry price at last SL hit -- same-level re-arm block
    // S53 2026-05-01 (SESSION_h trade-quality): post-win same-level block.
    //   Stamped on TRAIL_HIT or TP_HIT (not BE_HIT).
    //   Block radius = SAME_LEVEL_BLOCK_PTS (shared with loss-side).
    //   Block duration = cfg_.same_level_post_win_block_s (default 600s).
    double  m_win_exit_price        = 0.0;
    int64_t m_win_exit_block_ts     = 0;

    void cancel_losing_side(bool filled_long) noexcept {
        if (filled_long && !pending_short_clOrdId.empty()) {
            if (cancel_fn) cancel_fn(pending_short_clOrdId);
            pending_short_clOrdId.clear();
        } else if (!filled_long && !pending_long_clOrdId.empty()) {
            if (cancel_fn) cancel_fn(pending_long_clOrdId);
            pending_long_clOrdId.clear();
        }
    }

    void cancel_both() noexcept {
        if (cancel_fn) {
            if (!pending_long_clOrdId.empty())  cancel_fn(pending_long_clOrdId);
            if (!pending_short_clOrdId.empty()) cancel_fn(pending_short_clOrdId);
        }
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();
    }

    void reset_to_idle() noexcept {
        phase = Phase::IDLE;
        bracket_high = bracket_low = range = 0.0;
        m_inside_ticks = 0;
        // Bug #5 (KNOWN_BUGS.md, 2026-05-03) -- reset the PENDING-blocked
        // grace timer here so the next ARM cycle starts with a clean window.
        // Without this, a stale m_pending_blocked_since persists across
        // cancel/reset cycles and the next FIRE -> PENDING that briefly sees
        // can_enter=false reads "blocked since the very first tick", which
        // immediately exceeds the 15s grace and cancels the order.
        m_pending_blocked_since = 0;
        pos = OpenPos{};
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();
    }

    void manage(double bid, double ask, double mid,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        // S51 1A.1.a 2026-04-28: track maximum adverse excursion in price-points.
        //   Convention matches HBG (GoldHybridBracketEngine.hpp L433): pos.mae
        //   stays <= 0 and represents the worst against-position move.
        //   `move` is favourable-positive, so adverse moves register as `move < 0`.
        if (move < pos.mae) pos.mae = move;
        if ((now_s - pos.entry_ts) < cfg_.min_hold_s) return;

        const double tp_dist    = std::fabs(pos.tp - pos.entry);
        const double trail_dist = std::max(range * cfg_.trail_frac, (ask - bid) * 2.0);

        // S23 2026-04-25: Trail-arm guards -- ported from gold S20 70bc25b6.
        //   BE lock + every subsequent trail step require BOTH:
        //     - pos.mfe >= cfg_.min_trail_arm_pts
        //     - (now_s - pos.entry_ts) >= cfg_.min_trail_arm_secs
        //   Either threshold set to 0 disables that half of the gate.
        //   Prevents sub-pt MFE on tick 2 from arming trail and being hit by
        //   bid-ask noise immediately (see gold 2026-04-24 15:44:43 3sec TRAIL).
        //   Hard-SL path (below) is NOT gated -- original SL still fires.
        const int64_t held_s     = now_s - pos.entry_ts;
        const bool    arm_mfe_ok  = (cfg_.min_trail_arm_pts  <= 0.0) || (pos.mfe >= cfg_.min_trail_arm_pts);
        const bool    arm_hold_ok = (cfg_.min_trail_arm_secs <= 0)   || (held_s  >= cfg_.min_trail_arm_secs);
        const bool    trail_arm_ok = arm_mfe_ok && arm_hold_ok;

        // S23 2026-04-25: BE trigger fraction widened from 0.40 to cfg_.be_trigger_frac
        //   (default 0.60, NAS100 override 0.70). Ported from gold S19 a076e314.
        //   Raw gate formula: move >= tp_dist * be_trigger_frac AND !be_locked
        //   AND trail-arm guards satisfied.
        if (trail_arm_ok && move >= tp_dist * cfg_.be_trigger_frac && !pos.be_locked) {
            pos.sl = pos.entry; pos.be_locked = true;
            std::cout << "[HYBRID-" << cfg_.symbol << "] TRAIL-BE "
                      << (pos.is_long ? "LONG" : "SHORT")
                      << " move=" << std::fixed << std::setprecision(2) << move
                      << " frac=" << std::setprecision(2) << cfg_.be_trigger_frac
                      << " mfe=" << std::setprecision(2) << pos.mfe
                      << " held=" << held_s << "s\n";
            std::cout.flush();
        }
        // Subsequent trail steps are gated on be_locked AND trail_arm_ok.
        // Once BE is locked, the arm guards stay enforced to prevent a BE-locked
        // trail from being whipsawed by noise before a decisive move.
        if (trail_arm_ok && pos.be_locked && move >= tp_dist) {
            const double lock = pos.is_long
                ? pos.entry + tp_dist * 0.50 : pos.entry - tp_dist * 0.50;
            if ((pos.is_long && lock > pos.sl) || (!pos.is_long && lock < pos.sl)) pos.sl = lock;
        }
        if (trail_arm_ok && pos.be_locked && move >= tp_dist * 2.0) {
            const double lock = pos.is_long ? pos.entry + tp_dist : pos.entry - tp_dist;
            if ((pos.is_long && lock > pos.sl) || (!pos.is_long && lock < pos.sl)) pos.sl = lock;
        }
        if (trail_arm_ok && pos.be_locked && move >= tp_dist * 2.0) {
            const double trail_sl = pos.is_long
                ? (pos.entry + pos.mfe - trail_dist)
                : (pos.entry - pos.mfe + trail_dist);
            if ((pos.is_long && trail_sl > pos.sl) || (!pos.is_long && trail_sl < pos.sl))
                pos.sl = trail_sl;
        }

        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (!sl_hit) return;

        const double exit_px = pos.is_long ? bid : ask;
        const char* reason   = pos.be_locked
            ? (pos.sl > pos.entry + 0.01 || pos.sl < pos.entry - 0.01
               ? "TRAIL_HIT" : "BE_HIT")
            : "SL_HIT";

        if (std::strcmp(reason, "SL_HIT") == 0) {
            m_sl_cooldown_dir = pos.is_long ? 1 : -1;
            m_sl_cooldown_ts  = now_s + cfg_.dir_sl_cooldown_s;
            m_sl_price        = pos.entry;  // block same-level re-arm after SL
        }
        // S53 2026-05-01 (SESSION_h trade-quality): stamp post-win block.
        //   Engine emits TRAIL_HIT (when SL has ratcheted past entry into
        //   profit) and BE_HIT (when SL is locked at entry). TP_HIT is
        //   never emitted -- the SL ratchets all the way to TP via trail.
        //   So stamping on TRAIL_HIT covers all win exits. BE_HIT carries
        //   no signal so it is NOT stamped. Set
        //   cfg_.same_level_post_win_block_s = 0 to disable per-symbol.
        if (std::strcmp(reason, "TRAIL_HIT") == 0 &&
            cfg_.same_level_post_win_block_s > 0) {
            m_win_exit_price    = exit_px;
            m_win_exit_block_ts = now_s + cfg_.same_level_post_win_block_s;
        }

        omega::TradeRecord tr;
        tr.id         = ++m_trade_id;
        tr.symbol     = cfg_.symbol;
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos.entry;
        tr.exitPrice  = exit_px;
        tr.sl         = pos.sl;
        tr.size       = pos.size;
        tr.pnl        = (pos.is_long ? (exit_px - pos.entry)
                                     : (pos.entry - exit_px)) * pos.size;
        tr.mfe        = pos.mfe * pos.size;
        // S51 1A.1.a 2026-04-28: was hardcoded 0.0; now real MAE.
        tr.mae        = pos.mae * pos.size;
        tr.entryTs    = pos.entry_ts;
        tr.exitTs     = now_s;
        tr.exitReason = reason;
        tr.engine     = "HybridBracketIndex";
        tr.regime     = "HYBRID";
        // S51 1A.1.a 2026-04-28: was hardcoded 0.0; now real spread captured at fill.
        //   Consumed by apply_realistic_costs() for slippage_entry/slippage_exit.
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.shadow     = shadow_mode;

        std::cout << "[HYBRID-" << cfg_.symbol << "] EXIT "
                  << (pos.is_long ? "LONG" : "SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " reason=" << reason
                  << " pnl_usd=" << std::setprecision(2) << (tr.pnl * cfg_.usd_per_pt) << "\n";
        std::cout.flush();
        fflush(stdout);

        reset_to_idle();
        m_cooldown_start = now_s;
        phase = Phase::COOLDOWN;
        if (on_close) on_close(tr);
    }
};

} // namespace idx
} // namespace omega
