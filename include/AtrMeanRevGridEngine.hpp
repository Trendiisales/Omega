#pragma once
// =============================================================================
// AtrMeanRevGridEngine.hpp  --  Forex mean-reversion grid (C++ port of the
// AtrMeanRevGrid.mq5 v0.20 MT5 EA, 2026-05-25). CRTP-based so per-symbol
// params resolve at compile time. Designed for H1 bars (MA + RSI + ATR all
// computed on H1 closes). Tick-driven SL/TP monitoring.
//
// Strategy in one paragraph:
//   BUY when price is X*ATR below slow EMA AND RSI is below threshold. SELL
//   mirrors. Once in, add levels with multiplier ramp (Mult A for L1, Mult B
//   for L2-4, Mult C for L5+). Add trigger: RSI must recover by N then
//   re-dip, AND >= MinCandlesBetweenAdds bars since last entry, AND price
//   has moved >= (BaseMult * EffectiveRatio * ATR_short) from last entry,
//   where EffectiveRatio = MAX(1.0, ATR_short/ATR_long). Unified trailing
//   SL anchored to slow EMA + (X+Y)*ATR buffer, ratchet only. TP: either
//   internal RSI/MA close-all OR WAP-based broker-style close-all (fixed
//   pips, % from WAP, or N*ATR from WAP). MaxAccountExposurePct circuit
//   breaker. Optional MA cap on WAP target. Optional time-based fallback.
//
// CRTP shape mirrors SweepableEnginesCRTP: Base implements logic, per-symbol
// derived struct provides constexpr param values via Traits typedef.
//
// Backtest-first. shadow_mode=true by default. Engine MUST be warm-seeded
// via seed_from_h1_csv() (or the H1 bar-replay path) before live use --
// CLAUDE.md "Engine Warm-Seed Mandate" mandates this for any new D1/H4/H1
// engine.
//
// History:
//   v0.10 -- MT5 EA prototype 2026-05-25
//   v0.20 -- spec firmed up (TP modes, OnTradeTransaction, no fast MA)
//   v0.30 -- C++ CRTP port (this file) 2026-05-25
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <functional>
#include "OmegaTradeLedger.hpp"  // omega::TradeRecord
// CostGuard left optional -- engine uses its own SL geometry to gate fills.
// Shadow-CSV writer is injected via on_close_cb (set in engine_init.hpp) so
// this header avoids a hard include of logging.hpp / omega_runtime.hpp and
// can compile standalone in backtest harnesses that don't have the live
// shadow CSV plumbing wired up.

namespace omega {

// -----------------------------------------------------------------------------
// TP methods (mirror the MT5 enum exactly)
// -----------------------------------------------------------------------------
enum class AmrTpMethod : int {
    RSI_OR_MA           = 1,
    FIXED_PIPS_FROM_WAP = 2,
    PCT_FROM_WAP        = 3,
    ATR_FROM_WAP        = 4
};

enum class AmrLotMode : int {
    FIXED    = 0,
    RISK_PCT = 1
};

// -----------------------------------------------------------------------------
// Default-params struct. Per-symbol Traits inherits + overrides as needed.
// All values are static constexpr -- read via Derived::traits_t::PARAM at
// compile time, so the optimiser can inline / fold constants.
// -----------------------------------------------------------------------------
struct AmrBaseParams {
    // Indicators -- per-TF sweep overrides via compile macros so the multi-TF
    // harness can scale H1-calibrated periods to the chosen bar duration.
    static constexpr int    ATR_PERIOD_SHORT  =
    #ifdef AMR_OVERRIDE_ATR_SHORT
        AMR_OVERRIDE_ATR_SHORT;
    #else
        100;
    #endif
    static constexpr int    ATR_PERIOD_LONG   =
    #ifdef AMR_OVERRIDE_ATR_LONG
        AMR_OVERRIDE_ATR_LONG;
    #else
        1000;
    #endif
    static constexpr int    SLOW_MA_PERIOD    =
    #ifdef AMR_OVERRIDE_EMA
        AMR_OVERRIDE_EMA;
    #else
        200;
    #endif
    static constexpr int    RSI_PERIOD        =
    #ifdef AMR_OVERRIDE_RSI
        AMR_OVERRIDE_RSI;
    #else
        14;
    #endif

    // Entry / SL geometry (ATR-multiplier based). Sweep override via macros.
#ifdef AMR_OVERRIDE_X
    static constexpr double ENTRY_ATR_MULT_X  = AMR_OVERRIDE_X;
#else
    static constexpr double ENTRY_ATR_MULT_X  = 10.0;
#endif
#ifdef AMR_OVERRIDE_ADD
    static constexpr double ADD_DIST_BASE_MULT= AMR_OVERRIDE_ADD;
#else
    static constexpr double ADD_DIST_BASE_MULT= 1.0;
#endif
    static constexpr double SL_ATR_BUFFER_Y   =
    #ifdef AMR_OVERRIDE_SL_Y
        AMR_OVERRIDE_SL_Y;
    #else
        4.0;
    #endif

    // RSI bands
    static constexpr double RSI_ENTRY_LOW     =
    #ifdef AMR_OVERRIDE_RSI_LOW
        AMR_OVERRIDE_RSI_LOW;
    #else
        20.0;
    #endif
    static constexpr double RSI_RECOVERY_OFF  =
    #ifdef AMR_OVERRIDE_RSI_RECOV
        AMR_OVERRIDE_RSI_RECOV;
    #else
        15.0;
    #endif
    static constexpr double RSI_TP_LEVEL      = 50.0;

    // Grid multipliers
    static constexpr double MULT_A            = 1.5;     // L1
    static constexpr double MULT_B            = 1.7;     // L2-L4
    static constexpr double MULT_C            = 2.0;     // L5+
    static constexpr int    MIN_CANDLES_BETWEEN_ADDS = 5;

