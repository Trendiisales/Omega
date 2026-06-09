// omega/src/main_scan.cpp
// CLI: scan a directory of OHLCV CSVs for Omega entry candidates.
//
//   omega_scan <csv_dir> [--benchmark SPY] [--top 50] [--no-regime] [--rvol 2.0]
//
// The benchmark symbol (matched by file stem, e.g. SPY.csv) is pulled out of
// the universe and used for the regime filter + relative strength.
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "omega/CsvLoader.h"
#include "omega/Scanner.h"

using namespace omega;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: omega_scan <csv_dir> [--benchmark SYM] [--top N] "
                     "[--no-regime] [--rvol X]\n";
        return 1;
    }
    std::string dir = argv[1];
    std::string benchSym = "SPY";
    int topN = 50;
    StratConfig cfg;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--benchmark" && i + 1 < argc) benchSym = argv[++i];
        else if (a == "--top" && i + 1 < argc) topN = std::atoi(argv[++i]);
        else if (a == "--no-regime") cfg.useRegimeFilter = false;
        else if (a == "--rvol" && i + 1 < argc) cfg.rvolMin = std::atof(argv[++i]);
    }

    try {
        std::vector<Series> all = loadCsvDir(dir);
        std::vector<Series> universe;
        Series benchmark;
        bool haveBench = false;
        for (Series& s : all) {
            if (s.symbol == benchSym) { benchmark = s; haveBench = true; }
            else universe.push_back(s);
        }
        if (universe.empty()) {
            std::cerr << "No symbols to scan in " << dir << "\n";
            return 2;
        }

        std::vector<ScanRow> rows = runScan(universe, cfg, haveBench ? &benchmark : nullptr);
        std::cout << formatScan(rows, topN);
        if (!haveBench)
            std::cerr << "(note: benchmark '" << benchSym
                      << "' not found - regime/RS columns limited)\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 3;
    }
    return 0;
}
