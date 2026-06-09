// omega/include/omega/Bar.h
// Core OHLCV bar type and a per-symbol series container.
// Deterministic, dependency-free. Shared by loader, indicators, strategy,
// backtester, scanner and the IBKR adapter.
#ifndef OMEGA_BAR_H
#define OMEGA_BAR_H

#include <string>
#include <vector>

namespace omega {

// A single daily (or intraday) price bar.
struct Bar {
    std::string date;   // ISO date string, e.g. "2026-06-09"
    double open  = 0.0;
    double high  = 0.0;
    double low   = 0.0;
    double close = 0.0;
    long long volume = 0;
};

// Ordered series of bars for one symbol (oldest first).
struct Series {
    std::string symbol;
    std::vector<Bar> bars;

    std::size_t size() const { return bars.size(); }
    bool empty()      const { return bars.empty(); }

    // Column extractors used heavily by the indicator library.
    std::vector<double> opens()  const { return col(&Bar::open); }
    std::vector<double> highs()  const { return col(&Bar::high); }
    std::vector<double> lows()   const { return col(&Bar::low); }
    std::vector<double> closes() const { return col(&Bar::close); }

    std::vector<double> volumes() const {
        std::vector<double> v;
        v.reserve(bars.size());
        for (const Bar& b : bars) v.push_back(static_cast<double>(b.volume));
        return v;
    }

private:
    std::vector<double> col(double Bar::*field) const {
        std::vector<double> v;
        v.reserve(bars.size());
        for (const Bar& b : bars) v.push_back(b.*field);
        return v;
    }
};

} // namespace omega

#endif // OMEGA_BAR_H
