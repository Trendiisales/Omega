// QndxStrat.hpp -- QNDX (Nasdaq-100 SQF) validated signal + sizing core.
//
// Ported VERBATIM from the ex-IBKRCrypto book (Crypto/include/IbkrCryptoStrat.hpp)
// when the IBKR AU account U23757894 turned out crypto-INELIGIBLE (2026-07-03): every
// crypto secType (spot Paxos + QTF/QEF SQF) is err201 closing-only, leaving QNDX -- an
// index FUT, not crypto -- as the only tradeable leg. The QNDX book is folded into Omega
// as the in-process sub-executor [[QndxSqfIbkr]] (sibling of NqMomoIbkr / BigCapMomoIbkr).
//
// Kept the strategy class BIT-FOR-BIT (only the namespace + roster changed) so the ported
// signals reproduce the Crypto book's QNDX entries/exits exactly -- the parity check (plan
// T5) compares against Crypto/build/ibkrcrypto_bt on the same daily CSV.
//
// QNDX roster uses TWO of the six modes: TSMOM (trend, lb50) + RSIREV (mean-rev, n14).
// The other modes are retained unused (pure daily-OHLC math, no deps) rather than deleted,
// so the class stays identical to the validated source and the parity check is exact.
//
// Pure price (daily OHLC). No funding/L2/CVD/microstructure. Header-only, matches the BT
// bar-for-bar. LOCKED params from the extended-history walk-forward BT (2026-06-24).
//
// PROTECTION: NO per-trade profit-stop / cold-loss cut (kills the trend edge, proven on
// gold/futures/crypto). Protection = vol-target sizing (size_mult) + exit-on-turn (each
// leg flips/closes when its OWN signal reverses). This is the backtested adverse-protection
// verdict per the Engine Adverse-Protection Mandate; see [[QndxSqfIbkr]] header.
#ifndef OMEGA_QNDX_STRAT_H
#define OMEGA_QNDX_STRAT_H
#include <deque>
#include <cmath>
#include <algorithm>

namespace omega {
namespace qndx {

struct Bar { double o,h,l,c; };

// ---- locked params (from extended-history walk-forward BT, 2026-06-24) ----
struct StratParams {
    // IBS
    double ibs_lo = 0.15, ibs_hi = 0.85;
    // TSMom
    int    tsmom_lb = 50;
    // EMA-cross trend
    int    ema_fast = 20, ema_slow = 50;
    // RSI reversal
    int    rsi_n = 14; double rsi_lo = 30, rsi_hi = 70;
    // Keltner channel breakout
    int    kelt_n = 20; double kelt_m = 2.0;
    // Regime-switch (efficiency-ratio gate)
    int    reg_n = 20; double er_hi = 0.40, er_lo = 0.25;
    // vol-target sizing
    double vt_target = 0.02;   // 2%/day notional vol
    int    vt_lb     = 20;
    double vt_min = 0.10, vt_max = 1.50;
    bool   allow_short = true;  // SQF can short (index future)
};

enum class Mode { IBS, TSMOM, EMAX, RSIREV, KELT, REGIME };

// Rolling daily-bar strategy. Feed one completed daily bar; get target position
// (-1/0/+1) and a vol-target size multiplier. Mirrors run_bt() exactly.
class QndxStrat {
public:
    QndxStrat(Mode m, StratParams p=StratParams{}) : mode_(m), p_(p) {}

    void on_daily_bar(const Bar& b){
        bars_.push_back(b);
        if((int)bars_.size() > keep_) bars_.pop_front();
    }