    // Risk
    static constexpr double MAX_ACCT_EXPOSURE_PCT = 25.0;
    static constexpr double MAX_SPREAD_PRICE      = 0.00050; // ~5 pips on 5-dp pair (tick CSVs include off-session quotes)

    // Sizing
    static constexpr AmrLotMode LOT_MODE       = AmrLotMode::FIXED;
    static constexpr double     BASE_LOT       = 0.01;
    static constexpr double     RISK_PCT_PER_TRADE = 0.5;

    // TP
    static constexpr AmrTpMethod TP_METHOD     =
    #ifdef AMR_OVERRIDE_TP_METHOD
        AmrTpMethod::AMR_OVERRIDE_TP_METHOD;
    #else
        AmrTpMethod::RSI_OR_MA;
    #endif
    static constexpr double      TP_FIXED_PIPS = 200.0;  // method 2 (points = 1/POINT units)
    static constexpr double      TP_PCT_FROM_WAP = 0.5;  // method 3 (% of WAP)
    static constexpr double      TP_ATR_MULT_FROM_WAP = 8.0; // method 4
    static constexpr bool        USE_MA_CAP    = false;
    static constexpr bool        USE_TIME_EXIT = false;
    static constexpr int         TIME_EXIT_BARS = 200;

    // Misc
    static constexpr bool        VERBOSE_LOG   = true;

    // Bar interval (ms) -- aggregator + indicator cadence. Default H1.
    // Override per-TF via -DAMR_OVERRIDE_BAR_MS=<ms>.
    //   M5 = 300000  M15 = 900000  M30 = 1800000  H1 = 3600000
    static constexpr std::int64_t BAR_INTERVAL_MS =
    #ifdef AMR_OVERRIDE_BAR_MS
        AMR_OVERRIDE_BAR_MS;
    #else
        3600LL * 1000LL;
    #endif
};

// -----------------------------------------------------------------------------
// Per-symbol Traits structs. To add a new symbol: inherit AmrBaseParams and
// override only the differing constants. Wire in engine_init.hpp.
// -----------------------------------------------------------------------------
struct AmrTraits_EURUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "EURUSD";
    static constexpr double POINT = 0.00001;  // 5-dp pricing
    // 2026-05-26 S37f -- replaced M15 X=14 with H4 X=3 SL=7 per full validation
    // pipeline (3-period intersect + WF 3/4 folds + OOS hold-out PF>=0.7*IS).
    // Survivor table:
    //   H4 X=3 SL=7  IS PF 1.60  OOS PF 1.89  WR 75%  OOS beats IS (clean)
    // Prior M15 X=14 (PF 1.04 sweep, no WF support) -> failed validation.
    static constexpr std::int64_t BAR_INTERVAL_MS = 14400LL * 1000LL; // H4
    static constexpr double       ENTRY_ATR_MULT_X = 3.0;
    // 2026-06-14: was SL_ATR_MULT_Y (wrong name -> silently ignored, engine
    // reads SL_ATR_BUFFER_Y). Bug ran EURUSD at base SL=4 not validated 7.
    // Independent HistData WF: SL=4 PF1.06 (H2 0.61, fails) -> SL=7 PF1.39
    // (both halves +, WR71%). Corrected to the validated value.
    static constexpr double       SL_ATR_BUFFER_Y  = 7.0;
};

struct AmrTraits_GBPUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "GBPUSD";
    static constexpr double POINT = 0.00001;
    static constexpr double MAX_SPREAD_PRICE = 0.00030; // GBP typically wider
};

struct AmrTraits_AUDUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "AUDUSD";
    static constexpr double POINT = 0.00001;
};

struct AmrTraits_NZDUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "NZDUSD";
    static constexpr double POINT = 0.00001;
    static constexpr double MAX_SPREAD_PRICE = 0.00035; // NZD wider still
};

// USDJPY: 3-dp pricing; pip = 0.01, point = 0.001. ~10x bigger absolute moves
// than EURUSD so MAX_SPREAD_PRICE scaled accordingly.
struct AmrTraits_USDJPY : AmrBaseParams {
    static constexpr const char* SYMBOL = "USDJPY";
    static constexpr double POINT = 0.001;
    static constexpr double MAX_SPREAD_PRICE = 0.050;
    static constexpr double TP_FIXED_PIPS = 200.0;
};

struct AmrTraits_USDCAD : AmrBaseParams {
    static constexpr const char* SYMBOL = "USDCAD";
    static constexpr double POINT = 0.00001;
    static constexpr double MAX_SPREAD_PRICE = 0.00035;
};

struct AmrTraits_EURGBP : AmrBaseParams {
    static constexpr const char* SYMBOL = "EURGBP";
    static constexpr double POINT = 0.00001;
    static constexpr double MAX_SPREAD_PRICE = 0.00030;
    // 2026-05-26 S37f -- validated H1 X=5 SL=3 per full validation pipeline.
    //   H1 X=5 SL=3  IS PF 1.80  OOS PF 1.68  WR 60%  RF 1.39
    static constexpr std::int64_t BAR_INTERVAL_MS = 3600LL * 1000LL; // H1
    static constexpr double       ENTRY_ATR_MULT_X = 5.0;
    // 2026-06-14: same wrong-name bug as EURUSD (was SL_ATR_MULT_Y, ignored).
    // EURGBP is PARKED (not enabled) and fails WF either way (SL=4 PF0.72,
    // SL=3 PF0.53 on HistData) -- rename is correctness only; stays parked.
    static constexpr double       SL_ATR_BUFFER_Y  = 3.0;
};

