#pragma once
// =============================================================================
// SweepableEnginesCRTP.hpp -- CRTP-based sweep engines (S51 1A.1.b, post-G4)
// =============================================================================
// PURPOSE
//   Provide CRTP (Curiously Recurring Template Pattern) versions of the 5
//   sweepable engines. Architecturally distinct from SweepableEngines.hpp:
//
//     SweepableEngines.hpp         -- non-type template params; each combo is
//                                     a distinct class instantiation
//                                     (e.g. HBG_T<6.0, 32.0, 0.42, 2.0, 0.25, 0.20>).
//                                     Engine logic lives in the class body.
//
//     SweepableEnginesCRTP.hpp     -- CRTP base + Traits<I> derived. Each
//                                     combo I is a tiny derived struct
//                                     supplying constexpr params via the
//                                     Traits<I> typedef. The base implements
//                                     all engine logic and reads params via
//                                     static dispatch to Derived::Traits.
//                                     One engine logic body, 490 derived
//                                     types per engine.
//
//   Both versions produce equivalent assembly and equivalent output. The CRTP
//   shape is the architecture Jo specified: derived types encode the param
//   set, base provides logic via static_cast<Derived const*>(this) when it
//   needs to inspect derived state. Traits<I> machine-generates 490 distinct
//   derived types from a single template via constexpr index arithmetic.
//
// AUTHORITY
//   Created S51 1A.1.b post-G4 (2026-04-29) under explicit user authorisation
//   ("build the CRTP I asked for"). Coexists with SweepableEngines.hpp which
//   remains the harness backing for the existing OmegaSweepHarness target.
//   This header backs the new OmegaSweepHarnessCRTP target.
//
// DESIGN
//   For each engine X:
//     1. struct X_BaseParams { static constexpr ... } -- the engine's live
//        defaults expressed as a base traits class. Used by Traits<I>::base.
//     2. template <typename Traits> class X_CRTP : public X_Base<X_CRTP<Traits>>
//        -- CRTP-derived. Pulls SL_TICKS, MIN_RANGE, etc. from Traits at
//        compile time via static dispatch. Inherits all logic from X_Base.
//     3. template <typename Derived> class X_Base
//        -- engine logic. Reads params via static_cast<Derived const*>(this)
//        ->traits_t::PARAM (where traits_t is a derived-side typedef).
//     4. template <std::size_t I> struct X_Traits
//        -- generates 490 distinct types by mapping combo index I to the
//        static constexpr param values for that combo via mult_for_param().
//
//   The harness instantiates std::tuple<X_CRTP<X_Traits<0>>, ..., X_CRTP<X_Traits<489>>>
//   and dispatches every tick into all 490 via fold-expression over
//   std::make_index_sequence<490>{} -- identical to SweepableEngines.hpp's
//   call shape, so the harness ports cleanly.
//
// SCOPE
//   Engines:
//     1. AsianRangeCRTP        (live: AsianRangeEngine)
//     2. VWAPStretchCRTP       (live: VWAPStretchReversionEngine)
//     3. DXYDivergenceCRTP     (live: DXYDivergenceEngine)
//     4. HBG_CRTP              (live: GoldHybridBracketEngine)
//     5. EMACrossCRTP          (live: EMACrossEngine)
//
//   Each base is a faithful port of SweepableEngines.hpp's class body. All
//   non-swept constants are kept hardcoded as `static constexpr` in the
//   *Base. All swept params come from the traits class via the CRTP
//   downcast.
//
// SWEEP PARAMETERS (5 per engine, geometric grid 0.5x..2.0x of default,
//                   pairwise 2-factor; 490 combos per engine):
//   AsianRange   : BUFFER, MIN_RANGE, MAX_RANGE, SL_TICKS, TP_TICKS
//   VWAPStretch  : SL_TICKS, TP_TICKS, COOLDOWN_SEC, SIGMA_ENTRY, VOL_WINDOW
//   DXYDivergence: SL_TICKS, TP_TICKS, COOLDOWN_SEC, WINDOW, DIV_THRESHOLD
//   HBG          : MIN_RANGE, MAX_RANGE, SL_FRAC, TP_RR, MFE_LOCK_FRAC
//                  (TRAIL_FRAC fixed at 0.25 -- HBG-FIX-1, see SweepableEngines.hpp)
//   EMACross     : FAST_PERIOD, SLOW_PERIOD, RSI_LO, RSI_HI, SL_MULT
//
// TIME SHIM
//   When OMEGA_BT_SHIM_ACTIVE is defined (set by OmegaTimeShim.hpp), the
//   sweep time helpers route through omega::bt::g_sim_now_ms. When undefined,
//   wall clock fallback. Identical contract to SweepableEngines.hpp.
//
// SINK CONTRACT
//   on_tick takes Sink& by reference. Sink must be callable as
//   sink(const omega::TradeRecord&). HBG_CRTP and EMACrossCRTP emit trade
//   records via the sink. AsianRangeCRTP, VWAPStretchCRTP, DXYDivergenceCRTP
//   emit Signal objects (the harness simulates SL/TP for those, identical
//   to the existing OmegaSweepHarness pattern).
//
// EXTERNAL DEPENDENCIES (provided by includer)
//   * omega::TradeRecord  (include/OmegaTradeLedger.hpp)
//   * int bracket_trend_bias(const char*)  (include/BracketTrendState.hpp)
//   * omega::bt::g_sim_now_ms  (backtest/OmegaTimeShim.hpp -- when active)
//
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <chrono>
#include <array>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <mutex>
#include "OmegaTradeLedger.hpp"
#include "BracketTrendState.hpp"

