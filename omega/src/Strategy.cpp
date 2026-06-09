// omega/src/Strategy.cpp
#include "omega/Strategy.h"

#include <cmath>

namespace omega {

namespace {
inline bool ok(double x) { return std::isfinite(x); }
} // namespace

void Strategy::prepare(const Series& s, const Series* benchmark) {
    s_ = &s;
    std::vector<double> high  = s.highs();
    std::vector<double> low   = s.lows();
    std::vector<double> close = s.closes();
    std::vector<double> vol   = s.volumes();

    ix_.emaFast = ind::ema(close, cfg_.emaFast);
    ix_.emaMid  = ind::ema(close, cfg_.emaMid);
    ix_.emaSlow = ind::ema(close, cfg_.emaSlow);
    ix_.adx     = ind::adx(high, low, close, cfg_.adxPeriod);
    ix_.atr     = ind::atr(high, low, close, cfg_.atrPeriod);
    ix_.rsi     = ind::rsi(close, cfg_.rsiPeriod);
    ix_.roc     = ind::roc(close, cfg_.rocPeriod);
    ix_.obv     = ind::obv(close, vol);
    ix_.cmf     = ind::cmf(high, low, close, vol, cfg_.cmfPeriod);
    ix_.rvol    = ind::rvol(vol, cfg_.rvolPeriod);
    ix_.chandelier = ind::chandelierLong(high, low, close,
                                         cfg_.chandelierPeriod, cfg_.chandelierMult);
    ix_.priorHigh  = ind::rollingHighestPrior(high, cfg_.breakoutLookback);
    ix_.macd    = ind::macd(close, cfg_.macdFast, cfg_.macdSlow, cfg_.macdSignal);

    regimeByDate_.clear();
    if (benchmark && !benchmark->empty()) {
        std::vector<double> bClose = benchmark->closes();
        std::vector<double> bLong  = ind::ema(bClose, cfg_.emaLong);
        for (int i = 0; i < static_cast<int>(bClose.size()); ++i) {
            bool healthy = false;
            if (ok(bLong[i]) && i >= cfg_.slowRisingLookback)
                healthy = bClose[i] > bLong[i] && bLong[i] > bLong[i - cfg_.slowRisingLookback];
            regimeByDate_[benchmark->bars[i].date] = healthy;
        }
    }
}

bool Strategy::regimeOk(int i) const {
    if (!cfg_.useRegimeFilter) return true;
    if (regimeByDate_.empty()) return true;  // no benchmark supplied -> don't block
    auto it = regimeByDate_.find(s_->bars[i].date);
    return it == regimeByDate_.end() ? true : it->second;
}

bool Strategy::trendStackOk(int i) const {
    if (i < cfg_.slowRisingLookback) return false;
    double c = s_->bars[i].close;
    double f = ix_.emaFast[i], m = ix_.emaMid[i], sl = ix_.emaSlow[i];
    if (!ok(f) || !ok(m) || !ok(sl)) return false;
    if (!(c > f && f > m && m > sl)) return false;
    double slPrev = ix_.emaSlow[i - cfg_.slowRisingLookback];
    return ok(slPrev) && sl > slPrev;          // slow EMA rising
}

bool Strategy::ignitionOk(int i) const {
    bool rvolOk = ok(ix_.rvol[i]) && ix_.rvol[i] > cfg_.rvolMin;
    bool breakout = ok(ix_.priorHigh[i]) && s_->bars[i].close > ix_.priorHigh[i];
    return rvolOk || breakout;
}

bool Strategy::flowOk(int i) const {
    return ok(ix_.cmf[i]) && ix_.cmf[i] > 0.0;
}

bool Strategy::entryLong(int i) const {
    if (i < 1 || i >= static_cast<int>(s_->size())) return false;
    return trendStackOk(i) && ok(ix_.adx[i]) && ix_.adx[i] > cfg_.adxMin
        && ignitionOk(i) && flowOk(i) && regimeOk(i);
}

std::string Strategy::indicatorExit(int i) const {
    if (i < 1 || i >= static_cast<int>(s_->size())) return "";
    double c = s_->bars[i].close;
    if (ok(ix_.emaFast[i]) && c < ix_.emaFast[i]) return "ema_break";

    const auto& md = ix_.macd;
    if (ok(md.macd[i]) && ok(md.signal[i]) && ok(md.macd[i - 1]) && ok(md.signal[i - 1])
        && md.macd[i] < md.signal[i] && md.macd[i - 1] >= md.signal[i - 1])
        return "macd_cross";

    if (ok(ix_.cmf[i]) && ix_.cmf[i] < 0.0) return "distribution";
    return "";
}

} // namespace omega
