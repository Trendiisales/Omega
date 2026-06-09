// omega/src/Scanner.cpp
#include "omega/Scanner.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

#include "omega/Indicators.h"

namespace omega {

namespace {
inline bool ok(double x) { return std::isfinite(x); }
} // namespace

std::vector<ScanRow> runScan(const std::vector<Series>& universe,
                             const StratConfig& cfg,
                             const Series* benchmark) {
    // Benchmark ROC by date, for the relative-strength column.
    std::map<std::string, double> benchRoc;
    if (benchmark && !benchmark->empty()) {
        std::vector<double> br = ind::roc(benchmark->closes(), cfg.rocPeriod);
        for (int i = 0; i < static_cast<int>(br.size()); ++i)
            benchRoc[benchmark->bars[i].date] = br[i];
    }

    std::vector<ScanRow> rows;
    rows.reserve(universe.size());

    for (const Series& s : universe) {
        if (s.empty()) continue;
        Strategy strat(cfg);
        strat.prepare(s, benchmark);
        const IndicatorSet& ix = strat.indicators();
        const int i = static_cast<int>(s.size()) - 1;

        ScanRow row;
        row.symbol = s.symbol;
        row.date = s.bars[i].date;
        row.close = s.bars[i].close;
        row.adx  = ix.adx[i];
        row.rvol = ix.rvol[i];
        row.cmf  = ix.cmf[i];
        row.roc  = ix.roc[i];

        double bench = 0.0;
        auto it = benchRoc.find(row.date);
        if (it != benchRoc.end() && ok(it->second)) bench = it->second;
        row.rsVsBench = (ok(row.roc) ? row.roc : 0.0) - bench;

        // Component readout (mirrors Strategy's gates).
        double c = row.close, f = ix.emaFast[i], m = ix.emaMid[i], sl = ix.emaSlow[i];
        row.trendStack = ok(f) && ok(m) && ok(sl) && c > f && f > m && m > sl;
        bool rvolOk = ok(row.rvol) && row.rvol > cfg.rvolMin;
        bool breakout = ok(ix.priorHigh[i]) && c > ix.priorHigh[i];
        row.ignition = rvolOk || breakout;
        row.flow = ok(row.cmf) && row.cmf > 0.0;
        row.regime = strat.regimeOk(i);

        row.pass = strat.entryLong(i);

        if (row.pass && ok(ix.atr[i]) && ix.atr[i] > 0.0) {
            row.entry = c;
            row.stop = c - cfg.atrStopMult * ix.atr[i];
            row.riskPerShare = row.entry - row.stop;
        }

        double adxFactor = ok(row.adx) ? row.adx / cfg.adxMin : 0.0;
        double rocPart = ok(row.roc) ? row.roc : 0.0;
        row.score = rocPart * adxFactor + row.rsVsBench;
        rows.push_back(row);
    }

    std::sort(rows.begin(), rows.end(), [](const ScanRow& a, const ScanRow& b) {
        if (a.pass != b.pass) return a.pass > b.pass;          // passers first
        if (a.score != b.score) return a.score > b.score;       // then strongest
        return a.symbol < b.symbol;                              // stable tiebreak
    });
    return rows;
}

std::string formatScan(const std::vector<ScanRow>& rows, int topN) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    os << "===================== OMEGA SCAN =====================\n";
    os << "symbol   date        close   ADX   RVOL    CMF    ROC%   RS%   T I F R  signal   entry    stop\n";
    int shown = 0;
    for (const ScanRow& r : rows) {
        if (shown++ >= topN) break;
        auto yn = [](bool b) { return b ? 'Y' : '.'; };
        os << std::left << std::setw(8) << r.symbol << " "
           << r.date << " " << std::right
           << std::setw(7) << r.close << " "
           << std::setw(5) << r.adx << " "
           << std::setw(6) << r.rvol << " "
           << std::setw(6) << r.cmf << " "
           << std::setw(6) << r.roc << " "
           << std::setw(5) << r.rsVsBench << "   "
           << yn(r.trendStack) << " " << yn(r.ignition) << " "
           << yn(r.flow) << " " << yn(r.regime) << "  "
           << (r.pass ? "ENTER " : "  -   ");
        if (r.pass)
            os << " " << std::setw(7) << r.entry << " " << std::setw(7) << r.stop;
        os << "\n";
    }
    os << "T=TrendStack I=Ignition F=Flow R=Regime.  ENTER = all gates pass.\n";
    os << "======================================================\n";
    return os.str();
}

} // namespace omega
