// omega/src/Backtester.cpp
#include "omega/Backtester.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace omega {

namespace {
inline bool ok(double x) { return std::isfinite(x); }
} // namespace

BacktestResult runBacktest(const Series& symbol,
                           const StratConfig& strat,
                           const BacktestConfig& bt,
                           const Series* benchmark) {
    BacktestResult r;
    r.startEquity = bt.startEquity;

    Strategy strategy(strat);
    strategy.prepare(symbol, benchmark);
    const IndicatorSet& ix = strategy.indicators();
    const int n = static_cast<int>(symbol.size());

    double realized = bt.startEquity;  // realized equity (closed trades)
    bool inPos = false;
    double shares = 0.0, entryPrice = 0.0, initialStop = 0.0, stop = 0.0;
    std::string entryDate;

    double peakEquity = bt.startEquity;
    double maxDD = 0.0;
    int barsInMarket = 0;

    auto markEquity = [&](int i) {
        double eq = realized;
        if (inPos) eq += shares * (symbol.bars[i].close - entryPrice);
        r.equityDates.push_back(symbol.bars[i].date);
        r.equityCurve.push_back(eq);
        if (eq > peakEquity) peakEquity = eq;
        double dd = peakEquity > 0 ? (peakEquity - eq) / peakEquity * 100.0 : 0.0;
        if (dd > maxDD) maxDD = dd;
        return eq;
    };

    auto closeTrade = [&](int i, double exitPx, const std::string& reason) {
        double exitFill = exitPx - bt.slippagePerShare;
        double gross = shares * (exitFill - entryPrice);
        double commission = bt.commissionPerShare * shares * 2.0;  // in + out
        double pnl = gross - commission;
        realized += pnl;

        Trade t;
        t.entryDate = entryDate;
        t.exitDate  = symbol.bars[i].date;
        t.entryPrice = entryPrice;
        t.exitPrice  = exitFill;
        t.initialStop = initialStop;
        t.shares = shares;
        t.pnl = pnl;
        double risk = (entryPrice - initialStop) * shares;
        t.rMultiple = risk > 0 ? pnl / risk : 0.0;
        t.exitReason = reason;
        r.trades.push_back(t);

        inPos = false;
        shares = 0.0;
    };

    for (int i = 1; i < n; ++i) {
        if (inPos) {
            ++barsInMarket;
            const Bar& b = symbol.bars[i];

            // Ratchet the trailing stop upward only.
            if (ok(ix.chandelier[i]) && ix.chandelier[i] > stop) stop = ix.chandelier[i];

            // 1) Hard stop / trailing stop (intrabar).
            if (b.low <= stop) {
                double exitPx = std::min(stop, b.open);  // gap-through fills at open
                closeTrade(i, exitPx, "stop");
                markEquity(i);
                continue;
            }
            // 2) Indicator exit at the close.
            std::string reason = strategy.indicatorExit(i);
            if (!reason.empty()) {
                closeTrade(i, b.close, reason);
                markEquity(i);
                continue;
            }
            markEquity(i);
            continue;
        }

        // Flat: look for an entry.
        if (strategy.entryLong(i) && ok(ix.atr[i]) && ix.atr[i] > 0.0) {
            double equityNow = realized;  // flat, so realized == current equity
            entryPrice = symbol.bars[i].close + bt.slippagePerShare;
            initialStop = symbol.bars[i].close - strat.atrStopMult * ix.atr[i];
            double riskPerShare = entryPrice - initialStop;
            if (riskPerShare > 0.0) {
                double sized = std::floor((equityNow * strat.riskPerTrade) / riskPerShare);
                if (sized >= 1.0) {
                    shares = sized;
                    stop = initialStop;
                    entryDate = symbol.bars[i].date;
                    inPos = true;
                    ++barsInMarket;
                }
            }
        }
        markEquity(i);
    }

    // Close any open position at the last bar.
    if (inPos) {
        int i = n - 1;
        closeTrade(i, symbol.bars[i].close, "eod_close");
        if (!r.equityCurve.empty()) {
            r.equityCurve.back() = realized;
            if (realized > peakEquity) peakEquity = realized;
            double dd = (peakEquity - realized) / peakEquity * 100.0;
            if (dd > maxDD) maxDD = dd;
        }
    }

    // Aggregate metrics.
    r.endEquity = realized;
    r.totalReturnPct = (realized / bt.startEquity - 1.0) * 100.0;
    r.maxDrawdownPct = maxDD;
    r.numTrades = static_cast<int>(r.trades.size());

    double grossWin = 0.0, grossLoss = 0.0, sumR = 0.0;
    double sumWinPct = 0.0, sumLossPct = 0.0;
    for (const Trade& t : r.trades) {
        sumR += t.rMultiple;
        double pct = (t.entryPrice > 0)
                   ? (t.exitPrice - t.entryPrice) / t.entryPrice * 100.0 : 0.0;
        if (t.pnl >= 0) { ++r.wins; grossWin += t.pnl; sumWinPct += pct; }
        else            { ++r.losses; grossLoss += -t.pnl; sumLossPct += pct; }
    }
    r.winRatePct = r.numTrades ? 100.0 * r.wins / r.numTrades : 0.0;
    r.profitFactor = grossLoss > 0 ? grossWin / grossLoss
                                   : (grossWin > 0 ? 1e9 : 0.0);
    r.avgWinPct = r.wins ? sumWinPct / r.wins : 0.0;
    r.avgLossPct = r.losses ? sumLossPct / r.losses : 0.0;
    r.expectancyR = r.numTrades ? sumR / r.numTrades : 0.0;
    r.exposurePct = n > 1 ? 100.0 * barsInMarket / (n - 1) : 0.0;

    double years = (n > 1) ? static_cast<double>(n - 1) / 252.0 : 0.0;
    if (years > 0 && bt.startEquity > 0 && realized > 0)
        r.cagrPct = (std::pow(realized / bt.startEquity, 1.0 / years) - 1.0) * 100.0;

    return r;
}