namespace omega {
namespace sweep_crtp {

// -----------------------------------------------------------------------------
// Local types -- duplicated from omega::sweep so this header is self-contained.
// Identical layout / semantics to SweepableEngines.hpp.
// -----------------------------------------------------------------------------
enum class SessionType { ASIAN, LONDON, NEWYORK, OVERLAP, UNKNOWN };
enum class TradeSide   { LONG, SHORT, NONE };

struct GoldSnapshot {
    double bid=0, ask=0, mid=0, spread=0;
    double vwap=0, volatility=0, trend=0, sweep_size=0, prev_mid=0;
    double dx_mid=0;
    SessionType session = SessionType::UNKNOWN;
    const char* supervisor_regime = "";
    bool is_valid() const { return bid>0 && ask>0 && bid<ask; }
};

struct Signal {
    bool valid=false; TradeSide side=TradeSide::NONE;
    double confidence=0,size=0,entry=0,tp=0,sl=0;
    char reason[32]={}, engine[32]={};
    bool is_long() const { return side==TradeSide::LONG; }
};

// -----------------------------------------------------------------------------
// CircularBuffer / MinMaxCircularBuffer -- minimal duplicates from
// SweepableEngines.hpp. Same implementation, same invariants.
// -----------------------------------------------------------------------------
template<typename T, std::size_t N>
class CircularBuffer {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be power of two");
    std::array<T,N> buf_{}; std::size_t head_=0, count_=0;
    static constexpr std::size_t MASK = N-1;
public:
    void push_back(const T& v) {
        buf_[(head_+count_)&MASK]=v;
        if(count_<N) ++count_; else head_=(head_+1)&MASK;
    }
    std::size_t size()  const { return count_; }
    bool        empty() const { return count_==0; }
    const T& operator[](std::size_t i) const { return buf_[(head_+i)&MASK]; }
    const T& back()  const { return buf_[(head_+count_-1)&MASK]; }
    const T& front() const { return buf_[head_]; }
    void clear() { head_=0; count_=0; }
};

template<typename T, std::size_t N>
class MinMaxCircularBuffer {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be power of two");
    std::array<T,N>            buf_{};
    std::array<std::size_t,2*N> min_idx_{}, max_idx_{};
    std::size_t abs_head_=0,count_=0,min_h_=0,min_t_=0,max_h_=0,max_t_=0;
    static constexpr std::size_t MASK=N-1, IMASK=2*N-1;
public:
    void push_back(const T& v) {
        std::size_t slot=abs_head_&MASK; buf_[slot]=v;
        if(count_==N){ std::size_t ev=(abs_head_-N)&MASK;
            if(min_h_!=min_t_&&min_idx_[min_h_&IMASK]==ev)++min_h_;
            if(max_h_!=max_t_&&max_idx_[max_h_&IMASK]==ev)++max_h_; }
        while(min_h_!=min_t_&&buf_[min_idx_[(min_t_-1)&IMASK]]>=v)--min_t_;
        min_idx_[min_t_++&IMASK]=slot;
        while(max_h_!=max_t_&&buf_[max_idx_[(max_t_-1)&IMASK]]<=v)--max_t_;
        max_idx_[max_t_++&IMASK]=slot;
        ++abs_head_; if(count_<N)++count_;
    }
    std::size_t size() const { return count_; }
    void clear() { count_=0; min_h_=min_t_=max_h_=max_t_=0; abs_head_=0; }
    T min() const { return buf_[min_idx_[min_h_&IMASK]]; }
    T max() const { return buf_[max_idx_[max_h_&IMASK]]; }
    const T& operator[](std::size_t i) const {
        return buf_[(abs_head_-count_+i)&MASK];
    }
};

// -----------------------------------------------------------------------------
// Time helpers -- identical to SweepableEngines.hpp's sweep_now_*.
// -----------------------------------------------------------------------------
inline int64_t sweep_now_sec() noexcept {
#ifdef OMEGA_BT_SHIM_ACTIVE
    return omega::bt::g_sim_now_ms / 1000LL;
#else
    return static_cast<int64_t>(std::time(nullptr));
#endif
}

inline int64_t sweep_now_ms() noexcept {
#ifdef OMEGA_BT_SHIM_ACTIVE
    return omega::bt::g_sim_now_ms;
#else
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

inline void sweep_utc_hms(int& h, int& m, int& yday) noexcept {
    const int64_t s   = sweep_now_sec();
    const int64_t day = s / 86400LL;
    const int64_t sod = s - day * 86400LL;
    h = static_cast<int>(sod / 3600LL);
    m = static_cast<int>((sod / 60LL) % 60LL);

    const int64_t z   = day + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int      y   = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    const unsigned mp  = (5*doy + 2) / 153;
    const unsigned mo  = mp < 10 ? mp + 3 : mp - 9;
    if (mo <= 2) ++y;

    static constexpr int days_before_month[12] =
        {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
    const unsigned d = doy - (153*mp + 2)/5 + 1;
    yday = days_before_month[mo - 1] + static_cast<int>(d) - 1
         + ((leap && mo > 2) ? 1 : 0);
}

inline int sweep_utc_mins() noexcept {
    const int64_t s   = sweep_now_sec();
    const int64_t day = s / 86400LL;
    const int64_t sod = s - day * 86400LL;
    return static_cast<int>(sod / 60LL);
}

// =============================================================================
// Sweep grid -- 7-value geometric grid 0.5x..2.0x. Mirrors OmegaSweepHarness.cpp.
// Lifted into the header so Traits<I> can call it at compile time.
// =============================================================================
inline constexpr double GRID_MULT[7] = {
    0.5, 0.6299605249474366, 0.7937005259840998,
    1.0, 1.2599210498948732, 1.5874010519681994, 2.0
};

inline constexpr int N_PAIRS  = 10;
inline constexpr int PAIR_GRID = 7;
inline constexpr int N_COMBOS = N_PAIRS * PAIR_GRID * PAIR_GRID;  // 490

// Pair (p_a, p_b) for pair index k in [0..9].
constexpr std::pair<int,int> pair_indices_crtp(int k) {
    constexpr std::pair<int,int> P[10] = {
        {0,1}, {0,2}, {0,3}, {0,4},
        {1,2}, {1,3}, {1,4},
        {2,3}, {2,4},
        {3,4}
    };
    return P[k];
}

constexpr int combo_pair_crtp (int I) { return I / 49; }
constexpr int combo_ix_a_crtp (int I) { return (I % 49) / 7; }
constexpr int combo_ix_b_crtp (int I) { return (I % 49) % 7; }

constexpr double mult_for_param_crtp(int I, int p) {
    const int pi = combo_pair_crtp(I);
    const auto pp = pair_indices_crtp(pi);
    if (pp.first  == p) return GRID_MULT[combo_ix_a_crtp(I)];
    if (pp.second == p) return GRID_MULT[combo_ix_b_crtp(I)];
    return 1.0;
}

// =============================================================================
// 1. AsianRangeCRTP -- CRTP port of AsianRangeT
// =============================================================================
//   * AsianRangeBaseParams: live defaults
//   * AsianRangeBase<Derived>: shared logic; reads params via
//     static_cast<Derived const*>(this)->traits_t::PARAM
//   * AsianRangeCRTP<Traits>: derives from Base<AsianRangeCRTP<Traits>>;
//     defines `using traits_t = Traits;` so the base can pull params
//     statically.
//   * AsianRangeTraits<I>: maps combo index I to the 5 constexpr params.
// -----------------------------------------------------------------------------
struct AsianRangeBaseParams {
    static constexpr double BUFFER       = 0.50;
    static constexpr double MIN_RANGE    = 3.0;
    static constexpr double MAX_RANGE    = 50.0;
    static constexpr int    SL_TICKS     = 80;
    static constexpr int    TP_TICKS     = 200;
};

template <std::size_t I>
struct AsianRangeTraits {
    static constexpr double BUFFER    =
        AsianRangeBaseParams::BUFFER    * mult_for_param_crtp(static_cast<int>(I), 0);
    static constexpr double MIN_RANGE =
        AsianRangeBaseParams::MIN_RANGE * mult_for_param_crtp(static_cast<int>(I), 1);
    static constexpr double MAX_RANGE =
        AsianRangeBaseParams::MAX_RANGE * mult_for_param_crtp(static_cast<int>(I), 2);
    static constexpr int    SL_TICKS  = static_cast<int>(
        static_cast<double>(AsianRangeBaseParams::SL_TICKS) *
        mult_for_param_crtp(static_cast<int>(I), 3) + 0.5);
    static constexpr int    TP_TICKS  = static_cast<int>(
        static_cast<double>(AsianRangeBaseParams::TP_TICKS) *
        mult_for_param_crtp(static_cast<int>(I), 4) + 0.5);
};

template <typename Derived>
class AsianRangeBase {
protected:
    static constexpr double MAX_SPREAD   = 2.0;
    static constexpr int    FIRE_START_H = 7;
    static constexpr int    FIRE_END_H   = 11;
    static constexpr int64_t COOLDOWN_S  = 600;

    double  asian_hi_  = 0.0;
    double  asian_lo_  = 1e9;
    int     last_day_  = -1;
    bool    long_fired_  = false;
    bool    short_fired_ = false;
    uint64_t signal_count_ = 0;
    bool    enabled_ = true;
    int64_t last_signal_s_ = -3700;

    // CRTP downcast helpers. Compile-time static dispatch -- no virtual
    // call, no runtime cost. This is the canonical CRTP idiom.
    Derived*       derived()       noexcept { return static_cast<Derived*>(this); }
    Derived const* derived() const noexcept { return static_cast<Derived const*>(this); }

public:
    AsianRangeBase() = default;

    void setEnabled(bool e){ if(e && !enabled_) reset(); enabled_ = e; }
    bool isEnabled() const { return enabled_; }
    uint64_t signal_count() const { return signal_count_; }

    void reset() {
        asian_hi_ = 0.0; asian_lo_ = 1e9;
        last_day_ = -1; long_fired_ = false; short_fired_ = false;
    }

    Signal noSignal() const { return Signal{}; }

    Signal process(const GoldSnapshot& s) {
        // Static-dispatch param reads. Derived::traits_t is the per-combo
        // traits class (e.g. AsianRangeTraits<37>). The compiler resolves
        // these to immediate constants -- no indirection.
        using T = typename Derived::traits_t;

        if (!enabled_ || !s.is_valid())              return noSignal();
        if (s.spread > MAX_SPREAD)                   return noSignal();
        if (s.session == SessionType::UNKNOWN)       return noSignal();

        int h, m, yday;
        sweep_utc_hms(h, m, yday);

        if (yday != last_day_) {
            asian_hi_    = 0.0;
            asian_lo_    = 1e9;
            long_fired_  = false;
            short_fired_ = false;
            last_day_    = yday;
        }

        if (h >= 0 && h < FIRE_START_H) {
            if (s.mid > asian_hi_) asian_hi_ = s.mid;
            if (s.mid < asian_lo_) asian_lo_ = s.mid;
            return noSignal();
        }

        if (h < FIRE_START_H || h >= FIRE_END_H)     return noSignal();

        if (asian_hi_ <= 0.0 || asian_lo_ >= 1e8)    return noSignal();
        const double rng = asian_hi_ - asian_lo_;
        if (rng < T::MIN_RANGE || rng > T::MAX_RANGE) return noSignal();

        const int64_t now_s   = sweep_now_sec();
        const int64_t elapsed = now_s - last_signal_s_;
        if (elapsed < COOLDOWN_S)                    return noSignal();

        Signal sig;
        sig.size = 0.01; sig.sl = T::SL_TICKS; sig.tp = T::TP_TICKS;

        if (!long_fired_ && s.mid > asian_hi_ + T::BUFFER) {
            sig.valid      = true;
            sig.side       = TradeSide::LONG;
            sig.entry      = s.ask;
            sig.confidence = std::min(1.5, (s.mid - asian_hi_) / 2.0 + 0.9);
            std::strncpy(sig.reason, "ASIAN_RANGE_LONG",  31);
            std::strncpy(sig.engine, "AsianRange",        31);
            long_fired_  = true;
        } else if (!short_fired_ && s.mid < asian_lo_ - T::BUFFER) {
            sig.valid      = true;
            sig.side       = TradeSide::SHORT;
            sig.entry      = s.bid;
            sig.confidence = std::min(1.5, (asian_lo_ - s.mid) / 2.0 + 0.9);
            std::strncpy(sig.reason, "ASIAN_RANGE_SHORT", 31);
            std::strncpy(sig.engine, "AsianRange",        31);
            short_fired_ = true;
        }

        if (sig.valid) {
            last_signal_s_ = now_s;
            ++signal_count_;
        }
        (void)derived();  // silence unused-warning when no derived state read
        return sig;
    }
};

template <typename Traits>
class AsianRangeCRTP : public AsianRangeBase<AsianRangeCRTP<Traits>> {
public:
    using traits_t = Traits;
};

// =============================================================================
// 2. VWAPStretchCRTP -- CRTP port of VWAPStretchT
// =============================================================================
struct VWAPStretchBaseParams {
    static constexpr int    SL_TICKS     = 40;
    static constexpr int    TP_TICKS     = 88;
    static constexpr int    COOLDOWN_SEC = 300;
    static constexpr double SIGMA_ENTRY  = 2.0;
    static constexpr int    VOL_WINDOW   = 40;
};

template <std::size_t I>
struct VWAPStretchTraits {
    static constexpr int    SL_TICKS     = static_cast<int>(
        static_cast<double>(VWAPStretchBaseParams::SL_TICKS) *
        mult_for_param_crtp(static_cast<int>(I), 0) + 0.5);
    static constexpr int    TP_TICKS     = static_cast<int>(
        static_cast<double>(VWAPStretchBaseParams::TP_TICKS) *
        mult_for_param_crtp(static_cast<int>(I), 1) + 0.5);
    static constexpr int    COOLDOWN_SEC = static_cast<int>(
        static_cast<double>(VWAPStretchBaseParams::COOLDOWN_SEC) *
        mult_for_param_crtp(static_cast<int>(I), 2) + 0.5);
    static constexpr double SIGMA_ENTRY  =
        VWAPStretchBaseParams::SIGMA_ENTRY *
        mult_for_param_crtp(static_cast<int>(I), 3);
    static constexpr int    VOL_WINDOW   = static_cast<int>(
        static_cast<double>(VWAPStretchBaseParams::VOL_WINDOW) *
        mult_for_param_crtp(static_cast<int>(I), 4) + 0.5);
};

template <typename Derived>
class VWAPStretchBase {
protected:
    static constexpr double MAX_SPREAD = 2.0;
    static constexpr int    VOL_BUF    = 128;  // covers up to 2.0x of max default

    CircularBuffer<double, 64> recent_;
    double price_buf_[VOL_BUF] = {};
    int    pb_head_ = 0, pb_count_ = 0;
    uint64_t signal_count_ = 0;
    bool   enabled_ = true;
    int64_t last_signal_s_ = -100000;

    double rolling_std() const noexcept {
        using T = typename Derived::traits_t;
        if (pb_count_ < 8) return 0.0;
        const int n = (pb_count_ < T::VOL_WINDOW) ? pb_count_ : T::VOL_WINDOW;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += price_buf_[i];
        const double mean = sum / n;
        double sq = 0.0;
        for (int i = 0; i < n; ++i) { double d = price_buf_[i] - mean; sq += d*d; }
        return std::sqrt(sq / (n - 1));
    }

    bool is_decelerating() const noexcept {
        if (recent_.size() < 10) return false;
        const int n = static_cast<int>(recent_.size());
        double fast = 0.0, slow = 0.0;
        for (int i = n-5;  i < n;   ++i) fast += std::fabs(recent_[i] - recent_[i > 0 ? i-1 : 0]);
        for (int i = n-10; i < n-5; ++i) slow += std::fabs(recent_[i] - recent_[i > 0 ? i-1 : 0]);
        return (slow > 0.01) && (fast < slow * 0.65);
    }

    Derived*       derived()       noexcept { return static_cast<Derived*>(this); }
    Derived const* derived() const noexcept { return static_cast<Derived const*>(this); }

public:
    double ewm_drift_ = 0.0;
    double ema9_      = 0.0;
    double ema50_     = 0.0;
    double z_         = 0.0;

    VWAPStretchBase() {
        static_assert(Derived::traits_t::VOL_WINDOW <= VOL_BUF,
                      "VOL_WINDOW exceeds internal buffer; raise VOL_BUF.");
    }

    void set_ewm_drift(double d) { ewm_drift_ = d; }
    void set_ema_trend(double e9, double e50) { ema9_ = e9; ema50_ = e50; }
    void setEnabled(bool e){ enabled_ = e; }
    bool isEnabled() const { return enabled_; }
    uint64_t signal_count() const { return signal_count_; }

    void reset() {
        recent_.clear();
        pb_head_ = pb_count_ = 0;
    }

    Signal noSignal() const { return Signal{}; }

    Signal process(const GoldSnapshot& s) {
        using T = typename Derived::traits_t;

        if (!enabled_ || !s.is_valid())          return noSignal();
        if (s.spread > MAX_SPREAD)               return noSignal();
        if (s.session == SessionType::UNKNOWN)   return noSignal();
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }

        const int mins = sweep_utc_mins();
        if (mins < 780 || mins > 1020) return noSignal();

        if (std::fabs(ewm_drift_) > 2.0) return noSignal();

        if (ema9_ > 0.0 && ema50_ > 0.0) {
            const bool bullish_trend = (ema9_ > ema50_);
            const bool wants_short   = (s.mid > s.vwap);
            if (bullish_trend && wants_short)   return noSignal();
            if (!bullish_trend && !wants_short) return noSignal();
        }

        if (s.vwap < 1.0) return noSignal();

        recent_.push_back(s.mid);
        price_buf_[pb_head_ % T::VOL_WINDOW] = s.mid;
        ++pb_head_;
        if (pb_count_ < T::VOL_WINDOW) ++pb_count_;

        const double sigma = rolling_std();
        if (sigma < 0.5) return noSignal();

        const double z = (s.mid - s.vwap) / sigma;
        z_ = z;
        if (std::fabs(z) < T::SIGMA_ENTRY) return noSignal();

        if (!is_decelerating()) return noSignal();

        const int64_t now_s = sweep_now_sec();
        if (now_s - last_signal_s_ < T::COOLDOWN_SEC) return noSignal();

        const int bt_bias = bracket_trend_bias("XAUUSD");
        if (z >  T::SIGMA_ENTRY && bt_bias == 1)  return noSignal();
        if (z < -T::SIGMA_ENTRY && bt_bias == -1) return noSignal();

        Signal sig;
        sig.size = 0.01; sig.sl = T::SL_TICKS; sig.tp = T::TP_TICKS;
        sig.confidence = std::min(1.4, 0.80 + std::fabs(z) * 0.10);

        if (z > T::SIGMA_ENTRY) {
            sig.valid = true; sig.side = TradeSide::SHORT; sig.entry = s.bid;
            std::strncpy(sig.reason, "VWAP_STRETCH_SHORT", 31);
        } else {
            sig.valid = true; sig.side = TradeSide::LONG; sig.entry = s.ask;
            std::strncpy(sig.reason, "VWAP_STRETCH_LONG", 31);
        }
        std::strncpy(sig.engine, "VWAPStretchReversion", 31);

        last_signal_s_ = now_s;
        ++signal_count_;
        (void)derived();
        return sig;
    }
};

template <typename Traits>
class VWAPStretchCRTP : public VWAPStretchBase<VWAPStretchCRTP<Traits>> {
public:
    using traits_t = Traits;
};

// =============================================================================
// 3. DXYDivergenceCRTP -- CRTP port of DXYDivergenceT
// =============================================================================
struct DXYDivergenceBaseParams {
    static constexpr int    SL_TICKS      = 60;
    static constexpr int    TP_TICKS      = 120;
    static constexpr int    COOLDOWN_SEC  = 3600;
    static constexpr int    WINDOW        = 20;
    static constexpr double DIV_THRESHOLD = 2.50;
};

template <std::size_t I>
struct DXYDivergenceTraits {
    static constexpr int    SL_TICKS      = static_cast<int>(
        static_cast<double>(DXYDivergenceBaseParams::SL_TICKS) *
        mult_for_param_crtp(static_cast<int>(I), 0) + 0.5);
    static constexpr int    TP_TICKS      = static_cast<int>(
        static_cast<double>(DXYDivergenceBaseParams::TP_TICKS) *
        mult_for_param_crtp(static_cast<int>(I), 1) + 0.5);
    static constexpr int    COOLDOWN_SEC  = static_cast<int>(
        static_cast<double>(DXYDivergenceBaseParams::COOLDOWN_SEC) *
        mult_for_param_crtp(static_cast<int>(I), 2) + 0.5);
    static constexpr int    WINDOW        = static_cast<int>(
        static_cast<double>(DXYDivergenceBaseParams::WINDOW) *
        mult_for_param_crtp(static_cast<int>(I), 3) + 0.5);
    static constexpr double DIV_THRESHOLD =
        DXYDivergenceBaseParams::DIV_THRESHOLD *
        mult_for_param_crtp(static_cast<int>(I), 4);
};

template <typename Derived>
class DXYDivergenceBase {
protected:
    static constexpr double MAX_SPREAD    = 2.5;
    static constexpr double GOLD_DX_BETA  = 12.0;
    static constexpr double MIN_DX_MOVE   = 0.05;

    MinMaxCircularBuffer<double, 64> gold_hist_;
    MinMaxCircularBuffer<double, 64> dx_hist_;
    int64_t last_signal_ts_ = 0;
    uint64_t signal_count_ = 0;
    bool enabled_ = true;

    Derived*       derived()       noexcept { return static_cast<Derived*>(this); }
    Derived const* derived() const noexcept { return static_cast<Derived const*>(this); }

public:
    DXYDivergenceBase() {
        static_assert(Derived::traits_t::WINDOW < 64,
                      "WINDOW must fit MinMaxCircularBuffer<,64>");
    }

    void setEnabled(bool e){ enabled_ = e; }
    bool isEnabled() const { return enabled_; }
    uint64_t signal_count() const { return signal_count_; }

    void reset() {
        gold_hist_.clear();
        dx_hist_.clear();
        last_signal_ts_ = 0;
    }

    Signal noSignal() const { return Signal{}; }

    Signal process(const GoldSnapshot& s) {
        using T = typename Derived::traits_t;

        if (!enabled_ || !s.is_valid())              return noSignal();
        if (s.spread > MAX_SPREAD)                   return noSignal();
        if (s.session == SessionType::UNKNOWN)       return noSignal();
        if (s.dx_mid <= 0.0)                         return noSignal();

        if (s.supervisor_regime != nullptr) {
            const bool trending = (std::strcmp(s.supervisor_regime, "EXPANSION_BREAKOUT") == 0
                                || std::strcmp(s.supervisor_regime, "TREND_CONTINUATION") == 0);
            if (trending) return noSignal();
        }

        gold_hist_.push_back(s.mid);
        dx_hist_.push_back(s.dx_mid);
        if (static_cast<int>(gold_hist_.size()) < T::WINDOW + 1) return noSignal();

        const int64_t now_s = sweep_now_sec();
        if (now_s - last_signal_ts_ < T::COOLDOWN_SEC) return noSignal();

        const double gold_move =
            s.mid    - gold_hist_[gold_hist_.size() - T::WINDOW];
        const double dx_move   =
            s.dx_mid - dx_hist_  [dx_hist_.size()   - T::WINDOW];

        if (std::fabs(dx_move) < MIN_DX_MOVE) return noSignal();

        const double expected_gold = -dx_move * GOLD_DX_BETA;
        const double divergence    = gold_move - expected_gold;

        if (std::fabs(divergence) < T::DIV_THRESHOLD) return noSignal();

        if (s.vwap > 0.0) {
            if (divergence > 0 && s.mid < s.vwap * 0.998) return noSignal();
            if (divergence < 0 && s.mid > s.vwap * 1.002) return noSignal();
        }

        last_signal_ts_ = now_s;
        ++signal_count_;

        Signal sig;
        sig.valid      = true;
        sig.side       = (divergence > 0) ? TradeSide::LONG : TradeSide::SHORT;
        sig.confidence = std::min(1.5, std::fabs(divergence) / (T::DIV_THRESHOLD * 2.0));
        sig.size       = 0.01;
        sig.entry      = (divergence > 0) ? s.ask : s.bid;
        sig.tp         = T::TP_TICKS;
        sig.sl         = T::SL_TICKS;
        std::strncpy(sig.reason, divergence > 0 ? "DXY_DIV_LONG" : "DXY_DIV_SHORT", 31);
        std::strncpy(sig.engine, "DXYDivergence", 31);
        (void)derived();
        return sig;
    }
};

template <typename Traits>
class DXYDivergenceCRTP : public DXYDivergenceBase<DXYDivergenceCRTP<Traits>> {
public:
    using traits_t = Traits;
};

// =============================================================================
// 4. HBG_CRTP -- CRTP port of HBG_T (GoldHybridBracketEngine)
// =============================================================================
//   Self-managed engine: tracks position, SL, TP, trail, BE internally.
//   Emits TradeRecord via templated Sink callback. Receives now_ms from
//   harness (driven by tick timestamps).
//
//   HBG-FIX-1 lineage: TRAIL_FRAC fixed at live default (0.25); slot 4 in
//   the sweep is MFE_LOCK_FRAC (centred 0.20). See SweepableEngines.hpp
//   line 691-697 for full rationale.
// -----------------------------------------------------------------------------
struct HBGBaseParams {
    static constexpr double MIN_RANGE     = 6.0;
    static constexpr double MAX_RANGE     = 32.0;   // D6 rebase from 25.0
    static constexpr double SL_FRAC       = 0.42;   // D6 rebase from 0.5
    static constexpr double TP_RR         = 2.0;
    static constexpr double TRAIL_FRAC    = 0.25;   // FIXED -- HBG-FIX-1
    static constexpr double MFE_LOCK_FRAC = 0.20;
};

template <std::size_t I>
struct HBGTraits {
    static constexpr double MIN_RANGE     =
        HBGBaseParams::MIN_RANGE     * mult_for_param_crtp(static_cast<int>(I), 0);
    static constexpr double MAX_RANGE     =
        HBGBaseParams::MAX_RANGE     * mult_for_param_crtp(static_cast<int>(I), 1);
    static constexpr double SL_FRAC       =
        HBGBaseParams::SL_FRAC       * mult_for_param_crtp(static_cast<int>(I), 2);
    static constexpr double TP_RR         =
        HBGBaseParams::TP_RR         * mult_for_param_crtp(static_cast<int>(I), 3);
    static constexpr double TRAIL_FRAC    = HBGBaseParams::TRAIL_FRAC;  // FIXED
    static constexpr double MFE_LOCK_FRAC =
        HBGBaseParams::MFE_LOCK_FRAC * mult_for_param_crtp(static_cast<int>(I), 4);
};

template <typename Derived>
class HBGBase {
public:
    // Non-swept constants -- identical to live values.
    static constexpr int    STRUCTURE_LOOKBACK   = 20;
    static constexpr int    MIN_ENTRY_TICKS      = 15;
    static constexpr int    MIN_BREAK_TICKS      = 3;
    static constexpr double SL_BUFFER            = 0.5;
    static constexpr double MIN_TRAIL_ARM_PTS    = 1.5;
    static constexpr int    MIN_TRAIL_ARM_SECS   = 15;
    static constexpr double MAX_SPREAD           = 2.5;
    static constexpr double RISK_DOLLARS         = 30.0;
    static constexpr double RISK_DOLLARS_PYRAMID = 10.0;
    static constexpr double USD_PER_PT           = 100.0;
    static constexpr int    PENDING_TIMEOUT_S    = 30;
    static constexpr int    COOLDOWN_S           = 60;
    static constexpr int    DIR_SL_COOLDOWN_S    = 120;
    static constexpr double DOM_SLOPE_CONFIRM    = 0.15;
    static constexpr double DOM_LOT_BONUS        = 1.3;
    static constexpr double DOM_WALL_PENALTY     = 0.5;
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;
    static constexpr std::size_t WINDOW_CAP       = 64;

    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    bool shadow_mode = true;

    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = 0.01;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts = 0;
    } pos;

    double bracket_high      = 0.0;
    double bracket_low       = 0.0;
    double range             = 0.0;
    double pending_lot       = 0.01;
    double pending_lot_long  = 0.01;
    double pending_lot_short = 0.01;

    bool has_open_position() const noexcept { return pos.active; }

protected:
    Derived*       derived()       noexcept { return static_cast<Derived*>(this); }
    Derived const* derived() const noexcept { return static_cast<Derived const*>(this); }

public:
    template <typename Sink>
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 bool flow_live,
                 bool flow_be_locked,
                 int  flow_trail_stage,
                 Sink& on_close,
                 double book_slope  = 0.0,
                 bool   vacuum_ask  = false,
                 bool   vacuum_bid  = false,
                 bool   wall_above  = false,
                 bool   wall_below  = false,
                 bool   l2_real     = false) noexcept
    {
        using T = typename Derived::traits_t;

        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        m_last_tick_s = now_s;

        ++m_ticks_received;
        m_window.push_back(mid);

        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) phase = Phase::IDLE;
            else return;
        }

        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, now_s, on_close);
            return;
        }

        if (phase == Phase::PENDING) {
            if (ask >= bracket_high) { confirm_fill(true,  bracket_high, pending_lot_long,  spread); return; }
            if (bid <= bracket_low)  { confirm_fill(false, bracket_low,  pending_lot_short, spread); return; }
            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                phase = Phase::IDLE;
            }
            return;
        }

        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if (static_cast<int>(m_window.size()) < STRUCTURE_LOOKBACK) return;

        if (!can_enter) {
            if (phase == Phase::ARMED) return;
            return;
        }
        if (spread > MAX_SPREAD) return;

        const bool flow_pyramid_ok = flow_live && flow_be_locked && flow_trail_stage >= 1;
        if (flow_live && !flow_pyramid_ok && phase == Phase::IDLE) return;

        const double w_hi = m_window.max();
        const double w_lo = m_window.min();
        range = w_hi - w_lo;

        if (phase == Phase::IDLE) {
            if (range >= T::MIN_RANGE && range <= T::MAX_RANGE) {
                phase        = Phase::ARMED;
                bracket_high = w_hi;
                bracket_low  = w_lo;
                m_inside_ticks = 0;
                m_armed_ts   = now_s;
            }
            return;
        }

        if (phase == Phase::ARMED) {
            bracket_high = std::max(bracket_high, w_hi);
            bracket_low  = std::min(bracket_low,  w_lo);
            range        = bracket_high - bracket_low;
            if (range > T::MAX_RANGE) { phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return; }
            if (range < T::MIN_RANGE || range > T::MAX_RANGE) { phase = Phase::IDLE; return; }

            if (mid >= bracket_low && mid <= bracket_high) {
                ++m_inside_ticks;
            } else {
                m_inside_ticks = 0;
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
                return;
            }
            if (m_inside_ticks < MIN_BREAK_TICKS) return;

            const double sl_dist = range * T::SL_FRAC + SL_BUFFER;
            const double tp_dist = sl_dist * T::TP_RR;
            const double min_tp  = spread * 2.0 + 0.12;
            if (tp_dist < min_tp) { phase = Phase::IDLE; return; }

            if (m_range_history_count >= EXPANSION_MIN_HISTORY) {
                std::array<double, EXPANSION_HISTORY_LEN> sorted{};
                for (int i = 0; i < m_range_history_count; ++i) sorted[i] = m_range_history[i];
                std::sort(sorted.begin(), sorted.begin() + m_range_history_count);
                const int n = m_range_history_count;
                const double median = (n % 2 == 1)
                    ? sorted[n / 2]
                    : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
                const double threshold = median * EXPANSION_MULT;
                if (range < threshold) {
                    phase = Phase::IDLE;
                    bracket_high = bracket_low = 0.0;
                    return;
                }
            }
            if (m_range_history_count < EXPANSION_HISTORY_LEN) {
                m_range_history[m_range_history_count++] = range;
            } else {
                for (int i = 0; i < EXPANSION_HISTORY_LEN - 1; ++i) {
                    m_range_history[i] = m_range_history[i + 1];
                }
                m_range_history[EXPANSION_HISTORY_LEN - 1] = range;
            }

            const bool is_pyramid = flow_pyramid_ok;
            const double risk     = is_pyramid ? RISK_DOLLARS_PYRAMID : RISK_DOLLARS;
            const double base_lot = std::max(0.01, std::min(0.01, risk / (sl_dist * USD_PER_PT)));

            double lot_long  = base_lot;
            double lot_short = base_lot;

            if (l2_real) {
                if (wall_above && wall_below) { phase = Phase::IDLE; return; }
                const bool slope_long  = (book_slope >  DOM_SLOPE_CONFIRM);
                const bool slope_short = (book_slope < -DOM_SLOPE_CONFIRM);
                if (slope_long  || vacuum_ask) lot_long  = std::min(0.01, lot_long  * DOM_LOT_BONUS);
                if (slope_short || vacuum_bid) lot_short = std::min(0.01, lot_short * DOM_LOT_BONUS);
                if (wall_above) lot_long  = std::max(0.01, lot_long  * DOM_WALL_PENALTY);
                if (wall_below) lot_short = std::max(0.01, lot_short * DOM_WALL_PENALTY);
            }

            pending_lot       = base_lot;
            pending_lot_long  = lot_long;
            pending_lot_short = lot_short;
            phase             = Phase::PENDING;
            m_armed_ts        = now_s;
            m_pending_blocked_since = 0;
        }
        (void)derived();
    }

    void confirm_fill(bool is_long, double fill_px, double fill_lot,
                      double spread_at_fill = 0.0) noexcept {
        using T = typename Derived::traits_t;
        const double sl_dist = range * T::SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * T::TP_RR;
        pos.active          = true;
        pos.is_long         = is_long;
        pos.entry           = fill_px;
        pos.sl              = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp              = is_long ? (fill_px + tp_dist) : (fill_px - tp_dist);
        pos.size            = fill_lot;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;
        pos.spread_at_entry = spread_at_fill;
        pos.entry_ts        = m_last_tick_s;
        phase               = Phase::LIVE;
    }

    template <typename Sink>
    void manage(double bid, double ask, double mid,
                int64_t now_s, Sink& on_close) noexcept
    {
        using T = typename Derived::traits_t;
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        const int64_t held_s     = now_s - pos.entry_ts;
        const bool    arm_mfe_ok = (MIN_TRAIL_ARM_PTS  <= 0.0) || (pos.mfe >= MIN_TRAIL_ARM_PTS);
        const bool    arm_hold_ok = (MIN_TRAIL_ARM_SECS <= 0 ) || (held_s  >= MIN_TRAIL_ARM_SECS);
        if (move > 0 && arm_mfe_ok && arm_hold_ok) {
            const double mfe_trail   = pos.mfe * T::MFE_LOCK_FRAC;
            const double range_trail = range   * T::TRAIL_FRAC;
            const double trail_dist = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
            const double trail_sl = pos.is_long ? (pos.entry + pos.mfe - trail_dist)
                                                : (pos.entry - pos.mfe + trail_dist);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) {
            _close(pos.tp, "TP_HIT", now_s, on_close);
            return;
        }

        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            const bool sl_at_be        = (pos.sl <= pos.entry + 0.01)
                                      && (pos.sl >= pos.entry - 0.01);
            const bool trail_in_profit = pos.is_long
                ? (pos.sl > pos.entry + 0.01)
                : (pos.sl < pos.entry - 0.01);
            const char* reason;
            if      (sl_at_be)        reason = "BE_HIT";
            else if (trail_in_profit) reason = "TRAIL_HIT";
            else                      reason = "SL_HIT";
            _close(exit_px, reason, now_s, on_close);
        }
    }

    template <typename Sink>
    void force_close(double bid, double ask, int64_t now_ms, Sink& on_close) noexcept {
        if (!pos.active) return;
        _close(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms / 1000, on_close);
    }

