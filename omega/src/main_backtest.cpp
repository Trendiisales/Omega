// omega/src/main_backtest.cpp
// CLI: backtest the Omega strategy on one symbol CSV.
//
//   omega_backtest <symbol.csv> [benchmark.csv] [--risk 0.01] [--equity 100000]
//                  [--no-regime] [--equity-out curve.csv]
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "omega/Backtester.h"
#include "omega/CsvLoader.h"

using namespace omega;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: omega_backtest <symbol.csv> [benchmark.csv] "
                     "[--risk R] [--equity E] [--no-regime] [--equity-out file]\n";
        return 1;
    }

    std::string symPath = argv[1];
    std::string benchPath;
    std::string equityOut;
    StratConfig strat;
    BacktestConfig bt;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--risk" && i + 1 < argc) strat.riskPerTrade = std::atof(argv[++i]);
        else if (a == "--equity" && i + 1 < argc) bt.startEquity = std::atof(argv[++i]);
        else if (a == "--no-regime") strat.useRegimeFilter = false;
        else if (a == "--equity-out" && i + 1 < argc) equityOut = argv[++i];
        else if (!a.empty() && a[0] != '-') benchPath = a;  // positional benchmark
    }

    try {
        Series symbol = loadCsv(symPath);
        Series bench;
        bool haveBench = false;
        if (!benchPath.empty()) { bench = loadCsv(benchPath); haveBench = true; }

        BacktestResult r = runBacktest(symbol, strat, bt, haveBench ? &bench : nullptr);
        std::cout << formatReport(symbol, r);

        if (!equityOut.empty()) {
            std::ofstream out(equityOut);
            out << "date,equity\n";
            for (std::size_t i = 0; i < r.equityCurve.size(); ++i)
                out << r.equityDates[i] << "," << r.equityCurve[i] << "\n";
            std::cout << "Equity curve written to " << equityOut << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
