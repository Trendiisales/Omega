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
    bool allow_trade;
    Regime regime;

    SupervisorDecision()
        : allow_trade(true), regime(Regime::UNKNOWN) {}
};

class SymbolSupervisor
{
public:

    SymbolSupervisor() {}

    template<typename Engine>
    SupervisorDecision update(
        Engine&,
        bool signal,
        const std::string&,
        double,
        double)
    {
        SupervisorDecision d;

        if (!signal)
        {
            d.allow_trade = false;
            d.regime = Regime::HIGH_RISK_NO_TRADE;
            return d;
        }

        d.allow_trade = true;
        d.regime = Regime::TREND_CONTINUATION;

        return d;
    }

    void on_trade_success()
    {
    }
};

}
