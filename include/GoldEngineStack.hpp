#pragma once
// =============================================================================
// GoldEngineStack.hpp — Self-contained gold multi-engine stack for Omega
//
// Ported from ChimeraMetals. Zero external dependencies on ChimeraMetals.
// All 5 engines + RegimeGovernor + Supervisor in one header.
//
// Engines:
//   1. CompressionBreakout   — COMPRESSION regime: tight range → expansion
//   2. ImpulseContinuation   — TREND/IMPULSE regime: directional continuation
//   3. SessionMomentum       — IMPULSE regime: session open volatility expansion
//   4. VWAPSnapback          — MEAN_REVERSION regime: fade exhausted moves to VWAP
//   5. LiquiditySweepPro     — MEAN_REVERSION/IMPULSE: stop-hunt reversal
//   6. LiquiditySweepPressure— MEAN_REVERSION/IMPULSE: pre-sweep pressure detection
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

// ─────────────────────────────────────────────────────────────────────────────
// CircularBuffer / MinMaxCircularBuffer  (ported from ChimeraMetals)
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// MarketSnapshot
// ─────────────────────────────────────────────────────────────────────────────
enum class SessionType { ASIAN, LONDON, NEWYORK, OVERLAP, UNKNOWN };
enum class TradeSide   { LONG, SHORT, NONE };

struct GoldSnapshot {
    double bid=0, ask=0, mid=0, spread=0;
    double vwap=0, volatility=0, trend=0, sweep_size=0, prev_mid=0;
    SessionType session = SessionType::UNKNOWN;
    bool is_valid() const { return bid>0 && ask>0 && bid<ask; }
};

// ─────────────────────────────────────────────────────────────────────────────
// GoldSignal — what the stack returns on a valid entry
// ─────────────────────────────────────────────────────────────────────────────
struct GoldSignal {
    bool   valid      = false;
    bool   is_long    = true;
    double entry      = 0.0;
    double tp_ticks   = 0.0;   // in price ticks (0.10 per tick for gold)
    double sl_ticks   = 0.0;
    double confidence = 0.0;
    double size       = 1.0;   // contract size — carried from sub-engine Signal.size
    char   engine[32] = {};
    char   reason[32] = {};
};

// ─────────────────────────────────────────────────────────────────────────────
// GoldFeatures — VWAP + session + PriceStdDev state (replaces ChimeraMetals MarketFeatures)
// ─────────────────────────────────────────────────────────────────────────────
class GoldFeatures {
    double cum_pv_=0, cum_vol_=0, vwap_=0;
    double last_price_=0, volatility_=0;
    double sweep_hi_=0, sweep_lo_=0;
    double trend_=0;
    CircularBuffer<double,256> price_window_;
    int tick_counter_=0;
    int last_reset_day_=-1;  // UTC day-of-year; prevents double-reset same day

