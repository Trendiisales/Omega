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
class CompressionBreakoutEngine : public EngineBase {
    MinMaxCircularBuffer<double,32> history_;
    static constexpr size_t WINDOW=30;
    static constexpr double COMPRESSION_RANGE=2.00, BREAKOUT_TRIGGER=0.25, MAX_SPREAD=1.80;
    static constexpr int TP_TICKS=50, SL_TICKS=18; // TP 40→50: genuine compression breakouts on gold run $4-6, not $4
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::seconds(2)};
public:
    CompressionBreakoutEngine(): EngineBase("CompressionBreakout",1.0){}
    Signal process(const GoldSnapshot& s) override {
        if(!enabled_||!s.is_valid()) return noSignal();
        if(s.spread>MAX_SPREAD) return noSignal();
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
    static constexpr double MIN_MOMENTUM=0.40,MIN_MOVE_5T=0.40,MAX_MOMENTUM=5.0,PARABOLIC_VWAP=10.0;
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
        if(s.session!=SessionType::LONDON&&s.session!=SessionType::NEWYORK&&
           s.session!=SessionType::OVERLAP&&s.session!=SessionType::ASIAN)
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
    VWAPSnapbackEngine(): EngineBase("VWAP_SNAPBACK",1.4){ enabled_=false; } // DISABLED: 1T 0%WR -$0.80 — insufficient data, re-enable after 20+ shadow trades
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
    static constexpr int    MAX_HOLD_SEC  = 600;   // 10min max hold: winners close 22-157s, 20min was wasting capital on stale trades
    static constexpr double CONTRACT_SIZE = 1.0;   // notional per trade unit

    struct GoldPos {
        bool    active    = false;
        bool    is_long   = true;
        double  entry     = 0;
        double  tp        = 0;   // absolute price level
        double  sl        = 0;   // absolute price level
        double  mfe       = 0;
        double  mae       = 0;
        double  spread_at_entry = 0;
        int64_t entry_ts  = 0;
        char    engine[32] = {};
        char    reason[32] = {};
    } pos_;

    int trade_id_ = 1000;  // separate ID range from CRTP engines (those start at 1)

    static int64_t nowSec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void do_close(double exit_px, const char* why,
                  double latency_ms, const char* regime,
                  std::function<void(const omega::TradeRecord&)>& on_close) {
        if (!pos_.active) return;
        omega::TradeRecord tr;
        tr.id          = trade_id_++;
        tr.symbol      = "GOLD.F";
        tr.side        = pos_.is_long ? "LONG" : "SHORT";
        tr.entryPrice  = pos_.entry;
        tr.exitPrice   = exit_px;
        tr.tp          = pos_.tp;
        tr.sl          = pos_.sl;
        tr.size        = CONTRACT_SIZE;
        tr.pnl         = (pos_.is_long ? (exit_px - pos_.entry)
                                       : (pos_.entry - exit_px)) * CONTRACT_SIZE;
        tr.mfe         = pos_.mfe;
        tr.mae         = pos_.mae;
        tr.entryTs     = pos_.entry_ts;
        tr.exitTs      = nowSec();
        tr.exitReason  = why;
        tr.spreadAtEntry = pos_.spread_at_entry;
        tr.latencyMs   = latency_ms;
        tr.engine      = std::string(pos_.engine);
        tr.regime      = regime ? regime : "";
        pos_.active    = false;
        pos_           = GoldPos{};
        if (on_close) on_close(tr);
    }

public:
    bool active() const { return pos_.active; }

    // Open a new position from a GoldSignal.
    // tp_ticks / sl_ticks are in $0.10 increments (standard gold tick).
    void open(const GoldSignal& sig, double spread,
              double latency_ms, const char* regime) {
        if (pos_.active) return;  // never double-enter
        pos_.active   = true;
        pos_.is_long  = sig.is_long;
        pos_.entry    = sig.entry;
        pos_.tp       = sig.is_long
                        ? sig.entry + sig.tp_ticks * TICK_SIZE
                        : sig.entry - sig.tp_ticks * TICK_SIZE;
        pos_.sl       = sig.is_long
                        ? sig.entry - sig.sl_ticks * TICK_SIZE
                        : sig.entry + sig.sl_ticks * TICK_SIZE;
        pos_.mfe      = 0;
        pos_.mae      = 0;
        pos_.spread_at_entry = spread;
        pos_.entry_ts = nowSec();
        strncpy(pos_.engine, sig.engine, 31);
        strncpy(pos_.reason, sig.reason, 31);
        printf("[GOLD-STACK-ENTRY] %s entry=%.2f tp=%.2f sl=%.2f eng=%s reason=%s regime=%s\n",
               pos_.is_long?"LONG":"SHORT", pos_.entry, pos_.tp, pos_.sl,
               pos_.engine, pos_.reason, regime?regime:"?");
        fflush(stdout);
    }

    // Called every tick while position is open. Manages TP/SL/timeout.
    // Returns true if position was closed this tick.
    bool manage(double bid, double ask, double latency_ms, const char* regime,
                std::function<void(const omega::TradeRecord&)>& on_close) {
        if (!pos_.active) return false;
        double mid = (bid + ask) * 0.5;
        // MFE / MAE (in price units, not bp)
        double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > pos_.mfe) pos_.mfe = move;
        if (move < pos_.mae) pos_.mae = move;

        // Profit lock / trail:
        // 1) Once trade reaches +2.0, lock small gain (BE + 0.2).
        // 2) Once trade reaches +3.0, trail stop by 1.2 from current mid.
        if (move >= 2.0) {
            if (pos_.is_long) {
                const double be_lock = pos_.entry + 0.2;
                if (be_lock > pos_.sl) pos_.sl = be_lock;
            } else {
                const double be_lock = pos_.entry - 0.2;
                if (be_lock < pos_.sl) pos_.sl = be_lock;
            }
        }
        if (move >= 3.0) {
            if (pos_.is_long) {
                const double trail = mid - 1.2;
                if (trail > pos_.sl) pos_.sl = trail;
            } else {
                const double trail = mid + 1.2;
                if (trail < pos_.sl) pos_.sl = trail;
            }
        }
        // TP
        bool tp_hit = pos_.is_long ? (ask >= pos_.tp) : (bid <= pos_.tp);
        if (tp_hit) {
            double fill = pos_.is_long ? pos_.tp : pos_.tp;
            printf("[GOLD-STACK-TP] %s fill=%.2f pnl=%.2f\n",
                   pos_.engine, fill,
                   pos_.is_long?(fill-pos_.entry):(pos_.entry-fill));
            fflush(stdout);
            do_close(fill, "TP_HIT", latency_ms, regime, on_close);
            return true;
        }
        // SL
        bool sl_hit = pos_.is_long ? (bid <= pos_.sl) : (ask >= pos_.sl);
        if (sl_hit) {
            double fill = pos_.is_long ? pos_.sl : pos_.sl;
            printf("[GOLD-STACK-SL] %s fill=%.2f pnl=%.2f\n",
                   pos_.engine, fill,
                   pos_.is_long?(fill-pos_.entry):(pos_.entry-fill));
            fflush(stdout);
            do_close(fill, "SL_HIT", latency_ms, regime, on_close);
            return true;
        }
        // Timeout
        if (nowSec() - pos_.entry_ts >= MAX_HOLD_SEC) {
            printf("[GOLD-STACK-TIMEOUT] %s hold=%lds exit=%.2f\n",
                   pos_.engine, (long)(nowSec()-pos_.entry_ts), mid);
            fflush(stdout);
            do_close(mid, "TIMEOUT", latency_ms, regime, on_close);
            return true;
        }
        return false;
    }

    // Force-close on disconnect / session end
    void force_close(double bid, double ask, double latency_ms, const char* regime,
                     std::function<void(const omega::TradeRecord&)>& on_close) {
        if (!pos_.active) return;
        double mid = (bid + ask) * 0.5;
        do_close(mid, "FORCE_CLOSE", latency_ms, regime, on_close);
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

        // ── Manage existing position (TP/SL/timeout) ─────────────────────────
        // Wrap on_close to detect SL exit and arm the 120s inter-engine cooldown
        CloseCallback wrapped_close = on_close
            ? [this, &on_close](const omega::TradeRecord& tr) {
                on_close(tr);
                if (tr.exitReason == "SL_HIT") {
                    sl_cooldown_until_ = static_cast<int64_t>(std::time(nullptr)) + 60;
                    printf("[GOLD-SL-COOLDOWN] armed 60s — blocks all engines until cooldown expires\n");
                    fflush(stdout);
                }
              }
            : CloseCallback{};
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

        // SL cooldown: after any gold SL, block ALL engines for 120s
        // Prevents multiple engines firing sequentially on same fake breakout
        if(static_cast<int64_t>(std::time(nullptr)) < sl_cooldown_until_) return GoldSignal{};

        // Volatility gate
        if(!vol_filter_.allow(snap.mid)) return GoldSignal{};

        // VWAP chop zone gate
        if(snap.vwap>0&&std::fabs(snap.mid-snap.vwap)<0.5) return GoldSignal{};

        // Fast path: ImpulseContinuation first
        for(auto& e:engines_){
            if(e->getName()=="ImpulseContinuation"&&e->isEnabled()){
                Signal s=e->process(snap);
                if(s.valid){
                    GoldSignal gs=to_gold_signal(s);
                    pos_mgr_.open(gs, spread, latency_ms, current_regime_name());
                    has_open_pos_=true;
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
            GoldSignal gs=to_gold_signal(best);
            pos_mgr_.open(gs, spread, latency_ms, current_regime_name());
            has_open_pos_=true;
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
    std::vector<std::unique_ptr<EngineBase>> engines_;
    GoldFeatures     features_;
    RegimeGovernor   governor_;
    VolatilityFilter vol_filter_;
    GoldPositionManager pos_mgr_;
    MarketRegime     current_regime_=MarketRegime::MEAN_REVERSION;
    bool             has_open_pos_=false;
    double           last_mid_=0;
    int64_t          sl_cooldown_until_=0;  // block ALL engines for 120s after any SL

    void apply_asian_session_overrides(SessionType session) {
        if (session != SessionType::ASIAN) return;
        // Core mismatch fix:
        // Session gate allows Asia trading globally, but regime routing can disable
        // CompressionBreakout while other enabled engines remain London/NY-only.
        // Keep Asia-capable engines available so stack can actually emit entries.
        for (auto& e : engines_) {
            const auto& n = e->getName();
            if (n == "CompressionBreakout" || n == "ImpulseContinuation") {
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
        strncpy(g.engine,s.engine,31); strncpy(g.reason,s.reason,31);
        return g;
    }
};

} // namespace gold
} // namespace omega
