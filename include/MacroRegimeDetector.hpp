#pragma once
#include <string>
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
    double VIX_HIGH = 25.0;   // above = risk-off / trend dominant
    double VIX_LOW  = 15.0;   // below = risk-on / range dominant

    // Divergence window
    int    DIV_WINDOW = 20;   // ticks

    void updateVIX(double vix_mid) {
        if (vix_mid > 0) m_vix = vix_mid;
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
    std::string regime() const
    {
        if (m_vix >= VIX_HIGH) return "RISK_OFF";
        if (m_vix > 0 && m_vix <= VIX_LOW) return "RISK_ON";
        return "NEUTRAL";
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
    std::deque<double> m_es_prices;
    std::deque<double> m_nq_prices;
};

} // namespace omega