protected:
    int     m_ticks_received  = 0;
    int     m_inside_ticks    = 0;
    int64_t m_armed_ts        = 0;
    int64_t m_cooldown_start  = 0;
    int     m_sl_cooldown_dir = 0;
    int64_t m_sl_cooldown_ts  = 0;
    int64_t m_pending_blocked_since = 0;
    int     m_trade_id        = 0;
    int64_t m_last_tick_s     = 0;
    MinMaxCircularBuffer<double, WINDOW_CAP> m_window;
    std::array<double, EXPANSION_HISTORY_LEN> m_range_history{};
    int     m_range_history_count = 0;

    template <typename Sink>
    void _close(double exit_px, const char* reason,
                int64_t now_s, Sink& on_close) noexcept
    {
        if (std::strcmp(reason, "SL_HIT") == 0) {
            m_sl_cooldown_dir = pos.is_long ? 1 : -1;
            m_sl_cooldown_ts  = now_s + DIR_SL_COOLDOWN_S;
        }

        omega::TradeRecord tr;
        tr.id = ++m_trade_id; tr.symbol = "XAUUSD";
        tr.side = pos.is_long ? "LONG" : "SHORT";
        tr.engine = "HybridBracketGold"; tr.regime = "COMPRESSION";
        tr.entryPrice = pos.entry; tr.exitPrice = exit_px;
        tr.tp = pos.tp; tr.sl = pos.sl;
        tr.size = pos.size;
        tr.pnl = (pos.is_long ? (exit_px - pos.entry)
                              : (pos.entry - exit_px)) * pos.size;
        tr.net_pnl = tr.pnl;
        tr.mfe = pos.mfe * pos.size;
        tr.mae = pos.mae * pos.size;
        tr.entryTs = pos.entry_ts; tr.exitTs = now_s;
        tr.exitReason = reason; tr.spreadAtEntry = pos.spread_at_entry;
        tr.bracket_hi = bracket_high; tr.bracket_lo = bracket_low;
        tr.shadow = shadow_mode;

        pos = LivePos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start = now_s;
        bracket_high = bracket_low = range = 0.0;

        on_close(tr);
    }
};

