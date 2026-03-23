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
    MinMaxCircularBuffer<double,64> history_;  // raised 32→64: larger buffer required for 50-tick WINDOW
    static constexpr size_t WINDOW        = 50;            // raised 30→50: 30 ticks at London open = ~3-6s, too short for real compression; 50 ticks = ~10-15s
    static constexpr double COMPRESSION_RANGE = 6.00;      // lowered 8.00→6.00: $8 was too permissive; $6 requires genuinely tight pre-break range
    static constexpr double BREAKOUT_TRIGGER  = 1.50;      // lowered 3.00→1.50: $3.00 was 133% of typical $2.25 compression range, never reachable; $1.50 confirms real directional break without requiring an impulse move
    static constexpr double MAX_SPREAD        = 2.00;      // unchanged
    static constexpr int    TP_TICKS          = 80;        // $8.00 target — 2.67:1 R:R on $3.00 SL
    static constexpr int    SL_TICKS          = 30;        // raised 25→30: $3.00 stop matches new $3.00 trigger; avoids instant stop-out on breakout retest
    std::chrono::steady_clock::time_point last_signal_{std::chrono::steady_clock::now()-std::chrono::seconds(5)};

    // NY/Tokyo handoff dead zone: 21:00–23:00 UTC
    // Thin liquidity, erratic spreads, stale VWAP (resets at midnight).
    // Tokyo gold directional flow does not establish until ~23:00 UTC.
    // Without this gate: 4 SL hits in 16 min observed 22:35 UTC Mar 17 2026.
    //
    // Two dead zones blocked:
    //   21:00–23:00 UTC — NY/Tokyo handoff: thin, no directional flow yet
    //   05:00–07:00 UTC — late Asia/Sydney runoff: Tokyo volume exhausted,
    //                     London not yet open, spreads widen, moves fade
    //                     Evidence: GOLD SHORT timeout Mar 18 06:03 UTC
    static bool in_handoff_dead_zone() noexcept {
        const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        const int h = ti.tm_hour;
        return (h >= 21 && h < 23) ||  // NY/Tokyo handoff
               (h >= 5  && h < 7);     // late Asia runoff → London dead zone
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
        Signal sig; sig.entry=s.mid; sig.size=0.01;  // fallback min_lot — overridden by compute_size() in main
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
    static constexpr int TP_TICKS=40,SL_TICKS=16;
    // TP $4.00 (40 ticks), SL $1.60 (16 ticks) — 2.5:1 R:R
    // SL raised from 8 ticks ($0.80): was AT or BELOW typical spread noise ($0.30-$0.80).
    // A single wide tick could stop out a valid trade. $1.60 = 2x max spread = real signal floor.
    // TP raised from 16 ($1.60) to maintain 2.5:1 R:R and match observed impulse move sizes.
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
            sig.size=0.01; sig.entry=s.mid; sig.tp=TP_TICKS; sig.sl=SL_TICKS;  // fallback min_lot
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
    static constexpr double IMPULSE_MIN=3.50,MAX_SPREAD=2.50;
    static constexpr int TP_TICKS=60,SL_TICKS=25;
    // IMPULSE_MIN raised 1.60→3.50: $1.60 range over 60 ticks is normal London
    // micro-noise. $3.50 is the minimum for a genuine directional session open move.
    // Observed losing trade: 3s hold, $0.20 gross, $0.18 slip → $0.02 net.
    // That impulse was sub-$2.00 — pure noise, not a momentum signal.
    // MAX_SPREAD tightened 3.50→2.50: wide spread = price discovery, not momentum.
    // SL raised 20→25 ticks ($2.00→$2.50): more room vs. London open volatility.
    // TP raised 50→60 ticks ($5.00→$6.00): keeps R:R at 2.4:1 after spread cost.
    static constexpr double VWAP_DEV_MIN=1.50; // price must be >$1.50 from VWAP to confirm direction
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
        // 07:15-10:30 (was 07:00): first 15min of London open is high-noise.
        // Spread is widest, Asia/London gap absorbing, no institutional flow yet.
        // Confirmed: SHORT at 07:04 UTC hit SL immediately — Asia VWAP ≠ London signal.
        // 12:30-15:30 unchanged — well before NY open, lower risk window.
        return (m>=435&&m<=630)||(m>=750&&m<=930); // 07:15-10:30 and 12:30-15:30
    }
