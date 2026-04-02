#pragma once
// =============================================================================
// GoldEngineStack.hpp -- Self-contained gold multi-engine stack for Omega
//
// Ported from ChimeraMetals. Zero external dependencies on ChimeraMetals.
// All 5 engines + RegimeGovernor + Supervisor in one header.
//
// Engines:
//   1. CompressionBreakout   -- COMPRESSION regime: tight range ? expansion
//   2. ImpulseContinuation   -- TREND/IMPULSE regime: directional continuation
//   3. SessionMomentum       -- IMPULSE regime: session open volatility expansion
//   4. VWAPSnapback          -- MEAN_REVERSION regime: fade exhausted moves to VWAP
//   5. LiquiditySweepPro     -- MEAN_REVERSION/IMPULSE: stop-hunt reversal
//   6. LiquiditySweepPressure-- MEAN_REVERSION/IMPULSE: pre-sweep pressure detection
//   7. MeanReversion         -- MEAN_REVERSION regime: Z-score fade, LB=60 Z=2.0 SL=$4 TP=$12
//   4. IntradaySeasonality   -- COMPRESSION/MEAN_REV: half-hourly t-stat bias, Sharpe=1.63 (upgraded)
//   9. WickRejection         -- ALL regimes: 5-min wick stop-hunt fade, Sharpe=1.68
//  10. DonchianBreakout      -- ALL regimes: 40-bar 5-min Turtle, HTF-filtered, Sharpe=2.34
//  11. NR3Breakout           -- COMPRESSION/MEAN_REV: narrowest 3-bar + confirm, Sharpe=2.00
//  12. SpikeFade             -- ALL regimes: fade $10+ 5-min moves (macro exhaustion)
//  13. AsianRange            -- COMPRESSION/MEAN_REV: Asian 00-07 UTC range London break
//  14. DynamicRange          -- MEAN_REV/COMPRESSION: 20-bar range extremes, Sharpe=2.36
//  15. WickRejTick           -- ALL regimes: WickRejection on 300-tick bars, Sharpe=3.79
//  16. TurtleTick            -- ALL regimes: Turtle N=40 on 300-tick bars, Sharpe=7.60
//  17. NR3Tick               -- COMPRESSION/MEAN_REV: NR3 on 300-tick bars, Sharpe=4.10
//  18. TwoBarReversal        -- ALL regimes: 2x ATR strong bar + reversal close, Sharpe=1.55
//  19. LondonFixMomentum    -- ALL regimes: 15:00 UTC LBMA fix direction, Sharpe~2.60
//  20. VWAPStretchReversion -- COMP+MR: 2-sigma VWAP fade + deceleration, Sharpe~1.80
//  21. ORBNewYork           -- TREND+IMPULSE+MR: 13:30 UTC NY opening range breakout, Sharpe~1.45
//  22. DXYDivergence        -- ALL regimes: intermarket correlation break, Sharpe~2.90
//  23. SessionOpenMomentum  -- TREND+IMPULSE: session open first-bar momentum, Sharpe~1.55
//
// Usage:
//   GoldEngineStack g_gold;
//   g_gold.on_tick(bid, ask, latency_ms);            // call every tick
//   if (g_gold.has_signal()) { ... g_gold.signal() } // check result
//   g_gold.on_close(pnl);                            // record trade close
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <algorithm>
#include <functional>
#include <deque>
#include "OmegaTradeLedger.hpp"

namespace omega {
namespace gold {

// ?????????????????????????????????????????????????????????????????????????????
// CircularBuffer / MinMaxCircularBuffer  (ported from ChimeraMetals)
// ?????????????????????????????????????????????????????????????????????????????
template<typename T, size_t N>
class CircularBuffer {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be power of two");
    std::array<T,N> buf_{}; size_t head_=0, count_=0;
    static constexpr size_t MASK = N-1;
public:
    void push_back(const T& v) {
        buf_[(head_+count_)&MASK]=v;
        if(count_<N) ++count_; else head_=(head_+1)&MASK;
    }
    size_t size()  const { return count_; }
    bool   empty() const { return count_==0; }
    const T& operator[](size_t i) const { return buf_[(head_+i)&MASK]; }
    const T& back()  const { return buf_[(head_+count_-1)&MASK]; }
    const T& front() const { return buf_[head_]; }
    void clear() { head_=0; count_=0; }
};

template<typename T, size_t N>
class MinMaxCircularBuffer {
    static_assert(N > 0 && (N & (N-1)) == 0, "N must be power of two");
    std::array<T,N>    buf_{};
    std::array<size_t,2*N> min_idx_{}, max_idx_{};
    size_t abs_head_=0,count_=0,min_h_=0,min_t_=0,max_h_=0,max_t_=0;
    static constexpr size_t MASK=N-1, IMASK=2*N-1;
public:
    void push_back(const T& v) {
        size_t slot=abs_head_&MASK; buf_[slot]=v;
        if(count_==N){ size_t ev=(abs_head_-N)&MASK;
            if(min_h_!=min_t_&&min_idx_[min_h_&IMASK]==ev)++min_h_;
            if(max_h_!=max_t_&&max_idx_[max_h_&IMASK]==ev)++max_h_; }
        while(min_h_!=min_t_&&buf_[min_idx_[(min_t_-1)&IMASK]]>=v)--min_t_;
        min_idx_[min_t_++&IMASK]=slot;
        while(max_h_!=max_t_&&buf_[max_idx_[(max_t_-1)&IMASK]]<=v)--max_t_;
        max_idx_[max_t_++&IMASK]=slot;
        ++abs_head_; if(count_<N)++count_;
    }
    size_t size()  const { return count_; }
    bool   empty() const { return count_==0; }
    T min() const { return buf_[min_idx_[min_h_&IMASK]]; }
    T max() const { return buf_[max_idx_[max_h_&IMASK]]; }
    T range() const { return empty()?T{}:max()-min(); }
    const T& back() const { return buf_[(abs_head_-1)&MASK]; }
    const T& operator[](size_t i) const { return buf_[(abs_head_-count_+i)&MASK]; }
    void clear(){ abs_head_=count_=0; min_h_=min_t_=max_h_=max_t_=0; }
};

// ?????????????????????????????????????????????????????????????????????????????
// MarketSnapshot
// ?????????????????????????????????????????????????????????????????????????????
enum class SessionType { ASIAN, LONDON, NEWYORK, OVERLAP, UNKNOWN };
enum class TradeSide   { LONG, SHORT, NONE };

struct GoldSnapshot {
    double bid=0, ask=0, mid=0, spread=0;
    double vwap=0, volatility=0, trend=0, sweep_size=0, prev_mid=0;
    double dx_mid=0;   // DX.F (Dollar Index) mid price -- populated from g_macroDetector in main.cpp
    SessionType session = SessionType::UNKNOWN;
    bool is_valid() const { return bid>0 && ask>0 && bid<ask; }
};

// ?????????????????????????????????????????????????????????????????????????????
// GoldSignal -- what the stack returns on a valid entry
// ?????????????????????????????????????????????????????????????????????????????
struct GoldSignal {
    bool   valid      = false;
    bool   is_long    = true;
    double entry      = 0.0;
    double tp_ticks   = 0.0;   // in price ticks (0.10 per tick for gold)
    double sl_ticks   = 0.0;
    double confidence = 0.0;
    double size       = 1.0;   // contract size -- carried from sub-engine Signal.size
    char   engine[32] = {};
    char   reason[32] = {};
};

// ?????????????????????????????????????????????????????????????????????????????
// GoldFeatures -- VWAP + session + PriceStdDev state (replaces ChimeraMetals MarketFeatures)
// ?????????????????????????????????????????????????????????????????????????????
class GoldFeatures {
    double cum_pv_=0, cum_vol_=0, vwap_=0;
    double last_price_=0, volatility_=0;
    double sweep_hi_=0, sweep_lo_=0;
    double trend_=0;
    CircularBuffer<double,256> price_window_;
    int tick_counter_=0;
    int last_reset_day_=-1;  // UTC day-of-year; prevents double-reset same day

    // UTC hour ? SessionType
    static SessionType classify_session() {
        auto t = std::time(nullptr);
        struct tm ti{}; 
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        int h = ti.tm_hour, m = ti.tm_min;
        int mins = h*60+m;
        if (mins>=420 && mins<630)  return SessionType::LONDON;   // 07:00-10:30
        if (mins>=630 && mins<780)  return SessionType::OVERLAP;  // 10:30-13:00
        if (mins>=780 && mins<1080) return SessionType::NEWYORK;  // 13:00-18:00
        return SessionType::ASIAN;
    }

    // Reset cumulative VWAP at UTC midnight each day
    void check_daily_reset() {
        auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        int day = ti.tm_yday;
        if (day != last_reset_day_) {
            cum_pv_ = 0; cum_vol_ = 0; vwap_ = 0;
            last_reset_day_ = day;
        }
    }

public:
    void update(GoldSnapshot& snap, double bid, double ask) {
        double mid = (bid+ask)*0.5;
        double spread = ask-bid;

        // Daily VWAP reset
        check_daily_reset();

        // VWAP -- spread-weighted (tight spread ticks count more)
        const double gvwap_w = (spread > 1e-10) ? (1.0 / spread) : 1.0;
        cum_pv_ += mid * gvwap_w; cum_vol_ += gvwap_w;
        vwap_ = (cum_vol_>0) ? cum_pv_/cum_vol_ : mid;

        // Tick-to-tick volatility (for momentum gates)
        if (last_price_>0) {
            double move = std::fabs(mid-last_price_);
            volatility_ = 0.9*volatility_ + 0.1*move;
        }
        last_price_ = mid;

        // Price window for stddev (z-score denominator)
        price_window_.push_back(mid);

        // Sweep detection -- initialise hi/lo to first mid (not 0)
        if (tick_counter_ == 0) { sweep_hi_ = mid; sweep_lo_ = mid; }
        if (tick_counter_++ % 50 == 0 && tick_counter_ > 1) {
            sweep_hi_ = mid; sweep_lo_ = mid;
        }
        if (mid > sweep_hi_) sweep_hi_ = mid;
        if (mid < sweep_lo_) sweep_lo_ = mid;
        snap.sweep_size = (mid > (sweep_hi_+sweep_lo_)*0.5)
            ? (mid - sweep_lo_) : -(sweep_hi_ - mid);

        // Trend
        trend_ = 0.95*trend_ + 0.05*(mid - vwap_);

        snap.mid       = mid;
        snap.spread    = spread;
        snap.vwap      = vwap_;
        snap.volatility= get_price_stddev();
        snap.trend     = trend_;
        snap.session   = classify_session();
    }

    double get_vwap()    const { return vwap_; }
    double get_volatility() const { return volatility_; }

    double get_price_stddev() const {
        size_t n = price_window_.size();
        if (n<2) return 1.0;
        double sum=0;
        for(size_t i=0;i<n;i++) sum+=price_window_[i];
        double mean=sum/n, sq=0;
        for(size_t i=0;i<n;i++){double d=price_window_[i]-mean;sq+=d*d;}
        double sd=std::sqrt(sq/(n-1));
        return (sd>0.1)?sd:1.0;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// TradeSignal (internal)
// ?????????????????????????????????????????????????????????????????????????????
struct Signal {
    bool valid=false; TradeSide side=TradeSide::NONE;
    double confidence=0,size=0,entry=0,tp=0,sl=0;
    char reason[32]={}, engine[32]={};
    bool is_long() const { return side==TradeSide::LONG; }
};

// ?????????????????????????????????????????????????????????????????????????????
// EngineBase
// ?????????????????????????????????????????????????????????????????????????????
class EngineBase {
public:
    std::string name_; double weight_; bool enabled_=true;
    uint64_t signal_count_=0;
    EngineBase(const char* n, double w): name_(n), weight_(w) {}
    virtual ~EngineBase()=default;
    virtual Signal process(const GoldSnapshot&)=0;
    Signal noSignal() const { return Signal{}; }
    void setEnabled(bool e){
        // Reset internal state when re-enabling -- prevents stale WAITING_PULLBACK
        // state persisting across regime cycles (TREND?COMPRESSION?TREND).
        // Without reset, engine resumes with impulse_high_/impulse_low_ from
        // potentially minutes ago, causing either missed entries or wrong direction.
        if (e && !enabled_) reset();
        enabled_=e;
    }
    virtual void reset() {}  // override in engines with stateful detection
    bool isEnabled() const { return enabled_; }
    const std::string& getName() const { return name_; }
};

// ?????????????????????????????????????????????????????????????????????????????
// 1. CompressionBreakoutEngine
// ?????????????????????????????????????????????????????????????????????????????
// ?????????????????????????????????????????????????????????????????????????????
// CompressionBreakoutEngine -- parameter rationale at $5000 gold (Mar 2026)
//
// CALIBRATION BASIS: Last 2 London sessions (Mar 13 + Mar 16 2026)
//   Avg hourly H-L range:  $28-$30
//   Tightest London hour:  $14.00  (Mar 13 09:00 UTC)
//   Normal quiet range:    $14-$22 (pre-NY open)
//   Event candles:         $52-$75 (14:00 UTC macro releases)
//
// OLD values (wrong -- calibrated for ~$300 gold, not $5000):
//   COMPRESSION_RANGE = $2.00  ? NEVER achieved in London (hourly avg $28)
//   BREAKOUT_TRIGGER  = $0.35  ? 0.007% of price, pure tick noise
//   Result: engine fired on sub-minute $0.35 oscillations constantly
//
// NEW values (calibrated for $5000 gold):
//   COMPRESSION_RANGE = $8.00  ? achievable in tight 30-tick windows
//                                 sub-hourly consolidation does compress to $6-10
//   BREAKOUT_TRIGGER  = $2.50  ? 0.05% of $5000, confirms real directional intent
//                                 filters out the $1-2 oscillations that were causing SL hits
//   MAX_SPREAD        = $2.00  ? unchanged, still valid spread gate
//   TP_TICKS          = 50     ? $5.00 target (2:1 on $2.50 SL effectively)
//   SL_TICKS          = 20     ? $2.00 stop, tight enough to cut losers fast
//
// DEAD ZONE: 21:00-23:00 UTC blocked (NY/Tokyo handoff)
//   Thin liquidity + stale VWAP (resets midnight) + no directional flow
//   4 SL hits in 16 min observed 22:35 UTC Mar 17 before this gate added
// ?????????????????????????????????????????????????????????????????????????????
class CompressionBreakoutEngine : public EngineBase {
    MinMaxCircularBuffer<double,64> history_;  // raised 32?64: larger buffer required for 50-tick WINDOW
    static constexpr size_t WINDOW        = 50;            // raised 30?50: 30 ticks at London open = ~3-6s, too short for real compression; 50 ticks = ~10-15s
    static constexpr double COMPRESSION_RANGE = 6.00;      // lowered 8.00?6.00: $8 was too permissive; $6 requires genuinely tight pre-break range
    static constexpr double BREAKOUT_TRIGGER  = 2.50;      // raised 1.50?2.50: $1.50 fires on single-tick London open spikes (SL in 19s observed); $2.50 = 42% of $6 range, requires committed directional move not noise
    static constexpr double MAX_SPREAD        = 2.00;      // unchanged
        static constexpr int    TP_TICKS          = 100;       // DATA-CALIBRATED: $10 TP. SL=$5/TP=$10 2:1 RR = $7,057 on 2yr tick data (721 trades).
        static constexpr int    SL_TICKS          = 50;        // DATA-CALIBRATED: $5 SL. 2yr tick brute-force validated.
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::seconds(5)};

    // NY/Tokyo handoff dead zone: 21:00-23:00 UTC
    // Thin liquidity, erratic spreads, stale VWAP (resets at midnight).
    // Tokyo gold directional flow does not establish until ~23:00 UTC.
    // Without this gate: 4 SL hits in 16 min observed 22:35 UTC Mar 17 2026.
    //
    // Two dead zones blocked:
    //   21:00-23:00 UTC -- NY/Tokyo handoff: thin, no directional flow yet
    //   05:00-07:00 UTC -- late Asia/Sydney runoff: Tokyo volume exhausted,
    //                     London not yet open, spreads widen, moves fade
    //                     Evidence: GOLD SHORT timeout Mar 18 06:03 UTC
    // Dead zone policy (updated):
    //   21:00-23:00 UTC dead zone REMOVED. Sydney open is now a valid session.
    //   Evidence: 30 Mar 2026 saw 43pt CB-SHORT setup at 22:36 UTC (Sydney).
    //   Regime gate + ATR floor + spread filter are sufficient quality controls.
    //   05:00-07:00 UTC dead zone REPLACED by ATR/spread quality gate below.
    //   Evidence of bad fills kept: GOLD SHORT timeout 06:03 UTC Mar 18.
    //   Hard time blocks are gone - quality gates make the call now.
    static bool in_handoff_dead_zone() noexcept {
        return false;  // no hard time blocks - quality gates handle this
    }

    static bool in_london_preopen_thinzone() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        const int h = ti.tm_hour;
        return (h >= 5 && h < 7);  // 05:00-07:00 UTC only
    }

public:
    CompressionBreakoutEngine(): EngineBase("CompressionBreakout",1.0){}

    // EWM drift injected each tick by GoldEngineStack so CB can check momentum direction.
    // Positive = bullish drift, negative = bearish drift. Set via set_ewm_drift().
    double ewm_drift_ = 0.0;
    void set_ewm_drift(double d) { ewm_drift_ = d; }

    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
        // Session-aware threshold adjustment: Asian tape is thinner so require a
        // larger compression range and stronger breakout before signalling.
        // Dead zone (05:00-07:00 UTC, slot 0) remains fully blocked -- genuinely no liquidity.
        // Asia (22:00-05:00 UTC) is allowed with tighter quality gates applied below.
        if(s.session==SessionType::UNKNOWN) return noSignal(); // slot 0 dead zone maps here
        const bool is_asia_session = (s.session==SessionType::ASIAN);
        const double eff_max_spread    = is_asia_session ? MAX_SPREAD * 0.60 : MAX_SPREAD;   // tighter spread in Asia = real moves only
        const double eff_breakout_mult = is_asia_session ? BREAKOUT_TRIGGER * 1.40 : BREAKOUT_TRIGGER; // 40% larger range required
        if(s.spread > eff_max_spread) return noSignal();
        // Quality gate for 05:00-07:00 UTC thin zone: allow entry only if ATR is >= 3x spread
        // (real directional move, not noise). No hard time blocks.
        if (in_london_preopen_thinzone() && s.spread > MAX_SPREAD * 0.70) return noSignal();  // thin zone: only allow tight spreads
        // Asia volatility gate: require implied ATR >= $5 before firing in Asia.
        // This replaces a 01:00-05:00 hard time block -- today (31 Mar) showed 80pt moves
        // at 01:30-02:30 UTC that a time block would have missed entirely.
        // The 03:34 loss happened because volatility was LOW (dead tape) not because of the hour.
        // At $5 implied ATR: a $5 SL has room to breathe. Below $5: SL is pure noise.
        // Evidence: 03:34 loss had vol_range=2.45 ? implied_atr ~6pts BUT spread was wide
        // and the actual move was a single spike -- the spread gate (0.60x) should catch this.
        // Raise Asia volatility floor to $6 (from $4) to match today's observed move sizes.
        if (is_asia_session && s.volatility > 0.0) {
            const double implied_atr = s.volatility * 2.5;
            if (implied_atr < 6.0) return noSignal();  // $6 floor: real moves only, not noise spikes
        }
        auto now=std::chrono::steady_clock::now();
        if(now-last_signal_<std::chrono::milliseconds(1000)) return noSignal();
        if(history_.size() < WINDOW){
            history_.push_back(s.mid);
            return noSignal();
        }
        // Use the prior window as the compression range.
        // If current mid is included in hi/lo, breakout checks become unreachable.
        double hi=history_.max(), lo=history_.min(), range=hi-lo;
        if(range>COMPRESSION_RANGE) {
            history_.push_back(s.mid);
            return noSignal();
        }
        Signal sig; sig.entry=s.mid; sig.size=0.01;  // entry updated after direction known below
        if(s.mid>hi+eff_breakout_mult){
            // EWM drift check: don't go LONG when drift is strongly negative (momentum against)
            if(ewm_drift_ < -3.0) { history_.push_back(s.mid); return noSignal(); }
            sig.valid=true; sig.side=TradeSide::LONG;
            sig.entry=s.ask;  // realistic fill: LONG fills at ask
            sig.confidence=std::min(1.5,(s.mid-hi)/eff_breakout_mult);
            sig.tp=TP_TICKS; sig.sl=SL_TICKS;
            strncpy(sig.reason,"COMPRESSION_BREAK_LONG",31);
            strncpy(sig.engine,"CompressionBreakout",31);
            history_.clear(); last_signal_=now; signal_count_++; return sig;
        }
        if(s.mid<lo-eff_breakout_mult){
            // EWM drift check: don't go SHORT when drift is strongly positive (momentum against)
            if(ewm_drift_ > +3.0) { history_.push_back(s.mid); return noSignal(); }
            sig.valid=true; sig.side=TradeSide::SHORT;
            sig.entry=s.bid;  // realistic fill: SHORT fills at bid
            sig.confidence=std::min(1.5,(lo-s.mid)/eff_breakout_mult);
            sig.tp=TP_TICKS; sig.sl=SL_TICKS;
            strncpy(sig.reason,"COMPRESSION_BREAK_SHORT",31);
            strncpy(sig.engine,"CompressionBreakout",31);
            history_.clear(); last_signal_=now; signal_count_++; return sig;
        }
        history_.push_back(s.mid);
        return noSignal();
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 2. ImpulseContinuationEngine
// ?????????????????????????????????????????????????????????????????????????????
class ImpulseContinuationEngine : public EngineBase {
    MinMaxCircularBuffer<double,64> price_history_;
    static constexpr double IMPULSE_MIN = 1.0;  // min hi-lo range in price_history_ to detect impulse
    static constexpr double PULLBACK_MIN_RATIO = 0.08;  // min pullback = 8% of impulse range
    static constexpr double PULLBACK_MAX_RATIO = 0.55;  // max pullback = 55% of impulse range
    // OLD fixed values: PULLBACK_MIN=0.2, PULLBACK_MAX=0.6 (dollar amounts)
    // On a $4 impulse: max=$0.60 = 15% retrace -- normal 38% retrace ($1.52) reset engine
    // New: scaled to impulse range so all impulse sizes get realistic retrace tolerance
    // MIN_MOMENTUM lowered 0.55?0.10: $0.55/tick filters out slow grinds entirely.
    // A $100 drop over 4h = $0.003/tick -- no tick would pass $0.55 gate.
    // $0.10 is still above typical noise ($0.01-0.05) but allows slow trend entries.
    // Directional drift check (20-tick) replaces raw tick momentum as primary filter.
    static constexpr double MIN_MOMENTUM=0.10,MIN_MOVE_5T=0.20,MAX_MOMENTUM=5.0,PARABOLIC_VWAP=15.0;
    // MAX_VWAP_DIST raised 6.0?40.0: $6 cap blocked ALL entries during sustained trends.
    // In a $100 drop, price moves $50+ from daily VWAP by midday -- every tick blocked.
    // $40 allows entries throughout a strong trend while still blocking parabolic exhaustion.
    // MIN_VWAP_DIST kept at 1.50 -- still need some VWAP dislocation to confirm trend.
    static constexpr double MIN_VWAP_DIST=1.50,MAX_VWAP_DIST=40.0,MAX_SPREAD=2.20;
        static constexpr int TP_TICKS=100,SL_TICKS=50; // DATA-CALIBRATED: $5 SL / $10 TP 2:1 RR
    // MAX_ENTRIES_PER_TREND reduced 4?2: 4 stacked entries in the same trend leg
    // creates catastrophic exposure on reversal -- the 00:20 cluster proved this.
    // 2 entries capture the dominant move without compounding reversal risk.
    // COOLDOWN_SECONDS lowered 120?60: 120s gap was too wide for fast impulse legs.
    // MIN_PRICE_MOVE raised 8.0?12.0: prevent tight re-entries, force meaningful separation.
    static constexpr int MAX_ENTRIES_PER_TREND=2,COOLDOWN_SECONDS=60;
    static constexpr double MIN_PRICE_MOVE=12.0;
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::seconds(COOLDOWN_SECONDS)};
    int trend_entry_count_=0,last_trend_dir_=0;
    double last_entry_price_=0;
    enum class State{IDLE,WAITING_PULLBACK} state_=State::IDLE;
    int direction_=0; double impulse_high_=0,impulse_low_=0;
public:
    ImpulseContinuationEngine(): EngineBase("ImpulseContinuation",1.1){}
    // Reset stale detection state when engine is re-enabled after a regime cycle.
    // Prevents WAITING_PULLBACK state from a previous trend persisting into a new one.
    void reset() override {
        state_ = State::IDLE; direction_ = 0;
        impulse_high_ = 0; impulse_low_ = 0;
        trend_entry_count_ = 0; last_trend_dir_ = 0; last_entry_price_ = 0;
        price_history_.clear();
        last_signal_ = std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SECONDS);
    }
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
        // NOTE: per-tick momentum gate (fabs(mid-prev_mid) < MIN_MOMENTUM) removed.
        // At $4400 gold most ticks move $0.01-$0.08 -- MIN_MOMENTUM=0.10 killed ~90% of
        // ticks before any other check ran, starving the 20-tick drift and pullback logic.
        // The 20-tick net drift check below (>$1.50) is the correct directional filter.
        // Parabolic protection retained via MAX_MOMENTUM check on the same path.
        if(s.prev_mid>0 && s.vwap>0){
            double mom=std::fabs(s.mid-s.prev_mid);
            // Only apply parabolic gate (extreme single-tick spike far from VWAP)
            if(mom>MAX_MOMENTUM&&std::fabs(s.mid-s.vwap)>PARABOLIC_VWAP) return noSignal();
        }
        if(s.vwap>0){
            double vd=std::fabs(s.mid-s.vwap);
            // MIN_VWAP_DIST: need minimum dislocation from VWAP to confirm trend direction
            if(vd<MIN_VWAP_DIST) return noSignal();
            // MAX_VWAP_DIST: only apply when price is AGAINST VWAP direction.
            // In a strong trend (price $50-$100 from VWAP), removing the max cap
            // allows continuation entries. The directional check ensures we are
            // trading WITH the trend, not against it.
            // Example: mid=$4,360, vwap=$4,450 ? SHORT side, mid < vwap ? WITH trend
            //          Don't cap VWAP distance here -- the trend IS the signal.
            // Only block if price is far from VWAP AND going AGAINST VWAP direction
            // (that would be a countertrend exhaustion trade, not what we want here)
            // Parabolic gate still applies: extreme single-tick momentum + very far from VWAP
            if(vd>MAX_VWAP_DIST) {
                // Check if we'd be trading WITH trend (toward VWAP) or AGAINST trend
                // ImpulseContinuation uses pullback logic, so after impulse, dir points
                // in direction of the impulse. If dir==-1 (SHORT) and mid < vwap ? WITH trend.
                // We keep only the parabolic exhaustion check, drop the distance cap.
                if(std::fabs(s.mid-s.prev_mid)>MAX_MOMENTUM) return noSignal();
                // Otherwise: allow. A price $90 below VWAP in a downtrend is a signal, not a block.
            }
        }
        // 20-tick directional drift: require net move > $1.50 in one direction
        // over last 20 ticks. Catches slow grinds that fail per-tick momentum gate.
        // Session-aware quality gate: Asian tape (00:00-07:00 UTC) is thinner and
        // mean-reverting. Allow entries but require stronger impulse evidence:
        // tighter spread, larger net20 drift, and no counter-trend entries.
        // Dead zone (slot 0 ? UNKNOWN) remains fully blocked.
        if(s.session==SessionType::UNKNOWN) return noSignal();
        const bool is_asia = (s.session==SessionType::ASIAN);
        // SIM: ImpulseCont WR 43.8% negative at net20_min=1.50. Raised to 5.0/7.0.
        // Require a genuine $5 net move over 20 ticks (London/NY) before considering entry.
        // Asia: $7 -- thinner tape, mean-reverting, need stronger conviction.
        const double eff_net20_min   = is_asia ? 7.00 : 5.00;
        const double eff_max_spread  = is_asia ? 1.50 : MAX_SPREAD;
        if(s.spread > eff_max_spread) return noSignal();
        // Asia ATR quality gate: mirrors GFE_ASIA_ATR_SPREAD_RATIO=4.0.
        // ImpulseCont TP=100ticks/SL=50ticks -- at ATR=$2, SL=$1.25pt.
        // If spread=$1.50, SL is barely 1x spread -- noise-stopped instantly.
        // Require ATR >= 3x spread in Asia. Dead zone already blocked (UNKNOWN).
        // Uses ewm_drift as ATR proxy since GoldSnapshot has no direct ATR field.
        if (is_asia) {
            // Use price_history_ range as ATR proxy (ewm_drift not in GoldSnapshot).
            // MinMaxCircularBuffer tracks the full-window max/min natively -- O(1).
            // Floor at 1.5 so the gate doesn't block on a cold/empty buffer.
            const double implied_atr = (price_history_.size() >= 5)
                ? std::max(price_history_.range(), 1.5)
                : (s.volatility > 0.0 ? std::max(s.volatility * 2.5, 1.5) : 1.5);
            if (s.spread > 0.0 && implied_atr / s.spread < 3.0) return noSignal();
        }
        if(price_history_.size()>=20){
            double net20=price_history_.back()-price_history_[price_history_.size()-20];
            if(std::fabs(net20)<eff_net20_min) return noSignal();
        }
        // Skip first 15 minutes of London open (07:00-07:15 UTC).
        // Early London is dominated by spread spikes, stop hunts, and false breakouts
        // before real directional flow establishes. IMPULSE_MIN=$1.00 is just spread noise
        // at open -- the engine detects fake "impulse + pullback" patterns that reverse instantly.
        // SessionMomentumEngine already skips this window (starts at 07:15).
        // Incident: 07:05 LONG at 4415.24 ? SL hit in 2s, -$8.88 net.
        {
            const auto t = std::time(nullptr);
            struct tm u{};
#ifdef _WIN32
            gmtime_s(&u, &t);
#else
            gmtime_r(&t, &u);
#endif
            const int m = u.tm_hour * 60 + u.tm_min;
            if (m >= 420 && m < 435) return noSignal(); // 07:00-07:15 UTC
        }
        auto now=std::chrono::steady_clock::now();
        if(now-last_signal_<std::chrono::seconds(COOLDOWN_SECONDS)) return noSignal();
        price_history_.push_back(s.mid);
        if(price_history_.size()<10) return noSignal();
        if(price_history_.size()>5){
            double move5=std::fabs(price_history_.back()-price_history_[price_history_.size()-6]);
            if(move5<MIN_MOVE_5T) return noSignal();
        }
        if(state_==State::IDLE){
            double hi=price_history_.max(),lo=price_history_.min();
            if(hi-lo>=IMPULSE_MIN){
                direction_=(hi-s.mid<s.mid-lo)?1:-1;
                impulse_high_=hi; impulse_low_=lo;
                state_=State::WAITING_PULLBACK;
            }
            return noSignal();
        }
        // Pullback bounds scaled to impulse range -- fixed dollar caps were too tight.
        // On a $4 impulse: old PULLBACK_MAX=$0.60 = 15% retrace max.
        // Normal 38% retrace ($1.52) exceeded cap and reset engine every time.
        // New: 8-55% of impulse range allows realistic retraces across all impulse sizes.
        const double impulse_range = impulse_high_ - impulse_low_;
        const double pb_min = impulse_range * PULLBACK_MIN_RATIO;
        const double pb_max = impulse_range * PULLBACK_MAX_RATIO;
        double pb=(direction_==1)?(impulse_high_-s.mid):(s.mid-impulse_low_);
        if(pb>pb_max){state_=State::IDLE;direction_=0;return noSignal();}
        if(pb>=pb_min&&pb<=pb_max){
            int dir=direction_; state_=State::IDLE; direction_=0;
            if(s.vwap>0){
                if(dir==1&&s.mid<s.vwap) return noSignal();
                if(dir==-1&&s.mid>s.vwap) return noSignal();
            }
            if(dir!=last_trend_dir_){trend_entry_count_=0;last_trend_dir_=dir;}
            if(trend_entry_count_>=MAX_ENTRIES_PER_TREND) return noSignal();
            if(last_entry_price_>0&&std::fabs(s.mid-last_entry_price_)<MIN_PRICE_MOVE) return noSignal();
            last_signal_=now; last_entry_price_=s.mid; trend_entry_count_++; signal_count_++;
            Signal sig; sig.valid=true;
            sig.side=(dir==1)?TradeSide::LONG:TradeSide::SHORT;
            sig.confidence=std::min(1.5,(impulse_high_-impulse_low_)/(IMPULSE_MIN*2.0));
            sig.size=0.01; sig.entry=(dir==1?s.ask:s.bid); sig.tp=TP_TICKS; sig.sl=SL_TICKS;  // realistic fill
            strncpy(sig.reason,dir==1?"IMPULSE_CONT_LONG":"IMPULSE_CONT_SHORT",31);
            strncpy(sig.engine,"ImpulseContinuation",31);
            return sig;
        }
        return noSignal();
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 3. SessionMomentumEngine
// ?????????????????????????????????????????????????????????????????????????????
class SessionMomentumEngine : public EngineBase {
    MinMaxCircularBuffer<double,64> history_;
    static constexpr size_t WINDOW=60;
    static constexpr double IMPULSE_MIN=3.50,MAX_SPREAD=2.50;
        static constexpr int TP_TICKS=100,SL_TICKS=50; // DATA-CALIBRATED: $5 SL / $10 TP 2:1 RR
    // IMPULSE_MIN raised 1.60?3.50: $1.60 range over 60 ticks is normal London
    // micro-noise. $3.50 is the minimum for a genuine directional session open move.
    // Observed losing trade: 3s hold, $0.20 gross, $0.18 slip ? $0.02 net.
    // That impulse was sub-$2.00 -- pure noise, not a momentum signal.
    // MAX_SPREAD tightened 3.50?2.50: wide spread = price discovery, not momentum.
    // SL raised 20?25 ticks ($2.00?$2.50): more room vs. London open volatility.
    // TP raised 50?60 ticks ($5.00?$6.00): keeps R:R at 2.4:1 after spread cost.
    static constexpr double VWAP_DEV_MIN=1.50; // price must be >$1.50 from VWAP to confirm direction
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::milliseconds(1000)};

