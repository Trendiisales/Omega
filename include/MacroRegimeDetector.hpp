#pragma once
#include <string>
#include <chrono>
#include <deque>
#include <cmath>

namespace omega {

// ==============================================================================
// MacroRegimeDetector
// Classifies macro regime as RISK_ON / RISK_OFF / NEUTRAL using three signals:
//
//   1. VIX level         — fear gauge; primary signal
//   2. DXY momentum      — USD strength; early warning independent of VIX
//      Rising DXY = USD bid = risk-off (carry unwind, EM flight to safety)
//      Falling DXY = risk-on (yield-seeking, USD sold for risk assets)
//   3. ES/NQ divergence  — cross-asset confirmation
//
// Regime logic:
//   RISK_OFF: VIX >= VIX_HIGH  OR  (DXY rising fast AND VIX > VIX_LOW)
//   RISK_ON:  VIX <= VIX_LOW   AND DXY falling (or neutral)
//   NEUTRAL:  everything else, or stale VIX
// ==============================================================================

class MacroRegimeDetector
{
public:
    // VIX thresholds
    double VIX_HIGH = 28.0;
    double VIX_LOW  = 20.0;

    // DXY fast-rise threshold — % change over DXY_WINDOW ticks that signals
    // a risk-off USD bid. 0.15% = 15bp over ~60 ticks is a meaningful move.
    double DXY_RISK_OFF_PCT  =  0.15;   // +0.15% DXY = risk-off signal
    double DXY_RISK_ON_PCT   = -0.10;   // -0.10% DXY = confirms risk-on

    int DIV_WINDOW = 60;
    int DXY_WINDOW = 60;   // ticks for DXY momentum window

    void updateVIX(double vix_mid) {
        if (vix_mid > 0) { m_vix = vix_mid; m_vix_ts = nowSec(); }
    }

    void updateDXY(double dxy_mid) {
        if (dxy_mid <= 0) return;
        m_dxy_prices.push_back(dxy_mid);
        if ((int)m_dxy_prices.size() > DXY_WINDOW) m_dxy_prices.pop_front();
        m_dxy_ts = nowSec();
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
        constexpr int64_t VIX_STALE_SEC = 300;
        const bool vix_fresh = (m_vix_ts > 0) &&
                               (nowSec() - m_vix_ts < VIX_STALE_SEC);
        if (!vix_fresh) return "NEUTRAL";

        // Primary: VIX level
        if (m_vix >= VIX_HIGH) return "RISK_OFF";

        // DXY momentum — early warning
        const double dxy_ret = dxyReturn();
        const bool dxy_risk_off = (dxy_ret >= DXY_RISK_OFF_PCT / 100.0);

        // DXY risk-off fires only when VIX is already elevated (not baseline)
        if (dxy_risk_off && m_vix > VIX_LOW) return "RISK_OFF";

        // Risk-on: low VIX + DXY not spiking
        if (m_vix > 0 && m_vix <= VIX_LOW && !dxy_risk_off) return "RISK_ON";

        return "NEUTRAL";
    }

    bool vix_is_stale() const {
        constexpr int64_t VIX_STALE_SEC = 300;
        return (m_vix_ts == 0) || (nowSec() - m_vix_ts >= VIX_STALE_SEC);
    }

    double vixLevel() const { return m_vix; }
    double dxyReturn() const {
        if ((int)m_dxy_prices.size() < 2) return 0.0;
        const double f = m_dxy_prices.front(), b = m_dxy_prices.back();
        return (f > 0) ? (b - f) / f : 0.0;
    }

    // ES vs NQ relative return over DIV_WINDOW ticks
    double esNqDivergence() const
    {
        if ((int)m_es_prices.size() < 2 || (int)m_nq_prices.size() < 2) return 0.0;
        double es_ret = (m_es_prices.back() - m_es_prices.front()) / m_es_prices.front();
        double nq_ret = (m_nq_prices.back() - m_nq_prices.front()) / m_nq_prices.front();
        return es_ret - nq_ret;
    }

private:
    double             m_vix    = 0;
    int64_t            m_vix_ts = 0;
    int64_t            m_dxy_ts = 0;
    std::deque<double> m_es_prices;
    std::deque<double> m_nq_prices;
    std::deque<double> m_dxy_prices;

    static int64_t nowSec() noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

} // namespace omega
