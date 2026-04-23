#pragma once
// ==============================================================================
// OmegaRegimeAdaptor.hpp
// Regime-adaptive engine weight system.
//
// MacroRegimeDetector already classifies RISK_ON / RISK_OFF / NEUTRAL.
// Currently this is advisory only -- it logs but does not gate entries.
// This module wires regime into engine PARAMETERS:
//
//   RISK_OFF ? suppress US equity breakout/bracket (equities fall in risk-off)
//              tighten GOLD compression threshold (gold compression ? breakout more reliable)
//              boost Gold stack (gold = risk-off beneficiary)
//              block FX cascade entry (volatile carry unwinds)
//
//   RISK_ON  ? allow all equity engines at full weight
//              tighten Gold stack (gold rarely trends in risk-on)
//              boost FX carry and equity breakout
//
//   NEUTRAL  ? normal weights everywhere
//
// Also tracks the rolling volatility regime per session:
//   HIGH_VOL  ? compress lot sizes, widen spread tolerances
//   LOW_VOL   ? normal sizes, normal spreads
//   CRUSH     ? extreme compression -- ideal for breakout entries
//
// Usage:
//   static omega::regime::RegimeAdaptor g_regime_adaptor;
//   g_regime_adaptor.update(regime_str, vix_level, now_sec);
//   float w = g_regime_adaptor.weight("GOLD_STACK", regime_str);    // 0..2.0
//   bool  b = g_regime_adaptor.blocked("US_EQUITY_BREAKOUT", regime_str);
// ==============================================================================

#include <string>
#include <unordered_map>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <chrono>
#include <algorithm>

namespace omega { namespace regime {

// ?????????????????????????????????????????????????????????????????????????????
// EngineClass -- logical grouping of engines for regime weighting
// ?????????????????????????????????????????????????????????????????????????????
enum class EngineClass {
    US_EQUITY_BREAKOUT,   // SP, NQ, DJ30, NAS100 CRTP breakout
    US_EQUITY_BRACKET,    // SP, NQ, DJ30, NAS100 bracket
    EU_EQUITY_BREAKOUT,   // GER40, UK100, ESTX50 breakout
    EU_EQUITY_BRACKET,    // GER40, UK100, ESTX50 bracket
    GOLD_STACK,           // 5-engine gold stack
    GOLD_BRACKET,         // gold bracket engine
    GOLD_FLOW,            // gold flow engine
    OIL_BREAKOUT,         // USOIL.F / BRENT breakout
    OIL_BRACKET,          // BRENT bracket
    FX_BREAKOUT,          // EURUSD / GBPUSD breakout
    FX_CASCADE,           // FxCascadeEngine
    FX_CARRY,             // CarryUnwindEngine + Asia FX
    CROSS_ASSET,          // EsNqDiv, EIAFade, BrentWTI, ORB, VWAP, TrendPB
    LATENCY_EDGE,         // LatencyEdgeStack
};

// ?????????????????????????????????????????????????????????????????????????????
// RegimeWeightTable
// Per-class weight multiplier for each macro regime.
// Weight 1.0 = normal, 0.5 = half size, 2.0 = double size, 0.0 = blocked.
// ?????????????????????????????????????????????????????????????????????????????
struct RegimeWeightTable {
    // [RISK_ON, NEUTRAL, RISK_OFF]
    // Index: 0=RISK_ON, 1=NEUTRAL, 2=RISK_OFF
    struct Row {
        float risk_on;
        float neutral;
        float risk_off;
    };

    std::unordered_map<int, Row> table;

    RegimeWeightTable() {
        using EC = EngineClass;
        //                                  RISK_ON  NEUTRAL  RISK_OFF
        table[static_cast<int>(EC::US_EQUITY_BREAKOUT)]  = {1.30f,  1.00f,  0.00f}; // block in risk-off
        table[static_cast<int>(EC::US_EQUITY_BRACKET)]   = {1.20f,  1.00f,  0.25f}; // very reduced
        table[static_cast<int>(EC::EU_EQUITY_BREAKOUT)]  = {1.20f,  1.00f,  0.10f}; // nearly blocked
        table[static_cast<int>(EC::EU_EQUITY_BRACKET)]   = {1.10f,  1.00f,  0.20f};
        table[static_cast<int>(EC::GOLD_STACK)]          = {0.80f,  1.00f,  1.50f}; // boost in risk-off
        table[static_cast<int>(EC::GOLD_BRACKET)]        = {0.85f,  1.00f,  1.40f};
        table[static_cast<int>(EC::GOLD_FLOW)]           = {0.80f,  1.00f,  1.50f};
        table[static_cast<int>(EC::OIL_BREAKOUT)]        = {1.10f,  1.00f,  0.70f};
        table[static_cast<int>(EC::OIL_BRACKET)]         = {1.00f,  1.00f,  0.70f};
        table[static_cast<int>(EC::FX_BREAKOUT)]         = {1.00f,  1.00f,  0.50f};
        table[static_cast<int>(EC::FX_CASCADE)]          = {1.00f,  1.00f,  0.00f}; // block carry cascade in risk-off
        table[static_cast<int>(EC::FX_CARRY)]            = {1.20f,  1.00f,  0.00f}; // block carry unwind entry in risk-off
        table[static_cast<int>(EC::CROSS_ASSET)]         = {1.00f,  1.00f,  0.70f};
        table[static_cast<int>(EC::LATENCY_EDGE)]        = {1.00f,  1.00f,  0.80f};
    }

