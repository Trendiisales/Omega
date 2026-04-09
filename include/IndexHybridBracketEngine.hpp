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
// SL FORMULA: entry ± (range * 0.5 + sl_buffer)
//   This gives RR~4 with TP = sl_dist * 2.0 beyond entry.
//   Much better than SL=full_range which gives RR~1.
//
// On April 2 SP500 context:
//   SP dropped ~250pt in one session. Pre-crash range was ~25-35pt.
//   Hybrid bracket SHORT filling below 25pt range in a 30pt compression
//   → rides 250pt crash at $50/pt × 0.01 lots = $125 per pt = potential $31,250
//   At risk-controlled 0.02 lots with $30 risk: ~$250 realistic capture
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
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
    int    cooldown_s         = 120;
    int    dir_sl_cooldown_s  = 60;
    int    pending_timeout_s  = 300;
    int    min_hold_s         = 15;
    int    structure_lookback = 180;   // raised 30->180: 30 ticks=3s at index speed=pure noise.
                                       // 180 ticks=18s of real structure. NAS100 14:35 loss:
                                       // 30-tick 'compression' captured 63pt adverse NY open move.
    int    min_break_ticks    = 3;
    int    min_entry_ticks    = 150;
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
    return c;
}

inline IndexHybridConfig make_nas100_config() {
    IndexHybridConfig c;
    c.symbol       = "NAS100";
    c.usd_per_pt   = 1.0;
    c.min_range    = 28.0;
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
        bool    active    = false;
        bool    is_long   = false;
        bool    be_locked = false;
        double  entry     = 0.0;
        double  tp        = 0.0;
        double  sl        = 0.0;
        double  size      = 0.0;
        double  mfe       = 0.0;
        int64_t entry_ts  = 0;
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
                    printf("[HYBRID-%s] PENDING CANCEL blocked=%llds -- cancelling orders\n",
                           cfg_.symbol, (long long)(now_s - m_pending_blocked_since));
                    fflush(stdout);
                    cancel_both(); reset_to_idle();
                }
                return;
            } else { m_pending_blocked_since = 0; }
            if ((now_s - m_armed_ts) > cfg_.pending_timeout_s) {
                printf("[HYBRID-%s] PENDING TIMEOUT\n", cfg_.symbol);
                fflush(stdout);
                cancel_both(); reset_to_idle(); return;
            }
            if (shadow_mode) {
                if (ask >= bracket_high) { confirm_fill(true,  bracket_high, pending_lot); return; }
                if (bid <= bracket_low)  { confirm_fill(false, bracket_low,  pending_lot); return; }
            }
            return;
        }

        m_window.push_back(mid);
        ++m_ticks_received;
        if ((int)m_window.size() > cfg_.structure_lookback * 2) m_window.pop_front();

        if (m_ticks_received < cfg_.min_entry_ticks) return;
        if ((int)m_window.size() < cfg_.structure_lookback) return;
        if (!can_enter) {
            if (phase == Phase::ARMED) return;
            return;
        }
        if (spread > cfg_.max_spread) return;

        // NY open noise gate: 13:15-13:45 UTC
        // Root cause of 0% WR on NAS100: 30-tick bracket at NY open captured
        // the full volatile opening range as 'compression'.
        // 14:35 NAS100 LONG: 63pt adverse move = -$19.04 net.
        // 13:37 NAS100 SHORT: 43pt adverse move = -$21.68 net.
        // Fix: hard block arming during NY open noise window.
        // ARMED/PENDING/LIVE phases pass through -- only new arming blocked.
        {
            const int64_t t_s = now_ms / 1000;
            struct tm ti{}; const time_t ts = static_cast<time_t>(t_s);
#ifdef _WIN32
            gmtime_s(&ti, &ts);
#else
            gmtime_r(&ts, &ti);
#endif
            const int mins = ti.tm_hour * 60 + ti.tm_min;
            const bool ny_open_noise = (mins >= 13 * 60 + 15) && (mins < 13 * 60 + 45);
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
            static constexpr int64_t SAME_LEVEL_BLOCK_S   = 1800; // 30 min
            if (m_sl_price > 0.0 && (now_s < m_sl_cooldown_ts + SAME_LEVEL_BLOCK_S)) {
                if (std::fabs(w_hi - m_sl_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_sl_price) < SAME_LEVEL_BLOCK_PTS) {
                    return;
                }
            }
            if (range >= cfg_.min_range && range <= cfg_.max_range) {
                phase        = Phase::ARMED;
                bracket_high = w_hi;
                bracket_low  = w_lo;
                m_armed_ts   = now_s;
                m_inside_ticks = 0;
                printf("[HYBRID-%s] ARMED hi=%.2f lo=%.2f range=%.2f\n",
                       cfg_.symbol, bracket_high, bracket_low, range);
                fflush(stdout);
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
                printf("[HYBRID-%s] COST_FAIL range=%.2f sl_dist=%.2f\n",
                       cfg_.symbol, range, sl_dist);
                fflush(stdout);
                phase = Phase::IDLE;
                return;
            }

            pending_lot  = lot;
            phase        = Phase::PENDING;
            m_armed_ts   = now_s;

            printf("[HYBRID-%s] FIRE hi=%.2f lo=%.2f range=%.2f sl_dist=%.2f lot=%.3f %s\n",
                   cfg_.symbol, bracket_high, bracket_low, range, sl_dist, lot,
                   flow_pyramid_ok ? "[PYRAMID]" : "");
            fflush(stdout);
        }
    }

    void confirm_fill(bool is_long, double fill_px, double fill_lot) noexcept {
        if (phase != Phase::PENDING) return;
        cancel_losing_side(is_long);

        const double sl_dist = range * cfg_.sl_frac + cfg_.sl_buffer;
        const double tp_dist = sl_dist * cfg_.tp_rr;

        pos.active    = true;
        pos.is_long   = is_long;
        pos.entry     = fill_px;
        pos.sl        = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp        = is_long ? (fill_px + tp_dist)  : (fill_px - tp_dist);
        pos.size      = fill_lot;
        pos.mfe       = 0.0;
        pos.be_locked = false;
        pos.entry_ts  = static_cast<int64_t>(std::time(nullptr));
        phase         = Phase::LIVE;

        printf("[HYBRID-%s] FILL %s @ %.2f sl=%.2f tp=%.2f lot=%.3f\n",
               cfg_.symbol, is_long ? "LONG" : "SHORT",
               fill_px, pos.sl, pos.tp, fill_lot);
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
        if ((now_s - pos.entry_ts) < cfg_.min_hold_s) return;

        const double tp_dist    = std::fabs(pos.tp - pos.entry);
        const double trail_dist = std::max(range * cfg_.trail_frac, (ask - bid) * 2.0);

        if (move >= tp_dist * 0.40 && !pos.be_locked) {
            pos.sl = pos.entry; pos.be_locked = true;
            printf("[HYBRID-%s] TRAIL-BE %s move=%.2f\n", cfg_.symbol,
                   pos.is_long ? "LONG" : "SHORT", move);
            fflush(stdout);
        }
        if (pos.be_locked && move >= tp_dist) {
            const double lock = pos.is_long
                ? pos.entry + tp_dist * 0.50 : pos.entry - tp_dist * 0.50;
            if ((pos.is_long && lock > pos.sl) || (!pos.is_long && lock < pos.sl)) pos.sl = lock;
        }
        if (pos.be_locked && move >= tp_dist * 2.0) {
            const double lock = pos.is_long ? pos.entry + tp_dist : pos.entry - tp_dist;
            if ((pos.is_long && lock > pos.sl) || (!pos.is_long && lock < pos.sl)) pos.sl = lock;
        }
        if (pos.be_locked && move >= tp_dist * 2.0) {
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
        tr.mae        = 0.0;
        tr.entryTs    = pos.entry_ts;
        tr.exitTs     = now_s;
        tr.exitReason = reason;
        tr.engine     = "HybridBracketIndex";
        tr.regime     = "HYBRID";
        tr.spreadAtEntry = 0.0;

        printf("[HYBRID-%s] EXIT %s @ %.2f reason=%s pnl_usd=%.2f\n",
               cfg_.symbol, pos.is_long ? "LONG" : "SHORT",
               exit_px, reason, tr.pnl * cfg_.usd_per_pt);
        fflush(stdout);

        reset_to_idle();
        m_cooldown_start = now_s;
        phase = Phase::COOLDOWN;
        if (on_close) on_close(tr);
    }
};

} // namespace idx
} // namespace omega