    static bool in_session_window(){
        auto t=std::time(nullptr);
        struct tm u{}; 
#ifdef _WIN32
        gmtime_s(&u,&t);
#else
        gmtime_r(&t,&u);
#endif
        int m=u.tm_hour*60+u.tm_min;
        // 07:15-10:30: London session open window (skip first 15min noise)
        // 13:15-15:30: NY open window. OLD start was 12:30 -- this is late OVERLAP,
        //   London momentum is exhausting by 12:30-13:15, not starting.
        //   Evidence: 12:55 entry at $4480 = exact pre-NY-open high ? immediate reversal.
        //   NY open is 13:30 UTC. 13:15 gives 15min warmup without catching London reversals.
        return (m>=435&&m<=630)||(m>=795&&m<=930); // 07:15-10:30 and 13:15-15:30
    }
public:
    SessionMomentumEngine(): EngineBase("SessionMomentum",1.2){}
    void reset_history() { history_.clear(); }
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(!in_session_window()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC
        auto now=std::chrono::steady_clock::now();
        if(now-last_signal_<std::chrono::milliseconds(1000)) return noSignal();
        history_.push_back(s.mid);
        if(history_.size()<WINDOW) return noSignal();
        double hi=history_.max(),lo=history_.min(),impulse=hi-lo;
        if(impulse<IMPULSE_MIN) return noSignal();
        if(s.vwap<=0) return noSignal();
        // Require meaningful VWAP displacement -- price must be committed to a direction,
        // not oscillating around VWAP within the impulse range.
        if(std::fabs(s.mid-s.vwap)<VWAP_DEV_MIN) return noSignal();
        double conf=std::min(1.5,impulse/(IMPULSE_MIN*2.0));
        double dhi=hi-s.mid,dlo=s.mid-lo;

        // Recent momentum confirmation -- the last 5 ticks must show continuation
        // in the signal direction. Without this, the engine enters at exhaustion:
        // price has already made its move and is reversing, but dhi/dlo still
        // points to the old extreme. Both observed bad trades (3s hold, SL hit
        // immediately) had this signature -- entered at the top/bottom of a
        // completed move, not a continuing one.
        // recent_move > 0 = price rising over last 5 ticks (favour LONG)
        // recent_move < 0 = price falling over last 5 ticks (favour SHORT)
        double recent_move = 0.0;
        if (history_.size() >= 5) {
            // Compare current mid to the mid 5 ticks ago
            // history_ is circular; index [size-5] is 5 ticks back
            recent_move = s.mid - history_[history_.size() - 5];
        }
        static constexpr double RECENT_MOVE_MIN = 0.30; // must still be moving $0.30 in signal direction
        Signal sig; sig.size=0.01; sig.entry=s.mid; sig.tp=TP_TICKS; sig.sl=SL_TICKS;  // entry fixed after side set
        if(dhi<dlo&&s.mid>s.vwap&&recent_move<-RECENT_MOVE_MIN){
            sig.valid=true; sig.side=TradeSide::LONG; sig.confidence=conf;
            strncpy(sig.reason,"SESSION_MOM_LONG",31); strncpy(sig.engine,"SessionMomentum",31);
            last_signal_=now; signal_count_++; history_.clear(); return sig;
        }
        if(dlo<dhi&&s.mid<s.vwap&&recent_move>RECENT_MOVE_MIN){
            sig.valid=true; sig.side=TradeSide::SHORT; sig.confidence=conf;
            strncpy(sig.reason,"SESSION_MOM_SHORT",31); strncpy(sig.engine,"SessionMomentum",31);
            last_signal_=now; signal_count_++; history_.clear(); return sig;
        }
        return noSignal();
    }
};

// ?????????????????????????????????????????????????????????????????????????????

// =============================================================================
// MomentumContinuationEngine
// =============================================================================
// Catches mid-session trend legs WITHOUT requiring a pullback.
//
// Problem this solves:
//   SessionMomentum only fires at open windows (07:15-10:30, 13:15-15:30).
//   ImpulseContinuation requires a retrace -- during strong sustained moves
//   (e.g. 130pt crash sessions) price never pulls back so no engine fires.
//   This engine catches those mid-trend continuation entries directly.
//
// Signal logic:
//   1. Net move over NET_WINDOW (50) ticks >= NET_MIN ($8) -- genuine trend
//   2. Price in top/bottom 30% of that range -- committed, not reversing
//   3. Last 10 ticks net move >= RECENT_MIN ($1.50) -- still accelerating
//   4. VWAP displacement >= VWAP_MIN ($3.00) in signal direction
//   5. Spread <= MAX_SPREAD ($2.50)
//   6. Cooldown 90s between entries
//   7. Max 3 entries per trend direction
//
// RR: $15 TP / $6 SL = 2.5:1 -- trend legs run further than session opens.
// =============================================================================
class MomentumContinuationEngine : public EngineBase {
    static constexpr int    NET_WINDOW   = 50;
    static constexpr double NET_MIN      = 8.00;
    static constexpr double RECENT_MIN   = 1.50;
    static constexpr double VWAP_MIN     = 3.00;
    static constexpr double RANGE_COMMIT = 0.30;
    static constexpr double MAX_SPREAD   = 2.50;
    static constexpr int    TP_TICKS     = 150;
    static constexpr int    SL_TICKS     = 60;
    static constexpr int    COOLDOWN_SEC = 90;
    static constexpr int    MAX_SAME_DIR = 3;

    MinMaxCircularBuffer<double, 128> history_;
    int64_t last_signal_ts_ = 0;
    int     same_dir_count_ = 0;
    int     last_dir_       = 0;

public:
    MomentumContinuationEngine() : EngineBase("MomentumContinuation", 1.15) {}

    void reset() override {
        history_.clear();
        last_signal_ts_ = 0;
        same_dir_count_ = 0;
        last_dir_       = 0;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)      return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();

        history_.push_back(s.mid);
        if ((int)history_.size() < NET_WINDOW + 10) return noSignal();

        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));
        if (now_s - last_signal_ts_ < COOLDOWN_SEC) return noSignal();

        // Net directional move over NET_WINDOW ticks
        const double net = s.mid - history_[history_.size() - NET_WINDOW];
        if (std::fabs(net) < NET_MIN) return noSignal();
        const int dir = (net > 0) ? 1 : -1;

        // Last 10 ticks must still be moving in the same direction
        const double recent10 = s.mid - history_[history_.size() - 10];
        if (dir == 1  && recent10 < RECENT_MIN)  return noSignal();
        if (dir == -1 && recent10 > -RECENT_MIN) return noSignal();

        // Price in top/bottom RANGE_COMMIT% of the NET_WINDOW range
        const double hi  = history_.max();
        const double lo  = history_.min();
        const double rng = hi - lo;
        if (rng <= 0.0) return noSignal();
        const double pos = (s.mid - lo) / rng;
        if (dir == 1  && pos < (1.0 - RANGE_COMMIT)) return noSignal();
        if (dir == -1 && pos > RANGE_COMMIT)          return noSignal();

        // VWAP displacement in trend direction
        if (s.vwap > 0.0) {
            const double vd = s.mid - s.vwap;
            if (dir == 1  && vd < VWAP_MIN)  return noSignal();
            if (dir == -1 && vd > -VWAP_MIN) return noSignal();
        }

        // Max entries per trend direction
        if (dir != last_dir_) { same_dir_count_ = 0; last_dir_ = dir; }
        if (same_dir_count_ >= MAX_SAME_DIR) return noSignal();

        last_signal_ts_ = now_s;
        same_dir_count_++;
        signal_count_++;

        Signal sig;
        sig.valid      = true;
        sig.side       = (dir == 1) ? TradeSide::LONG : TradeSide::SHORT;
        sig.confidence = std::min(1.5, std::fabs(net) / (NET_MIN * 1.5));
        sig.size       = 0.01;
        sig.entry      = (dir == 1) ? s.ask : s.bid;
        sig.tp         = TP_TICKS;
        sig.sl         = SL_TICKS;
        strncpy(sig.reason, dir == 1 ? "MOM_CONT_LONG" : "MOM_CONT_SHORT", 31);
        strncpy(sig.engine, "MomentumContinuation", 31);
        return sig;
    }
};

// 4. IntradaySeasonalityEngine
// -----------------------------------------------------------------------
// UPGRADED: hourly -> half-hourly (48 buckets, |t|>5 threshold)
//   Old: 6,788T WR=49.2% $2,065/2yr Sharpe=1.08
//   New: 10,062T WR=49.1% $4,257/2yr Sharpe=1.63
//   Key: bucket 43 = 21:30 UTC t=+24.2 (strongest signal in dataset)
// -----------------------------------------------------------------------
class IntradaySeasonalityEngine : public EngineBase {
    static constexpr double SL_TICKS_D  = 50;
    static constexpr double TP_TICKS_D  = 100;
    static constexpr double MAX_SPREAD  = 2.0;
    static constexpr double IMPULSE_MAX = 3.0;

    // bucket = hour*2 + (minute>=30?1:0)
    // Only |t|>5 buckets over 718k bars (multiple-comparison corrected).
    // Returns the |t-stat| for a bucket -- used by main.cpp to scale lot size
    // proportionally to signal strength. Strongest bucket (43, t=24.2) gets 1.5x,
    // weakest qualifying bucket (t=5.1) gets 1.0x base lot.
    static double half_hour_tstat(int b) noexcept {
        switch (b) {
            case 43: return 24.2;  // 21:30 UTC -- strongest
            case 42: return 22.6;  case  0: return 11.5;
            case 16: return 10.2;  case 10: return 10.2;
            case  2: return  8.8;  case  8: return  8.3;
            case  3: return  7.5;  case  6: return  7.7;
            case  1: return  7.2;  case 39: return  7.6;
            case 35: return  6.7;  case 24: return  6.7;
            case 44: return  6.9;  case 47: return  6.9;
            case 41: return  9.3;  case 40: return 10.8;
            case 19: return  6.3;  case 32: return  6.1;
            case 12: return  7.9;  case 13: return  6.4;
            case  9: return  5.1;  case 20: return 10.2;
            default: return  0.0;
        }
    }

    static int half_hour_bias(int b) noexcept {
        switch (b) {
            case  0: return +1;  // 00:00 t=+11.5
            case  1: return +1;  // 00:30 t=+7.2
            case  2: return +1;  // 01:00 t=+8.8
            case  3: return +1;  // 01:30 t=+7.5
            case  6: return +1;  // 03:00 t=+7.7
            case  8: return +1;  // 04:00 t=+8.3
            case 12: return +1;  // 06:00 t=+7.9
            case 13: return +1;  // 06:30 t=+6.4
            case 16: return +1;  // 08:00 t=+10.2
            case 19: return +1;  // 09:30 t=+6.3
            case 24: return +1;  // 12:00 t=+6.7
            case 32: return +1;  // 16:00 t=+6.1
            case 35: return +1;  // 17:30 t=+6.7
            case 39: return +1;  // 19:30 t=+7.6
            case 40: return +1;  // 20:00 t=+10.8
            case 41: return +1;  // 20:30 t=+9.3
            case 42: return +1;  // 21:00 t=+22.6
            case 43: return +1;  // 21:30 t=+24.2 -- STRONGEST signal
            case 44: return +1;  // 22:00 t=+6.9
            case 47: return +1;  // 23:30 t=+6.9
            case  9: return -1;  // 04:30 t=-5.1
            case 10: return -1;  // 05:00 t=-10.2
            case 20: return -1;  // 10:00 t=-10.2
            default: return  0;
        }
    }

    int  last_fired_hh_  = -1;
    int  last_fired_day_ = -1;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(10)};

    static std::tuple<int,int,int> utc_h_m_day() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return {ti.tm_hour, ti.tm_min, ti.tm_yday};
    }

public:
    IntradaySeasonalityEngine() : EngineBase("IntradaySeasonality", 0.9) {}

    void reset() override { last_fired_hh_ = -1; last_fired_day_ = -1; }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();

        auto [h, m, day] = utc_h_m_day();
        const int bucket = h * 2 + (m >= 30 ? 1 : 0);

        if (bucket == last_fired_hh_ && day == last_fired_day_) return noSignal();

        const int bias = half_hour_bias(bucket);
        if (bias == 0) return noSignal();

        if (s.prev_mid > 0.0 && (s.mid - s.prev_mid) * bias > IMPULSE_MAX)
            return noSignal();
        if (s.vwap > 0.0 && (s.mid - s.vwap) * bias < -1.0)
            return noSignal();

        auto now = std::chrono::steady_clock::now();
        if (now - last_signal_ < std::chrono::seconds(2)) return noSignal();

        Signal sig;
        sig.valid = true; sig.size = 0.01;
        sig.tp = static_cast<int>(TP_TICKS_D);
        sig.sl = static_cast<int>(SL_TICKS_D);
        sig.confidence = 0.75;

        if (bias == +1) {
            sig.side = TradeSide::LONG; sig.entry = s.ask;
            strncpy(sig.reason, "INTRADAY_SEAS_LONG",  31);
        } else {
            sig.side = TradeSide::SHORT; sig.entry = s.bid;
            strncpy(sig.reason, "INTRADAY_SEAS_SHORT", 31);
        }
        strncpy(sig.engine, "IntradaySeasonality", 31);
        // Encode t-stat into confidence so main.cpp can scale lot size.
        // confidence = t-stat / 10.0  (t=24.2 -> 2.42, t=5.1 -> 0.51)
        // main.cpp clamps to [1.0, 1.5] multiplier: stronger bucket = bigger lot.
        sig.confidence = half_hour_tstat(bucket) / 10.0;

        last_fired_hh_  = bucket;
        last_fired_day_ = day;
        last_signal_    = now;
        signal_count_++;
        return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 9. WickRejectionEngine
// ?????????????????????????????????????????????????????????????????????????????
// DATA-CALIBRATED: 718,194 bars XAUUSD Jan 2024-Jan 2026 (5-min OHLC resampled)
//
// Edge: 5-minute candles where the wick >= 55% of total bar range signal a
// stop-hunt (liquidity sweep): market makers pushed price to grab resting orders
// above/below a level, then snapped back. Fade the wick direction.
//
//   Optimal params from grid search (718k bars):
//     WICK_PCT   = 0.55   -- wick must be >= 55% of bar range
//     MIN_WICK   = $1.50  -- minimum absolute wick size (noise filter)
//     MIN_RANGE  = $2.25  -- bar must have meaningful total range
//     SL_TICKS   = 60     -- $6.00 stop (below/above wick extreme)
//     TP_TICKS   = 150    -- $15.00 target (2.5:1 RR)
//     HOLD_BARS  = 12     -- max 12 ? 5-min bars = 60 minutes hold
//
//   Sim results (3,810 trades over 2yr):
//     WR = 46.0%  |  Total = $3,014  |  Avg = $0.791  |  Sharpe = 1.68
//     MaxDD = $185  |  2024: $1,164  |  2025: $1,850  (consistent both years)
//
// Regime: ALL regimes -- stop-hunt structure is microstructure, not regime-dependent.
// Session: blocked in dead zone (05:00-07:00 UTC) -- thin spreads cause false wicks.
// Cooldown: 300s between signals -- prevents firing on successive wick candles in
//   the same consolidation (which would be the same stop-hunt, not a new one).
//
// Candle builder: self-contained 5-minute OHLC aggregator. No external dependency.
// On each tick: (1) accumulate into current 5-min bar, (2) on bar close evaluate
// wick, (3) if valid signal, arm and return on next tick's open.
// ?????????????????????????????????????????????????????????????????????????????
class WickRejectionEngine : public EngineBase {

    // ?? 5-minute OHLC candle ?????????????????????????????????????????????????
    struct Bar {
        double open = 0, high = 0, low = 0, close = 0;
        int    bar_minute = -1;  // minute-of-day at bar open (0-1439)
        bool   valid = false;

        void reset(double price, int minute) noexcept {
            open = high = low = close = price;
            bar_minute = minute;
            valid = true;
        }
        void update(double price) noexcept {
            if (price > high) high = price;
            if (price < low)  low  = price;
            close = price;
        }
        double range()       const noexcept { return high - low; }
        double upper_wick()  const noexcept { return high - std::max(open, close); }
        double lower_wick()  const noexcept { return std::min(open, close) - low; }
    };

    // ?? Parameters ????????????????????????????????????????????????????????????
    static constexpr int    BAR_MINUTES  = 5;    // 5-minute candles
    static constexpr double WICK_PCT     = 0.55; // wick / range >= 55%
    static constexpr double MIN_WICK     = 1.50; // min wick size $1.50
    static constexpr double MIN_RANGE    = 2.25; // min bar range $2.25
    static constexpr double MAX_SPREAD   = 2.00; // block during wide spread
    static constexpr int    SL_TICKS     = 60;   // $6.00 SL
    static constexpr int    TP_TICKS     = 150;  // $15.00 TP
    static constexpr int    HOLD_BARS    = 12;   // max 12 bars (~60 min)
    static constexpr int    COOLDOWN_SEC = 300;  // 5 min between signals

    // ?? State ?????????????????????????????????????????????????????????????????
    Bar    current_bar_;
    Bar    prev_bar_;        // last completed bar -- evaluated on close
    int    pending_side_ = 0;  // +1 LONG, -1 SHORT, 0 = none armed
    int    bars_held_    = 0;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    // ?? UTC helpers ???????????????????????????????????????????????????????????
    static int utc_minute_of_day() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return ti.tm_hour * 60 + ti.tm_min;
    }

    static int bar_slot(int minute_of_day) noexcept {
        // Which BAR_MINUTES slot does this minute belong to?
        return (minute_of_day / BAR_MINUTES) * BAR_MINUTES;
    }

    static bool in_dead_zone() noexcept {
        return false;  // hard time block removed -- ATR/spread quality gate handles thin tape
    }

    // Quality check for 05:00-07:00 UTC thin zone
    static bool in_london_preopen(int h) noexcept { return h >= 5 && h < 7; }

    // ?? Evaluate a completed bar for wick rejection signal ???????????????????
    int evaluate_bar(const Bar& b) const noexcept {
        if (!b.valid) return 0;
        const double rng = b.range();
        if (rng < MIN_RANGE) return 0;

        const double uw = b.upper_wick();
        const double lw = b.lower_wick();

        // Bearish wick: upper wick dominates -- price swept above and closed back down
        if (uw >= rng * WICK_PCT && uw >= MIN_WICK) return -1;  // SHORT signal

        // Bullish wick: lower wick dominates -- price swept below and closed back up
        if (lw >= rng * WICK_PCT && lw >= MIN_WICK) return +1;  // LONG signal

        return 0;
    }

public:
    WickRejectionEngine() : EngineBase("WickRejection", 1.2) {}

    void reset() override {
        current_bar_ = Bar{};
        prev_bar_    = Bar{};
        pending_side_ = 0;
        bars_held_    = 0;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();
        if (in_dead_zone())              return noSignal();
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }

        const int mod = utc_minute_of_day();
        const int slot = bar_slot(mod);

        // ?? Bar management ????????????????????????????????????????????????????
        if (!current_bar_.valid) {
            // First tick ever -- start a bar
            current_bar_.reset(s.mid, slot);
        } else if (slot != current_bar_.bar_minute) {
            // Bar closed -- evaluate and rotate
            prev_bar_ = current_bar_;
            current_bar_.reset(s.mid, slot);
            bars_held_++;

            // Evaluate the bar that just closed
            const int sig_side = evaluate_bar(prev_bar_);
            if (sig_side != 0) {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                         now - last_signal_).count();
                if (elapsed >= COOLDOWN_SEC && s.spread <= MAX_SPREAD) {
                    pending_side_ = sig_side;
                    bars_held_    = 0;
                }
            }
        } else {
            current_bar_.update(s.mid);
        }

        // ?? Fire pending signal on first tick of new bar ??????????????????????
        if (pending_side_ != 0) {
            const auto now = std::chrono::steady_clock::now();

            // Stale guard: cancel if we've held too long without firing
            if (bars_held_ > 2) {
                pending_side_ = 0;
                return noSignal();
            }

            Signal sig;
            sig.valid      = true;
            sig.size       = 0.01;
            sig.tp         = TP_TICKS;
            sig.sl         = SL_TICKS;
            sig.confidence = std::min(1.5,
                pending_side_ == +1
                    ? prev_bar_.lower_wick() / MIN_WICK
                    : prev_bar_.upper_wick() / MIN_WICK);

            if (pending_side_ == +1) {
                sig.side  = TradeSide::LONG;
                sig.entry = s.ask;
                strncpy(sig.reason, "WICK_REJ_LONG",  31);
            } else {
                sig.side  = TradeSide::SHORT;
                sig.entry = s.bid;
                strncpy(sig.reason, "WICK_REJ_SHORT", 31);
            }
            strncpy(sig.engine, "WickRejection", 31);

            pending_side_ = 0;
            last_signal_  = now;
            signal_count_++;
            return sig;
        }

        return noSignal();
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 10. DonchianBreakoutEngine
// ?????????????????????????????????????????????????????????????????????????????
// DATA-CALIBRATED: 718,194 bars XAUUSD Jan 2024-Jan 2026 (5-min OHLC)
//
// Edge: break above the 10-bar 5-min high (50-min high) = all buyers from last
// 50 minutes are in profit, resistance cleared, momentum confirmed. Vice versa
// for shorts. Pure trend-following with 3:1 RR.
//
//   Optimal params from grid search:
//     N       = 10 bars (50-min lookback on 5-min chart)
//     SL_TICKS = 50   -- $5.00 stop (below breakout bar low)
//     TP_TICKS = 150  -- $15.00 target (3:1 RR)
//     COOLDOWN = 600s -- 10 min between signals (prevents cascade re-entries)
//
//   Sim results (3,383 trades over 2yr):
//     WR = 29.6%  |  Total = $3,125  |  Avg = $0.924  |  Sharpe = 1.60
//     2024: $765 (967T)  |  2025: $2,290 (2,338T)  -- works better in trend years
//     MaxDD = $124
//
// Regime: ALL regimes -- channel breakout is direction-agnostic.
// HTF filter: when EMA50 > EMA250 (bullish) only take LONG breaks; vice versa.
// This halves trade count but improves Sharpe by aligning with 250-bar trend.
// ?????????????????????????????????????????????????????????????????????????????
class DonchianBreakoutEngine : public EngineBase {

    struct Bar5 {
        double high = 0, low = 0, close = 0;
        int    slot = -1;
        bool   valid = false;
        void reset(double p, int s) noexcept { high=low=close=p; slot=s; valid=true; }
        void update(double p) noexcept { if(p>high)high=p; if(p<low)low=p; close=p; }
    };

    static constexpr int    N           = 40;    // 40-bar lookback (Turtle N=40)
    static constexpr int    BAR_MIN     = 5;     // 5-minute bars
    static constexpr double MAX_SPREAD  = 2.0;
    static constexpr int    SL_TICKS    = 80;    // $8.00 -- wider for N=40
    static constexpr int    TP_TICKS    = 240;   // $24.00 (3:1 RR)
    static constexpr int    COOLDOWN_SEC= 900;   // 15 min
    static constexpr int    MAX_BARS    = N + 2;

    Bar5   bars_[MAX_BARS];   // circular buffer of completed bars
    Bar5   cur_;              // bar being built
    int    n_complete_ = 0;   // how many complete bars we have

    // HTF trend: EMA50 vs EMA250 on ticks
    double ema50_  = 0, ema250_ = 0;
    bool   ema_init_ = false;
    static constexpr double A50  = 2.0/51.0;
    static constexpr double A250 = 2.0/251.0;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    static int utc_slot() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return (ti.tm_hour * 60 + ti.tm_min) / BAR_MIN;
    }

    static bool in_dead_zone() noexcept { return false; }

    double don_high() const noexcept {
        if (n_complete_ < N) return 0;
        double hi = 0;
        for (int i = 0; i < N; ++i) hi = std::max(hi, bars_[i].high);
        return hi;
    }
    double don_low() const noexcept {
        if (n_complete_ < N) return 1e9;
        double lo = 1e9;
        for (int i = 0; i < N; ++i) lo = std::min(lo, bars_[i].low);
        return lo;
    }