template <typename Traits>
class HBG_CRTP : public HBGBase<HBG_CRTP<Traits>> {
public:
    using traits_t = Traits;
};

// =============================================================================
// 5. EMACrossCRTP -- CRTP port of EMACrossT
// =============================================================================
//   Bar-driven engine: on_bar() fires on minute close (harness drives via
//   BtBarEngine), on_tick() fires every tick. ECE_CULLED is FALSE here --
//   the harness lifts the cull purely for measurement; the live binary is
//   unaffected (see SweepableEngines.hpp line 1105-1110).
//
//   Degenerate-grid filter: 74/490 (15.1%) combos have FAST >= SLOW which
//   is structurally invalid. The if-constexpr guard emits zero code for
//   non-degenerate instances; the harness marks them in CSV output and
//   excludes from top-50 ranking.
// -----------------------------------------------------------------------------
struct EMACrossBaseParams {
    static constexpr int    FAST_PERIOD = 9;
    static constexpr int    SLOW_PERIOD = 15;
    static constexpr double RSI_LO      = 40.0;
    static constexpr double RSI_HI      = 50.0;
    static constexpr double SL_MULT     = 1.5;
};

template <std::size_t I>
struct EMACrossTraits {
    static constexpr int    FAST_PERIOD = static_cast<int>(
        static_cast<double>(EMACrossBaseParams::FAST_PERIOD) *
        mult_for_param_crtp(static_cast<int>(I), 0) + 0.5);
    static constexpr int    SLOW_PERIOD = static_cast<int>(
        static_cast<double>(EMACrossBaseParams::SLOW_PERIOD) *
        mult_for_param_crtp(static_cast<int>(I), 1) + 0.5);
    static constexpr double RSI_LO      =
        EMACrossBaseParams::RSI_LO  * mult_for_param_crtp(static_cast<int>(I), 2);
    static constexpr double RSI_HI      =
        EMACrossBaseParams::RSI_HI  * mult_for_param_crtp(static_cast<int>(I), 3);
    static constexpr double SL_MULT     =
        EMACrossBaseParams::SL_MULT * mult_for_param_crtp(static_cast<int>(I), 4);
};

