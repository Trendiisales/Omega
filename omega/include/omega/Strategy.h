// omega/include/omega/Strategy.h
// The Omega momentum strategy: configuration, precomputed indicator set, and
// the entry/exit rule engine. Pure functions of price data -> deterministic.
//
// The method (long-only):
//   ENTER  when ALL agree:
//     * Trend stack : close > emaFast > emaMid > emaSlow, slow EMA rising
//     * Strength    : ADX > adxMin
//     * Ignition    : RVOL > rvolMin  OR  breakout above prior-N high
//     * Flow        : CMF > 0  (net accumulation / "money in")
//     * Regime      : benchmark above its long EMA (skip if disabled)
//   EXIT on the FIRST of:
//     * Stop / chandelier trailing stop hit          (handled by backtester)
//     * close < emaFast            -> "ema_break"
//     * MACD crosses below signal  -> "macd_cross"
//     * CMF < 0                    -> "distribution" (money leaving)
#ifndef OMEGA_STRATEGY_H
#define OMEGA_STRATEGY_H

#include <map>
#include <string>
#include <vector>

#include "omega/Bar.h"
#include "omega/Indicators.h"

namespace omega {

struct StratConfig {
    // Trend stack
    int emaFast = 10;
    int emaMid  = 20;
    int emaSlow = 50;
    int emaLong = 200;   // used for the benchmark regime filter
    int slowRisingLookback = 5;

    // Strength
    int adxPeriod = 14;
    double adxMin = 25.0;

    // Volatility / sizing / stops
    int atrPeriod = 14;
    double atrStopMult  = 2.0;   // initial stop distance
    int chandelierPeriod = 22;
    double chandelierMult = 3.0; // trailing stop distance

    // Ignition
    int rvolPeriod = 30;
    double rvolMin = 2.0;
    int breakoutLookback = 20;
    int rocPeriod = 20;

    // Flow
    int cmfPeriod = 20;

    // Momentum exit
    int rsiPeriod = 14;
    int macdFast = 12, macdSlow = 26, macdSignal = 9;

    // Risk
    double riskPerTrade = 0.01;  // fraction of equity risked per trade
    bool useRegimeFilter = true;
};

// Precomputed indicator vectors aligned 1:1 with the series bars.
struct IndicatorSet {
    std::vector<double> emaFast, emaMid, emaSlow;
    std::vector<double> adx, atr, rsi, roc, obv, cmf, rvol;
    std::vector<double> chandelier, priorHigh;
    ind::Macd macd;
};

class Strategy {
public:
    explicit Strategy(StratConfig cfg) : cfg_(cfg) {}

    // Precompute everything for one symbol. `benchmark` (e.g. SPY) is optional
    // and drives the regime filter via date matching.
    void prepare(const Series& s, const Series* benchmark = nullptr);

    bool entryLong(int i) const;

    // Returns an exit reason for indicator-based exits, or "" if none.
    // Stop/trailing-stop exits are handled by the backtester.
    std::string indicatorExit(int i) const;

    bool regimeOk(int i) const;

    const IndicatorSet& indicators() const { return ix_; }
    const StratConfig&  config()     const { return cfg_; }
    const Series&       series()     const { return *s_; }

private:
    bool trendStackOk(int i) const;
    bool ignitionOk(int i)   const;
    bool flowOk(int i)       const;

    StratConfig cfg_;
    const Series* s_ = nullptr;
    IndicatorSet ix_;
    std::map<std::string, bool> regimeByDate_;  // benchmark date -> healthy?
};

} // namespace omega

#endif // OMEGA_STRATEGY_H