// =============================================================================
// INDEX traits -- from 2026-05-26 deep eval sweep on real tick CSVs.
// Production symbol names: US500 (=SPXUSD), NAS100 (=NSXUSD), GER40.
// =============================================================================

// US500 / SPXUSD: H1, X=8, SL_Y=6, TP=ATR_FROM_WAP
// Sweep result: 86 trd, WR 38%, PF 1.75, +$81.49, DD $31.86, Recovery 2.56
struct AmrTraits_US500 : AmrBaseParams {
    static constexpr const char* SYMBOL = "US500";
    static constexpr double POINT = 0.1;
    static constexpr std::int64_t BAR_INTERVAL_MS = 3600LL * 1000LL;   // H1
    static constexpr double       ENTRY_ATR_MULT_X = 8.0;
    static constexpr double       SL_ATR_BUFFER_Y  = 6.0;
    static constexpr AmrTpMethod  TP_METHOD = AmrTpMethod::ATR_FROM_WAP;
    static constexpr double       MAX_SPREAD_PRICE = 2.0;
};

// NAS100 / NSXUSD: M15, X=14, SL_Y=4, TP=RSI_OR_MA
// Sweep result: 69 trd, WR 26%, PF 1.55, +$15.50, DD $10.46, Recovery 1.48
struct AmrTraits_NAS100 : AmrBaseParams {
    static constexpr const char* SYMBOL = "NAS100";
    static constexpr double POINT = 0.1;
    static constexpr std::int64_t BAR_INTERVAL_MS = 900LL * 1000LL;    // M15
    static constexpr double       ENTRY_ATR_MULT_X = 14.0;
    static constexpr double       SL_ATR_BUFFER_Y  = 4.0;
    static constexpr AmrTpMethod  TP_METHOD = AmrTpMethod::RSI_OR_MA;
    static constexpr double       MAX_SPREAD_PRICE = 5.0;
};

// GER40: M15, X=14, SL_Y=6, TP=ATR_FROM_WAP
// Stage-4 stress result: 37 trd, WR 43%, PF 1.86
struct AmrTraits_GER40 : AmrBaseParams {
    static constexpr const char* SYMBOL = "GER40";
    static constexpr double POINT = 0.1;
    static constexpr std::int64_t BAR_INTERVAL_MS = 900LL * 1000LL;    // M15
    static constexpr double       ENTRY_ATR_MULT_X = 14.0;
    static constexpr double       SL_ATR_BUFFER_Y  = 6.0;
    static constexpr AmrTpMethod  TP_METHOD = AmrTpMethod::ATR_FROM_WAP;
    static constexpr double       MAX_SPREAD_PRICE = 5.0;
};

// "Sweep" traits = inherit base only (no per-pair overrides). Macros control all
// params (X, BAR_INTERVAL_MS, RSI, SL_Y, etc). Used by deep-eval sweep harness.
struct AmrTraits_SWEEP_EURUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "EURUSD";
    static constexpr double POINT = 0.00001;
};
struct AmrTraits_SWEEP_GBPUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "GBPUSD";
    static constexpr double POINT = 0.00001;
    static constexpr double MAX_SPREAD_PRICE = 0.00030;
};
struct AmrTraits_SWEEP_AUDUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "AUDUSD";
    static constexpr double POINT = 0.00001;
};
struct AmrTraits_SWEEP_NZDUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "NZDUSD";
    static constexpr double POINT = 0.00001;
    static constexpr double MAX_SPREAD_PRICE = 0.00035;
};
struct AmrTraits_SWEEP_USDCAD : AmrBaseParams {
    static constexpr const char* SYMBOL = "USDCAD";
    static constexpr double POINT = 0.00001;
    static constexpr double MAX_SPREAD_PRICE = 0.00035;
};
// Non-FX traits (Stage 4 stress test). Adjusted MAX_SPREAD_PRICE for instrument scale.
struct AmrTraits_SWEEP_BCOUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "BCOUSD";  // Brent crude (~$60-90)
    static constexpr double POINT = 0.01;
    static constexpr double MAX_SPREAD_PRICE = 0.10;
};
struct AmrTraits_SWEEP_GER40 : AmrBaseParams {
    static constexpr const char* SYMBOL = "GER40";  // DAX (~15000-20000)
    static constexpr double POINT = 0.1;
    static constexpr double MAX_SPREAD_PRICE = 5.0;
};
struct AmrTraits_SWEEP_SPXUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "SPXUSD";  // S&P (~4000-5500)
    static constexpr double POINT = 0.1;
    static constexpr double MAX_SPREAD_PRICE = 2.0;
};
struct AmrTraits_SWEEP_NSXUSD : AmrBaseParams {
    static constexpr const char* SYMBOL = "NSXUSD";  // NAS-100 (~15000-21000)
    static constexpr double POINT = 0.1;
    static constexpr double MAX_SPREAD_PRICE = 5.0;
};

// =============================================================================
// CRTP Engine -- all logic lives here.
//
// Usage:
//   omega::AtrMeanRevGridEngine<omega::AmrTraits_EURUSD> g_amr_eurusd;
//   ... (in tick handler) ...
//   g_amr_eurusd.on_tick(bid, ask, ts_ms);
//   ... (in H1 bar boundary) ...
//   g_amr_eurusd.on_h1_bar(o, h, l, c, ts_ms);
//
// Warm-seed (CLAUDE.md mandate) before live:
//   omega::seed_h1_amr(g_amr_eurusd, "phase1/signal_discovery/warmup_EURUSD_H1.csv");
// =============================================================================
template<class Traits>
class AtrMeanRevGridEngine {
public:
    using traits_t = Traits;