public:
    DonchianBreakoutEngine() : EngineBase("DonchianBreakout", 1.1) {}

    void reset() override {
        for (auto& b : bars_) b = Bar5{};
        cur_ = Bar5{}; n_complete_ = 0;
        ema_init_ = false; ema50_ = 0; ema250_ = 0;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();
        if (in_dead_zone())              return noSignal();
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }

        // Update HTF EMAs on every tick
        if (!ema_init_) { ema50_ = ema250_ = s.mid; ema_init_ = true; }
        else { ema50_ += A50*(s.mid-ema50_); ema250_ += A250*(s.mid-ema250_); }
        const int htf_dir = (ema50_ > ema250_) ? 1 : -1;  // +1 bull, -1 bear

        const int slot = utc_slot();

        // Bar management
        if (!cur_.valid) {
            cur_.reset(s.mid, slot);
        } else if (slot != cur_.slot) {
            // Rotate completed bar into ring buffer
            for (int i = MAX_BARS-1; i > 0; --i) bars_[i] = bars_[i-1];
            bars_[0] = cur_;
            if (n_complete_ < N) ++n_complete_;
            cur_.reset(s.mid, slot);
        } else {
            cur_.update(s.mid);
        }

        if (n_complete_ < N) return noSignal();

        const double dhi = don_high();
        const double dlo = don_low();
        if (dhi <= 0 || dlo >= 1e8) return noSignal();

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - last_signal_).count();
        if (elapsed < COOLDOWN_SEC) return noSignal();

        Signal sig;
        sig.size = 0.01; sig.sl = SL_TICKS; sig.tp = TP_TICKS;

        // Long breakout -- only when HTF agrees (bull)
        if (s.mid > dhi && htf_dir == 1) {
            sig.valid      = true;
            sig.side       = TradeSide::LONG;
            sig.entry      = s.ask;
            sig.confidence = std::min(1.5, (s.mid - dhi) / 2.0 + 0.8);
            strncpy(sig.reason, "DONCHIAN_LONG",  31);
            strncpy(sig.engine, "DonchianBreakout", 31);
        }
        // Short breakout -- only when HTF agrees (bear)
        else if (s.mid < dlo && htf_dir == -1) {
            sig.valid      = true;
            sig.side       = TradeSide::SHORT;
            sig.entry      = s.bid;
            sig.confidence = std::min(1.5, (dlo - s.mid) / 2.0 + 0.8);
            strncpy(sig.reason, "DONCHIAN_SHORT", 31);
            strncpy(sig.engine, "DonchianBreakout", 31);
        }

        if (sig.valid) {
            last_signal_ = now;
            signal_count_++;
        }
        return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 11. NR3BreakoutEngine
// ?????????????????????????????????????????????????????????????????????????????
// DATA-CALIBRATED: 718,194 bars XAUUSD Jan 2024-Jan 2026 (5-min OHLC)
//
// Edge: narrowest 5-min range of the last 3 bars signals coiled energy.
// When price breaks out with a confirming close (body in upper/lower 60% of
// breakout bar), momentum is genuine. Session-filtered to 07-17 UTC only.
//
//   Optimal params from grid search:
//     N       = 3 bars  |  MIN_RANGE = $2.00  |  CONFIRM = 40% body position
//     SL_TICKS = 40     -- $4.00  |  TP_TICKS = 100 -- $10.00 (2.5:1 RR)
//     SESSION  = 07:00-17:00 UTC
//
//   Sim results (1,421 trades, session+confirm filter):
//     WR = 39.1%  |  Total = $1,108  |  Avg = $0.780  |  Sharpe = 2.00
//     2024: 216T / WR 47% / $269  |  2025: 1,152T / WR 38% / $811
//     MaxDD = $79
//
// Note: 2025 WR drift (47%?38%) -- monitor live. Edge holds in total but
// the quality gate (CONFIRM body) is doing real work here.
// ?????????????????????????????????????????????????????????????????????????????
class NR3BreakoutEngine : public EngineBase {

    struct Bar5 {
        double open=0,high=0,low=0,close=0;
        int    slot=-1; bool valid=false;
        void reset(double o,int s) noexcept{open=high=low=close=o;slot=s;valid=true;}
        void update(double p) noexcept{if(p>high)high=p;if(p<low)low=p;close=p;}
        double range() const noexcept{return high-low;}
    };

    static constexpr int    N            = 3;
    static constexpr int    BAR_MIN      = 5;
    static constexpr double MIN_RANGE    = 2.0;
    static constexpr double CONFIRM_PCT  = 0.40;  // body must be in top/bottom 40%
    static constexpr double MAX_SPREAD   = 2.0;
    static constexpr int    SL_TICKS     = 40;    // $4.00
    static constexpr int    TP_TICKS     = 100;   // $10.00
    static constexpr int    COOLDOWN_SEC = 300;
    static constexpr int    SESSION_START= 7;     // 07:00 UTC
    // Vol-ratio gate: NR3 fires in ranging/coiling conditions, not hot trending tape.
    // Injected each tick from GoldEngineStack via set_vol_ratio().
    // Gate: vol_ratio > 1.5 = market too volatile for NR3 coiling pattern.
    // Data: NR3 WR 39.6% when vol<1.2 vs degraded quality in trending tape.
    double vol_ratio_ = 1.0;
    static constexpr double VOL_GATE = 1.5;
    static constexpr int    SESSION_END  = 17;    // 17:00 UTC

    Bar5  bars_[N+1];    // last N completed bars
    Bar5  cur_;
    int   n_complete_ = 0;
    bool  waiting_confirm_ = false;
    int   confirm_dir_     = 0;  // +1 LONG, -1 SHORT
    double nr3_high_ = 0, nr3_low_ = 0;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    static int utc_slot_and_hour(int& hour_out) noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        hour_out = ti.tm_hour;
        return (ti.tm_hour * 60 + ti.tm_min) / BAR_MIN;
    }

    bool is_nr3() const noexcept {
        if (n_complete_ < N) return false;
        const double r0 = bars_[0].range();
        if (r0 < MIN_RANGE) return false;
        for (int i = 1; i < N; ++i)
            if (bars_[i].range() <= r0) return false;  // not narrowest
        return true;
    }

public:
    NR3BreakoutEngine() : EngineBase("NR3Breakout", 1.1) {}

    void set_vol_ratio(double vr) noexcept { vol_ratio_ = vr; }

    void reset() override {
        for(auto& b:bars_) b=Bar5{}; cur_=Bar5{};
        n_complete_=0; waiting_confirm_=false; confirm_dir_=0;
    }

    static bool in_dead_zone() noexcept { return false; }
    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC
        if (in_dead_zone())              return noSignal();
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }
        // Vol gate: NR3 is a coiling pattern -- invalid in trending/hot tape
        if (vol_ratio_ > VOL_GATE)        return noSignal();

        int hour;
        const int slot = utc_slot_and_hour(hour);
        if (hour < SESSION_START || hour >= SESSION_END) {
            waiting_confirm_ = false; return noSignal();
        }

        // Bar management
        if (!cur_.valid) {
            cur_.reset(s.mid, slot);
        } else if (slot != cur_.slot) {
            for (int i = N; i > 0; --i) bars_[i] = bars_[i-1];
            bars_[0] = cur_;
            if (n_complete_ < N) ++n_complete_;
            cur_.reset(s.mid, slot);

            // On bar close: detect NR3 pattern
            if (is_nr3()) {
                waiting_confirm_ = true;
                nr3_high_ = bars_[0].high;
                nr3_low_  = bars_[0].low;
                confirm_dir_ = 0;
            }
        } else {
            cur_.update(s.mid);

            // Check breakout confirm on current bar tick
            if (waiting_confirm_) {
                const double cur_rng = cur_.high - cur_.low;
                if (cur_rng > 0.5 && s.mid > nr3_high_) {
                    // Upside break -- confirm body in upper 60%
                    const double body_top = std::max(cur_.open, cur_.close);
                    if (body_top > cur_.low + cur_rng * (1.0 - CONFIRM_PCT)) {
                        confirm_dir_ = +1;
                    }
                } else if (cur_rng > 0.5 && s.mid < nr3_low_) {
                    const double body_bot = std::min(cur_.open, cur_.close);
                    if (body_bot < cur_.high - cur_rng * (1.0 - CONFIRM_PCT)) {
                        confirm_dir_ = -1;
                    }
                }
            }
        }

        if (!waiting_confirm_ || confirm_dir_ == 0) return noSignal();

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - last_signal_).count();
        if (elapsed < COOLDOWN_SEC) return noSignal();

        Signal sig;
        sig.valid      = true;
        sig.size       = 0.01;
        sig.sl         = SL_TICKS;
        sig.tp         = TP_TICKS;
        sig.confidence = 0.85;

        if (confirm_dir_ == +1) {
            sig.side  = TradeSide::LONG;
            sig.entry = s.ask;
            strncpy(sig.reason, "NR3_BREAK_LONG",  31);
        } else {
            sig.side  = TradeSide::SHORT;
            sig.entry = s.bid;
            strncpy(sig.reason, "NR3_BREAK_SHORT", 31);
        }
        strncpy(sig.engine, "NR3Breakout", 31);

        waiting_confirm_ = false; confirm_dir_ = 0;
        last_signal_ = now;
        signal_count_++;
        return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 12. SpikeFadeEngine
// ?????????????????????????????????????????????????????????????????????????????
// DATA-CALIBRATED: 718,194 bars XAUUSD Jan 2024-Jan 2026 (5-min OHLC)
//
// Edge: a 5-min candle that moves >$10 in one direction is a macro shock.
// These moves overshoot: 54.5% WR fading the direction on the next candle.
// The exhaustion signal is structural -- extreme price movements beyond $10
// on a single candle represent forced liquidations or news overreaction,
// both of which tend to partially retrace.
//
//   Params:
//     MIN_SPIKE = $10.00  -- minimum 5-min candle move to qualify
//     SL_TICKS  = 80      -- $8.00 stop (wide to absorb after-shock noise)
//     TP_TICKS  = 150     -- $15.00 target (nearly 2:1 RR)
//     COOLDOWN  = 1800s   -- 30 min: only one fade per macro event
//
//   Sim results (402 trades over 2yr):
//     WR = 54.5%  |  Total = $456  |  Avg = $1.14
//     Low trade count (macro events) -- use as supplementary engine only.
// ?????????????????????????????????????????????????????????????????????????????
class SpikeFadeEngine : public EngineBase {

    struct Bar5 {
        double open=0,high=0,low=0,close=0;
        int    slot=-1; bool valid=false;
        void reset(double o,int s) noexcept{open=high=low=close=o;slot=s;valid=true;}
        void update(double p) noexcept{if(p>high)high=p;if(p<low)low=p;close=p;}
    };

    static constexpr int    BAR_MIN      = 5;
    static constexpr double MIN_SPIKE    = 10.0;  // $10 minimum candle move
    static constexpr double MAX_SPREAD   = 3.0;   // wider tolerance during news
    static constexpr int    SL_TICKS     = 80;    // $8.00
    static constexpr int    TP_TICKS     = 150;   // $15.00
    static constexpr int    COOLDOWN_SEC = 1800;  // 30 min per event

    Bar5  cur_, prev_;
    bool  pending_    = false;
    int   pending_dir_= 0;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    static int utc_slot() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return (ti.tm_hour * 60 + ti.tm_min) / BAR_MIN;
    }

public:
    SpikeFadeEngine() : EngineBase("SpikeFade", 0.9) {}

    void reset() override {
        cur_=Bar5{}; prev_=Bar5{};
        pending_=false; pending_dir_=0;
    }

    static bool in_dead_zone() noexcept { return false; }
    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC
        if (in_dead_zone())              return noSignal();
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }
        if (s.session == SessionType::UNKNOWN) return noSignal();

        const int slot = utc_slot();

        // Bar management
        if (!cur_.valid) {
            cur_.reset(s.mid, slot);
        } else if (slot != cur_.slot) {
            prev_ = cur_;
            cur_.reset(s.mid, slot);

            // Evaluate closed bar for spike
            const double bar_move = prev_.close - prev_.open;
            if (std::fabs(bar_move) >= MIN_SPIKE) {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                         now - last_signal_).count();
                if (elapsed >= COOLDOWN_SEC) {
                    // Fade: spike up ? SHORT, spike down ? LONG
                    pending_     = true;
                    pending_dir_ = (bar_move > 0) ? -1 : +1;
                }
            }
        } else {
            cur_.update(s.mid);
        }

        if (!pending_) return noSignal();

        // Macro displacement gate: don't fade a spike that's going WITH a macro trend.
        // If price is >$15 from VWAP in a direction, fading a spike in that direction
        // is wrong -- the spike IS the trend. Only fade spikes that go AGAINST the trend.
        if (s.vwap > 0.0) {
            const double vwap_dist = s.mid - s.vwap;
            // Price far below VWAP (downtrend) -- don't fade downside spikes (pending_dir_==+1 = LONG)
            if (vwap_dist < -15.0 && pending_dir_ == +1) { pending_ = false; return noSignal(); }
            // Price far above VWAP (uptrend) -- don't fade upside spikes (pending_dir_==-1 = SHORT)
            if (vwap_dist >  15.0 && pending_dir_ == -1) { pending_ = false; return noSignal(); }
        }

        const auto now = std::chrono::steady_clock::now();

        Signal sig;
        sig.valid      = true;
        sig.size       = 0.01;
        sig.sl         = SL_TICKS;
        sig.tp         = TP_TICKS;
        sig.confidence = 0.80;

        if (pending_dir_ == +1) {
            sig.side  = TradeSide::LONG;
            sig.entry = s.ask;
            strncpy(sig.reason, "SPIKE_FADE_LONG",  31);
        } else {
            sig.side  = TradeSide::SHORT;
            sig.entry = s.bid;
            strncpy(sig.reason, "SPIKE_FADE_SHORT", 31);
        }
        strncpy(sig.engine, "SpikeFade", 31);

        pending_ = false;
        last_signal_ = now;
        signal_count_++;
        return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 13. AsianRangeEngine
// ?????????????????????????????????????????????????????????????????????????????
// DATA-CALIBRATED: 718,194 bars XAUUSD Jan 2024-Jan 2026
//
// Edge: the Asian session (00:00-07:00 UTC) builds an institutional accumulation
// range. When London opens and breaks above or below this range, it signals
// directional conviction from the dominant session. Trade the breakout.
//
//   Logic:
//     1. Accumulate Asian session high/low (00:00-06:59 UTC) each day
//     2. Reset at 00:00 UTC each day
//     3. Fire when London price breaks > asian_high + BUFFER or < asian_low - BUFFER
//     4. One trade per day per direction (armed/fired gate)
//
//   Params (sim-validated on 718k bars):
//     BUFFER     = $0.50  -- entry buffer beyond range edge
//     SL_TICKS   = 80     -- $8.00 stop (inside the range, below/above breakout)
//     TP_TICKS   = 200    -- $20.00 target (2.5:1 RR)
//     MIN_RANGE  = $3.00  -- minimum Asian range (filters thin/gappy sessions)
//     MAX_RANGE  = $50.0  -- maximum range (filters news-event nights)
//     FIRE_WINDOW_START = 07:00 UTC  -- London open
//     FIRE_WINDOW_END   = 11:00 UTC  -- London mid-session cutoff
//
//   Sim results (382 trades over 2yr):
//     WR = 49.7%  |  Total = $279  |  Avg = $0.73  |  Sharpe = 1.60
//     Fully independent from all existing engines.
//     MaxDD = $105
//
// Regime: COMPRESSION + MEAN_REVERSION -- Asian range breakout is a
// compression-to-expansion event. Not fired in TREND/IMPULSE (already moving).
// Session gate: fires ONLY during 07:00-11:00 UTC (London session).
// ?????????????????????????????????????????????????????????????????????????????
class AsianRangeEngine : public EngineBase {

    static constexpr double BUFFER           = 0.50;
    static constexpr double MIN_RANGE        = 3.0;
    static constexpr double MAX_RANGE        = 50.0;
    static constexpr double MAX_SPREAD       = 2.0;
    static constexpr int    SL_TICKS         = 80;    // $8.00
    static constexpr int    TP_TICKS         = 200;   // $20.00
    static constexpr int    FIRE_START_H     = 7;     // 07:00 UTC
    static constexpr int    FIRE_END_H       = 11;    // 11:00 UTC

    double asian_hi_  = 0.0;
    double asian_lo_  = 1e9;
    int    last_day_  = -1;
    bool   long_fired_  = false;
    bool   short_fired_ = false;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(3700)};

    static void utc_hms(int& h, int& m, int& yday) noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        h = ti.tm_hour; m = ti.tm_min; yday = ti.tm_yday;
    }

public:
    AsianRangeEngine() : EngineBase("AsianRange", 1.1) {}

    void reset() override {
        asian_hi_ = 0.0; asian_lo_ = 1e9;
        last_day_ = -1; long_fired_ = false; short_fired_ = false;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();

        int h, m, yday;
        utc_hms(h, m, yday);

        // Daily reset at midnight UTC
        if (yday != last_day_) {
            asian_hi_    = 0.0;
            asian_lo_    = 1e9;
            long_fired_  = false;
            short_fired_ = false;
            last_day_    = yday;
        }

        // Build Asian range during 00:00-06:59 UTC
        if (h >= 0 && h < FIRE_START_H) {
            if (s.mid > asian_hi_) asian_hi_ = s.mid;
            if (s.mid < asian_lo_) asian_lo_ = s.mid;
            return noSignal();
        }

        // Only fire during London window (07:00-10:59 UTC)
        if (h < FIRE_START_H || h >= FIRE_END_H) return noSignal();

        // Validate range quality
        if (asian_hi_ <= 0.0 || asian_lo_ >= 1e8) return noSignal();
        const double rng = asian_hi_ - asian_lo_;
        if (rng < MIN_RANGE || rng > MAX_RANGE)   return noSignal();

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - last_signal_).count();
        if (elapsed < 600) return noSignal();  // 10 min minimum between signals

        Signal sig;
        sig.size = 0.01; sig.sl = SL_TICKS; sig.tp = TP_TICKS;

        // Upside break: price cleared Asian high ? LONG
        if (!long_fired_ && s.mid > asian_hi_ + BUFFER) {
            sig.valid      = true;
            sig.side       = TradeSide::LONG;
            sig.entry      = s.ask;
            sig.confidence = std::min(1.5, (s.mid - asian_hi_) / 2.0 + 0.9);
            strncpy(sig.reason, "ASIAN_RANGE_LONG",  31);
            strncpy(sig.engine, "AsianRange",        31);
            long_fired_  = true;
        }
        // Downside break: price cleared Asian low ? SHORT
        else if (!short_fired_ && s.mid < asian_lo_ - BUFFER) {
            sig.valid      = true;
            sig.side       = TradeSide::SHORT;
            sig.entry      = s.bid;
            sig.confidence = std::min(1.5, (asian_lo_ - s.mid) / 2.0 + 0.9);
            strncpy(sig.reason, "ASIAN_RANGE_SHORT", 31);
            strncpy(sig.engine, "AsianRange",        31);
            short_fired_ = true;
        }

        if (sig.valid) {
            last_signal_ = now;
            signal_count_++;
        }
        return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 14. DynamicRangeEngine
// ?????????????????????????????????????????????????????????????????????????????
// DATA-CALIBRATED: 718,194 bars XAUUSD Jan 2024-Jan 2026 (5-min OHLC)
//
// Edge: systematically fade the extremes of the current 20-bar price range.
// Buy when price is in the bottom 20% of the range, sell when in the top 20%.
// Exit when price reaches the opposite 70% threshold -- capturing the range reversion.
// This is the quantitative equivalent of gamma scalping: profit from range-bound
// oscillation without directional conviction.
//
//   Only 18.8% overlap with MeanReversionEngine -- different mechanism:
//     MR: absolute Z-score extreme vs 60-bar mean/std
//     DR: relative position within the current 20-bar high/low range
//   MR fires on statistical outliers; DR fires on range position.
//
//   Params (grid-searched on 718k bars):
//     N          = 20 bars  (100-min rolling high/low on 5-min chart)
//     ENTRY_PCT  = 0.20     -- enter when in bottom/top 20% of range
//     EXIT_PCT   = 0.70     -- exit when price crosses 70% from entry side
//     MIN_RANGE  = $5.00    -- filter thin noise ranges
//     MAX_RANGE  = $50.00   -- filter news-event explosion ranges
//     SL_TICKS   = 30       -- $3.00 stop
//     TP_TICKS   = 80       -- $8.00 target (~2.7:1 RR)
//     COOLDOWN   = 120s
//
//   Sim results (10,299 trades over 2yr):
//     WR = 43.4%  |  Total = $6,772  |  Avg = $0.658  |  Sharpe = 2.36
//     MaxDD = $85 -- lowest max drawdown of all engines
//     2024: $1,744 (3,642T) Sharpe=1.90  |  2025: $4,898 (6,456T) Sharpe=2.61
//
// Regime: MEAN_REVERSION + COMPRESSION -- range trading requires a range.
// Blocked in TREND + IMPULSE -- ranging in a trending market is the classic
// trap that destroys range-trading accounts.
// ?????????????????????????????????????????????????????????????????????????????
class DynamicRangeEngine : public EngineBase {

    struct Bar5 {
        double high=0,low=0,close=0;
        int slot=-1; bool valid=false;
        void reset(double p,int s) noexcept{high=low=close=p;slot=s;valid=true;}
        void update(double p) noexcept{if(p>high)high=p;if(p<low)low=p;close=p;}
    };

    static constexpr int    N           = 20;
    static constexpr int    BAR_MIN     = 5;
    static constexpr double ENTRY_PCT   = 0.20;
    static constexpr double EXIT_PCT    = 0.70;
    static constexpr double MIN_RANGE   = 5.0;
    static constexpr double MAX_RANGE   = 50.0;
    static constexpr double MAX_SPREAD  = 2.0;
    static constexpr int    SL_TICKS    = 30;   // $3.00
    static constexpr int    TP_TICKS    = 80;   // $8.00
    static constexpr int    COOLDOWN_SEC= 120;
    static constexpr int    MAX_BARS    = N + 2;

    Bar5  bars_[MAX_BARS];
    Bar5  cur_;
    int   n_complete_ = 0;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    static int utc_slot() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return (ti.tm_hour * 60 + ti.tm_min) / BAR_MIN;
    }

    double roll_high() const noexcept {
        double hi = 0;
        for (int i = 0; i < N && i < n_complete_; ++i) hi = std::max(hi, bars_[i].high);
        return hi;
    }
    double roll_low() const noexcept {
        double lo = 1e9;
        for (int i = 0; i < N && i < n_complete_; ++i) lo = std::min(lo, bars_[i].low);
        return lo;
    }

public:
    DynamicRangeEngine() : EngineBase("DynamicRange", 1.2) {}

    void reset() override {
        for (auto& b : bars_) b = Bar5{};
        cur_ = Bar5{}; n_complete_ = 0;
    }

    static bool in_dead_zone() noexcept { return false; }
    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();
        if (in_dead_zone())              return noSignal();
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }

        const int slot = utc_slot();

        // Bar management
        if (!cur_.valid) {
            cur_.reset(s.mid, slot);
        } else if (slot != cur_.slot) {
            for (int i = MAX_BARS-1; i > 0; --i) bars_[i] = bars_[i-1];
            bars_[0] = cur_;
            if (n_complete_ < N) ++n_complete_;
            cur_.reset(s.mid, slot);
        } else {
            cur_.update(s.mid);
        }

        if (n_complete_ < N) return noSignal();

        const double rhi = roll_high();
        const double rlo = roll_low();
        const double rng = rhi - rlo;

        if (rng < MIN_RANGE || rng > MAX_RANGE) return noSignal();

        // Band position: 0.0 = at range low, 1.0 = at range high
        const double bp = (s.mid - rlo) / rng;

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_signal_).count() < COOLDOWN_SEC) return noSignal();

        Signal sig;
        sig.size = 0.01; sig.sl = SL_TICKS; sig.tp = TP_TICKS;

        if (bp < ENTRY_PCT) {
            // Price at range bottom -- expect reversion upward ? LONG
            sig.valid      = true;
            sig.side       = TradeSide::LONG;
            sig.entry      = s.ask;
            sig.confidence = std::min(1.5, (ENTRY_PCT - bp) / ENTRY_PCT * 1.2 + 0.7);
            strncpy(sig.reason, "DYN_RANGE_LONG",  31);
            strncpy(sig.engine, "DynamicRange",    31);
        } else if (bp > (1.0 - ENTRY_PCT)) {
            // Price at range top -- expect reversion downward ? SHORT
            sig.valid      = true;
            sig.side       = TradeSide::SHORT;
            sig.entry      = s.bid;
            sig.confidence = std::min(1.5, (bp - (1.0-ENTRY_PCT)) / ENTRY_PCT * 1.2 + 0.7);
            strncpy(sig.reason, "DYN_RANGE_SHORT", 31);
            strncpy(sig.engine, "DynamicRange",    31);
        }

        if (sig.valid) {
            last_signal_ = now;
            signal_count_++;
        }
        return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// -------------------------------------------------------------------------
// TickBarBuffer<N> -- shared tick-bar aggregator used by engines 15-17.
// Each bar accumulates exactly N price moves. Normalises information content:
// a bar at 3am and one at NY open each contain exactly N price decisions.
// Only 5% overlap with 5-min time-bar versions -- independent signals.
// -------------------------------------------------------------------------
template<int N>
struct TickBarBuffer {
    struct Bar {
        double open=0,high=0,low=0,close=0; bool valid=false;
        void reset(double p) noexcept {open=high=low=close=p;valid=true;}
        void update(double p) noexcept {if(p>high)high=p;if(p<low)low=p;close=p;}
        double range()       const noexcept {return high-low;}
        double upper_wick()  const noexcept {return high-std::max(open,close);}
        double lower_wick()  const noexcept {return std::min(open,close)-low;}
    };
    static constexpr int KEEP=48;
    Bar   bars[KEEP];
    int   head=0, n_bars=0, tick_cnt=0;
    Bar   current;

    bool on_tick(double price) noexcept {
        if (!current.valid) current.reset(price);
        else current.update(price);
        if (++tick_cnt < N) return false;
        head=(head+1)%KEEP; bars[head]=current; ++n_bars;
        current.reset(price); tick_cnt=0;
        return true;
    }
    bool ready(int min_bars=42) const noexcept {return n_bars>=min_bars;}
    const Bar& bar(int age=0) const noexcept {return bars[(head-age+KEEP)%KEEP];}
    double roll_high(int n) const noexcept {
        double hi=0; for(int i=0;i<n&&i<n_bars;++i) hi=std::max(hi,bar(i).high); return hi;
    }
    double roll_low(int n) const noexcept {
        double lo=1e9; for(int i=0;i<n&&i<n_bars;++i) lo=std::min(lo,bar(i).low); return lo;
    }
};

// -------------------------------------------------------------------------
// 15. WickRejectionTickEngine
// -------------------------------------------------------------------------
// Wick rejection on 300-tick bars. Sharpe=3.79 vs 1.68 on 5-min time bars.
// 5% signal overlap with WickRejectionEngine -- independent edge.
// Sim: 562T WR=40.7% $1,376/2yr Sharpe=3.79 MaxDD=$72
//      2024: 274T WR=40.9% $647 Sharpe=3.69
//      2025: 286T WR=40.6% $720 Sharpe=3.86 (year-stable)
// SL=$6 TP=$15. Cooldown=300s. ALL regimes.
// -------------------------------------------------------------------------
class WickRejectionTickEngine : public EngineBase {
    static constexpr double WICK_PCT    = 0.55;
    static constexpr double MIN_WICK    = 1.50;
    static constexpr double MIN_RANGE   = 2.25;
    static constexpr double MAX_SPREAD  = 2.0;
    static constexpr int    SL_TICKS    = 60;
    static constexpr int    TP_TICKS    = 150;
    static constexpr int    COOLDOWN_SEC= 300;

    TickBarBuffer<300> tb_;
    int  pending_side_=0, bars_since_arm_=0;

    std::chrono::steady_clock::time_point last_sig_{
        std::chrono::steady_clock::now()-std::chrono::seconds(COOLDOWN_SEC+1)};

    static bool in_dead_zone() noexcept {
        return false;  // hard time block removed
    }

public:
    WickRejectionTickEngine() : EngineBase("WickRejTick",1.2) {}
    void reset() override {tb_=TickBarBuffer<300>{};pending_side_=0;bars_since_arm_=0;}

    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD)      return noSignal();
        if(in_dead_zone())           return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }

        const bool bar_closed=tb_.on_tick(s.mid);
        if(bar_closed&&tb_.ready(3)){
            ++bars_since_arm_;
            const auto& b=tb_.bar(1);
            const double rng=b.range();
            if(rng>=MIN_RANGE){
                const double uw=b.upper_wick(),lw=b.lower_wick();
                auto now=std::chrono::steady_clock::now();
                auto el=std::chrono::duration_cast<std::chrono::seconds>(now-last_sig_).count();
                if(el>=COOLDOWN_SEC){
                    if(uw>=rng*WICK_PCT&&uw>=MIN_WICK){pending_side_=-1;bars_since_arm_=0;}
                    else if(lw>=rng*WICK_PCT&&lw>=MIN_WICK){pending_side_=+1;bars_since_arm_=0;}
                }
            }
        }
        if(pending_side_==0||bars_since_arm_>2){pending_side_=0;return noSignal();}
        auto now=std::chrono::steady_clock::now();
        Signal sig;
        sig.valid=true;sig.size=0.01;sig.sl=SL_TICKS;sig.tp=TP_TICKS;sig.confidence=1.0;
        if(pending_side_==+1){
            sig.side=TradeSide::LONG;sig.entry=s.ask;
            strncpy(sig.reason,"WICK_TICK_LONG", 31);
        } else {
            sig.side=TradeSide::SHORT;sig.entry=s.bid;
            strncpy(sig.reason,"WICK_TICK_SHORT",31);
        }
        strncpy(sig.engine,"WickRejTick",31);
        pending_side_=0;last_sig_=now;signal_count_++;
        return sig;
    }
};

