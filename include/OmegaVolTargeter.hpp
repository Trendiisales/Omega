#pragma once
// =============================================================================
// OmegaVolTargeter.hpp -- Volatility targeting + momentum regime classifier
//
// Reads from OHLCBarEngine atomics (adx14, ewma_vol_20, atr_expanding, rsi14)
// and macro event calendar to provide:
//
//   size_multiplier()     -- position size scalar from vol targeting + ADX boost
//   is_momentum_regime()  -- ADX >= 25 AND ATR expanding -> RSI extremes = continuation
//   rsi_conviction_mult() -- RSI<20 short in trending regime = 1.20x conviction
//   high_impact_window()  -- UTC time within +-30min of any HIGH impact event
//   load_events()         -- parse config/macro_events_today.txt
//   log_state()           -- diagnostic print on entry
//
// Usage in main.cpp:
//   OmegaVolTargeter g_vol_targeter;
//   g_vol_targeter.load_events("config/macro_events_today.txt");
//
//   // In compute_size / GoldStack sizing block:
//   const double vm = g_vol_targeter.size_multiplier(g_bars_gold.m1.ind);
//
//   // In Gate 3 RSI block (SHORT entry):
//   if (!gf_long && bar_rsi < RSI_OS) {
//       if (!g_vol_targeter.is_momentum_regime(g_bars_gold.m1.ind)) {
//           // block entry -- oversold in non-trending regime
//       }
//       // else: allow -- RSI<20 in trending = continuation short
//   }
//
// Thread safety: all reads are from atomics (relaxed). load_events() must be
// called from a single thread at startup before any trading begins.
// =============================================================================

#include <cmath>
#include <ctime>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include "OHLCBarEngine.hpp"

// =============================================================================
// MacroEvent -- a single HIGH-impact calendar event for today
// =============================================================================
struct MacroEvent {
    int    hour_utc   = 0;   // event hour   (UTC)
    int    minute_utc = 0;   // event minute (UTC)
    std::string label;        // e.g. "NFP", "CPI", "FOMC"
};

// =============================================================================
// OmegaVolTargeter
// =============================================================================
class OmegaVolTargeter {
public:
    // =========================================================================
    // size_multiplier()
    // Returns a position size scalar based on EWMA vol targeting + ADX conviction.
    //
    // Base: vol_target_mult = VOL_TARGET(20%) / ewma_vol_20 from OHLCBarEngine
    //   -- already clamped [0.25, 1.50] by the engine
    //
    // ADX strong-trend boost: when ADX >= 40 (strong trend), add +10% to mult
    //   Rationale: in a confirmed strong trend, Sharpe is empirically higher,
    //   so Kelly optimal size is larger. Cap at VOL_MULT_MAX after boost.
    //
    // Returns 1.0 if no valid vol data yet (ewma_vol_20 == 0).
    // =========================================================================
    double size_multiplier(const BarIndicators& ind) const noexcept {
        const double vol_s = ind.ewma_vol_20.load(std::memory_order_relaxed);
        if (vol_s < 1e-10) return 1.0;  // not yet seeded

        double mult = ind.vol_target_mult.load(std::memory_order_relaxed);

        // ADX strong-trend conviction boost (+10%)
        if (ind.adx_strong.load(std::memory_order_relaxed)) {
            mult *= 1.10;
        }

        // Re-clamp after boost
        mult = std::max(VOL_MULT_MIN, std::min(VOL_MULT_MAX, mult));
        return mult;
    }

    // =========================================================================
    // is_momentum_regime()
    // Returns true when:
    //   ADX >= ADX_TREND_THRESHOLD (25)  AND
    //   ATR is expanding (atr_expanding == true from OHLCBarEngine)
    //
    // When in momentum regime, RSI extremes signal CONTINUATION not reversal:
    //   RSI < 20 in a downtrend = aggressive selling pressure -> SHORT ok
    //   RSI > 80 in an uptrend  = aggressive buying pressure  -> LONG ok
    //
    // This flips Gate 3 logic: instead of blocking RSI<20 short entries,
    // Gate 3 should ALLOW them when is_momentum_regime() returns true.
    // =========================================================================
    bool is_momentum_regime(const BarIndicators& ind) const noexcept {
        const bool adx_trend   = ind.adx_trending.load(std::memory_order_relaxed);
        const bool atr_expand  = ind.atr_expanding.load(std::memory_order_relaxed);
        return adx_trend && atr_expand;
    }

    // =========================================================================
    // rsi_conviction_mult()
    // Returns a conviction multiplier for SHORT entries when RSI is deeply
    // oversold in a trending, momentum regime.
    //
    // RSI < RSI_EXTREME_SHORT (20) AND is_momentum_regime() AND short signal:
    //   base mult = 1.20 (20% size boost for high-conviction momentum short)
    //   scaled by ADX strength: (adx14 - 25) / (40 - 25) normalised [0,1]
    //   final = 1.0 + 0.20 * adx_scale   range: [1.00, 1.20]
    //
    // Returns 1.0 if conditions not met.
    // =========================================================================
    double rsi_conviction_mult(const BarIndicators& ind, bool is_short) const noexcept {
        if (!is_short) return 1.0;
        if (!is_momentum_regime(ind)) return 1.0;

        const double rsi = ind.rsi14.load(std::memory_order_relaxed);
        if (rsi >= RSI_EXTREME_SHORT) return 1.0;

        const double adx = ind.adx14.load(std::memory_order_relaxed);
        // Scale conviction linearly from ADX_TREND_THRESHOLD to ADX_STRONG_THRESHOLD
        const double adx_scale = std::max(0.0, std::min(1.0,
            (adx - ADX_TREND_THRESHOLD) / (ADX_STRONG_THRESHOLD - ADX_TREND_THRESHOLD)));

        return 1.0 + 0.20 * adx_scale;  // [1.00 .. 1.20]
    }

