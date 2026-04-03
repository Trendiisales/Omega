// =============================================================================
//  OmegaVPIN.hpp
//  Volume-Synchronised Probability of Informed Trading (VPIN)
//  Tick-classified implementation for BlackBull (no real volume data).
//
//  BACKGROUND:
//    VPIN (Easley, López de Prado, O'Hara 2012) measures the fraction of
//    recent order flow that is "informed" (toxic) vs "uninformed" (noise).
//    High VPIN → informed traders dominate → adverse selection risk is high →
//    market makers widen spreads → momentum entries face worse fills and
//    faster mean-reversion. Gate entries when VPIN is elevated.
//
//  TICK-CLASSIFICATION APPROACH:
//    BlackBull does not send actual trade volumes (FIX tag 271 omitted).
//    We classify each tick as buy or sell using the standard Lee-Ready rule:
//      - Price up from last → buy tick (buyer-initiated)
//      - Price down from last → sell tick (seller-initiated)
//      - Unchanged → repeat last classification
//    Each classified tick counts as 1 unit of volume (unit-volume VPIN).
//
//  VPIN COMPUTATION:
//    1. Divide the tick stream into volume buckets of size V_BUCKET ticks.
//    2. Within each bucket: OI = |buy_volume - sell_volume|
//       (order imbalance — proxy for informed flow).
//    3. VPIN = (1/N) Σ OI_k / V_BUCKET over the last N buckets.
//       Default: N=50 buckets of 50 ticks each → 2500 ticks lookback.
//    4. At 10 ticks/s London session = ~4 min lookback.
//
//  TOXIC GATE:
//    toxic() returns true when VPIN >= toxic_threshold (default 0.70).
//    At VPIN=0.70: 70% of recent flow is one-directional informed flow.
//    This is a very elevated reading — typically precedes a sharp directional
//    move or a spread-widening event. Do not enter on the wrong side.
//
//    NOTE: We gate NEW entries only. Existing positions are not affected.
//    The VPIN gate fires at entry time as a pre-condition check.
//
//  DIRECTIONAL VPIN:
//    vpin_long_bias() returns true if VPIN is elevated AND recent flow is
//    buy-heavy — useful as a confirming signal for long entries.
//    vpin_short_bias() is the equivalent for shorts.
//    These are used as optional bonuses, not blockers.
//
//  INTEGRATION (main.cpp):
//    1. Include after OmegaAdaptiveRisk.hpp.
//    2. Declare: static omega::vpin::VPINTracker g_vpin;
//    3. In GoldFlow tick loop, after computing mid price:
//         g_vpin.on_tick(mid, now_ms);
//         // Pre-entry gate (in GoldFlow on_tick entry path):
//         if (g_vpin.toxic()) { /* skip entry */ return; }
//    4. Optional in enter_directional for XAUUSD:
//         if (esym == "XAUUSD" && g_vpin.toxic()) return 0.0;
//
//  THREAD SAFETY:
//    VPINTracker is designed for single-threaded tick processing.
//    If called from multiple threads, wrap with external mutex.
// =============================================================================

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

namespace omega { namespace vpin {

// ---------------------------------------------------------------------------
//  Config
// ---------------------------------------------------------------------------
static constexpr int    VPIN_BUCKET_SIZE    = 50;    // ticks per volume bucket
static constexpr int    VPIN_NUM_BUCKETS    = 50;    // N buckets in lookback
static constexpr double VPIN_TOXIC_THRESH   = 0.70;  // VPIN >= this → toxic
static constexpr double VPIN_WARN_THRESH    = 0.55;  // VPIN >= this → warn
static constexpr double VPIN_BIAS_THRESH    = 0.60;  // directional bias floor

// ---------------------------------------------------------------------------
//  VPINTracker
// ---------------------------------------------------------------------------
class VPINTracker {
public:
    // Configurable thresholds (can override before first tick)
    double toxic_threshold = VPIN_TOXIC_THRESH;
    double warn_threshold  = VPIN_WARN_THRESH;
    double bias_threshold  = VPIN_BIAS_THRESH;

    // -----------------------------------------------------------------------
    //  on_tick — call every tick with the current mid price and timestamp.
    //  Classifies the tick, updates the current bucket, and advances the
    //  bucket when full.
    // -----------------------------------------------------------------------
    void on_tick(double mid, int64_t now_ms) {
        if (mid <= 0.0) return;
        (void)now_ms;

        // Lee-Ready tick classification
        if (mid > last_mid_ && last_mid_ > 0.0) {
            current_buy_vol_  += 1.0;
            last_dir_ = 1;
        } else if (mid < last_mid_ && last_mid_ > 0.0) {
            current_sell_vol_ += 1.0;
            last_dir_ = -1;
        } else {
            // Unchanged — repeat last direction
            if (last_dir_ >= 0) current_buy_vol_  += 1.0;
            else                current_sell_vol_ += 1.0;
        }
        last_mid_ = mid;
        ++current_ticks_;

        // Advance bucket when full
        if (current_ticks_ >= VPIN_BUCKET_SIZE) {
            commit_bucket();
        }

        // Recompute VPIN every tick (cheap — just sum deque)
        update_vpin();
    }