template <typename Derived>
class EMACrossBase {
public:
    static constexpr double  TP_RR              = 1.0;
    static constexpr int64_t MAX_LAG_MS         = 60000;
    static constexpr int64_t TIMEOUT_MS         = 180000;
    static constexpr int64_t COOLDOWN_MS        = 20000;
    static constexpr double  GAP_BLOCK          = 0.30;
    static constexpr double  RISK_DOLLARS       = 30.0;
    static constexpr double  MIN_LOT            = 0.01;
    static constexpr double  MAX_LOT            = 0.01;
    static constexpr int64_t STARTUP_MS         = 120000;
    static constexpr double  BE_COMMISSION_PTS  = 0.06;
    static constexpr double  BE_SAFETY_PTS      = 0.05;
    static constexpr double  BE_MIN_BUFFER_PTS  = 0.20;
    static constexpr double  BE_MAX_BUFFER_PTS  = 0.80;

    bool shadow_mode = true;

    struct OpenPos {
        bool   active    = false;
        bool   is_long   = false;
        bool   be_done   = false;
        double entry     = 0.0;
        double sl        = 0.0;
        double tp        = 0.0;
        double size      = 0.0;
        double mfe       = 0.0;
        double atr       = 0.0;
        double spread_at_entry = 0.0;
        int64_t ets      = 0;
        int    trade_id  = 0;
    } pos;

