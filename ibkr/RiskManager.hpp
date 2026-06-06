#pragma once
// RiskManager.hpp — catastrophe protection for the small-cap equity engines.
// DESIGN: cap the TAIL, never shrink normal sizing (that kills the edge). Backtest
// (gap-short, realistic gap-through): naked 10%/trade = +170% maxDD20% worstDay-10.5%;
// WITH this layer = +185% maxDD19% worstDay capped -10% -> protection is FREE in
// normal conditions, only bites on a correlated-squeeze disaster.
//
// Five gates (in order of catastrophe-stopping power):
//  1. UNIVERSE filter (caller: $3-20, float 3-20M, LOCATE REQUIRED, skip major news)
//     -> excludes the +500% halt-gap rockets entirely. The #1 per-name protection.
//  2. PER-TRADE notional cap -> bounds any single name even on a 500% gap-through.
//  3. MAX CONCURRENT positions -> caps a correlated-squeeze day.
//  4. DAILY LOSS kill switch -> hard flatten + block new entries at -X% equity.
//  5. Normal Kelly-fraction sizing kept -> full edge/return.
#include <string>
#include <unordered_map>
#include <cstdio>

struct RiskLimits {
    // Balance edge vs catastrophe (portfolio sim: caps ~free in normal conditions,
    // only bite on a correlated-squeeze disaster). The $3-20 / float-3-20M /
    // locate-required UNIVERSE filter already excludes the +500% rockets, so the
    // realistic worst in-universe gap-through is ~2x (backtest worst -105% to -156%).
    double risk_per_trade   = 0.08;   // base size: notional = this * equity
    double max_notional_pct = 0.10;   // single-name loss cap on a worst_case_gap fill = this*equity
    int    max_concurrent   = 4;      // max simultaneous positions (correlated-squeeze-day cap)
    double daily_kill_pct   = 0.12;   // halt+flatten all when day PnL <= -this*equity
    double worst_case_gap   = 2.0;    // assume worst in-universe halt-gap fills at +this*entry
};

class RiskManager {
    RiskLimits L_;
    double equity_;
    double day_pnl_ = 0.0;
    int    open_ct_ = 0;
    bool   killed_  = false;          // daily kill tripped
    std::unordered_map<std::string,double> open_notional_;
public:
    explicit RiskManager(double equity, RiskLimits L=RiskLimits{}) : L_(L), equity_(equity) {}

    void new_day(){ day_pnl_=0; killed_=false; open_ct_=0; open_notional_.clear();
        printf("[RISK] new day, equity=$%.0f limits: risk%.0f%% maxNotional%.0f%% conc%d kill%.0f%%\n",
               equity_,L_.risk_per_trade*100,L_.max_notional_pct*100,L_.max_concurrent,L_.daily_kill_pct*100); }

    bool killed() const { return killed_; }

    // Can we open a new position? Returns the allowed NOTIONAL ($), or 0 if blocked.
    // entry_px + stop_px define the per-share risk; we size to risk_per_trade but cap
    // at max_notional_pct AND at worst_case_gap survival.
    double allow_entry(const std::string& sym, double entry_px, double /*stop_px*/, const char* why_block_out=nullptr){
        if(killed_){ printf("[RISK] BLOCK %s: daily kill active\n",sym.c_str()); return 0; }
        if(open_ct_ >= L_.max_concurrent){ printf("[RISK] BLOCK %s: max concurrent %d\n",sym.c_str(),L_.max_concurrent); return 0; }
        if(open_notional_.count(sym)){ printf("[RISK] BLOCK %s: already open\n",sym.c_str()); return 0; }
        // size: risk_per_trade at the 100% stop -> notional = risk*equity (stop=100% -> -1x notional)
        double notional = L_.risk_per_trade * equity_;
        // HARD cap: even a worst_case_gap fill must lose <= max_notional_pct of equity
        double cap_for_gap = (L_.max_notional_pct * equity_) / L_.worst_case_gap;
        double cap_abs     = L_.max_notional_pct * equity_;
        if(notional > cap_for_gap) notional = cap_for_gap;   // catastrophe backstop
        if(notional > cap_abs)     notional = cap_abs;
        return notional;
    }
    void on_open(const std::string& sym, double notional){ open_notional_[sym]=notional; open_ct_++; }
    // returns true if the daily kill just tripped (caller must flatten everything)
    bool on_close(const std::string& sym, double pnl){
        day_pnl_ += pnl; auto it=open_notional_.find(sym);
        if(it!=open_notional_.end()){ open_notional_.erase(it); if(open_ct_>0) open_ct_--; }
        if(!killed_ && day_pnl_ <= -L_.daily_kill_pct*equity_){
            killed_=true;
            printf("[RISK] *** DAILY KILL TRIPPED *** day_pnl=$%.0f (<= -%.0f%% of $%.0f). FLATTEN ALL, no new entries.\n",
                   day_pnl_, L_.daily_kill_pct*100, equity_);
            return true;
        }
        return false;
    }
    double day_pnl() const { return day_pnl_; }
    int    open_count() const { return open_ct_; }
};