public:
    SessionMomentumEngine(): EngineBase("SessionMomentum",1.2){}
    void reset_history() { history_.clear(); }
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
        // Require meaningful VWAP displacement — price must be committed to a direction,
        // not oscillating around VWAP within the impulse range.
        if(std::fabs(s.mid-s.vwap)<VWAP_DEV_MIN) return noSignal();
        double conf=std::min(1.5,impulse/(IMPULSE_MIN*2.0));
        double dhi=hi-s.mid,dlo=s.mid-lo;

        // Recent momentum confirmation — the last 5 ticks must show continuation
        // in the signal direction. Without this, the engine enters at exhaustion:
        // price has already made its move and is reversing, but dhi/dlo still
        // points to the old extreme. Both observed bad trades (3s hold, SL hit
        // immediately) had this signature — entered at the top/bottom of a
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
        Signal sig; sig.size=0.01; sig.entry=s.mid; sig.tp=TP_TICKS; sig.sl=SL_TICKS;  // fallback min_lot
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

// ─────────────────────────────────────────────────────────────────────────────
// 4. VWAPSnapbackEngine
// ─────────────────────────────────────────────────────────────────────────────
class VWAPSnapbackEngine : public EngineBase {
    static constexpr double VWAP_DEV_ENTRY=3.5,VWAP_DEV_STRONG=5.5,MOMENTUM_SPIKE=2.5,MAX_SPREAD=4.00;
    static constexpr int TP_TICKS=35,SL_TICKS=15;
    // TP $3.50 (35 ticks), SL $1.50 (15 ticks) — 2.3:1 R:R
    // SL raised from 8 ($0.80): was at spread noise floor. A single ask/bid bounce
    // would stop out the trade before it could develop. $1.50 = 2x spread.
    // TP raised from 12 ($1.20): mean-reversion to VWAP from 3.5σ is typically
    // $2-$4 of reversion. $1.20 was capping winners well below their natural target.
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
        sig.size=0.01; sig.entry=s.mid; sig.tp=TP_TICKS; sig.sl=SL_TICKS;  // fallback min_lot
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
    // Runtime members — set via apply_cfg() from GoldStackCfg.
    // Defaults match the calibrated constexpr values used prior to config-driven refactor.
    double MAX_SPREAD      = 4.00;
    double BASE_SIZE       = 0.01;  // fallback min_lot — overridden by compute_size() in main
    int    SL_TICKS        = 18;    // $1.80 — above max spread noise floor for sweep entries
    // Fixed internal constants — not exposed to config (structural, not tunable)
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
        // Trend alignment gate: block sweep fades that go against an active trend.
        // A genuine exhausted sweep shows trend already rolling (trend < 0 for SHORT,
        // trend > 0 for LONG). Shorting into trend > 0.30 = fading a live London rally.
        if (side == TradeSide::SHORT && s.trend >  0.30) return noSignal();
        if (side == TradeSide::LONG  && s.trend < -0.30) return noSignal();
        int tp=std::max(18,std::min(40,(int)((dv*TP_RATIO)/0.1)));
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
    // Runtime members — set via apply_cfg() from GoldStackCfg.
    // Defaults match the calibrated constexpr values used prior to config-driven refactor.
    double MAX_SPREAD  = 4.00;
    int    SL_TICKS    = 10;    // $1.00 — tighter than SweepPro, pressure entries are earlier
    double BASE_SIZE   = 0.01;  // fallback min_lot — overridden by compute_size() in main
    // Fixed internal constants — not exposed to config (structural, not tunable)
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
    LiquiditySweepPressureEngine(): EngineBase("LiquiditySweepPressure",1.15){ enabled_=false; } // DISABLED: 51T 29%WR -$12.10 — fires too early into sweep, structural loser
    void apply_cfg(double max_spread, int sl_ticks, double base_size) {
        MAX_SPREAD = max_spread;
        SL_TICKS   = sl_ticks;
        BASE_SIZE  = base_size;
    }
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
    MinMaxCircularBuffer<double,128> history_;
    MarketRegime current_=MarketRegime::MEAN_REVERSION;
    MarketRegime candidate_=MarketRegime::MEAN_REVERSION;
    int confirm_count_=0;
    std::chrono::steady_clock::time_point last_switch_=std::chrono::steady_clock::now();
    static constexpr int CONFIRM_TICKS=5,MIN_LOCK_MS=1000;
    static constexpr size_t WINDOW=80;  // reduced 120→80: 120 ticks = 20-30s warmup before any regime; 80 ticks = ~12-18s, still meaningful structure