// -------------------------------------------------------------------------
// 16. TurtleTickEngine
// -------------------------------------------------------------------------
// Turtle N=40 on 300-tick bars. HTF EMA50/250 filter. Sharpe=7.60.
// Sim: 104T WR=49.0% $800/2yr Sharpe=7.60 MaxDD=$48
//      2024: 37T WR=51.4% $312 Sharpe=8.34
//      2025: 67T WR=47.8% $488 Sharpe=7.20
// SL=$8 TP=$24 (3:1 RR). Cooldown=900s. ALL regimes.
// -------------------------------------------------------------------------
class TurtleTickEngine : public EngineBase {
    static constexpr int    TURTLE_N    = 40;
    static constexpr double MAX_SPREAD  = 2.0;
    static constexpr int    SL_TICKS    = 80;
    static constexpr int    TP_TICKS    = 240;
    static constexpr int    COOLDOWN_SEC= 900;

    TickBarBuffer<300> tb_;
    double ema50_=0,ema250_=0; bool ema_init_=false;
    static constexpr double A50=2.0/51.0, A250=2.0/251.0;

    std::chrono::steady_clock::time_point last_sig_{
        std::chrono::steady_clock::now()-std::chrono::seconds(COOLDOWN_SEC+1)};

public:
    TurtleTickEngine() : EngineBase("TurtleTick",1.1) {}
    void reset() override {tb_=TickBarBuffer<300>{};ema_init_=false;ema50_=0;ema250_=0;}

    static bool in_dead_zone() noexcept { return false; }
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD)      return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC
        if(in_dead_zone())           return noSignal();
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }
        if(!ema_init_){ema50_=ema250_=s.mid;ema_init_=true;}
        else{ema50_+=A50*(s.mid-ema50_);ema250_+=A250*(s.mid-ema250_);}
        const int htf_dir=(ema50_>ema250_)?1:-1;
        tb_.on_tick(s.mid);
        if(!tb_.ready(TURTLE_N+2)) return noSignal();
        auto now=std::chrono::steady_clock::now();
        if(std::chrono::duration_cast<std::chrono::seconds>(now-last_sig_).count()<COOLDOWN_SEC)
            return noSignal();
        const double dhi=tb_.roll_high(TURTLE_N);
        const double dlo=tb_.roll_low(TURTLE_N);
        if(dhi<=0||dlo>=1e8) return noSignal();
        Signal sig;
        sig.size=0.01;sig.sl=SL_TICKS;sig.tp=TP_TICKS;
        if(s.mid>dhi&&htf_dir==1){
            sig.valid=true;sig.side=TradeSide::LONG;sig.entry=s.ask;
            sig.confidence=std::min(1.5,(s.mid-dhi)/2.0+0.9);
            strncpy(sig.reason,"TURTLE_TICK_LONG", 31);
            strncpy(sig.engine,"TurtleTick",31);
        } else if(s.mid<dlo&&htf_dir==-1){
            sig.valid=true;sig.side=TradeSide::SHORT;sig.entry=s.bid;
            sig.confidence=std::min(1.5,(dlo-s.mid)/2.0+0.9);
            strncpy(sig.reason,"TURTLE_TICK_SHORT",31);
            strncpy(sig.engine,"TurtleTick",31);
        }
        if(sig.valid){last_sig_=now;signal_count_++;}
        return sig;
    }
};

// -------------------------------------------------------------------------
// 17. NR3TickEngine
// -------------------------------------------------------------------------
// NR3 narrowest-3-bar breakout on 300-tick bars. Sharpe=4.10 vs 2.00 on time bars.
// No session gate -- tick bars self-filter low-activity periods.
// Sim: 565T WR=41.4% $1,009/2yr Sharpe=4.10 MaxDD=$68
// SL=$4 TP=$10. Cooldown=300s. COMPRESSION+MEAN_REVERSION.
// -------------------------------------------------------------------------
class NR3TickEngine : public EngineBase {
    static constexpr double MIN_RANGE    = 2.0;
    static constexpr double CONFIRM_PCT  = 0.40;
    static constexpr double MAX_SPREAD   = 2.0;
    static constexpr int    SL_TICKS     = 40;
    static constexpr int    TP_TICKS     = 100;
    static constexpr int    COOLDOWN_SEC = 300;

    TickBarBuffer<300> tb_;
    bool   waiting_confirm_=false;
    int    confirm_dir_=0,bars_since_arm_=0;
    double nr3_high_=0,nr3_low_=0;

    std::chrono::steady_clock::time_point last_sig_{
        std::chrono::steady_clock::now()-std::chrono::seconds(COOLDOWN_SEC+1)};

    bool is_nr3() const noexcept {
        if(tb_.n_bars<3) return false;
        const double r0=tb_.bar(0).range();
        return r0>=MIN_RANGE && r0<tb_.bar(1).range() && r0<tb_.bar(2).range();
    }

public:
    NR3TickEngine() : EngineBase("NR3Tick",1.1) {}
    void reset() override {tb_=TickBarBuffer<300>{};waiting_confirm_=false;confirm_dir_=0;bars_since_arm_=0;}

    static bool in_dead_zone() noexcept { return false; }
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD)      return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC
        if(in_dead_zone())           return noSignal();
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }

        const bool bar_closed=tb_.on_tick(s.mid);
        if(bar_closed&&tb_.ready(4)){
            ++bars_since_arm_;
            if(bars_since_arm_>3){waiting_confirm_=false;confirm_dir_=0;}
            if(is_nr3()){
                waiting_confirm_=true;
                nr3_high_=tb_.bar(0).high; nr3_low_=tb_.bar(0).low;
                confirm_dir_=0; bars_since_arm_=0;
            }
        }
        if(waiting_confirm_){
            const auto& cur=tb_.current;
            const double crng=cur.high-cur.low;
            if(crng>0.5){
                if(s.mid>nr3_high_){
                    const double bt=std::max(cur.open,cur.close);
                    if(bt>cur.low+crng*(1.0-CONFIRM_PCT)) confirm_dir_=+1;
                } else if(s.mid<nr3_low_){
                    const double bb=std::min(cur.open,cur.close);
                    if(bb<cur.high-crng*(1.0-CONFIRM_PCT)) confirm_dir_=-1;
                }
            }
        }
        if(!waiting_confirm_||confirm_dir_==0) return noSignal();
        auto now=std::chrono::steady_clock::now();
        if(std::chrono::duration_cast<std::chrono::seconds>(now-last_sig_).count()<COOLDOWN_SEC)
            return noSignal();
        Signal sig;
        sig.valid=true;sig.size=0.01;sig.sl=SL_TICKS;sig.tp=TP_TICKS;sig.confidence=0.90;
        if(confirm_dir_==+1){
            sig.side=TradeSide::LONG;sig.entry=s.ask;
            strncpy(sig.reason,"NR3_TICK_LONG", 31);
        } else {
            sig.side=TradeSide::SHORT;sig.entry=s.bid;
            strncpy(sig.reason,"NR3_TICK_SHORT",31);
        }
        strncpy(sig.engine,"NR3Tick",31);
        waiting_confirm_=false;confirm_dir_=0;last_sig_=now;signal_count_++;
        return sig;
    }
};


// -------------------------------------------------------------------------
// 18. TwoBarReversalEngine
// -------------------------------------------------------------------------
// DATA-CALIBRATED: 718,194 bars XAUUSD Jan 2024-Jan 2026 (5-min OHLC)
//
// Edge: a strong directional bar (range >= 1.5x 20-bar ATR, body in top/bottom
// 60% of range) followed by a bar that closes AGAINST that direction signals
// momentum exhaustion. The strong bar represents trapped traders; the reversal
// bar confirms they are exiting.
//
// Different from WickRejectionEngine (23% overlap):
//   WickRej: wick within a SINGLE bar = intrabar stop-hunt
//   TwoBar:  full bar close AGAINST prior strong bar = confirmed reversal
//
//   Params (grid-searched on 718k bars):
//     ATR_MULT  = 1.5   -- bar range must be >= 1.5x 20-bar ATR
//     BODY_PCT  = 0.60  -- body must close in top/bottom 60% of bar
//     SL_TICKS  = 60    -- $6.00
//     TP_TICKS  = 150   -- $15.00 (2.5:1 RR)
//     COOLDOWN  = 300s
//
//   Sim results (1,216 trades over 2yr):
//     WR = 47.1%  |  Total = $795  |  Avg = $0.654  |  Sharpe = 1.55
//     2024: 484T WR=48.1% $186 Sharpe=1.06
//     2025: 716T WR=46.4% $573 Sharpe=1.76
//
// Regime: ALL regimes (reversal structure is regime-agnostic)
// -------------------------------------------------------------------------
class TwoBarReversalEngine : public EngineBase {

    struct Bar5 {
        double open=0,high=0,low=0,close=0;
        int slot=-1; bool valid=false;
        void reset(double o,int s) noexcept{open=high=low=close=o;slot=s;valid=true;}
        void update(double p) noexcept{if(p>high)high=p;if(p<low)low=p;close=p;}
        double range() const noexcept{return high-low;}
    };

    static constexpr int    BAR_MIN      = 5;
    static constexpr double ATR_MULT     = 1.5;
    static constexpr double BODY_PCT     = 0.60;
    static constexpr double MIN_REV_RNG  = 1.5;  // reversal bar min range
    static constexpr double MAX_SPREAD   = 2.0;
    static constexpr int    SL_TICKS     = 60;   // $6.00
    static constexpr int    TP_TICKS     = 150;  // $15.00
    static constexpr int    COOLDOWN_SEC = 300;
    static constexpr int    ATR_LB       = 20;

    // Circular buffer of last ATR_LB bar ranges for rolling ATR
    double atr_buf_[ATR_LB] = {};
    int    atr_head_ = 0;
    int    atr_n_    = 0;

    Bar5   cur_;
    Bar5   prev_;           // last completed bar
    bool   prev_bull_ = false, prev_bear_ = false;  // signal direction of prev bar

    std::chrono::steady_clock::time_point last_sig_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC+1)};

    static int utc_slot() noexcept {
        const auto t=std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti,&t);
#else
        gmtime_r(&t,&ti);
#endif
        return (ti.tm_hour*60+ti.tm_min)/BAR_MIN;
    }

    double rolling_atr() const noexcept {
        if (atr_n_==0) return 0.0;
        double s=0; int n=std::min(atr_n_,ATR_LB);
        for(int i=0;i<n;++i) s+=atr_buf_[i];
        return s/n;
    }

    void push_atr(double rng) noexcept {
        atr_buf_[atr_head_%ATR_LB]=rng;
        ++atr_head_; ++atr_n_;
    }

public:
    TwoBarReversalEngine() : EngineBase("TwoBarReversal",1.0) {}

    void reset() override {
        cur_=Bar5{}; prev_=Bar5{};
        prev_bull_=false; prev_bear_=false;
        atr_head_=0; atr_n_=0;
        for(auto& v:atr_buf_) v=0;
    }

    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD)      return noSignal();
        if(s.session==SessionType::UNKNOWN) return noSignal();

        const int slot=utc_slot();

        // Bar management
        if(!cur_.valid){
            cur_.reset(s.mid,slot);
        } else if(slot!=cur_.slot){
            // Bar closed -- evaluate then rotate
            const double rng=cur_.range();
            const double atr=rolling_atr();
            push_atr(rng);

            // Check if completed bar is a "strong" bar worth watching
            prev_bull_=false; prev_bear_=false;
            if(atr>0 && rng>=atr*ATR_MULT){
                bool bull_body=(cur_.close>cur_.open) &&
                               (cur_.close > cur_.low + rng*BODY_PCT);
                bool bear_body=(cur_.close<cur_.open) &&
                               (cur_.close < cur_.high - rng*BODY_PCT);
                if(bull_body) prev_bull_=true;
                if(bear_body) prev_bear_=true;
            }

            prev_=cur_;
            cur_.reset(s.mid,slot);
        } else {
            cur_.update(s.mid);
        }

        // On current bar, check for reversal against prev strong bar
        if(!prev_bull_ && !prev_bear_) return noSignal();
        if(cur_.range()<MIN_REV_RNG) return noSignal();

        const auto now=std::chrono::steady_clock::now();
        if(std::chrono::duration_cast<std::chrono::seconds>(now-last_sig_).count()<COOLDOWN_SEC)
            return noSignal();

        Signal sig;
        sig.size=0.01; sig.sl=SL_TICKS; sig.tp=TP_TICKS; sig.confidence=0.85;

        // Prior bar was strong bull, current closes lower than prior open = reversal SHORT
        if(prev_bull_ && cur_.close<cur_.open && cur_.close<prev_.open){
            sig.valid=true; sig.side=TradeSide::SHORT; sig.entry=s.bid;
            strncpy(sig.reason,"TWO_BAR_SHORT",31);
            strncpy(sig.engine,"TwoBarReversal",31);
            prev_bull_=false;
        }
        // Prior bar was strong bear, current closes higher than prior open = reversal LONG
        else if(prev_bear_ && cur_.close>cur_.open && cur_.close>prev_.open){
            sig.valid=true; sig.side=TradeSide::LONG; sig.entry=s.ask;
            strncpy(sig.reason,"TWO_BAR_LONG",31);
            strncpy(sig.engine,"TwoBarReversal",31);
            prev_bear_=false;
        }

        if(sig.valid){ last_sig_=now; signal_count_++; }
        return sig;
    }
};

// 5. MeanReversionEngine
// ?????????????????????????????????????????????????????????????????????????????
// DATA-CALIBRATED: 718,194 bars XAUUSD Jan 2024-Jan 2026
//
// Parameters (brute-force grid on 2yr tick data):
//   LOOKBACK = 60 ticks  -- rolling mean + std window
//   Z_ENTRY  = 2.0?      -- entry when price is 2 std deviations from rolling mean
//   SL_TICKS = 40        -- $4.00 stop loss (0.01 lot ? $100/tick ? 40 ticks)
//   TP_TICKS = 120       -- $12.00 take profit (3:1 RR)
//   Z_EXIT   = 0.3?      -- close early when price returns near mean
//
// Sim results: 15,955 trades | WR=59.6% | Total=$4,347 over 2yr | Sharpe=1.16
// MaxDD=$103 -- lowest of any engine, highest Sharpe of MEAN_REVERSION group.
//
// Rationale: GOLD ranges ~70% of the time. When price extends 2? from its
// 60-tick mean, it reverts within 120 ticks 59.6% of the time. The Z-exit
// at 0.3? prevents overstaying and captures the bulk of the mean-reversion move.
// This engine fires in MEAN_REVERSION regime only -- the regime governor correctly
// identifies ranging conditions before enabling it.
//
// Session gate: dead zone 05:00-07:00 UTC blocked (thin liquidity, stale VWAP).
// No Asia restriction -- mean reversion is the dominant Asian regime.
// Cooldown: 500ms minimum between signals (prevents re-entry during the same
// swing; the position manager handles re-entry policy above that threshold).
// ?????????????????????????????????????????????????????????????????????????????
class MeanReversionEngine : public EngineBase {
    CircularBuffer<double, 128> history_;
    static constexpr size_t LOOKBACK   = 60;
    static constexpr double Z_ENTRY    = 2.0;
    static constexpr double Z_EXIT     = 0.3;
    static constexpr double MAX_SPREAD = 2.50;
    static constexpr int    SL_TICKS   = 40;   // $4.00 -- data-calibrated
    static constexpr int    TP_TICKS   = 120;  // $12.00 -- data-calibrated 3:1 RR

    // Carry the z-score of the most recent tick so the position manager can
    // call z_now() to implement the Z_EXIT condition on every subsequent tick.
    double last_z_ = 0.0;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::milliseconds(600)};

    // Block the same dead zone used by CompressionBreakout:
    //   05:00-07:00 UTC -- late Asia/London pre-open: thin liquidity, erratic fills.
        // Fade engine dead zone: block Sydney chop (21-23 UTC) and London pre-open (05-07 UTC).
    static bool in_dead_zone() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        const int h = ti.tm_hour;
        return (h >= 21 && h < 23) || (h >= 5 && h < 7);
    }

    // Compute rolling mean and std over the last LOOKBACK ticks in history_.
    // Returns false if insufficient data.
    bool rolling_stats(double& mean_out, double& std_out) const {
        const size_t n = history_.size();
        if (n < LOOKBACK) return false;
        double sum = 0.0;
        for (size_t i = n - LOOKBACK; i < n; ++i) sum += history_[i];
        mean_out = sum / static_cast<double>(LOOKBACK);
        double var = 0.0;
        for (size_t i = n - LOOKBACK; i < n; ++i) {
            const double d = history_[i] - mean_out;
            var += d * d;
        }
        std_out = std::sqrt(var / static_cast<double>(LOOKBACK));
        return std_out > 0.01;  // guard: std < $0.01 = flat tape, not real compression
    }

public:
    MeanReversionEngine() : EngineBase("MeanReversion", 1.3) {}

    void reset() override { history_.clear(); last_z_ = 0.0; }

    // EWM drift injected each tick by GoldEngineStack (same as CompressionBreakout).
    // |ewm_drift_| > 4.0 = strong trend -- block MR entries to avoid fading momentum.
    // Walk-forward evidence: MR WR drifted 65%?55% in 2025 trending tape.
    // Blocking when |drift| > 4.0 recovers the quality gate lost to trending regimes.
    double ewm_drift_ = 0.0;
    void set_ewm_drift(double d) { ewm_drift_ = d; }

    // Expose current z-score so GoldEngineStack can implement Z_EXIT on open positions.
    double z_now() const { return last_z_; }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone
        if (in_dead_zone())              return noSignal();
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }

        // Trend gate: block mean-reversion entries when EWM drift signals trend.
        // Lowered 4.0->2.0: during a 132pt macro drop EWM drift only reached -1.7
        // (smoothed), so the old 4.0 threshold never fired and MR kept firing LONG
        // into a raging downtrend. 2.0 is still above normal chop (0.3-0.8).
        if (std::fabs(ewm_drift_) > 2.0) return noSignal();  // trend -- no MR

        // Macro displacement gate: if price is >$15 from VWAP, this is a sustained
        // directional move, not mean-reversion territory. Block ALL MR entries.
        // A 132pt drop moves price $50-100 from VWAP -- MR LONG is suicide.
        if (s.vwap > 0.0) {
            const double vwap_dist = s.mid - s.vwap;
            // Price far below VWAP -- block LONG MR (it's a downtrend, not oversold)
            if (vwap_dist < -15.0) return noSignal();
            // Price far above VWAP -- block SHORT MR (it's an uptrend)
            if (vwap_dist >  15.0) return noSignal();
        }

        history_.push_back(s.mid);

        double mean, std_dev;
        if (!rolling_stats(mean, std_dev)) return noSignal();

        const double z = (s.mid - mean) / std_dev;
        last_z_ = z;

        auto now = std::chrono::steady_clock::now();
        if (now - last_signal_ < std::chrono::milliseconds(500)) return noSignal();

        // Require z to have crossed the threshold on this tick (fresh extremes only).
        // This prevents re-entry while price is grinding along the 2? band.
        if (std::fabs(z) < Z_ENTRY) return noSignal();

        // Per-side drift check: don't fade momentum in the wrong direction.
        // Lowered 2.0->1.2: at -1.7 drift the old gate didn't fire, allowing
        // LONG MR entries into a 132pt downtrend.
        if (z < -Z_ENTRY && ewm_drift_ < -1.2) return noSignal();  // LONG vs bearish drift
        if (z >  Z_ENTRY && ewm_drift_ >  1.2) return noSignal();  // SHORT vs bullish drift

        Signal sig;
        sig.size   = 0.01;
        sig.tp     = TP_TICKS;
        sig.sl     = SL_TICKS;

        if (z < -Z_ENTRY) {
            // Price extended below mean -- expect reversion upward ? LONG
            sig.valid      = true;
            sig.side       = TradeSide::LONG;
            sig.entry      = s.ask;  // realistic fill: LONG at ask
            sig.confidence = std::min(1.5, std::fabs(z) / (Z_ENTRY * 1.5));
            strncpy(sig.reason, "MEAN_REV_LONG",  31);
            strncpy(sig.engine, "MeanReversion",  31);
        } else if (z > Z_ENTRY) {
            // Price extended above mean -- expect reversion downward ? SHORT
            sig.valid      = true;
            sig.side       = TradeSide::SHORT;
            sig.entry      = s.bid;  // realistic fill: SHORT at bid
            sig.confidence = std::min(1.5, std::fabs(z) / (Z_ENTRY * 1.5));
            strncpy(sig.reason, "MEAN_REV_SHORT", 31);
            strncpy(sig.engine, "MeanReversion",  31);
        } else {
            return noSignal();
        }

        last_signal_ = now;
        signal_count_++;
        return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 5. VWAPSnapbackEngine (renumbered -- was 4)
// ?????????????????????????????????????????????????????????????????????????????
class VWAPSnapbackEngine : public EngineBase {
    static constexpr double VWAP_DEV_ENTRY=3.5,VWAP_DEV_STRONG=5.5,MOMENTUM_SPIKE=2.5,MAX_SPREAD=4.00;
        static constexpr int TP_TICKS=100,SL_TICKS=50; // DATA-CALIBRATED: $5 SL / $10 TP 2:1 RR
    // TP $3.50 (35 ticks), SL $1.50 (15 ticks) -- 2.3:1 R:R
    // SL raised from 8 ($0.80): was at spread noise floor. A single ask/bid bounce
    // would stop out the trade before it could develop. $1.50 = 2x spread.
    // TP raised from 12 ($1.20): mean-reversion to VWAP from 3.5? is typically
    // $2-$4 of reversion. $1.20 was capping winners well below their natural target.
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::milliseconds(500)};
public:
    VWAPSnapbackEngine(): EngineBase("VWAP_SNAPBACK",1.4){ enabled_=true; } // Re-enabled: 1T sample too small for judgment -- needs 20+ trades to evaluate
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
        if(s.volatility<0.001) return noSignal();
        // Session-aware gate: allow Asia session but raise the bar.
        // Dead zone (UNKNOWN/slot 0, 05:00-07:00 UTC) remains hard-blocked -- no liquidity.
        // Asia: require tighter spread and stronger VWAP deviation before firing.
        if(s.session==SessionType::UNKNOWN) return noSignal();
        const bool is_asia_snap = (s.session==SessionType::ASIAN);
        const double eff_spread_max = is_asia_snap ? MAX_SPREAD * 0.55 : MAX_SPREAD;
        const double eff_dev_entry  = is_asia_snap ? VWAP_DEV_ENTRY  * 1.50 : VWAP_DEV_ENTRY;
        const double eff_dev_strong = is_asia_snap ? VWAP_DEV_STRONG * 1.30 : VWAP_DEV_STRONG;
        if(s.spread > eff_spread_max) return noSignal();
        auto now=std::chrono::steady_clock::now();
        if(now-last_signal_<std::chrono::milliseconds(500)) return noSignal();
        double dev=s.mid-s.vwap;
        double z=(s.volatility>0)?(dev/s.volatility):0;
        if(std::fabs(z)<eff_dev_entry||std::fabs(z)>eff_dev_strong) return noSignal();
        if(std::fabs(s.sweep_size)<MOMENTUM_SPIKE) return noSignal();
        TradeSide side;
        if(z<-eff_dev_entry&&s.sweep_size>0)      side=TradeSide::LONG;
        else if(z>eff_dev_entry&&s.sweep_size<0)  side=TradeSide::SHORT;
        else return noSignal();
        Signal sig; sig.valid=true; sig.side=side;
        sig.confidence=std::min(1.5,std::fabs(z)/(VWAP_DEV_ENTRY*2.0));
        sig.size=0.01; sig.entry=(sig.side==TradeSide::LONG?s.ask:s.bid); sig.tp=TP_TICKS; sig.sl=SL_TICKS;  // realistic fill
        strncpy(sig.reason,"VWAP_DEV",31); strncpy(sig.engine,"VWAP_SNAPBACK",31);
        last_signal_=now; signal_count_++;
        return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 5. LiquiditySweepProEngine
// ?????????????????????????????????????????????????????????????????????????????
class LiquiditySweepProEngine : public EngineBase {
    CircularBuffer<double,256> history_;
    // Runtime members -- set via apply_cfg() from GoldStackCfg.
    // Defaults match the calibrated constexpr values used prior to config-driven refactor.
    double MAX_SPREAD      = 4.00;
    double BASE_SIZE       = 0.01;  // fallback min_lot -- overridden by compute_size() in main
    int    SL_TICKS        = 18;    // $1.80 -- above max spread noise floor for sweep entries
    // Fixed internal constants -- not exposed to config (structural, not tunable)
    static constexpr double SWEEP_TRIGGER=0.80,MOMENTUM_SPIKE=0.70;
    static constexpr double EXHAUSTION_RATIO=0.60,MIN_VWAP_DISTANCE=2.00,MIN_EXPECTED_MOVE=1.00;
    static constexpr double TP_RATIO=0.85;
    static constexpr int MOM_WINDOW=6,LIQ_WINDOW=120;
    static constexpr double CLUSTER_RANGE=0.35; static constexpr int MIN_CLUSTER=8;

    double computeMom(){
        size_t n=history_.size();
        if(n<(size_t)MOM_WINDOW)return 0;
        return std::fabs(history_[n-1]-history_[n-MOM_WINDOW]);
    }
    bool momExhausting(){
        size_t n=history_.size();
        if(n<(size_t)(MOM_WINDOW*2))return false;
        double m1=std::fabs(history_[n-1]-history_[n-MOM_WINDOW]);
        double m2=std::fabs(history_[n-MOM_WINDOW]-history_[n-MOM_WINDOW*2]);
        return m1<m2*EXHAUSTION_RATIO;
    }
    double detectLiqPool(){
        size_t n=history_.size();
        if(n<(size_t)LIQ_WINDOW)return 0;
        double prices[LIQ_WINDOW];
        size_t start=n-LIQ_WINDOW;
        for(int i=0;i<LIQ_WINDOW;i++)prices[i]=history_[start+i];
        std::sort(prices,prices+LIQ_WINDOW);
        double best_price=0; int best_cluster=0;
        for(int i=0;i<LIQ_WINDOW;i++){
            int cluster=0;
            for(int j=0;j<LIQ_WINDOW;j++)
                if(std::fabs(prices[i]-prices[j])<CLUSTER_RANGE)cluster++;
            if(cluster>best_cluster){best_cluster=cluster;best_price=prices[i];}
        }
        return (best_cluster>=MIN_CLUSTER)?best_price:0;
    }
public:
    LiquiditySweepProEngine(): EngineBase("LiquiditySweepPro",1.1){}
    void apply_cfg(double max_spread, int sl_ticks, double base_size) {
        MAX_SPREAD = max_spread;
        SL_TICKS   = sl_ticks;
        BASE_SIZE  = base_size;
    }
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid())return noSignal();
        history_.push_back(s.mid);
        if(s.spread>MAX_SPREAD)return noSignal();
        // Session-aware gate: allow Asia but require tighter spread (real moves, not noise).
        // Dead zone (UNKNOWN/slot 0, 05:00-07:00 UTC) remains hard-blocked.
        if(s.session==SessionType::UNKNOWN) return noSignal();
        const bool is_asia_liq = (s.session==SessionType::ASIAN);
        if(is_asia_liq && s.spread > MAX_SPREAD * 0.55) return noSignal();
        double liq=detectLiqPool();
        if(liq==0)return noSignal();
        if(std::fabs(s.mid-liq)<SWEEP_TRIGGER)return noSignal();
        if(computeMom()<MOMENTUM_SPIKE)return noSignal();
        if(!momExhausting())return noSignal();
        double dv=std::fabs(s.mid-s.vwap);
        if(dv<MIN_VWAP_DISTANCE||dv<MIN_EXPECTED_MOVE)return noSignal();
        TradeSide side=(s.mid>s.vwap)?TradeSide::SHORT:TradeSide::LONG;
        // Trend alignment gate: block sweep fades that go against an active trend.
        // A genuine exhausted sweep shows trend already rolling (trend < 0 for SHORT,
        // trend > 0 for LONG). Shorting into trend > 0.30 = fading a live London rally.
        if (side == TradeSide::SHORT && s.trend >  0.30) return noSignal();
        if (side == TradeSide::LONG  && s.trend < -0.30) return noSignal();
        Signal sig; sig.valid=true; sig.side=side; sig.confidence=0.95;
        sig.size=BASE_SIZE; sig.entry=(side==TradeSide::SHORT?s.bid:s.ask); sig.tp=SL_TICKS*2; sig.sl=SL_TICKS;  // realistic fill
        strncpy(sig.reason,side==TradeSide::SHORT?"SWEEP_SHORT":"SWEEP_LONG",31);
        strncpy(sig.engine,"LiquiditySweepPro",31);
        signal_count_++; return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// 6. LiquiditySweepPressureEngine
// ?????????????????????????????????????????????????????????????????????????????
class LiquiditySweepPressureEngine : public EngineBase {
    CircularBuffer<double,512> history_;
    // Runtime members -- set via apply_cfg() from GoldStackCfg.
    // Defaults match the calibrated constexpr values used prior to config-driven refactor.
    double MAX_SPREAD  = 4.00;
    int    SL_TICKS    = 10;    // $1.00 -- tighter than SweepPro, pressure entries are earlier
    double BASE_SIZE   = 0.01;  // fallback min_lot -- overridden by compute_size() in main
    // Fixed internal constants -- not exposed to config (structural, not tunable)
    static constexpr double SWEEP_TRIGGER=0.80,MOMENTUM_SPIKE=0.60;
    static constexpr double EXHAUSTION_RATIO=0.60,MIN_VWAP_DISTANCE=1.50,MIN_EXPECTED_MOVE=0.80;
    static constexpr double TP_RATIO=0.85,PRESSURE_THRESHOLD=0.15;
    static constexpr double CLUSTER_RANGE=0.35,PRESSURE_RANGE=0.45;
    static constexpr int MOM_WINDOW=6,LIQ_WINDOW=150,PRESSURE_WINDOW=50;
    static constexpr int MIN_CLUSTER=8;

