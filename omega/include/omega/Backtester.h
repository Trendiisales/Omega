// omega/include/omega/Backtester.h
// Event-driven, single-symbol backtester for the Omega strategy.
// Long-only, ATR-sized positions, chandelier trailing stop, indicator exits.
// Fully deterministic: same inputs -> identical trades and metrics every run.
#ifndef OMEGA_BACKTESTER_H
#define OMEGA_BACKTESTER_H

#include <string>
#include <vector>

#include "omega/Bar.h"
#include "omega/Strategy.h"

namespace omega {

struct Trade {
    std::string entryDate, exitDate;
    double entryPrice = 0.0;
    double exitPrice  = 0.0;
    double initialStop = 0.0;
    double shares = 0.0;
    double pnl = 0.0;          // absolute $ P/L
    double rMultiple = 0.0;    // pnl / initial risk
    std::string exitReason;
};

struct BacktestConfig {
    double startEquity = 100000.0;
    double commissionPerShare = 0.005;  // IBKR-style; set 0 to ignore
    double slippagePerShare   = 0.01;   // applied against you on entry & exit
};

struct BacktestResult {
    std::vector<Trade> trades;
    std::vector<std::string> equityDates;
    std::vector<double> equityCurve;

    double startEquity = 0.0, endEquity = 0.0;
    double totalReturnPct = 0.0, cagrPct = 0.0, maxDrawdownPct = 0.0;
    double winRatePct = 0.0, profitFactor = 0.0;
    double avgWinPct = 0.0, avgLossPct = 0.0, expectancyR = 0.0;
    int numTrades = 0, wins = 0, losses = 0;
    double exposurePct = 0.0;
};

BacktestResult runBacktest(const Series& symbol,
                           const StratConfig& strat,
                           const BacktestConfig& bt,
                           const Series* benchmark = nullptr);

// Human-readable report + a trade blotter.
std::string formatReport(const Series& symbol, const BacktestResult& r);

} // namespace omega

#endif // OMEGA_BACKTESTER_H
