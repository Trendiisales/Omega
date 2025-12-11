#include "BinanceTickNormalizer.hpp"
#include "../json/Json.hpp"
#include <cstdlib>

namespace Omega {

bool BinanceTickNormalizer::parse(const std::string& s, Tick& t)
{
    std::unordered_map<std::string,std::string> kv;
    JSON::parse(s, kv);

    auto g = [&](const std::string& k){
        auto it = kv.find(k);
        return it==kv.end() ? "" : it->second;
    };

    t.symbol       = g("s");
    t.bid          = std::atof(g("b").c_str());
    t.ask          = std::atof(g("a").c_str());
    t.spread       = t.ask - t.bid;
    t.buyVol       = std::atof(g("B").c_str());
    t.sellVol      = std::atof(g("A").c_str());
    t.delta        = std::atof(g("p").c_str());
    t.liquidityGap = std::atof(g("q").c_str());
    t.ts           = std::atoll(g("E").c_str());

    return true;
}

}