    bool enabled     = true;
    bool shadow_mode = true;  // default safe; flip in engine_init.hpp after validation
    // Trade-close callback (set in engine_init.hpp to write_shadow_csv). Optional.
    std::function<void(const TradeRecord&)> on_close_cb = nullptr;

    AtrMeanRevGridEngine() {
        // Pre-allocate ring history sized to the longest period we need.
        const int cap = std::max({Traits::ATR_PERIOD_LONG, Traits::SLOW_MA_PERIOD, Traits::RSI_PERIOD * 4}) + 8;
        bar_closes_.reserve_capacity_hint = cap;
    }

    // -------------------------------------------------------------------------
    // Bar callback -- updates EMA, ATR_short, ATR_long, RSI, and runs the
    // decision loop (entry / add / TP / SL trail) once per bar close.
    // -------------------------------------------------------------------------
    void on_h1_bar(double o, double h, double l, double c, std::int64_t ts_ms) {
        (void)o;
        push_bar(h, l, c);

        // Compute indicators only when we have enough history.
        if ((int)bar_closes_.deque.size() < Traits::ATR_PERIOD_LONG + 2) return;

        const double slow_ma   = compute_ema(bar_closes_.deque, Traits::SLOW_MA_PERIOD);
        const double atr_short = compute_atr(Traits::ATR_PERIOD_SHORT);
        const double atr_long  = compute_atr(Traits::ATR_PERIOD_LONG);
        const double rsi       = compute_rsi(Traits::RSI_PERIOD);

        if (atr_short <= 0.0 || atr_long <= 0.0) return;

        // Cache for tick-side TP/SL evaluation (broker-style)
        cached_slow_ma_   = slow_ma;
        cached_atr_short_ = atr_short;
        cached_atr_long_  = atr_long;
        cached_rsi_       = rsi;

        if (!enabled) return;

        // SL trail anchored to slow EMA + (X+Y)*ATR buffer, ratchet only
        trail_sl(slow_ma, atr_short, ts_ms);

        // TP / time-based close-all (internal modes 1; modes 2/3/4 fire on tick)
        if (long_.levels_open  > 0 && should_close_internal(/*is_long=*/true,  slow_ma, rsi, ts_ms)) close_all(/*is_long=*/true,  "rsi_or_slow_ma", ts_ms);
        if (short_.levels_open > 0 && should_close_internal(/*is_long=*/false, slow_ma, rsi, ts_ms)) close_all(/*is_long=*/false, "rsi_or_slow_ma", ts_ms);

        // Entry / add
        try_entry(slow_ma, rsi, atr_short, atr_long, ts_ms);
    }