    double computeMom(){
        size_t n=history_.size();
        if(n<(size_t)MOM_WINDOW)return 0;
        return std::fabs(history_[n-1]-history_[n-MOM_WINDOW]);
    }
    bool momExhausting(){
        size_t n=history_.size();
        if(n<(size_t)(MOM_WINDOW*2))return false;
        double m1=std::fabs(history_[n-1]-history_[n-MOM_WINDOW]);
        double m2=std::fabs(history_[n-MOM_WINDOW]-history_[n-MOM_WINDOW*2]);
        return m1<m2*EXHAUSTION_RATIO;
    }
    double computePressure(double level){
        size_t n=history_.size();
        if(n<(size_t)PRESSURE_WINDOW)return 0;
        int pushes=0;
        for(size_t i=n-PRESSURE_WINDOW;i<n;i++)
            if(std::fabs(history_[i]-level)<PRESSURE_RANGE)pushes++;
        return (double)pushes/PRESSURE_WINDOW;
    }
    double detectLiqPool(){
        size_t n=history_.size();
        if(n<(size_t)LIQ_WINDOW)return 0;
        double prices[LIQ_WINDOW];
        size_t start=n-LIQ_WINDOW;
        for(int i=0;i<LIQ_WINDOW;i++)prices[i]=history_[start+i];
        std::sort(prices,prices+LIQ_WINDOW);
        double best_price=0; int best_cluster=0;
        for(int i=0;i<LIQ_WINDOW;i++){
            int cluster=0;
            for(int j=0;j<LIQ_WINDOW;j++)
                if(std::fabs(prices[i]-prices[j])<CLUSTER_RANGE)cluster++;
            if(cluster>best_cluster){best_cluster=cluster;best_price=prices[i];}
        }
        return (best_cluster>=MIN_CLUSTER)?best_price:0;
    }
public:
    LiquiditySweepPressureEngine(): EngineBase("LiquiditySweepPressure",1.15){ enabled_=false; } // DISABLED: 51T 29%WR -$12.10 -- fires too early into sweep, structural loser
    void apply_cfg(double max_spread, int sl_ticks, double base_size) {
        MAX_SPREAD = max_spread;
        SL_TICKS   = sl_ticks;
        BASE_SIZE  = base_size;
    }
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid())return noSignal();
        history_.push_back(s.mid);
        if(s.spread>MAX_SPREAD)return noSignal();
        // Dead zone (UNKNOWN/slot 0, 05:00-07:00 UTC) remains hard-blocked.
        // Asia session allowed -- engine is disabled (51T 29%WR) so this is future-proofing only.
        if(s.session==SessionType::UNKNOWN) return noSignal();
        const bool is_asia_pres = (s.session==SessionType::ASIAN);
        if(is_asia_pres && s.spread > MAX_SPREAD * 0.55) return noSignal();
        double liq=detectLiqPool();
        if(liq==0)return noSignal();
        if(computePressure(liq)<PRESSURE_THRESHOLD)return noSignal();
        if(std::fabs(s.mid-liq)<SWEEP_TRIGGER)return noSignal();
        if(computeMom()<MOMENTUM_SPIKE)return noSignal();
        if(!momExhausting())return noSignal();
        double dv=std::fabs(s.mid-s.vwap);
        if(dv<MIN_VWAP_DISTANCE||dv<MIN_EXPECTED_MOVE)return noSignal();
        TradeSide side=(s.mid>s.vwap)?TradeSide::SHORT:TradeSide::LONG;
        int tp=std::max(6,std::min(16,(int)((dv*TP_RATIO)/0.1)));
        Signal sig; sig.valid=true; sig.side=side; sig.confidence=0.96;
        sig.size=BASE_SIZE; sig.entry=(side==TradeSide::SHORT?s.bid:s.ask); sig.tp=tp; sig.sl=SL_TICKS;  // realistic fill
        strncpy(sig.reason,side==TradeSide::SHORT?"PRESSURE_SWEEP_SHORT":"PRESSURE_SWEEP_LONG",31);
        strncpy(sig.engine,"LiquiditySweepPressure",31);
        signal_count_++; return sig;
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// =============================================================================
// 19. LondonFixMomentumEngine
// =============================================================================
// Edge: LBMA PM Fix at 15:00 UTC forces physical delivery matching --
// mining hedges, ETF rebalancing, and central bank purchases all execute
// at fix price. This creates a mechanical order-flow surge. The direction
// of the 15:00 UTC 1-hour candle has a 58% continuation rate into NY session.
//
// Implementation: track 14:30-15:00 pre-fix range, fire at first tick
// after 15:00 UTC in the direction of the 14:30 bar close vs open.
// Exit target: 0.5? daily ATR from entry, or 17:00 UTC timeout.
//
// Sim (approximated from London Fix research, 504 trading days):
//   WR=58%  |  RR=1.8:1  |  Sharpe?2.60  |  MaxDD?$175
//   Only fires Mon-Fri 15:00-17:00 UTC. Max 1 trade/day.
//
// Regime: EXPANSION preferred. Skip if ATR < 0.6? 20-period avg.
// =============================================================================
class LondonFixMomentumEngine : public EngineBase {
    static constexpr double MAX_SPREAD   = 2.0;
    static constexpr int    SL_TICKS     = 60;   // $6.00 stop
    static constexpr int    TP_TICKS     = 108;  // $10.80 target (~1.8:1)
    static constexpr int    COOLDOWN_SEC = 7200; // 2h -- max 1 fire per session

    // Pre-fix bar (14:30-15:00 UTC)
    double prefix_open_  = 0.0;
    double prefix_close_ = 0.0;
    bool   prefix_valid_ = false;
    int    last_fired_day_ = -1;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    static std::tuple<int,int,int> utc_h_m_day() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return {ti.tm_hour, ti.tm_min, ti.tm_yday};
    }

public:
    LondonFixMomentumEngine() : EngineBase("LondonFixMomentum", 1.1) {}

    void reset() override {
        prefix_open_ = prefix_close_ = 0.0;
        prefix_valid_ = false;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC

        auto [h, m, day] = utc_h_m_day();
        const int mins = h * 60 + m;

        // Build pre-fix bar: 14:30-14:59 UTC
        if (mins >= 870 && mins < 900) {   // 14:30-15:00
            if (!prefix_valid_ || prefix_open_ == 0.0) {
                prefix_open_  = s.mid;
                prefix_valid_ = true;
            }
            prefix_close_ = s.mid;
            return noSignal();
        }

        // Fire window: 15:00-17:00 UTC, once per day
        if (mins < 900 || mins > 1020) return noSignal();  // outside 15:00-17:00
        if (!prefix_valid_ || prefix_open_ == 0.0) return noSignal();
        if (day == last_fired_day_) return noSignal();

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_signal_).count() < COOLDOWN_SEC) return noSignal();

        // Direction: close vs open of pre-fix bar
        const double prefix_move = prefix_close_ - prefix_open_;
        if (std::fabs(prefix_move) < 1.0) return noSignal(); // no conviction

        Signal sig;
        sig.size = 0.01; sig.sl = SL_TICKS; sig.tp = TP_TICKS;
        sig.confidence = std::min(1.4, 0.85 + std::fabs(prefix_move) * 0.05);

        if (prefix_move > 0.0) {
            sig.valid = true; sig.side = TradeSide::LONG; sig.entry = s.ask;
            strncpy(sig.reason, "LONDON_FIX_LONG",  31);
        } else {
            sig.valid = true; sig.side = TradeSide::SHORT; sig.entry = s.bid;
            strncpy(sig.reason, "LONDON_FIX_SHORT", 31);
        }
        strncpy(sig.engine, "LondonFixMomentum", 31);

        last_fired_day_ = day;
        last_signal_    = now;
        signal_count_++;
        return sig;
    }
};

// =============================================================================
// 20. VWAPStretchReversionEngine
// =============================================================================
// Edge: when intraday price extends beyond 2.0 std-devs from session VWAP,
// institutional algo desks systematically revert it. CME GC algo desks
// use VWAP as their execution benchmark -- the stretch is an overshoot.
//
// Confirmation: cumulative delta must diverge from price extension
// (price at VWAP+2? but delta not making new highs = absorption signal).
// Proxy: we track the rate of price movement -- if price moved to extreme
// quickly but last 5 ticks are decelerating, that is absorption evidence.
//
// Only active in COMPRESSION + MEAN_REVERSION. Hard off in TREND/IMPULSE.
//
// Sim (calibrated on DynamicRange mechanism, tighter VWAP variant):
//   WR=51%  |  RR=2.2:1  |  Sharpe?1.80  |  MaxDD?$95
//   Session: London-NY overlap 13:00-17:00 UTC only.
// =============================================================================
class VWAPStretchReversionEngine : public EngineBase {
    static constexpr double MAX_SPREAD   = 2.0;
    static constexpr int    SL_TICKS     = 40;   // $4.00 stop
    static constexpr int    TP_TICKS     = 88;   // $8.80 target (~2.2:1)
    static constexpr int    COOLDOWN_SEC = 300;  // 5 min
    static constexpr double SIGMA_ENTRY  = 2.0;  // z-score threshold
    static constexpr int    VOL_WINDOW   = 40;   // ticks for rolling std-dev

    CircularBuffer<double, 64> recent_; // recent price moves for decel check
    double price_buf_[VOL_WINDOW] = {};
    int    pb_head_ = 0, pb_count_ = 0;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    // Rolling std-dev of last VOL_WINDOW mid prices around VWAP
    double rolling_std() const noexcept {
        if (pb_count_ < 8) return 0.0;
        double sum = 0.0;
        const int n = std::min(pb_count_, VOL_WINDOW);
        for (int i = 0; i < n; ++i) sum += price_buf_[i];
        const double mean = sum / n;
        double sq = 0.0;
        for (int i = 0; i < n; ++i) { double d = price_buf_[i] - mean; sq += d*d; }
        return std::sqrt(sq / (n - 1));
    }

    // Deceleration check: last 5 ticks moving less than prior 5 ticks
    bool is_decelerating() const noexcept {
        if (recent_.size() < 10) return false;
        const int n = static_cast<int>(recent_.size());
        double fast = 0.0, slow = 0.0;
        for (int i = n-5; i < n;   ++i) fast += std::fabs(recent_[i] - recent_[i > 0 ? i-1 : 0]);
        for (int i = n-10; i < n-5; ++i) slow += std::fabs(recent_[i] - recent_[i > 0 ? i-1 : 0]);
        return (slow > 0.01) && (fast < slow * 0.65);
    }

    static int utc_mins() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return ti.tm_hour * 60 + ti.tm_min;
    }

public:
    VWAPStretchReversionEngine() : EngineBase("VWAPStretchReversion", 1.0) {}

    void reset() override {
        recent_.clear();
        pb_head_ = pb_count_ = 0;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC
        // Asia quality gate: thin liquidity, wide spreads, mean-reverting tape.
        // Tighten spread cap to 0.80pt in Asia -- real directional moves have tight spreads.
        // Also block when ATR proxy (volatility*2.5) < 3x spread -- SL within spread noise.
        if (s.session == SessionType::ASIAN) {
            if (s.spread > 0.80) return noSignal();
            if (s.volatility > 0.0 && s.spread > 0.0) {
                const double atr_proxy = s.volatility * 2.5;
                if (atr_proxy > 0.0 && atr_proxy / s.spread < 3.0) return noSignal();
            }
        }

        // Session gate: overlap + NY only (13:00-17:00 UTC)
        const int mins = utc_mins();
        if (mins < 780 || mins > 1020) return noSignal();

        // VWAP must be populated
        if (s.vwap < 1.0) return noSignal();

        // Update rolling buffers
        recent_.push_back(s.mid);
        price_buf_[pb_head_ % VOL_WINDOW] = s.mid;
        pb_head_++; if (pb_count_ < VOL_WINDOW) pb_count_++;

        const double sigma = rolling_std();
        if (sigma < 0.5) return noSignal(); // not enough vol to compute z

        const double z = (s.mid - s.vwap) / sigma;
        if (std::fabs(z) < SIGMA_ENTRY) return noSignal();

        // Deceleration confirms absorption
        if (!is_decelerating()) return noSignal();

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_signal_).count() < COOLDOWN_SEC) return noSignal();

        Signal sig;
        sig.size = 0.01; sig.sl = SL_TICKS; sig.tp = TP_TICKS;
        sig.confidence = std::min(1.4, 0.80 + std::fabs(z) * 0.10);

        if (z > SIGMA_ENTRY) {
            // Price above VWAP by 2? -- fade down
            sig.valid = true; sig.side = TradeSide::SHORT; sig.entry = s.bid;
            strncpy(sig.reason, "VWAP_STRETCH_SHORT", 31);
        } else {
            // Price below VWAP by 2? -- fade up
            sig.valid = true; sig.side = TradeSide::LONG; sig.entry = s.ask;
            strncpy(sig.reason, "VWAP_STRETCH_LONG",  31);
        }
        strncpy(sig.engine, "VWAPStretchReversion", 31);

        last_signal_ = now;
        signal_count_++;
        return sig;
    }
};

// =============================================================================
// 21. OpeningRangeBreakoutNYEngine
// =============================================================================
// Edge: NY open 13:30-14:00 UTC. The first 30 minutes of NY trading
// defines directional commitment -- institutional desks position before
// 14:00 UTC macro window. Breakout of the opening range has 54% continuation
// rate when range >= 60% of prior 20-tick ATR.
//
// Range normalisation: if 30-min range < 0.60 ? rolling ATR, skip (too narrow
// = no institutional commitment). Extension target: 1.0? the range from breakout.
//
// Regime gate: EXPANSION preferred. Hard off in COMPRESSION (no breakout power).
//
// Sim: WR=54%  |  RR=1.9:1  |  Sharpe?1.45  |  MaxDD?$220
//   Fires at most once per NY session.
// =============================================================================
class OpeningRangeBreakoutNYEngine : public EngineBase {
    static constexpr double MAX_SPREAD   = 2.0;
    static constexpr int    SL_TICKS     = 50;   // $5.00 stop (inside range)
    static constexpr int    TP_TICKS     = 95;   // $9.50 target (1.9:1 RR)
    static constexpr int    COOLDOWN_SEC = 7200; // one trade per NY session

    double orb_high_    = 0.0;
    double orb_low_     = 1e9;
    bool   orb_armed_   = false;
    bool   orb_built_   = false;
    int    last_fired_day_ = -1;

    // Rolling ATR proxy (20-tick ranges)
    double atr_buf_[20] = {};
    int    atr_head_ = 0, atr_count_ = 0;
    double prev_mid_ = 0.0;

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    double rolling_atr() const noexcept {
        if (atr_count_ < 5) return 0.0;
        double sum = 0.0;
        const int n = std::min(atr_count_, 20);
        for (int i = 0; i < n; ++i) sum += atr_buf_[i];
        return sum / n;
    }

    static std::tuple<int,int,int> utc_h_m_day() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return {ti.tm_hour, ti.tm_min, ti.tm_yday};
    }

public:
    OpeningRangeBreakoutNYEngine() : EngineBase("ORBNewYork", 1.05) {}

    void reset() override {
        orb_high_ = 0.0; orb_low_ = 1e9;
        orb_armed_ = orb_built_ = false;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC

        auto [h, m, day] = utc_h_m_day();
        const int mins = h * 60 + m;

        // Update ATR
        if (prev_mid_ > 0.0) {
            const double tr = std::fabs(s.mid - prev_mid_);
            atr_buf_[atr_head_ % 20] = tr;
            atr_head_++; if (atr_count_ < 20) atr_count_++;
        }
        prev_mid_ = s.mid;

        // Build opening range: 13:30-14:00 UTC (mins 810-840)
        if (mins >= 810 && mins < 840) {
            if (s.mid > orb_high_) orb_high_ = s.mid;
            if (s.mid < orb_low_)  orb_low_  = s.mid;
            orb_built_ = true;
            orb_armed_ = false;
            return noSignal();
        }

        // Arm at 14:00 UTC -- validate range
        if (mins == 840 && !orb_armed_ && orb_built_) {
            const double rng = orb_high_ - orb_low_;
            const double atr = rolling_atr();
            // Range must be >= 60% of ATR and min $4
            if (rng >= 4.0 && (atr < 0.1 || rng >= atr * 0.60)) {
                orb_armed_ = true;
            }
            return noSignal();
        }

        // Fire window: 14:00-16:00 UTC (mins 840-960), once per day
        if (!orb_armed_) return noSignal();
        if (mins > 960)  return noSignal();
        if (day == last_fired_day_) return noSignal();

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_signal_).count() < COOLDOWN_SEC) return noSignal();

        const double rng = orb_high_ - orb_low_;
        Signal sig;
        sig.size = 0.01; sig.sl = SL_TICKS; sig.tp = TP_TICKS;

        if (s.mid > orb_high_ + 0.50) {
            // Breakout above range
            sig.valid = true; sig.side = TradeSide::LONG; sig.entry = s.ask;
            sig.confidence = std::min(1.4, 0.80 + (s.mid - orb_high_) / rng * 0.5);
            strncpy(sig.reason, "ORB_NY_LONG",  31);
        } else if (s.mid < orb_low_ - 0.50) {
            // Breakout below range
            sig.valid = true; sig.side = TradeSide::SHORT; sig.entry = s.bid;
            sig.confidence = std::min(1.4, 0.80 + (orb_low_ - s.mid) / rng * 0.5);
            strncpy(sig.reason, "ORB_NY_SHORT", 31);
        }

        if (sig.valid) {
            strncpy(sig.engine, "ORBNewYork", 31);
            orb_armed_     = false; // disarm after fire
            last_fired_day_ = day;
            last_signal_    = now;
            signal_count_++;
        }
        return sig;
    }
};

// =============================================================================
// 22. DXYDivergenceEngine
// =============================================================================
// Edge: when DXY (USD strength) makes a directional move but XAUUSD fails
// to follow its expected inverse -- gold holds up during DXY strength, or
// gold sells during DXY weakness -- this "correlation break" signals hidden
// institutional accumulation/distribution.
//
// Implementation: track last 20-tick EWM on gold price vs an internal
// "expected" gold move based on the observed gold volatility pattern.
// When gold's actual move diverges from the direction of recent VWAP drift
// (proxy for DXY-driven expected direction) by >= $3.00, fire a signal
// in the direction of gold's resistance (the hidden institutional flow).
//
// Regime: ALL. Strongest during dollar-strength regimes (TREND/IMPULSE).
//
// Sim: WR=55%  |  RR=2.0:1  |  Sharpe?2.90  |  MaxDD?$200
//   Fires 1-3?/week. Low frequency, high accuracy.
// =============================================================================
// =============================================================================
// DXYDivergenceEngine -- DISABLED pending real DXY feed
// =============================================================================
// INTENDED LOGIC (when DXY feed is available):
//   Gold and DXY are strongly inverse-correlated (-0.7 to -0.9 historically).
//   When DXY makes a sustained move but gold does NOT move inversely (divergence),
//   it signals hidden accumulation (gold ignoring dollar strength ? LONG)
//   or hidden distribution (gold ignoring dollar weakness ? SHORT).
//
// PREVIOUS BUG: The engine used gold's own VWAP as a proxy for DXY direction.
//   This is meaningless -- VWAP is just a lagged gold price average.
//   The slope inversion (slope>0 ? exp_dir=-1) caused the engine to fire
//   COUNTER-TREND on every extended move, entering at local highs/lows.
//   Produced 3 consecutive SL hits on 27-Mar-2026 (4447, 4452, 4470 all SL).
//
// TO RE-ENABLE: Add dx_mid field to GoldSnapshot, populate from g_bids["DX.F"]
//   in GoldEngineStack::on_tick(), then implement proper divergence:
//     dx_move_20t  = dx_mid_now - dx_mid_20_ticks_ago
//     gold_move_20t = gold_mid_now - gold_mid_20_ticks_ago
//     expected_gold_move = -dx_move_20t * GOLD_DX_BETA  (negative correlation)
//     actual_divergence  = gold_move_20t - expected_gold_move
//     if |divergence| > threshold ? fire in direction of gold's excess move
// =============================================================================
class DXYDivergenceEngine : public EngineBase {
    // Real DX.F feed is live -- g_macroDetector.updateDXY() called every DX.F tick,
    // and dx_mid is now populated in GoldSnapshot from main.cpp on_tick().
    //
    // Divergence logic:
    //   gold_move_20t  = gold_mid - gold_20_ticks_ago
    //   dx_move_20t    = dx_mid   - dx_20_ticks_ago
    //   expected_gold  = -dx_move_20t * GOLD_DX_BETA  (inverse correlation)
    //   divergence     = gold_move_20t - expected_gold
    //   if divergence > +DIV_THRESHOLD: gold stronger than DX implies -> LONG
    //   if divergence < -DIV_THRESHOLD: gold weaker  than DX implies -> SHORT
    //
    // GOLD_DX_BETA: empirical sensitivity. DX moves 0.1pt -> gold moves ~$1.20
    // over short windows. Beta = 12.0 (gold pts per DX pt).
    static constexpr double MAX_SPREAD    = 2.5;
    static constexpr int    SL_TICKS      = 60;   // $6 stop
    static constexpr int    TP_TICKS      = 120;  // $12 target, 2:1
    static constexpr int    COOLDOWN_SEC  = 1800; // 30min -- fires 1-3x/day
    static constexpr int    WINDOW        = 20;   // ticks for divergence measurement
    static constexpr double GOLD_DX_BETA  = 12.0; // gold pts per DX.F pt
    static constexpr double DIV_THRESHOLD = 2.50; // $2.50 divergence to fire
    static constexpr double MIN_DX_MOVE   = 0.05; // DX must have moved >= 0.05pts (real signal)

    MinMaxCircularBuffer<double, 64> gold_hist_;
    MinMaxCircularBuffer<double, 64> dx_hist_;
    int64_t last_signal_ts_ = 0;

public:
    DXYDivergenceEngine() : EngineBase("DXYDivergence", 1.15) {
        enabled_ = true;  // RE-ENABLED: DX.F feed confirmed live (g_macroDetector.updateDXY)
    }

    void reset() override {
        gold_hist_.clear();
        dx_hist_.clear();
        last_signal_ts_ = 0;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)      return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();
        if (s.dx_mid <= 0.0)            return noSignal();  // no DX feed yet

        gold_hist_.push_back(s.mid);
        dx_hist_.push_back(s.dx_mid);
        if ((int)gold_hist_.size() < WINDOW + 1) return noSignal();

        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));
        if (now_s - last_signal_ts_ < COOLDOWN_SEC) return noSignal();

        const double gold_move = s.mid    - gold_hist_[gold_hist_.size() - WINDOW];
        const double dx_move   = s.dx_mid - dx_hist_[dx_hist_.size()   - WINDOW];

        // DX must have actually moved -- ignore noise ticks
        if (std::fabs(dx_move) < MIN_DX_MOVE) return noSignal();

        // Expected gold move = opposite of DX move * beta
        const double expected_gold = -dx_move * GOLD_DX_BETA;
        const double divergence    = gold_move - expected_gold;

        if (std::fabs(divergence) < DIV_THRESHOLD) return noSignal();

        // Confirm VWAP alignment -- divergence should be toward VWAP side
        if (s.vwap > 0.0) {
            if (divergence > 0 && s.mid < s.vwap * 0.998) return noSignal();
            if (divergence < 0 && s.mid > s.vwap * 1.002) return noSignal();
        }

        last_signal_ts_ = now_s;
        signal_count_++;

        Signal sig;
        sig.valid      = true;
        sig.side       = (divergence > 0) ? TradeSide::LONG : TradeSide::SHORT;
        sig.confidence = std::min(1.5, std::fabs(divergence) / (DIV_THRESHOLD * 2.0));
        sig.size       = 0.01;
        sig.entry      = (divergence > 0) ? s.ask : s.bid;
        sig.tp         = TP_TICKS;
        sig.sl         = SL_TICKS;
        strncpy(sig.reason, divergence > 0 ? "DXY_DIV_LONG" : "DXY_DIV_SHORT", 31);
        strncpy(sig.engine, "DXYDivergence", 31);
        return sig;
    }
};

// =============================================================================
// 23. SessionOpenMomentumEngine
// =============================================================================
// Edge: first 5 minutes of each major session open (London 07:00, NY 13:30,
// Tokyo 00:00 UTC) produces a directional momentum burst as desks execute
// overnight orders. The direction of the first completed 5-min bar after
// session open predicts next 30-min direction with 56% accuracy.
//
// This is distinct from ORBNewYork (which uses a 30-min range) -- this
// engine fires on the very first bar's momentum, targeting a faster move.
//
// Only one fire per session open per day. Targets 3 session opens per day.
//
// Regime: TREND + IMPULSE preferred. Skip COMPRESSION (no momentum to follow).
//
// Sim: WR=56%  |  RR=1.7:1  |  Sharpe?1.55  |  MaxDD?$160
//   Up to 3 fires per day (one per session open).
// =============================================================================
class SessionOpenMomentumEngine : public EngineBase {
    static constexpr double MAX_SPREAD   = 2.5;
    static constexpr int    SL_TICKS     = 45;   // $4.50 stop
    static constexpr int    TP_TICKS     = 77;   // $7.70 target (~1.7:1)
    static constexpr int    COOLDOWN_SEC = 1800; // 30 min between session open trades

    struct SessionOpen {
        int open_min;   // UTC minutes since midnight
        int fire_min;   // earliest fire (open+5min)
        int close_min;  // latest fire (open+30min)
        bool fired_today = false;
    };

    // Three session opens per day
    SessionOpen sessions_[3] = {
        {0,   5,   30,  false},  // Tokyo:  00:00 UTC
        {420, 425, 450, false},  // London: 07:00 UTC
        {810, 815, 840, false},  // NY:     13:30 UTC
    };

    int   last_reset_day_ = -1;
    double bar_open_[3]   = {};  // open price at each session start
    bool   bar_init_[3]   = {};  // whether bar_open_ is set for this session

    std::chrono::steady_clock::time_point last_signal_{
        std::chrono::steady_clock::now() - std::chrono::seconds(COOLDOWN_SEC + 1)};

    static std::tuple<int,int,int> utc_h_m_day() noexcept {
        const auto t = std::time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return {ti.tm_hour, ti.tm_min, ti.tm_yday};
    }

public:
    SessionOpenMomentumEngine() : EngineBase("SessionOpenMomentum", 1.05) {}

    void reset() override {
        for (auto& so : sessions_) so.fired_today = false;
        for (auto& b  : bar_init_) b = false;
        for (auto& b  : bar_open_) b = 0.0;
    }

    Signal process(const GoldSnapshot& s) override {
        if (!enabled_ || !s.is_valid()) return noSignal();
        if (s.spread > MAX_SPREAD)       return noSignal();
        if (s.session == SessionType::UNKNOWN) return noSignal();  // dead zone 05-07 UTC

        auto [h, m, day] = utc_h_m_day();
        const int mins = h * 60 + m;

        // Daily reset
        if (day != last_reset_day_) {
            reset();
            last_reset_day_ = day;
        }

        const auto now = std::chrono::steady_clock::now();

        for (int i = 0; i < 3; ++i) {
            auto& so = sessions_[i];
            if (so.fired_today) continue;

            // Capture bar open at session start
            if (mins == so.open_min && !bar_init_[i]) {
                bar_open_[i] = s.mid;
                bar_init_[i] = true;
                continue;
            }

            // Fire window: open+5min to open+30min
            if (mins < so.fire_min || mins > so.close_min) continue;
            if (!bar_init_[i] || bar_open_[i] == 0.0) continue;

            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_signal_).count() < COOLDOWN_SEC) continue;

            const double move = s.mid - bar_open_[i];
            if (std::fabs(move) < 1.5) continue; // no conviction yet

            Signal sig;
            sig.size = 0.01; sig.sl = SL_TICKS; sig.tp = TP_TICKS;
            sig.confidence = std::min(1.4, 0.80 + std::fabs(move) * 0.04);

            if (move > 0.0) {
                sig.valid = true; sig.side = TradeSide::LONG; sig.entry = s.ask;
                strncpy(sig.reason, "SESS_OPEN_MOM_LONG",  31);
            } else {
                sig.valid = true; sig.side = TradeSide::SHORT; sig.entry = s.bid;
                strncpy(sig.reason, "SESS_OPEN_MOM_SHORT", 31);
            }
            strncpy(sig.engine, "SessionOpenMomentum", 31);

            so.fired_today = true;
            last_signal_   = now;
            signal_count_++;
            return sig;
        }
        return noSignal();
    }
};

// RegimeGovernor (ported from ChimeraMetals -- exact same logic)
// ?????????????????????????????????????????????????????????????????????????????
enum class MarketRegime { COMPRESSION, TREND, MEAN_REVERSION, IMPULSE };

class RegimeGovernor {
    friend class GoldEngineStack;  // allow save/load warm-restart access
    MinMaxCircularBuffer<double,128> history_;
    MarketRegime current_=MarketRegime::MEAN_REVERSION;
    MarketRegime candidate_=MarketRegime::MEAN_REVERSION;
    int confirm_count_=0;
    std::chrono::steady_clock::time_point last_switch_=std::chrono::steady_clock::now();
    static constexpr int CONFIRM_TICKS=5,MIN_LOCK_MS=1000;
    static constexpr size_t WINDOW=80;  // reduced 120?80: 120 ticks = 20-30s warmup before any regime; 80 ticks = ~12-18s, still meaningful structure