    // UTC hour → SessionType
    static SessionType classify_session() {
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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

        // VWAP — cumulative tick-weighted (resets at midnight)
        cum_pv_ += mid; cum_vol_ += 1.0;
        vwap_ = (cum_vol_>0) ? cum_pv_/cum_vol_ : mid;

        // Tick-to-tick volatility (for momentum gates)
        if (last_price_>0) {
            double move = std::fabs(mid-last_price_);
            volatility_ = 0.9*volatility_ + 0.1*move;
        }
        last_price_ = mid;

        // Price window for stddev (z-score denominator)
        price_window_.push_back(mid);

        // Sweep detection — initialise hi/lo to first mid (not 0)
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

// ─────────────────────────────────────────────────────────────────────────────
// TradeSignal (internal)
// ─────────────────────────────────────────────────────────────────────────────
struct Signal {
    bool valid=false; TradeSide side=TradeSide::NONE;
    double confidence=0,size=0,entry=0,tp=0,sl=0;
    char reason[32]={}, engine[32]={};
    bool is_long() const { return side==TradeSide::LONG; }
};

// ─────────────────────────────────────────────────────────────────────────────
// EngineBase
// ─────────────────────────────────────────────────────────────────────────────
class EngineBase {
public:
    std::string name_; double weight_; bool enabled_=true;
    uint64_t signal_count_=0;
    EngineBase(const char* n, double w): name_(n), weight_(w) {}
    virtual ~EngineBase()=default;
    virtual Signal process(const GoldSnapshot&)=0;
    Signal noSignal() const { return Signal{}; }
    void setEnabled(bool e){ enabled_=e; }
    bool isEnabled() const { return enabled_; }
    const std::string& getName() const { return name_; }
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. CompressionBreakoutEngine
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// CompressionBreakoutEngine — parameter rationale at $5000 gold (Mar 2026)
//
// CALIBRATION BASIS: Last 2 London sessions (Mar 13 + Mar 16 2026)
//   Avg hourly H-L range:  $28–$30
//   Tightest London hour:  $14.00  (Mar 13 09:00 UTC)
//   Normal quiet range:    $14–$22 (pre-NY open)
//   Event candles:         $52–$75 (14:00 UTC macro releases)
//
// OLD values (wrong — calibrated for ~$300 gold, not $5000):
//   COMPRESSION_RANGE = $2.00  → NEVER achieved in London (hourly avg $28)
//   BREAKOUT_TRIGGER  = $0.35  → 0.007% of price, pure tick noise
//   Result: engine fired on sub-minute $0.35 oscillations constantly
//
// NEW values (calibrated for $5000 gold):
//   COMPRESSION_RANGE = $8.00  → achievable in tight 30-tick windows
//                                 sub-hourly consolidation does compress to $6–10
//   BREAKOUT_TRIGGER  = $2.50  → 0.05% of $5000, confirms real directional intent
//                                 filters out the $1–2 oscillations that were causing SL hits
//   MAX_SPREAD        = $2.00  → unchanged, still valid spread gate
//   TP_TICKS          = 50     → $5.00 target (2:1 on $2.50 SL effectively)
//   SL_TICKS          = 20     → $2.00 stop, tight enough to cut losers fast
//
// DEAD ZONE: 21:00–23:00 UTC blocked (NY/Tokyo handoff)
//   Thin liquidity + stale VWAP (resets midnight) + no directional flow
//   4 SL hits in 16 min observed 22:35 UTC Mar 17 before this gate added
// ─────────────────────────────────────────────────────────────────────────────
class CompressionBreakoutEngine : public EngineBase {
    MinMaxCircularBuffer<double,32> history_;
    static constexpr size_t WINDOW        = 30;
    static constexpr double COMPRESSION_RANGE = 8.00;  // was 2.00 — $2 never achievable at $5000 gold
    static constexpr double BREAKOUT_TRIGGER  = 2.50;  // was 0.35 — $0.35 is tick noise, $2.50 is real
    static constexpr double MAX_SPREAD        = 2.00;  // unchanged
    static constexpr int    TP_TICKS          = 50;    // $5.00 target
    static constexpr int    SL_TICKS          = 20;    // $2.00 stop — was 18, matches new trigger scale
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::seconds(5)};

    // NY/Tokyo handoff dead zone: 21:00–23:00 UTC
    // Thin liquidity, erratic spreads, stale VWAP (resets at midnight).
    // Tokyo gold directional flow does not establish until ~23:00 UTC.
    // Without this gate: 4 SL hits in 16 min observed 22:35 UTC Mar 17 2026.
    static bool in_handoff_dead_zone() noexcept {
        const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        return (ti.tm_hour >= 21 && ti.tm_hour < 23);
    }

public:
    CompressionBreakoutEngine(): EngineBase("CompressionBreakout",1.0){}
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
        if(in_handoff_dead_zone()) return noSignal(); // NY/Tokyo handoff — no compression trades
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
        Signal sig; sig.entry=s.mid; sig.size=0.02;
        if(s.mid>hi+BREAKOUT_TRIGGER){
            sig.valid=true; sig.side=TradeSide::LONG;
            sig.confidence=std::min(1.5,(s.mid-hi)/BREAKOUT_TRIGGER);
            sig.tp=TP_TICKS; sig.sl=SL_TICKS;
            strncpy(sig.reason,"COMPRESSION_BREAK_LONG",31);
            strncpy(sig.engine,"CompressionBreakout",31);
            history_.clear(); last_signal_=now; signal_count_++; return sig;
        }
        if(s.mid<lo-BREAKOUT_TRIGGER){
            sig.valid=true; sig.side=TradeSide::SHORT;
            sig.confidence=std::min(1.5,(lo-s.mid)/BREAKOUT_TRIGGER);
            sig.tp=TP_TICKS; sig.sl=SL_TICKS;
            strncpy(sig.reason,"COMPRESSION_BREAK_SHORT",31);
            strncpy(sig.engine,"CompressionBreakout",31);
            history_.clear(); last_signal_=now; signal_count_++; return sig;
        }
        history_.push_back(s.mid);
        return noSignal();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 2. ImpulseContinuationEngine
// ─────────────────────────────────────────────────────────────────────────────
class ImpulseContinuationEngine : public EngineBase {
    MinMaxCircularBuffer<double,64> price_history_;
    static constexpr double IMPULSE_MIN=1.0,PULLBACK_MIN=0.2,PULLBACK_MAX=0.6;
    static constexpr double MIN_MOMENTUM=0.55,MIN_MOVE_5T=0.55,MAX_MOMENTUM=5.0,PARABOLIC_VWAP=10.0;
    static constexpr double MIN_VWAP_DIST=1.50,MAX_VWAP_DIST=6.0,MAX_SPREAD=2.20;
    static constexpr int TP_TICKS=16,SL_TICKS=8;
    static constexpr int MAX_ENTRIES_PER_TREND=2,COOLDOWN_SECONDS=120;
    static constexpr double MIN_PRICE_MOVE=8.0;
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::seconds(COOLDOWN_SECONDS)};
    int trend_entry_count_=0,last_trend_dir_=0;
    double last_entry_price_=0;
    enum class State{IDLE,WAITING_PULLBACK} state_=State::IDLE;
    int direction_=0; double impulse_high_=0,impulse_low_=0;
public:
    ImpulseContinuationEngine(): EngineBase("ImpulseContinuation",1.1){}
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
        if(s.prev_mid>0){
            double mom=std::fabs(s.mid-s.prev_mid);
            if(mom<MIN_MOMENTUM) return noSignal();
            if(mom>MAX_MOMENTUM&&std::fabs(s.mid-s.vwap)>PARABOLIC_VWAP) return noSignal();
        }
        if(s.vwap>0){
            double vd=std::fabs(s.mid-s.vwap);
            if(vd<MIN_VWAP_DIST||vd>MAX_VWAP_DIST) return noSignal();
        }
        // ASIAN session removed from whitelist (was previously included).
        // ImpulseContinuation needs sustained directional flow with clear impulse
        // + pullback structure. Asian gold (00:00-07:00 UTC) is choppy mean-reverting
        // tape — no trending structure exists to continue. Incident Mar 17 2026:
        // fired SHORT in MEAN_REVERSION at 00:47 UTC → $205 loss.
        if(s.session!=SessionType::LONDON&&s.session!=SessionType::NEWYORK&&
           s.session!=SessionType::OVERLAP)
            return noSignal();
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
        double pb=(direction_==1)?(impulse_high_-s.mid):(s.mid-impulse_low_);
        if(pb>PULLBACK_MAX){state_=State::IDLE;direction_=0;return noSignal();}
        if(pb>=PULLBACK_MIN&&pb<=PULLBACK_MAX){
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
            sig.size=0.02; sig.entry=s.mid; sig.tp=TP_TICKS; sig.sl=SL_TICKS;
            strncpy(sig.reason,dir==1?"IMPULSE_CONT_LONG":"IMPULSE_CONT_SHORT",31);
            strncpy(sig.engine,"ImpulseContinuation",31);
            return sig;
        }
        return noSignal();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 3. SessionMomentumEngine
// ─────────────────────────────────────────────────────────────────────────────
class SessionMomentumEngine : public EngineBase {
    MinMaxCircularBuffer<double,64> history_;
    static constexpr size_t WINDOW=60;
    static constexpr double IMPULSE_MIN=1.60,MAX_SPREAD=3.50; // 1.20→1.60: SL trades had MFE 0.04-0.58, winners $2.27-$2.96. Stronger threshold = fewer but better signals
    static constexpr int TP_TICKS=30,SL_TICKS=15; // TP 30 ticks = $3.00, matching observed winner MFE of $2.27-$2.96
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::milliseconds(1000)};

    static bool in_session_window(){
        auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm u{}; 
#ifdef _WIN32
        gmtime_s(&u,&t);
#else
        gmtime_r(&t,&u);
#endif
        int m=u.tm_hour*60+u.tm_min;
        return (m>=420&&m<=630)||(m>=750&&m<=930); // 07:00-10:30 and 12:30-15:30
    }
public:
    SessionMomentumEngine(): EngineBase("SessionMomentum",1.2){}
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(!in_session_window()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
        auto now=std::chrono::steady_clock::now();
        if(now-last_signal_<std::chrono::milliseconds(1000)) return noSignal();
        history_.push_back(s.mid);
        if(history_.size()<WINDOW) return noSignal();
        double hi=history_.max(),lo=history_.min(),impulse=hi-lo;
        if(impulse<IMPULSE_MIN) return noSignal();
        if(s.vwap<=0) return noSignal();
        double conf=std::min(1.5,impulse/(IMPULSE_MIN*2.0));
        double dhi=hi-s.mid,dlo=s.mid-lo;
        Signal sig; sig.size=0.02; sig.entry=s.mid; sig.tp=TP_TICKS; sig.sl=SL_TICKS;
        if(dhi<dlo&&s.mid>s.vwap){
            sig.valid=true; sig.side=TradeSide::LONG; sig.confidence=conf;
            strncpy(sig.reason,"SESSION_MOM_LONG",31); strncpy(sig.engine,"SessionMomentum",31);
            last_signal_=now; signal_count_++; history_.clear(); return sig;
        }
        if(dlo<dhi&&s.mid<s.vwap){
            sig.valid=true; sig.side=TradeSide::SHORT; sig.confidence=conf;
            strncpy(sig.reason,"SESSION_MOM_SHORT",31); strncpy(sig.engine,"SessionMomentum",31);
            last_signal_=now; signal_count_++; history_.clear(); return sig;
        }
        return noSignal();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 4. VWAPSnapbackEngine
// ─────────────────────────────────────────────────────────────────────────────
class VWAPSnapbackEngine : public EngineBase {
    static constexpr double VWAP_DEV_ENTRY=3.5,VWAP_DEV_STRONG=5.5,MOMENTUM_SPIKE=2.5,MAX_SPREAD=4.00;
    static constexpr int TP_TICKS=12,SL_TICKS=8;
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::milliseconds(500)};
public:
    VWAPSnapbackEngine(): EngineBase("VWAP_SNAPBACK",1.4){ enabled_=true; } // Re-enabled: 1T sample too small for judgment — needs 20+ trades to evaluate
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
        if(s.volatility<0.001) return noSignal();
        if(s.session!=SessionType::LONDON&&s.session!=SessionType::NEWYORK&&s.session!=SessionType::OVERLAP)
            return noSignal();
        auto now=std::chrono::steady_clock::now();
        if(now-last_signal_<std::chrono::milliseconds(500)) return noSignal();
        double dev=s.mid-s.vwap;
        double z=(s.volatility>0)?(dev/s.volatility):0;
        if(std::fabs(z)<VWAP_DEV_ENTRY||std::fabs(z)>VWAP_DEV_STRONG) return noSignal();
        if(std::fabs(s.sweep_size)<MOMENTUM_SPIKE) return noSignal();
        TradeSide side;
        if(z<-VWAP_DEV_ENTRY&&s.sweep_size>0)      side=TradeSide::LONG;
        else if(z>VWAP_DEV_ENTRY&&s.sweep_size<0)  side=TradeSide::SHORT;
        else return noSignal();
        Signal sig; sig.valid=true; sig.side=side;
        sig.confidence=std::min(1.5,std::fabs(z)/(VWAP_DEV_ENTRY*2.0));
        sig.size=0.05; sig.entry=s.mid; sig.tp=TP_TICKS; sig.sl=SL_TICKS;
        strncpy(sig.reason,"VWAP_DEV",31); strncpy(sig.engine,"VWAP_SNAPBACK",31);
        last_signal_=now; signal_count_++;
        return sig;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 5. LiquiditySweepProEngine
// ─────────────────────────────────────────────────────────────────────────────
class LiquiditySweepProEngine : public EngineBase {
    CircularBuffer<double,256> history_;
    static constexpr double MAX_SPREAD=4.00,SWEEP_TRIGGER=0.80,MOMENTUM_SPIKE=0.70; // spike 0.60→0.70: require stronger momentum to filter weak sweeps
    static constexpr double EXHAUSTION_RATIO=0.60,MIN_VWAP_DISTANCE=2.00,MIN_EXPECTED_MOVE=1.00; // VWAP dist 1.50→2.00: need real displacement; expected move 0.80→1.00
    static constexpr double TP_RATIO=0.85,BASE_SIZE=0.02;
    static constexpr int SL_TICKS=12,MOM_WINDOW=6,LIQ_WINDOW=120; // SL 10→12: losers avg MAE=-0.66, not -1.00 — SL was too far, wasting $0.34
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
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid())return noSignal();
        history_.push_back(s.mid);
        if(s.spread>MAX_SPREAD)return noSignal();
        if(s.session!=SessionType::LONDON&&s.session!=SessionType::NEWYORK&&s.session!=SessionType::OVERLAP)
            return noSignal();
        double liq=detectLiqPool();
        if(liq==0)return noSignal();
        if(std::fabs(s.mid-liq)<SWEEP_TRIGGER)return noSignal();
        if(computeMom()<MOMENTUM_SPIKE)return noSignal();
        if(!momExhausting())return noSignal();
        double dv=std::fabs(s.mid-s.vwap);
        if(dv<MIN_VWAP_DISTANCE||dv<MIN_EXPECTED_MOVE)return noSignal();
        TradeSide side=(s.mid>s.vwap)?TradeSide::SHORT:TradeSide::LONG;
        int tp=std::max(8,std::min(30,(int)((dv*TP_RATIO)/0.1))); // cap 16→30 ticks ($1.60→$3.00): winners avg MFE $1.77 = hitting old cap, not natural target
        Signal sig; sig.valid=true; sig.side=side; sig.confidence=0.95;
        sig.size=BASE_SIZE; sig.entry=s.mid; sig.tp=tp; sig.sl=SL_TICKS;
        strncpy(sig.reason,side==TradeSide::SHORT?"SWEEP_SHORT":"SWEEP_LONG",31);
        strncpy(sig.engine,"LiquiditySweepPro",31);
        signal_count_++; return sig;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 6. LiquiditySweepPressureEngine
// ─────────────────────────────────────────────────────────────────────────────
class LiquiditySweepPressureEngine : public EngineBase {
    CircularBuffer<double,512> history_;
    static constexpr double MAX_SPREAD=4.00,SWEEP_TRIGGER=0.80,MOMENTUM_SPIKE=0.60;
    static constexpr double EXHAUSTION_RATIO=0.60,MIN_VWAP_DISTANCE=1.50,MIN_EXPECTED_MOVE=0.80;
    static constexpr double TP_RATIO=0.85,BASE_SIZE=0.02,PRESSURE_THRESHOLD=0.15;
    static constexpr double CLUSTER_RANGE=0.35,PRESSURE_RANGE=0.45;
    static constexpr int SL_TICKS=10,MOM_WINDOW=6,LIQ_WINDOW=150,PRESSURE_WINDOW=50;
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
    LiquiditySweepPressureEngine(): EngineBase("LiquiditySweepPressure",1.15){ enabled_=false; } // DISABLED: 51T 29%WR -$12.10 — fires too early into sweep, structural loser
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid())return noSignal();
        history_.push_back(s.mid);
        if(s.spread>MAX_SPREAD)return noSignal();
        if(s.session!=SessionType::LONDON&&s.session!=SessionType::NEWYORK&&s.session!=SessionType::OVERLAP)
            return noSignal();
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
        sig.size=BASE_SIZE; sig.entry=s.mid; sig.tp=tp; sig.sl=SL_TICKS;
        strncpy(sig.reason,side==TradeSide::SHORT?"PRESSURE_SWEEP_SHORT":"PRESSURE_SWEEP_LONG",31);
        strncpy(sig.engine,"LiquiditySweepPressure",31);
        signal_count_++; return sig;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RegimeGovernor (ported from ChimeraMetals — exact same logic)
// ─────────────────────────────────────────────────────────────────────────────
enum class MarketRegime { COMPRESSION, TREND, MEAN_REVERSION, IMPULSE };

class RegimeGovernor {
    MinMaxCircularBuffer<double,64> history_;
    MarketRegime current_=MarketRegime::MEAN_REVERSION;
    MarketRegime candidate_=MarketRegime::MEAN_REVERSION;
    int confirm_count_=0;
    std::chrono::steady_clock::time_point last_switch_=std::chrono::steady_clock::now();
    static constexpr int CONFIRM_TICKS=5,MIN_LOCK_MS=1000;
    static constexpr size_t WINDOW=60;
    static constexpr double CE=0.70,CX=1.00,IE=1.20,IX=0.90,TE=2.20,TX=1.60;

    MarketRegime classifyRaw(double range,double mid,double hi,double lo) const {
        double centre=(hi+lo)*0.5;
        bool at_extreme=std::fabs(mid-centre)>=0.4;
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
public:
    MarketRegime detect(double mid, bool has_open_pos) {
        if(has_open_pos) return current_;
        history_.push_back(mid);
        if(history_.size()<WINDOW) return current_;
        double hi=history_.max(),lo=history_.min(),range=hi-lo;
        MarketRegime raw=classifyRaw(range,mid,hi,lo);
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
                    en=(n=="CompressionBreakout"); break;
                case MarketRegime::TREND:
                    en=(n=="ImpulseContinuation"); break;
                case MarketRegime::MEAN_REVERSION:
                    en=(n=="VWAP_SNAPBACK"||n=="LiquiditySweepPro"||n=="LiquiditySweepPressure"); break;
                case MarketRegime::IMPULSE:
                    en=(n=="ImpulseContinuation"||n=="SessionMomentum"||n=="LiquiditySweepPro"||n=="LiquiditySweepPressure"); break;
            }
            e->setEnabled(en);
        }
    }
    MarketRegime current() const { return current_; }
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

// ─────────────────────────────────────────────────────────────────────────────
// VolatilityFilter
// ─────────────────────────────────────────────────────────────────────────────
class VolatilityFilter {
    MinMaxCircularBuffer<double,64> history_;
    static constexpr size_t WINDOW=50;
    static constexpr double VOL_THRESHOLD=0.8;  // 1.4→0.8: $1.40 was blocking all Asian session signals; $0.80 = realistic quiet-period floor
public:
    bool allow(double mid){
        history_.push_back(mid);
        if(history_.size()<WINDOW)return false;
        return history_.range()>=VOL_THRESHOLD;
    }
    double current_range() const { return history_.empty()?0:history_.range(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// GoldEngineStack — the public interface wired into Omega's on_tick
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// GoldPositionManager — tracks open position, runs TP/SL/timeout each tick,
// calls on_close with a filled TradeRecord when trade exits.
// ─────────────────────────────────────────────────────────────────────────────
class GoldPositionManager {
    static constexpr double TICK_SIZE     = 0.10;  // GOLD.F minimum price increment
    static constexpr int    MAX_HOLD_SEC  = 600;   // 10 min: matches main config max_hold_sec
                                                    // was 120 — too short for $5 TP targets
    static constexpr double CONTRACT_SIZE = 1.0;   // notional per trade unit
    static constexpr int    MAX_PYRAMID_LEGS = 3;  // base + 2 add-ons
    static constexpr double PYR_COVER_MOVE   = 0.80; // add only after prior leg has clearly covered costs
    static constexpr double PYR_MIN_STEP     = 0.70; // avoid stacking at nearly same level in chop
    static constexpr int64_t PYR_ADD_COOLDOWN_SEC = 4;
    static constexpr int    PYR_TP_TICKS     = 18;
    static constexpr int    PYR_SL_TICKS     = 7;
    static constexpr double LOCK_ARM_MOVE    = 0.90;
    static constexpr double LOCK_GAIN        = 0.20;
    static constexpr double TRAIL_ARM_1      = 1.40;
    static constexpr double TRAIL_DIST_1     = 0.50;
    static constexpr double TRAIL_ARM_2      = 2.20;
    static constexpr double TRAIL_DIST_2     = 0.35;
    static constexpr double MIN_LOCKED_PROFIT = 0.05;
    static constexpr double MAX_BASE_SL_TICKS = 12.0; // cap hard loss to about $1.20 on base entries

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
        omega::TradeRecord tr;
        tr.id          = trade_id_++;
        tr.symbol      = "GOLD.F";
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

    void apply_tight_trail(GoldPos& leg, double mid) {
        const double move = leg.is_long ? (mid - leg.entry) : (leg.entry - mid);
        if (move >= LOCK_ARM_MOVE) {
            if (leg.is_long) {
                const double be_lock = leg.entry + LOCK_GAIN;
                if (be_lock > leg.sl) leg.sl = be_lock;
            } else {
                const double be_lock = leg.entry - LOCK_GAIN;
                if (be_lock < leg.sl) leg.sl = be_lock;
            }
        }
        if (move >= TRAIL_ARM_1) {
            if (leg.is_long) {
                const double trail = mid - TRAIL_DIST_1;
                if (trail > leg.sl) leg.sl = trail;
            } else {
                const double trail = mid + TRAIL_DIST_1;
                if (trail < leg.sl) leg.sl = trail;
            }
        }
        if (move >= TRAIL_ARM_2) {
            if (leg.is_long) {
                const double trail = mid - TRAIL_DIST_2;
                if (trail > leg.sl) leg.sl = trail;
            } else {
                const double trail = mid + TRAIL_DIST_2;
                if (trail < leg.sl) leg.sl = trail;
            }
        }
    }

    static bool regime_allows_pyramid(const char* regime) {
        if (!regime) return false;
        return std::strcmp(regime, "TREND") == 0 || std::strcmp(regime, "IMPULSE") == 0;
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
        if (leader_move < PYR_COVER_MOVE) return false;
        for (const auto& leg : legs_) {
            if (!leg_profit_locked(leg)) return false;
        }

        const GoldPos& last = legs_.back();
        const double move = last.is_long ? (mid - last.entry) : (last.entry - mid);
        if (move < PYR_COVER_MOVE) return false;
        if (std::fabs(mid - last_add_price_) < PYR_MIN_STEP) return false;
        return true;
    }

    void add_pyramid_leg(double mid, double spread, double latency_ms, const char* regime) {
        if (legs_.empty() || static_cast<int>(legs_.size()) >= MAX_PYRAMID_LEGS) return;
        const bool is_long = legs_.front().is_long;
        GoldPos leg;
        leg.active   = true;
        leg.is_long  = is_long;
        leg.entry    = mid;
        leg.tp       = is_long ? mid + PYR_TP_TICKS * TICK_SIZE
                               : mid - PYR_TP_TICKS * TICK_SIZE;
        leg.sl       = is_long ? mid - PYR_SL_TICKS * TICK_SIZE
                               : mid + PYR_SL_TICKS * TICK_SIZE;
        leg.mfe      = 0;
        leg.mae      = 0;
        leg.size     = CONTRACT_SIZE;
        leg.spread_at_entry = spread;
        leg.entry_ts = nowSec();
        strncpy(leg.engine, "PYRAMID", 31);
        strncpy(leg.reason, "PYR_ADD", 31);
        strncpy(leg.regime, regime ? regime : "", 31);
        legs_.push_back(leg);
        last_add_price_ = mid;
        last_add_ts_ = leg.entry_ts;
        printf("[GOLD-PYRAMID-ADD] %s lvl=%zu entry=%.2f tp=%.2f sl=%.2f\n",
               is_long ? "LONG" : "SHORT",
               legs_.size(), leg.entry, leg.tp, leg.sl);
        fflush(stdout);
        (void)latency_ms;
    }

public:
    bool active() const { return !legs_.empty(); }
    size_t leg_count() const { return legs_.size(); }

    // Open a new position from a GoldSignal.
    // tp_ticks / sl_ticks are in $0.10 increments (standard gold tick).
    void open(const GoldSignal& sig, double spread,
              double latency_ms, const char* regime) {
        if (!legs_.empty()) return;  // signal-based base entry only when flat
        const double sl_ticks = std::max(4.0, std::min(MAX_BASE_SL_TICKS, sig.sl_ticks));
        GoldPos leg;
        leg.active   = true;
        leg.is_long  = sig.is_long;
        leg.entry    = sig.entry;
        leg.tp       = sig.is_long
                        ? sig.entry + sig.tp_ticks * TICK_SIZE
                        : sig.entry - sig.tp_ticks * TICK_SIZE;
        leg.sl       = sig.is_long
                        ? sig.entry - sl_ticks * TICK_SIZE
                        : sig.entry + sl_ticks * TICK_SIZE;
        leg.mfe      = 0;
        leg.mae      = 0;
        leg.size     = CONTRACT_SIZE;
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
                std::function<void(const omega::TradeRecord&)>& on_close) {
        if (legs_.empty()) return false;
        double mid = (bid + ask) * 0.5;
        bool closed_any = false;
        const int64_t now = nowSec();

        for (int i = static_cast<int>(legs_.size()) - 1; i >= 0; --i) {
            GoldPos& leg = legs_[static_cast<size_t>(i)];
            const double move = leg.is_long ? (mid - leg.entry) : (leg.entry - mid);
            if (move > leg.mfe) leg.mfe = move;
            if (move < leg.mae) leg.mae = move;

            // Regime change exit — only close if position has been open >= 60s.
            // Short positions opened during IMPULSE are valid even as regime
            // transitions to MEAN_REVERSION — gold doesn't stop moving because
            // the regime label changed. Instant REGIME_FLIP exits were causing
            // churn: enter → regime flips 10s later → exit → repeat.
            const int64_t held_so_far = now - leg.entry_ts;
            if (regime && leg.regime[0] != '\0' &&
                std::strncmp(regime, leg.regime, 31) != 0 &&
                held_so_far >= 60) {
                close_leg(static_cast<size_t>(i), mid, "REGIME_FLIP", latency_ms, regime, on_close);
                closed_any = true;
                continue;
            }

            apply_tight_trail(leg, mid);

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
                // Cap timeout exit at SL if price has blown through — prevents a
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

// ─────────────────────────────────────────────────────────────────────────────
class GoldEngineStack {
public:
    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    GoldEngineStack() {
        engines_.push_back(std::make_unique<CompressionBreakoutEngine>());
        engines_.push_back(std::make_unique<ImpulseContinuationEngine>());
        engines_.push_back(std::make_unique<SessionMomentumEngine>());
        engines_.push_back(std::make_unique<VWAPSnapbackEngine>());
        engines_.push_back(std::make_unique<LiquiditySweepProEngine>());
        engines_.push_back(std::make_unique<LiquiditySweepPressureEngine>());
    }

    // Call every tick.
    // on_close is called when a position closes (TP/SL/timeout/force).
    // can_enter=false → manage existing position only, no new entries.
    // Returns valid GoldSignal if a NEW entry was just opened this tick.
    GoldSignal on_tick(double bid, double ask, double latency_ms,
                       CloseCallback on_close = nullptr, bool can_enter = true) {
        if(bid<=0||ask<=0||bid>=ask) return GoldSignal{};
        double spread = ask - bid;
        const int64_t now_s = static_cast<int64_t>(std::time(nullptr));

        // ── Manage existing position (TP/SL/timeout) ─────────────────────────
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
                                           current_regime_name(), wrapped_close);
        (void)just_closed;

        // Update has_open_pos_ so regime governor freezes while in trade
        has_open_pos_ = pos_mgr_.active();

        // Build snapshot
        GoldSnapshot snap;
        snap.bid=bid; snap.ask=ask;
        snap.prev_mid=(last_mid_>0)?last_mid_:((bid+ask)*0.5);
        features_.update(snap,bid,ask);
        last_mid_=snap.mid;

        // Regime classification (frozen while position open)
        MarketRegime regime=governor_.detect(snap.mid,has_open_pos_);
        governor_.apply(engines_,regime);
        current_regime_=regime;
        apply_asian_session_overrides(snap.session);

        // Don't look for new entries if already in a position or gated out
        if(has_open_pos_ || !can_enter) return GoldSignal{};

        // Hard-loss cooldown to avoid immediate revenge trading after failed break.
        if(now_s < sl_cooldown_until_) return GoldSignal{};

        // Minimum entry gap — prevents re-entering immediately after every TP/SL.
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
                    pos_mgr_.open(gs, spread, latency_ms, current_regime_name());
                    has_open_pos_=true;
                    last_entry_ts_=now_s;
                    return gs;
                }
                break;
            }
        }

        // Slow path: best confidence×weight
        Signal best; double best_score=0;
        for(auto& e:engines_){
            if(e->getName()=="ImpulseContinuation"||!e->isEnabled()) continue;
            Signal s=e->process(snap);
            if(!s.valid) continue;
            double score=s.confidence*e->weight_;
            if(score>best_score){ best_score=score; best=s; }
        }
        if(best.valid){
            if (!entry_quality_ok(best, best_score, snap, now_s)) return GoldSignal{};
            GoldSignal gs=to_gold_signal(best);
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

    // Legacy: kept for compatibility — managed internally now
    void set_has_open_position(bool v) { (void)v; }

    bool has_open_position() const { return pos_mgr_.active(); }
    const char* regime_name() const { return RegimeGovernor::name(current_regime_); }
    double vwap()      const { return features_.get_vwap(); }
    double vol_range() const { return vol_filter_.current_range(); }

    void print_stats() const {
        for(const auto& e:engines_)
            printf("[GOLD-ENGINE] %-26s signals=%llu\n",
                   e->getName().c_str(),(unsigned long long)e->signal_count_);
        fflush(stdout);
    }

private:
    static constexpr int64_t HARD_SL_GLOBAL_COOLDOWN_SEC = 60;
    static constexpr int64_t SIDE_CHOP_WINDOW_SEC = 90;
    static constexpr int64_t SIDE_CHOP_PAUSE_SEC = 60;
    static constexpr size_t  SIDE_CHOP_TRIGGER_COUNT = 2;
    static constexpr int64_t SAME_LEVEL_REENTRY_SEC = 30;
    static constexpr double  SAME_LEVEL_REENTRY_BAND = 0.80;
    static constexpr double  MIN_VWAP_DISLOCATION = 0.80;
    static constexpr double  MAX_ENTRY_SPREAD = 1.60;
    static constexpr double  IMPULSE_MIN_CONFIDENCE = 1.05;
    static constexpr double  IMPULSE_MIN_SCORE = 1.20;
    static constexpr double  GENERAL_MIN_SCORE = 1.20;
    // Minimum gap between any new entry — prevents re-entering immediately after TP/SL.
    // Was causing 30+ gold trades per hour as CompressionBreakout re-fired every tick.
    static constexpr int64_t MIN_ENTRY_GAP_SEC = 30;

    std::vector<std::unique_ptr<EngineBase>> engines_;
    GoldFeatures     features_;
    RegimeGovernor   governor_;
    VolatilityFilter vol_filter_;
    GoldPositionManager pos_mgr_;
    MarketRegime     current_regime_=MarketRegime::MEAN_REVERSION;
    bool             has_open_pos_=false;
    double           last_mid_=0;
    int64_t          sl_cooldown_until_=0;
    int64_t          last_entry_ts_=0;     // timestamp of last entry — enforces MIN_ENTRY_GAP_SEC
    std::array<int64_t, 2> side_pause_until_{{0,0}};
    std::array<std::deque<int64_t>, 2> side_hard_sl_times_{};
    std::array<double, 2> last_exit_price_{{0.0,0.0}};
    std::array<int64_t, 2> last_exit_ts_{{0,0}};

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
        if (s.engine[0] != '\0' && std::strcmp(s.engine, "ImpulseContinuation") == 0) {
            if (s.confidence < IMPULSE_MIN_CONFIDENCE || score < IMPULSE_MIN_SCORE) return false;
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

        if (tr.exitReason != "SL_HIT") return;

        // Only treat non-positive SL exits as hard stop-outs.
        if (tr.pnl > 0.0) return;

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
        // Asian session: only CompressionBreakout is allowed.
        // ImpulseContinuation REMOVED from this override (was previously included).
        //
        // WHY: ImpulseContinuation requires an established directional trend with
        // clear impulse + pullback structure. During Asian hours (00:00-07:00 UTC)
        // gold moves are thin, choppy, and lack the sustained directional flow the
        // engine needs. Allowing it bypassed the RegimeGovernor's correct decision
        // to disable it in MEAN_REVERSION — which is the dominant Asian regime.
        //
        // INCIDENT (Mar 17 2026 00:47 UTC): ImpulseContinuation fired SHORT in
        // MEAN_REVERSION regime. Gold was $4.26 below VWAP=5015, already
        // mean-reverting upward. Result: $205 loss (2.05 pts × $100/pt).
        //
        // CompressionBreakout is safe in Asia — it requires only a compression
        // range break, which is a self-contained price structure signal that works
        // in any session. The dead-zone gate (21:00-23:00) blocks the worst hours.
        for (auto& e : engines_) {
            if (e->getName() == "CompressionBreakout") {
                e->setEnabled(true);
            }
        }
    }

    const char* current_regime_name() const {
        return RegimeGovernor::name(current_regime_);
    }

    static GoldSignal to_gold_signal(const Signal& s){
        GoldSignal g;
        g.valid=true; g.is_long=(s.side==TradeSide::LONG);
        g.entry=s.entry; g.tp_ticks=s.tp; g.sl_ticks=s.sl;
        g.confidence=s.confidence;
        g.size = s.size > 0.0 ? s.size : 1.0;  // carry sub-engine sizing through
        strncpy(g.engine,s.engine,31); strncpy(g.reason,s.reason,31);
        return g;
    }
};

} // namespace gold
} // namespace omega