    // -------------------------------------------------------------------------
    // Tick callback -- for broker-style TP modes (2/3/4), check WAP-relative
    // price targets. Also evaluates SL on every tick. Internally aggregates
    // H1 bars from tick mids; calls on_h1_bar() on bar close so production
    // dispatchers only need to call on_tick() (no separate H1 hookup needed).
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, std::int64_t ts_ms) {
        cached_bid_ = bid;
        cached_ask_ = ask;

        // Internal bar aggregator (interval from Traits, default H1)
        const double mid = (bid + ask) * 0.5;
        constexpr std::int64_t BAR_MS = Traits::BAR_INTERVAL_MS;
        const std::int64_t bar = (ts_ms / BAR_MS) * BAR_MS;
        if (!h1_active_) {
            h1_bar_start_ = bar;
            h1_o_ = h1_h_ = h1_l_ = h1_c_ = mid;
            h1_active_ = true;
        } else if (bar != h1_bar_start_) {
            // Close prior bar
            on_h1_bar(h1_o_, h1_h_, h1_l_, h1_c_, h1_bar_start_);
            h1_bar_start_ = bar;
            h1_o_ = h1_h_ = h1_l_ = h1_c_ = mid;
        } else {
            if (mid > h1_h_) h1_h_ = mid;
            if (mid < h1_l_) h1_l_ = mid;
            h1_c_ = mid;
        }

        if (!enabled) return;

        // Broker-style WAP TP modes -- intra-bar wick capture
        if constexpr (Traits::TP_METHOD != AmrTpMethod::RSI_OR_MA) {
            if (long_.levels_open  > 0) {
                const double tp_long  = wap_target(/*is_long=*/true);
                if (tp_long > 0.0 && bid >= tp_long) close_all(/*is_long=*/true,  "wap_tp_hit", ts_ms);
            }
            if (short_.levels_open > 0) {
                const double tp_short = wap_target(/*is_long=*/false);
                if (tp_short > 0.0 && ask <= tp_short) close_all(/*is_long=*/false, "wap_tp_hit", ts_ms);
            }
        }

        // Unified SL check (intra-bar)
        if (long_.levels_open  > 0 && long_.current_sl  > 0.0 && bid <= long_.current_sl)  close_all(/*is_long=*/true,  "sl_hit", ts_ms);
        if (short_.levels_open > 0 && short_.current_sl > 0.0 && ask >= short_.current_sl) close_all(/*is_long=*/false, "sl_hit", ts_ms);
    }

    // -------------------------------------------------------------------------
    // Warm-seed from H1 CSV (CLAUDE.md mandate). Format: ts,o,h,l,c (ts secs).
    // Replays bars with `enabled=false` so no entries fire during seeding.
    // -------------------------------------------------------------------------
    void seed_from_h1_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            std::fprintf(stderr, "[SEED-FAIL] %s: cannot open %s\n", Traits::SYMBOL, path.c_str());
            return;
        }
        const bool was_enabled = enabled;
        enabled = false;
        std::string line;
        int n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#' || line[0] == 't') continue; // skip header / comments
            std::int64_t ts = 0;
            double o=0, h=0, l=0, c=0;
            char* p = const_cast<char*>(line.c_str());
            ts = std::strtoll(p, &p, 10); if (*p==',') ++p;
            o  = std::strtod(p, &p);      if (*p==',') ++p;
            h  = std::strtod(p, &p);      if (*p==',') ++p;
            l  = std::strtod(p, &p);      if (*p==',') ++p;
            c  = std::strtod(p, &p);
            on_h1_bar(o, h, l, c, ts * 1000);
            ++n;
        }
        enabled = was_enabled;
        std::printf("[SEED] %s loaded %d H1 bars from %s (buffer=%zu)\n",
                    Traits::SYMBOL, n, path.c_str(), bar_closes_.deque.size());
    }

    // -------------------------------------------------------------------------
    // Persist bar deques to disk -- eliminates the per-restart warmup pain.
    // Called once per minute from the bar-save thread; loaded once at boot.
    // Format (binary, little-endian, host-native double):
    //   header: magic 'AMRS' + version u32 + n_bars u32 + prev_close f64
    //   body:   n_bars * (close f64, high f64, low f64, tr f64)
    // Returns bytes written / -1 on error.
    // -------------------------------------------------------------------------
    int save_state(const std::string& path) const noexcept {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return -1;
        const std::uint32_t magic = 0x53524D41; // 'AMRS'
        const std::uint32_t ver   = 1u;
        const std::uint32_t n     = static_cast<std::uint32_t>(bar_closes_.deque.size());
        std::fwrite(&magic, 4, 1, f);
        std::fwrite(&ver,   4, 1, f);
        std::fwrite(&n,     4, 1, f);
        std::fwrite(&prev_close_, sizeof(double), 1, f);
        for (std::uint32_t i = 0; i < n; ++i) {
            const double c = bar_closes_.deque[i];
            const double h = (i < bar_highs_.size()) ? bar_highs_[i] : c;
            const double l = (i < bar_lows_.size())  ? bar_lows_[i]  : c;
            const double t = (i < tr_buf_.size())    ? tr_buf_[i]    : 0.0;
            std::fwrite(&c, sizeof(double), 1, f);
            std::fwrite(&h, sizeof(double), 1, f);
            std::fwrite(&l, sizeof(double), 1, f);
            std::fwrite(&t, sizeof(double), 1, f);
        }
        std::fclose(f);
        return 16 + static_cast<int>(n) * 32;
    }

    // Load persisted state. Returns true if loaded; false on missing/corrupt.
    // Engine deque clears + repopulates from file. enabled toggle preserved.
    bool load_state(const std::string& path) noexcept {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::uint32_t magic = 0, ver = 0, n = 0;
        if (std::fread(&magic, 4, 1, f) != 1 || magic != 0x53524D41) { std::fclose(f); return false; }
        if (std::fread(&ver, 4, 1, f) != 1   || ver != 1u)            { std::fclose(f); return false; }
        if (std::fread(&n, 4, 1, f) != 1     || n > 10000)            { std::fclose(f); return false; }
        double prev_c = 0.0;
        if (std::fread(&prev_c, sizeof(double), 1, f) != 1)           { std::fclose(f); return false; }
        bar_closes_.deque.clear();
        bar_highs_.clear();
        bar_lows_.clear();
        tr_buf_.clear();
        bool ok = true;
        for (std::uint32_t i = 0; i < n; ++i) {
            double c=0,h=0,l=0,t=0;
            if (std::fread(&c, sizeof(double), 1, f) != 1) { ok=false; break; }
            if (std::fread(&h, sizeof(double), 1, f) != 1) { ok=false; break; }
            if (std::fread(&l, sizeof(double), 1, f) != 1) { ok=false; break; }
            if (std::fread(&t, sizeof(double), 1, f) != 1) { ok=false; break; }
            bar_closes_.deque.push_back(c);
            bar_highs_.push_back(h);
            bar_lows_.push_back(l);
            tr_buf_.push_back(t);
        }
        std::fclose(f);
        if (!ok) { bar_closes_.deque.clear(); bar_highs_.clear(); bar_lows_.clear(); tr_buf_.clear(); return false; }
        prev_close_ = prev_c;
        std::printf("[STATE-LOAD] %s loaded %u bars from %s (no warmup needed)\n",
                    Traits::SYMBOL, n, path.c_str());
        return true;
    }

    // -------------------------------------------------------------------------
    // Public diagnostics
    // -------------------------------------------------------------------------
    int long_levels() const  { return long_.levels_open; }
    int short_levels() const { return short_.levels_open; }
    double last_long_sl() const  { return long_.current_sl; }
    double last_short_sl() const { return short_.current_sl; }