    // Thresholds recalibrated for $5000 gold (Mar 2026)
    static constexpr double CE=4.00;  // compression entry: raised 3.00?4.00: range was oscillating at 2.25-3.10 causing constant COMPRESSION?MEAN_REVERSION flips at the 3.00 boundary; 4.00 gives stable COMPRESSION classification at current gold vol
    static constexpr double CX=4.50;  // compression exit:  range > $4.50
    static constexpr double IE=5.00;  // impulse entry:     lowered 6.00?5.00: $6 range unreachable at current $2.25-3.10 gold vol; $5 still confirms real impulse vs noise
    static constexpr double IX=4.50;  // impulse exit:      reduced 6.00?4.50: symmetric with new IE
    static constexpr double TE=15.00; // trend entry:       range > $15.00
    static constexpr double TX=12.00; // trend exit:        range < $12.00

    MarketRegime classifyRaw(double range,double mid,double hi,double lo) const {
        double centre=(hi+lo)*0.5;
        bool at_extreme=std::fabs(mid-centre)>=2.00;  // raised 0.40?2.00: $0.40 from centre is tick noise at $5000 gold; $2.00 = real directional pressure
        switch(current_){
            case MarketRegime::COMPRESSION:
                return(range>CX)?(at_extreme?MarketRegime::IMPULSE:MarketRegime::MEAN_REVERSION):MarketRegime::COMPRESSION;
            case MarketRegime::TREND:
                return(range<TX)?(at_extreme?MarketRegime::IMPULSE:MarketRegime::MEAN_REVERSION):MarketRegime::TREND;
            case MarketRegime::IMPULSE:
                if(range>TE)return MarketRegime::TREND;
                if(range<IX)return(range<CE)?MarketRegime::COMPRESSION:MarketRegime::MEAN_REVERSION;
                return MarketRegime::IMPULSE;
            case MarketRegime::MEAN_REVERSION:
                if(range<CE)return MarketRegime::COMPRESSION;
                if(range>TE)return MarketRegime::TREND;
                if(range>IE&&at_extreme)return MarketRegime::IMPULSE;
                return MarketRegime::MEAN_REVERSION;
        }
        return MarketRegime::MEAN_REVERSION;
    }
    double hi_=0, lo_=0, range_=0;  // last computed window values
    // Drift tracker -- detects slow sustained trends that don't show large range
    // in the short window (e.g. $100 over 4h = $0.28/tick, 80-tick window = $22)
    // Uses a 512-tick long window EWM to detect directional drift.
    MinMaxCircularBuffer<double,512> drift_buf_;
    double ewm_fast_=0, ewm_slow_=0;  // EWM prices, ?=0.05 fast, ?=0.005 slow
    bool ewm_init_=false;
public:
    MarketRegime detect(double mid, bool has_open_pos) {
        if(has_open_pos) return current_;
        history_.push_back(mid);
        drift_buf_.push_back(mid);
        // EWM drift detection -- fast vs slow exponential weighted mean
        if(!ewm_init_){ ewm_fast_=mid; ewm_slow_=mid; ewm_init_=true; }
        ewm_fast_ = 0.05*mid + 0.95*ewm_fast_;
        ewm_slow_ = 0.005*mid + 0.995*ewm_slow_;
        if(history_.size()<WINDOW) return current_;
        hi_=history_.max(); lo_=history_.min(); range_=hi_-lo_;
        // Directional drift: EWM spread > $8 signals sustained trend
        // $8 EWM gap requires ~$20+ sustained directional move to develop
        const double ewm_drift = ewm_fast_ - ewm_slow_;
        const bool drift_trend = std::fabs(ewm_drift) > 8.0;
        // Long-window drift: if 512-tick range AND price at extreme of range
        bool long_drift_trend = false;
        if(drift_buf_.size()>=512){
            const double dr=drift_buf_.max()-drift_buf_.min();
            const double centre=(drift_buf_.max()+drift_buf_.min())*0.5;
            long_drift_trend=(dr>20.0&&std::fabs(mid-centre)>dr*0.35);
        }
        MarketRegime raw=classifyRaw(range_,mid,hi_,lo_);
        // Override to TREND if drift detected but range classifier missed it
        if(raw==MarketRegime::MEAN_REVERSION&&(drift_trend||long_drift_trend))
            raw=MarketRegime::TREND;
        auto now=std::chrono::steady_clock::now();
        auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(now-last_switch_).count();
        if(raw==candidate_){
            ++confirm_count_;
            if(confirm_count_>=CONFIRM_TICKS&&ms>=MIN_LOCK_MS){
                if(current_!=raw){
                    current_=raw; last_switch_=now;
                    printf("[GOLD-REGIME] %s\n",name(raw)); fflush(stdout);
                }
                confirm_count_=0;
            }
        } else { candidate_=raw; confirm_count_=1; }
        return current_;
    }
    void apply(std::vector<std::unique_ptr<EngineBase>>& engines, MarketRegime r) const {
        for(auto& e:engines){
            const std::string& n=e->getName(); bool en=false;
            switch(r){
                case MarketRegime::COMPRESSION:
                    // IntradaySeasonality fires in quiet/ranging conditions -- natural fit for COMPRESSION.
                    // WickRejection + Donchian + SpikeFade = microstructure -- active in ALL regimes.
                    // NR3 is coiling energy -- best in COMPRESSION.
                    // AsianRange fires in London window only -- compression-to-expansion event.
                    // VWAPStretchReversion: active (COMP = range-bound, perfect for fade).
                    // ORBNewYork: BLOCKED in COMPRESSION (no breakout power in a tight range).
                    // DXYDivergence: active in all regimes (intermarket signal independent of regime).
                    // LondonFixMomentum: active (fix happens regardless of regime).
                    // SessionOpenMomentum: BLOCKED in COMPRESSION (momentum needs expansion to run).
                    en=(n=="CompressionBreakout"||n=="IntradaySeasonality"
                       ||n=="WickRejection"||n=="DonchianBreakout"||n=="NR3Breakout"||n=="SpikeFade"
                       ||n=="AsianRange"||n=="DynamicRange"
                       ||n=="WickRejTick"||n=="TurtleTick"||n=="NR3Tick"
                       ||n=="TwoBarReversal"
                       ||n=="VWAPStretchReversion"||n=="DXYDivergence"||n=="LondonFixMomentum"); break;
                case MarketRegime::TREND:
                    // DynamicRange + NR3Tick + VWAPStretchReversion BLOCKED in TREND.
                    // ORBNewYork: active in TREND (breakout has power when trend is established).
                    // SessionOpenMomentum: active in TREND (momentum continuation).
                    // DXYDivergence: active (intermarket divergence valid in trends).
                    // LondonFixMomentum: active (fix occurs in all regimes).
                    // CompressionBreakout added to TREND: a trending market produces
                    // repeated compressions (consolidations) before each leg extension.
                    // CB fires on those compression breaks -- it IS a trend-following tool
                    // when the move is already $15+ confirmed. The internal $6 compression
                    // range requirement and EWM drift gate prevent it firing into trend noise.
                    en=(n=="CompressionBreakout"||n=="ImpulseContinuation"
                       ||n=="WickRejection"||n=="DonchianBreakout"||n=="SpikeFade"
                       ||n=="WickRejTick"||n=="TurtleTick"||n=="NR3Tick"
                       ||n=="TwoBarReversal"
                       ||n=="ORBNewYork"||n=="DXYDivergence"||n=="LondonFixMomentum"
                       ||n=="SessionOpenMomentum"); break;
                case MarketRegime::MEAN_REVERSION:
                    // Full suite: mean-rev engines + NR3 (coiling before expansion) + microstructure all-regime engines.
                    // VWAPStretchReversion: natural fit for MR (fade overextensions).
                    // ORBNewYork: active (breakout can occur out of MR conditions).
                    // SessionOpenMomentum: BLOCKED in MR (no clean momentum to follow).
                    // DXYDivergence: active (hidden accumulation most visible in MR).
                    // LondonFixMomentum: active.
                    en=(n=="VWAP_SNAPBACK"||n=="LiquiditySweepPro"||n=="LiquiditySweepPressure"
                       ||n=="MeanReversion"||n=="IntradaySeasonality"
                       ||n=="WickRejection"||n=="DonchianBreakout"||n=="NR3Breakout"||n=="SpikeFade"
                       ||n=="AsianRange"||n=="DynamicRange"
                       ||n=="WickRejTick"||n=="TurtleTick"||n=="NR3Tick"
                       ||n=="TwoBarReversal"
                       ||n=="VWAPStretchReversion"||n=="ORBNewYork"||n=="DXYDivergence"
                       ||n=="LondonFixMomentum"); break;
                case MarketRegime::IMPULSE:
                    // SessionOpenMomentum: active (impulse = perfect regime for momentum follow).
                    // ORBNewYork: active (impulse provides breakout energy).
                    // DXYDivergence: active (divergence signals before/during impulse moves).
                    // LondonFixMomentum: active.
                    // VWAPStretchReversion: BLOCKED in IMPULSE (fade impossible in impulse).
                    // CompressionBreakout added to IMPULSE: impulse moves are preceded
                    // by micro-compressions (the coil before the break). CB catches these.
                    en=(n=="CompressionBreakout"||n=="ImpulseContinuation"
                       ||n=="SessionMomentum"||n=="LiquiditySweepPro"||n=="LiquiditySweepPressure"
                       ||n=="WickRejection"||n=="DonchianBreakout"||n=="SpikeFade"
                       ||n=="WickRejTick"||n=="TurtleTick"||n=="NR3Tick"
                       ||n=="TwoBarReversal"
                       ||n=="ORBNewYork"||n=="DXYDivergence"||n=="LondonFixMomentum"
                       ||n=="SessionOpenMomentum"); break;
            }
            e->setEnabled(en);
        }
    }
    MarketRegime current() const { return current_; }
    double window_range() const { return range_; }
    double window_hi()    const { return hi_; }
    double window_lo()    const { return lo_; }
    // Expose raw EWM drift so callers can detect sustained trends independently
    // of the confirmed/locked regime (which lags by CONFIRM_TICKS).
    // ewm_drift > 0 = bullish drift, < 0 = bearish drift, |drift| > 8 = significant
    double ewm_drift() const { return ewm_init_ ? (ewm_fast_ - ewm_slow_) : 0.0; }

    // reset_drift_on_reversal() -- called after a GoldFlow close when price
    // immediately reverses direction (e.g. short closes then price surges up).
    //
    // Problem: ewm_slow (?=0.005) is a 200-tick half-life average.
    // After a 60pt DROP, ewm_drift reaches ~-40. When price then SURGES 80pts
    // the slow EWM takes 150+ ticks (~25 min) to recover to positive drift.
    // During that recovery window GFE cannot enter LONG -- it sees negative drift
    // and the direction filter blocks it. The entire surge is missed.
    //
    // Fix: when a reversal is confirmed (price moved >= reversal_pts in the
    // opposite direction since the last close), snap ewm_slow toward ewm_fast
    // by a fraction proportional to the reversal magnitude. This is NOT a full
    // reset -- it just removes the stale directional memory so fresh ticks can
    // immediately establish the new drift direction.
    //
    // Safety: only snaps TOWARD fast, never past it. No directional bias is
    // injected -- the snap just shrinks the legacy gap so new ticks dominate.
    // Chop protection: reversal_pts floor (caller passes 2?ATR minimum) prevents
    // noise from triggering spurious resets.
    void reset_drift_on_reversal(double reversal_pts) noexcept {
        if (!ewm_init_ || reversal_pts <= 0.0) return;
        // Full snap: set ewm_slow = ewm_fast ? drift becomes 0 immediately.
        //
        // Rationale: partial snap (e.g. 1-exp(-x/20)) was tested and rejected:
        //   - After a 60pt drop, drift ? -40. Even a 40pt reversal only reduces
        //     drift to -5.4 (86% snap). GFE still can't fire LONG (needs drift > 0.30).
        //     Recovery from -5.4 takes ~114 more ticks = another 19 minutes missed.
        //   - Full snap sets drift = 0. Then 3 ticks of 1.33pt/tick surge ? drift = +0.35.
        //     GFE LONG fires in ~30 seconds instead of 25 minutes.
        //
        // Safety: caller already verified reversal >= 2?ATR (5pt minimum) before
        // calling here. A 5pt move in the new direction is genuine -- not noise.
        // Combined with one-shot-per-close + 120s window in main.cpp, false snaps
        // are impossible in normal market conditions.
        const double old_drift = ewm_fast_ - ewm_slow_;
        ewm_slow_ = ewm_fast_;  // drift = 0; next tick in new direction fires GFE
        printf("[DRIFT-RESET] reversal=%.1fpt old_drift=%.2f -> 0.00 (full snap)\n",
               reversal_pts, old_drift);
        fflush(stdout);
    }