    bool has_open_position() const noexcept { return pos.active; }

protected:
    Derived*       derived()       noexcept { return static_cast<Derived*>(this); }
    Derived const* derived() const noexcept { return static_cast<Derived const*>(this); }

public:
    static bool rsi_allowed(double rsi, bool is_long) noexcept {
        using T = typename Derived::traits_t;
        if (rsi >= 30.0 && rsi < 35.0) return false;
        if (rsi >= 35.0 && rsi < 40.0) return false;
        if (rsi >= 50.0 && rsi < 55.0) return false;
        if (rsi >= 65.0 && rsi < 70.0) return false;
        if (is_long  && (rsi < T::RSI_LO || rsi > 75.0)) return false;
        if (!is_long && (rsi > T::RSI_HI || rsi < 25.0)) return false;
        return true;
    }

    static bool hour_allowed(int64_t ms) noexcept {
        int h = static_cast<int>(((ms/1000LL)%86400LL)/3600LL);
        return !(h==5 || h==12 || h==13 || h==14 || h==16 || h==17 || h==18 || h==19 || h==20);
    }

    void on_bar(double bar_close, double bar_atr, double bar_rsi, int64_t now_ms) noexcept {
        using T = typename Derived::traits_t;
        if constexpr (T::FAST_PERIOD >= T::SLOW_PERIOD) return;

        if (bar_atr > 0.5 && bar_atr < 50.0) _atr = bar_atr;
        _rsi = bar_rsi;

        const double prev_fast = _ema_fast;
        const double prev_slow = _ema_slow;
        _update_ema(_ema_fast, _ema_fast_alpha, bar_close, _fast_warmed, _fast_count, T::FAST_PERIOD);
        _update_ema(_ema_slow, _ema_slow_alpha, bar_close, _slow_warmed, _slow_count, T::SLOW_PERIOD);

        if (!_fast_warmed || !_slow_warmed) { (void)now_ms; return; }

        const bool long_cross  = (_ema_fast > _ema_slow) && (prev_fast <= prev_slow);
        const bool short_cross = (_ema_fast < _ema_slow) && (prev_fast >= prev_slow);

        if (long_cross)  { _cross_dir = 1;  _cross_ms = now_ms; }
        if (short_cross) { _cross_dir = -1; _cross_ms = now_ms; }

        ++_bars_total;
    }

