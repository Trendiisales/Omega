#pragma once
#include <string>
#include <chrono>
#include <deque>
#include <cmath>

namespace omega {

// ==============================================================================
// MacroRegimeDetector
// Uses VIX level + ES/NQ divergence to classify macro regime.
// Feeds into BreakoutEngine as context (logged, not a gate).
// ==============================================================================

class MacroRegimeDetector
{
public:
    // VIX thresholds
    double VIX_HIGH = 28.0;   // above = risk-off. Lowered from 30 -- triggers slightly earlier in elevated vol
    double VIX_LOW  = 20.0;   // below = risk-on. Raised from 18 -- post-2024 VIX baseline is 20-25, 18 never fired

    // Divergence window
    int    DIV_WINDOW = 20;   // ticks

    void updateVIX(double vix_mid) {
        if (vix_mid > 0) { m_vix = vix_mid; m_vix_ts = nowSec(); }
    }

    void updateES(double es_mid) {
        if (es_mid <= 0) return;
        m_es_prices.push_back(es_mid);
        if ((int)m_es_prices.size() > DIV_WINDOW) m_es_prices.pop_front();
    }

    void updateNQ(double nq_mid) {
        if (nq_mid <= 0) return;
        m_nq_prices.push_back(nq_mid);
        if ((int)m_nq_prices.size() > DIV_WINDOW) m_nq_prices.pop_front();
    }

    // Returns "RISK_ON", "RISK_OFF", "NEUTRAL"
    // Falls back to NEUTRAL if VIX data is stale (>5 minutes) -- never block on stale regime
    std::string regime() const
    {
        constexpr int64_t VIX_STALE_SEC = 300;  // 5 minutes
        const bool vix_fresh = (m_vix_ts > 0) &&
                               (nowSec() - m_vix_ts < VIX_STALE_SEC);
        if (!vix_fresh) return "NEUTRAL";  // stale VIX -- don't block on unknown data
        if (m_vix >= VIX_HIGH) return "RISK_OFF";
        if (m_vix > 0 && m_vix <= VIX_LOW) return "RISK_ON";
        return "NEUTRAL";
    }

    bool vix_is_stale() const {
        constexpr int64_t VIX_STALE_SEC = 300;
        return (m_vix_ts == 0) || (nowSec() - m_vix_ts >= VIX_STALE_SEC);
    }

    double vixLevel() const { return m_vix; }

    // ES vs NQ relative return over DIV_WINDOW ticks
    double esNqDivergence() const
    {
        if ((int)m_es_prices.size() < 2 || (int)m_nq_prices.size() < 2) return 0.0;
        double es_ret = (m_es_prices.back() - m_es_prices.front()) / m_es_prices.front();
        double nq_ret = (m_nq_prices.back() - m_nq_prices.front()) / m_nq_prices.front();
        return es_ret - nq_ret;  // positive = ES outperforming NQ
    }

private:
    double             m_vix = 0;
    int64_t            m_vix_ts = 0;      // Unix seconds of last VIX update
    std::deque<double> m_es_prices;
    std::deque<double> m_nq_prices;

    static int64_t nowSec() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

} // namespace omega