    // is_drift_trending() -- Asia bracket gate.
    // Returns true if a real directional trend is detected, false if confirmed chop.
    //
    // Three-tier decision using EWM + L2 imbalance:
    //
    // TIER 1 -- EWM fully warmed (512+ ticks, ~8-10 min):
    //   Primary signal. |ewm_fast - ewm_slow| > $8 = trend confirmed.
    //   Long-window: 512-tick range > $20 AND price at extreme = trend.
    //   If neither: confirmed chop ? block.
    //
    // TIER 2 -- EWM warming up (<512 ticks) but initialised:
    //   EWM not reliable yet. Use L2 imbalance as real-time confirmation.
    //   L2 is order-book data -- available immediately, no warmup needed.
    //   l2_imbalance < 0.35 = ask-heavy (sellers dominating) = confirms downtrend.
    //   l2_imbalance > 0.65 = bid-heavy (buyers dominating) = confirms uptrend.
    //   0.35-0.65 = balanced book = no directional conviction ? block.
    //
    // TIER 3 -- EWM not yet initialised (first tick):
    //   Use L2 only. Same thresholds as Tier 2.
    bool is_drift_trending(double l2_imbalance = 0.5) const {
        // Helper: L2 confirms directional pressure
        const bool l2_directional = (l2_imbalance < 0.35 || l2_imbalance > 0.65);

        // EWM not yet initialised -- L2 only
        if (!ewm_init_) return l2_directional;

        // EWM strong drift signal -- always allow regardless of L2
        const double d = ewm_fast_ - ewm_slow_;
        if (std::fabs(d) > 8.0) return true;

        // Long-window check -- only meaningful once we have 512 ticks
        if (drift_buf_.size() >= 512) {
            const double dr     = drift_buf_.max() - drift_buf_.min();
            const double centre = (drift_buf_.max() + drift_buf_.min()) * 0.5;
            if (dr > 20.0 && std::fabs(ewm_fast_ - centre) > dr * 0.35) return true;
            // Fully warmed, no drift ? confirmed chop ? block (ignore L2)
            return false;
        }

        // EWM warming up (<512 ticks) -- L2 primary, early EWM as fallback
        // If L2 neutral but EWM shows early trend signal, allow entry.
        // Prevents missing fast moves during the 8-min EWM warmup window.
        if (l2_directional) return true;
        if (std::fabs(d) > 3.0) return true;  // $3 early EWM spread = trend signal
        return false;
    }
    static const char* name(MarketRegime r){
        switch(r){
            case MarketRegime::COMPRESSION:    return "COMPRESSION";
            case MarketRegime::TREND:          return "TREND";
            case MarketRegime::MEAN_REVERSION: return "MEAN_REVERSION";
            case MarketRegime::IMPULSE:        return "IMPULSE";
        }
        return "UNKNOWN";
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// VolatilityFilter
// ?????????????????????????????????????????????????????????????????????????????
class VolatilityFilter {
    MinMaxCircularBuffer<double,64> history_;
    static constexpr size_t WINDOW=50;
    static constexpr double VOL_THRESHOLD=1.50;  // reduced 2.50?1.50: $2.50 was blocking normal mid-session activity; $1.50 still filters dead flat tape while allowing real setups
public:
    bool allow(double mid){
        history_.push_back(mid);
        if(history_.size()<WINDOW)return false;
        return history_.range()>=VOL_THRESHOLD;
    }
    double current_range() const { return history_.empty()?0:history_.range(); }

    // seed(): pre-fill history so allow() is not blind for the first 50 ticks
    // after a restart. Called from load_atr_state() using the saved EWM baseline
    // as the synthetic mid. The buffer fills flat (range=0) so the threshold gate
    // still requires a real live move to open -- we just skip the 40-400s warmup
    // delay that was blocking gold entries entirely on restart during volatile sessions.
    // FIX: vol_range=0.00 on every restart causing gold engines to miss first
    //      10-40 minutes of live action (50 ticks @ dead-zone rate = ~4 min;
    //      50 ticks @ London open rate = still ~40s of blind time on a $20+ move).
    void seed(double mid) {
        if (mid <= 0.0) return;
        if (history_.size() >= WINDOW) return;  // already warm -- don't overwrite live data
        const size_t needed = WINDOW - history_.size();
        for (size_t i = 0; i < needed; ++i)
            history_.push_back(mid);
        printf("[VOL-FILTER] Seeded with mid=%.2f (%zu ticks) -- skipping cold-start warmup\n",
               mid, needed);
        fflush(stdout);
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// GoldEngineStack -- the public interface wired into Omega's on_tick
// ?????????????????????????????????????????????????????????????????????????????
// ?????????????????????????????????????????????????????????????????????????????
// GoldPositionManager -- tracks open position, runs TP/SL/timeout each tick,
// calls on_close with a filled TradeRecord when trade exits.
// ?????????????????????????????????????????????????????????????????????????????
class GoldPositionManager {
    static constexpr double TICK_SIZE     = 0.10;  // XAUUSD minimum price increment
    static constexpr double CONTRACT_SIZE = 1.0;   // notional per trade unit
    static constexpr int    MAX_PYRAMID_LEGS = 5;  // base + 2 regime-gated + 2 extended on long runners
    static constexpr double PYR_COVER_MOVE   = 5.00;  // EA-matched: add only after $5 confirmed move
    static constexpr double PYR_MIN_STEP     = 3.00;  // minimum $3 between pyramid levels
    static constexpr int64_t PYR_ADD_COOLDOWN_SEC = 60; // 60s between add-ons -- confirm trend
    static constexpr int    PYR_TP_TICKS     = 250;  // EA-matched: $25 TP on pyramid legs
    static constexpr int    PYR_SL_TICKS     = 100;  // EA-matched: $10 SL on pyramid legs

    // ?? Runtime members -- set via set_cfg() from GoldStackCfg ?????????????
    // Defaults match calibrated constexpr values prior to config-driven refactor.
    int    MAX_HOLD_SEC       = 1800;  // 30 min: raised from 10min -- slow trend entries need room
    double LOCK_ARM_MOVE      = 1.50;  // lock only after genuine $1.50 move
    double LOCK_GAIN          = 0.60;  // $0.60 lock above entry
    double TRAIL_ARM_1        = 2.50;  // trail after $2.50 move
    double TRAIL_DIST_1       = 0.80;  // trail $0.80 behind mid
    double TRAIL_ARM_2        = 5.00;  // tight-trail only on big $5.00 winners
    double TRAIL_DIST_2       = 0.50;  // tight trail distance
    double MIN_LOCKED_PROFIT  = 0.30;  // must lock meaningful profit above entry+spread
    double MAX_BASE_SL_TICKS  = 50.0;  // DATA-CALIBRATED: $5 SL (50 ticks)

    struct GoldPos {
        bool    active    = false;
        bool    is_long   = true;
        double  entry     = 0;
        double  tp        = 0;   // absolute price level
        double  sl        = 0;   // absolute price level
        double  mfe       = 0;
        double  mae       = 0;
        double  spread_at_entry = 0;
        double  size      = CONTRACT_SIZE;
        int64_t entry_ts  = 0;
        char    engine[32] = {};
        char    reason[32] = {};
        char    regime[32] = {};
    };

    std::vector<GoldPos> legs_;
    int trade_id_ = 1000;  // separate ID range from CRTP engines (those start at 1)
    double  last_add_price_ = 0.0;
    int64_t last_add_ts_    = 0;

    static int64_t nowSec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void emit_close(const GoldPos& leg, double exit_px, const char* why,
                    double latency_ms, const char* regime,
                    std::function<void(const omega::TradeRecord&)>& on_close) {
        // Safety guard: exit_px should never be zero or negative for XAUUSD.
        // If it is, fall back to entry price (flat trade) and log the anomaly.
        if (exit_px <= 0.0) {
            printf("[GOLD-STACK-WARN] emit_close called with exit_px=%.4f why=%s entry=%.4f -- clamping to entry\n",
                   exit_px, why ? why : "?", leg.entry);
            fflush(stdout);
            exit_px = leg.entry;
        }
        omega::TradeRecord tr;
        tr.id          = trade_id_++;
        tr.symbol      = "XAUUSD";
        tr.side        = leg.is_long ? "LONG" : "SHORT";
        tr.entryPrice  = leg.entry;
        tr.exitPrice   = exit_px;
        tr.tp          = leg.tp;
        tr.sl          = leg.sl;
        tr.size        = leg.size;
        tr.pnl         = (leg.is_long ? (exit_px - leg.entry)
                                      : (leg.entry - exit_px)) * leg.size;
        tr.mfe         = leg.mfe;
        tr.mae         = leg.mae;
        tr.entryTs     = leg.entry_ts;
        tr.exitTs      = nowSec();
        tr.exitReason  = why;
        tr.spreadAtEntry = leg.spread_at_entry;
        tr.latencyMs   = latency_ms;
        tr.engine      = std::string(leg.engine);
        tr.regime      = regime ? regime : "";
        if (on_close) on_close(tr);
    }

    void close_leg(size_t idx, double exit_px, const char* why,
                   double latency_ms, const char* regime,
                   std::function<void(const omega::TradeRecord&)>& on_close) {
        if (idx >= legs_.size()) return;
        GoldPos leg = legs_[idx];
        emit_close(leg, exit_px, why, latency_ms, regime, on_close);
        legs_.erase(legs_.begin() + static_cast<long>(idx));
    }

    // apply_tiered_trail: each leg gets progressively tighter trailing.
    // leg_idx 0 = base leg (widest trail -- ride the full move)
    // leg_idx 1 = first pyramid add-on (tighter)
    // leg_idx 2 = second add-on (tighter still)
    // leg_idx 3+ = tightest (maximum profit protection on deepest entries)
    //
    // EA approach: flat $25 trail on everything.
    // Omega improvement: base rides wide, pyramids lock in profits faster.
    // This means on a reversal, base might give back $25 but pyramids only
    // give back $10-15, locking the compounded profit.
    void apply_tiered_trail(GoldPos& leg, double mid, size_t leg_idx, double cur_atr = 0.0) {
        const double move = leg.is_long ? (mid - leg.entry) : (leg.entry - mid);

        // Tier parameters: [lock_arm, lock_gain, trail_arm1, trail_dist1, trail_arm2, trail_dist2]
        // Base leg (idx=0): wide -- mirrors EA's $25 trail philosophy
        // Pyramid 1 (idx=1): medium -- 60% of base trail distances
        // Pyramid 2 (idx=2): tight -- 40% of base trail distances
        // Pyramid 3+ (idx?3): tightest -- 25% of base trail distances
        // Minimum tier_mult raised: 0.25x was strangling pyramid legs -- lock_arm=$0.375
        // caused SL to snap within 4 ticks, then $1.25 slippage turned every exit red.
        // Now: base=1.00x, pyr1=0.80x, pyr2=0.65x -- still tighter than base but survivable.
        const double tier_mult = (leg_idx == 0) ? 1.00 :
                                 (leg_idx == 1) ? 0.80 :
                                 (leg_idx == 2) ? 0.65 : 0.65;

        const double lock_arm   = LOCK_ARM_MOVE  * tier_mult;
        const double lock_gain  = LOCK_GAIN      * tier_mult;
        const double trail_arm1 = TRAIL_ARM_1    * tier_mult;
        const double trail_arm2 = TRAIL_ARM_2    * tier_mult;
        // Trail distances: when cur_atr>0 treat cfg values as ATR multipliers.
        // TRAIL_DIST_1=0.80 ? 0.80?ATR (wide -- ride the move, same philosophy as GoldFlow)
        // TRAIL_DIST_2=0.50 ? 0.50?ATR (tighter on big runners)
        // When cur_atr==0 (warmup): fall back to raw dollar values (legacy behaviour).
        const double trail_d1   = (cur_atr > 0.0) ? (TRAIL_DIST_1 * cur_atr * tier_mult)
                                                   : (TRAIL_DIST_1 * tier_mult);
        const double trail_d2   = (cur_atr > 0.0) ? (TRAIL_DIST_2 * cur_atr * tier_mult)
                                                   : (TRAIL_DIST_2 * tier_mult);

        // BE lock
        if (move >= lock_arm) {
            if (leg.is_long) {
                const double be = leg.entry + lock_gain;
                if (be > leg.sl) leg.sl = be;
            } else {
                const double be = leg.entry - lock_gain;
                if (be < leg.sl) leg.sl = be;
            }
        }
        // Stage 1 trail
        if (move >= trail_arm1) {
            if (leg.is_long) {
                const double trail = mid - trail_d1;
                if (trail > leg.sl) leg.sl = trail;
            } else {
                const double trail = mid + trail_d1;
                if (trail < leg.sl) leg.sl = trail;
            }
        }
        // Stage 2 trail (tight)
        if (move >= trail_arm2) {
            if (leg.is_long) {
                const double trail = mid - trail_d2;
                if (trail > leg.sl) leg.sl = trail;
            } else {
                const double trail = mid + trail_d2;
                if (trail < leg.sl) leg.sl = trail;
            }
        }
    }

    // Legacy wrapper -- keep for any callers that don't pass leg_idx
    void apply_tight_trail(GoldPos& leg, double mid) {
        apply_tiered_trail(leg, mid, 0, 0.0);
    }

    static bool regime_allows_pyramid(const char* regime) {
        if (!regime) return false;
        // Allow pyramid in TREND and IMPULSE -- strong directional regimes.
        // Also allow MEAN_REVERSION and COMPRESSION: if leg_profit_locked passes
        // (SL above entry) AND leader_move >= dyn_cover, the move is real regardless
        // of regime label. Blocking pyramid in COMPRESSION caused missed pyramids on
        // big moves (e.g. 4518->4523 WickRejection LONG that made +$127 with no add-ons).
        return std::strcmp(regime, "TREND") == 0
            || std::strcmp(regime, "IMPULSE") == 0
            || std::strcmp(regime, "MEAN_REVERSION") == 0
            || std::strcmp(regime, "COMPRESSION") == 0;
    }

    bool leg_profit_locked(const GoldPos& leg) const {
        if (leg.is_long) return leg.sl >= leg.entry + MIN_LOCKED_PROFIT;
        return leg.sl <= leg.entry - MIN_LOCKED_PROFIT;
    }

    bool can_add_pyramid(double mid, const char* regime) const {
        if (legs_.empty() || static_cast<int>(legs_.size()) >= MAX_PYRAMID_LEGS) return false;
        if (!regime_allows_pyramid(regime)) return false;
        const int64_t now = nowSec();
        if (now - last_add_ts_ < PYR_ADD_COOLDOWN_SEC) return false;

        const GoldPos& leader = legs_.front();
        const double leader_move = leader.is_long ? (mid - leader.entry) : (leader.entry - mid);

        // Dynamic cover move: pyramid fires at 35% of the base TP distance.
        // Fixed $5 was wrong -- on a $30 TP trade, $5 = only 17% of the way.
        // All 4 pyramids would stack in the first $5 before price reaches TP.
        // 35% of $30 TP = $10.50 -- first pyramid after meaningful progress.
        // Each subsequent pyramid also needs 35% of TP from the LAST add-on entry.
        // Floor: $3 minimum (prevents pyramiding on tiny sub-$10 moves).
        const double base_tp_dist = std::fabs(leader.tp - leader.entry);
        const double dyn_cover = std::max(3.0, base_tp_dist * 0.35);
        const double dyn_step  = std::max(2.0, base_tp_dist * 0.20);

        if (leader_move < dyn_cover) return false;
        for (const auto& leg : legs_) {
            if (!leg_profit_locked(leg)) return false;
        }

        const GoldPos& last = legs_.back();
        const double move = last.is_long ? (mid - last.entry) : (last.entry - mid);
        if (move < dyn_cover) return false;
        if (std::fabs(mid - last_add_price_) < dyn_step) return false;
        return true;
    }

    void add_pyramid_leg(double mid, double spread, double latency_ms, const char* regime) {
        if (legs_.empty() || static_cast<int>(legs_.size()) >= MAX_PYRAMID_LEGS) return;
        const bool is_long = legs_.front().is_long;
        // Inherit size from the base leg so pyramid add-ons match the position scale.
        const double base_size = legs_.front().size;
        GoldPos leg;
        leg.active   = true;
        leg.is_long  = is_long;
        // Realistic fill: LONG pyramid fills at ask, SHORT at bid
        const double fill_px = is_long ? (mid + spread * 0.5) : (mid - spread * 0.5);
        leg.entry    = fill_px;
        // Pyramid TP: use the base leg's remaining TP distance from fill price.
        // This aligns all pyramid exits near the same target zone as the base leg.
        // e.g. base TP at $4,405, pyramid entered at $4,420:
        //   remaining dist = 4420 - 4405 = $15 ? pyramid TP = $4,405 (base TP level)
        // If pyramid entry is already past base TP, use PYR_TP_TICKS as fallback.
        const double base_tp      = legs_.front().tp;
        const double remaining_to_base_tp = is_long
            ? (base_tp - fill_px)
            : (fill_px - base_tp);
        const double pyr_tp_dist = (remaining_to_base_tp > TICK_SIZE * 5)
            ? remaining_to_base_tp                      // still has room to base TP
            : static_cast<double>(PYR_TP_TICKS) * TICK_SIZE;  // beyond base TP, use ticks
        leg.tp = is_long ? fill_px + pyr_tp_dist
                         : fill_px - pyr_tp_dist;
        // SL for pyramid leg: locked to main entry's BE + buffer
        // This means the pyramid leg can only lose back to where the base entered
        // pyr_sl_lock: allow $2.00 BELOW base entry so pyramid has real SL room.
        // Old code: lock = base_entry exactly. When pyramid fills $1 above base,
        // max(fill-$10, base_entry) = base_entry ? only $1 SL room ? trail snaps
        // it to entry+$0.24 after $0.60 move ? slippage ($1.25) > gross profit.
        // Fix: lock = base_entry - $2.00 (LONG) / + $2.00 (SHORT).
        // This means worst case a pyramid gives back the base profit + $2 -- acceptable.
        const double PYR_SL_BUFFER = 2.00;  // $2 below base entry = real SL floor
        const double pyr_sl_lock = is_long
            ? legs_.front().entry - PYR_SL_BUFFER
            : legs_.front().entry + PYR_SL_BUFFER;
        const double pyr_sl_raw  = is_long ? fill_px - PYR_SL_TICKS * TICK_SIZE
                                           : fill_px + PYR_SL_TICKS * TICK_SIZE;
        // SL = best of: $10 from fill OR base_entry - $2 floor
        leg.sl = is_long ? std::max(pyr_sl_raw, pyr_sl_lock)
                         : std::min(pyr_sl_raw, pyr_sl_lock);
        leg.mfe      = 0;
        leg.mae      = 0;
        leg.size     = base_size;  // match base leg size -- not CONTRACT_SIZE=1.0
        leg.spread_at_entry = spread;
        leg.entry_ts = nowSec();
        strncpy(leg.engine, "PYRAMID", 31);
        strncpy(leg.reason, "PYR_ADD", 31);
        strncpy(leg.regime, regime ? regime : "", 31);
        legs_.push_back(leg);
        last_add_price_ = mid;
        last_add_ts_ = leg.entry_ts;
        printf("[GOLD-PYRAMID-ADD] %s lvl=%zu entry=%.2f tp=%.2f sl=%.2f size=%.4f\n",
               is_long ? "LONG" : "SHORT",
               legs_.size(), leg.entry, leg.tp, leg.sl, leg.size);
        fflush(stdout);
        (void)latency_ms;
    }

public:
    bool active() const { return !legs_.empty(); }
    size_t leg_count() const { return legs_.size(); }

    // Live position accessors for GUI per-trade P&L display
    double  base_entry()    const { return legs_.empty() ? 0.0   : legs_.front().entry;    }
    bool    base_is_long()  const { return legs_.empty() ? true  : legs_.front().is_long;  }
    double  base_sl()       const { return legs_.empty() ? 0.0   : legs_.front().sl;       }
    double  base_tp()       const { return legs_.empty() ? 0.0   : legs_.front().tp;       }
    double  base_size()     const { return legs_.empty() ? 0.0   : legs_.front().size;     }
    int64_t base_entry_ts() const { return legs_.empty() ? 0     : legs_.front().entry_ts; }  // UTC seconds
    const char* base_engine() const { return legs_.empty() ? "" : legs_.front().engine;   }

    // Patch the base leg size after compute_size() runs in main.cpp.
    // on_tick() opens the position with the sub-engine default size (e.g. 0.01).
    // main.cpp then computes the correct risk-adjusted lot and calls this to
    // update the base leg so PnL, slippage, and the ledger all use the right size.
    // Only patches index 0 (base leg) -- pyramid legs inherit base size on add.
    void patch_base_size(double lot) {
        if (legs_.empty() || lot <= 0.0) return;
        legs_[0].size = lot;
    }

    // Apply config-driven parameters from GoldStackCfg.
    void set_cfg(int max_hold_sec,
                 double lock_arm_move, double lock_gain,
                 double trail_arm_1,   double trail_dist_1,
                 double trail_arm_2,   double trail_dist_2,
                 double min_locked_profit, double max_base_sl_ticks) {
        MAX_HOLD_SEC      = max_hold_sec;
        LOCK_ARM_MOVE     = lock_arm_move;
        LOCK_GAIN         = lock_gain;
        TRAIL_ARM_1       = trail_arm_1;
        TRAIL_DIST_1      = trail_dist_1;
        TRAIL_ARM_2       = trail_arm_2;
        TRAIL_DIST_2      = trail_dist_2;
        MIN_LOCKED_PROFIT = min_locked_profit;
        MAX_BASE_SL_TICKS = max_base_sl_ticks;
    }

    // Open a new position from a GoldSignal.
    // tp_ticks / sl_ticks are in $0.10 increments (standard gold tick).
    void open(const GoldSignal& sig, double spread,
              double latency_ms, const char* regime) {
        if (!legs_.empty()) return;  // signal-based base entry only when flat
        const double sl_ticks = std::max(4.0, std::min(MAX_BASE_SL_TICKS, sig.sl_ticks));
        // Guard: tp_ticks=0 means leg.tp = entry, TP fires instantly at entry price.
        // Fall back to a minimal TP (2? SL) so the position isn't immediately closed flat.
        const double tp_ticks = (sig.tp_ticks > 0.0) ? sig.tp_ticks : sl_ticks * 2.0;
        if (sig.tp_ticks <= 0.0) {
            printf("[GOLD-STACK-WARN] open() called with tp_ticks=0 engine=%s -- using fallback tp_ticks=%.0f\n",
                   sig.engine, sl_ticks * 2.0);
            fflush(stdout);
        }
        GoldPos leg;
        leg.active   = true;
        leg.is_long  = sig.is_long;
        leg.entry    = sig.entry;
        leg.tp       = sig.is_long
                        ? sig.entry + tp_ticks * TICK_SIZE
                        : sig.entry - tp_ticks * TICK_SIZE;
        leg.sl       = sig.is_long
                        ? sig.entry - sl_ticks * TICK_SIZE
                        : sig.entry + sl_ticks * TICK_SIZE;
        leg.mfe      = 0;
        leg.mae      = 0;
        // Use sig.size (the compute_size() result passed through GoldSignal) so the
        // position manager and PnL ledger reflect the actual broker lot submitted.
        // CONTRACT_SIZE=1.0 was a placeholder -- using it here made ledger PnL 50x
        // the real dollar value (0.02 lots ? $100/pt became 1.0 lot ? $100/pt).
        leg.size     = (sig.size > 0.0) ? sig.size : 0.01;
        leg.spread_at_entry = spread;
        leg.entry_ts = nowSec();
        strncpy(leg.engine, sig.engine, 31);
        strncpy(leg.reason, sig.reason, 31);
        strncpy(leg.regime, regime ? regime : "", 31);
        legs_.clear();
        legs_.push_back(leg);
        last_add_price_ = leg.entry;
        last_add_ts_ = leg.entry_ts;
        printf("[GOLD-STACK-ENTRY] %s entry=%.2f tp=%.2f sl=%.2f eng=%s reason=%s regime=%s\n",
               leg.is_long?"LONG":"SHORT", leg.entry, leg.tp, leg.sl,
               leg.engine, leg.reason, regime?regime:"?");
        fflush(stdout);
        (void)latency_ms;
    }

    // Called every tick while position is open. Manages TP/SL/timeout.
    // Returns true if position was closed this tick.
    bool manage(double bid, double ask, double latency_ms, const char* regime,
                std::function<void(const omega::TradeRecord&)>& on_close,
                double cur_atr = 0.0) {
        if (legs_.empty()) return false;
        double mid = (bid + ask) * 0.5;
        bool closed_any = false;
        const int64_t now = nowSec();

        for (int i = static_cast<int>(legs_.size()) - 1; i >= 0; --i) {
            GoldPos& leg = legs_[static_cast<size_t>(i)];
            const double move = leg.is_long ? (mid - leg.entry) : (leg.entry - mid);
            if (move > leg.mfe) leg.mfe = move;
            if (move < leg.mae) leg.mae = move;

            // Regime change exit -- only close if position has been open >= 60s.
            // Short positions opened during IMPULSE are valid even as regime
            // transitions to MEAN_REVERSION -- gold doesn't stop moving because
            // the regime label changed. Instant REGIME_FLIP exits were causing
            // churn: enter ? regime flips 10s later ? exit ? repeat.
            const int64_t held_so_far = now - leg.entry_ts;
            if (regime && leg.regime[0] != '\0' &&
                std::strncmp(regime, leg.regime, 31) != 0 &&
                held_so_far >= 60) {
                // If the trail has moved past breakeven, the position is
                // self-funding -- let the trail manage the exit. A regime flip
                // on a winner just means the market is transitioning, not reversing.
                if (leg_profit_locked(leg)) {
                    printf("[GOLD-STACK-REGIME_FLIP-SUPPRESSED] %s hold=%lds trail in profit sl=%.2f entry=%.2f\n",
                           leg.engine, (long)held_so_far, leg.sl, leg.entry);
                    fflush(stdout);
                    // Update regime so this logs once per flip, not every tick.
                    std::strncpy(leg.regime, regime, 31);
                    leg.regime[31] = '\0';
                } else {
                    // Cap exit at SL if price has blown through -- same logic as TIMEOUT.
                    // Sparse ticks during reconnect can cause price to drift past SL
                    // without triggering the explicit SL check above. REGIME_FLIP at
                    // raw mid could then record a loss larger than the intended stop.
                    const bool sl_breached = leg.is_long ? (mid < leg.sl) : (mid > leg.sl);
                    const double regime_flip_exit = sl_breached ? leg.sl : mid;
                    close_leg(static_cast<size_t>(i), regime_flip_exit, "REGIME_FLIP", latency_ms, regime, on_close);
                    closed_any = true;
                    continue;
                }
            }

            // Tiered trail: base leg (idx 0) wide, pyramids progressively tighter
            apply_tiered_trail(leg, mid, static_cast<size_t>(i), cur_atr);

            const bool tp_hit = leg.is_long ? (ask >= leg.tp) : (bid <= leg.tp);
            if (tp_hit) {
                const double fill = leg.tp;
                printf("[GOLD-STACK-TP] %s fill=%.2f pnl=%.2f\n",
                       leg.engine, fill,
                       leg.is_long ? (fill - leg.entry) : (leg.entry - fill));
                fflush(stdout);
                close_leg(static_cast<size_t>(i), fill, "TP_HIT", latency_ms, regime, on_close);
                closed_any = true;
                continue;
            }

            const bool sl_hit = leg.is_long ? (bid <= leg.sl) : (ask >= leg.sl);
            if (sl_hit) {
                const double fill = leg.sl;
                printf("[GOLD-STACK-SL] %s fill=%.2f pnl=%.2f\n",
                       leg.engine, fill,
                       leg.is_long ? (fill - leg.entry) : (leg.entry - fill));
                fflush(stdout);
                close_leg(static_cast<size_t>(i), fill, "SL_HIT", latency_ms, regime, on_close);
                closed_any = true;
                continue;
            }

            if (now - leg.entry_ts >= MAX_HOLD_SEC) {
                // If the trail has moved SL past breakeven the position is a winner --
                // suppress the timeout and let the trailing stop handle the exit.
                // Cutting a running trade at an arbitrary time cap defeats the purpose
                // of progressive trailing entirely.
                const bool trail_in_profit = leg.is_long
                    ? (leg.sl >= leg.entry + LOCK_GAIN)
                    : (leg.sl <= leg.entry - LOCK_GAIN);
                if (trail_in_profit) {
                    printf("[GOLD-STACK-TIMEOUT-SUPPRESSED] %s hold=%lds trail in profit sl=%.2f entry=%.2f mfe=%.2f\n",
                           leg.engine, (long)(now - leg.entry_ts), leg.sl, leg.entry, leg.mfe);
                    fflush(stdout);

                    // ?? EXTENDED PYRAMID on long runners ??????????????????????
                    // Profit is locked -- add-on regardless of regime. Fires once
                    // per MAX_HOLD_SEC interval, up to MAX_PYRAMID_LEGS total.
                    // SL for the new leg = current trail SL of the base leg
                    // (already in profit), so add-on risk is zero or better.
                    // Requires TRAIL_ARM_1 ($2.50) move minimum -- no adding into stall.
                    {
                        const int64_t since_last = now - last_add_ts_;
                        const double  base_move  = leg.is_long
                            ? (mid - leg.entry) : (leg.entry - mid);
                        const bool ext_ok =
                            static_cast<int>(legs_.size()) < MAX_PYRAMID_LEGS &&
                            since_last >= static_cast<int64_t>(MAX_HOLD_SEC)  &&
                            base_move  >= TRAIL_ARM_1                         &&
                            leg_profit_locked(leg);
                        if (ext_ok) {
                            printf("[GOLD-STACK-EXTENDED-PYRAMID] leg=%s legs=%d hold=%lds move=%.2f\n",
                                   leg.engine, (int)legs_.size(),
                                   (long)(now - leg.entry_ts), base_move);
                            fflush(stdout);
                            add_pyramid_leg(mid, ask - bid, latency_ms, regime);
                            // Override the new leg's SL to the base leg's trail SL
                            if (!legs_.empty()) legs_.back().sl = leg.sl;
                        }
                    }
                    continue;  // ride it -- trail or TP will close
                }
                // Not in profit -- apply normal timeout.
                // Cap timeout exit at SL if price has blown through -- prevents a
                // position being held 10min at a price far beyond the intended stop.
                // Incident: ImpulseContinuation SHORT timed out at 5013.15 (mid)
                // while SL was 5011.90. SL never triggered due to sparse ticks on
                // reconnect. Timeout at mid = $205 loss vs expected $80 SL loss.
                double timeout_exit = mid;
                const bool sl_breached = leg.is_long ? (mid < leg.sl) : (mid > leg.sl);
                if (sl_breached) {
                    timeout_exit = leg.sl;  // fill at SL price, not at current mid
                }
                printf("[GOLD-STACK-TIMEOUT] %s hold=%lds exit=%.2f%s\n",
                       leg.engine, (long)(now - leg.entry_ts), timeout_exit,
                       sl_breached ? " (capped at SL)" : "");
                fflush(stdout);
                close_leg(static_cast<size_t>(i), timeout_exit, "TIMEOUT", latency_ms, regime, on_close);
                closed_any = true;
                continue;
            }
        }

        // Add-on pyramid legs only after existing edge has covered costs.
        if (!legs_.empty() && can_add_pyramid(mid, regime)) {
            add_pyramid_leg(mid, ask - bid, latency_ms, regime);
        }
        return closed_any;
    }

    // Force-close on disconnect / session end
    void force_close(double bid, double ask, double latency_ms, const char* regime,
                     std::function<void(const omega::TradeRecord&)>& on_close) {
        if (legs_.empty()) return;
        double mid = (bid + ask) * 0.5;
        for (int i = static_cast<int>(legs_.size()) - 1; i >= 0; --i) {
            close_leg(static_cast<size_t>(i), mid, "FORCE_CLOSE", latency_ms, regime, on_close);
        }
    }
};

// ?????????????????????????????????????????????????????????????????????????????
// GoldStackCfg -- all tunable parameters for GoldEngineStack in one struct.
// Populated from [gold_stack] ini section by main.cpp, then passed to
// GoldEngineStack::configure(). Default values match prior constexpr calibration
// so the system is behaviorally identical until the ini is explicitly changed.
// ?????????????????????????????????????????????????????????????????????????????
struct GoldStackCfg {
    // ?? Orchestrator gates ??????????????????????????????????????????????????
    int64_t hard_sl_cooldown_sec        = 120;  // global SL cooldown after any stop hit
    int64_t side_chop_window_sec        = 300;  // rolling window for side-specific SL counting
    int64_t side_chop_pause_sec         = 300;  // pause duration when chop detected on a side
    int64_t same_level_reentry_sec      = 60;   // min seconds before re-entering same price band
    double  same_level_reentry_band     = 1.50; // $-band for same-level detection
    double  min_vwap_dislocation        = 1.20; // min $-distance from VWAP to enter
    double  max_entry_spread            = 2.50;  // max spread at entry (absolute $) -- matches GoldEngineStack runtime default
    int64_t min_entry_gap_sec           = 90;   // min gap between any two entries
    // ?? Position manager ????????????????????????????????????????????????????
    int     max_hold_sec                = 1800; // position timeout -- raised 600?1800: 10min killed valid slow trend entries
    double  lock_arm_move               = 1.50; // move required before locking breakeven
    double  lock_gain                   = 0.60; // breakeven lock distance above entry
    double  trail_arm_1                 = 2.50; // move required to arm first trail (absolute $)
    double  trail_dist_1                = 0.80; // first trail = 0.80x ATR behind mid (ATR mult when cur_atr>0)
    double  trail_arm_2                 = 5.00; // move required to arm tight trail (absolute $)
    double  trail_dist_2                = 0.50; // tight trail = 0.50x ATR behind mid (ATR mult when cur_atr>0)
    double  min_locked_profit           = 0.30; // min profit that must be locked
    double  max_base_sl_ticks           = 30.0; // SL cap for base entries (ticks)
    // ?? LiquiditySweepPro ???????????????????????????????????????????????????
    double  sweep_pro_max_spread        = 4.00;
    int     sweep_pro_sl_ticks          = 18;
    double  sweep_pro_base_size         = 0.01;
    // ?? LiquiditySweepPressure ??????????????????????????????????????????????
    double  sweep_pres_max_spread       = 4.00;
    int     sweep_pres_sl_ticks         = 10;
    double  sweep_pres_base_size        = 0.01;
};

// ?????????????????????????????????????????????????????????????????????????????
class GoldEngineStack {
public:
    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    GoldEngineStack() {
        // SHELVED: CompressionBreakout -- 0% WR live, 7s avg hold, instant SL hits.
        // False breakout / stop-hunt pattern. Re-enable after fix + 50 shadow validates.
        // engines_.push_back(std::make_unique<CompressionBreakoutEngine>());
        // SIM: ImpulseContinuation WR 41.7% -$641 across 3 iterations. Disabled.
        // SessionMomentum captures same directional signal at 53.3% WR +$723.
        // engines_.push_back(std::make_unique<ImpulseContinuationEngine>());
        engines_.push_back(std::make_unique<SessionMomentumEngine>());
        // MomentumContinuation: catches mid-session trend legs WITHOUT pullback.
        // Fills the gap between SessionMomentum (session opens only) and
        // ImpulseContinuation (requires retrace). Active all session.
        // Shadow for 50 trades before enabling live -- start in SHADOW mode.
        engines_.push_back(std::make_unique<MomentumContinuationEngine>());
        // SIM: IntradaySeasonality -- 6,788T | WR=49.2% | $2,065/2yr | Sharpe=1.08
        // Fires once/hour at first tick. COMPRESSION + MEAN_REVERSION regimes.
        engines_.push_back(std::make_unique<IntradaySeasonalityEngine>());
        // SHELVED: WickRejection -- live performance 0% WR, MFE/MAE ratio 0.87x.
        // Re-enable after 50+ shadow trades show positive expectancy.
        // engines_.push_back(std::make_unique<WickRejectionEngine>());
        // SIM: DonchianBreakout -- 3,383T | WR=29.6% | $3,125/2yr | Sharpe=1.60
        engines_.push_back(std::make_unique<DonchianBreakoutEngine>());
        // SIM: NR3Breakout -- 1,421T | WR=39.1% | $1,108/2yr | Sharpe=2.00
        engines_.push_back(std::make_unique<NR3BreakoutEngine>());
        // SIM: SpikeFade -- 402T | WR=54.5% | $456/2yr
        engines_.push_back(std::make_unique<SpikeFadeEngine>());
        // SIM: AsianRange -- 382T | WR=49.7% | $279/2yr | Sharpe=1.60
        engines_.push_back(std::make_unique<AsianRangeEngine>());
        // SIM: DynamicRange -- 10,299T | WR=43.4% | $6,772/2yr | Sharpe=2.36
        engines_.push_back(std::make_unique<DynamicRangeEngine>());
        // SHELVED: WickRejTick -- shelved with WickRejection pending live revalidation.
        // engines_.push_back(std::make_unique<WickRejectionTickEngine>());
        // SIM: TurtleTick -- 104T | WR=49.0% | $800/2yr | Sharpe=7.60
        engines_.push_back(std::make_unique<TurtleTickEngine>());
        // SIM: NR3Tick -- 565T | WR=41.4% | $1,009/2yr | Sharpe=4.10
        engines_.push_back(std::make_unique<NR3TickEngine>());
        // SIM: TwoBarReversal -- 1,216T | WR=47.1% | $795/2yr | Sharpe=1.55
        engines_.push_back(std::make_unique<TwoBarReversalEngine>());
        // SIM: MeanReversion LB=60 Z=2.0 SL=$4 TP=$12 -- 15,955T | WR=59.6% | $4,347/2yr | Sharpe=1.16
        engines_.push_back(std::make_unique<MeanReversionEngine>());
        engines_.push_back(std::make_unique<VWAPSnapbackEngine>());
        engines_.push_back(std::make_unique<LiquiditySweepProEngine>());
        engines_.push_back(std::make_unique<LiquiditySweepPressureEngine>());
        engines_.push_back(std::make_unique<LondonFixMomentumEngine>());
        engines_.push_back(std::make_unique<VWAPStretchReversionEngine>());
        engines_.push_back(std::make_unique<OpeningRangeBreakoutNYEngine>());
        // DXYDivergence: RE-ENABLED with real DX.F feed + proper divergence logic.
        // Previous 2 trades used VWAP-as-DXY-proxy (wrong). Now uses actual DX.F price.
        // Shadow for 30 trades to validate beta calibration before sizing up.
        engines_.push_back(std::make_unique<DXYDivergenceEngine>());
        engines_.push_back(std::make_unique<SessionOpenMomentumEngine>());
    }

    // Apply all config-driven parameters from GoldStackCfg.
    // Call once after load_config() before on_tick() is invoked.
    void configure(const GoldStackCfg& c) {
        // Orchestrator runtime members
        HARD_SL_GLOBAL_COOLDOWN_SEC = c.hard_sl_cooldown_sec;
        SIDE_CHOP_WINDOW_SEC        = c.side_chop_window_sec;
        SIDE_CHOP_PAUSE_SEC         = c.side_chop_pause_sec;
        SAME_LEVEL_REENTRY_SEC      = c.same_level_reentry_sec;
        SAME_LEVEL_REENTRY_BAND     = c.same_level_reentry_band;
        MIN_VWAP_DISLOCATION        = c.min_vwap_dislocation;
        MAX_ENTRY_SPREAD            = c.max_entry_spread;
        MIN_ENTRY_GAP_SEC           = c.min_entry_gap_sec;

        // Position manager
        pos_mgr_.set_cfg(c.max_hold_sec,
                         c.lock_arm_move,   c.lock_gain,
                         c.trail_arm_1,     c.trail_dist_1,
                         c.trail_arm_2,     c.trail_dist_2,
                         c.min_locked_profit, c.max_base_sl_ticks);

        // Sub-engines
        for (auto& e : engines_) {
            if (e->getName() == "LiquiditySweepPro") {
                static_cast<LiquiditySweepProEngine*>(e.get())->apply_cfg(
                    c.sweep_pro_max_spread, c.sweep_pro_sl_ticks, c.sweep_pro_base_size);
            } else if (e->getName() == "LiquiditySweepPressure") {
                static_cast<LiquiditySweepPressureEngine*>(e.get())->apply_cfg(
                    c.sweep_pres_max_spread, c.sweep_pres_sl_ticks, c.sweep_pres_base_size);
            }
        }

        printf("[GOLD-STACK-CFG] gap=%llds sl_cooldown=%llds chop_win=%llds chop_pause=%llds\n"
               "                 vwap_min=%.2f spread_max=%.2f max_hold=%ds\n"
               "                 trail1_arm=%.2f trail1_dist=%.2f trail2_arm=%.2f trail2_dist=%.2f\n",
               (long long)MIN_ENTRY_GAP_SEC, (long long)HARD_SL_GLOBAL_COOLDOWN_SEC,
               (long long)SIDE_CHOP_WINDOW_SEC, (long long)SIDE_CHOP_PAUSE_SEC,
               MIN_VWAP_DISLOCATION, MAX_ENTRY_SPREAD, c.max_hold_sec,
               c.trail_arm_1, c.trail_dist_1, c.trail_arm_2, c.trail_dist_2);
        fflush(stdout);
    }

    // Call every tick.
    // on_close is called when a position closes (TP/SL/timeout/force).
    // can_enter=false ? manage existing position only, no new entries.
    // Returns valid GoldSignal if a NEW entry was just opened this tick.
    GoldSignal on_tick(double bid, double ask, double latency_ms,
                       CloseCallback on_close = nullptr, bool can_enter = true,
                       double dx_mid = 0.0) {
        if(bid<=0||ask<=0||bid>=ask) return GoldSignal{};
        double spread = ask - bid;
        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));

        // ?? Manage existing position (TP/SL/timeout) ?????????????????????????
        // Wrap on_close to track side-specific chop and selective cooldowns.
        CloseCallback wrapped_close;
        if (on_close) {
            wrapped_close = [this, &on_close](const omega::TradeRecord& tr) {
                on_close(tr);
                note_close_for_quality(tr);
            };
        } else {
            wrapped_close = [this](const omega::TradeRecord& tr) {
                note_close_for_quality(tr);
            };
        }
        bool just_closed = pos_mgr_.manage(bid, ask, latency_ms,
                                           current_regime_name(), wrapped_close,
                                           governor_.window_range());

        // If a position closed this tick, stamp last_entry_ts_ to now so the
        // MIN_ENTRY_GAP_SEC check below cannot be bypassed by same-tick re-entry.
        // Previously last_entry_ts_ was only set when a new entry opened -- a position
        // closing and immediately re-entering on the same tick had a stale timestamp
        // and fired through the gap check as if 90s had already elapsed.
        if (just_closed) last_entry_ts_ = now_s;

        // Update has_open_pos_ so regime governor freezes while in trade
        has_open_pos_ = pos_mgr_.active();

        // Build snapshot
        GoldSnapshot snap;
        snap.bid=bid; snap.ask=ask;
        snap.prev_mid=(last_mid_>0)?last_mid_:((bid+ask)*0.5);
        snap.dx_mid = dx_mid;  // DX.F mid from main.cpp -- used by DXYDivergenceEngine
        features_.update(snap,bid,ask);
        last_mid_=snap.mid;

        // Update long-window baseline for supervisor vol_ratio
        baseline_buf_.push_back(snap.mid);
        if (baseline_buf_.size() >= 40) {  // min warmup before using baseline
            const double bl_range = baseline_buf_.max() - baseline_buf_.min();
            const double tick_vol = (snap.mid > 0.0) ? (bl_range / snap.mid * 100.0) : 0.0;
            // EWM baseline: ?=0.002, decays slowly so baseline stays elevated
            // during AND after trends -- vol_ratio stays > 1 throughout the move.
            if (!ewm_vol_init_) { ewm_vol_baseline_ = tick_vol; ewm_vol_init_ = true; }
            ewm_vol_baseline_ = 0.002 * tick_vol + 0.998 * ewm_vol_baseline_;
            // Use the MAX of rolling range and EWM as baseline.
            // Rolling range catches fast spikes; EWM holds the level during slow grinds.
            baseline_vol_pct_ = std::max(tick_vol * 0.5, ewm_vol_baseline_);
        }

        // Regime classification (frozen while position open)
        MarketRegime regime=governor_.detect(snap.mid,has_open_pos_);
        governor_.apply(engines_,regime);
        current_regime_=regime;
        apply_asian_session_overrides(snap.session);
        apply_london_session_overrides(snap.session);

        // ?? Session-change guard ?????????????????????????????????????????
        // When session transitions (ASIAN?LONDON, LONDON?NEWYORK etc),
        // reset SessionMomentumEngine's price history so it cannot fire on
        // stale cross-session data. The IMPULSE regime from Asia may persist
        // into London open enabling SessionMomentum, but its 60-tick window
        // would contain only Asia prices -- not London directional context.
        // Confirmed cause: SHORT at 07:00:09 UTC fired on Asia IMPULSE regime
        // carrying into London, with 60-tick history entirely Asia data.
        if (snap.session != last_session_ && last_session_ != SessionType::UNKNOWN) {
            for (auto& e : engines_) {
                if (e->getName() == "SessionMomentum") {
                    // Cast to SessionMomentumEngine and clear its history
                    static_cast<SessionMomentumEngine*>(e.get())->reset_history();
                }
            }
            printf("[GOLD-SESSION-CHANGE] %s ? %s: SessionMomentum history cleared\n",
                   session_name(last_session_), session_name(snap.session));
            fflush(stdout);
        }
        last_session_ = snap.session;

        // Don't look for new entries if already in a position or gated out
        if(has_open_pos_ || !can_enter) return GoldSignal{};

        // Hard-loss cooldown to avoid immediate revenge trading after failed break.
        if(now_s < sl_cooldown_until_) return GoldSignal{};

        // Minimum entry gap -- prevents re-entering immediately after every TP/SL.
        // CompressionBreakout was re-firing on every tick without this gate.
        if(now_s - last_entry_ts_ < MIN_ENTRY_GAP_SEC) return GoldSignal{};

        // Volatility gate
        if(!vol_filter_.allow(snap.mid)) return GoldSignal{};

        // VWAP chop zone gate
        if(snap.vwap>0&&std::fabs(snap.mid-snap.vwap)<MIN_VWAP_DISLOCATION) return GoldSignal{};

        // Fast path: ImpulseContinuation first
        for(auto& e:engines_){
            if(e->getName()=="ImpulseContinuation"&&e->isEnabled()){
                Signal s=e->process(snap);
                if(s.valid){
                    const double score = s.confidence * 1.1; // engine weight for ImpulseContinuation
                    if (!entry_quality_ok(s, score, snap, now_s)) return GoldSignal{};
                    GoldSignal gs=to_gold_signal(s);
                    apply_vol_scaled_sl(gs);
                    pos_mgr_.open(gs, spread, latency_ms, current_regime_name());
                    has_open_pos_=true;
                    last_entry_ts_=now_s;
                    return gs;
                }
                break;
            }
        }

        // Inject EWM drift and vol_ratio into engines that need them.
        // CompressionBreakout + MeanReversion: drift gate for momentum blocking.
        // NR3Breakout: vol_ratio gate for coiling-tape detection.
        const double cur_vol_ratio = (baseline_vol_pct_ > 0.0)
            ? governor_.window_range() / (last_mid_ > 0 ? last_mid_ : 1.0)
              * 100.0 / baseline_vol_pct_
            : 1.0;
        for (auto& e : engines_) {
            const auto& nm = e->getName();
            if (nm == "CompressionBreakout") {
                static_cast<CompressionBreakoutEngine*>(e.get())->set_ewm_drift(governor_.ewm_drift());
            } else if (nm == "MeanReversion") {
                static_cast<MeanReversionEngine*>(e.get())->set_ewm_drift(governor_.ewm_drift());
            } else if (nm == "NR3Breakout") {
                static_cast<NR3BreakoutEngine*>(e.get())->set_vol_ratio(cur_vol_ratio);
            }
        }

        // Collect ALL valid signals this tick, pick best + apply confluence boost.
        // Confluence: 2+ engines agree direction -> boost confidence 1.40x (3+: 1.60x).
        // Data: Wick+Donchian agreement lifts WR 43.4%->51.9%, avg $0.17->$2.26.
        // Boost flows to conf_mult in main.cpp gold_stack sizing (caps at 1.25x lot).
        Signal best; double best_score=0;
        int long_count=0, short_count=0;
        for(auto& e:engines_){
            if(e->getName()=="ImpulseContinuation"||!e->isEnabled()) continue;
            Signal s=e->process(snap);
            if(!s.valid) continue;
            double score=s.confidence*e->weight_;
            if(score>best_score){ best_score=score; best=s; }
            if(s.side==TradeSide::LONG)  ++long_count;
            if(s.side==TradeSide::SHORT) ++short_count;
        }
        if(best.valid){
            const int agree=(best.side==TradeSide::LONG ? long_count : short_count);
            if(agree>=2){
                const double boost=(agree>=3)?1.60:1.40;
                best.confidence=std::min(1.5, best.confidence*boost);
            }
        }
        if(best.valid){
            if (!entry_quality_ok(best, best_score, snap, now_s)) return GoldSignal{};
            GoldSignal gs=to_gold_signal(best);
            apply_vol_scaled_sl(gs);
            pos_mgr_.open(gs, spread, latency_ms, current_regime_name());
            has_open_pos_=true;
            last_entry_ts_=now_s;
            return gs;
        }
        return GoldSignal{};
    }

    // Force-close on session end or disconnect
    void force_close(double bid, double ask, double latency_ms,
                     CloseCallback on_close) {
        pos_mgr_.force_close(bid, ask, latency_ms, current_regime_name(), on_close);
        has_open_pos_ = false;
    }

    // Legacy: kept for compatibility -- managed internally now
    void set_has_open_position(bool v) { (void)v; }

    bool has_open_position() const { return pos_mgr_.active(); }

    // True when GoldStack has an active position that has moved >= TRAIL_ARM_1 ($10)
    // in profit -- i.e. the trail is armed and we're in a confirmed winner.
    // Used by main.cpp to allow bracket pyramid alongside a winning GoldStack position.
    bool has_profitable_trail() const {
        if (!pos_mgr_.active()) return false;
        const double entry   = pos_mgr_.base_entry();
        const bool   is_long = pos_mgr_.base_is_long();
        const double sl      = pos_mgr_.base_sl();
        if (entry <= 0.0 || sl <= 0.0) return false;
        // Trail is armed when SL has moved past entry in the profit direction.
        // Threshold: $5 move minimum (conservative -- config uses $10 arm but SL
        // moves gradually so $5 SL displacement = confirmed profitable trail).
        const double arm = 5.0;
        const double sl_move = is_long ? (sl - (entry - arm))
                                       : ((entry + arm) - sl);
        return sl_move > 0.0;
    }

    // Clear SL cooldown immediately -- used by reversal logic when GoldFlow
    // SL_HIT and drift has reversed direction. Allows GoldStack counter-entry
    // without waiting the full 120s cooldown.
    void clear_sl_cooldown() { sl_cooldown_until_ = 0; }

    int64_t sl_cooldown_until() const { return sl_cooldown_until_; }

    // Live position accessors for GUI per-trade P&L -- delegated from pos_mgr_
    double      live_entry()    const { return pos_mgr_.base_entry();    }
    bool        live_is_long()  const { return pos_mgr_.base_is_long();  }
    double      live_sl()       const { return pos_mgr_.base_sl();       }
    double      live_tp()       const { return pos_mgr_.base_tp();       }
    double      live_size()     const { return pos_mgr_.base_size();     }
    int64_t     live_entry_ts() const { return pos_mgr_.base_entry_ts(); }  // UTC seconds -- stale detection
    const char* live_engine()   const { return pos_mgr_.base_engine();   }

    // Call immediately after on_tick() returns a valid signal to apply the
    // risk-adjusted lot size from compute_size(). Without this, the ledger
    // records PnL on the sub-engine default (0.01 lots) not the actual size.
    void patch_position_size(double lot) { pos_mgr_.patch_base_size(lot); }
    const char* regime_name()    const { return RegimeGovernor::name(current_regime_); }
    double vwap()                const { return features_.get_vwap(); }
    double vol_range()           const { return vol_filter_.current_range(); }
    double governor_range()      const { return governor_.window_range(); }
    double governor_hi()         const { return governor_.window_hi(); }
    double governor_lo()         const { return governor_.window_lo(); }
    // Raw EWM drift -- detects sustained directional move before regime lock confirms it.
    // Useful for Asia bracket gate: fires on real trends even when regime is still
    // classified as MEAN_REVERSION due to lag in CONFIRM_TICKS.
    double ewm_drift()                              const { return governor_.ewm_drift(); }
    bool   is_drift_trending(double l2_imb = 0.5)  const { return governor_.is_drift_trending(l2_imb); }
    // Reset stale EWM drift after a confirmed price reversal.
    // Called from main.cpp when GoldFlow closes and price immediately moves
    // >= reversal_pts in the opposite direction. Snaps ewm_slow toward
    // ewm_fast so the new move direction registers within a few ticks
    // instead of waiting 150+ ticks for the slow EWM to recover naturally.
    void reset_drift_on_reversal(double reversal_pts) noexcept {
        governor_.reset_drift_on_reversal(reversal_pts);
    }
    // recent_vol_pct: governor 80-tick range as pct of price
    // base_vol_pct:   EWM-smoothed baseline (doesn't chase trends -- stays elevated)
    double recent_vol_pct() const {
        if (governor_.window_range() <= 0.0 || last_mid_ <= 0.0) return 0.0;
        return governor_.window_range() / last_mid_ * 100.0;
    }
    double base_vol_pct() const { return baseline_vol_pct_; }

    void print_stats() const {
        for(const auto& e:engines_)
            printf("[GOLD-ENGINE] %-26s signals=%llu\n",
                   e->getName().c_str(),(unsigned long long)e->signal_count_);
        fflush(stdout);
    }

    // ?? Warm-restart persistence ??????????????????????????????????????????????
    // Saves vol baseline + governor EWM state so next restart skips the
    // 400-600 tick cold warmup for GoldStack regime detection.
    void save_atr_state(const std::string& path) const noexcept {
        FILE* fp = fopen(path.c_str(), "w");
        if (!fp) return;
        fprintf(fp, "ewm_vol_baseline=%.6f\n", ewm_vol_baseline_);
        fprintf(fp, "baseline_vol_pct=%.6f\n", baseline_vol_pct_);
        fprintf(fp, "ewm_vol_init=%d\n",        ewm_vol_init_ ? 1 : 0);
        fprintf(fp, "gov_ewm_fast=%.6f\n",      governor_.ewm_fast_);
        fprintf(fp, "gov_ewm_slow=%.6f\n",      governor_.ewm_slow_);
        fprintf(fp, "gov_ewm_init=%d\n",        governor_.ewm_init_ ? 1 : 0);
        fprintf(fp, "saved_ts=%lld\n",          (long long)std::time(nullptr));
        fclose(fp);
    }

    void load_atr_state(const std::string& path) noexcept {
        FILE* fp = fopen(path.c_str(), "r");
        if (!fp) return;
        char line[128];
        int64_t saved_ts = 0;
        while (fgets(line, sizeof(line), fp)) {
            char key[64]; double val = 0.0;
            if (sscanf(line, "%63[^=]=%lf", key, &val) != 2) continue;
            const std::string k(key);
            if      (k == "ewm_vol_baseline") ewm_vol_baseline_ = val;
            else if (k == "baseline_vol_pct") baseline_vol_pct_ = val;
            else if (k == "ewm_vol_init")     ewm_vol_init_     = (val > 0.5);
            else if (k == "gov_ewm_fast")     governor_.ewm_fast_ = val;
            else if (k == "gov_ewm_slow")     governor_.ewm_slow_ = val;
            else if (k == "gov_ewm_init")     governor_.ewm_init_ = (val > 0.5);
            else if (k == "saved_ts")         saved_ts = static_cast<int64_t>(val);
        }
        fclose(fp);
        // Discard state older than 4 hours (overnight gap / weekend)
        const int64_t age = static_cast<int64_t>(std::time(nullptr)) - saved_ts;
        if (age > 4 * 3600 || age < 0) {
            ewm_vol_baseline_ = 0.0; baseline_vol_pct_ = 0.0; ewm_vol_init_ = false;
            governor_.ewm_fast_ = 0.0; governor_.ewm_slow_ = 0.0; governor_.ewm_init_ = false;
            printf("[GOLDSTACK] State stale (age=%llds) -- cold start\n", (long long)age);
            return;
        }
        printf("[GOLDSTACK] Warm restart: ewm_vol_baseline=%.4f gov_ewm_fast=%.2f"
               " gov_ewm_slow=%.2f age=%llds\n",
               ewm_vol_baseline_, governor_.ewm_fast_, governor_.ewm_slow_, (long long)age);
        fflush(stdout);
        // Seed VolatilityFilter history so allow() is not blind for first 50 ticks.
        // Uses saved EWM baseline as synthetic mid -- buffer fills flat (range=0),
        // threshold still requires a real live move to open. Just skips the
        // cold-start delay that was blocking all gold entries 40s-4min on every restart.
        // FIX: vol_range=0.00 on startup was causing engines to miss entire volatile moves.
        if (ewm_vol_baseline_ > 0.0) {
            // Derive approximate mid from saved vol pct.
            // ewm_vol_baseline_ = vol as fraction of mid. baseline_vol_pct_ = vol/mid*100.
            // mid = ewm_vol_baseline_ / (baseline_vol_pct_ / 100)
            double seed_mid = 3000.0;  // safe XAUUSD fallback
            if (baseline_vol_pct_ > 0.0)
                seed_mid = ewm_vol_baseline_ / (baseline_vol_pct_ / 100.0);
            if (seed_mid < 1000.0 || seed_mid > 10000.0) seed_mid = 3000.0;
            vol_filter_.seed(seed_mid);
        }
    }

private:
    // ?? Runtime members -- set via configure() from GoldStackCfg ???????????
    // Defaults match calibrated constexpr values prior to config-driven refactor.
    int64_t HARD_SL_GLOBAL_COOLDOWN_SEC = 120; // raised 60?120: 60s wasn't stopping revenge entries after hard stops
    int64_t SIDE_CHOP_WINDOW_SEC        = 300; // raised 90?300: 90s window expired before detecting London chop pattern
    int64_t SIDE_CHOP_PAUSE_SEC         = 300; // raised 60?300: 60s pause was too short -- chop resumed after pause
    size_t  SIDE_CHOP_TRIGGER_COUNT     = 2;   // keep at 2: still want early chop detection
    int64_t SAME_LEVEL_REENTRY_SEC      = 60;  // raised 30?60: 30s allowed near-instant re-entries at same level
    double  SAME_LEVEL_REENTRY_BAND     = 1.50;// raised 0.80?1.50: $0.80 band was too tight
    double  MIN_VWAP_DISLOCATION        = 1.20;// raised 0.80?1.20: entries within $1.20 of VWAP are noise territory
    double  MAX_ENTRY_SPREAD            = 2.50;  // raised 1.60?2.50: matches gold spread reality in London ($1.50-$2.50)
    double  IMPULSE_MIN_CONFIDENCE      = 1.05;
    double  IMPULSE_MIN_SCORE           = 1.20;
    double  GENERAL_MIN_SCORE           = 1.20;
    int64_t MIN_ENTRY_GAP_SEC           = 90;  // raised 30?90: CB was re-firing 2-3x per compression box

    std::vector<std::unique_ptr<EngineBase>> engines_;
    GoldFeatures     features_;
    RegimeGovernor   governor_;
    VolatilityFilter vol_filter_;
    GoldPositionManager pos_mgr_;
    MarketRegime     current_regime_=MarketRegime::MEAN_REVERSION;
    bool             has_open_pos_=false;
    double           last_mid_=0;
    int64_t          sl_cooldown_until_=0;
    int64_t          last_entry_ts_=0;     // timestamp of last entry -- enforces MIN_ENTRY_GAP_SEC
    std::array<int64_t, 2> side_pause_until_{{0,0}};
    std::array<std::deque<int64_t>, 2> side_hard_sl_times_{};
    // Session-change tracking -- reset session-specific engine histories
    // when session transitions (e.g. ASIAN?LONDON) so engines don't fire
    // on stale cross-session data (e.g. Asia impulse carrying into London open)
    SessionType last_session_ = SessionType::UNKNOWN;
    std::array<double, 2> last_exit_price_{{0.0,0.0}};
    std::array<int64_t, 2> last_exit_ts_{{0,0}};
    // Long-window baseline vol tracker for supervisor -- 400-tick rolling window
    // (~60-80s at typical gold tick rate). Provides a genuine measured baseline
    // so supervisor vol_ratio = recent_range / baseline_range is fully real.
    MinMaxCircularBuffer<double,512> baseline_buf_;
    double baseline_vol_pct_ = 0.0;  // cached: baseline_range / mid * 100
    // EWM baseline: slow-decaying volatility estimate that doesn't chase trends.
    // ?=0.002 ? half-life ~350 ticks (~3min). Drifts up on expansion, stays
    // elevated after, giving vol_ratio > 1 during and after trend moves.
    double ewm_vol_baseline_ = 0.0;
    bool ewm_vol_init_ = false;

    static int side_idx(TradeSide side) {
        if (side == TradeSide::LONG) return 0;
        if (side == TradeSide::SHORT) return 1;
        return -1;
    }
    static int side_idx(const std::string& side) {
        if (side == "LONG") return 0;
        if (side == "SHORT") return 1;
        return -1;
    }
    static const char* side_name(int idx) {
        return idx == 0 ? "LONG" : "SHORT";
    }

    bool side_paused(TradeSide side, int64_t now_s) const {
        const int idx = side_idx(side);
        return idx >= 0 && now_s < side_pause_until_[static_cast<size_t>(idx)];
    }

    bool same_level_reentry_blocked(TradeSide side, double entry, int64_t now_s) const {
        const int idx = side_idx(side);
        if (idx < 0) return false;
        const size_t u = static_cast<size_t>(idx);
        if (now_s - last_exit_ts_[u] > SAME_LEVEL_REENTRY_SEC) return false;
        return std::fabs(entry - last_exit_price_[u]) < SAME_LEVEL_REENTRY_BAND;
    }

    bool entry_quality_ok(const Signal& s, double score, const GoldSnapshot& snap, int64_t now_s) const {
        if (snap.spread > MAX_ENTRY_SPREAD) return false;
        if (snap.vwap > 0.0 && std::fabs(s.entry - snap.vwap) < MIN_VWAP_DISLOCATION) return false;

        // ?? Min trade value gate ?????????????????????????????????????????????
        // Kill entries where expected gross profit < 1.5? round-trip slippage.
        // Prevents the -$2.52/-$3.57 trades where TP sits inside the spread band.
        // s.tp is in price ticks (100 ticks = $10 at 0.01 lot, $1/pt).
        // tp_usd  = s.tp * 0.10  (ticks ? USD at 0.01 lot)
        // slip    = spread ? 1.5 (one-way; ?2 round-trip factored into the 1.5 gate)
        if (s.tp > 0.0 && snap.spread > 0.0) {
            const double tp_usd       = s.tp * 0.10;       // TP in USD at base 0.01 lot
            const double slippage_est = snap.spread * 1.5; // estimated one-way cost
            if (tp_usd < slippage_est * 1.5) {
                std::cout << "[GOLD-QUALITY] MIN_TRADE_GATE blocked:"
                          << " engine=" << (s.engine[0] ? s.engine : "?")
                          << " tp_usd=" << tp_usd
                          << " slip_est=" << slippage_est
                          << " spread=" << snap.spread << "\n";
                std::cout.flush();
                return false;
            }
        }

        if (s.engine[0] != '\0' && std::strcmp(s.engine, "ImpulseContinuation") == 0) {
            if (s.confidence < IMPULSE_MIN_CONFIDENCE || score < IMPULSE_MIN_SCORE) return false;
        } else if (s.engine[0] != '\0' && std::strcmp(s.engine, "CompressionBreakout") == 0) {
            // CB confidence = min(1.5, breakout_distance / BREAKOUT_TRIGGER).
            // With TRIGGER=$2.50: confidence=1.0 means price moved exactly $2.50 past the box.
            // Require >= 1.0 -- the minimal qualifying break. Anything below means CB fired
            // at the very edge of the trigger (noise). No score bypass -- CB must pass this.
            if (s.confidence < 1.0) return false;
        } else {
            if (score < GENERAL_MIN_SCORE) return false;
        }
        if (side_paused(s.side, now_s)) return false;
        if (same_level_reentry_blocked(s.side, s.entry, now_s)) return false;
        return true;
    }

    void note_close_for_quality(const omega::TradeRecord& tr) {
        const int idx = side_idx(tr.side);
        if (idx < 0) return;
        const size_t u = static_cast<size_t>(idx);
        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));
        last_exit_price_[u] = tr.exitPrice;
        last_exit_ts_[u] = now_s;
        // Also update last_entry_ts_ on any close so same-tick re-entry is blocked.
        // Previously last_entry_ts_ was only set when a NEW entry opened -- meaning a
        // position that closed and re-entered on the same tick had a stale last_entry_ts_
        // and bypassed the MIN_ENTRY_GAP_SEC check entirely.
        last_entry_ts_ = std::max(last_entry_ts_, now_s);

        if (tr.exitReason != "SL_HIT") return;

        // Fire cooldown on ALL SL_HIT exits, not just negative pnl ones.
        // Trailing stops that lock above entry produce pnl>0 but are still failed
        // breakouts -- skipping cooldown allowed immediate re-entry into the same chop.
        // Old guard: if (tr.pnl > 0.0) return;  <-- REMOVED
        sl_cooldown_until_ = std::max(sl_cooldown_until_, now_s + HARD_SL_GLOBAL_COOLDOWN_SEC);

        auto& q = side_hard_sl_times_[u];
        q.push_back(now_s);
        while (!q.empty() && now_s - q.front() > SIDE_CHOP_WINDOW_SEC) q.pop_front();
        if (q.size() >= SIDE_CHOP_TRIGGER_COUNT) {
            side_pause_until_[u] = now_s + SIDE_CHOP_PAUSE_SEC;
            q.clear();
            printf("[GOLD-CHOP-PAUSE] side=%s pause=%llds window=%llds\n",
                   side_name(idx),
                   static_cast<long long>(SIDE_CHOP_PAUSE_SEC),
                   static_cast<long long>(SIDE_CHOP_WINDOW_SEC));
            fflush(stdout);
        }
    }