    template <typename Sink>
    void on_tick(double bid, double ask, int64_t now_ms,
                 Sink& on_close) noexcept
    {
        using T = typename Derived::traits_t;
        if constexpr (T::FAST_PERIOD >= T::SLOW_PERIOD) return;

        if (_startup_ms == 0) _startup_ms = now_ms;
        if (now_ms - _startup_ms < STARTUP_MS) return;

        _daily_reset(now_ms);
        if (_daily_pnl <= -200.0) return;

        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        if (pos.active) {
            const double mv = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (mv > pos.mfe) pos.mfe = mv;

            const double td = std::fabs(pos.tp - pos.entry);
            if (!pos.be_done && td > 0 && pos.mfe >= td * 0.50) {
                const double live_sp = spread;
                const double use_sp  = std::max(pos.spread_at_entry, live_sp);
                double buffer = use_sp + BE_COMMISSION_PTS + BE_SAFETY_PTS;
                if (buffer < BE_MIN_BUFFER_PTS) buffer = BE_MIN_BUFFER_PTS;
                if (buffer > BE_MAX_BUFFER_PTS) buffer = BE_MAX_BUFFER_PTS;

                const double max_safe_buffer = pos.mfe - live_sp * 0.5 - BE_SAFETY_PTS;
                if (buffer <= max_safe_buffer && max_safe_buffer > 0.0) {
                    pos.sl = pos.is_long ? (pos.entry + buffer)
                                         : (pos.entry - buffer);
                    pos.be_done = true;
                }
            }

            const double ep = pos.is_long ? bid : ask;

            if (pos.is_long ? (bid >= pos.tp) : (ask <= pos.tp)) {
                _close(ep, "TP", now_ms, on_close); return;
            }
            if (pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl)) {
                _close(ep, pos.be_done ? "BE" : "SL", now_ms, on_close); return;
            }
            if (now_ms - pos.ets > TIMEOUT_MS) {
                _close(ep, "TIMEOUT", now_ms, on_close); return;
            }
            return;
        }

        if (!_fast_warmed || !_slow_warmed) return;
        if (_cross_dir == 0) return;
        if (_atr <= 0.0) return;
        if (now_ms - _last_exit_ms < COOLDOWN_MS) return;
        if (now_ms < _sl_kill_until) return;
        if (spread > _atr * 0.30) return;
        if (!hour_allowed(now_ms)) return;
        if (std::fabs(_ema_fast - _ema_slow) > GAP_BLOCK) return;
        if (now_ms - _cross_ms > MAX_LAG_MS) { _cross_dir = 0; return; }

        const bool isl = (_cross_dir == 1);

        if (!rsi_allowed(_rsi, isl)) return;

        const double sl_dist = _atr * T::SL_MULT;
        const double tp_dist = sl_dist * TP_RR;
        const double cost    = spread + 0.20;
        if (tp_dist <= cost) return;

        const int bias = bracket_trend_bias("XAUUSD");
        if ((isl  && bias == -1) || (!isl && bias == 1)) return;

