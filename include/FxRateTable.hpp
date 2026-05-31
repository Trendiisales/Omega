// =============================================================================
// FxRateTable.hpp -- central-bank policy-rate history for FX carry (S43)
//
// Carry = rate(base_ccy) - rate(quote_ccy). The CARRY EDGE is the persistent
// excess return to holding high-yield vs low-yield currencies (compensation for
// crash risk). It is FX-native: there is NO gold/index analogue. This table is
// the signal source the FxCarryMomentum engine ranks on.
//
// PRECISION: rates are policy-decision levels (Fed upper bound, ECB depo, BoE
// bank rate, BoJ policy rate, RBA/RBNZ/BoC overnight, SNB policy rate), stepped
// at the decision month. Carry RANKING only needs the relative ordering + sign,
// which is robust to small level errors -- so month-granularity step functions
// are sufficient. Values cover 2019-01 .. 2026-01 (knowledge cutoff). Extend the
// tail as new decisions land; the engine clamps to the last known level.
//
// SWAP NOTE: a broker actually pays/charges *swap points*, not the policy rate.
// Swap ~ rate_diff minus the broker's markup. For backtest carry-ranking the
// policy diff is the clean proxy; when live, calibrate against the broker's
// actual tom-next swap (IBKR reports it) before trusting absolute carry $.
// =============================================================================
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace omega {

// days_from_civil (Howard Hinnant) -> unix epoch seconds at 00:00 UTC.
inline int64_t ep(int y,int m,int d){
    y -= m<=2;
    const int era=(y>=0?y:y-399)/400;
    const unsigned yoe=(unsigned)(y-era*400);
    const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    const unsigned doe=yoe*365+yoe/4-yoe/100+doy;
    const int64_t days=(int64_t)era*146097+(int64_t)doe-719468;
    return days*86400LL;
}

struct RateStep { int64_t ts; double rate; };

// Each table is sorted ascending by ts. rate() returns the level effective at
// query ts (last step with step.ts <= ts; clamps to first/last).
struct CcyRates {
    std::string ccy;
    std::vector<RateStep> steps;
    double rate(int64_t ts) const {
        if(steps.empty()) return 0.0;
        double r=steps.front().rate;
        for(const auto& s:steps){ if(s.ts<=ts) r=s.rate; else break; }
        return r;
    }
};

// ---- Policy-rate histories (decision month -> level, % p.a.) ----------------
inline const std::vector<CcyRates>& fx_rate_tables(){
    static const std::vector<CcyRates> T = {
      {"USD", {  // Fed funds upper bound
        {ep(2019,1,1),2.50},{ep(2019,8,1),2.25},{ep(2019,9,1),2.00},{ep(2019,10,1),1.75},
        {ep(2020,3,1),0.25},
        {ep(2022,3,1),0.50},{ep(2022,5,1),1.00},{ep(2022,6,1),1.75},{ep(2022,7,1),2.50},
        {ep(2022,9,1),3.25},{ep(2022,11,1),4.00},{ep(2022,12,1),4.50},
        {ep(2023,2,1),4.75},{ep(2023,3,1),5.00},{ep(2023,5,1),5.25},{ep(2023,7,1),5.50},
        {ep(2024,9,1),5.00},{ep(2024,11,1),4.75},{ep(2024,12,1),4.50},
        {ep(2025,9,1),4.25},{ep(2025,10,1),4.00},{ep(2025,12,1),3.75},
      }},
      {"EUR", {  // ECB deposit rate
        {ep(2019,1,1),-0.40},{ep(2019,9,1),-0.50},
        {ep(2022,7,1),0.00},{ep(2022,9,1),0.75},{ep(2022,11,1),1.50},{ep(2022,12,1),2.00},
        {ep(2023,2,1),2.50},{ep(2023,3,1),3.00},{ep(2023,5,1),3.25},{ep(2023,6,1),3.50},
        {ep(2023,8,1),3.75},{ep(2023,9,1),4.00},
        {ep(2024,6,1),3.75},{ep(2024,9,1),3.50},{ep(2024,10,1),3.25},{ep(2024,12,1),3.00},
        {ep(2025,3,1),2.50},{ep(2025,6,1),2.00},
      }},
      {"GBP", {  // BoE bank rate
        {ep(2019,1,1),0.75},
        {ep(2020,3,1),0.10},
        {ep(2021,12,1),0.25},
        {ep(2022,2,1),0.50},{ep(2022,3,1),0.75},{ep(2022,5,1),1.00},{ep(2022,6,1),1.25},
        {ep(2022,8,1),1.75},{ep(2022,9,1),2.25},{ep(2022,11,1),3.00},{ep(2022,12,1),3.50},
        {ep(2023,2,1),4.00},{ep(2023,3,1),4.25},{ep(2023,5,1),4.50},{ep(2023,6,1),5.00},
        {ep(2023,8,1),5.25},
        {ep(2024,8,1),5.00},{ep(2024,11,1),4.75},
        {ep(2025,2,1),4.50},{ep(2025,5,1),4.25},{ep(2025,8,1),4.00},
      }},
      {"JPY", {  // BoJ policy rate
        {ep(2019,1,1),-0.10},
        {ep(2024,3,1),0.10},{ep(2024,7,1),0.25},
        {ep(2025,1,1),0.50},
      }},
      {"AUD", {  // RBA cash rate
        {ep(2019,1,1),1.50},{ep(2019,6,1),1.25},{ep(2019,7,1),1.00},{ep(2019,10,1),0.75},
        {ep(2020,3,1),0.25},{ep(2020,11,1),0.10},
        {ep(2022,5,1),0.35},{ep(2022,6,1),0.85},{ep(2022,7,1),1.35},{ep(2022,8,1),1.85},
        {ep(2022,9,1),2.35},{ep(2022,10,1),2.60},{ep(2022,11,1),2.85},{ep(2022,12,1),3.10},
        {ep(2023,2,1),3.35},{ep(2023,3,1),3.60},{ep(2023,5,1),3.85},{ep(2023,6,1),4.10},
        {ep(2023,11,1),4.35},
        {ep(2025,2,1),4.10},{ep(2025,5,1),3.85},{ep(2025,8,1),3.60},
      }},
      {"NZD", {  // RBNZ OCR
        {ep(2019,1,1),1.75},{ep(2019,5,1),1.50},{ep(2019,8,1),1.00},
        {ep(2020,3,1),0.25},
        {ep(2021,10,1),0.50},{ep(2021,11,1),0.75},
        {ep(2022,2,1),1.00},{ep(2022,4,1),1.50},{ep(2022,5,1),2.00},{ep(2022,7,1),2.50},
        {ep(2022,8,1),3.00},{ep(2022,10,1),3.50},{ep(2022,11,1),4.25},
        {ep(2023,2,1),4.75},{ep(2023,4,1),5.25},{ep(2023,5,1),5.50},
        {ep(2024,8,1),5.25},{ep(2024,10,1),4.75},{ep(2024,11,1),4.25},
        {ep(2025,2,1),3.75},{ep(2025,4,1),3.50},{ep(2025,5,1),3.25},
      }},
      {"CAD", {  // BoC overnight rate
        {ep(2019,1,1),1.75},
        {ep(2020,3,1),0.25},
        {ep(2022,3,1),0.50},{ep(2022,4,1),1.00},{ep(2022,6,1),1.50},{ep(2022,7,1),2.50},
        {ep(2022,9,1),3.25},{ep(2022,10,1),3.75},{ep(2022,12,1),4.25},
        {ep(2023,1,1),4.50},{ep(2023,6,1),4.75},{ep(2023,7,1),5.00},
        {ep(2024,6,1),4.75},{ep(2024,7,1),4.50},{ep(2024,9,1),4.25},{ep(2024,10,1),3.75},{ep(2024,12,1),3.25},
        {ep(2025,1,1),3.00},{ep(2025,3,1),2.75},
      }},
      {"CHF", {  // SNB policy rate
        {ep(2019,1,1),-0.75},
        {ep(2022,6,1),-0.25},{ep(2022,9,1),0.50},{ep(2022,12,1),1.00},
        {ep(2023,3,1),1.50},{ep(2023,6,1),1.75},
        {ep(2024,3,1),1.50},{ep(2024,6,1),1.25},{ep(2024,9,1),1.00},{ep(2024,12,1),0.50},
        {ep(2025,3,1),0.25},{ep(2025,6,1),0.00},
      }},
    };
    return T;
}

inline double ccy_rate(const char* ccy, int64_t ts){
    for(const auto& t:fx_rate_tables()) if(t.ccy==ccy) return t.rate(ts);
    return 0.0;
}

// Carry of a pair "EURUSD" at ts = rate(EUR) - rate(USD). Pair must be 6 chars.
inline double pair_carry(const char* pair, int64_t ts){
    char base[4]={0}, quote[4]={0};
    std::memcpy(base, pair, 3);
    std::memcpy(quote, pair+3, 3);
    return ccy_rate(base, ts) - ccy_rate(quote, ts);
}

} // namespace omega
