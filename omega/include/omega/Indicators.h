// omega/include/omega/Indicators.h
// Deterministic technical-indicator library.
//
// Conventions:
//  * Every function returns a vector the same length as its input.
//  * Leading values that cannot be computed yet are NaN (std::isnan == true).
//  * No randomness, no global state, no allocation surprises -> identical
//    output for identical input on every run and platform.
#ifndef OMEGA_INDICATORS_H
#define OMEGA_INDICATORS_H

#include <vector>

namespace omega {
namespace ind {

// Simple moving average.
std::vector<double> sma(const std::vector<double>& v, int period);

// Exponential moving average (skips leading NaNs, seeds with an SMA).
std::vector<double> ema(const std::vector<double>& v, int period);

// Rate of change in percent: (v[i] / v[i-period] - 1) * 100.
std::vector<double> roc(const std::vector<double>& v, int period);

// Wilder RSI.
std::vector<double> rsi(const std::vector<double>& close, int period = 14);

// True range and Wilder ATR.
std::vector<double> trueRange(const std::vector<double>& high,
                              const std::vector<double>& low,
                              const std::vector<double>& close);
std::vector<double> atr(const std::vector<double>& high,
                        const std::vector<double>& low,
                        const std::vector<double>& close,
                        int period = 14);

// Wilder ADX (trend-strength). Returns ADX line; +DI/-DI via the *Di helpers.
std::vector<double> adx(const std::vector<double>& high,
                        const std::vector<double>& low,
                        const std::vector<double>& close,
                        int period = 14);

// On-balance volume (cumulative).
std::vector<double> obv(const std::vector<double>& close,
                        const std::vector<double>& volume);

// Chaikin Money Flow over `period`.
std::vector<double> cmf(const std::vector<double>& high,
                        const std::vector<double>& low,
                        const std::vector<double>& close,
                        const std::vector<double>& volume,
                        int period = 20);

// Relative volume: volume[i] / SMA(volume, period)[i-1].
std::vector<double> rvol(const std::vector<double>& volume, int period = 30);

struct Macd {
    std::vector<double> macd;    // fast EMA - slow EMA
    std::vector<double> signal;  // EMA(macd, signalPeriod)
    std::vector<double> hist;    // macd - signal
};
Macd macd(const std::vector<double>& close,
          int fast = 12, int slow = 26, int signal = 9);

// Chandelier exit (long): highest-high(period) - mult * ATR(period).
// Use as a trailing stop; rises with price, never falls within a trade
// (the backtester ratchets it).
std::vector<double> chandelierLong(const std::vector<double>& high,
                                   const std::vector<double>& low,
                                   const std::vector<double>& close,
                                   int period = 22, double mult = 3.0);

// Rolling highest high / lowest low over the prior `period` bars
// (excludes the current bar -> usable as breakout reference).
std::vector<double> rollingHighestPrior(const std::vector<double>& v, int period);
std::vector<double> rollingLowestPrior(const std::vector<double>& v, int period);

} // namespace ind
} // namespace omega

#endif // OMEGA_INDICATORS_H