        _enter(isl, bid, ask, spread, sl_dist, tp_dist, now_ms, on_close);
    }

    template <typename Sink>
    void force_close(double bid, double ask, int64_t now_ms, Sink& cb) noexcept {
        using T = typename Derived::traits_t;
        if constexpr (T::FAST_PERIOD >= T::SLOW_PERIOD) return;

        std::lock_guard<std::mutex> lk(_close_mtx);
        if (!pos.active) return;
        _close_locked(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

protected:
    double _ema_fast       = 0.0;
    double _ema_slow       = 0.0;
    double _ema_fast_alpha = 2.0 / (Derived::traits_t::FAST_PERIOD + 1.0);
    double _ema_slow_alpha = 2.0 / (Derived::traits_t::SLOW_PERIOD + 1.0);
    bool   _fast_warmed    = false;
    bool   _slow_warmed    = false;
    int    _fast_count     = 0;
    int    _slow_count     = 0;

    double  _atr           = 0.0;
    double  _rsi           = 50.0;
    int     _cross_dir     = 0;
    int64_t _cross_ms      = 0;
    int64_t _bars_total    = 0;

    double  _daily_pnl     = 0.0;
    int64_t _daily_day     = 0;
    int64_t _last_exit_ms  = 0;
    int64_t _startup_ms    = 0;
    int     _total_trades  = 0;
    int     _total_wins    = 0;
    int     _trade_id      = 0;
    int     _consec_sl     = 0;
    int64_t _sl_kill_until = 0;

    mutable std::mutex _close_mtx;

    static void _update_ema(double& ema, double alpha, double price,
                             bool& warmed, int& count, int period) noexcept {
        if (!warmed) {
            ema = (ema * count + price) / (count + 1);
            ++count;
            if (count >= period) warmed = true;
        } else {
            ema = price * alpha + ema * (1.0 - alpha);
        }
    }

    template <typename Sink>
    void _enter(bool is_long, double bid, double ask, double spread,
                double sl_dist, double tp_dist,
                int64_t now_ms, Sink& on_close) noexcept
    {
        const double entry_px = is_long ? ask : bid;
        const double sl_px    = is_long ? (entry_px - sl_dist) : (entry_px + sl_dist);
        const double tp_px    = is_long ? (entry_px + tp_dist) : (entry_px - tp_dist);

        const double sl_safe = std::max(0.10, sl_dist);
        double size = RISK_DOLLARS / (sl_safe * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(MIN_LOT, std::min(MAX_LOT, size));

        ++_trade_id;

        pos.active   = true;
        pos.is_long  = is_long;
        pos.entry    = entry_px;
        pos.sl       = sl_px;
        pos.tp       = tp_px;
        pos.size     = size;
        pos.mfe      = 0.0;
        pos.be_done  = false;
        pos.atr      = _atr;
        pos.spread_at_entry = spread;
        pos.ets      = now_ms;
        pos.trade_id = _trade_id;

        _cross_dir = 0;
        (void)bid; (void)ask; (void)on_close;
    }

    template <typename Sink>
    void _close(double exit_px, const char* reason,
                int64_t now_ms, Sink& on_close) noexcept
    {
        std::lock_guard<std::mutex> lk(_close_mtx);
        if (!pos.active) return;
        _close_locked(exit_px, reason, now_ms, on_close);
    }

    template <typename Sink>
    void _close_locked(double exit_px, const char* reason,
                       int64_t now_ms, Sink& on_close) noexcept
    {
        const double pnl_pts = pos.is_long
            ? (exit_px - pos.entry) : (pos.entry - exit_px);
        const double pnl_usd = pnl_pts * pos.size * 100.0;

        _daily_pnl   += pnl_usd;
        _last_exit_ms = now_ms;
        ++_total_trades;

        const bool win = (pnl_usd > 0);
        if (win) ++_total_wins;

        if (std::strcmp(reason, "SL") == 0) {
            if (++_consec_sl >= 3) {
                _sl_kill_until = now_ms + 1800000LL;
            }
        } else {
            _consec_sl = 0;
        }

        omega::TradeRecord tr;
        tr.id         = pos.trade_id;
        tr.symbol     = "XAUUSD";
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.engine     = "EMACross";
        tr.entryPrice = pos.entry;
        tr.exitPrice  = exit_px;
        tr.sl         = pos.sl;
        tr.size       = pos.size;
        tr.pnl        = pnl_pts * pos.size;
        tr.mfe        = pos.mfe;
        tr.mae        = 0.0;
        tr.entryTs    = pos.ets / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "EMA_CROSS";
        tr.l2_live    = true;
        tr.shadow     = shadow_mode;
        tr.spreadAtEntry = pos.spread_at_entry;
        on_close(tr);

        pos = OpenPos{};
    }

    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != _daily_day) {
            _daily_pnl = 0.0;
            _daily_day = day;
        }
    }
};

template <typename Traits>
class EMACrossCRTP : public EMACrossBase<EMACrossCRTP<Traits>> {
public:
    using traits_t = Traits;
};

// =============================================================================
// Default-instantiation aliases. Combo 0 is NOT the live default (combo 0
// is pair (0,1), ix_a=0, ix_b=0 -> 0.5x mults on params 0 and 1, 1.0x on
// the rest). For "live default", use the Base<...>Params struct directly
// or instantiate with a custom traits class. These aliases match the
// SweepableEngines.hpp aliases as a documentation breadcrumb only.
// =============================================================================
struct AsianRangeDefaultTraits {
    static constexpr double BUFFER       = AsianRangeBaseParams::BUFFER;
    static constexpr double MIN_RANGE    = AsianRangeBaseParams::MIN_RANGE;
    static constexpr double MAX_RANGE    = AsianRangeBaseParams::MAX_RANGE;
    static constexpr int    SL_TICKS     = AsianRangeBaseParams::SL_TICKS;
    static constexpr int    TP_TICKS     = AsianRangeBaseParams::TP_TICKS;
};
struct VWAPStretchDefaultTraits {
    static constexpr int    SL_TICKS     = VWAPStretchBaseParams::SL_TICKS;
    static constexpr int    TP_TICKS     = VWAPStretchBaseParams::TP_TICKS;
    static constexpr int    COOLDOWN_SEC = VWAPStretchBaseParams::COOLDOWN_SEC;
    static constexpr double SIGMA_ENTRY  = VWAPStretchBaseParams::SIGMA_ENTRY;
    static constexpr int    VOL_WINDOW   = VWAPStretchBaseParams::VOL_WINDOW;
};
struct DXYDivergenceDefaultTraits {
    static constexpr int    SL_TICKS      = DXYDivergenceBaseParams::SL_TICKS;
    static constexpr int    TP_TICKS      = DXYDivergenceBaseParams::TP_TICKS;
    static constexpr int    COOLDOWN_SEC  = DXYDivergenceBaseParams::COOLDOWN_SEC;
    static constexpr int    WINDOW        = DXYDivergenceBaseParams::WINDOW;
    static constexpr double DIV_THRESHOLD = DXYDivergenceBaseParams::DIV_THRESHOLD;
};
struct HBGDefaultTraits {
    static constexpr double MIN_RANGE     = HBGBaseParams::MIN_RANGE;
    static constexpr double MAX_RANGE     = HBGBaseParams::MAX_RANGE;
    static constexpr double SL_FRAC       = HBGBaseParams::SL_FRAC;
    static constexpr double TP_RR         = HBGBaseParams::TP_RR;
    static constexpr double TRAIL_FRAC    = HBGBaseParams::TRAIL_FRAC;
    static constexpr double MFE_LOCK_FRAC = HBGBaseParams::MFE_LOCK_FRAC;
};
struct EMACrossDefaultTraits {
    static constexpr int    FAST_PERIOD = EMACrossBaseParams::FAST_PERIOD;
    static constexpr int    SLOW_PERIOD = EMACrossBaseParams::SLOW_PERIOD;
    static constexpr double RSI_LO      = EMACrossBaseParams::RSI_LO;
    static constexpr double RSI_HI      = EMACrossBaseParams::RSI_HI;
    static constexpr double SL_MULT     = EMACrossBaseParams::SL_MULT;
};

using AsianRangeDefault    = AsianRangeCRTP   <AsianRangeDefaultTraits>;
using VWAPStretchDefault   = VWAPStretchCRTP  <VWAPStretchDefaultTraits>;
using DXYDivergenceDefault = DXYDivergenceCRTP<DXYDivergenceDefaultTraits>;
using HBGDefault           = HBG_CRTP         <HBGDefaultTraits>;
using EMACrossDefault      = EMACrossCRTP     <EMACrossDefaultTraits>;

} // namespace sweep_crtp
} // namespace omega