std::string formatReport(const Series& symbol, const BacktestResult& r) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    os << "================ OMEGA BACKTEST: " << symbol.symbol << " ================\n";
    os << "Bars                : " << symbol.size() << "\n";
    os << "Start equity        : $" << r.startEquity << "\n";
    os << "End equity          : $" << r.endEquity << "\n";
    os << "Total return        : " << r.totalReturnPct << " %\n";
    os << "CAGR (approx)       : " << r.cagrPct << " %\n";
    os << "Max drawdown        : " << r.maxDrawdownPct << " %\n";
    os << "Trades              : " << r.numTrades
       << "  (W " << r.wins << " / L " << r.losses << ")\n";
    os << "Win rate            : " << r.winRatePct << " %\n";
    os << "Profit factor       : " << r.profitFactor << "\n";
    os << "Avg win / avg loss  : " << r.avgWinPct << " % / " << r.avgLossPct << " %\n";
    os << "Expectancy          : " << r.expectancyR << " R per trade\n";
    os << "Time in market      : " << r.exposurePct << " %\n";
    os << "-----------------------------------------------------------\n";
    os << "Blotter:\n";
    os << "  #  entry       exit        in      out     shares    P/L($)   R    reason\n";
    int k = 0;
    for (const Trade& t : r.trades) {
        ++k;
        os << std::setw(3) << k << "  "
           << t.entryDate << "  " << t.exitDate << "  "
           << std::setw(7) << t.entryPrice << " "
           << std::setw(7) << t.exitPrice << " "
           << std::setw(8) << t.shares << " "
           << std::setw(9) << t.pnl << " "
           << std::setw(5) << t.rMultiple << "  "
           << t.exitReason << "\n";
    }
    os << "===========================================================\n";
    return os.str();
}

} // namespace omega
