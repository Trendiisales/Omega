// omega/include/omega/Scanner.h
// Multi-symbol scanner. Evaluates the Omega entry stack on the latest bar of
// every symbol and ranks candidates by a momentum/flow composite score.
#ifndef OMEGA_SCANNER_H
#define OMEGA_SCANNER_H

#include <string>
#include <vector>

#include "omega/Bar.h"
#include "omega/Strategy.h"

namespace omega {

struct ScanRow {
    std::string symbol;
    std::string date;
    double close = 0.0;

    bool pass = false;       // passes the full entry stack?
    double score = 0.0;      // ranking score (higher = stronger)

    // Suggested trade levels (only meaningful when pass == true).
    double entry = 0.0;
    double stop = 0.0;
    double riskPerShare = 0.0;

    // Indicator readout for transparency.
    double adx = 0.0, rvol = 0.0, cmf = 0.0, roc = 0.0, rsVsBench = 0.0;
    bool trendStack = false, ignition = false, flow = false, regime = false;
};

// Scan a universe. `benchmark` (e.g. SPY) enables the regime filter and the
// relative-strength column. Results are sorted: passing rows first, then by
// descending score.
std::vector<ScanRow> runScan(const std::vector<Series>& universe,
                             const StratConfig& cfg,
                             const Series* benchmark = nullptr);

std::string formatScan(const std::vector<ScanRow>& rows, int topN = 50);

} // namespace omega

#endif // OMEGA_SCANNER_H
