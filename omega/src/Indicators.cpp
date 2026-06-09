// omega/src/Indicators.cpp
#include "omega/Indicators.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace omega {
namespace ind {

namespace {
const double NaN = std::numeric_limits<double>::quiet_NaN();
inline bool ok(double x) { return std::isfinite(x); }
} // namespace

std::vector<double> sma(const std::vector<double>& v, int period) {
    std::vector<double> out(v.size(), NaN);
    if (period <= 0 || static_cast<int>(v.size()) < period) return out;
    double sum = 0.0;
    for (int i = 0; i < static_cast<int>(v.size()); ++i) {
        sum += v[i];
        if (i >= period) sum -= v[i - period];
        if (i >= period - 1) out[i] = sum / period;
    }
    return out;
}

std::vector<double> ema(const std::vector<double>& v, int period) {
    std::vector<double> out(v.size(), NaN);
    if (period <= 0) return out;

    // Skip leading NaNs so EMA can sit on top of other indicators (e.g. MACD).
    int start = 0;
    while (start < static_cast<int>(v.size()) && !ok(v[start])) ++start;
    if (static_cast<int>(v.size()) - start < period) return out;

    const double k = 2.0 / (period + 1.0);
    double seed = 0.0;
    for (int i = start; i < start + period; ++i) seed += v[i];
    seed /= period;
    out[start + period - 1] = seed;
    for (int i = start + period; i < static_cast<int>(v.size()); ++i)
        out[i] = v[i] * k + out[i - 1] * (1.0 - k);
    return out;
}

std::vector<double> roc(const std::vector<double>& v, int period) {
    std::vector<double> out(v.size(), NaN);
    if (period <= 0) return out;
    for (int i = period; i < static_cast<int>(v.size()); ++i)
        if (v[i - period] != 0.0) out[i] = (v[i] / v[i - period] - 1.0) * 100.0;
    return out;
}

std::vector<double> rsi(const std::vector<double>& close, int period) {
    std::vector<double> out(close.size(), NaN);
    if (period <= 0 || static_cast<int>(close.size()) <= period) return out;

    double gain = 0.0, loss = 0.0;
    for (int i = 1; i <= period; ++i) {
        double ch = close[i] - close[i - 1];
        if (ch >= 0) gain += ch; else loss -= ch;
    }
    double avgGain = gain / period, avgLoss = loss / period;
    out[period] = (avgLoss == 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + avgGain / avgLoss);

    for (int i = period + 1; i < static_cast<int>(close.size()); ++i) {
        double ch = close[i] - close[i - 1];
        double g = ch > 0 ? ch : 0.0;
        double l = ch < 0 ? -ch : 0.0;
        avgGain = (avgGain * (period - 1) + g) / period;
        avgLoss = (avgLoss * (period - 1) + l) / period;
        out[i] = (avgLoss == 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + avgGain / avgLoss);
    }
    return out;
}

std::vector<double> trueRange(const std::vector<double>& high,
                              const std::vector<double>& low,
                              const std::vector<double>& close) {
    std::vector<double> tr(high.size(), NaN);
    if (high.empty()) return tr;
    tr[0] = high[0] - low[0];
    for (int i = 1; i < static_cast<int>(high.size()); ++i) {
        double a = high[i] - low[i];
        double b = std::fabs(high[i] - close[i - 1]);
        double c = std::fabs(low[i] - close[i - 1]);
        tr[i] = std::max(a, std::max(b, c));
    }
    return tr;
}

std::vector<double> atr(const std::vector<double>& high,
                        const std::vector<double>& low,
                        const std::vector<double>& close,
                        int period) {
    std::vector<double> out(high.size(), NaN);
    if (period <= 0 || static_cast<int>(high.size()) <= period) return out;
    std::vector<double> tr = trueRange(high, low, close);

    double sum = 0.0;
    for (int i = 1; i <= period; ++i) sum += tr[i];   // TR[0] excluded (no prev close)
    double prev = sum / period;
    out[period] = prev;
    for (int i = period + 1; i < static_cast<int>(high.size()); ++i) {
        prev = (prev * (period - 1) + tr[i]) / period;  // Wilder smoothing
        out[i] = prev;
    }
    return out;
}

std::vector<double> adx(const std::vector<double>& high,
                        const std::vector<double>& low,
                        const std::vector<double>& close,
                        int period) {
    const int n = static_cast<int>(high.size());
    std::vector<double> out(n, NaN);
    if (period <= 0 || n <= 2 * period) return out;

    std::vector<double> tr(n, 0.0), plusDM(n, 0.0), minusDM(n, 0.0);
    for (int i = 1; i < n; ++i) {
        double up = high[i] - high[i - 1];
        double dn = low[i - 1] - low[i];
        plusDM[i]  = (up > dn && up > 0) ? up : 0.0;
        minusDM[i] = (dn > up && dn > 0) ? dn : 0.0;
        double a = high[i] - low[i];
        double b = std::fabs(high[i] - close[i - 1]);
        double c = std::fabs(low[i] - close[i - 1]);
        tr[i] = std::max(a, std::max(b, c));
    }

    // Wilder smoothed sums seeded over the first `period` bars (indices 1..period).
    double trS = 0.0, pS = 0.0, mS = 0.0;
    for (int i = 1; i <= period; ++i) { trS += tr[i]; pS += plusDM[i]; mS += minusDM[i]; }

    std::vector<double> dx(n, NaN);
    auto computeDx = [&](double tS, double p, double m) -> double {
        double pdi = tS == 0.0 ? 0.0 : 100.0 * p / tS;
        double mdi = tS == 0.0 ? 0.0 : 100.0 * m / tS;
        double denom = pdi + mdi;
        return denom == 0.0 ? 0.0 : 100.0 * std::fabs(pdi - mdi) / denom;
    };
    dx[period] = computeDx(trS, pS, mS);
    for (int i = period + 1; i < n; ++i) {
        trS = trS - trS / period + tr[i];
        pS  = pS  - pS  / period + plusDM[i];
        mS  = mS  - mS  / period + minusDM[i];
        dx[i] = computeDx(trS, pS, mS);
    }

    // ADX = Wilder smoothing of DX over `period`, first value at index 2*period.
    double adxSum = 0.0;
    for (int i = period; i < 2 * period; ++i) adxSum += dx[i];
    double prevAdx = adxSum / period;
    out[2 * period - 1] = prevAdx;
    for (int i = 2 * period; i < n; ++i) {
        prevAdx = (prevAdx * (period - 1) + dx[i]) / period;
        out[i] = prevAdx;
    }
    return out;
}

std::vector<double> obv(const std::vector<double>& close,
                        const std::vector<double>& volume) {
    std::vector<double> out(close.size(), NaN);
    if (close.empty()) return out;
    double run = 0.0;
    out[0] = 0.0;
    for (int i = 1; i < static_cast<int>(close.size()); ++i) {
        if (close[i] > close[i - 1]) run += volume[i];
        else if (close[i] < close[i - 1]) run -= volume[i];
        out[i] = run;
    }
    return out;
}

std::vector<double> cmf(const std::vector<double>& high,
                        const std::vector<double>& low,
                        const std::vector<double>& close,
                        const std::vector<double>& volume,
                        int period) {
    const int n = static_cast<int>(high.size());
    std::vector<double> out(n, NaN);
    if (period <= 0 || n < period) return out;

    std::vector<double> mfv(n, 0.0);
    for (int i = 0; i < n; ++i) {
        double range = high[i] - low[i];
        double mult = range == 0.0 ? 0.0
                    : ((close[i] - low[i]) - (high[i] - close[i])) / range;
        mfv[i] = mult * volume[i];
    }
    double mfvSum = 0.0, volSum = 0.0;
    for (int i = 0; i < n; ++i) {
        mfvSum += mfv[i];
        volSum += volume[i];
        if (i >= period) { mfvSum -= mfv[i - period]; volSum -= volume[i - period]; }
        if (i >= period - 1) out[i] = volSum == 0.0 ? 0.0 : mfvSum / volSum;
    }
    return out;
}

std::vector<double> rvol(const std::vector<double>& volume, int period) {
    std::vector<double> out(volume.size(), NaN);
    std::vector<double> avg = sma(volume, period);
    for (int i = 1; i < static_cast<int>(volume.size()); ++i)
        if (ok(avg[i - 1]) && avg[i - 1] > 0.0) out[i] = volume[i] / avg[i - 1];
    return out;
}

Macd macd(const std::vector<double>& close, int fast, int slow, int signal) {
    Macd m;
    std::vector<double> ef = ema(close, fast);
    std::vector<double> es = ema(close, slow);
    m.macd.assign(close.size(), NaN);
    for (int i = 0; i < static_cast<int>(close.size()); ++i)
        if (ok(ef[i]) && ok(es[i])) m.macd[i] = ef[i] - es[i];
    m.signal = ema(m.macd, signal);
    m.hist.assign(close.size(), NaN);
    for (int i = 0; i < static_cast<int>(close.size()); ++i)
        if (ok(m.macd[i]) && ok(m.signal[i])) m.hist[i] = m.macd[i] - m.signal[i];
    return m;
}

std::vector<double> rollingHighestPrior(const std::vector<double>& v, int period) {
    std::vector<double> out(v.size(), NaN);
    if (period <= 0) return out;
    for (int i = period; i < static_cast<int>(v.size()); ++i) {
        double hi = v[i - period];
        for (int j = i - period + 1; j < i; ++j) hi = std::max(hi, v[j]);
        out[i] = hi;
    }
    return out;
}

std::vector<double> rollingLowestPrior(const std::vector<double>& v, int period) {
    std::vector<double> out(v.size(), NaN);
    if (period <= 0) return out;
    for (int i = period; i < static_cast<int>(v.size()); ++i) {
        double lo = v[i - period];
        for (int j = i - period + 1; j < i; ++j) lo = std::min(lo, v[j]);
        out[i] = lo;
    }
    return out;
}

std::vector<double> chandelierLong(const std::vector<double>& high,
                                   const std::vector<double>& low,
                                   const std::vector<double>& close,
                                   int period, double mult) {
    const int n = static_cast<int>(high.size());
    std::vector<double> out(n, NaN);
    std::vector<double> a = atr(high, low, close, period);
    for (int i = period; i < n; ++i) {
        if (!ok(a[i])) continue;
        double hh = high[i];
        for (int j = i - period + 1; j <= i; ++j) hh = std::max(hh, high[j]);
        out[i] = hh - mult * a[i];
    }
    return out;
}

} // namespace ind
} // namespace omega