    // =========================================================================
    // high_impact_window()
    // Returns true if current UTC time is within +/- EVENT_WINDOW_MIN (30) minutes
    // of any loaded HIGH-impact event.
    //
    // Used to suppress new entries around major macro releases where spread
    // blows out and fills are unreliable.
    // =========================================================================
    bool high_impact_window() const noexcept {
        if (events_.empty()) return false;

        // Get current UTC time-of-day in minutes since midnight
        const std::time_t now_t = std::time(nullptr);
        std::tm utc_tm{};
#ifdef _WIN32
        gmtime_s(&utc_tm, &now_t);
#else
        gmtime_r(&now_t, &utc_tm);
#endif
        const int now_min = utc_tm.tm_hour * 60 + utc_tm.tm_min;

        for (const auto& ev : events_) {
            const int ev_min = ev.hour_utc * 60 + ev.minute_utc;
            if (std::abs(now_min - ev_min) <= EVENT_WINDOW_MIN) {
                return true;
            }
        }
        return false;
    }

    // =========================================================================
    // load_events()
    // Parse config/macro_events_today.txt for HIGH-impact USD/XAU/OIL events.
    //
    // File format (one event per line):
    //   HH:MM CURRENCY IMPACT LABEL
    //   13:30 USD HIGH NFP
    //   19:00 USD HIGH FOMC
    //   15:30 XAU HIGH EIA
    //
    // Lines not matching this format are silently skipped.
    // Only lines with IMPACT == "HIGH" are loaded.
    // Currencies: USD, XAU, OIL (others ignored -- not relevant to gold trading).
    // =========================================================================
    void load_events(const std::string& path) noexcept {
        events_.clear();
        FILE* f = fopen(path.c_str(), "r");
        if (!f) {
            printf("[VolTargeter] No macro event file at %s -- high_impact_window disabled\n",
                   path.c_str());
            fflush(stdout);
            return;
        }

        char line[256];
        int  loaded = 0;
        while (fgets(line, sizeof(line), f)) {
            int hh = 0, mm = 0;
            char currency[16]{}, impact[16]{}, label[64]{};
            // Expected format: "HH:MM CURRENCY IMPACT LABEL"
            const int parsed = sscanf(line, "%d:%d %15s %15s %63s",
                                      &hh, &mm, currency, impact, label);
            if (parsed < 4) continue;
            if (std::string(impact) != "HIGH") continue;

            const std::string ccy(currency);
            if (ccy != "USD" && ccy != "XAU" && ccy != "OIL") continue;

            MacroEvent ev;
            ev.hour_utc   = hh;
            ev.minute_utc = mm;
            ev.label      = (parsed >= 5) ? std::string(label) : std::string(currency);
            events_.push_back(ev);
            ++loaded;

            printf("[VolTargeter] Loaded HIGH event: %02d:%02d UTC %s %s\n",
                   hh, mm, currency, ev.label.c_str());
        }
        fclose(f);

        printf("[VolTargeter] %d HIGH-impact events loaded from %s\n",
               loaded, path.c_str());
        fflush(stdout);
    }

    // =========================================================================
    // log_state()
    // Full diagnostic print -- call once per entry attempt for observability.
    // =========================================================================
    void log_state(const BarIndicators& ind) const noexcept {
        const double adx      = ind.adx14         .load(std::memory_order_relaxed);
        const double vol_s    = ind.ewma_vol_20    .load(std::memory_order_relaxed);
        const double vol_l    = ind.ewma_vol_100   .load(std::memory_order_relaxed);
        const double vol_r    = ind.vol_ratio_ewma .load(std::memory_order_relaxed);
        const double vtm      = ind.vol_target_mult.load(std::memory_order_relaxed);
        const bool   adx_tr   = ind.adx_trending   .load(std::memory_order_relaxed);
        const bool   adx_str  = ind.adx_strong     .load(std::memory_order_relaxed);
        const bool   adx_rise = ind.adx_rising     .load(std::memory_order_relaxed);
        const bool   atr_exp  = ind.atr_expanding  .load(std::memory_order_relaxed);
        const bool   mom      = is_momentum_regime(ind);
        const bool   hiw      = high_impact_window();
        const double sm       = size_multiplier(ind);

        printf("[VolTargeter] ADX=%.1f %s%s%s | vol_s=%.1f%% vol_l=%.1f%% ratio=%.2f"
               " vtm=%.2f sm=%.2f | MomRegime=%s ATRexp=%s HighImpact=%s\n",
               adx,
               adx_tr   ? "TRENDING "  : "",
               adx_str  ? "STRONG "    : "",
               adx_rise ? "RISING "    : "",
               vol_s * 100.0, vol_l * 100.0, vol_r,
               vtm, sm,
               mom ? "YES" : "no",
               atr_exp ? "YES" : "no",
               hiw ? "YES" : "no");
        fflush(stdout);
    }

private:
    std::vector<MacroEvent> events_;

    // Thresholds -- mirror OHLCBarEngine constants for self-containment
    static constexpr double ADX_TREND_THRESHOLD  = 25.0;
    static constexpr double ADX_STRONG_THRESHOLD = 40.0;
    static constexpr double VOL_MULT_MIN         = 0.25;
    static constexpr double VOL_MULT_MAX         = 1.50;
    static constexpr double RSI_EXTREME_SHORT    = 20.0;  // RSI below this = extreme OS
    static constexpr int    EVENT_WINDOW_MIN     = 30;    // minutes either side of event
};