    // -----------------------------------------------------------------------
    //  toxic — returns true if VPIN >= toxic_threshold.
    //  Main entry gate: call before firing any new position.
    // -----------------------------------------------------------------------
    bool toxic() const {
        return vpin_ >= toxic_threshold;
    }

    // -----------------------------------------------------------------------
    //  warn_elevated — returns true if VPIN is elevated but below toxic.
    //  Used for sizing reduction (reduce lot if warn but not toxic).
    // -----------------------------------------------------------------------
    bool warn_elevated() const {
        return vpin_ >= warn_threshold && vpin_ < toxic_threshold;
    }

    // -----------------------------------------------------------------------
    //  vpin_long_bias — elevated VPIN with buy-heavy recent flow.
    //  Confirming signal: if you want to go long AND flow is buy-heavy AND
    //  VPIN is elevated → the informed flow is on your side.
    // -----------------------------------------------------------------------
    bool vpin_long_bias() const {
        return vpin_ >= bias_threshold && recent_buy_fraction_ >= 0.60;
    }

    // -----------------------------------------------------------------------
    //  vpin_short_bias — elevated VPIN with sell-heavy recent flow.
    // -----------------------------------------------------------------------
    bool vpin_short_bias() const {
        return vpin_ >= bias_threshold && recent_buy_fraction_ <= 0.40;
    }

    // -----------------------------------------------------------------------
    //  vpin — current VPIN value [0, 1].
    // -----------------------------------------------------------------------
    double vpin() const { return vpin_; }

    // -----------------------------------------------------------------------
    //  recent_buy_fraction — fraction of buy volume in last N buckets [0, 1].
    // -----------------------------------------------------------------------
    double buy_fraction() const { return recent_buy_fraction_; }

    // -----------------------------------------------------------------------
    //  buckets_filled — how many buckets have been committed. VPIN is only
    //  meaningful after ~10 buckets are filled (cold-start guard).
    // -----------------------------------------------------------------------
    int buckets_filled() const { return static_cast<int>(buckets_.size()); }

    bool warmed() const { return buckets_filled() >= 10; }

    // -----------------------------------------------------------------------
    //  print_status — diagnostic log line.
    // -----------------------------------------------------------------------
    void print_status() const {
        std::printf("[VPIN] vpin=%.3f buy_frac=%.3f buckets=%d %s%s\n",
                    vpin_, recent_buy_fraction_, buckets_filled(),
                    toxic()         ? "[TOXIC] "    : "",
                    warn_elevated() ? "[ELEVATED] " : "");
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    //  reset — clear all state (e.g. at session open).
    // -----------------------------------------------------------------------
    void reset() {
        buckets_.clear();
        current_buy_vol_  = 0.0;
        current_sell_vol_ = 0.0;
        current_ticks_    = 0;
        last_mid_         = 0.0;
        last_dir_         = 0;
        vpin_             = 0.0;
        recent_buy_fraction_ = 0.5;
    }

private:
    struct Bucket {
        double buy_vol  = 0.0;
        double sell_vol = 0.0;
        double oi() const { return std::fabs(buy_vol - sell_vol); }
        double total() const { return buy_vol + sell_vol; }
    };

    std::deque<Bucket> buckets_;

    // Current (in-progress) bucket
    double current_buy_vol_  = 0.0;
    double current_sell_vol_ = 0.0;
    int    current_ticks_    = 0;

    // Tick-by-tick state
    double last_mid_  = 0.0;
    int    last_dir_  = 0;   // +1 = up, -1 = down

    // Computed output
    double vpin_                = 0.0;
    double recent_buy_fraction_ = 0.5;

    void commit_bucket() {
        Bucket b;
        b.buy_vol  = current_buy_vol_;
        b.sell_vol = current_sell_vol_;
        buckets_.push_back(b);

        // Trim to N buckets
        while ((int)buckets_.size() > VPIN_NUM_BUCKETS)
            buckets_.pop_front();

        // Reset current bucket
        current_buy_vol_  = 0.0;
        current_sell_vol_ = 0.0;
        current_ticks_    = 0;
    }

    void update_vpin() {
        if (buckets_.empty()) {
            vpin_ = 0.0;
            recent_buy_fraction_ = 0.5;
            return;
        }
        double sum_oi    = 0.0;
        double sum_total = 0.0;
        double sum_buy   = 0.0;
        for (const auto& b : buckets_) {
            sum_oi    += b.oi();
            sum_total += b.total();
            sum_buy   += b.buy_vol;
        }
        const double denom = static_cast<double>(buckets_.size()) * VPIN_BUCKET_SIZE;
        vpin_ = (denom > 0.0) ? sum_oi / denom : 0.0;
        recent_buy_fraction_ = (sum_total > 0.0) ? sum_buy / sum_total : 0.5;
    }
};

}} // namespace omega::vpin