private:
    // ---------- Indicator math ----------
    struct BarBuf {
        std::deque<double> deque;
        int reserve_capacity_hint = 64;
    };
    BarBuf bar_closes_;
    std::deque<double> bar_highs_;
    std::deque<double> bar_lows_;
    // True-range buffer; computed as max(h-l, |h-prev_c|, |l-prev_c|)
    std::deque<double> tr_buf_;
    double prev_close_ = 0.0;

    void push_bar(double h, double l, double c) {
        const int max_hist = Traits::ATR_PERIOD_LONG + Traits::SLOW_MA_PERIOD + 8;
        const double tr = (prev_close_ > 0.0)
                          ? std::max({h - l, std::fabs(h - prev_close_), std::fabs(l - prev_close_)})
                          : (h - l);
        bar_closes_.deque.push_back(c);
        bar_highs_.push_back(h);
        bar_lows_.push_back(l);
        tr_buf_.push_back(tr);
        while ((int)bar_closes_.deque.size() > max_hist) bar_closes_.deque.pop_front();
        while ((int)bar_highs_.size()        > max_hist) bar_highs_.pop_front();
        while ((int)bar_lows_.size()         > max_hist) bar_lows_.pop_front();
        while ((int)tr_buf_.size()           > max_hist) tr_buf_.pop_front();
        prev_close_ = c;
        ++bar_count_;
    }

    static double compute_ema(const std::deque<double>& v, int period) {
        if ((int)v.size() < period) return 0.0;
        const double k = 2.0 / (period + 1);
        // Seed with SMA over first `period` closes, then walk forward.
        double ema = 0.0;
        for (int i = 0; i < period; ++i) ema += v[i];
        ema /= period;
        for (int i = period; i < (int)v.size(); ++i) ema = v[i] * k + ema * (1.0 - k);
        return ema;
    }

    double compute_atr(int period) const {
        if ((int)tr_buf_.size() < period + 1) return 0.0;
        // Wilder's smoothing equivalent: ATR = ((n-1)*ATR_prev + TR) / n.
        // Seed with SMA over first `period` TRs.
        double atr = 0.0;
        for (int i = 1; i <= period; ++i) atr += tr_buf_[i];
        atr /= period;
        for (int i = period + 1; i < (int)tr_buf_.size(); ++i)
            atr = ((period - 1) * atr + tr_buf_[i]) / period;
        return atr;
    }

    double compute_rsi(int period) const {
        if ((int)bar_closes_.deque.size() < period + 1) return 50.0;
        double gain = 0.0, loss = 0.0;
        const auto& v = bar_closes_.deque;
        const int n = (int)v.size();
        const int start = n - period - 1;
        for (int i = start + 1; i < n; ++i) {
            const double d = v[i] - v[i - 1];
            if (d > 0) gain += d; else loss -= d;
        }
        gain /= period;
        loss /= period;
        if (loss < 1e-12) return 100.0;
        const double rs = gain / loss;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    // ---------- Grid state ----------
    struct Side {
        int      levels_open       = 0;
        double   last_entry_price  = 0.0;
        std::int64_t last_entry_ts = 0;
        double   last_entry_lot    = 0.0;
        double   current_sl        = 0.0;
        double   rsi_at_last_entry = 0.0;
        bool     rsi_recovered     = false;
        std::int64_t first_entry_ts= 0;
        int      first_entry_bar   = 0;
        // Open-position storage: parallel arrays for cache-friendly iteration.
        // Bounded by max levels possible under the multiplier ramp before
        // MaxAccountExposure kicks in -- 16 is plenty for any realistic setting.
        static constexpr int CAPACITY = 16;
        double  entry_px[CAPACITY] = {0};
        double  lot[CAPACITY]      = {0};
        std::int64_t entry_ts[CAPACITY] = {0};
    };
    Side long_;
    Side short_;
    int  bar_count_ = 0;
    // Internal H1 aggregator state (so production dispatch can use on_tick only)
    bool   h1_active_ = false;
    std::int64_t h1_bar_start_ = 0;
    double h1_o_ = 0, h1_h_ = 0, h1_l_ = 0, h1_c_ = 0;
public:
    std::int64_t spread_skips_ = 0;  // count of try_entry calls rejected by spread filter
private:

    // Cached indicator values (set on each H1 bar)
    double cached_slow_ma_   = 0.0;
    double cached_atr_short_ = 0.0;
    double cached_atr_long_  = 0.0;
    double cached_rsi_       = 50.0;
    double cached_bid_       = 0.0;
    double cached_ask_       = 0.0;

    // ---------- WAP / floating PnL ----------
    void compute_wap(const Side& s, double& wap, double& tot_lot) const {
        double sp = 0.0, sv = 0.0;
        for (int i = 0; i < s.levels_open; ++i) {
            sp += s.entry_px[i] * s.lot[i];
            sv += s.lot[i];
        }
        wap = (sv > 0.0) ? sp / sv : 0.0;
        tot_lot = sv;
    }

    double floating_pnl(const Side& s, bool is_long) const {
        double pnl = 0.0;
        const double px = is_long ? cached_bid_ : cached_ask_;
        if (px <= 0.0) return 0.0;
        for (int i = 0; i < s.levels_open; ++i) {
            const double diff = is_long ? (px - s.entry_px[i]) : (s.entry_px[i] - px);
            pnl += diff * s.lot[i];
        }
        return pnl;
    }

    // ---------- Add distance (vol-adaptive, floor=1.0) ----------
    double add_distance_px(double atr_short, double atr_long) const {
        const double ratio = (atr_long > 0.0) ? atr_short / atr_long : 1.0;
        const double eff   = std::max(1.0, ratio);
        return Traits::ADD_DIST_BASE_MULT * eff * atr_short;
    }

    // ---------- Entry / add ----------
    void try_entry(double slow_ma, double rsi, double atr_short, double atr_long, std::int64_t ts_ms) {
        // Spread filter (silent skip -- spread excursions are common on tick
        // CSV data; logging every one floods the backtest output. Counter
        // can be added if visibility needed.)
        if (cached_bid_ > 0.0 && cached_ask_ > cached_bid_) {
            const double spr = cached_ask_ - cached_bid_;
            if (spr > Traits::MAX_SPREAD_PRICE) { ++spread_skips_; return; }
        }

        const double entry_dist = Traits::ENTRY_ATR_MULT_X * atr_short;
        const double rsi_high   = 100.0 - Traits::RSI_ENTRY_LOW;

        // --- BUY ---
        const double ask = (cached_ask_ > 0.0 ? cached_ask_ : cached_close());
        if (ask > 0.0) {
            const bool price_ok = (ask <= slow_ma - entry_dist);
            const bool rsi_ok   = (rsi <= Traits::RSI_ENTRY_LOW);
            if (long_.levels_open == 0) {
                if (price_ok && rsi_ok) place_buy(slow_ma, atr_short, /*is_base=*/true, ts_ms);
            } else {
                update_rsi_recovery(long_, rsi, /*is_long=*/true);
                const int bars_since = bar_count_ - long_.first_entry_bar; // approximation: bars since base
                const bool candle_ok = (bars_since >= Traits::MIN_CANDLES_BETWEEN_ADDS);
                const double need    = add_distance_px(atr_short, atr_long);
                const bool dist_ok   = (long_.last_entry_price - ask) >= need;
                if (long_.rsi_recovered && candle_ok && dist_ok && rsi_ok)
                    place_buy(slow_ma, atr_short, /*is_base=*/false, ts_ms);
            }
        }

        // --- SELL ---
        const double bid = (cached_bid_ > 0.0 ? cached_bid_ : cached_close());
        if (bid > 0.0) {
            const bool price_ok = (bid >= slow_ma + entry_dist);
            const bool rsi_ok   = (rsi >= rsi_high);
            if (short_.levels_open == 0) {
                if (price_ok && rsi_ok) place_sell(slow_ma, atr_short, /*is_base=*/true, ts_ms);
            } else {
                update_rsi_recovery(short_, rsi, /*is_long=*/false);
                const int bars_since = bar_count_ - short_.first_entry_bar;
                const bool candle_ok = (bars_since >= Traits::MIN_CANDLES_BETWEEN_ADDS);
                const double need    = add_distance_px(atr_short, atr_long);
                const bool dist_ok   = (bid - short_.last_entry_price) >= need;
                if (short_.rsi_recovered && candle_ok && dist_ok && rsi_ok)
                    place_sell(slow_ma, atr_short, /*is_base=*/false, ts_ms);
            }
        }
    }

    double cached_close() const { return bar_closes_.deque.empty() ? 0.0 : bar_closes_.deque.back(); }

    void update_rsi_recovery(Side& s, double rsi, bool is_long) {
        if (s.levels_open == 0) return;
        if (is_long) { if (rsi >= s.rsi_at_last_entry + Traits::RSI_RECOVERY_OFF) s.rsi_recovered = true; }
        else         { if (rsi <= s.rsi_at_last_entry - Traits::RSI_RECOVERY_OFF) s.rsi_recovered = true; }
    }

    double level_lot(double prev_lot, int next_level_index) const {
        double mult;
        if      (next_level_index == 1) mult = Traits::MULT_A;
        else if (next_level_index <= 4) mult = Traits::MULT_B;
        else                            mult = Traits::MULT_C;
        return prev_lot * mult;
    }

    void place_buy(double slow_ma, double atr_short, bool is_base, std::int64_t ts_ms) {
        if (long_.levels_open >= Side::CAPACITY) return;
        const double price   = cached_ask_ > 0.0 ? cached_ask_ : cached_close();
        const double init_sl = slow_ma - (Traits::ENTRY_ATR_MULT_X + Traits::SL_ATR_BUFFER_Y) * atr_short;
        if (init_sl >= price) return;  // geometry safety
        const double lot = is_base ? Traits::BASE_LOT
                                    : level_lot(long_.last_entry_lot, long_.levels_open /*next index*/);
        long_.entry_px[long_.levels_open] = price;
        long_.lot[long_.levels_open]      = lot;
        long_.entry_ts[long_.levels_open] = ts_ms;
        ++long_.levels_open;
        long_.last_entry_price  = price;
        long_.last_entry_ts     = ts_ms;
        long_.last_entry_lot    = lot;
        long_.rsi_at_last_entry = cached_rsi_;
        long_.rsi_recovered     = false;
        if (is_base) {
            long_.current_sl       = init_sl;
            long_.first_entry_ts   = ts_ms;
            long_.first_entry_bar  = bar_count_;
        }
        if constexpr (Traits::VERBOSE_LOG)
            std::printf("[AMR-%s] BUY lvl=%d lot=%.4f px=%.5f sl=%.5f rsi=%.2f atr=%.5f shadow=%d\n",
                        Traits::SYMBOL, long_.levels_open, lot, price, init_sl, cached_rsi_, atr_short, (int)shadow_mode);
    }

    void place_sell(double slow_ma, double atr_short, bool is_base, std::int64_t ts_ms) {
        if (short_.levels_open >= Side::CAPACITY) return;
        const double price   = cached_bid_ > 0.0 ? cached_bid_ : cached_close();
        const double init_sl = slow_ma + (Traits::ENTRY_ATR_MULT_X + Traits::SL_ATR_BUFFER_Y) * atr_short;
        if (init_sl <= price) return;
        const double lot = is_base ? Traits::BASE_LOT
                                    : level_lot(short_.last_entry_lot, short_.levels_open);
        short_.entry_px[short_.levels_open] = price;
        short_.lot[short_.levels_open]      = lot;
        short_.entry_ts[short_.levels_open] = ts_ms;
        ++short_.levels_open;
        short_.last_entry_price  = price;
        short_.last_entry_ts     = ts_ms;
        short_.last_entry_lot    = lot;
        short_.rsi_at_last_entry = cached_rsi_;
        short_.rsi_recovered     = false;
        if (is_base) {
            short_.current_sl      = init_sl;
            short_.first_entry_ts  = ts_ms;
            short_.first_entry_bar = bar_count_;
        }
        if constexpr (Traits::VERBOSE_LOG)
            std::printf("[AMR-%s] SELL lvl=%d lot=%.4f px=%.5f sl=%.5f rsi=%.2f atr=%.5f shadow=%d\n",
                        Traits::SYMBOL, short_.levels_open, lot, price, init_sl, cached_rsi_, atr_short, (int)shadow_mode);
    }

    // ---------- SL trail (ratchet-only, anchored slow EMA) ----------
    void trail_sl(double slow_ma, double atr_short, std::int64_t /*ts_ms*/) {
        const double buf_dist = (Traits::ENTRY_ATR_MULT_X + Traits::SL_ATR_BUFFER_Y) * atr_short;
        if (long_.levels_open > 0) {
            const double new_sl = slow_ma - buf_dist;
            if (new_sl > long_.current_sl) long_.current_sl = new_sl;
        }
        if (short_.levels_open > 0) {
            const double new_sl = slow_ma + buf_dist;
            if (new_sl < short_.current_sl || short_.current_sl == 0.0) short_.current_sl = new_sl;
        }
    }

    // ---------- TP target (WAP-based modes 2/3/4) ----------
    double wap_target(bool is_long) {
        const Side& s = is_long ? long_ : short_;
        if (s.levels_open == 0) return 0.0;
        double wap = 0.0, tot = 0.0;
        compute_wap(s, wap, tot);
        if (wap <= 0.0) return 0.0;

        double tgt = 0.0;
        if constexpr (Traits::TP_METHOD == AmrTpMethod::FIXED_PIPS_FROM_WAP) {
            const double d = Traits::TP_FIXED_PIPS * Traits::POINT;
            tgt = is_long ? (wap + d) : (wap - d);
        } else if constexpr (Traits::TP_METHOD == AmrTpMethod::PCT_FROM_WAP) {
            const double p = Traits::TP_PCT_FROM_WAP / 100.0;
            tgt = is_long ? wap * (1.0 + p) : wap * (1.0 - p);
        } else if constexpr (Traits::TP_METHOD == AmrTpMethod::ATR_FROM_WAP) {
            if (cached_atr_short_ <= 0.0) return 0.0;
            const double d = Traits::TP_ATR_MULT_FROM_WAP * cached_atr_short_;
            tgt = is_long ? (wap + d) : (wap - d);
        } else { return 0.0; }

        if constexpr (Traits::USE_MA_CAP) {
            if (cached_slow_ma_ > 0.0) {
                if (is_long) tgt = std::min(tgt, cached_slow_ma_);
                else         tgt = std::max(tgt, cached_slow_ma_);
            }
        }
        return tgt;
    }

    // ---------- Internal close-all (RSI/MA mode + time exit fallback) ----------
    bool should_close_internal(bool is_long, double slow_ma, double rsi, std::int64_t /*ts_ms*/) {
        const Side& s = is_long ? long_ : short_;

        // Time-based fallback (applies regardless of TP mode if toggled)
        if constexpr (Traits::USE_TIME_EXIT) {
            if (s.first_entry_bar > 0) {
                const int bars_open = bar_count_ - s.first_entry_bar;
                if (bars_open >= Traits::TIME_EXIT_BARS) return true;
            }
        }

        if constexpr (Traits::TP_METHOD == AmrTpMethod::RSI_OR_MA) {
            const double px = is_long ? cached_bid_ : cached_ask_;
            const double rsi_tp_long  = Traits::RSI_TP_LEVEL;
            const double rsi_tp_short = 100.0 - Traits::RSI_TP_LEVEL;
            return is_long
                ? ((rsi >= rsi_tp_long)  || (px > 0.0 && px >= slow_ma))
                : ((rsi <= rsi_tp_short) || (px > 0.0 && px <= slow_ma));
        } else {
            (void)slow_ma; (void)rsi;
            return false;
        }
    }

    // ---------- Close-all (writes shadow CSV records via OmegaTradeLedger) ----------
    void close_all(bool is_long, const char* reason, std::int64_t ts_ms) {
        Side& s = is_long ? long_ : short_;
        if (s.levels_open == 0) return;
        const double exit_px = is_long ? cached_bid_ : cached_ask_;
        if (exit_px <= 0.0) return;

        double total_pnl = 0.0;
        for (int i = 0; i < s.levels_open; ++i) {
            const double diff = is_long ? (exit_px - s.entry_px[i]) : (s.entry_px[i] - exit_px);
            const double pnl  = diff * s.lot[i];
            total_pnl += pnl;
            // Per-leg trade record (shadow audit)
            TradeRecord tr{};
            tr.symbol      = Traits::SYMBOL;
            tr.side        = is_long ? "BUY" : "SELL";
            tr.engine      = "AtrMeanRevGrid";
            tr.entryPrice  = s.entry_px[i];
            tr.exitPrice   = exit_px;
            tr.pnl         = pnl;
            tr.net_pnl     = pnl;          // costs would be deducted here in live deploy
            tr.mfe         = 0.0;
            tr.mae         = 0.0;
            tr.entryTs     = (s.entry_ts[i] / 1000);
            tr.exitTs      = (ts_ms / 1000);
            tr.exitReason  = reason;
            tr.spreadAtEntry = 0.0;
            tr.latencyMs   = 0;
            tr.regime      = "MEAN_REV";
            // Route via injected callback (set in engine_init.hpp to point at
            // write_shadow_csv). Decouples this header from logging.hpp.
            if (on_close_cb) on_close_cb(tr);
        }
        if constexpr (Traits::VERBOSE_LOG)
            std::printf("[AMR-%s] CLOSE-%s reason=%s levels=%d pnl=%.5f\n",
                        Traits::SYMBOL, is_long ? "LONG" : "SHORT", reason, s.levels_open, total_pnl);
        // Reset
        s = Side{};
    }
};

// -----------------------------------------------------------------------------
// Seed helper free function (matches the engine_seed_helpers naming convention).
// -----------------------------------------------------------------------------
template<class Traits>
inline void seed_h1_amr(AtrMeanRevGridEngine<Traits>& eng, const std::string& csv_path) {
    eng.seed_from_h1_csv(csv_path);
}

} // namespace omega