    // Thresholds recalibrated for $5000 gold (Mar 2026)
    static constexpr double CE=4.00;  // compression entry: raised 3.00→4.00: range was oscillating at 2.25–3.10 causing constant COMPRESSION↔MEAN_REVERSION flips at the 3.00 boundary; 4.00 gives stable COMPRESSION classification at current gold vol
    static constexpr double CX=4.50;  // compression exit:  range > $4.50
    static constexpr double IE=5.00;  // impulse entry:     lowered 6.00→5.00: $6 range unreachable at current $2.25–3.10 gold vol; $5 still confirms real impulse vs noise
    static constexpr double IX=4.50;  // impulse exit:      reduced 6.00→4.50: symmetric with new IE
    static constexpr double TE=15.00; // trend entry:       range > $15.00
    static constexpr double TX=12.00; // trend exit:        range < $12.00

    MarketRegime classifyRaw(double range,double mid,double hi,double lo) const {
        double centre=(hi+lo)*0.5;
        bool at_extreme=std::fabs(mid-centre)>=2.00;  // raised 0.40→2.00: $0.40 from centre is tick noise at $5000 gold; $2.00 = real directional pressure
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
public:
    MarketRegime detect(double mid, bool has_open_pos) {
        if(has_open_pos) return current_;
        history_.push_back(mid);
        if(history_.size()<WINDOW) return current_;
        hi_=history_.max(); lo_=history_.min(); range_=hi_-lo_;
        MarketRegime raw=classifyRaw(range_,mid,hi_,lo_);
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
    double window_range() const { return range_; }
    double window_hi()    const { return hi_; }
    double window_lo()    const { return lo_; }
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
    static constexpr double VOL_THRESHOLD=1.50;  // reduced 2.50→1.50: $2.50 was blocking normal mid-session activity; $1.50 still filters dead flat tape while allowing real setups
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
    static constexpr double CONTRACT_SIZE = 1.0;   // notional per trade unit
    static constexpr int    MAX_PYRAMID_LEGS = 3;  // base + 2 add-ons
    static constexpr double PYR_COVER_MOVE   = 0.80;
    static constexpr double PYR_MIN_STEP     = 0.70;
    static constexpr int64_t PYR_ADD_COOLDOWN_SEC = 4;
    static constexpr int    PYR_TP_TICKS     = 25;
    static constexpr int    PYR_SL_TICKS     = 12;

    // ── Runtime members — set via set_cfg() from GoldStackCfg ─────────────
    // Defaults match calibrated constexpr values prior to config-driven refactor.
    int    MAX_HOLD_SEC       = 600;   // 10 min: matches main config max_hold_sec
    double LOCK_ARM_MOVE      = 1.50;  // lock only after genuine $1.50 move
    double LOCK_GAIN          = 0.60;  // $0.60 lock above entry
    double TRAIL_ARM_1        = 2.50;  // trail after $2.50 move
    double TRAIL_DIST_1       = 0.80;  // trail $0.80 behind mid
    double TRAIL_ARM_2        = 5.00;  // tight-trail only on big $5.00 winners
    double TRAIL_DIST_2       = 0.50;  // tight trail distance
    double MIN_LOCKED_PROFIT  = 0.30;  // must lock meaningful profit above entry+spread
    double MAX_BASE_SL_TICKS  = 30.0;  // cap must be >= highest engine SL (CB uses 30)

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
        // Safety guard: exit_px should never be zero or negative for GOLD.F.
        // If it is, fall back to entry price (flat trade) and log the anomaly.
        if (exit_px <= 0.0) {
            printf("[GOLD-STACK-WARN] emit_close called with exit_px=%.4f why=%s entry=%.4f — clamping to entry\n",
                   exit_px, why ? why : "?", leg.entry);
            fflush(stdout);
            exit_px = leg.entry;
        }
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
        // Inherit size from the base leg so pyramid add-ons match the position scale.
        // Base entries use sig.size (e.g. 0.02 from sub-engines). Using CONTRACT_SIZE=1.0
        // here would make pyramid legs 50x larger than the base — a critical size mismatch.
        const double base_size = legs_.front().size;
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
        leg.size     = base_size;  // match base leg size — not CONTRACT_SIZE=1.0
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

    // Patch the base leg size after compute_size() runs in main.cpp.
    // on_tick() opens the position with the sub-engine default size (e.g. 0.01).
    // main.cpp then computes the correct risk-adjusted lot and calls this to
    // update the base leg so PnL, slippage, and the ledger all use the right size.
    // Only patches index 0 (base leg) — pyramid legs inherit base size on add.
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
        // Fall back to a minimal TP (2× SL) so the position isn't immediately closed flat.
        const double tp_ticks = (sig.tp_ticks > 0.0) ? sig.tp_ticks : sl_ticks * 2.0;
        if (sig.tp_ticks <= 0.0) {
            printf("[GOLD-STACK-WARN] open() called with tp_ticks=0 engine=%s — using fallback tp_ticks=%.0f\n",
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
        // CONTRACT_SIZE=1.0 was a placeholder — using it here made ledger PnL 50x
        // the real dollar value (0.02 lots × $100/pt became 1.0 lot × $100/pt).
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
                // Cap exit at SL if price has blown through — same logic as TIMEOUT.
                // Sparse ticks during reconnect can cause price to drift past SL
                // without triggering the explicit SL check above. REGIME_FLIP at
                // raw mid could then record a loss larger than the intended stop.
                const bool sl_breached = leg.is_long ? (mid < leg.sl) : (mid > leg.sl);
                const double regime_flip_exit = sl_breached ? leg.sl : mid;
                close_leg(static_cast<size_t>(i), regime_flip_exit, "REGIME_FLIP", latency_ms, regime, on_close);
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
// GoldStackCfg — all tunable parameters for GoldEngineStack in one struct.
// Populated from [gold_stack] ini section by main.cpp, then passed to
// GoldEngineStack::configure(). Default values match prior constexpr calibration
// so the system is behaviorally identical until the ini is explicitly changed.
// ─────────────────────────────────────────────────────────────────────────────
struct GoldStackCfg {
    // ── Orchestrator gates ──────────────────────────────────────────────────
    int64_t hard_sl_cooldown_sec        = 120;  // global SL cooldown after any stop hit
    int64_t side_chop_window_sec        = 300;  // rolling window for side-specific SL counting
    int64_t side_chop_pause_sec         = 300;  // pause duration when chop detected on a side
    int64_t same_level_reentry_sec      = 60;   // min seconds before re-entering same price band
    double  same_level_reentry_band     = 1.50; // $-band for same-level detection
    double  min_vwap_dislocation        = 1.20; // min $-distance from VWAP to enter
    double  max_entry_spread            = 1.60; // max spread at entry (absolute $)
    int64_t min_entry_gap_sec           = 90;   // min gap between any two entries
    // ── Position manager ────────────────────────────────────────────────────
    int     max_hold_sec                = 600;  // position timeout
    double  lock_arm_move               = 1.50; // move required before locking breakeven
    double  lock_gain                   = 0.60; // breakeven lock distance above entry
    double  trail_arm_1                 = 2.50; // move required to arm first trail
    double  trail_dist_1                = 0.80; // first trail distance behind mid
    double  trail_arm_2                 = 5.00; // move required to arm tight trail
    double  trail_dist_2                = 0.50; // tight trail distance behind mid
    double  min_locked_profit           = 0.30; // min profit that must be locked
    double  max_base_sl_ticks           = 30.0; // SL cap for base entries (ticks)
    // ── LiquiditySweepPro ───────────────────────────────────────────────────
    double  sweep_pro_max_spread        = 4.00;
    int     sweep_pro_sl_ticks          = 18;
    double  sweep_pro_base_size         = 0.01;
    // ── LiquiditySweepPressure ──────────────────────────────────────────────
    double  sweep_pres_max_spread       = 4.00;
    int     sweep_pres_sl_ticks         = 10;
    double  sweep_pres_base_size        = 0.01;
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

        // If a position closed this tick, stamp last_entry_ts_ to now so the
        // MIN_ENTRY_GAP_SEC check below cannot be bypassed by same-tick re-entry.
        // Previously last_entry_ts_ was only set when a new entry opened — a position
        // closing and immediately re-entering on the same tick had a stale timestamp
        // and fired through the gap check as if 90s had already elapsed.
        if (just_closed) last_entry_ts_ = now_s;

        // Update has_open_pos_ so regime governor freezes while in trade
        has_open_pos_ = pos_mgr_.active();

        // Build snapshot
        GoldSnapshot snap;
        snap.bid=bid; snap.ask=ask;
        snap.prev_mid=(last_mid_>0)?last_mid_:((bid+ask)*0.5);
        features_.update(snap,bid,ask);
        last_mid_=snap.mid;

        // Update long-window baseline for supervisor vol_ratio
        baseline_buf_.push_back(snap.mid);
        if (baseline_buf_.size() >= 40) {  // min warmup before using baseline
            const double bl_range = baseline_buf_.max() - baseline_buf_.min();
            baseline_vol_pct_ = (snap.mid > 0.0) ? (bl_range / snap.mid * 100.0) : 0.0;
        }

        // Regime classification (frozen while position open)
        MarketRegime regime=governor_.detect(snap.mid,has_open_pos_);
        governor_.apply(engines_,regime);
        current_regime_=regime;
        apply_asian_session_overrides(snap.session);
        apply_london_session_overrides(snap.session);

        // ── Session-change guard ─────────────────────────────────────────
        // When session transitions (ASIAN→LONDON, LONDON→NEWYORK etc),
        // reset SessionMomentumEngine's price history so it cannot fire on
        // stale cross-session data. The IMPULSE regime from Asia may persist
        // into London open enabling SessionMomentum, but its 60-tick window
        // would contain only Asia prices — not London directional context.
        // Confirmed cause: SHORT at 07:00:09 UTC fired on Asia IMPULSE regime
        // carrying into London, with 60-tick history entirely Asia data.
        if (snap.session != last_session_ && last_session_ != SessionType::UNKNOWN) {
            for (auto& e : engines_) {
                if (e->getName() == "SessionMomentum") {
                    // Cast to SessionMomentumEngine and clear its history
                    static_cast<SessionMomentumEngine*>(e.get())->reset_history();
                }
            }
            printf("[GOLD-SESSION-CHANGE] %s → %s: SessionMomentum history cleared\n",
                   session_name(last_session_), session_name(snap.session));
            fflush(stdout);
        }
        last_session_ = snap.session;

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
    double recent_vol_pct()      const { return (governor_.window_range() > 0.0 && last_mid_ > 0.0)
                                              ? (governor_.window_range() / last_mid_ * 100.0) : 0.0; }
    double base_vol_pct()        const { return baseline_vol_pct_; }

    void print_stats() const {
        for(const auto& e:engines_)
            printf("[GOLD-ENGINE] %-26s signals=%llu\n",
                   e->getName().c_str(),(unsigned long long)e->signal_count_);
        fflush(stdout);
    }

private:
    // ── Runtime members — set via configure() from GoldStackCfg ───────────
    // Defaults match calibrated constexpr values prior to config-driven refactor.
    int64_t HARD_SL_GLOBAL_COOLDOWN_SEC = 120; // raised 60→120: 60s wasn't stopping revenge entries after hard stops
    int64_t SIDE_CHOP_WINDOW_SEC        = 300; // raised 90→300: 90s window expired before detecting London chop pattern
    int64_t SIDE_CHOP_PAUSE_SEC         = 300; // raised 60→300: 60s pause was too short — chop resumed after pause
    size_t  SIDE_CHOP_TRIGGER_COUNT     = 2;   // keep at 2: still want early chop detection
    int64_t SAME_LEVEL_REENTRY_SEC      = 60;  // raised 30→60: 30s allowed near-instant re-entries at same level
    double  SAME_LEVEL_REENTRY_BAND     = 1.50;// raised 0.80→1.50: $0.80 band was too tight
    double  MIN_VWAP_DISLOCATION        = 1.20;// raised 0.80→1.20: entries within $1.20 of VWAP are noise territory
    double  MAX_ENTRY_SPREAD            = 1.60;
    double  IMPULSE_MIN_CONFIDENCE      = 1.05;
    double  IMPULSE_MIN_SCORE           = 1.20;
    double  GENERAL_MIN_SCORE           = 1.20;
    int64_t MIN_ENTRY_GAP_SEC           = 90;  // raised 30→90: CB was re-firing 2-3x per compression box

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
    // Session-change tracking — reset session-specific engine histories
    // when session transitions (e.g. ASIAN→LONDON) so engines don't fire
    // on stale cross-session data (e.g. Asia impulse carrying into London open)
    SessionType last_session_ = SessionType::UNKNOWN;
    std::array<double, 2> last_exit_price_{{0.0,0.0}};
    std::array<int64_t, 2> last_exit_ts_{{0,0}};
    // Long-window baseline vol tracker for supervisor — 400-tick rolling window
    // (~60-80s at typical gold tick rate). Provides a genuine measured baseline
    // so supervisor vol_ratio = recent_range / baseline_range is fully real.
    MinMaxCircularBuffer<double,512> baseline_buf_;
    double baseline_vol_pct_ = 0.0;  // cached: baseline_range / mid * 100

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
        } else if (s.engine[0] != '\0' && std::strcmp(s.engine, "CompressionBreakout") == 0) {
            // CompressionBreakout uses its own internal confidence calc:
            // confidence = min(1.5, breakout_distance / BREAKOUT_TRIGGER)
            // A minimal breakout (price exits by exactly 1x trigger) gives confidence=1.0
            // and score=1.0*weight(1.0)=1.0, which fails GENERAL_MIN_SCORE=1.20.
            // The engine's compression range + breakout trigger gates are already sufficient
            // quality filters — bypassing the generic score gate here is intentional.
            // No score check: the structural requirement (compression box + breakout) IS the quality gate.
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
        // Previously last_entry_ts_ was only set when a NEW entry opened — meaning a
        // position that closed and re-entered on the same tick had a stale last_entry_ts_
        // and bypassed the MIN_ENTRY_GAP_SEC check entirely.
        last_entry_ts_ = std::max(last_entry_ts_, now_s);

        if (tr.exitReason != "SL_HIT") return;

        // Fire cooldown on ALL SL_HIT exits, not just negative pnl ones.
        // Trailing stops that lock above entry produce pnl>0 but are still failed
        // breakouts — skipping cooldown allowed immediate re-entry into the same chop.
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

    void apply_london_session_overrides(SessionType session) {
        if (session != SessionType::LONDON) return;
        // London open (07:00-10:30 UTC): force-enable CompressionBreakout regardless
        // of RegimeGovernor classification.
        //
        // WHY: RegimeGovernor starts in MEAN_REVERSION and transitions to COMPRESSION
        // only after CONFIRM_TICKS (5 ticks) of tight range. At London open, gold often
        // establishes a compression box during the pre-release consolidation, but the
        // governor may still be classifying as MEAN_REVERSION from Asian session.
        // Result: CompressionBreakout is disabled by governor.apply() and misses the
        // very breakout that London open consolidation sets up.
        //
        // Guard: only force-enable if the volatility filter has warmed up (vol_range > 0).
        // Without this, CB fires on every tick from bar-1 of the London session before
        // any real compression structure exists — this was causing the London open SL chain.
        if (vol_filter_.current_range() < 0.50) return;  // not enough history yet — wait for warmup
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
};

} // namespace gold
} // namespace omega