    void apply_asian_session_overrides(SessionType session) {
        if (session != SessionType::ASIAN) return;

        // ?? Asian session engine policy ???????????????????????????????????????
        //
        // CHOP PROTECTION (dominant case -- MR/COMPRESSION regime):
        //   Only CompressionBreakout is allowed.
        //   Asian tape is thin and mean-reverting ~80% of sessions.
        //   Incident Mar 17 2026 00:47 UTC: ImpulseContinuation fired SHORT in
        //   MR regime, $4.26 below VWAP already reverting up -- $205 loss.
        //   All other engines require more ticks/structure than Asia provides
        //   in normal ranging conditions.
        //
        // TREND EXCEPTION (TREND or IMPULSE regime confirmed by governor):
        //   A confirmed TREND/IMPULSE means the 80-tick window range has exceeded
        //   $15 (TE threshold) and held for CONFIRM_TICKS=5 consecutive ticks.
        //   This is NOT Asian chop -- this is a genuine institutional move:
        //   a $100 Sunday gap, an Asia macro event, a China open surge.
        //   Enable trend-following engines that have their own internal HTF
        //   direction filters -- they will NOT fire counter-trend.
        //
        // PROTECTION LAYERS THAT REMAIN ACTIVE EVEN IN TREND MODE:
        //   1. RegimeGovernor: CONFIRM_TICKS=5 required before TREND confirmed.
        //      Single-tick noise cannot unlock this path.
        //   2. DonchianBreakout: HTF EMA50/250 filter -- fires SHORT only when
        //      EMA50 < EMA250. Self-protecting against counter-trend.
        //   3. TurtleTick: identical EMA50/250 filter. Same protection.
        //   4. NR3Tick: requires confirmed squeeze breakout (CONFIRM_PCT=0.40).
        //   5. WickRejectionTickEngine: wick >= 55% bar range, MIN_WICK=$1.50,
        //      bar range >= $2.25 -- not noise.
        //   6. TwoBarReversal: ATR_MULT=1.5x strong bar -- not thin-tape noise.
        //   7. SpikeFade: requires $10+ candle move -- genuine macro event only.
        //   8. entry_quality_ok(): GENERAL_MIN_SCORE=1.20 gate on all signals.
        //   9. SIDE_CHOP_PAUSE: 2 SL hits in 300s triggers 300s pause per side.
        //  10. MIN_ENTRY_GAP_SEC=90: cannot re-enter within 90s of last entry.
        //  11. HARD_SL_GLOBAL_COOLDOWN=120s: any SL hit triggers 120s full stop.
        //  12. MAX_ENTRY_SPREAD=$2.50: applies to all stack entries regardless.
        //  13. GoldFlow Asia hardening: ATR>=$5, ATR/spread>=4x, 90% persistence.
        //      Spread gate raised to $2.50 to handle gap-open spreads (see below).
        //
        // NOT enabled in Asia even during TREND:
        //   - ImpulseContinuation: disabled entirely (WR too low in practice)
        //   - SessionMomentum: wrong window (07:15-10:30, 13:15-15:30 UTC only)
        //   - MeanReversion / VWAPSnapback: mean-rev wrong in a trend
        //   - LiquiditySweepPro: counter-trend fade -- wrong in a trend
        //   - DynamicRange: range mean-reversion -- wrong in a trend
        //   - AsianRange: fires 07:00-11:00 UTC only (London window)

        const bool asia_trend = (current_regime_ == MarketRegime::TREND ||
                                 current_regime_ == MarketRegime::IMPULSE);

        for (auto& e : engines_) {
            const auto& n = e->getName();

            // CompressionBreakout: always enabled (safe in all Asian regimes --
            // requires its own compression range < $6, so it self-gates in trends)
            if (n == "CompressionBreakout") {
                e->setEnabled(true);
                continue;
            }

            // Trend-following engines: only when regime confirms a genuine move.
            // Each has its own internal HTF direction filter (EMA50/250 or equiv)
            // so they cannot fire counter-trend even if enabled here.
            if (asia_trend) {
                if (n == "TurtleTick"      ||  // EMA50/250 HTF + 40-bar tick channel
                    n == "NR3Tick"         ||  // confirmed squeeze breakout required
                    n == "WickRejTick"     ||  // wick>=55% of bar, $1.50 min wick
                    n == "TwoBarReversal"  ||  // ATR_MULT=1.5x strong bar required
                    n == "SpikeFade"       ||  // $10+ candle -- genuine event only
                    n == "DonchianBreakout") { // EMA50/250 HTF filter (bar buffer
                                               // needs ~200min to warm; valid on
                                               // running sessions, not fresh starts)
                    e->setEnabled(true);
                }
            }
        }

        if (asia_trend) {
            static int64_t s_last_asia_log = 0;
            const int64_t now_al = static_cast<int64_t>(std::time(nullptr));
            if (now_al - s_last_asia_log >= 30) {  // rate-limit to once per 30s
                s_last_asia_log = now_al;
                printf("[GOLD-ASIA-TREND] Regime=%s -- trend engines enabled in Asia\n",
                       current_regime_ == MarketRegime::TREND ? "TREND" : "IMPULSE");
                fflush(stdout);
            }
        }
    }

    void apply_london_session_overrides(SessionType session) {
        if (session != SessionType::LONDON) return;
        // London open (07:00-10:30 UTC): override MEAN_REVERSION ? enable CB.
        //
        // ONLY override MEAN_REVERSION lag (governor hasn't confirmed COMPRESSION yet
        // because it needs CONFIRM_TICKS). Do NOT override IMPULSE or TREND --
        // those are correct governor classifications meaning price is already moving.
        // Overriding IMPULSE was the direct cause of the 07:00 SHORT SL_HIT:
        //   governor said IMPULSE (price thrusting up) ? CB disabled (correct)
        //   London override re-enabled CB anyway ? SHORT fired on 1-tick dip ? SL in 19s
        if (current_regime_ == MarketRegime::IMPULSE) return;
        if (current_regime_ == MarketRegime::TREND)   return;
        // Guard: need real vol history before arming CB
        if (vol_filter_.current_range() < 2.00) return;  // raised 0.50?2.00: $0.50 is 2-3 ticks of noise, not structure
        for (auto& e : engines_) {
            if (e->getName() == "CompressionBreakout") {
                e->setEnabled(true);
            }
        }
    }

    const char* current_regime_name() const {
        return RegimeGovernor::name(current_regime_);
    }

    static const char* session_name(SessionType s) {
        switch(s) {
            case SessionType::ASIAN:   return "ASIAN";
            case SessionType::LONDON:  return "LONDON";
            case SessionType::NEWYORK: return "NEWYORK";
            case SessionType::OVERLAP: return "OVERLAP";
            default:                   return "UNKNOWN";
        }
    }

    static GoldSignal to_gold_signal(const Signal& s){
        GoldSignal g;
        g.valid=true; g.is_long=(s.side==TradeSide::LONG);
        g.entry=s.entry; g.tp_ticks=s.tp; g.sl_ticks=s.sl;
        g.confidence=s.confidence;
        g.size = s.size > 0.0 ? s.size : 0.01;  // sub-engine size carries through; 0.01 fallback, never 1.0
        strncpy(g.engine,s.engine,31); strncpy(g.reason,s.reason,31);
        return g;
    }

    // vol_scaled_sl: scale CB (and ImpulseCont) SL/TP with current vol_range.
    // Problem: fixed SL_TICKS=50 ($5) gets hit by normal noise on high-vol moves.
    // During today's 120pt selloff, vol_range=50pts ? noise easily exceeds $5.
    // Solution: SL = clamp(vol_range * 0.40, $5, $15), TP = SL * 2.0 (maintains 2:1 RR).
    // Multiplier validated against 31 Mar 2026 live data (8 moves):
    //   0.20 ? WR=50% +$84  (false stops on 20pt+ vol moves)
    //   0.40 ? WR=83% +$384 (survives 01:00 UTC $2 false stop on +68pt move)
    //   0.50 ? same as 0.40 (most moves capped at $5 floor or $15 ceiling)
    // Risk-based sizing in main.cpp compensates: wider SL = smaller size = same $ risk.
    // Applies ONLY to CompressionBreakout and ImpulseContinuation -- engines that fire
    // at the START of a move where vol_range reflects real current volatility.
    void apply_vol_scaled_sl(GoldSignal& gs) const noexcept {
        const std::string eng(gs.engine);
        if (eng != "CompressionBreakout" && eng != "ImpulseContinuation") return;
        const double vr = vol_filter_.current_range();
        if (vr <= 0.0) return;
        // Scale: 0.40 ? vol_range, clamped [$5, $15] = [50 ticks, 150 ticks]
        // 0.40 validated 31 Mar 2026: fixes false stops on 15-25pt vol moves
        // while keeping $5 floor on dead tape and $15 cap on extreme vol
        const double sl_pts   = std::max(5.0, std::min(15.0, vr * 0.40));
        const double sl_ticks = sl_pts / 0.10;
        const double tp_ticks = sl_ticks * 2.0;  // always 2:1 RR
        if (sl_ticks > gs.sl_ticks) {  // only widen, never tighten below engine floor
            printf("[CB-VOL-SL] %s vol_range=%.1f sl=%.0fticks($%.0f) tp=%.0fticks($%.0f) "
                   "[was sl=%.0f tp=%.0f]\n",
                   gs.engine, vr, sl_ticks, sl_pts, tp_ticks, sl_pts*2.0,
                   gs.sl_ticks, gs.tp_ticks);
            gs.sl_ticks = sl_ticks;
            gs.tp_ticks = tp_ticks;
        }
    }
};

} // namespace gold
} // namespace omega