    // target position at the close just pushed (entered next open, live).
    int target() const {
        const int n=(int)bars_.size();
        if(mode_==Mode::IBS){
            const Bar& b=bars_.back(); double rng=b.h-b.l; if(rng<=0) return 0;
            double v=(b.c-b.l)/rng;
            if(v<p_.ibs_lo) return 1;
            if(v>p_.ibs_hi) return p_.allow_short?-1:0;
            return 0;
        } else if(mode_==Mode::TSMOM){
            if(n<p_.tsmom_lb+1) return 0;
            double r=bars_[n-1].c - bars_[n-1-p_.tsmom_lb].c;
            if(r>0) return 1; if(r<0) return p_.allow_short?-1:0; return 0;
        } else if(mode_==Mode::EMAX){
            if(n<4*p_.ema_slow) return 0;
            auto ema=[&](int p)->double{ int st=n-1-4*p; if(st<0)st=0; double a=2.0/(p+1),e=bars_[st].c;
                for(int j=st+1;j<=n-1;++j)e=a*bars_[j].c+(1-a)*e; return e; };
            double ef=ema(p_.ema_fast),es=ema(p_.ema_slow);
            if(ef>es) return 1; if(ef<es) return p_.allow_short?-1:0; return 0;
        } else if(mode_==Mode::RSIREV){
            if(n<p_.rsi_n+1) return 0; double g=0,l=0;
            for(int j=n-p_.rsi_n;j<n;++j){ double d=bars_[j].c-bars_[j-1].c; if(d>0)g+=d; else l-=d; }
            g/=p_.rsi_n; l/=p_.rsi_n; double rs=l>0?g/l:999.0, rsi=100.0-100.0/(1.0+rs);
            if(rsi<p_.rsi_lo) return 1; if(rsi>p_.rsi_hi) return p_.allow_short?-1:0; return 0;
        } else if(mode_==Mode::KELT){
            int N=p_.kelt_n; if(n<N+1) return 0; double a=2.0/(N+1),e=bars_[n-1-N].c;
            for(int j=n-N;j<=n-1;++j)e=a*bars_[j].c+(1-a)*e;
            double atr=0; for(int j=n-N;j<=n-1;++j){double tr=std::max(bars_[j].h-bars_[j].l,std::max(std::fabs(bars_[j].h-bars_[j-1].c),std::fabs(bars_[j].l-bars_[j-1].c)));atr+=tr;} atr/=N;
            double c=bars_[n-1].c; if(atr<=0)return 0;
            if(c>e+p_.kelt_m*atr) return 1; if(c<e-p_.kelt_m*atr) return p_.allow_short?-1:0; return 0;
        } else { // REGIME (ER-gated trend/MR)
            int N=p_.reg_n; if(n<N+1) return 0; double net=std::fabs(bars_[n-1].c-bars_[n-1-N].c),vol=0;
            for(int j=n-N;j<=n-1;++j)vol+=std::fabs(bars_[j].c-bars_[j-1].c);
            double er=vol>0?net/vol:0;
            if(er>p_.er_hi){ int lb=(n-1>=50?50:n-1); double r=bars_[n-1].c-bars_[n-1-lb].c; return r>0?1:(r<0?(p_.allow_short?-1:0):0); }
            if(er<p_.er_lo){ const Bar&b=bars_[n-1]; double rng=b.h-b.l; if(rng<=0)return 0; double v=(b.c-b.l)/rng;
                if(v<p_.ibs_lo)return 1; if(v>p_.ibs_hi)return p_.allow_short?-1:0; }
            return 0;
        }
    }

    // vol-target size multiplier (apply to base notional).
    double size_mult() const {
        if(p_.vt_target<=0) return 1.0;
        const int n=(int)bars_.size(); if(n<p_.vt_lb+1) return p_.vt_min;
        double m=0; int k=0;
        for(int j=n-p_.vt_lb;j<n;++j){ double rr=(bars_[j].c-bars_[j-1].c)/bars_[j-1].c; m+=rr; ++k; }
        m/=k; double s2=0;
        for(int j=n-p_.vt_lb;j<n;++j){ double rr=(bars_[j].c-bars_[j-1].c)/bars_[j-1].c; s2+=(rr-m)*(rr-m); }
        double rv=std::sqrt(s2/k); if(rv<=0) return p_.vt_min;
        return std::max(p_.vt_min,std::min(p_.vt_max,p_.vt_target/rv));
    }
    bool warm() const { int need=p_.vt_lb+1;
        if(mode_==Mode::TSMOM) need=p_.tsmom_lb+1;
        else if(mode_==Mode::EMAX) need=4*p_.ema_slow;
        else if(mode_==Mode::RSIREV) need=p_.rsi_n+1;
        else if(mode_==Mode::KELT) need=p_.kelt_n+1;
        else if(mode_==Mode::REGIME) need=52;
        return (int)bars_.size() >= need; }

private:
    Mode mode_; StratParams p_;
    std::deque<Bar> bars_;
    int keep_ = 256;
};

// The deployable QNDX roster (index leg of the ex-IBKRCrypto book -- the only account-
// eligible instrument). Two orthogonal legs: TSMom50 trend + RSIrev mean-rev.
struct EdgeSpec { const char* sym; Mode mode; const char* tag; const char* grade; };
inline const EdgeSpec QNDX_ROSTER[] = {
    {"QNDX", Mode::TSMOM,  "QndxSqfTrend",  "NDX trend TSMom  OOS 1.64 (index)"},
    {"QNDX", Mode::RSIREV, "QndxSqfMeanRev","NDX RSIrev       OOS 1.30 / bear 1.27 (index MR)"},
    // PROTECTION: NO per-trade profit-stop (kills trend edge, proven); protection =
    // vol-target sizing + exit-on-turn + orthogonal-leg diversification at the book level.
};

} // namespace qndx
} // namespace omega
#endif