    float get(EngineClass ec, const std::string& regime) const {
        auto it = table.find(static_cast<int>(ec));
        if (it == table.end()) return 1.0f;
        const auto& row = it->second;
        if (regime == "RISK_ON")  return row.risk_on;
        if (regime == "RISK_OFF") return row.risk_off;
        return row.neutral;  // NEUTRAL or any other
    }

    bool blocked(EngineClass ec, const std::string& regime) const {
        return get(ec, regime) < 0.05f;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// SessionVolRegime -- rolling intra-session volatility classification
// Computes true range per N-second bucket, classifies as CRUSH/LOW/NORMAL/HIGH
// ?????????????????????????????????????????????????????????????????????????????
enum class VolRegime { CRUSH, LOW, NORMAL, HIGH };

struct SessionVolRegime {
    static constexpr int BUCKET_SEC  = 300;  // 5-minute buckets
    static constexpr int WINDOW_BUKS = 12;   // 1-hour window
    static constexpr int WARM_BUKS   = 3;    // min buckets before classifying

    struct Bucket { double tr; int64_t ts; };
    std::deque<Bucket> buks_;
    mutable std::mutex mtx_;

    int64_t last_bucket_ts_ = 0;
    double  bucket_high_    = 0, bucket_low_ = 0;
    bool    bucket_init_    = false;

    void update(double mid, int64_t now_sec) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!bucket_init_) {
            bucket_high_ = bucket_low_ = mid;
            last_bucket_ts_ = (now_sec / BUCKET_SEC) * BUCKET_SEC;
            bucket_init_ = true;
            return;
        }
        bucket_high_ = std::max(bucket_high_, mid);
        bucket_low_  = std::min(bucket_low_,  mid);

        const int64_t bucket_ts = (now_sec / BUCKET_SEC) * BUCKET_SEC;
        if (bucket_ts > last_bucket_ts_) {
            const double tr = (bucket_low_ > 0) ? ((bucket_high_ - bucket_low_) / bucket_low_) : 0.0;
            buks_.push_back({tr, last_bucket_ts_});
            if ((int)buks_.size() > WINDOW_BUKS) buks_.pop_front();
            bucket_high_ = bucket_low_ = mid;
            last_bucket_ts_ = bucket_ts;
        }
    }

    VolRegime classify() const {
        std::lock_guard<std::mutex> lk(mtx_);
        if ((int)buks_.size() < WARM_BUKS) return VolRegime::NORMAL;

        double sum = 0;
        for (const auto& b : buks_) sum += b.tr;
        const double mean = sum / buks_.size();

        // Compute current bucket TR vs mean
        const double recent_tr = buks_.back().tr;
        const double ratio = (mean > 1e-10) ? recent_tr / mean : 1.0;

        if (ratio < 0.50) return VolRegime::CRUSH;
        if (ratio < 0.75) return VolRegime::LOW;
        if (ratio > 2.00) return VolRegime::HIGH;
        return VolRegime::NORMAL;
    }

    float size_scale() const {
        switch (classify()) {
            case VolRegime::CRUSH:  return 1.10f;  // slightly larger -- breakout imminent
            case VolRegime::LOW:    return 1.00f;
            case VolRegime::NORMAL: return 1.00f;
            case VolRegime::HIGH:   return 0.75f;  // reduce in high vol
            default:                return 1.00f;
        }
    }

