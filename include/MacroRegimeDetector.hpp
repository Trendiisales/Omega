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
//   1. VIX level         -- fear gauge; primary signal
//   2. DXY momentum      -- USD strength; early warning independent of VIX
//      Rising DXY = USD bid = risk-off (carry unwind, EM flight to safety)
//      Falling DXY = risk-on (yield-seeking, USD sold for risk assets)
//   3. ES/NQ divergence  -- cross-asset confirmation
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
    // VIX_HIGH raised 28?35: tariff/macro volatility environment keeps VIX 25-32
    // indefinitely. At 28 the engine shuts down after 20 ticks on every session.
    // Individual engines have their own spread/ATR/L2 quality gates -- they don't
    // need VIX to block entries. RISK_OFF at 35+ = genuine crisis (GFC, COVID).
    // Below 35, engine entry gates handle quality filtering per-instrument.
    double VIX_HIGH = 35.0;
    double VIX_LOW  = 20.0;

    // DXY fast-rise threshold -- % change over DXY_WINDOW ticks that signals
    // a risk-off USD bid. 0.15% = 15bp over ~60 ticks is a meaningful move.
    // DXY momentum thresholds -- expressed as % change (e.g. 0.15 = 0.15% = 15bp)
    // Comparison: dxyReturn() returns fraction (e.g. 0.0015) vs threshold / 100
    double DXY_RISK_OFF_PCT  =  0.15;   // +0.15% DXY over window = risk-off signal
    // DXY_RISK_ON_PCT reserved for future: "falling DXY confirms RISK_ON" -- not yet implemented
    // double DXY_RISK_ON_PCT = -0.10;

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

        // DXY momentum -- early warning
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
    // Returns the latest DX.F mid price (0.0 if no feed yet).
    // Used by DXYDivergenceEngine to populate GoldSnapshot.dx_mid.
    double dxyMid() const {
        return m_dxy_prices.empty() ? 0.0 : m_dxy_prices.back();
    }

    // Returns fractional return (e.g. 0.0015 = +0.15%).
    // Compare against DXY_RISK_OFF_PCT / 100.0 to stay in consistent units.
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

} // namespace omega (MacroRegimeDetector)

namespace omega {

// ?????????????????????????????????????????????????????????????????????????????
// HTFBiasFilter -- Higher-Timeframe Bias Filter
//
// Problem: engines fire on their own timeframe without checking higher-TF
// structure. A 1-min ORB entry into a 4H supply zone is low-probability.
// Jane Street requires 2/3 timeframe agreement before adding risk.
//
// Approach: track two rolling windows per symbol --
//   "daily" (last D_WINDOW ticks ? a session) and
//   "intraday" (last ID_WINDOW ticks ? an hour).
// Bias = (recent_mid - window_open) / window_open
//
//   BULLISH: both daily and intraday positive
//   BEARISH: both daily and intraday negative
//   NEUTRAL: mixed or insufficient data
//
// Usage in symbol_gate (additive -- doesn't block, reduces size):
//   auto bias = g_htf_filter.bias(symbol);
//   if (is_long && bias == HTFBias::BEARISH) ? 0.5? size
//   if (is_long && bias == HTFBias::BULLISH) ? normal size
//
// NOTE: We don't hard-block on HTF bias (too many false negatives in ranging
// markets), but size is halved when bias opposes direction. Engines that
// already have regime gating get this as an additional soft filter.
// ?????????????????????????????????????????????????????????????????????????????

enum class HTFBias { BULLISH, BEARISH, NEUTRAL };

class HTFBiasFilter {
public:
    bool enabled       = true;
    int  D_WINDOW      = 500;   // ~session worth of ticks for daily bias
    int  ID_WINDOW     = 100;   // ~1 hour of ticks for intraday bias
    double bias_threshold = 0.0005;  // 0.05% minimum move to call directional

    struct SymState {
        std::deque<double> daily;    // last D_WINDOW mids
        std::deque<double> intraday; // last ID_WINDOW mids
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, SymState> state_;

    void update(const std::string& sym, double mid) {
        if (!enabled || mid <= 0.0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = state_[sym];
        s.daily.push_back(mid);
        if (static_cast<int>(s.daily.size()) > D_WINDOW)
            s.daily.pop_front();
        s.intraday.push_back(mid);
        if (static_cast<int>(s.intraday.size()) > ID_WINDOW)
            s.intraday.pop_front();
    }

    HTFBias bias(const std::string& sym) const {
        if (!enabled) return HTFBias::NEUTRAL;
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = state_.find(sym);
        if (it == state_.end()) return HTFBias::NEUTRAL;
        const auto& s = it->second;
        if (static_cast<int>(s.daily.size()) < 20) return HTFBias::NEUTRAL;
        if (static_cast<int>(s.intraday.size()) < 10) return HTFBias::NEUTRAL;

        const double d_ret  = (s.daily.back()    - s.daily.front())    / s.daily.front();
        const double id_ret = (s.intraday.back()  - s.intraday.front()) / s.intraday.front();

        const bool d_bull  = d_ret  >  bias_threshold;
        const bool d_bear  = d_ret  < -bias_threshold;
        const bool id_bull = id_ret >  bias_threshold;
        const bool id_bear = id_ret < -bias_threshold;

        // Require both TFs to agree (Jane Street 2/3 rule -- we use 2/2 for conservatism)
        if (d_bull && id_bull) return HTFBias::BULLISH;
        if (d_bear && id_bear) return HTFBias::BEARISH;
        return HTFBias::NEUTRAL;
    }

    // Size multiplier: 1.0 if bias aligns with direction, 0.5 if opposed, 0.75 if neutral
    double size_scale(const std::string& sym, bool is_long) const {
        if (!enabled) return 1.0;
        const HTFBias b = bias(sym);
        if (b == HTFBias::NEUTRAL)                  return 0.75;
        if (is_long  && b == HTFBias::BULLISH)      return 1.00;
        if (!is_long && b == HTFBias::BEARISH)      return 1.00;
        // Opposing bias -- halve size, don't block entirely
        return 0.50;
    }

    const char* bias_name(const std::string& sym) const {
        switch (bias(sym)) {
            case HTFBias::BULLISH: return "BULLISH";
            case HTFBias::BEARISH: return "BEARISH";
            default:               return "NEUTRAL";
        }
    }
};

} // namespace omega
