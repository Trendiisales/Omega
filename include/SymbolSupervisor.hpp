#pragma once
#include <string>

namespace omega {

enum class Regime
{
    UNKNOWN = 0,
    EXPANSION_BREAKOUT,
    TREND_CONTINUATION,
    HIGH_RISK_NO_TRADE
};

inline const char* regime_name(Regime r)
{
    switch (r)
    {
        case Regime::EXPANSION_BREAKOUT: return "EXPANSION_BREAKOUT";
        case Regime::TREND_CONTINUATION: return "TREND_CONTINUATION";
        case Regime::HIGH_RISK_NO_TRADE: return "HIGH_RISK_NO_TRADE";
        default: return "UNKNOWN";
    }
}

struct SupervisorDecision
{
    bool allow_trade = true;
    bool allow_breakout = true;
    Regime regime = Regime::UNKNOWN;
};

struct SupervisorConfig
{
    double max_spread = 0.0;
    double max_volatility = 0.0;
};

class SymbolSupervisor
{
public:

    std::string symbol;
    SupervisorConfig cfg;

    SymbolSupervisor() {}

    template<typename Engine>
    SupervisorDecision update(
        Engine&,
        bool signal,
        const std::string& sym,
        double price,
        double spread,
        double vol,
        double atr,
        double momentum,
        double velocity)
    {
        SupervisorDecision d;

        symbol = sym;

        if (!signal)
        {
            d.allow_trade = false;
            d.allow_breakout = false;
            d.regime = Regime::HIGH_RISK_NO_TRADE;
            return d;
        }

        d.allow_trade = true;
        d.allow_breakout = true;
        d.regime = Regime::TREND_CONTINUATION;

        return d;
    }

    void on_trade_success() {}
};

}