    const char* name() const {
        switch (classify()) {
            case VolRegime::CRUSH:  return "CRUSH";
            case VolRegime::LOW:    return "LOW";
            case VolRegime::NORMAL: return "NORMAL";
            case VolRegime::HIGH:   return "HIGH";
            default:                return "NORMAL";
        }
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// RegimeAdaptor -- unified facade
// ?????????????????????????????????????????????????????????????????????????????
class RegimeAdaptor {
public:
    bool enabled = true;

    RegimeWeightTable weights;

    // Per-symbol session vol regime trackers
    std::unordered_map<std::string, SessionVolRegime> sym_vol;

    // ?? Confirmed macro regime (with dwell-time filter) ??????????????????????
    // Problem: DXY/VIX can oscillate ?threshold repeatedly, causing the regime
    // to flip every few bars. Each flip triggers size adjustments and blocked
    // engines, creating whipsaw costs and inconsistent entry behaviour.
    //
    // Solution: two-state system -- candidate and confirmed.
    //   - candidate_regime: raw output from MacroRegimeDetector (updates every tick)
    //   - last_regime:      confirmed regime, only changes after candidate has been
    //                       stable for at least dwell_bars ticks without reverting.
    //
    // dwell_bars = 20 (default): regime must persist 20 consecutive ticks to confirm.
    // At 1-tick-per-second on FX, this is ~20 seconds -- fast enough to catch real
    // regime shifts, slow enough to ignore noise.
    std::string last_regime      = "NEUTRAL";   // confirmed (used by weight/blocked)
    std::string candidate_regime = "NEUTRAL";   // unconfirmed candidate
    int         candidate_count  = 0;            // consecutive ticks candidate has held
    int         dwell_bars       = 20;           // ticks before candidate confirms
    double      last_vix         = 0.0;
    int64_t     last_ts          = 0;

    // Update macro state -- applies dwell-time filter before changing confirmed regime.
    // Call each time MacroRegimeDetector produces a new regime string.
    void update(const std::string& regime, double vix, int64_t now_sec) {
        last_vix = vix;
        last_ts  = now_sec;

        if (regime == candidate_regime) {
            // Candidate is holding -- increment counter
            if (++candidate_count >= dwell_bars && regime != last_regime) {
                // Candidate has been stable long enough -- confirm the change
                std::printf("[REGIME] Confirmed regime change: %s ? %s  VIX=%.1f  (held %d ticks)\n",
                            last_regime.c_str(), regime.c_str(), vix, candidate_count);
                last_regime = regime;
            }
        } else {
            // New candidate -- reset counter
            if (regime != last_regime) {
                std::printf("[REGIME] Candidate regime: %s ? %s  VIX=%.1f  (need %d ticks to confirm)\n",
                            last_regime.c_str(), regime.c_str(), vix, dwell_bars);
            }
            candidate_regime = regime;
            candidate_count  = 1;
        }
    }

    // Update vol regime for a symbol (call each tick)
    void update_vol(const std::string& sym, double mid, int64_t now_sec) {
        if (!enabled) return;
        sym_vol[sym].update(mid, now_sec);
    }

    // Get weight multiplier for a given engine class under current regime
    float weight(EngineClass ec) const {
        if (!enabled) return 1.0f;
        return weights.get(ec, last_regime);
    }

    // Is this engine class blocked under current regime?
    bool blocked(EngineClass ec) const {
        if (!enabled) return false;
        return weights.blocked(ec, last_regime);
    }

    // Get vol-regime size scale for a symbol
    float vol_size_scale(const std::string& sym) const {
        if (!enabled) return 1.0f;
        auto it = sym_vol.find(sym);
        return (it != sym_vol.end()) ? it->second.size_scale() : 1.0f;
    }

    // Adaptive TP multiplier per vol regime.
    // CRUSH=0.70 -- compressed tape, mean-reversion target, TP must be tight to fill.
    // LOW=0.85   -- quieter than normal, compress slightly.
    // NORMAL=1.00 -- standard TP, no adjustment.
    // HIGH=1.15  -- momentum in play, let the trade run a bit further.
    float tp_vol_mult(const std::string& sym) const {
        if (!enabled) return 1.0f;
        auto it = sym_vol.find(sym);
        if (it == sym_vol.end()) return 1.0f;
        switch (it->second.classify()) {
            case VolRegime::CRUSH:  return 0.70f;
            case VolRegime::LOW:    return 0.85f;
            case VolRegime::NORMAL: return 1.00f;
            case VolRegime::HIGH:   return 1.15f;
            default:                return 1.00f;
        }
    }

    // Combined size multiplier: regime weight ? vol scale
    float combined_size_scale(EngineClass ec, const std::string& sym) const {
        if (!enabled) return 1.0f;
        return weight(ec) * vol_size_scale(sym);
    }

    // Convenience: check by symbol rather than EngineClass
    bool equity_blocked(const std::string& sym) const {
        if (sym == "US500.F" || sym == "USTEC.F" || sym == "DJ30.F" || sym == "NAS100")
            return blocked(EngineClass::US_EQUITY_BREAKOUT);
        if (sym == "GER40" || sym == "UK100" || sym == "ESTX50")
            return blocked(EngineClass::EU_EQUITY_BREAKOUT);
        return false;
    }

    const char* vol_regime_name(const std::string& sym) const {
        auto it = sym_vol.find(sym);
        return (it != sym_vol.end()) ? it->second.name() : "NORMAL";
    }

    void print_status() const {
        std::printf("[REGIME] Confirmed=%s  Candidate=%s(%d/%d)  VIX=%.1f\n",
                    last_regime.c_str(), candidate_regime.c_str(),
                    candidate_count, dwell_bars, last_vix);
        for (const auto& kv : sym_vol) {
            std::printf("[REGIME-VOL] %s  vol=%s  scale=%.2f\n",
                        kv.first.c_str(), kv.second.name(), kv.second.size_scale());
        }
    }
};

}} // namespace omega::regime
