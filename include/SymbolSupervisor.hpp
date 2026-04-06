#pragma once
#include <string>
#include <algorithm>
#include <cmath>

struct SupervisorDecision
{
    bool allow;
    std::string winner;
    std::string reason;
    double top_score;
};

class SymbolSupervisor
{
public:

    double breakout_score = 0.0;
    double bracket_score = 0.0;

    double threshold = 0.25;

    double current_spread = 0.0;
    double max_spread = 0.0;

    double latency_ms = 0.0;
    double latency_cap_ms = 150.0;

    bool governor_pause = false;

    SupervisorDecision decide()
    {
        SupervisorDecision d;

        double top = std::max(breakout_score, bracket_score);
        d.top_score = top;

        bool spread_block = (max_spread > 0.0 && current_spread > max_spread);
        bool latency_block = (latency_ms > latency_cap_ms);
        bool governor_block = governor_pause;

        if(spread_block)
        {
            d.allow = false;
            d.winner = "NONE";
            d.reason = "spread_guard";
            return d;
        }

        if(latency_block)
        {
            d.allow = false;
            d.winner = "NONE";
            d.reason = "latency_guard";
            return d;
        }

        if(governor_block)
        {
            d.allow = false;
            d.winner = "NONE";
            d.reason = "governor_pause";
            return d;
        }

        if(top < threshold)
        {
            d.allow = false;
            d.winner = "NONE";
            d.reason = "below_threshold";
            return d;
        }

        if(breakout_score >= bracket_score)
        {
            d.allow = true;
            d.winner = "BREAKOUT";
            d.reason = "score_breakout";
            return d;
        }

        d.allow = true;
        d.winner = "BRACKET";
        d.reason = "score_bracket";
        return d;
    }
};
